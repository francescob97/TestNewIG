#include "Roads/GeoStreetLightsSubsystem.h"

#include "GeoCoreModule.h"
#include "Geo/GeoUnits.h"
#include "Georeference/GeoreferenceSubsystem.h"
#include "Mesh/TileMesh.h"
#include "Sky/GeoSkySubsystem.h"
#include "Streaming/GeoTileStreamingSubsystem.h"
#include "Streaming/GeoVectorStreamingSubsystem.h"
#include "Terrain/GeoTerrainSubsystem.h"
#include "Tiles/TilingScheme.h"
#include "Vegetation/GeoTreeModels.h"
#include "Weather/GeoWeatherSubsystem.h"

#include "Camera/PlayerCameraManager.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Components/SpotLightComponent.h"
#include "DrawDebugHelpers.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/PlayerController.h"
#include "HAL/PlatformTime.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialParameterCollection.h"
#include "Materials/MaterialParameterCollectionInstance.h"
#include "MeshDescription.h"
#include "Misc/DateTime.h"
#include "StaticMeshResources.h"
#include "UObject/Package.h"

#include <cmath>

using namespace GeoWorld;
using GeoWorld::Tiles::FTileKey;

namespace
{
	const TCHAR* StreetLightMaterialPath = TEXT("/GeoWorld/Materials/M_GeoStreetLight.M_GeoStreetLight");
	const TCHAR* StreetLampMaterialPath = TEXT("/GeoWorld/Materials/M_GeoStreetLamp.M_GeoStreetLamp");
	const TCHAR* StreetLampFallbackMaterialPath = TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial");
	const TCHAR* StreetLightCollectionPath = TEXT("/GeoWorld/Materials/MPC_GeoWeather.MPC_GeoWeather");

	/** Ogni quanto si rifa' il conto delle celle, e la scelta delle luci vere, s. */
	constexpr double StreetLightSynchronisePeriodS = 0.25;
	constexpr double StreetLightChoicePeriodS = 0.2;
	/** Una cella lontana (solo punti luce) si rifa' per il terreno al piu' ogni tanti secondi. */
	constexpr double StreetLightFarRebuildS = 10.0;
	/** Le luci vere sfumano in tanti secondi. */
	constexpr float StreetLightFadeS = 0.4f;
	/** Il punto luce: pixel minimi, lato da vicino (m), di quanti punti avvicinarlo alla camera. */
	constexpr double StreetLightMinPixels = 2.5;
	constexpr double StreetLightCoreM = 0.8;
	constexpr double StreetLightPull = 2.0;
	/** I bounds del quadratino, allargati di tanto (m): il materiale lo apre fino a qualche decina di metri. */
	constexpr double StreetLightSpriteBoundsM = 60.0;
	/** Il LOD semplice dei pali: sotto questa grandezza sullo schermo (~150 m per un palo di 8 m). */
	constexpr float StreetLampFarLodScreenSize = 0.03f;

	/**
	 * Una mesh fatta da codice (StreetLights.h) come UStaticMesh: uno o due
	 * LOD. Come le forme degli alberi (GeoVegetationSubsystem.cpp): bFastBuild
	 * costruisce i dati di disegno senza la pipeline dell'editor.
	 */
	UStaticMesh* BuildStreetLightMesh(const TArray<const Vegetation::FTreeMesh*>& Lods, UMaterialInterface* Material, const TCHAR* What)
	{
		TArray<FMeshDescription> Descriptions;
		Descriptions.SetNum(Lods.Num());
		TArray<const FMeshDescription*> Pointers;
		for (int32 Lod = 0; Lod < Lods.Num(); ++Lod)
		{
			GeoVegetationModels::FillMeshDescription(*Lods[Lod], false, Descriptions[Lod]);
			Pointers.Add(&Descriptions[Lod]);
		}
		UStaticMesh* Mesh = NewObject<UStaticMesh>(GetTransientPackage(), NAME_None, RF_Transient);
		Mesh->GetStaticMaterials().Add(FStaticMaterial(Material, GeoVegetationModels::SlotName()));
		UStaticMesh::FBuildMeshDescriptionsParams BuildParameters;
		BuildParameters.bMarkPackageDirty = false;
		BuildParameters.bBuildSimpleCollision = false;
		BuildParameters.bCommitMeshDescription = false;
		BuildParameters.bFastBuild = true;
		if (!Mesh->BuildFromMeshDescriptions(Pointers, BuildParameters))
		{
			UE_LOG(LogGeoWorld, Warning, TEXT("[GeoLights] la forma '%s' non si costruisce"), What);
			return nullptr;
		}
		if (Lods.Num() > 1)
		{
			if (FStaticMeshRenderData* RenderData = Mesh->GetRenderData())
			{
				RenderData->ScreenSize[0].Default = 1.0f;
				RenderData->ScreenSize[1].Default = StreetLampFarLodScreenSize;
			}
		}
		return Mesh;
	}
}

// ============================================================================

bool UGeoStreetLightsSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	if (!Super::ShouldCreateSubsystem(Outer)) { return false; }
	const UWorld* World = Cast<UWorld>(Outer);
	return World && (World->WorldType == EWorldType::Game
	              || World->WorldType == EWorldType::PIE
	              || World->WorldType == EWorldType::Editor);
}

void UGeoStreetLightsSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Collection.InitializeDependency<UGeoTerrainSubsystem>();
	Collection.InitializeDependency<UGeoVectorStreamingSubsystem>();
	Collection.InitializeDependency<UGeoSkySubsystem>();
	Collection.InitializeDependency<UGeoWeatherSubsystem>();

	UWorld* World = GetWorld();
	Terrain = World->GetSubsystem<UGeoTerrainSubsystem>();
	Streaming = World->GetSubsystem<UGeoVectorStreamingSubsystem>();
	Queue = MakeShared<FCellQueue, ESPMode::ThreadSafe>();

	// Le bande: pali fino a 700 m, punti luce fino a 8 km (la terza banda non c'e').
	Bands.NearM = 700.0;
	Bands.MidM = 8000.0;
	Bands.FarM = 8000.0;
	Bands.HysteresisM = 75.0;

	if (UGeoreferenceSubsystem* Georeference = World->GetSubsystem<UGeoreferenceSubsystem>())
	{
		RebaseHandle = Georeference->OnGeoreferenceRebased.AddUObject(this, &UGeoStreetLightsSubsystem::OnGeoreferenceRebased);
	}
}

void UGeoStreetLightsSubsystem::Deinitialize()
{
	if (UWorld* World = GetWorld())
	{
		if (UGeoreferenceSubsystem* Georeference = World->GetSubsystem<UGeoreferenceSubsystem>())
		{
			Georeference->OnGeoreferenceRebased.Remove(RebaseHandle);
		}
	}
	// I lavori in volo eseguono codice di questo modulo: si aspettano.
	for (TPair<uint64, UE::Tasks::FTask>& Pair : InFlight) { Pair.Value.Wait(); }
	InFlight.Empty();
	PendingResults.Empty();
	RemoveAllCells();
	RemoveRealLights();
	if (AActor* Actor = Container.Get()) { Actor->Destroy(); }
	Container.Reset();
	SpriteMesh.Reset();
	for (TStrongObjectPtr<UStaticMesh>& Slot : PoleMeshes) { Slot.Reset(); }
	SpriteMaterial.Reset();
	PoleMaterial.Reset();
	WeatherCollection.Reset();
	Super::Deinitialize();
}

TStatId UGeoStreetLightsSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UGeoStreetLightsSubsystem, STATGROUP_Tickables);
}

// --- Impostazioni -----------------------------------------------------------------

void UGeoStreetLightsSubsystem::SetEnabled(bool bInEnabled)
{
	if (bEnabled == bInEnabled) { return; }
	bEnabled = bInEnabled;
	if (!bEnabled)
	{
		++Generation;
		RemoveAllCells();
		RemoveRealLights();
	}
	LastSynchronise = -100.0;
}

void UGeoStreetLightsSubsystem::OnDatasetChanged()
{
	++Generation;
	RemoveAllCells();
	RemoveRealLights();
	LastSynchronise = -100.0;
}

void UGeoStreetLightsSubsystem::SetRadiusM(double InRadiusM)
{
	Bands.MidM = FMath::Clamp(InRadiusM, 500.0, 40000.0);
	Bands.FarM = Bands.MidM;
	Bands.NearM = FMath::Min(Bands.NearM, Bands.MidM);
	LastSynchronise = -100.0;
}

void UGeoStreetLightsSubsystem::SetPoleRadiusM(double InRadiusM)
{
	Bands.NearM = FMath::Clamp(InRadiusM, 0.0, FMath::Min(3000.0, Bands.MidM));
	LastSynchronise = -100.0;
}

void UGeoStreetLightsSubsystem::SetLightCount(int32 InCount)
{
	LightCount = FMath::Clamp(InCount, 0, 1024);
	LastLightChoice = -100.0;
}

void UGeoStreetLightsSubsystem::SetShadows(bool bInShadows)
{
	bShadows = bInShadows;
	for (FRealLight& Slot : RealLights)
	{
		if (USpotLightComponent* Light = Slot.Light.Get()) { Light->SetCastShadows(bShadows); }
	}
}

void UGeoStreetLightsSubsystem::SetPlacement(const StreetLights::FStreetLightSettings& InSettings)
{
	Placement = InSettings;
	// Le celle si rifanno, ma le vecchie restano a schermo finche' le nuove non sono pronte.
	++Generation;
	for (TPair<uint64, FCell>& Pair : LampCells) { Pair.Value.Signature = 0; Pair.Value.BuiltAt = -1000.0; }
	LastSynchronise = -100.0;
}

double UGeoStreetLightsSubsystem::GetOnLevel() const
{
	if (!bEnabled) { return 0.0; }
	return StreetLights::LampOnLevel(AmbientLux, 30.0, bMorning);
}

FString UGeoStreetLightsSubsystem::Describe() const
{
	if (!Streaming || !Streaming->IsDatasetOpen()) { return FString(); }
	if (!bEnabled) { return TEXT("spenti (geo.Lights.Enable 1)"); }
	const double On = GetOnLevel();
	if (On <= 0.0)
	{
		return FString::Printf(TEXT("spenti, c'e' luce (%.0f lux); %d lampioni"), AmbientLux, Stats.Lampioni);
	}
	return FString::Printf(TEXT("%s, %d lampioni, %d luci vere%s"), On >= 1.0 ? TEXT("accesi") : TEXT("si accendono"),
		Stats.Lampioni, Stats.LuciVere, Dimming < 0.999 ? TEXT(", flusso ridotto") : TEXT(""));
}

FString UGeoStreetLightsSubsystem::GetMaterialProblem() const
{
	if (!SpriteMaterial.IsValid())
	{
		return TEXT("manca M_GeoStreetLight: niente punti luce da lontano. Crealo con geo.Imagery.CreateMaterial");
	}
	if (bPoleMaterialIsFallback)
	{
		return TEXT("manca M_GeoStreetLamp: i pali sono grigi e il vetro spento. Crealo con geo.Imagery.CreateMaterial");
	}
	if (bParametersMissing)
	{
		return TEXT("MPC_GeoWeather e' di prima dei lampioni (manca StreetLights): rilancia geo.Imagery.CreateMaterial");
	}
	return FString();
}

// --- Il giro ----------------------------------------------------------------------

void UGeoStreetLightsSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	if (!Streaming || !Streaming->IsDatasetOpen() || !bEnabled)
	{
		if (LampCells.Num() > 0) { RemoveAllCells(); }
		if (RealLights.Num() > 0) { RemoveRealLights(); }
		Stats = FGeoStreetLightsStats();
		// Spenti anche nei materiali (un componente rimasto non deve brillare).
		UpdateLightLevel();
		if (bShowDebugOverlay) { DrawDebugOverlay(); }
		return;
	}

	EnsureContainer();
	CheckAssets();
	UpdateLightLevel();
	const double Now = FPlatformTime::Seconds();
	if (Now - LastSynchronise >= StreetLightSynchronisePeriodS)
	{
		LastSynchronise = Now;
		Synchronise();
	}
	DrainFinished(MaxAppliesPerFrame);
	UpdateSpriteVisibility();
	if (Now - LastLightChoice >= StreetLightChoicePeriodS)
	{
		LastLightChoice = Now;
		ChooseRealLights();
	}
	UpdateRealLights(DeltaTime);
	if (bShowDebugOverlay) { DrawDebugOverlay(); }
}

double UGeoStreetLightsSubsystem::CameraAboveGround(double LatDeg, double LonDeg, double HeightM) const
{
	const UGeoTileStreamingSubsystem* Heights = GetWorld()->GetSubsystem<UGeoTileStreamingSubsystem>();
	if (!Heights) { return 0.0; }
	const FVisibleTile* Best = nullptr;
	for (const FVisibleTile& Visible : VisibleTiles)
	{
		if (LonDeg < Visible.Bounds.West || LonDeg > Visible.Bounds.East || LatDeg < Visible.Bounds.South || LatDeg > Visible.Bounds.North) { continue; }
		if (!Best || Visible.Key.Level > Best->Key.Level) { Best = &Visible; }
	}
	const UGeoTileStreamingSubsystem::FTilePtr HeightTile = Best ? Heights->PeekLoadedTile(Best->Key) : nullptr;
	if (!HeightTile || !HeightTile->IsValid()) { return 0.0; }
	return HeightM - Mesh::FSurfaceSampler(*HeightTile, MeshStep).HeightAt(LonDeg, LatDeg);
}

uint64 UGeoStreetLightsSubsystem::GatherGround(const Tiles::FTileBounds& Bounds, Cells::FGroundSurfaces* OutGround) const
{
	std::vector<FTileKey> Keys;
	for (const FVisibleTile& Visible : VisibleTiles)
	{
		if (Cells::BoundsOverlap(Visible.Bounds, Bounds)) { Keys.push_back(Visible.Key); }
	}
	if (OutGround)
	{
		const UGeoTileStreamingSubsystem* Heights = GetWorld()->GetSubsystem<UGeoTileStreamingSubsystem>();
		if (!Heights) { return 0; }
		for (const FTileKey& Key : Keys)
		{
			UGeoTileStreamingSubsystem::FTilePtr HeightTile = Heights->PeekLoadedTile(Key);
			if (!HeightTile || !HeightTile->IsValid()) { return 0; }
			OutGround->Add(MoveTemp(HeightTile), MeshStep);
		}
	}
	return Cells::SurfaceSignature(MoveTemp(Keys), MeshStep);
}

FTileKey UGeoStreetLightsSubsystem::SourceKeyFor(const FTileKey& Cell) const
{
	const uint32 SourceLevel = static_cast<uint32>(FMath::Max(0, Streaming->GetDataset().GetMaxLevel()));
	const uint32 Shift = Cell.Level > SourceLevel ? Cell.Level - SourceLevel : 0u;
	return FTileKey{ FMath::Min(Cell.Level, SourceLevel), Cell.X >> Shift, Cell.Y >> Shift };
}

void UGeoStreetLightsSubsystem::Synchronise()
{
	UWorld* World = GetWorld();
	const UGeoreferenceSubsystem* Georeference = World ? World->GetSubsystem<UGeoreferenceSubsystem>() : nullptr;
	FVector Location = FVector::ZeroVector;
	if (!Georeference || !Georeference->GetActiveViewLocation(Location) || !Terrain)
	{
		bHaveCamera = false;
		return;
	}

	TArray<FTileKey> Built;
	const int32 VisibleCount = Terrain->GetBuiltTileKeys(Built);
	VisibleTiles.Reset(VisibleCount);
	for (int32 Index = 0; Index < VisibleCount; ++Index)
	{
		const FTileKey& Key = Built[Index];
		VisibleTiles.Add(FVisibleTile{ Key, Tiles::GetTileBounds(Key.Level, Key.X, Key.Y) });
	}
	MeshStep = Terrain->GetMeshStep();

	const Core::FGeodetic Here = Georeference->GetSnapshot().UnrealToGeodetic(Location);
	CameraLatDeg = Here.LatRad / Core::DegToRad;
	CameraLonDeg = Here.LonRad / Core::DegToRad;
	CameraAboveGroundM = FMath::Max(0.0, CameraAboveGround(CameraLatDeg, CameraLonDeg, Here.HeightM));
	// La prima volta che si sa dove si e': la mattina o la sera si ricalcola subito.
	if (!bHaveCamera) { LastMorningCheck = -100.0; }
	bHaveCamera = true;

	const uint32 Level = StreetLights::CellLevelFor(Streaming->GetDataset().GetMaxLevel());

	// 1. Le celle che servono, dalla piu' vicina, con la loro banda (0 pali, 1 solo punti).
	std::vector<Cells::FCellRef> Around;
	Cells::CellsAround(CameraLatDeg, CameraLonDeg, CameraAboveGroundM, Bands.MidM + Bands.HysteresisM, Level, Around);
	TSet<uint64> Seen;
	Seen.Reserve(static_cast<int32>(Around.size()));
	for (const Cells::FCellRef& Ref : Around)
	{
		const uint64 Packed = Ref.Key.Pack();
		FCell* Existing = LampCells.Find(Packed);
		int32 Band = Cells::BandFor(Ref.DistanceM, Bands, Existing ? Existing->WantedBand : -1);
		if (Band == 2) { Band = 1; }
		if (Band == Cells::BandOutside && !Existing) { continue; }
		Seen.Add(Packed);
		if (!Existing)
		{
			const Tiles::FTileBounds Bounds = Tiles::GetTileBounds(Ref.Key.Level, Ref.Key.X, Ref.Key.Y);
			FCell New;
			New.Key = Ref.Key;
			New.Origin = Core::FGeodetic::FromDegrees(Bounds.CentreLat(), Bounds.CentreLon(), 0.0);
			Existing = &LampCells.Add(Packed, MoveTemp(New));
		}
		Existing->WantedBand = Band;
		Existing->DistanceM = Ref.DistanceM;
	}

	// 2. Quelle uscite dal raggio: via.
	TArray<uint64> Gone;
	for (TPair<uint64, FCell>& Pair : LampCells)
	{
		if (!Seen.Contains(Pair.Key)) { Pair.Value.WantedBand = Cells::BandOutside; }
		if (Pair.Value.WantedBand == Cells::BandOutside) { Gone.Add(Pair.Key); }
	}
	for (const uint64 Packed : Gone)
	{
		if (FCell* Cell = LampCells.Find(Packed)) { DestroyComponents(*Cell); }
		LampCells.Remove(Packed);
	}

	// 3. Per ogni cella, dalla piu' vicina: la tile delle strade, il terreno sotto, se e' da rifare.
	const double Now = FPlatformTime::Seconds();
	int32 BandApplies = 0;
	for (const Cells::FCellRef& Ref : Around)
	{
		FCell* Cell = LampCells.Find(Ref.Key.Pack());
		if (!Cell) { continue; }
		const EGeoTileState SourceState = Streaming->RequestTile(SourceKeyFor(Ref.Key), static_cast<int32>(Ref.DistanceM));
		if (SourceState == EGeoTileState::Assente || SourceState == EGeoTileState::Errore)
		{
			if (!Cell->bEmpty) { DestroyComponents(*Cell); }
			Cell->bEmpty = true;
			Cell->Instances.Reset();
			Cell->Signature = 0;
			continue;
		}
		Cell->bEmpty = false;
		if (SourceState != EGeoTileState::Pronta) { continue; }

		const Tiles::FTileBounds Bounds = Tiles::GetTileBounds(Ref.Key.Level, Ref.Key.X, Ref.Key.Y);
		const uint64 Signature = GatherGround(Bounds, nullptr);
		// Una cella lontana non si rifa' a ogni affinamento del terreno: i suoi
		// punti luce sono avvicinati alla camera, l'errore non si vede.
		const bool bFarAndFresh = Cell->WantedBand > 0 && Cell->Instances.IsValid() && Now - Cell->BuiltAt < StreetLightFarRebuildS;
		if (Signature != 0 && Signature != Cell->Signature && !bFarAndFresh && !InFlight.Contains(Ref.Key.Pack())
			&& InFlight.Num() < MaxJobsInFlight)
		{
			LaunchCell(*Cell, Signature);
		}
		if (Cell->Instances.IsValid() && Cell->Band != Cell->WantedBand && BandApplies < 2 * MaxAppliesPerFrame)
		{
			ApplyCell(*Cell);
			++BandApplies;
		}
	}

	// 4. Le statistiche.
	FGeoStreetLightsStats Next;
	for (const TPair<uint64, FCell>& Pair : LampCells)
	{
		const FCell& Cell = Pair.Value;
		if (Cell.bEmpty) { ++Next.CelleVuote; continue; }
		if (!Cell.Sprites.IsValid() && Cell.Poles.Num() == 0)
		{
			if (!Cell.Instances.IsValid()) { ++Next.CelleInAttesa; }
			else if (Cell.Instances->empty()) { ++Next.CelleVuote; }
			continue;
		}
		const int32 Count = Cell.Instances.IsValid() ? static_cast<int32>(Cell.Instances->size()) : 0;
		Next.CelleVicine += Cell.Band == 0 ? 1 : 0;
		Next.CelleLontane += Cell.Band == 1 ? 1 : 0;
		Next.Lampioni += Cell.Sprites.IsValid() ? Count : 0;
		Next.Pali += Cell.Poles.Num() > 0 ? Count : 0;
		Next.Componenti += (Cell.Sprites.IsValid() ? 1 : 0) + Cell.Poles.Num();
	}
	for (const FRealLight& Slot : RealLights) { Next.LuciVere += Slot.Weight > 0.0f ? 1 : 0; }
	Next.CostruzioniInCorso = InFlight.Num();
	Next.MillisecondiPerCella = CompletedJobs > 0 ? static_cast<float>(TotalJobSeconds / CompletedJobs * 1000.0) : 0.0f;
	Stats = Next;
}

bool UGeoStreetLightsSubsystem::LaunchCell(FCell& Cell, uint64 Signature)
{
	const UGeoVectorStreamingSubsystem::FVectorTilePtr Source = Streaming->FindLoadedTile(SourceKeyFor(Cell.Key));
	if (!Source || !Queue.IsValid()) { return false; }

	const Tiles::FTileBounds Bounds = Tiles::GetTileBounds(Cell.Key.Level, Cell.Key.X, Cell.Key.Y);
	TSharedPtr<Cells::FGroundSurfaces, ESPMode::ThreadSafe> Ground = MakeShared<Cells::FGroundSurfaces, ESPMode::ThreadSafe>();
	if (GatherGround(Bounds, Ground.Get()) != Signature) { return false; }

	const StreetLights::FStreetLightSettings Settings = Placement;
	const int32 JobGeneration = Generation;
	const FTileKey Key = Cell.Key;
	TSharedPtr<FCellQueue, ESPMode::ThreadSafe> JobQueue = Queue;

	// Tutto per copia, niente "this": come le celle degli alberi.
	UE::Tasks::FTask Task = UE::Tasks::Launch(TEXT("GeoStreetLightsCell"),
		[Source, Ground, Bounds, Settings, JobGeneration, Key, Signature, JobQueue]()
		{
			const double Started = FPlatformTime::Seconds();
			std::vector<StreetLights::FStreetLamp> Lamps;
			StreetLights::PlaceTileLamps(*Source, Settings, Lamps);
			TSharedPtr<FInstances, ESPMode::ThreadSafe> Instances = MakeShared<FInstances, ESPMode::ThreadSafe>();
			StreetLights::BuildCellLamps(Bounds, Lamps, *Ground, *Instances);
			FCellResult Result;
			Result.Key = Key;
			Result.Generation = JobGeneration;
			Result.Signature = Signature;
			Result.Instances = Instances;
			Result.Seconds = FPlatformTime::Seconds() - Started;
			JobQueue->Completed.Enqueue(MoveTemp(Result));
		},
		UE::Tasks::ETaskPriority::BackgroundNormal);

	InFlight.Add(Key.Pack(), Task);
	return true;
}

void UGeoStreetLightsSubsystem::DrainFinished(int32 Budget)
{
	FCellResult Result;
	while (Queue.IsValid() && Queue->Completed.Dequeue(Result)) { PendingResults.Add(MoveTemp(Result)); }

	int32 Applied = 0;
	int32 Index = 0;
	while (Index < PendingResults.Num())
	{
		FCellResult& Pending = PendingResults[Index];
		const uint64 Packed = Pending.Key.Pack();
		FCell* Cell = LampCells.Find(Packed);
		const bool bUseful = Pending.Generation == Generation && Cell && Cell->WantedBand != Cells::BandOutside;
		if (bUseful && Applied >= Budget) { ++Index; continue; }
		if (bUseful)
		{
			Cell->Instances = Pending.Instances;
			Cell->Signature = Pending.Signature;
			Cell->BuiltAt = FPlatformTime::Seconds();
			Cell->bEmpty = false;
			ApplyCell(*Cell);
			++Applied;
			++CompletedJobs;
			TotalJobSeconds += Pending.Seconds;
		}
		InFlight.Remove(Packed);
		PendingResults.RemoveAtSwap(Index);
	}
}

// --- I componenti ---------------------------------------------------------------------

void UGeoStreetLightsSubsystem::EnsureContainer()
{
	if (Container.IsValid()) { return; }
	UWorld* World = GetWorld();
	if (!World) { return; }
	FActorSpawnParameters SpawnParameters;
	SpawnParameters.ObjectFlags |= RF_Transient;
	AActor* Actor = World->SpawnActor<AActor>(AActor::StaticClass(), SpawnParameters);
	if (!Actor) { return; }
	Actor->SetRootComponent(NewObject<USceneComponent>(Actor, TEXT("Root")));
	Actor->GetRootComponent()->SetMobility(EComponentMobility::Movable);
	Actor->GetRootComponent()->RegisterComponent();
#if WITH_EDITOR
	Actor->SetActorLabel(TEXT("GeoWorld Lampioni"));
#endif
	Container = Actor;
}

void UGeoStreetLightsSubsystem::CheckAssets()
{
	const double Now = FPlatformTime::Seconds();
	const bool bComplete = SpriteMaterial.IsValid() && PoleMaterial.IsValid() && !bPoleMaterialIsFallback && WeatherCollection.IsValid();
	if (bComplete || Now - LastAssetLookup < 2.0) { return; }
	LastAssetLookup = Now;

	if (!WeatherCollection.IsValid())
	{
		WeatherCollection.Reset(LoadObject<UMaterialParameterCollection>(nullptr, StreetLightCollectionPath, nullptr, LOAD_NoWarn | LOAD_Quiet));
	}
	bool bChanged = false;
	if (!SpriteMaterial.IsValid())
	{
		if (UMaterialInterface* Found = LoadObject<UMaterialInterface>(nullptr, StreetLightMaterialPath, nullptr, LOAD_NoWarn | LOAD_Quiet))
		{
			SpriteMaterial.Reset(Found);
			bChanged = true;
		}
	}
	if (!PoleMaterial.IsValid() || bPoleMaterialIsFallback)
	{
		if (UMaterialInterface* Found = LoadObject<UMaterialInterface>(nullptr, StreetLampMaterialPath, nullptr, LOAD_NoWarn | LOAD_Quiet))
		{
			PoleMaterial.Reset(Found);
			bPoleMaterialIsFallback = false;
			bChanged = true;
		}
		else if (!PoleMaterial.IsValid())
		{
			PoleMaterial.Reset(LoadObject<UMaterialInterface>(nullptr, StreetLampFallbackMaterialPath));
			bPoleMaterialIsFallback = true;
			UE_LOG(LogGeoWorld, Warning, TEXT("[GeoLights] %s"), *GetMaterialProblem());
		}
	}
	// Un materiale arrivato adesso (geo.Imagery.CreateMaterial con il Play
	// aperto): le celle si rifanno con lui.
	if (bChanged)
	{
		for (TPair<uint64, FCell>& Pair : LampCells)
		{
			if (Pair.Value.Instances.IsValid()) { ApplyCell(Pair.Value); }
		}
	}
}

bool UGeoStreetLightsSubsystem::EnsureMeshes()
{
	if (!PoleMaterial.IsValid()) { return false; }
	if (!SpriteMesh.IsValid())
	{
		Vegetation::FTreeMesh Quad;
		StreetLights::BuildSpriteQuad(Quad);
		TArray<const Vegetation::FTreeMesh*> Lods;
		Lods.Add(&Quad);
		if (UStaticMesh* Mesh = BuildStreetLightMesh(Lods, SpriteMaterial.IsValid() ? SpriteMaterial.Get() : PoleMaterial.Get(), TEXT("punto luce")))
		{
			// Il quadratino e' di 5 cm, ma il materiale lo apre fino a qualche
			// decina di metri e lo avvicina alla camera: i bounds devono
			// saperlo, o il motore lo scarta quando il centro esce dallo schermo.
			// NOTA UE: in metri della mesh (il componente scala per 100).
			Mesh->SetPositiveBoundsExtension(FVector(StreetLightSpriteBoundsM));
			Mesh->SetNegativeBoundsExtension(FVector(StreetLightSpriteBoundsM));
			Mesh->CalculateExtendedBounds();
			SpriteMesh.Reset(Mesh);
		}
	}
	for (int32 Kind = 0; Kind < StreetLights::LampKindCount; ++Kind)
	{
		if (PoleMeshes[Kind].IsValid()) { continue; }
		Vegetation::FTreeMesh Near, Far;
		StreetLights::BuildLampMesh(static_cast<StreetLights::ELampKind>(Kind), false, Near);
		StreetLights::BuildLampMesh(static_cast<StreetLights::ELampKind>(Kind), true, Far);
		TArray<const Vegetation::FTreeMesh*> Lods;
		Lods.Add(&Near);
		Lods.Add(&Far);
		PoleMeshes[Kind].Reset(BuildStreetLightMesh(Lods, PoleMaterial.Get(),
			UTF8_TO_TCHAR(StreetLights::LampKindName(static_cast<StreetLights::ELampKind>(Kind)))));
	}
	return SpriteMesh.IsValid();
}

void UGeoStreetLightsSubsystem::DestroyComponents(FCell& Cell)
{
	if (UHierarchicalInstancedStaticMeshComponent* Component = Cell.Sprites.Get()) { Component->DestroyComponent(); }
	Cell.Sprites.Reset();
	for (const TWeakObjectPtr<UHierarchicalInstancedStaticMeshComponent>& Weak : Cell.Poles)
	{
		if (UHierarchicalInstancedStaticMeshComponent* Component = Weak.Get()) { Component->DestroyComponent(); }
	}
	Cell.Poles.Reset();
	Cell.Band = -1;
}

void UGeoStreetLightsSubsystem::RemoveAllCells()
{
	for (TPair<uint64, FCell>& Pair : LampCells) { DestroyComponents(Pair.Value); }
	LampCells.Empty();
}

void UGeoStreetLightsSubsystem::ApplyCell(FCell& Cell)
{
	DestroyComponents(Cell);
	Cell.Band = Cell.WantedBand;
	AActor* Actor = Container.Get();
	const UGeoreferenceSubsystem* Georeference = GetWorld() ? GetWorld()->GetSubsystem<UGeoreferenceSubsystem>() : nullptr;
	if (Cell.bEmpty || !Cell.Instances.IsValid() || Cell.Instances->empty() || Cell.Band < 0 || Cell.Band >= Cells::BandOutside
		|| !Actor || !Georeference || !EnsureMeshes())
	{
		return;
	}

	FTransform Transform = Georeference->GetSnapshot().GetLocalNeuTransform(Cell.Origin);
	Transform.SetScale3D(FVector(Units::MetersToUu));
	Cell.WorldTransform = Transform;

	// I punti luce (uno per lampione) e, nelle celle vicine, i pali per tipo.
	TArray<FTransform> SpriteTransforms;
	TArray<float> SpriteData;
	TArray<FTransform> PoleTransforms[StreetLights::LampKindCount];
	TArray<float> PoleData[StreetLights::LampKindCount];
	double GroundSum = 0.0;
	for (const StreetLights::FLampInstance& Lamp : *Cell.Instances)
	{
		float Values[StreetLights::InstanceDataFloats];
		StreetLights::InstanceData(Lamp, Values);
		SpriteTransforms.Emplace(FQuat::Identity, FVector(Lamp.Light[0], Lamp.Light[1], Lamp.Light[2]), FVector::OneVector);
		SpriteData.Append(Values, StreetLights::InstanceDataFloats);
		GroundSum += Lamp.Base[2];
		if (Cell.Band == 0)
		{
			const int32 Kind = FMath::Clamp<int32>(Lamp.Kind, 0, StreetLights::LampKindCount - 1);
			PoleTransforms[Kind].Emplace(FQuat(FVector::UpVector, Lamp.YawRad), FVector(Lamp.Base[0], Lamp.Base[1], Lamp.Base[2]),
				FVector(Lamp.Scale));
			PoleData[Kind].Append(Values, StreetLights::InstanceDataFloats);
		}
	}
	Cell.GroundM = GroundSum / static_cast<double>(Cell.Instances->size());

	auto MakeComponent = [Actor, &Transform](UStaticMesh* Mesh, UMaterialInterface* Material, const TArray<FTransform>& Transforms,
	                                               const TArray<float>& Data, bool bShadow)
	{
		UHierarchicalInstancedStaticMeshComponent* Component = NewObject<UHierarchicalInstancedStaticMeshComponent>(Actor);
		Component->SetupAttachment(Actor->GetRootComponent());
		Component->SetMobility(EComponentMobility::Movable);
		Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Component->SetCanEverAffectNavigation(false);
		Component->SetStaticMesh(Mesh);
		if (Material) { Component->SetMaterial(0, Material); }
		Component->SetCastShadow(bShadow);
		Component->SetNumCustomDataFloats(StreetLights::InstanceDataFloats);
		Component->AddInstances(Transforms, /*bShouldReturnIndices=*/false, /*bWorldSpace=*/false);
		for (int32 InstanceIndex = 0; InstanceIndex < Transforms.Num(); ++InstanceIndex)
		{
			Component->SetCustomData(InstanceIndex,
				TArrayView<const float>(Data.GetData() + InstanceIndex * StreetLights::InstanceDataFloats, StreetLights::InstanceDataFloats),
				/*bMarkRenderStateDirty=*/false);
		}
		Component->RegisterComponent();
		Component->SetWorldTransform(Transform);
		return Component;
	};

	if (SpriteMaterial.IsValid() && SpriteMesh.IsValid())
	{
		UHierarchicalInstancedStaticMeshComponent* Sprites = MakeComponent(SpriteMesh.Get(), SpriteMaterial.Get(), SpriteTransforms, SpriteData, false);
		Sprites->SetVisibility(bSpritesVisible);
		Cell.Sprites = Sprites;
	}
	for (int32 Kind = 0; Kind < StreetLights::LampKindCount; ++Kind)
	{
		if (PoleTransforms[Kind].Num() == 0 || !PoleMeshes[Kind].IsValid()) { continue; }
		UHierarchicalInstancedStaticMeshComponent* Poles = MakeComponent(PoleMeshes[Kind].Get(), PoleMaterial.Get(),
			PoleTransforms[Kind], PoleData[Kind], true);
		// Oltre il raggio dei pali non si disegnano (sono sottili: a quella distanza meno di un pixel).
		const int32 EndCm = static_cast<int32>((Bands.NearM + Bands.HysteresisM) * Units::MetersToUu);
		Poles->SetCullDistances(EndCm * 4 / 5, EndCm);
		Cell.Poles.Add(Poles);
	}
}

void UGeoStreetLightsSubsystem::UpdateSpriteVisibility()
{
	// Di giorno i punti luce sono neri (additivi: non si vedono), ma si
	// disegnano lo stesso: si nascondono. Tornano visibili un po' prima che
	// il circuito piu' sensibile cominci ad accendersi.
	const bool bVisible = Force == 1 || (Force == -1 && AmbientLux < 80.0 * (bMorning ? StreetLights::MorningSwitchFactor : 1.0));
	if (bVisible == bSpritesVisible) { return; }
	bSpritesVisible = bVisible;
	for (TPair<uint64, FCell>& Pair : LampCells)
	{
		if (UHierarchicalInstancedStaticMeshComponent* Component = Pair.Value.Sprites.Get()) { Component->SetVisibility(bSpritesVisible); }
	}
}

// --- La luce di adesso -----------------------------------------------------------------

void UGeoStreetLightsSubsystem::UpdateLightLevel()
{
	UWorld* World = GetWorld();
	if (!World) { return; }
	const UGeoSkySubsystem* Sky = World->GetSubsystem<UGeoSkySubsystem>();
	const UGeoWeatherSubsystem* Weather = World->GetSubsystem<UGeoWeatherSubsystem>();
	const bool bSky = Sky && Sky->IsEnabled() && Sky->GetState().bValid;

	// L'ora e il sole: quelli del cielo; senza, l'orologio vero sul punto della camera.
	const double JdUt = Sky ? Sky->GetTimeUt() : FDateTime::UtcNow().GetJulianDay();
	const double SunAltitude = bSky ? Sky->GetState().Bodies.Sun.AltitudeDeg : StreetLights::SunAltitudeDeg(JdUt, CameraLatDeg, CameraLonDeg);
	const double LocalHour = bSky ? Sky->GetState().LocalHour : FMath::Fmod(FMath::Fmod(JdUt + 0.5, 1.0) * 24.0 + CameraLonDeg / 15.0 + 48.0, 24.0);
	const double Cover = (Weather && Weather->AreCloudsEnabled()) ? Weather->GetCloudSettings().Cover : 0.0;
	const double DayLux = (bSky && Sky->GetSunLux() > 0.0) ? Sky->GetSunLux() : 10.0;

	const double Now = FPlatformTime::Seconds();
	if (Now - LastMorningCheck > 20.0)
	{
		LastMorningCheck = Now;
		bMorning = StreetLights::IsMorning(JdUt, CameraLatDeg, CameraLonDeg);
	}
	AmbientLux = StreetLights::AmbientLux(SunAltitude, Cover);
	if (Force == 0) { AmbientLux = 1.0e7; }
	else if (Force == 1) { AmbientLux = 0.0; }
	Dimming = StreetLights::NightDimming(LocalHour, DimmingDepth);
	FootLux = StreetLights::FootLux(DayLux, Brightness);

	UMaterialParameterCollectionInstance* Parameters = WeatherCollection.IsValid() ? World->GetParameterCollectionInstance(WeatherCollection.Get()) : nullptr;
	if (!Parameters) { return; }
	const bool bLit = bEnabled && Streaming && Streaming->IsDatasetOpen();
	bool bSet = Parameters->SetVectorParameterValue(TEXT("StreetLights"), FLinearColor(static_cast<float>(AmbientLux),
		static_cast<float>(bMorning ? StreetLights::MorningSwitchFactor : 1.0), static_cast<float>(FootLux),
		static_cast<float>(bLit ? Dimming : 0.0)));
	bSet = Parameters->SetVectorParameterValue(TEXT("StreetLightView"), FLinearColor(static_cast<float>(PixelAngle()),
		static_cast<float>(StreetLightMinPixels), static_cast<float>(StreetLightCoreM), static_cast<float>(StreetLightPull))) && bSet;
	bParametersMissing = !bSet;
}

double UGeoStreetLightsSubsystem::PixelAngle() const
{
	// Il campo visivo orizzontale della camera del giocatore, e la larghezza
	// della finestra; nell'editor (niente giocatore) 90 gradi e 1920 pixel.
	double FovDeg = 90.0;
	double WidthPixels = 1920.0;
	if (const UWorld* World = GetWorld())
	{
		if (const APlayerController* Player = World->GetFirstPlayerController())
		{
			if (Player->PlayerCameraManager) { FovDeg = Player->PlayerCameraManager->GetFOVAngle(); }
		}
	}
	if (GEngine && GEngine->GameViewport)
	{
		FVector2D Size = FVector2D::ZeroVector;
		GEngine->GameViewport->GetViewportSize(Size);
		if (Size.X > 1.0) { WidthPixels = Size.X; }
	}
	return 2.0 * FMath::Tan(FMath::DegreesToRadians(FMath::Clamp(FovDeg, 5.0, 170.0)) * 0.5) / WidthPixels;
}

// --- Le luci vere ------------------------------------------------------------------------

void UGeoStreetLightsSubsystem::ChooseRealLights()
{
	UWorld* World = GetWorld();
	const UGeoreferenceSubsystem* Georeference = World ? World->GetSubsystem<UGeoreferenceSubsystem>() : nullptr;
	FVector Camera = FVector::ZeroVector;
	if (!Georeference || !Georeference->GetActiveViewLocation(Camera)) { return; }

	// I candidati: i lampioni accesi delle celle vicine (con i pali).
	struct FSource
	{
		const FCell* Cell = nullptr;
		const StreetLights::FLampInstance* Lamp = nullptr;
	};
	std::vector<StreetLights::FLightCandidate> Candidates;
	std::vector<FSource> Sources;
	if (LightCount > 0)
	{
		for (const TPair<uint64, FCell>& Pair : LampCells)
		{
			const FCell& Cell = Pair.Value;
			if (Cell.Band != 0 || !Cell.Instances.IsValid()) { continue; }
			for (const StreetLights::FLampInstance& Lamp : *Cell.Instances)
			{
				if (StreetLights::LampOnLevel(AmbientLux, Lamp.SwitchLux, bMorning) <= 0.0) { continue; }
				const FVector World3 = Cell.WorldTransform.TransformPosition(FVector(Lamp.Light[0], Lamp.Light[1], Lamp.Light[2]));
				Candidates.push_back(StreetLights::FLightCandidate{ Lamp.Id, FVector::Dist(World3, Camera) * Units::UuToMeters });
				Sources.push_back(FSource{ &Cell, &Lamp });
			}
		}
	}
	std::vector<StreetLights::FLightChoice> Choice;
	StreetLights::SelectLights(Candidates, LightCount, Choice);

	// Chi non c'e' piu' sfuma via; chi c'e' gia' tiene la sua luce; i nuovi
	// prendono una luce libera (o una nuova).
	for (FRealLight& Slot : RealLights) { Slot.Target = 0.0f; }
	const FGeoreferenceSnapshot Snapshot = Georeference->GetSnapshot();
	for (const StreetLights::FLightChoice& Item : Choice)
	{
		FRealLight* Slot = RealLights.FindByPredicate([&Item](const FRealLight& Existing) { return Existing.Id == Item.Id; });
		if (!Slot)
		{
			Slot = RealLights.FindByPredicate([](const FRealLight& Existing) { return Existing.Id == 0 || (Existing.Weight <= 0.0f && Existing.Target <= 0.0f); });
		}
		if (!Slot)
		{
			if (RealLights.Num() >= LightCount + LightCount / 2 + 4) { continue; }
			Slot = &RealLights.AddDefaulted_GetRef();
		}
		if (Slot->Id != Item.Id)
		{
			const FSource& Source = Sources[Item.Candidate];
			Slot->Id = Item.Id;
			Slot->Origin = Source.Cell->Origin;
			Slot->Local = FVector(Source.Lamp->Light[0], Source.Lamp->Light[1], Source.Lamp->Light[2]);
			Slot->HeightM = Source.Lamp->HeightM;
			Slot->Output = Source.Lamp->Output;
			Slot->SwitchLux = Source.Lamp->SwitchLux;
			// Il colore della luce con il massimo a 1 (Unreal lo vuole cosi'), e
			// il resto nell'intensita': la luminanza resta quella del lampione.
			const float Peak = FMath::Max3(Source.Lamp->Colour[0], Source.Lamp->Colour[1], FMath::Max(Source.Lamp->Colour[2], 1.0e-3f));
			Slot->Colour = FLinearColor(Source.Lamp->Colour[0] / Peak, Source.Lamp->Colour[1] / Peak, Source.Lamp->Colour[2] / Peak, Peak);
			Slot->Weight = 0.0f;
			PlaceRealLight(*Slot, Snapshot);
		}
		Slot->Target = static_cast<float>(Item.Weight);
	}
}

void UGeoStreetLightsSubsystem::PlaceRealLight(FRealLight& Slot, const FGeoreferenceSnapshot& Snapshot) const
{
	AActor* Actor = Container.Get();
	if (!Actor) { return; }
	USpotLightComponent* Light = Slot.Light.Get();
	if (!Light)
	{
		Light = NewObject<USpotLightComponent>(Actor);
		Light->SetupAttachment(Actor->GetRootComponent());
		Light->SetMobility(EComponentMobility::Movable);
		// NOTA UE: in candele l'intensita' e' quella di StreetLights.h (SpotFor):
		// al piede del palo, H metri sotto, Candela / H² lux.
		Light->SetIntensityUnits(ELightUnits::Candelas);
		Light->SetInnerConeAngle(45.0f);
		Light->SetOuterConeAngle(72.0f);
		Light->SetSourceRadius(static_cast<float>(0.15 * Units::MetersToUu));
		Light->SetVolumetricScatteringIntensity(0.0f);
		Light->SetCastShadows(bShadows);
		Light->SetIntensity(0.0f);
		Light->RegisterComponent();
		Slot.Light = Light;
	}
	FTransform Transform = Snapshot.GetLocalNeuTransform(Slot.Origin);
	Transform.SetScale3D(FVector(Units::MetersToUu));
	const FVector Location = Transform.TransformPosition(Slot.Local);
	const FVector Down = -Transform.GetRotation().RotateVector(FVector::UpVector);
	Light->SetWorldLocationAndRotation(Location, FRotationMatrix::MakeFromX(Down).Rotator());
	Light->SetAttenuationRadius(static_cast<float>(StreetLights::SpotFor(Slot.HeightM, 1.0, 1.0, 1.0, 1.0).RadiusM * Units::MetersToUu));
	Light->SetLightColor(FLinearColor(Slot.Colour.R, Slot.Colour.G, Slot.Colour.B, 1.0f), /*bSRGB=*/false);
}

void UGeoStreetLightsSubsystem::UpdateRealLights(float DeltaTime)
{
	const float Step = DeltaTime / StreetLightFadeS;
	for (FRealLight& Slot : RealLights)
	{
		USpotLightComponent* Light = Slot.Light.Get();
		if (!Light) { continue; }
		Slot.Weight = Slot.Weight < Slot.Target ? FMath::Min(Slot.Target, Slot.Weight + Step) : FMath::Max(Slot.Target, Slot.Weight - Step);
		const double On = StreetLights::LampOnLevel(AmbientLux, Slot.SwitchLux, bMorning);
		const StreetLights::FSpotLight Spot = StreetLights::SpotFor(Slot.HeightM, Slot.Output, FootLux, On, Dimming);
		// Colour.A: quello che il colore ha perso portando il massimo a 1.
		const float Intensity = static_cast<float>(Spot.Candela) * Slot.Colour.A * Slot.Weight;
		const bool bVisible = Intensity > 0.0f;
		if (Light->IsVisible() != bVisible) { Light->SetVisibility(bVisible); }
		if (bVisible && FMath::Abs(Light->Intensity - Intensity) > 1.0e-3f * FMath::Max(Intensity, 1.0e-3f)) { Light->SetIntensity(Intensity); }
		if (Slot.Weight <= 0.0f && Slot.Target <= 0.0f) { Slot.Id = 0; }
	}
}

void UGeoStreetLightsSubsystem::RemoveRealLights()
{
	for (FRealLight& Slot : RealLights)
	{
		if (USpotLightComponent* Light = Slot.Light.Get()) { Light->DestroyComponent(); }
	}
	RealLights.Empty();
}

void UGeoStreetLightsSubsystem::OnGeoreferenceRebased(const FGeoreferenceSnapshot& Snapshot)
{
	// I lampioni sono nel frame della loro cella: si sposta solo il frame.
	for (TPair<uint64, FCell>& Pair : LampCells)
	{
		FTransform Transform = Snapshot.GetLocalNeuTransform(Pair.Value.Origin);
		Transform.SetScale3D(FVector(Units::MetersToUu));
		Pair.Value.WorldTransform = Transform;
		if (UHierarchicalInstancedStaticMeshComponent* Component = Pair.Value.Sprites.Get()) { Component->SetWorldTransform(Transform); }
		for (const TWeakObjectPtr<UHierarchicalInstancedStaticMeshComponent>& Weak : Pair.Value.Poles)
		{
			if (UHierarchicalInstancedStaticMeshComponent* Component = Weak.Get()) { Component->SetWorldTransform(Transform); }
		}
	}
	for (FRealLight& Slot : RealLights)
	{
		if (Slot.Light.IsValid()) { PlaceRealLight(Slot, Snapshot); }
	}
}

// --- L'overlay ------------------------------------------------------------------------

void UGeoStreetLightsSubsystem::DrawDebugOverlay() const
{
	if (!GEngine) { return; }
	int32 MessageKey = 0x7E60;
	auto Line = [&MessageKey](const FColor& Colour, const FString& Text)
	{
		GEngine->AddOnScreenDebugMessage(MessageKey++, 0.0f, Colour, Text);
	};

	Line(FColor::Cyan, TEXT("--- GeoWorld | lampioni ---"));
	if (!Streaming || !Streaming->IsDatasetOpen())
	{
		Line(FColor::Yellow, TEXT("Nessun dataset di strade: i lampioni stanno lungo le strade (geo.Roads.Open <cartella>)"));
		return;
	}
	if (!bEnabled)
	{
		Line(FColor::Yellow, TEXT("Spenti: geo.Lights.Enable 1"));
		return;
	}
	static const TCHAR* ForceNames[3] = { TEXT("fotocellula"), TEXT("sempre spenti"), TEXT("sempre accesi") };
	Line(FColor::White, FString::Printf(TEXT("Luce       : %.1f lux (%s), %s; accensione media %.0f%%; flusso %.0f%%; al piede %.4f lux di scena"),
		AmbientLux, bMorning ? TEXT("mattina") : TEXT("sera"), ForceNames[FMath::Clamp(Force + 1, 0, 2)], GetOnLevel() * 100.0, Dimming * 100.0, FootLux));   /* percentuali, non-unita */
	Line(FColor::White, FString::Printf(TEXT("Raggi      : pali fino a %.0f m, punti luce fino a %.0f m; %d luci vere (ombre: %s)"),
		Bands.NearM, Bands.MidM, LightCount, bShadows ? TEXT("si'") : TEXT("no")));
	Line(FColor::White, FString::Printf(TEXT("Celle      : %d con i pali, %d lontane, %d vuote, %d in attesa, %d in costruzione (%.1f ms l'una)"),
		Stats.CelleVicine, Stats.CelleLontane, Stats.CelleVuote, Stats.CelleInAttesa, Stats.CostruzioniInCorso, Stats.MillisecondiPerCella));
	Line(FColor::White, FString::Printf(TEXT("Lampioni   : %d punti luce (%s), %d pali, %d luci vere accese, %d componenti"),
		Stats.Lampioni, bSpritesVisible ? TEXT("visibili") : TEXT("nascosti di giorno"), Stats.Pali, Stats.LuciVere, Stats.Componenti));
	const FString Problem = GetMaterialProblem();
	if (!Problem.IsEmpty()) { Line(FColor::Red, Problem); }

	// Le celle (verde con i pali, arancio solo punti) e le luci vere (gialle).
	UWorld* World = GetWorld();
	const UGeoreferenceSubsystem* Georeference = World ? World->GetSubsystem<UGeoreferenceSubsystem>() : nullptr;
	if (!Georeference) { return; }
	const FGeoreferenceSnapshot Snapshot = Georeference->GetSnapshot();
	for (const TPair<uint64, FCell>& Pair : LampCells)
	{
		const FCell& Cell = Pair.Value;
		if (!Cell.Sprites.IsValid() && Cell.Poles.Num() == 0) { continue; }
		const Tiles::FTileBounds Bounds = Tiles::GetTileBounds(Cell.Key.Level, Cell.Key.X, Cell.Key.Y);
		const double Height = Cell.GroundM + 3.0;
		const FVector Corners[4] = {
			Snapshot.GeodeticToUnreal(Core::FGeodetic::FromDegrees(Bounds.North, Bounds.West, Height)),
			Snapshot.GeodeticToUnreal(Core::FGeodetic::FromDegrees(Bounds.North, Bounds.East, Height)),
			Snapshot.GeodeticToUnreal(Core::FGeodetic::FromDegrees(Bounds.South, Bounds.East, Height)),
			Snapshot.GeodeticToUnreal(Core::FGeodetic::FromDegrees(Bounds.South, Bounds.West, Height)) };
		const FColor Colour = Cell.Band == 0 ? FColor::Green : FColor::Orange;
		for (int32 Side = 0; Side < 4; ++Side)
		{
			DrawDebugLine(World, Corners[Side], Corners[(Side + 1) % 4], Colour, false, -1.0f, 0, 60.0f);
		}
	}
	for (const FRealLight& Slot : RealLights)
	{
		const USpotLightComponent* Light = Slot.Light.Get();
		if (!Light || Slot.Weight <= 0.0f) { continue; }
		DrawDebugSphere(World, Light->GetComponentLocation(), static_cast<float>(0.6 * Units::MetersToUu), 6, FColor::Yellow, false, -1.0f, 0, 3.0f);
	}
}

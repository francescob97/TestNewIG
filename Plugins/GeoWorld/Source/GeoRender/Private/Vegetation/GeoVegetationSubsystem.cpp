#include "Vegetation/GeoVegetationSubsystem.h"

#include "GeoCoreModule.h"
#include "Geo/GeoUnits.h"
#include "Georeference/GeoreferenceSubsystem.h"
#include "Mesh/TileMesh.h"
#include "Streaming/GeoTileStreamingSubsystem.h"
#include "Streaming/GeoVegetationStreamingSubsystem.h"
#include "Terrain/GeoTerrainSubsystem.h"
#include "Tiles/TilingScheme.h"
#include "Vegetation/GeoTreeModels.h"
#include "Vegetation/GeoVegetationSettings.h"
#include "Vegetation/TreeMesh.h"
#include "Water/GeoWaterSubsystem.h"
#include "Weather/GeoWeatherSubsystem.h"
#include "Weather/WindField.h"

#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "DrawDebugHelpers.h"
#include "Engine/Engine.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/PlatformTime.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialParameterCollection.h"
#include "Materials/MaterialParameterCollectionInstance.h"
#include "Misc/PackageName.h"
#include "MeshDescription.h"
#include "StaticMeshResources.h"
#include "UObject/Package.h"

#include <cmath>

using namespace GeoWorld;
using GeoWorld::Tiles::FTileKey;

namespace
{
	const TCHAR* VegetationMaterialPath = TEXT("/GeoWorld/Materials/M_GeoVegetation.M_GeoVegetation");
	const TCHAR* VegetationFallbackMaterialPath = TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial");
	const TCHAR* VegetationWeatherCollectionPath = TEXT("/GeoWorld/Materials/MPC_GeoWeather.MPC_GeoWeather");

	/** Ogni quanto si rifa' il conto delle celle, s. */
	constexpr double VegetationSynchronisePeriodS = 0.25;
	/**
	 * Sotto questa grandezza sullo schermo (il raggio dell'albero diviso
	 * l'altezza dello schermo, circa) si passa alla forma semplice: un albero
	 * di 15 m oltre ~450 m.
	 */
	constexpr float VegetationFarLodScreenSize = 0.02f;
	/** L'orologio del vento (MPC VegetationWind.w) ricomincia ogni giorno: un float resta preciso al centesimo. */
	constexpr double VegetationWindClockPeriodS = 86400.0;

	/**
	 * Una forma (specie, variante) come UStaticMesh con due LOD: il
	 * dettagliato da vicino, il semplice da lontano.
	 *
	 * NOTA UE: bFastBuild costruisce i dati di disegno direttamente dalla
	 * FMeshDescription, senza la pipeline dell'editor: funziona anche nel
	 * gioco impacchettato. Le soglie dei LOD le si mette a mano DOPO, nei dati
	 * di disegno (ScreenSize): senza, varrebbe sempre il LOD 0.
	 */
	UStaticMesh* BuildShapeMesh(Vegetation::ESpecies Species, uint32 Variant, bool bFlipWinding, UMaterialInterface* Material)
	{
		Vegetation::FTreeMesh Near, Far;
		Vegetation::BuildTreeMesh(Species, Vegetation::ETreeDetail::Near, Variant, Near);
		Vegetation::BuildTreeMesh(Species, Vegetation::ETreeDetail::Far, Variant, Far);
		FMeshDescription NearDescription, FarDescription;
		GeoVegetationModels::FillMeshDescription(Near, bFlipWinding, NearDescription);
		GeoVegetationModels::FillMeshDescription(Far, bFlipWinding, FarDescription);

		UStaticMesh* Mesh = NewObject<UStaticMesh>(GetTransientPackage(), NAME_None, RF_Transient);
		Mesh->GetStaticMaterials().Add(FStaticMaterial(Material, GeoVegetationModels::SlotName()));

		// NOTA UE: se uno di questi campi non c'e' nella tua versione, togli la
		// riga: sono tutti "non fare" (niente pacchetto sporco, niente
		// collisione, niente copia per l'editor).
		UStaticMesh::FBuildMeshDescriptionsParams BuildParameters;
		BuildParameters.bMarkPackageDirty = false;
		BuildParameters.bBuildSimpleCollision = false;
		BuildParameters.bCommitMeshDescription = false;
		BuildParameters.bFastBuild = true;
		TArray<const FMeshDescription*> Lods;
		Lods.Add(&NearDescription);
		Lods.Add(&FarDescription);
		if (!Mesh->BuildFromMeshDescriptions(Lods, BuildParameters))
		{
			UE_LOG(LogGeoWorld, Warning, TEXT("[GeoVegetation] la forma %s/%u non si costruisce"),
				UTF8_TO_TCHAR(Vegetation::SpeciesName(Species)), Variant);
			return nullptr;
		}
		if (FStaticMeshRenderData* RenderData = Mesh->GetRenderData())
		{
			RenderData->ScreenSize[0].Default = 1.0f;
			RenderData->ScreenSize[1].Default = VegetationFarLodScreenSize;
		}
		return Mesh;
	}
}

// ============================================================================

bool UGeoVegetationSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	if (!Super::ShouldCreateSubsystem(Outer)) { return false; }
	const UWorld* World = Cast<UWorld>(Outer);
	return World && (World->WorldType == EWorldType::Game
	              || World->WorldType == EWorldType::PIE
	              || World->WorldType == EWorldType::Editor);
}

void UGeoVegetationSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Collection.InitializeDependency<UGeoTerrainSubsystem>();
	Collection.InitializeDependency<UGeoVegetationStreamingSubsystem>();
	Collection.InitializeDependency<UGeoWeatherSubsystem>();

	UWorld* World = GetWorld();
	Terrain = World->GetSubsystem<UGeoTerrainSubsystem>();
	Streaming = World->GetSubsystem<UGeoVegetationStreamingSubsystem>();
	Queue = MakeShared<FCellQueue, ESPMode::ThreadSafe>();

	if (UGeoreferenceSubsystem* Georeference = World->GetSubsystem<UGeoreferenceSubsystem>())
	{
		RebaseHandle = Georeference->OnGeoreferenceRebased.AddUObject(this, &UGeoVegetationSubsystem::OnGeoreferenceRebased);
	}
}

void UGeoVegetationSubsystem::Deinitialize()
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
	if (AActor* Actor = Container.Get()) { Actor->Destroy(); }
	Container.Reset();
	for (TStrongObjectPtr<UStaticMesh>& Slot : Shapes) { Slot.Reset(); }
	Material.Reset();
	WeatherCollection.Reset();
	Super::Deinitialize();
}

TStatId UGeoVegetationSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UGeoVegetationSubsystem, STATGROUP_Tickables);
}

// --- Impostazioni -----------------------------------------------------------------

void UGeoVegetationSubsystem::SetEnabled(bool bInEnabled)
{
	if (bEnabled == bInEnabled) { return; }
	bEnabled = bInEnabled;
	if (!bEnabled)
	{
		++Generation;
		RemoveAllCells();
	}
	LastSynchronise = -100.0;
}

void UGeoVegetationSubsystem::OnDatasetChanged()
{
	++Generation;
	RemoveAllCells();
	LastSynchronise = -100.0;
}

void UGeoVegetationSubsystem::SetDensity(double InDensity)
{
	Placement.Density = FMath::Clamp(InDensity, 0.0, 4.0);
	// Le celle si rifanno, ma le vecchie restano a schermo finche' le nuove
	// non sono pronte: firma azzerata, componenti al loro posto.
	++Generation;
	for (TPair<uint64, FCell>& Pair : Cells) { Pair.Value.Signature = 0; }
	LastSynchronise = -100.0;
}

void UGeoVegetationSubsystem::SetRadiusM(double InRadiusM)
{
	Bands.SetFarM(FMath::Clamp(InRadiusM, 300.0, 10000.0));
	LastSynchronise = -100.0;
}

void UGeoVegetationSubsystem::SetShadowMode(int32 InMode)
{
	ShadowMode = FMath::Clamp(InMode, 0, 2);
	UpdateShadows();
}

bool UGeoVegetationSubsystem::CastsShadow(int32 Band) const
{
	return ShadowMode == 2 || (ShadowMode == 1 && Band == 0);
}

void UGeoVegetationSubsystem::SetFlipWinding(bool bInFlip)
{
	if (bFlipWinding == bInFlip) { return; }
	bFlipWinding = bInFlip;
	ReloadModels();
}

void UGeoVegetationSubsystem::ReloadModels()
{
	for (TStrongObjectPtr<UStaticMesh>& Slot : Shapes) { Slot.Reset(); }
	for (FShapeFit& Fit : Fits) { Fit = FShapeFit(); }
	for (TPair<uint64, FCell>& Pair : Cells)
	{
		if (Pair.Value.Instances.IsValid()) { ApplyCell(Pair.Value); }
	}
}

FString UGeoVegetationSubsystem::GetModelsDescription() const
{
	int32 Assets = 0, Nanite = 0, Built = 0;
	for (int32 Shape = 0; Shape < GeoWorld::Vegetation::ShapeCount; ++Shape)
	{
		if (!Shapes[Shape].IsValid()) { continue; }
		if (Fits[Shape].bAsset) { ++Assets; Nanite += Fits[Shape].bNanite ? 1 : 0; }
		else { ++Built; }
	}
	if (Assets + Built == 0) { return TEXT("non ancora caricati"); }
	FString Text = FString::Printf(TEXT("%d modelli dalla libreria (%d Nanite), %d forme fatte dal codice"), Assets, Nanite, Built);
	if (Built > 0) { Text += TEXT(" -- geo.Vegetation.CreateModels salva quelle Nanite"); }
	return Text;
}

FString UGeoVegetationSubsystem::GetMaterialProblem() const
{
	if (Material.IsValid() && bMaterialIsFallback)
	{
		return TEXT("manca M_GeoVegetation: gli alberi sono grigi e fermi. Crealo con geo.Imagery.CreateMaterial");
	}
	if (bWindParameterMissing)
	{
		return TEXT("MPC_GeoWeather e' di prima della vegetazione (manca VegetationWind): rilancia geo.Imagery.CreateMaterial");
	}
	return FString();
}

// --- Il giro ----------------------------------------------------------------------

void UGeoVegetationSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	if (!Streaming || !Streaming->IsDatasetOpen() || !bEnabled)
	{
		if (Cells.Num() > 0) { RemoveAllCells(); }
		Stats = FGeoVegetationStats();
		if (bShowDebugOverlay) { DrawDebugOverlay(); }
		return;
	}

	EnsureContainer();
	CheckMaterial();
	const double Now = FPlatformTime::Seconds();
	if (Now - LastSynchronise >= VegetationSynchronisePeriodS)
	{
		LastSynchronise = Now;
		Synchronise();
	}
	DrainFinished(MaxAppliesPerFrame);
	WriteWind(DeltaTime);
	if (bShowDebugOverlay) { DrawDebugOverlay(); }
}

double UGeoVegetationSubsystem::CameraAboveGround(double LatDeg, double LonDeg, double HeightM) const
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

uint64 UGeoVegetationSubsystem::GatherGround(const Tiles::FTileBounds& Bounds, Vegetation::FGroundSurfaces* OutGround) const
{
	std::vector<FTileKey> Keys;
	for (const FVisibleTile& Visible : VisibleTiles)
	{
		if (Vegetation::BoundsOverlap(Visible.Bounds, Bounds)) { Keys.push_back(Visible.Key); }
	}
	if (OutGround)
	{
		const UGeoTileStreamingSubsystem* Heights = GetWorld()->GetSubsystem<UGeoTileStreamingSubsystem>();
		if (!Heights) { return 0; }
		for (const FTileKey& Key : Keys)
		{
			// Le quote di una tile a schermo sono pinnate in cache: ci sono.
			// Se mancassero, la cella aspetta il giro dopo.
			UGeoTileStreamingSubsystem::FTilePtr HeightTile = Heights->PeekLoadedTile(Key);
			if (!HeightTile || !HeightTile->IsValid()) { return 0; }
			OutGround->Add(MoveTemp(HeightTile), MeshStep);
		}
	}
	return Vegetation::SurfaceSignature(MoveTemp(Keys), MeshStep);
}

void UGeoVegetationSubsystem::Synchronise()
{
	UWorld* World = GetWorld();
	const UGeoreferenceSubsystem* Georeference = World ? World->GetSubsystem<UGeoreferenceSubsystem>() : nullptr;
	FVector Location = FVector::ZeroVector;
	if (!Georeference || !Georeference->GetActiveViewLocation(Location) || !Terrain)
	{
		bHaveCamera = false;
		return;
	}

	// Le tile di terreno a schermo adesso: su queste stanno gli alberi.
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
	bHaveCamera = true;

	const FGeoVegetationDataset& Dataset = Streaming->GetDataset();
	const uint32 DatasetLevel = static_cast<uint32>(Dataset.GetLevel());
	const uint32 Level = FMath::Max(Vegetation::CellLevel, DatasetLevel);
	const uint32 Shift = Level - DatasetLevel;

	// 1. Le celle che servono, dalla piu' vicina, con la loro banda.
	std::vector<Vegetation::FCellRef> Around;
	Vegetation::CellsAround(CameraLatDeg, CameraLonDeg, CameraAboveGroundM, Bands.FarM + Bands.HysteresisM, Level, Around);
	TSet<uint64> Seen;
	Seen.Reserve(static_cast<int32>(Around.size()));
	for (const Vegetation::FCellRef& Ref : Around)
	{
		const uint64 Packed = Ref.Key.Pack();
		FCell* Existing = Cells.Find(Packed);
		const int32 Band = Vegetation::BandFor(Ref.DistanceM, Bands, Existing ? Existing->WantedBand : -1);
		if (Band == Vegetation::BandOutside && !Existing) { continue; }
		Seen.Add(Packed);
		if (!Existing)
		{
			const Tiles::FTileBounds Bounds = Tiles::GetTileBounds(Ref.Key.Level, Ref.Key.X, Ref.Key.Y);
			FCell New;
			New.Key = Ref.Key;
			New.Origin = Core::FGeodetic::FromDegrees(Bounds.CentreLat(), Bounds.CentreLon(), 0.0);
			Existing = &Cells.Add(Packed, MoveTemp(New));
		}
		Existing->WantedBand = Band;
		Existing->DistanceM = Ref.DistanceM;
	}

	// 2. Quelle uscite dal raggio: via. Se avevano un lavoro in volo, al
	//    ritorno non trova la cella e si butta (DrainFinished).
	TArray<uint64> Gone;
	for (TPair<uint64, FCell>& Pair : Cells)
	{
		if (!Seen.Contains(Pair.Key)) { Pair.Value.WantedBand = Vegetation::BandOutside; }
		if (Pair.Value.WantedBand == Vegetation::BandOutside) { Gone.Add(Pair.Key); }
	}
	for (const uint64 Packed : Gone)
	{
		if (FCell* Cell = Cells.Find(Packed)) { DestroyComponents(*Cell); }
		Cells.Remove(Packed);
	}

	// 3. Per ogni cella, dalla piu' vicina: la copertura del suolo, il
	//    terreno sotto, e se e' da rifare.
	int32 BandApplies = 0;
	for (const Vegetation::FCellRef& Ref : Around)
	{
		const uint64 Packed = Ref.Key.Pack();
		FCell* Cell = Cells.Find(Packed);
		if (!Cell) { continue; }

		const FTileKey CoverKey{ DatasetLevel, Ref.Key.X >> Shift, Ref.Key.Y >> Shift };
		const EGeoTileState CoverState = Streaming->RequestTile(CoverKey, static_cast<int32>(Ref.DistanceM));
		if (CoverState == EGeoTileState::Assente || CoverState == EGeoTileState::Errore)
		{
			// Qui non cresce niente (o il file e' rotto: l'Output Log lo dice).
			if (!Cell->bEmpty) { DestroyComponents(*Cell); }
			Cell->bEmpty = true;
			Cell->Instances.Reset();
			Cell->Signature = 0;
			continue;
		}
		Cell->bEmpty = false;
		if (CoverState != EGeoTileState::Pronta) { continue; }

		const Tiles::FTileBounds Bounds = Tiles::GetTileBounds(Ref.Key.Level, Ref.Key.X, Ref.Key.Y);
		const uint64 Signature = GatherGround(Bounds, nullptr);
		if (Signature != 0 && Signature != Cell->Signature && !InFlight.Contains(Packed) && InFlight.Num() < MaxJobsInFlight)
		{
			LaunchCell(*Cell, Signature);
		}
		// Cambio di banda: gli alberi ci sono gia', cambia quanti se ne mostrano.
		if (Cell->Instances.IsValid() && Cell->Band != Cell->WantedBand && BandApplies < 2 * MaxAppliesPerFrame)
		{
			ApplyCell(*Cell);
			++BandApplies;
		}
	}

	// 4. Le statistiche.
	FGeoVegetationStats Next;
	for (const TPair<uint64, FCell>& Pair : Cells)
	{
		const FCell& Cell = Pair.Value;
		if (Cell.bEmpty) { ++Next.CelleVuote; continue; }
		if (Cell.Components.Num() == 0)
		{
			if (!Cell.Instances.IsValid()) { ++Next.CelleInAttesa; }
			continue;
		}
		Next.CelleVicine += (Cell.Band == 0) ? 1 : 0;
		Next.CelleMedie += (Cell.Band == 1) ? 1 : 0;
		Next.CelleLontane += (Cell.Band == 2) ? 1 : 0;
		Next.Latifoglie += Cell.Counts[static_cast<int32>(Vegetation::ESpecies::Broadleaf)];
		Next.Conifere += Cell.Counts[static_cast<int32>(Vegetation::ESpecies::Conifer)];
		Next.PiniDomestici += Cell.Counts[static_cast<int32>(Vegetation::ESpecies::UmbrellaPine)];
		Next.Arbusti += Cell.Counts[static_cast<int32>(Vegetation::ESpecies::Shrub)];
		Next.Componenti += Cell.Components.Num();
	}
	Next.CostruzioniInCorso = InFlight.Num();
	Next.MillisecondiPerCella = CompletedJobs > 0 ? static_cast<float>(TotalJobSeconds / CompletedJobs * 1000.0) : 0.0f;
	Stats = Next;
}

bool UGeoVegetationSubsystem::LaunchCell(FCell& Cell, uint64 Signature)
{
	const FGeoVegetationDataset& Dataset = Streaming->GetDataset();
	const uint32 DatasetLevel = static_cast<uint32>(Dataset.GetLevel());
	const uint32 Shift = Cell.Key.Level - DatasetLevel;
	const FTileKey CoverKey{ DatasetLevel, Cell.Key.X >> Shift, Cell.Key.Y >> Shift };
	const UGeoVegetationStreamingSubsystem::FVegetationTilePtr Cover = Streaming->FindLoadedTile(CoverKey);
	if (!Cover || !Cover->IsValid() || !Queue.IsValid()) { return false; }

	const Tiles::FTileBounds Bounds = Tiles::GetTileBounds(Cell.Key.Level, Cell.Key.X, Cell.Key.Y);
	TSharedPtr<Vegetation::FGroundSurfaces, ESPMode::ThreadSafe> Ground = MakeShared<Vegetation::FGroundSurfaces, ESPMode::ThreadSafe>();
	if (GatherGround(Bounds, Ground.Get()) != Signature) { return false; }

	// Il geoide (per la quota sul mare, che decide le specie e il limite del
	// bosco) viene dal dataset dell'acqua. Senza, le quote sono sull'ellissoide:
	// in Italia ~45 m in piu', il limite del bosco scende di tanto.
	double Geoid = 0.0;
	if (const UGeoWaterSubsystem* Water = GetWorld()->GetSubsystem<UGeoWaterSubsystem>())
	{
		Water->GetGeoidUndulation(Bounds.CentreLon(), Bounds.CentreLat(), Geoid);
	}

	const Vegetation::FPlacementSettings Settings = Placement;
	const int32 JobGeneration = Generation;
	const FTileKey Key = Cell.Key;
	TSharedPtr<FCellQueue, ESPMode::ThreadSafe> JobQueue = Queue;

	// Tutto per copia, niente "this": come le mesh del terreno e dell'acqua.
	UE::Tasks::FTask Task = UE::Tasks::Launch(TEXT("GeoVegetationCell"),
		[Cover, Ground, Bounds, Settings, Geoid, JobGeneration, Key, Signature, JobQueue]()
		{
			const double Started = FPlatformTime::Seconds();
			TSharedPtr<FInstances, ESPMode::ThreadSafe> Instances = MakeShared<FInstances, ESPMode::ThreadSafe>();
			Vegetation::BuildCellInstances(Bounds, Settings, *Cover, *Ground, Geoid, *Instances);
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

void UGeoVegetationSubsystem::DrainFinished(int32 Budget)
{
	FCellResult Result;
	while (Queue.IsValid() && Queue->Completed.Dequeue(Result)) { PendingResults.Add(MoveTemp(Result)); }

	// Un risultato resta "in volo" finche' non e' consegnato: cosi' il giro
	// delle celle non rilancia lo stesso lavoro mentre aspetta il suo turno.
	int32 Applied = 0;
	int32 Index = 0;
	while (Index < PendingResults.Num())
	{
		FCellResult& Pending = PendingResults[Index];
		const uint64 Packed = Pending.Key.Pack();
		FCell* Cell = Cells.Find(Packed);
		const bool bUseful = Pending.Generation == Generation && Cell && Cell->WantedBand != Vegetation::BandOutside;
		if (bUseful && Applied >= Budget) { ++Index; continue; }
		if (bUseful)
		{
			Cell->Instances = Pending.Instances;
			Cell->Signature = Pending.Signature;
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

void UGeoVegetationSubsystem::EnsureContainer()
{
	if (Container.IsValid()) { return; }
	UWorld* World = GetWorld();
	if (!World) { return; }
	// Come il terreno: un attore transient senza nome fisso (SetActorLabel per l'Outliner).
	FActorSpawnParameters SpawnParameters;
	SpawnParameters.ObjectFlags |= RF_Transient;
	AActor* Actor = World->SpawnActor<AActor>(AActor::StaticClass(), SpawnParameters);
	if (!Actor) { return; }
	Actor->SetRootComponent(NewObject<USceneComponent>(Actor, TEXT("Root")));
	Actor->GetRootComponent()->SetMobility(EComponentMobility::Movable);
	Actor->GetRootComponent()->RegisterComponent();
#if WITH_EDITOR
	Actor->SetActorLabel(TEXT("GeoWorld Vegetation"));
#endif
	Container = Actor;
}

void UGeoVegetationSubsystem::CheckMaterial()
{
	const double Now = FPlatformTime::Seconds();
	if (Material.IsValid() && !bMaterialIsFallback) { return; }
	if (Material.IsValid() && Now - LastAssetLookup < 2.0) { return; }
	LastAssetLookup = Now;

	UMaterialInterface* Found = LoadObject<UMaterialInterface>(nullptr, VegetationMaterialPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
	if (!WeatherCollection.IsValid())
	{
		WeatherCollection.Reset(LoadObject<UMaterialParameterCollection>(nullptr, VegetationWeatherCollectionPath, nullptr,
			LOAD_NoWarn | LOAD_Quiet));
	}
	if (Found)
	{
		const bool bWasFallback = Material.IsValid();
		Material.Reset(Found);
		bMaterialIsFallback = false;
		// Il materiale e' arrivato adesso (geo.Imagery.CreateMaterial con il
		// Play aperto): i componenti che ci sono lo prendono subito.
		if (bWasFallback)
		{
			for (TPair<uint64, FCell>& Pair : Cells)
			{
				for (const TWeakObjectPtr<UHierarchicalInstancedStaticMeshComponent>& Weak : Pair.Value.Components)
				{
					UHierarchicalInstancedStaticMeshComponent* Component = Weak.Get();
					if (Component && UsesVegetationMaterial(Component->GetStaticMesh())) { Component->SetMaterial(0, Found); }
				}
			}
		}
		return;
	}
	if (!Material.IsValid())
	{
		Material.Reset(LoadObject<UMaterialInterface>(nullptr, VegetationFallbackMaterialPath));
		bMaterialIsFallback = true;
		UE_LOG(LogGeoWorld, Warning, TEXT("[GeoVegetation] %s"), *GetMaterialProblem());
	}
}

bool UGeoVegetationSubsystem::UsesVegetationMaterial(const UStaticMesh* Mesh) const
{
	for (int32 Shape = 0; Shape < GeoWorld::Vegetation::ShapeCount; ++Shape)
	{
		if (Mesh && Shapes[Shape].Get() == Mesh) { return !Fits[Shape].bOwnMaterial; }
	}
	return false;
}

bool UGeoVegetationSubsystem::EnsureShapes()
{
	if (!Material.IsValid()) { return false; }
	const UGeoVegetationSettings* Settings = GetDefault<UGeoVegetationSettings>();
	bool bAll = true;
	for (int32 Species = 0; Species < Vegetation::SpeciesCount; ++Species)
	{
		for (int32 Variant = 0; Variant < Vegetation::VariantCount; ++Variant)
		{
			const int32 Index = Vegetation::ShapeIndex(static_cast<uint8>(Species), static_cast<uint8>(Variant));
			TStrongObjectPtr<UStaticMesh>& Slot = Shapes[Index];
			if (Slot.IsValid()) { continue; }
			FShapeFit& Fit = Fits[Index];
			Fit = FShapeFit();

			// 1. Il modello della libreria (Project Settings), se c'e'. Si
			//    controlla che il package esista prima di caricarlo: un modello
			//    non ancora creato non deve riempire il log di avvisi.
			UStaticMesh* Model = nullptr;
			if (Settings)
			{
				const TSoftObjectPtr<UStaticMesh> Soft = Settings->GetModel(Species, Variant);
				const FSoftObjectPath& Path = Soft.ToSoftObjectPath();
				if (!Path.IsNull() && FPackageName::DoesPackageExist(Path.GetLongPackageName()))
				{
					Model = Soft.LoadSynchronous();
				}
			}
			if (Model)
			{
				// Il modello si adatta all'albero dai suoi bounds: larghezza sui
				// due assi orizzontali, altezza su Z, la base nell'origine.
				const FBoxSphereBounds Bounds = Model->GetBounds();
				const double Width = 2.0 * FMath::Max(Bounds.BoxExtent.X, Bounds.BoxExtent.Y);
				const double Height = 2.0 * Bounds.BoxExtent.Z;
				if (Width > UE_KINDA_SMALL_NUMBER && Height > UE_KINDA_SMALL_NUMBER)
				{
					Fit.InverseSize = FVector(1.0 / Width, 1.0 / Width, 1.0 / Height);
					Fit.Offset = FVector(-Bounds.Origin.X, -Bounds.Origin.Y, -(Bounds.Origin.Z - Bounds.BoxExtent.Z));
					Fit.bAsset = true;
					Fit.bNanite = Model->HasValidNaniteData();
					// Il materiale: M_GeoVegetation sui modelli di CreateModels
					// (ce l'hanno gia'), il loro su quelli propri.
					UMaterialInterface* Own = Model->GetMaterial(0);
					const bool bOurs = Own && Own->GetPathName() == FString(VegetationMaterialPath);
					Fit.bOwnMaterial = !bOurs && !(Settings && Settings->bForceVegetationMaterial);
					Slot.Reset(Model);
					continue;
				}
				UE_LOG(LogGeoWorld, Warning, TEXT("[GeoVegetation] il modello %s ha bounds vuoti: uso la forma fatta dal codice"),
					*Model->GetPathName());
			}

			// 2. La forma fatta dal codice: due LOD, non Nanite, gia' nel cubo unitario.
			Slot.Reset(BuildShapeMesh(static_cast<Vegetation::ESpecies>(Species), static_cast<uint32>(Variant), bFlipWinding, Material.Get()));
			bAll = bAll && Slot.IsValid();
		}
	}
	return bAll;
}

void UGeoVegetationSubsystem::DestroyComponents(FCell& Cell)
{
	for (const TWeakObjectPtr<UHierarchicalInstancedStaticMeshComponent>& Weak : Cell.Components)
	{
		if (UHierarchicalInstancedStaticMeshComponent* Component = Weak.Get()) { Component->DestroyComponent(); }
	}
	Cell.Components.Reset();
	Cell.Band = -1;
	for (int32& Count : Cell.Counts) { Count = 0; }
}

void UGeoVegetationSubsystem::RemoveAllCells()
{
	for (TPair<uint64, FCell>& Pair : Cells) { DestroyComponents(Pair.Value); }
	Cells.Empty();
}

void UGeoVegetationSubsystem::ApplyCell(FCell& Cell)
{
	DestroyComponents(Cell);
	Cell.Band = Cell.WantedBand;
	AActor* Actor = Container.Get();
	const UGeoreferenceSubsystem* Georeference = GetWorld() ? GetWorld()->GetSubsystem<UGeoreferenceSubsystem>() : nullptr;
	if (Cell.bEmpty || !Cell.Instances.IsValid() || Cell.Band < 0 || Cell.Band >= Vegetation::BandOutside
		|| !Actor || !Georeference || !EnsureShapes())
	{
		return;
	}

	// Gli alberi che la banda tiene, divisi per forma. Thin e' fisso per
	// albero: avvicinandosi se ne aggiungono, nessuno cambia posto.
	const float Fraction = Vegetation::BandFraction(Cell.Band);
	TArray<FTransform> Transforms[Vegetation::ShapeCount];
	TArray<float> Data[Vegetation::ShapeCount];
	double GroundSum = 0.0;
	int32 GroundCount = 0;
	for (const Vegetation::FTreeInstance& Tree : *Cell.Instances)
	{
		GroundSum += Tree.Local[2];
		++GroundCount;
		if (Tree.Thin >= Fraction) { continue; }
		const int32 Shape = Vegetation::ShapeIndex(Tree.Species, Tree.Variant);
		// Nel frame della cella, in metri: X nord, Y est, Z alto. La forma va
		// larga WidthM sui due assi orizzontali e alta HeightM su Z: le forme
		// del codice sono gia' nel cubo unitario (Fit neutro), un modello
		// della libreria si scala dai suoi bounds e si sposta con la base
		// nell'origine. Lo spostamento si fa PRIMA di scala e rotazione:
		// R(S(x + o)) + t = R(S x) + (t + R(S o)).
		const FShapeFit& Fit = Fits[Shape];
		const FQuat Rotation(FVector::UpVector, Tree.YawRad);
		const FVector Scale = FVector(Tree.WidthM, Tree.WidthM, Tree.HeightM) * Fit.InverseSize;
		const FVector Location = FVector(Tree.Local[0], Tree.Local[1], Tree.Local[2]) + Rotation.RotateVector(Scale * Fit.Offset);
		Transforms[Shape].Emplace(Rotation, Location, Scale);
		float Values[Vegetation::InstanceDataFloats];
		Vegetation::InstanceData(Tree, Values);
		Data[Shape].Append(Values, Vegetation::InstanceDataFloats);
		++Cell.Counts[FMath::Clamp<int32>(Tree.Species, 0, Vegetation::SpeciesCount - 1)];
	}
	Cell.GroundM = GroundCount > 0 ? GroundSum / GroundCount : 0.0;

	FTransform Transform = Georeference->GetSnapshot().GetLocalNeuTransform(Cell.Origin);
	Transform.SetScale3D(FVector(Units::MetersToUu));
	const bool bShadow = CastsShadow(Cell.Band);
	for (int32 Shape = 0; Shape < Vegetation::ShapeCount; ++Shape)
	{
		if (Transforms[Shape].Num() == 0 || !Shapes[Shape].IsValid()) { continue; }
		UHierarchicalInstancedStaticMeshComponent* Component = NewObject<UHierarchicalInstancedStaticMeshComponent>(Actor);
		Component->SetupAttachment(Actor->GetRootComponent());
		Component->SetMobility(EComponentMobility::Movable);
		Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Component->SetCanEverAffectNavigation(false);
		Component->SetStaticMesh(Shapes[Shape].Get());
		if (!Fits[Shape].bOwnMaterial) { Component->SetMaterial(0, Material.Get()); }
		Component->SetCastShadow(bShadow);
		// Il vento solo da vicino. Oltre ~700 m la cima di un albero si sposta
		// meno di un pixel, e un'ombra che si muove costringe le Virtual
		// Shadow Maps a ridisegnare le loro pagine a ogni frame; con Nanite,
		// in piu', un materiale che sposta i vertici prende la strada lenta.
		Component->SetEvaluateWorldPositionOffset(Cell.Band == 0);
		// I dati per istanza (il verde, l'altezza, il caso) PRIMA delle
		// istanze: il componente li alloca per ognuna mentre le aggiunge.
		Component->SetNumCustomDataFloats(Vegetation::InstanceDataFloats);
		Component->AddInstances(Transforms[Shape], /*bShouldReturnIndices=*/false, /*bWorldSpace=*/false);
		for (int32 InstanceIndex = 0; InstanceIndex < Transforms[Shape].Num(); ++InstanceIndex)
		{
			Component->SetCustomData(InstanceIndex,
				TArrayView<const float>(Data[Shape].GetData() + InstanceIndex * Vegetation::InstanceDataFloats, Vegetation::InstanceDataFloats),
				/*bMarkRenderStateDirty=*/false);
		}
		Component->RegisterComponent();
		Component->SetWorldTransform(Transform);
		Cell.Components.Add(Component);
	}
}

void UGeoVegetationSubsystem::UpdateShadows()
{
	for (TPair<uint64, FCell>& Pair : Cells)
	{
		const bool bShadow = CastsShadow(Pair.Value.Band);
		for (const TWeakObjectPtr<UHierarchicalInstancedStaticMeshComponent>& Weak : Pair.Value.Components)
		{
			if (UHierarchicalInstancedStaticMeshComponent* Component = Weak.Get()) { Component->SetCastShadow(bShadow); }
		}
	}
}

void UGeoVegetationSubsystem::OnGeoreferenceRebased(const FGeoreferenceSnapshot& Snapshot)
{
	// Le istanze sono nel frame della loro cella: si sposta solo il frame.
	for (TPair<uint64, FCell>& Pair : Cells)
	{
		FTransform Transform = Snapshot.GetLocalNeuTransform(Pair.Value.Origin);
		Transform.SetScale3D(FVector(Units::MetersToUu));
		for (const TWeakObjectPtr<UHierarchicalInstancedStaticMeshComponent>& Weak : Pair.Value.Components)
		{
			if (UHierarchicalInstancedStaticMeshComponent* Component = Weak.Get()) { Component->SetWorldTransform(Transform); }
		}
	}
}

// --- Il vento -------------------------------------------------------------------------

void UGeoVegetationSubsystem::WriteWind(float DeltaTime)
{
	WindClock = FMath::Fmod(WindClock + static_cast<double>(DeltaTime), VegetationWindClockPeriodS);
	UWorld* World = GetWorld();
	const UGeoWeatherSubsystem* WeatherSubsystem = World ? World->GetSubsystem<UGeoWeatherSubsystem>() : nullptr;
	UMaterialParameterCollectionInstance* WeatherParameters = (World && WeatherCollection.IsValid())
		? World->GetParameterCollectionInstance(WeatherCollection.Get()) : nullptr;
	if (!WeatherSubsystem || !WeatherParameters || !bHaveCamera) { return; }

	// Il vento a 10 m dal suolo sotto la camera, come lo sente un albero: il
	// medio per la direzione e la forza, la raffica di adesso per il di piu'.
	const UGeoreferenceSubsystem* Georeference = World->GetSubsystem<UGeoreferenceSubsystem>();
	FVector Location = FVector::ZeroVector;
	if (!Georeference || !Georeference->GetActiveViewLocation(Location)) { return; }
	const Core::FGeodetic Here = Georeference->GetSnapshot().UnrealToGeodetic(Location);
	const double GroundM = Here.HeightM - CameraAboveGroundM;
	const GeoWorld::Weather::FWindSample Wind = WeatherSubsystem->SampleWind(CameraLatDeg, CameraLonDeg, GroundM + GeoWorld::Weather::ReferenceHeightM);
	const double Gust = FMath::Clamp((Wind.Speed() - Wind.MeanSpeed()) / FMath::Max(2.0 * Wind.SigmaU, 0.1), 0.0, 1.0);
	const bool bSet = WeatherParameters->SetVectorParameterValue(TEXT("VegetationWind"),
		FLinearColor(static_cast<float>(Wind.MeanNorth), static_cast<float>(Wind.MeanEast), static_cast<float>(Gust),
			static_cast<float>(WindClock)));
	bWindParameterMissing = !bSet;
}

// --- L'overlay ------------------------------------------------------------------------

void UGeoVegetationSubsystem::DrawDebugOverlay() const
{
	if (!GEngine) { return; }
	int32 MessageKey = 0x7E40;
	auto Line = [&MessageKey](const FColor& Colour, const FString& Text)
	{
		GEngine->AddOnScreenDebugMessage(MessageKey++, 0.0f, Colour, Text);
	};

	Line(FColor::Cyan, TEXT("--- GeoWorld | vegetazione ---"));
	if (!Streaming || !Streaming->IsDatasetOpen())
	{
		Line(FColor::Yellow, TEXT("Nessun dataset di vegetazione: geo.Vegetation.Open <cartella> (geoworld build-vegetation)"));
		return;
	}
	const FGeoVegetationDataset& Dataset = Streaming->GetDataset();
	Line(FColor::White, FString::Printf(TEXT("Dataset    : '%s', livello %d, %lld tile"),
		*Dataset.GetDatasetName(), Dataset.GetLevel(), static_cast<long long>(Dataset.GetIndexedTileCount())));
	if (!bEnabled)
	{
		Line(FColor::Yellow, TEXT("Spenta: geo.Vegetation.Enable 1"));
		return;
	}
	static const TCHAR* ShadowNames[3] = { TEXT("nessuna"), TEXT("solo i vicini"), TEXT("tutti") };
	Line(FColor::White, FString::Printf(TEXT("Bande      : vicino %.0f m, medio %.0f m, lontano %.0f m; densita' %.2f; ombre: %s"),
		Bands.NearM, Bands.MidM, Bands.FarM, Placement.Density, ShadowNames[FMath::Clamp(ShadowMode, 0, 2)]));
	Line(FColor::White, FString::Printf(TEXT("Modelli    : %s"), *GetModelsDescription()));
	Line(FColor::White, FString::Printf(TEXT("Celle      : %d vicine, %d medie, %d lontane, %d vuote, %d in attesa, %d in costruzione"),
		Stats.CelleVicine, Stats.CelleMedie, Stats.CelleLontane, Stats.CelleVuote, Stats.CelleInAttesa, Stats.CostruzioniInCorso));
	Line(FColor::White, FString::Printf(TEXT("Alberi     : %d (latifoglie %d, conifere %d, pini domestici %d, arbusti %d), %d componenti"),
		Stats.GetTreeCount(), Stats.Latifoglie, Stats.Conifere, Stats.PiniDomestici, Stats.Arbusti, Stats.Componenti));
	Line(FColor::White, FString::Printf(TEXT("Camera     : %.0f m sopra il suolo; %d tile di terreno a schermo; %.1f ms per cella"),
		CameraAboveGroundM, VisibleTiles.Num(), Stats.MillisecondiPerCella));
	const FString Problem = GetMaterialProblem();
	if (!Problem.IsEmpty()) { Line(FColor::Red, Problem); }

	// I contorni delle celle, al suolo medio dei loro alberi: verde vicino,
	// giallo medio, arancio lontano.
	UWorld* World = GetWorld();
	const UGeoreferenceSubsystem* Georeference = World ? World->GetSubsystem<UGeoreferenceSubsystem>() : nullptr;
	if (!Georeference) { return; }
	const FGeoreferenceSnapshot Snapshot = Georeference->GetSnapshot();
	static const FColor BandColours[3] = { FColor::Green, FColor::Yellow, FColor::Orange };
	for (const TPair<uint64, FCell>& Pair : Cells)
	{
		const FCell& Cell = Pair.Value;
		if (Cell.Components.Num() == 0 || Cell.Band < 0 || Cell.Band > 2) { continue; }
		const Tiles::FTileBounds Bounds = Tiles::GetTileBounds(Cell.Key.Level, Cell.Key.X, Cell.Key.Y);
		const double Height = Cell.GroundM + 2.0;
		const FVector Corners[4] = {
			Snapshot.GeodeticToUnreal(Core::FGeodetic::FromDegrees(Bounds.North, Bounds.West, Height)),
			Snapshot.GeodeticToUnreal(Core::FGeodetic::FromDegrees(Bounds.North, Bounds.East, Height)),
			Snapshot.GeodeticToUnreal(Core::FGeodetic::FromDegrees(Bounds.South, Bounds.East, Height)),
			Snapshot.GeodeticToUnreal(Core::FGeodetic::FromDegrees(Bounds.South, Bounds.West, Height)) };
		for (int32 Side = 0; Side < 4; ++Side)
		{
			DrawDebugLine(World, Corners[Side], Corners[(Side + 1) % 4], BandColours[Cell.Band], false, -1.0f, 0, 60.0f);
		}
	}
}

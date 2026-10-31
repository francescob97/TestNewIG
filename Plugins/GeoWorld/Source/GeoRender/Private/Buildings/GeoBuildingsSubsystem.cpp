#include "Buildings/GeoBuildingsSubsystem.h"

#include "Buildings/GeoBuildingSettings.h"
#include "GeoCoreModule.h"
#include "Geo/GeoUnits.h"
#include "Georeference/GeoreferenceSubsystem.h"
#include "Mesh/TileMesh.h"
#include "Streaming/GeoBuildingStreamingSubsystem.h"
#include "Streaming/GeoTileStreamingSubsystem.h"
#include "Terrain/GeoTerrainSubsystem.h"
#include "Tiles/TilingScheme.h"

#include "Components/DynamicMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "DrawDebugHelpers.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "Engine/Engine.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/PlatformTime.h"
#include "Materials/MaterialInterface.h"
#include "Misc/PackageName.h"

#include <algorithm>

using namespace GeoWorld;
using GeoWorld::Tiles::FTileKey;

/** La mesh di una cella, preparata sul worker: si consegna al componente con uno spostamento. */
struct FGeoPreparedBuildingMesh
{
	UE::Geometry::FDynamicMesh3 Mesh;
};

namespace
{
	const TCHAR* BuildingMaterialPath = TEXT("/GeoWorld/Materials/M_GeoBuilding.M_GeoBuilding");
	const TCHAR* BuildingFallbackMaterialPath = TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial");
	constexpr double BuildingSynchronisePeriodS = 0.25;

	/**
	 * FBuildingMesh -> FDynamicMesh3, sul worker.
	 *
	 * NOTA UE: ogni vertice ha i suoi attributi (normale, colore, due UV): gli
	 * "elementi" degli overlay hanno gli stessi indici dei vertici, come per il
	 * terreno. UV0 la facciata, UV1 lo stile (BuildingShader.inl); il colore
	 * (il byte / 255) va al materiale cosi' com'e': il componente, senza
	 * trasformazioni di spazio colore (il suo default), lo passa invariato.
	 */
	TSharedPtr<FGeoPreparedBuildingMesh, ESPMode::ThreadSafe> PrepareBuildingMesh(const Buildings::FBuildingMesh& Source, int32& OutTriangles)
	{
		OutTriangles = 0;
		if (Source.TriangleCount() == 0) { return nullptr; }
		TSharedPtr<FGeoPreparedBuildingMesh, ESPMode::ThreadSafe> Prepared = MakeShared<FGeoPreparedBuildingMesh, ESPMode::ThreadSafe>();
		using namespace UE::Geometry;
		FDynamicMesh3& Mesh = Prepared->Mesh;
		Mesh.EnableAttributes();
		Mesh.Attributes()->SetNumUVLayers(2);
		Mesh.Attributes()->EnablePrimaryColors();
		FDynamicMeshNormalOverlay* Normals = Mesh.Attributes()->PrimaryNormals();
		FDynamicMeshUVOverlay* Facade = Mesh.Attributes()->GetUVLayer(0);
		FDynamicMeshUVOverlay* Style = Mesh.Attributes()->GetUVLayer(1);
		FDynamicMeshColorOverlay* Colours = Mesh.Attributes()->PrimaryColors();

		// Un vertice con i suoi attributi: posizione, normale, facciata, stile,
		// colore. Gli elementi degli overlay vanno di pari passo con i vertici
		// (stesso indice).
		auto AppendVertex = [&](int32 Index)
		{
			const int32 Id = Mesh.AppendVertex(FVector3d(Source.Positions[Index * 3], Source.Positions[Index * 3 + 1], Source.Positions[Index * 3 + 2]));
			Normals->AppendElement(FVector3f(Source.Normals[Index * 3], Source.Normals[Index * 3 + 1], Source.Normals[Index * 3 + 2]));
			Facade->AppendElement(FVector2f(Source.Facade[Index * 2], Source.Facade[Index * 2 + 1]));
			Style->AppendElement(FVector2f(Source.Style[Index * 2], Source.Style[Index * 2 + 1]));
			Colours->AppendElement(FVector4f(Source.Colours[Index * 4] / 255.0f, Source.Colours[Index * 4 + 1] / 255.0f,
				Source.Colours[Index * 4 + 2] / 255.0f, Source.Colours[Index * 4 + 3] / 255.0f));
			return Id;
		};

		// I vertici in METRI: la scala in centimetri e' nella trasformazione del componente.
		const int32 VertexCount = static_cast<int32>(Source.VertexCount());
		for (int32 Index = 0; Index < VertexCount; ++Index) { AppendVertex(Index); }

		const int32 TriangleCount = static_cast<int32>(Source.TriangleCount());
		for (int32 Index = 0; Index < TriangleCount; ++Index)
		{
			int32 A = static_cast<int32>(Source.Indices[Index * 3]);
			int32 B = static_cast<int32>(Source.Indices[Index * 3 + 1]);
			int32 C = static_cast<int32>(Source.Indices[Index * 3 + 2]);
			int32 TriangleId = Mesh.AppendTriangle(A, B, C);
			if (TriangleId == FDynamicMesh3::NonManifoldID)
			{
				// NOTA UE: FDynamicMesh3 non accetta un lato con piu' di due
				// triangoli (succede dove un cortile tocca il contorno, o con
				// due sagome che si sovrappongono). Il triangolo si aggiunge
				// lo stesso, con vertici suoi: a schermo e' identico.
				A = AppendVertex(A);
				B = AppendVertex(B);
				C = AppendVertex(C);
				TriangleId = Mesh.AppendTriangle(A, B, C);
			}
			if (TriangleId < 0) { continue; }   // un triangolo degenere: si salta
			Normals->SetTriangle(TriangleId, FIndex3i(A, B, C));
			Facade->SetTriangle(TriangleId, FIndex3i(A, B, C));
			Style->SetTriangle(TriangleId, FIndex3i(A, B, C));
			Colours->SetTriangle(TriangleId, FIndex3i(A, B, C));
			++OutTriangles;
		}
		return Prepared;
	}
}

// ============================================================================

bool UGeoBuildingsSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	if (!Super::ShouldCreateSubsystem(Outer)) { return false; }
	const UWorld* World = Cast<UWorld>(Outer);
	return World && (World->WorldType == EWorldType::Game
	              || World->WorldType == EWorldType::PIE
	              || World->WorldType == EWorldType::Editor);
}

void UGeoBuildingsSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Collection.InitializeDependency<UGeoTerrainSubsystem>();
	Collection.InitializeDependency<UGeoBuildingStreamingSubsystem>();

	UWorld* World = GetWorld();
	Terrain = World->GetSubsystem<UGeoTerrainSubsystem>();
	Streaming = World->GetSubsystem<UGeoBuildingStreamingSubsystem>();
	Queue = MakeShared<FCellQueue, ESPMode::ThreadSafe>();
	// Le bande degli edifici: si vedono piu' lontano degli alberi.
	Bands.NearM = 1500.0;
	Bands.MidM = 3500.0;
	Bands.FarM = 6000.0;
	Bands.HysteresisM = 100.0;

	if (UGeoreferenceSubsystem* Georeference = World->GetSubsystem<UGeoreferenceSubsystem>())
	{
		RebaseHandle = Georeference->OnGeoreferenceRebased.AddUObject(this, &UGeoBuildingsSubsystem::OnGeoreferenceRebased);
	}
}

void UGeoBuildingsSubsystem::Deinitialize()
{
	if (UWorld* World = GetWorld())
	{
		if (UGeoreferenceSubsystem* Georeference = World->GetSubsystem<UGeoreferenceSubsystem>())
		{
			Georeference->OnGeoreferenceRebased.Remove(RebaseHandle);
		}
	}
	for (TPair<uint64, UE::Tasks::FTask>& Pair : InFlight) { Pair.Value.Wait(); }
	InFlight.Empty();
	PendingResults.Empty();
	RemoveAllCells();
	if (AActor* Actor = Container.Get()) { Actor->Destroy(); }
	Container.Reset();
	LandmarkModels.Empty();
	Material.Reset();
	Super::Deinitialize();
}

TStatId UGeoBuildingsSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UGeoBuildingsSubsystem, STATGROUP_Tickables);
}

// --- Impostazioni -----------------------------------------------------------------

void UGeoBuildingsSubsystem::SetEnabled(bool bInEnabled)
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

void UGeoBuildingsSubsystem::OnDatasetChanged()
{
	++Generation;
	RemoveAllCells();
	LastSynchronise = -100.0;
}

void UGeoBuildingsSubsystem::SetRadiusM(double InRadiusM)
{
	Bands.SetFarM(FMath::Clamp(InRadiusM, 500.0, 20000.0));
	LastSynchronise = -100.0;
}

void UGeoBuildingsSubsystem::SetShadowMode(int32 InMode)
{
	ShadowMode = FMath::Clamp(InMode, 0, 2);
	for (TPair<uint64, FCell>& Pair : Cells)
	{
		const bool bShadow = CastsShadow(Pair.Value.Band);
		if (UDynamicMeshComponent* Component = Pair.Value.Component.Get()) { Component->SetCastShadow(bShadow); }
		for (const FPlacedLandmark& Landmark : Pair.Value.Landmarks)
		{
			if (UStaticMeshComponent* Model = Landmark.Component.Get()) { Model->SetCastShadow(bShadow); }
		}
	}
}

bool UGeoBuildingsSubsystem::CastsShadow(int32 Band) const
{
	return ShadowMode == 2 || (ShadowMode == 1 && Band == 0);
}

void UGeoBuildingsSubsystem::ReloadLandmarks()
{
	bLandmarksLoaded = false;
	LoadLandmarks();
	// Le celle si rifanno: un monumento nuovo toglie la sua sagoma, uno tolto la rimette.
	++Generation;
	for (TPair<uint64, FCell>& Pair : Cells) { Pair.Value.Signature = 0; Pair.Value.LaunchedBand = -1; }
	LastSynchronise = -100.0;
}

FString UGeoBuildingsSubsystem::GetMaterialProblem() const
{
	if (Material.IsValid() && bMaterialIsFallback)
	{
		return TEXT("manca M_GeoBuilding: edifici grigi, senza finestre. Crealo con geo.Imagery.CreateMaterial");
	}
	return FString();
}

Buildings::FBuildingDetail UGeoBuildingsSubsystem::DetailFor(int32 Band) const
{
	Buildings::FBuildingDetail Detail;
	if (Band == 1)
	{
		Detail.MinAreaM2 = 40.0;
		Detail.SimplifyM = 0.5;
	}
	else if (Band >= 2)
	{
		// Da lontano: niente box e baracche, contorni semplici, niente finestre
		// (sarebbero piu' piccole di un pixel e farebbero solo scintillare).
		Detail.MinAreaM2 = 150.0;
		Detail.SimplifyM = 2.0;
		Detail.bWindows = false;
	}
	return Detail;
}

void UGeoBuildingsSubsystem::LoadLandmarks()
{
	if (bLandmarksLoaded) { return; }
	bLandmarksLoaded = true;
	LandmarkModels.Reset();
	const UGeoBuildingSettings* Settings = GetDefault<UGeoBuildingSettings>();
	if (!Settings) { return; }
	for (const FGeoLandmark& Entry : Settings->Landmarks)
	{
		FLandmarkModel Loaded;
		if (!UGeoBuildingSettings::ParseId(Entry.Id, Loaded.OsmId, Loaded.Wikidata))
		{
			UE_LOG(LogGeoWorld, Warning, TEXT("[GeoBuildings] monumento '%s': id non valido (Q123, way/123, relation/123)"), *Entry.Id);
			continue;
		}
		const FSoftObjectPath& Path = Entry.Model.ToSoftObjectPath();
		UStaticMesh* ModelMesh = (!Path.IsNull() && FPackageName::DoesPackageExist(Path.GetLongPackageName())) ? Entry.Model.LoadSynchronous() : nullptr;
		if (!ModelMesh)
		{
			UE_LOG(LogGeoWorld, Warning, TEXT("[GeoBuildings] monumento '%s': il modello non c'e'"), *Entry.Id);
			continue;
		}
		Loaded.Mesh.Reset(ModelMesh);
		Loaded.YawDegrees = Entry.YawDegrees;
		Loaded.HeightOffsetM = Entry.HeightOffsetM;
		Loaded.bFitToFootprint = Entry.bFitToFootprint;
		LandmarkModels.Add(MoveTemp(Loaded));
	}
	UE_LOG(LogGeoWorld, Log, TEXT("[GeoBuildings] %d monumenti nella libreria"), LandmarkModels.Num());
}

const UGeoBuildingsSubsystem::FLandmarkModel* UGeoBuildingsSubsystem::FindLandmark(uint64 OsmId, uint32 Wikidata) const
{
	for (const FLandmarkModel& Model : LandmarkModels)
	{
		if ((Model.OsmId != 0 && Model.OsmId == OsmId) || (Model.Wikidata != 0 && Model.Wikidata == Wikidata)) { return &Model; }
	}
	return nullptr;
}

// --- Il giro ----------------------------------------------------------------------

void UGeoBuildingsSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	if (!Streaming || !Streaming->IsDatasetOpen() || !bEnabled)
	{
		if (Cells.Num() > 0) { RemoveAllCells(); }
		Stats = FGeoBuildingsStats();
		if (bShowDebugOverlay) { DrawDebugOverlay(); }
		return;
	}
	EnsureContainer();
	CheckMaterial();
	LoadLandmarks();
	const double Now = FPlatformTime::Seconds();
	if (Now - LastSynchronise >= BuildingSynchronisePeriodS)
	{
		LastSynchronise = Now;
		Synchronise();
	}
	DrainFinished(MaxAppliesPerFrame);
	if (bShowDebugOverlay) { DrawDebugOverlay(); }
}

double UGeoBuildingsSubsystem::CameraAboveGround(double LatDeg, double LonDeg, double HeightM) const
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

uint64 UGeoBuildingsSubsystem::GatherGround(const Tiles::FTileBounds& Bounds, Cells::FGroundSurfaces* OutGround) const
{
	// Gli edifici sporgono un po' dalla loro tile (ci stanno quelli col centro
	// dentro): il terreno si prende un po' piu' largo.
	const double Margin = 0.25 * (Bounds.North - Bounds.South);
	const Tiles::FTileBounds Wide{ Bounds.West - Margin, Bounds.South - Margin, Bounds.East + Margin, Bounds.North + Margin };
	std::vector<FTileKey> Keys;
	for (const FVisibleTile& Visible : VisibleTiles)
	{
		if (Cells::BoundsOverlap(Visible.Bounds, Wide)) { Keys.push_back(Visible.Key); }
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

void UGeoBuildingsSubsystem::Synchronise()
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
	bHaveCamera = true;

	// Le celle SONO le tile del dataset (livello 15 di default): gli edifici
	// di una tile sono quelli col baricentro dentro, e una cella li fa tutti.
	const uint32 Level = static_cast<uint32>(Streaming->GetDataset().GetLevel());

	// 1. Le celle che servono, dalla piu' vicina, con la loro banda.
	std::vector<Cells::FCellRef> Around;
	Cells::CellsAround(CameraLatDeg, CameraLonDeg, CameraAboveGroundM, Bands.FarM + Bands.HysteresisM, Level, Around);
	TSet<uint64> Seen;
	Seen.Reserve(static_cast<int32>(Around.size()));
	for (const Cells::FCellRef& Ref : Around)
	{
		// Solo le tile che il dataset ha: una cella senza edifici non costa niente.
		if (!Streaming->GetDataset().TileExists(Ref.Key)) { continue; }
		const uint64 Packed = Ref.Key.Pack();
		FCell* Existing = Cells.Find(Packed);
		const int32 Band = Cells::BandFor(Ref.DistanceM, Bands, Existing ? Existing->WantedBand : -1);
		if (Band == Cells::BandOutside && !Existing) { continue; }
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

	// 2. Quelle uscite dal raggio: via (un lavoro in volo, al ritorno, si butta).
	TArray<uint64> Gone;
	for (TPair<uint64, FCell>& Pair : Cells)
	{
		if (!Seen.Contains(Pair.Key)) { Pair.Value.WantedBand = Cells::BandOutside; }
		if (Pair.Value.WantedBand == Cells::BandOutside) { Gone.Add(Pair.Key); }
	}
	for (const uint64 Packed : Gone)
	{
		if (FCell* Cell = Cells.Find(Packed)) { DestroyCell(*Cell); }
		Cells.Remove(Packed);
	}

	// 3. Per ogni cella, dalla piu' vicina: la tile, il terreno, se e' da rifare.
	for (const Cells::FCellRef& Ref : Around)
	{
		const uint64 Packed = Ref.Key.Pack();
		FCell* Cell = Cells.Find(Packed);
		if (!Cell) { continue; }
		const EGeoTileState State = Streaming->RequestTile(Ref.Key, static_cast<int32>(Ref.DistanceM));
		if (State == EGeoTileState::Assente || State == EGeoTileState::Errore)
		{
			if (!Cell->bEmpty) { DestroyCell(*Cell); }
			Cell->bEmpty = true;
			continue;
		}
		if (State != EGeoTileState::Pronta) { continue; }
		const Tiles::FTileBounds Bounds = Tiles::GetTileBounds(Ref.Key.Level, Ref.Key.X, Ref.Key.Y);
		const uint64 Signature = GatherGround(Bounds, nullptr);
		// Da rifare se il terreno sotto e' cambiato, o se e' cambiata la banda
		// (un'altra banda vuol dire un altro dettaglio: un'altra mesh).
		const bool bStale = Signature != Cell->Signature || Cell->WantedBand != Cell->LaunchedBand;
		if (Signature != 0 && bStale && !InFlight.Contains(Packed) && InFlight.Num() < MaxJobsInFlight)
		{
			LaunchCell(*Cell, Signature);
		}
	}

	// 4. Le statistiche.
	FGeoBuildingsStats Next;
	for (const TPair<uint64, FCell>& Pair : Cells)
	{
		const FCell& Cell = Pair.Value;
		if (Cell.bEmpty) { continue; }
		if (!Cell.Component.IsValid() && Cell.Landmarks.Num() == 0)
		{
			++Next.CelleInAttesa;
			continue;
		}
		Next.CelleVicine += (Cell.Band == 0) ? 1 : 0;
		Next.CelleMedie += (Cell.Band == 1) ? 1 : 0;
		Next.CelleLontane += (Cell.Band == 2) ? 1 : 0;
		Next.Edifici += Cell.Buildings;
		Next.Triangoli += Cell.Triangles;
		Next.Monumenti += Cell.Landmarks.Num();
	}
	Next.CostruzioniInCorso = InFlight.Num();
	Next.MillisecondiPerCella = CompletedJobs > 0 ? static_cast<float>(TotalJobSeconds / CompletedJobs * 1000.0) : 0.0f;
	Stats = Next;
}

bool UGeoBuildingsSubsystem::LaunchCell(FCell& Cell, uint64 Signature)
{
	const UGeoBuildingStreamingSubsystem::FBuildingTilePtr Tile = Streaming->FindLoadedTile(Cell.Key);
	if (!Tile || !Queue.IsValid()) { return false; }
	const Tiles::FTileBounds Bounds = Tiles::GetTileBounds(Cell.Key.Level, Cell.Key.X, Cell.Key.Y);
	TSharedPtr<Cells::FGroundSurfaces, ESPMode::ThreadSafe> Ground = MakeShared<Cells::FGroundSurfaces, ESPMode::ThreadSafe>();
	if (GatherGround(Bounds, Ground.Get()) != Signature) { return false; }

	std::vector<uint64_t> LandmarkOsmIds;
	std::vector<uint32_t> LandmarkWikidata;
	for (const FLandmarkModel& Model : LandmarkModels)
	{
		if (Model.OsmId) { LandmarkOsmIds.push_back(Model.OsmId); }
		if (Model.Wikidata) { LandmarkWikidata.push_back(Model.Wikidata); }
	}
	const int32 Band = FMath::Clamp(Cell.WantedBand, 0, 2);
	const Buildings::FBuildingDetail Detail = DetailFor(Band);
	const int32 JobGeneration = Generation;
	const FTileKey Key = Cell.Key;
	TSharedPtr<FCellQueue, ESPMode::ThreadSafe> JobQueue = Queue;

	// Tutto per copia, niente "this": come le celle degli alberi.
	UE::Tasks::FTask Task = UE::Tasks::Launch(TEXT("GeoBuildingCell"),
		[Tile, Ground, Bounds, Detail, LandmarkOsmIds, LandmarkWikidata, JobGeneration, Key, Signature, Band, JobQueue]()
		{
			const double Started = FPlatformTime::Seconds();
			FCellResult Result;
			Result.Key = Key;
			Result.Generation = JobGeneration;
			Result.Signature = Signature;
			Result.Band = Band;
			Buildings::FBuildingMesh Built;
			Buildings::BuildBuildingCell(*Tile, Cells::FCellFrame::At(Bounds.CentreLat(), Bounds.CentreLon()), *Ground, Detail,
				LandmarkOsmIds, LandmarkWikidata, Built, Result.Spots, Result.CellStats);
			Result.Mesh = PrepareBuildingMesh(Built, Result.Triangles);
			Result.Seconds = FPlatformTime::Seconds() - Started;
			JobQueue->Completed.Enqueue(MoveTemp(Result));
		},
		UE::Tasks::ETaskPriority::BackgroundNormal);

	Cell.LaunchedBand = Cell.WantedBand;
	InFlight.Add(Key.Pack(), Task);
	return true;
}

void UGeoBuildingsSubsystem::DrainFinished(int32 Budget)
{
	FCellResult Result;
	while (Queue.IsValid() && Queue->Completed.Dequeue(Result)) { PendingResults.Add(MoveTemp(Result)); }

	// Un risultato resta "in volo" finche' non e' consegnato: il giro non lo rilancia.
	int32 Applied = 0;
	int32 Index = 0;
	while (Index < PendingResults.Num())
	{
		FCellResult& Pending = PendingResults[Index];
		const uint64 Packed = Pending.Key.Pack();
		FCell* Cell = Cells.Find(Packed);
		const bool bUseful = Pending.Generation == Generation && Cell && Cell->WantedBand != Cells::BandOutside;
		if (bUseful && Applied >= Budget) { ++Index; continue; }
		if (bUseful)
		{
			ApplyResult(*Cell, Pending);
			++Applied;
			++CompletedJobs;
			TotalJobSeconds += Pending.Seconds;
		}
		InFlight.Remove(Packed);
		PendingResults.RemoveAtSwap(Index);
	}
}

// --- I componenti ---------------------------------------------------------------------

void UGeoBuildingsSubsystem::EnsureContainer()
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
	Actor->SetActorLabel(TEXT("GeoWorld Buildings"));
#endif
	Container = Actor;
}

void UGeoBuildingsSubsystem::CheckMaterial()
{
	const double Now = FPlatformTime::Seconds();
	if (Material.IsValid() && !bMaterialIsFallback) { return; }
	if (Material.IsValid() && Now - LastAssetLookup < 2.0) { return; }
	LastAssetLookup = Now;
	UMaterialInterface* Found = LoadObject<UMaterialInterface>(nullptr, BuildingMaterialPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
	if (Found)
	{
		const bool bWasFallback = Material.IsValid();
		Material.Reset(Found);
		bMaterialIsFallback = false;
		if (bWasFallback)
		{
			for (TPair<uint64, FCell>& Pair : Cells)
			{
				if (UDynamicMeshComponent* Component = Pair.Value.Component.Get()) { Component->SetMaterial(0, Found); }
			}
		}
		return;
	}
	if (!Material.IsValid())
	{
		Material.Reset(LoadObject<UMaterialInterface>(nullptr, BuildingFallbackMaterialPath));
		bMaterialIsFallback = true;
		UE_LOG(LogGeoWorld, Warning, TEXT("[GeoBuildings] %s"), *GetMaterialProblem());
	}
}

FTransform UGeoBuildingsSubsystem::CellTransform(const FGeoreferenceSnapshot& Snapshot, const FCell& Cell) const
{
	FTransform Transform = Snapshot.GetLocalNeuTransform(Cell.Origin);
	Transform.SetScale3D(FVector(Units::MetersToUu));
	return Transform;
}

void UGeoBuildingsSubsystem::DestroyCell(FCell& Cell)
{
	if (UDynamicMeshComponent* Component = Cell.Component.Get()) { Component->DestroyComponent(); }
	Cell.Component.Reset();
	for (const FPlacedLandmark& Landmark : Cell.Landmarks)
	{
		if (UStaticMeshComponent* Model = Landmark.Component.Get()) { Model->DestroyComponent(); }
	}
	Cell.Landmarks.Reset();
	Cell.Band = -1;
	Cell.Buildings = 0;
	Cell.Triangles = 0;
}

void UGeoBuildingsSubsystem::RemoveAllCells()
{
	for (TPair<uint64, FCell>& Pair : Cells) { DestroyCell(Pair.Value); }
	Cells.Empty();
}

void UGeoBuildingsSubsystem::ApplyResult(FCell& Cell, FCellResult& Result)
{
	AActor* Actor = Container.Get();
	const UGeoreferenceSubsystem* Georeference = GetWorld() ? GetWorld()->GetSubsystem<UGeoreferenceSubsystem>() : nullptr;
	if (!Actor || !Georeference) { return; }
	DestroyCell(Cell);
	Cell.Signature = Result.Signature;
	Cell.Band = Result.Band;
	Cell.bEmpty = false;
	Cell.Buildings = Result.CellStats.Built;
	Cell.Triangles = Result.Triangles;
	Cell.GroundM = Result.GroundM;
	const FTransform Transform = CellTransform(Georeference->GetSnapshot(), Cell);
	const bool bShadow = CastsShadow(Cell.Band);

	if (Result.Mesh.IsValid())
	{
		UDynamicMeshComponent* Component = NewObject<UDynamicMeshComponent>(Actor);
		Component->SetupAttachment(Actor->GetRootComponent());
		Component->SetMobility(EComponentMobility::Movable);
		Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Component->SetCanEverAffectNavigation(false);
		// Come le tile del terreno: niente ray tracing per componente.
		// NOTA: se SetEnableRaytracing non compila sulla tua versione, togli la riga.
		Component->SetEnableRaytracing(false);
		Component->SetCastShadow(bShadow);
		if (UMaterialInterface* Surface = Material.Get()) { Component->SetMaterial(0, Surface); }
		Component->RegisterComponent();
		// NOTA UE: lo spostamento (MoveTemp) passa la FDynamicMesh3 preparata sul
		// worker al componente senza copiarla.
		Component->SetMesh(MoveTemp(Result.Mesh->Mesh));
		Component->NotifyMeshUpdated();
		Component->SetWorldTransform(Transform);
		Cell.Component = Component;
	}

	// I monumenti: al posto delle loro sagome, i modelli della libreria.
	for (const Buildings::FLandmarkSpot& Spot : Result.Spots)
	{
		const FLandmarkModel* Model = FindLandmark(Spot.OsmId, Spot.Wikidata);
		if (!Model || !Model->Mesh.IsValid()) { continue; }
		UStaticMesh* ModelMesh = Model->Mesh.Get();
		// Nel frame della cella (metri, X nord, Y est, Z alto): sul baricentro,
		// girato come il lato lungo. Il modello e' in centimetri: per usarlo
		// alla sua grandezza la scala e' 1/100 (la cella poi moltiplica per 100).
		FVector Scale(1.0 / Units::MetersToUu);
		if (Model->bFitToFootprint)
		{
			const FBoxSphereBounds ModelBounds = ModelMesh->GetBounds();
			const double LengthUu = FMath::Max(1.0, 2.0 * ModelBounds.BoxExtent.X);
			const double WidthUu = FMath::Max(1.0, 2.0 * ModelBounds.BoxExtent.Y);
			Scale = FVector(Spot.LengthM / LengthUu, Spot.WidthM / WidthUu, Spot.LengthM / LengthUu);
		}
		const FQuat Yaw(FVector::UpVector, Spot.YawRad + FMath::DegreesToRadians(static_cast<double>(Model->YawDegrees)));
		const FTransform Local(Yaw, FVector(Spot.Local[0], Spot.Local[1], Spot.Local[2] + Model->HeightOffsetM), Scale);
		UStaticMeshComponent* Placed = NewObject<UStaticMeshComponent>(Actor);
		Placed->SetupAttachment(Actor->GetRootComponent());
		Placed->SetMobility(EComponentMobility::Movable);
		Placed->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Placed->SetCanEverAffectNavigation(false);
		Placed->SetStaticMesh(ModelMesh);
		Placed->SetCastShadow(bShadow);
		Placed->RegisterComponent();
		Placed->SetWorldTransform(Local * Transform);
		Cell.Landmarks.Add(FPlacedLandmark{ Placed, Local });
	}
}

void UGeoBuildingsSubsystem::OnGeoreferenceRebased(const FGeoreferenceSnapshot& Snapshot)
{
	// Le mesh sono nel frame della loro cella: si sposta solo il frame.
	for (TPair<uint64, FCell>& Pair : Cells)
	{
		const FTransform Transform = CellTransform(Snapshot, Pair.Value);
		if (UDynamicMeshComponent* Component = Pair.Value.Component.Get()) { Component->SetWorldTransform(Transform); }
		for (const FPlacedLandmark& Landmark : Pair.Value.Landmarks)
		{
			if (UStaticMeshComponent* Model = Landmark.Component.Get()) { Model->SetWorldTransform(Landmark.Local * Transform); }
		}
	}
}

// --- L'overlay ------------------------------------------------------------------------

void UGeoBuildingsSubsystem::DrawDebugOverlay() const
{
	if (!GEngine) { return; }
	int32 MessageKey = 0x7F40;
	auto Line = [&MessageKey](const FColor& Colour, const FString& Text)
	{
		GEngine->AddOnScreenDebugMessage(MessageKey++, 0.0f, Colour, Text);
	};
	Line(FColor::Cyan, TEXT("--- GeoWorld | edifici ---"));
	if (!Streaming || !Streaming->IsDatasetOpen())
	{
		Line(FColor::Yellow, TEXT("Nessun dataset di edifici: geo.Buildings.Open <cartella> (costruisci-edifici.bat)"));
		return;
	}
	const FGeoBuildingDataset& Dataset = Streaming->GetDataset();
	Line(FColor::White, FString::Printf(TEXT("Dataset    : '%s', livello %d, %lld tile, %lld edifici"),
		*Dataset.GetDatasetName(), Dataset.GetLevel(), static_cast<long long>(Dataset.GetIndexedTileCount()),
		static_cast<long long>(Dataset.GetBuildingCount())));
	if (!bEnabled)
	{
		Line(FColor::Yellow, TEXT("Spenti: geo.Buildings.Enable 1"));
		return;
	}
	static const TCHAR* ShadowNames[3] = { TEXT("nessuna"), TEXT("solo i vicini"), TEXT("tutti") };
	Line(FColor::White, FString::Printf(TEXT("Bande      : vicino %.0f m, medio %.0f m, lontano %.0f m; ombre: %s"),
		Bands.NearM, Bands.MidM, Bands.FarM, ShadowNames[FMath::Clamp(ShadowMode, 0, 2)]));
	Line(FColor::White, FString::Printf(TEXT("Celle      : %d vicine, %d medie, %d lontane, %d in attesa, %d in costruzione; %.1f ms per cella"),
		Stats.CelleVicine, Stats.CelleMedie, Stats.CelleLontane, Stats.CelleInAttesa, Stats.CostruzioniInCorso, Stats.MillisecondiPerCella));
	Line(FColor::White, FString::Printf(TEXT("Edifici    : %d, %d triangoli; monumenti: %d a schermo, %d nella libreria"),
		Stats.Edifici, Stats.Triangoli, Stats.Monumenti, LandmarkModels.Num()));
	const FString Problem = GetMaterialProblem();
	if (!Problem.IsEmpty()) { Line(FColor::Red, Problem); }

	UWorld* World = GetWorld();
	const UGeoreferenceSubsystem* Georeference = World ? World->GetSubsystem<UGeoreferenceSubsystem>() : nullptr;
	if (!Georeference) { return; }
	const FGeoreferenceSnapshot Snapshot = Georeference->GetSnapshot();
	static const FColor BandColours[3] = { FColor::Green, FColor::Yellow, FColor::Orange };
	// I contorni delle celle, al suolo del loro centro: verde vicino, giallo
	// medio, arancio lontano.
	for (const TPair<uint64, FCell>& Pair : Cells)
	{
		const FCell& Cell = Pair.Value;
		if (!Cell.Component.IsValid() || Cell.Band < 0 || Cell.Band > 2) { continue; }
		const Tiles::FTileBounds Bounds = Tiles::GetTileBounds(Cell.Key.Level, Cell.Key.X, Cell.Key.Y);
		const double Height = Cell.GroundM + 2.0;
		const FVector Corners[4] = {
			Snapshot.GeodeticToUnreal(Core::FGeodetic::FromDegrees(Bounds.North, Bounds.West, Height)),
			Snapshot.GeodeticToUnreal(Core::FGeodetic::FromDegrees(Bounds.North, Bounds.East, Height)),
			Snapshot.GeodeticToUnreal(Core::FGeodetic::FromDegrees(Bounds.South, Bounds.East, Height)),
			Snapshot.GeodeticToUnreal(Core::FGeodetic::FromDegrees(Bounds.South, Bounds.West, Height)) };
		for (int32 Side = 0; Side < 4; ++Side)
		{
			DrawDebugLine(World, Corners[Side], Corners[(Side + 1) % 4], BandColours[Cell.Band], false, -1.0f, 0, 80.0f);
		}
	}
}

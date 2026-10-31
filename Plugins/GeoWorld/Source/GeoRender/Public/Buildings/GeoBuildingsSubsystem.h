// =============================================================================
//  GeoBuildingsSubsystem.h -- Gli edifici attorno alla camera. STRATO: UNREAL.
//
//  Come la vegetazione, a celle: una cella e' una tile .gbd (livello 15,
//  ~610 x 430 m). Ogni quarto di secondo:
//    1. le celle entro il raggio (6 km), con la loro banda: vicino (tutto,
//       finestre, ombre), medio (via gli edifici piu' piccoli), lontano (solo
//       i grandi, contorni semplificati, senza finestre) (Mesh/GroundCells.h);
//    2. per ogni cella la tile di edifici e le tile di terreno visibili sotto:
//       la FIRMA del terreno dice se e' da rifare;
//    3. sui worker: gli edifici in una mesh (Buildings/BuildingMesh.h), e la
//       FDynamicMesh3 pronta per il componente;
//    4. sul game thread, poche celle per frame: un UDynamicMeshComponent per
//       cella, e i MONUMENTI della libreria (UGeoBuildingSettings) al posto
//       delle loro sagome.
//  La cella vecchia resta a schermo finche' la nuova non e' pronta.
//
//  PERCHE' UDynamicMeshComponent E NON UNA STATIC MESH: la mesh di una cella di
//  citta' ha decine di migliaia di triangoli, ed e' diversa per ogni cella.
//  Una UStaticMesh fatta a runtime si costruisce sul game thread (decine di
//  millisecondi: uno scatto); una FDynamicMesh3 si prepara tutta sul worker e
//  al componente si consegna con uno spostamento. E' la strada del terreno.
//  I monumenti invece sono asset (Nanite) preparati nell'editor.
// =============================================================================
#pragma once

#include "CoreMinimal.h"
#include "Containers/Queue.h"
#include "Subsystems/WorldSubsystem.h"
#include "Tasks/Task.h"
#include "UObject/StrongObjectPtr.h"

#include "Buildings/BuildingMesh.h"
#include "Georeference/GeoreferenceSnapshot.h"
#include "Mesh/GroundCells.h"
#include "Tiles/TileKey.h"

#include <vector>

#include "GeoBuildingsSubsystem.generated.h"

class AActor;
class UDynamicMeshComponent;
class UGeoBuildingStreamingSubsystem;
class UGeoTerrainSubsystem;
class UMaterialInterface;
class UStaticMesh;
class UStaticMeshComponent;

/** La mesh di una cella pronta per il componente (una FDynamicMesh3: vedi il .cpp). */
struct FGeoPreparedBuildingMesh;

USTRUCT()
struct FGeoBuildingsStats
{
	GENERATED_BODY()

	UPROPERTY() int32 CelleVicine = 0;
	UPROPERTY() int32 CelleMedie = 0;
	UPROPERTY() int32 CelleLontane = 0;
	/** Celle che aspettano: la tile di edifici, il terreno, il worker. */
	UPROPERTY() int32 CelleInAttesa = 0;
	UPROPERTY() int32 CostruzioniInCorso = 0;
	UPROPERTY() int32 Edifici = 0;
	UPROPERTY() int32 Monumenti = 0;
	UPROPERTY() int32 Triangoli = 0;
	UPROPERTY() float MillisecondiPerCella = 0.0f;
};

UCLASS()
class GEORENDER_API UGeoBuildingsSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	using FTileKey = GeoWorld::Tiles::FTileKey;

	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual bool IsTickableInEditor() const override { return true; }

	void SetEnabled(bool bInEnabled);
	bool IsEnabled() const { return bEnabled; }

	/** Da chiamare dopo aver aperto (o chiuso) un dataset: tutto si rifa' da capo. */
	void OnDatasetChanged();

	/** Fin dove ci sono edifici, m (le bande vicina e media si allargano insieme). */
	void SetRadiusM(double InRadiusM);
	const GeoWorld::Cells::FBandSettings& GetBands() const { return Bands; }

	/** Chi fa ombra: 0 nessuno, 1 le celle vicine (default), 2 tutte. */
	void SetShadowMode(int32 InMode);
	int32 GetShadowMode() const { return ShadowMode; }

	/** Rilegge la libreria dei monumenti (Project Settings) e rifa' le celle. */
	void ReloadLandmarks();
	int32 GetLandmarkCount() const { return LandmarkModels.Num(); }

	void SetDebugOverlayEnabled(bool bInShow) { bShowDebugOverlay = bInShow; }
	bool IsDebugOverlayEnabled() const { return bShowDebugOverlay; }

	FGeoBuildingsStats GetStats() const { return Stats; }

	/** Vuota se il materiale c'e'; altrimenti la frase da dire in rosso. */
	FString GetMaterialProblem() const;

private:
	/** Il lavoro di una cella, dal worker al game thread. */
	struct FCellResult
	{
		FTileKey Key;
		int32 Generation = 0;
		uint64 Signature = 0;
		int32 Band = 0;
		/** La mesh pronta per il componente (nullptr se nella cella non c'e' niente da disegnare). */
		TSharedPtr<FGeoPreparedBuildingMesh, ESPMode::ThreadSafe> Mesh;
		std::vector<GeoWorld::Buildings::FLandmarkSpot> Spots;
		GeoWorld::Buildings::FBuildingCellStats CellStats;
		int32 Triangles = 0;
		/** Il suolo al centro della cella, m (per l'overlay). */
		double GroundM = 0.0;
		double Seconds = 0.0;
	};
	struct FCellQueue
	{
		TQueue<FCellResult, EQueueMode::Mpsc> Completed;
	};

	/** Un monumento a schermo: il componente e dove sta nel frame della sua cella. */
	struct FPlacedLandmark
	{
		TWeakObjectPtr<UStaticMeshComponent> Component;
		/** Dal modello al frame della cella (metri: la cella ha la scala metri -> cm). */
		FTransform Local;
	};

	struct FCell
	{
		FTileKey Key;
		GeoWorld::Core::FGeodetic Origin;
		uint64 Signature = 0;
		/** La banda della mesh a schermo (-1: nessuna). */
		int32 Band = -1;
		/** La banda voluta adesso. */
		int32 WantedBand = GeoWorld::Cells::BandOutside;
		/** La banda dell'ultimo lavoro lanciato (per non rilanciarlo uguale). */
		int32 LaunchedBand = -1;
		bool bEmpty = false;
		TWeakObjectPtr<UDynamicMeshComponent> Component;
		TArray<FPlacedLandmark> Landmarks;
		int32 Buildings = 0;
		int32 Triangles = 0;
		double GroundM = 0.0;
		double DistanceM = 0.0;
	};

	struct FVisibleTile
	{
		FTileKey Key;
		GeoWorld::Tiles::FTileBounds Bounds;
	};

	/** Un monumento della libreria, letto da Project Settings. */
	struct FLandmarkModel
	{
		uint64 OsmId = 0;
		uint32 Wikidata = 0;
		TStrongObjectPtr<UStaticMesh> Mesh;
		float YawDegrees = 0.0f;
		float HeightOffsetM = 0.0f;
		bool bFitToFootprint = false;
	};

	void Synchronise();
	uint64 GatherGround(const GeoWorld::Tiles::FTileBounds& Bounds, GeoWorld::Cells::FGroundSurfaces* OutGround) const;
	double CameraAboveGround(double LatDeg, double LonDeg, double HeightM) const;
	bool LaunchCell(FCell& Cell, uint64 Signature);
	void DrainFinished(int32 Budget);
	void ApplyResult(FCell& Cell, FCellResult& Result);
	void DestroyCell(FCell& Cell);
	void RemoveAllCells();
	void EnsureContainer();
	void CheckMaterial();
	void LoadLandmarks();
	const FLandmarkModel* FindLandmark(uint64 OsmId, uint32 Wikidata) const;
	bool CastsShadow(int32 Band) const;
	GeoWorld::Buildings::FBuildingDetail DetailFor(int32 Band) const;
	FTransform CellTransform(const FGeoreferenceSnapshot& Snapshot, const FCell& Cell) const;
	void OnGeoreferenceRebased(const FGeoreferenceSnapshot& Snapshot);
	void DrawDebugOverlay() const;

	UPROPERTY(Transient) TObjectPtr<UGeoTerrainSubsystem> Terrain;
	UPROPERTY(Transient) TObjectPtr<UGeoBuildingStreamingSubsystem> Streaming;

	TWeakObjectPtr<AActor> Container;
	TStrongObjectPtr<UMaterialInterface> Material;
	bool bMaterialIsFallback = false;
	double LastAssetLookup = -100.0;

	TArray<FLandmarkModel> LandmarkModels;
	bool bLandmarksLoaded = false;

	TSharedPtr<FCellQueue, ESPMode::ThreadSafe> Queue;
	TMap<uint64, UE::Tasks::FTask> InFlight;
	TArray<FCellResult> PendingResults;
	TMap<uint64, FCell> Cells;
	TArray<FVisibleTile> VisibleTiles;
	int32 MeshStep = 1;
	int32 Generation = 0;

	GeoWorld::Cells::FBandSettings Bands;
	bool bEnabled = true;
	int32 ShadowMode = 1;
	bool bShowDebugOverlay = false;

	double LastSynchronise = -100.0;
	double CameraLatDeg = 0.0, CameraLonDeg = 0.0, CameraAboveGroundM = 0.0;
	bool bHaveCamera = false;
	FDelegateHandle RebaseHandle;

	int32 MaxJobsInFlight = 4;
	int32 MaxAppliesPerFrame = 2;
	int32 CompletedJobs = 0;
	double TotalJobSeconds = 0.0;
	FGeoBuildingsStats Stats;
};

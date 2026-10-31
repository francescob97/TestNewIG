// =============================================================================
//  GeoVegetationSubsystem.h -- Gli alberi attorno alla camera. STRATO: UNREAL.
//
//  Sta in mezzo a:
//    - UGeoVegetationStreamingSubsystem, che carica la copertura del suolo
//      (le tile .gvg di geoworld build-vegetation);
//    - UGeoTerrainSubsystem, da cui prende le tile VISIBILI: gli alberi stanno
//      sul terreno disegnato, non sul DEM;
//    - UGeoWeatherSubsystem, da cui legge il vento (il materiale piega gli
//      alberi) e, attraverso MPC_GeoWeather, la neve.
//
//  IL LAVORO, OGNI QUARTO DI SECONDO
//    1. le celle (livello 15, ~610 x 430 m) entro il raggio, dalla piu'
//       vicina, e la banda di ognuna (Vegetation/VegetationCells.h);
//    2. per ogni cella: la tile di vegetazione (chiesta allo streaming) e le
//       tile di terreno visibili che la toccano. La loro FIRMA dice se la
//       cella e' da rifare: il terreno sotto si e' affinato;
//    3. sui worker: chi cresce dove e le istanze (BuildCellInstances);
//    4. sul game thread, poche celle per frame: un componente di istanze
//       (HISM) per forma, con gli alberi che la banda tiene.
//  Una cella vecchia resta a schermo finche' la nuova non e' pronta: niente
//  buchi nel bosco mentre il terreno si affina.
//
//  LE FORME sono otto mesh fatte da codice all'avvio (Vegetation/TreeMesh.h):
//  quattro specie, due varianti. Ognuna ha due LOD, dettagliato e semplice:
//  il componente sceglie da solo, per gruppi di alberi, in base a quanto sono
//  grandi sullo schermo.
//
//  IL REBASE cambia solo le trasformazioni dei componenti: le istanze sono nel
//  frame della loro cella, in metri.
// =============================================================================
#pragma once

#include "CoreMinimal.h"
#include "Containers/Queue.h"
#include "Subsystems/WorldSubsystem.h"
#include "Tasks/Task.h"
#include "UObject/StrongObjectPtr.h"

#include "Georeference/GeoreferenceSnapshot.h"
#include "Tiles/TileKey.h"
#include "Vegetation/TreePlacement.h"
#include "Vegetation/VegetationCells.h"

#include <vector>

#include "GeoVegetationSubsystem.generated.h"

class AActor;
class UGeoTerrainSubsystem;
class UGeoVegetationStreamingSubsystem;
class UHierarchicalInstancedStaticMeshComponent;
class UMaterialInterface;
class UMaterialParameterCollection;
class UStaticMesh;

USTRUCT()
struct FGeoVegetationStats
{
	GENERATED_BODY()

	/** Celle con alberi a schermo, per banda (vicino, medio, lontano). */
	UPROPERTY() int32 CelleVicine = 0;
	UPROPERTY() int32 CelleMedie = 0;
	UPROPERTY() int32 CelleLontane = 0;
	/** Celle dove non cresce niente (acqua, roccia, citta' fitta, fuori dal dataset). */
	UPROPERTY() int32 CelleVuote = 0;
	/** Celle che aspettano: la copertura del suolo, il terreno, il worker. */
	UPROPERTY() int32 CelleInAttesa = 0;
	UPROPERTY() int32 CostruzioniInCorso = 0;
	/** Alberi a schermo, per specie (TreePlacement.h, ESpecies). */
	UPROPERTY() int32 Latifoglie = 0;
	UPROPERTY() int32 Conifere = 0;
	UPROPERTY() int32 PiniDomestici = 0;
	UPROPERTY() int32 Arbusti = 0;
	UPROPERTY() int32 Componenti = 0;
	UPROPERTY() float MillisecondiPerCella = 0.0f;

	int32 GetTreeCount() const { return Latifoglie + Conifere + PiniDomestici + Arbusti; }
};

UCLASS()
class GEORENDER_API UGeoVegetationSubsystem : public UTickableWorldSubsystem
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

	/** Moltiplica gli alberi di ogni classe (1 = come in natura; 0,5 = la meta'). Rifa' le celle. */
	void SetDensity(double InDensity);
	double GetDensity() const { return Placement.Density; }

	/** Fin dove ci sono alberi, m (le tre bande si allargano insieme). */
	void SetRadiusM(double InRadiusM);
	const GeoWorld::Vegetation::FBandSettings& GetBands() const { return Bands; }

	/**
	 * Chi fa ombra: 0 nessuno, 1 gli alberi della banda vicina (default), 2
	 * tutti. Il 2 ha senso con i modelli Nanite e le Virtual Shadow Maps, che
	 * reggono tante ombre; con le forme fatte dal codice costa molto.
	 */
	void SetShadowMode(int32 InMode);
	int32 GetShadowMode() const { return ShadowMode; }

	/**
	 * Rilegge la libreria dei modelli (Project Settings) e rifa' i componenti:
	 * dopo geo.Vegetation.CreateModels, o dopo aver cambiato un modello.
	 */
	void ReloadModels();
	/** "8 modelli dalla libreria (8 Nanite), 0 forme fatte dal codice". */
	FString GetModelsDescription() const;

	/**
	 * Inverte l'ordine dei vertici delle forme. Come geo.Terrain.FlipWinding:
	 * se gli alberi si vedono "dal di dentro" (spariscono le facce verso di
	 * te), e' questo. Rifa' le forme e tutti i componenti.
	 */
	void SetFlipWinding(bool bInFlip);
	bool IsFlipWinding() const { return bFlipWinding; }

	void SetDebugOverlayEnabled(bool bInShow) { bShowDebugOverlay = bInShow; }
	bool IsDebugOverlayEnabled() const { return bShowDebugOverlay; }

	FGeoVegetationStats GetStats() const { return Stats; }

	/** Vuota se il materiale c'e'; altrimenti la frase da dire in rosso. */
	FString GetMaterialProblem() const;

private:
	using FInstances = std::vector<GeoWorld::Vegetation::FTreeInstance>;

	/** Il lavoro di una cella, dal worker al game thread. */
	struct FCellResult
	{
		FTileKey Key;
		int32 Generation = 0;
		uint64 Signature = 0;
		TSharedPtr<const FInstances, ESPMode::ThreadSafe> Instances;
		double Seconds = 0.0;
	};
	struct FCellQueue
	{
		TQueue<FCellResult, EQueueMode::Mpsc> Completed;
	};

	/** Una cella: cosa si vede adesso, e su che terreno e' stata fatta. */
	struct FCell
	{
		FTileKey Key;
		GeoWorld::Core::FGeodetic Origin;
		/** Il terreno su cui sono gli alberi a schermo (0: nessuno ancora). */
		uint64 Signature = 0;
		/** La banda dei componenti a schermo (-1: nessun componente). */
		int32 Band = -1;
		/** La banda voluta adesso (calcolata a ogni giro). */
		int32 WantedBand = GeoWorld::Vegetation::BandOutside;
		/** Niente cresce qui: la tile di vegetazione non c'e'. */
		bool bEmpty = false;
		TSharedPtr<const FInstances, ESPMode::ThreadSafe> Instances;
		TArray<TWeakObjectPtr<UHierarchicalInstancedStaticMeshComponent>> Components;
		int32 Counts[GeoWorld::Vegetation::SpeciesCount] = { 0, 0, 0, 0 };
		/** La quota media del suolo sotto gli alberi (ellissoidica, m): per l'overlay. */
		double GroundM = 0.0;
		/** La distanza dalla camera all'ultimo giro, m. */
		double DistanceM = 0.0;
	};

	/** Una tile di terreno a schermo, con il suo rettangolo. */
	struct FVisibleTile
	{
		FTileKey Key;
		GeoWorld::Tiles::FTileBounds Bounds;
	};

	void Synchronise();
	/** Cosa c'e' sotto la cella: la firma delle tile visibili che la toccano, e le loro quote. */
	uint64 GatherGround(const GeoWorld::Tiles::FTileBounds& Bounds, GeoWorld::Vegetation::FGroundSurfaces* OutGround) const;
	bool LaunchCell(FCell& Cell, uint64 Signature);
	void DrainFinished(int32 Budget);
	/** Rifa' i componenti della cella con gli alberi della sua banda. */
	void ApplyCell(FCell& Cell);
	void DestroyComponents(FCell& Cell);
	void RemoveAllCells();
	bool EnsureShapes();
	/** La mesh e' una delle forme che usano M_GeoVegetation (non un modello con il suo materiale)? */
	bool UsesVegetationMaterial(const UStaticMesh* Mesh) const;
	bool CastsShadow(int32 Band) const;
	void EnsureContainer();
	void UpdateShadows();
	/** Rilegge M_GeoVegetation se mancava (dopo geo.Imagery.CreateMaterial). */
	void CheckMaterial();
	void WriteWind(float DeltaTime);
	void OnGeoreferenceRebased(const FGeoreferenceSnapshot& Snapshot);
	void DrawDebugOverlay() const;
	/** La quota della camera sopra il terreno disegnato sotto di lei (o 0 se non si sa). */
	double CameraAboveGround(double LatDeg, double LonDeg, double HeightM) const;

	UPROPERTY(Transient) TObjectPtr<UGeoTerrainSubsystem> Terrain;
	UPROPERTY(Transient) TObjectPtr<UGeoVegetationStreamingSubsystem> Streaming;

	/** L'attore che tiene tutti i componenti (World Outliner: "GeoWorld Vegetation"). */
	TWeakObjectPtr<AActor> Container;
	/** Le otto forme: [specie * VariantCount + variante]. */
	TStrongObjectPtr<UStaticMesh> Shapes[GeoWorld::Vegetation::ShapeCount];

	/** Come adattare una forma alla larghezza e all'altezza di un albero. */
	struct FShapeFit
	{
		/** 1 / larghezza, 1 / larghezza, 1 / altezza del modello (1 per le forme del codice). */
		FVector InverseSize = FVector::OneVector;
		/** Porta la base del modello, al centro, nell'origine. */
		FVector Offset = FVector::ZeroVector;
		/** Viene dalla libreria (Project Settings), non dal codice. */
		bool bAsset = false;
		bool bNanite = false;
		/** Tiene il suo materiale (un modello proprio). */
		bool bOwnMaterial = false;
	};
	FShapeFit Fits[GeoWorld::Vegetation::ShapeCount];
	TStrongObjectPtr<UMaterialInterface> Material;
	TStrongObjectPtr<UMaterialParameterCollection> WeatherCollection;
	bool bMaterialIsFallback = false;
	bool bWindParameterMissing = false;
	double LastAssetLookup = -100.0;

	TSharedPtr<FCellQueue, ESPMode::ThreadSafe> Queue;
	TMap<uint64, UE::Tasks::FTask> InFlight;
	TArray<FCellResult> PendingResults;
	TMap<uint64, FCell> Cells;
	/** Le tile di terreno visibili, rilette a ogni giro. */
	TArray<FVisibleTile> VisibleTiles;
	int32 MeshStep = 1;
	int32 Generation = 0;

	GeoWorld::Vegetation::FPlacementSettings Placement;
	GeoWorld::Vegetation::FBandSettings Bands;
	bool bEnabled = true;
	int32 ShadowMode = 1;
	bool bFlipWinding = false;
	bool bShowDebugOverlay = false;

	double LastSynchronise = -100.0;
	double CameraLatDeg = 0.0, CameraLonDeg = 0.0, CameraAboveGroundM = 0.0;
	bool bHaveCamera = false;
	double WindClock = 0.0;
	FDelegateHandle RebaseHandle;

	int32 MaxJobsInFlight = 4;
	int32 MaxAppliesPerFrame = 2;
	int32 CompletedJobs = 0;
	double TotalJobSeconds = 0.0;
	FGeoVegetationStats Stats;
};

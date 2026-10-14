// =============================================================================
//  GeoWaterSubsystem.h -- Mare, laghi e fiumi sul terreno (Fase 9).
//  STRATO: UNREAL.
//
//  Sta in mezzo a:
//    - UGeoWaterStreamingSubsystem, che carica le tile d'acqua dal disco;
//    - UGeoTerrainSubsystem, a cui dice come SCAVARE le quote sotto l'acqua
//      (SetHeightEditor) e quando una tile e' pronta (predicato "Acqua");
//    - IGeoTerrainMeshProvider, che riceve la superficie di ogni tile;
//    - UGeoWeatherSubsystem, da cui legge il vento per le onde.
//
//  IL LAVORO, PER OGNI TILE DI TERRENO CON ACQUA
//    1. il terreno si costruisce scavato (lo fa il terreno, con la funzione
//       che gli diamo noi: Water::CarveHeights);
//    2. sui worker: la stessa tile scavata -> la mesh della superficie
//       (Water::BuildWaterMesh) e i pixel del raster (BuildWaterRasterPixels);
//    3. sul game thread: texture, consegna al provider.
//  Una tile compare solo con la sua acqua pronta (come le strade): niente
//  laghi vuoti per un frame, che sarebbero buche nel terreno.
//
//  LE ONDE si calcolano una volta per tutto il mondo (Water::FWaveField) e
//  vanno al materiale attraverso una Material Parameter Collection
//  (MPC_GeoWater): una scrittura per frame, non una per tile.
// =============================================================================
#pragma once

#include "CoreMinimal.h"
#include "Containers/Queue.h"
#include "Subsystems/WorldSubsystem.h"
#include "Tasks/Task.h"
#include "UObject/StrongObjectPtr.h"

#include "Georeference/GeoreferenceSnapshot.h"
#include "Terrain/GeoTerrainMeshProvider.h"
#include "Tiles/TileKey.h"
#include "Water/WaveSpectrum.h"

#include <cstdint>
#include <vector>

#include "GeoWaterSubsystem.generated.h"

class UGeoTerrainSubsystem;
class UGeoWaterStreamingSubsystem;
class UMaterialParameterCollection;
class UTexture2D;

USTRUCT()
struct FGeoWaterStats
{
	GENERATED_BODY()

	/** Tile con la superficie dell'acqua consegnata. */
	UPROPERTY() int32 TileConAcqua = 0;
	/** Tile che toccano una tile d'acqua ma senza acqua dentro (solo scavo, o niente). */
	UPROPERTY() int32 TileVuote = 0;
	/** Tile che aspettano: la tile d'acqua, le quote, la costruzione. */
	UPROPERTY() int32 TileInAttesa = 0;
	UPROPERTY() int32 CostruzioniInCorso = 0;
	UPROPERTY() int32 ConsegneQuestoFrame = 0;
	UPROPERTY() float MillisecondiPerCostruzione = 0.0f;
	UPROPERTY() int32 Triangoli = 0;
};

UCLASS()
class GEORENDER_API UGeoWaterSubsystem : public UTickableWorldSubsystem
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

	/** Accende e spegne l'acqua (e lo scavo del terreno: il terreno si ricostruisce). */
	void SetEnabled(bool bInEnabled);
	bool IsEnabled() const { return bEnabled; }

	/** Da chiamare dopo aver aperto (o chiuso) un dataset: tutto si rifa' da capo. */
	void OnDatasetChanged();

	/** N del geoide nel punto, se il dataset d'acqua lo porta (per la quota s.l.m.). */
	bool GetGeoidUndulation(double LonDeg, double LatDeg, double& OutUndulation) const;

	void SetDebugOverlayEnabled(bool bInShow) { bShowDebugOverlay = bInShow; }
	bool IsDebugOverlayEnabled() const { return bShowDebugOverlay; }

	FGeoWaterStats GetStats() const { return Stats; }
	const GeoWorld::Water::FWaveField& GetWaveField() const { return Waves; }

	/** Vuota se materiale e collezione dei parametri ci sono. */
	FString GetMaterialProblem() const;

private:
	/** Il lavoro di una tile, dai worker al game thread. */
	struct FMeshResult
	{
		FTileKey Key;
		FTileKey WaterKey;
		int32 Generation = 0;
		int32 Step = 0;
		FGeoPreparedTileMeshPtr Prepared;
		std::vector<uint8_t> Pixels;
		std::vector<std::vector<uint8_t>> Mips;
		float MetresPerPixel = 1.0f;
		float CellMetres = 1.0f;
		bool bEmpty = false;
		double Seconds = 0.0;
	};
	struct FMeshQueue
	{
		TQueue<FMeshResult, EQueueMode::Mpsc> Completed;
	};

	/** Cosa e' stato consegnato per una tile di terreno. */
	struct FWaterState
	{
		FTileKey Key;
		FTileKey WaterKey;
		int32 Step = 0;
		bool bEmpty = false;
	};

	void Synchronise(IGeoTerrainMeshProvider* Provider);
	bool LaunchJob(IGeoTerrainMeshProvider* Provider, const FTileKey& Key, const FTileKey& WaterKey, int32 Step);
	void DrainFinished(IGeoTerrainMeshProvider* Provider, int32 Budget);
	void InstallCarving(bool bInstall);
	void EnsureSharedTextures(IGeoTerrainMeshProvider* Provider);
	void UpdateWaves(float DeltaTime);
	void RemoveAllWater();
	void OnGeoreferenceRebased(const FGeoreferenceSnapshot& Snapshot);
	void DrawDebugOverlay();
	/** Senza overlay: la riga rossa del dataset vecchio, se lo e'. */
	void DrawStaleWarning() const;

	UPROPERTY(Transient) TObjectPtr<UGeoTerrainSubsystem> Terrain;
	UPROPERTY(Transient) TObjectPtr<UGeoWaterStreamingSubsystem> Streaming;
	/** MPC_GeoWater: i parametri delle onde, per tutti i materiali dell'acqua. */
	UPROPERTY(Transient) TObjectPtr<UMaterialParameterCollection> WaveCollection;

	TStrongObjectPtr<UTexture2D> DetailNormal;
	TStrongObjectPtr<UTexture2D> Foam;

	TSharedPtr<FMeshQueue, ESPMode::ThreadSafe> Queue;
	TMap<uint64, UE::Tasks::FTask> InFlight;
	TArray<FMeshResult> PendingCommits;
	TMap<uint64, FWaterState> Delivered;
	/** Le tile che il predicato "Acqua" dichiara pronte (ricalcolate a ogni frame). */
	TSet<uint64> Dressed;
	int32 Generation = 0;

	bool bEnabled = true;
	bool bCarvingInstalled = false;
	bool bShowDebugOverlay = false;
	bool bWavesStarted = false;
	bool bNeedsTerrainRebuild = false;
	double LastCollectionCheck = 0.0;

	// --- Le onde ------------------------------------------------------------------
	GeoWorld::Water::FWaveField Waves;
	/** Scorrimento delle due normali di dettaglio, in unita' di texture (ridotto a [0,1)). */
	double DetailOffset[4] = { 0.0, 0.0, 0.0, 0.0 };
	/** Il tempo della corrente dei fiumi (due fasi sfalsate). */
	double FlowSeconds = 0.0;
	FGeoreferenceSnapshot LastSnapshot;
	bool bHaveSnapshot = false;
	FDelegateHandle RebaseHandle;

	int32 MaxJobsInFlight = 4;
	int32 MaxCommitsPerFrame = 4;
	int32 CompletedJobs = 0;
	double TotalJobSeconds = 0.0;
	FGeoWaterStats Stats;
};

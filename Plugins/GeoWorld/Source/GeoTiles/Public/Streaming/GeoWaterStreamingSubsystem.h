// =============================================================================
//  GeoWaterStreamingSubsystem.h -- Caricamento asincrono delle tile d'acqua
//  (Fase 9). STRATO: UNREAL.
//
//  Stesso schema delle strade: pool di thread, cache LRU con budget, coda di
//  risultati svuotata dal game thread. Il worker legge il file, lo
//  DECOMPRIME (zlib, con FCompression del motore) e lo interpreta
//  (WaterTileFormat.h). Una tile in memoria pesa ~0,4 MB (quote in float,
//  raster in byte).
// =============================================================================
#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"

#include "Streaming/GeoLoaderPool.h"
#include "Streaming/GeoTileStreamingSubsystem.h"   // per EGeoTileState
#include "Streaming/GeoWaterDataset.h"
#include "Tiles/TileCache.h"
#include "Tiles/WaterTileFormat.h"

#include "GeoWaterStreamingSubsystem.generated.h"

USTRUCT()
struct FGeoWaterStreamingStats
{
	GENERATED_BODY()

	UPROPERTY() int32 TileResidenti = 0;
	UPROPERTY() int32 TileInVolo = 0;
	UPROPERTY() float MemoriaMB = 0.0f;
	UPROPERTY() float BudgetMB = 0.0f;
	UPROPERTY() int32 CaricamentiTotali = 0;
	UPROPERTY() int32 ErroriDiCaricamento = 0;
	UPROPERTY() float TempoMedioCaricamentoMs = 0.0f;
};

class FGeoWaterLoadWork;

UCLASS()
class GEOTILES_API UGeoWaterStreamingSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	using FTileKey = GeoWorld::Tiles::FTileKey;
	using FWaterTilePtr = GeoWorld::Tiles::TTileCache<GeoWorld::Tiles::FWaterTile>::FTilePtr;

	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual bool IsTickableInEditor() const override { return true; }

	bool OpenDataset(const FString& RootDirectory, FString& OutError);
	void CloseDataset();
	bool IsDatasetOpen() const { return Dataset.IsOpen(); }
	const FGeoWaterDataset& GetDataset() const { return Dataset; }

	/** Gia' in cache, oppure nullptr. Non avvia nessun caricamento. */
	FWaterTilePtr FindLoadedTile(const FTileKey& Key);

	/** Chiede una tile. Ritorna subito. Priorita': piu' basso = piu' urgente. */
	EGeoTileState RequestTile(const FTileKey& Key, int32 Priority = 0);

	void CancelPendingRequests();
	void ClearCache();

	FGeoWaterStreamingStats GetStats() const;

private:
	friend class FGeoWaterLoadWork;

	struct FLoadResult
	{
		FTileKey Key;
		TSharedPtr<GeoWorld::Tiles::FWaterTile> Tile;
		FString Error;
		double Seconds = 0.0;
		bool bCancelled = false;
	};

	TQueue<FLoadResult, EQueueMode::Mpsc> CompletedLoads;

	FGeoWaterDataset Dataset;
	GeoWorld::Tiles::TTileCache<GeoWorld::Tiles::FWaterTile> Cache;
	FGeoLoaderPool Pool;
	TSet<uint64> InFlight;
	TSet<uint64> Failed;

	int32 LoadThreadCount = 2;
	int32 CompletedCount = 0;
	int32 ErrorCount = 0;
	double TotalSeconds = 0.0;
};

// =============================================================================
//  GeoVegetationStreamingSubsystem.h -- Caricamento asincrono delle tile di
//  vegetazione. STRATO: UNREAL.
//
//  Lo schema dell'acqua: pool di thread, cache LRU con budget, coda dei
//  risultati svuotata dal game thread. Il worker legge il file, lo
//  DECOMPRIME (zlib, FCompression) e lo interpreta (VegetationTileFormat.h).
//  Una tile in memoria pesa 64 KB (un byte per pixel): ne servono una
//  decina attorno alla camera.
// =============================================================================
#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"

#include "Streaming/GeoLoaderPool.h"
#include "Streaming/GeoTileStreamingSubsystem.h"   // per EGeoTileState
#include "Streaming/GeoVegetationDataset.h"
#include "Tiles/TileCache.h"
#include "Tiles/VegetationTileFormat.h"

#include "GeoVegetationStreamingSubsystem.generated.h"

USTRUCT()
struct FGeoVegetationStreamingStats
{
	GENERATED_BODY()

	UPROPERTY() int32 TileResidenti = 0;
	UPROPERTY() int32 TileInVolo = 0;
	UPROPERTY() float MemoriaMB = 0.0f;
	UPROPERTY() int32 CaricamentiTotali = 0;
	UPROPERTY() int32 ErroriDiCaricamento = 0;
	UPROPERTY() float TempoMedioCaricamentoMs = 0.0f;
};

class FGeoVegetationLoadWork;

UCLASS()
class GEOTILES_API UGeoVegetationStreamingSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	using FTileKey = GeoWorld::Tiles::FTileKey;
	using FVegetationTilePtr = GeoWorld::Tiles::TTileCache<GeoWorld::Tiles::FVegetationTile>::FTilePtr;

	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual bool IsTickableInEditor() const override { return true; }

	bool OpenDataset(const FString& RootDirectory, FString& OutError);
	void CloseDataset();
	bool IsDatasetOpen() const { return Dataset.IsOpen(); }
	const FGeoVegetationDataset& GetDataset() const { return Dataset; }

	/** Gia' in cache, oppure nullptr. Non avvia nessun caricamento e non tocca l'ordine della cache. */
	FVegetationTilePtr PeekLoadedTile(const FTileKey& Key) const { return Cache.Peek(Key); }
	/** Come Peek, ma la tile diventa la piu' recente (resta in cache piu' a lungo). */
	FVegetationTilePtr FindLoadedTile(const FTileKey& Key) { return Cache.Find(Key); }

	/**
	 * Chiede una tile. Ritorna subito: Pronta, InCaricamento, Assente (non
	 * c'e': li' non cresce niente) o Errore. Priorita': piu' basso = piu' urgente.
	 */
	EGeoTileState RequestTile(const FTileKey& Key, int32 Priority = 0);

	void CancelPendingRequests();
	void ClearCache();

	FGeoVegetationStreamingStats GetStats() const;

private:
	friend class FGeoVegetationLoadWork;

	struct FLoadResult
	{
		FTileKey Key;
		TSharedPtr<GeoWorld::Tiles::FVegetationTile> Tile;
		FString Error;
		double Seconds = 0.0;
		bool bCancelled = false;
	};

	TQueue<FLoadResult, EQueueMode::Mpsc> CompletedLoads;

	FGeoVegetationDataset Dataset;
	GeoWorld::Tiles::TTileCache<GeoWorld::Tiles::FVegetationTile> Cache;
	FGeoLoaderPool Pool;
	TSet<uint64> InFlight;
	TSet<uint64> Failed;

	int32 LoadThreadCount = 1;
	int32 CompletedCount = 0;
	int32 ErrorCount = 0;
	double TotalSeconds = 0.0;
};

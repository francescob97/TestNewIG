// =============================================================================
//  GeoBuildingStreamingSubsystem.h -- Caricamento asincrono delle tile di
//  edifici. STRATO: UNREAL.
//
//  Lo schema della vegetazione: pool di thread, cache LRU con budget, coda
//  dei risultati svuotata dal game thread. Il worker legge il file, lo
//  DECOMPRIME (zlib, FCompression) e lo interpreta (BuildingTileFormat.h).
//  Una tile di citta' in memoria pesa qualche centinaio di KB (i vertici in
//  double); ne servono qualche centinaio attorno alla camera.
// =============================================================================
#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"

#include "Streaming/GeoLoaderPool.h"
#include "Streaming/GeoTileStreamingSubsystem.h"   // per EGeoTileState
#include "Streaming/GeoBuildingDataset.h"
#include "Tiles/TileCache.h"
#include "Tiles/BuildingTileFormat.h"

#include "GeoBuildingStreamingSubsystem.generated.h"

USTRUCT()
struct FGeoBuildingStreamingStats
{
	GENERATED_BODY()

	UPROPERTY() int32 TileResidenti = 0;
	UPROPERTY() int32 TileInVolo = 0;
	UPROPERTY() float MemoriaMB = 0.0f;
	UPROPERTY() int32 CaricamentiTotali = 0;
	UPROPERTY() int32 ErroriDiCaricamento = 0;
	UPROPERTY() float TempoMedioCaricamentoMs = 0.0f;
};

class FGeoBuildingLoadWork;

UCLASS()
class GEOTILES_API UGeoBuildingStreamingSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	using FTileKey = GeoWorld::Tiles::FTileKey;
	using FBuildingTilePtr = GeoWorld::Tiles::TTileCache<GeoWorld::Tiles::FBuildingTile>::FTilePtr;

	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual bool IsTickableInEditor() const override { return true; }

	bool OpenDataset(const FString& RootDirectory, FString& OutError);
	void CloseDataset();
	bool IsDatasetOpen() const { return Dataset.IsOpen(); }
	const FGeoBuildingDataset& GetDataset() const { return Dataset; }

	/** Gia' in cache, oppure nullptr. Non avvia nessun caricamento e non tocca l'ordine della cache. */
	FBuildingTilePtr PeekLoadedTile(const FTileKey& Key) const { return Cache.Peek(Key); }
	/** Come Peek, ma la tile diventa la piu' recente (resta in cache piu' a lungo). */
	FBuildingTilePtr FindLoadedTile(const FTileKey& Key) { return Cache.Find(Key); }

	/**
	 * Chiede una tile. Ritorna subito: Pronta, InCaricamento, Assente (non
	 * c'e': li' nessun edificio) o Errore. Priorita': piu' basso = piu' urgente.
	 */
	EGeoTileState RequestTile(const FTileKey& Key, int32 Priority = 0);

	void CancelPendingRequests();
	void ClearCache();

	FGeoBuildingStreamingStats GetStats() const;

private:
	friend class FGeoBuildingLoadWork;

	struct FLoadResult
	{
		FTileKey Key;
		TSharedPtr<GeoWorld::Tiles::FBuildingTile> Tile;
		FString Error;
		double Seconds = 0.0;
		bool bCancelled = false;
	};

	TQueue<FLoadResult, EQueueMode::Mpsc> CompletedLoads;

	FGeoBuildingDataset Dataset;
	GeoWorld::Tiles::TTileCache<GeoWorld::Tiles::FBuildingTile> Cache;
	FGeoLoaderPool Pool;
	TSet<uint64> InFlight;
	TSet<uint64> Failed;

	int32 LoadThreadCount = 2;
	int32 CompletedCount = 0;
	int32 ErrorCount = 0;
	double TotalSeconds = 0.0;
};

// =============================================================================
//  GeoImageryStreamingSubsystem.h -- Caricamento asincrono delle ortofoto.
//  STRATO: UNREAL.
//
//  E' corto perche' tutto cio' che aveva senso condividere e' gia' altrove: il
//  pool di thread in FGeoLoaderPool, la cache in TTileCache, il formato in
//  ImageTileFormat.h. Qui resta solo cio' che e' davvero specifico delle
//  immagini: la decodifica del JPEG e il dataset da cui leggere.
// =============================================================================
#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"

#include "Streaming/GeoImageryDataset.h"
#include "Streaming/GeoLoaderPool.h"
#include "Streaming/GeoTileStreamingSubsystem.h"   // per EGeoTileState
#include "Tiles/ImageTileFormat.h"
#include "Tiles/TileCache.h"

#include "GeoImageryStreamingSubsystem.generated.h"

USTRUCT()
struct FGeoImageryStreamingStats
{
	GENERATED_BODY()

	UPROPERTY() int32 TileResidenti = 0;
	UPROPERTY() int32 TileInVolo = 0;
	UPROPERTY() int32 TilePinnate = 0;
	UPROPERTY() float MemoriaMB = 0.0f;
	UPROPERTY() float BudgetMB = 0.0f;
	UPROPERTY() float HitRate = 0.0f;
	UPROPERTY() int32 CaricamentiTotali = 0;
	UPROPERTY() int32 ErroriDiCaricamento = 0;
	UPROPERTY() float TempoMedioCaricamentoMs = 0.0f;
	UPROPERTY() float TempoMedioDecodificaMs = 0.0f;
};

class FGeoImageryLoadWork;

UCLASS()
class GEOTILES_API UGeoImageryStreamingSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	using FTileKey = GeoWorld::Tiles::FTileKey;
	using FImageTilePtr = GeoWorld::Tiles::TTileCache<GeoWorld::Tiles::FImageTile>::FTilePtr;

	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual bool IsTickableInEditor() const override { return true; }

	bool OpenDataset(const FString& RootDirectory, FString& OutError);
	bool IsDatasetOpen() const { return Dataset.IsOpen(); }
	const FGeoImageryDataset& GetDataset() const { return Dataset; }

	/** Gia' in cache, oppure nullptr. Non avvia nessun caricamento. */
	FImageTilePtr FindLoadedTile(const FTileKey& Key);

	/** Chiede una tile. Ritorna subito. Priorita': piu' basso = piu' urgente. */
	EGeoTileState RequestTile(const FTileKey& Key, int32 Priority = 0);

	EGeoTileState GetTileState(const FTileKey& Key) const;
	void CancelPendingRequests();

	void SetTilePinned(const FTileKey& Key, bool bPinned);
	void SetCacheBudgetMB(int32 Megabytes);
	int32 GetCacheBudgetMB() const;
	void ClearCache();

	FGeoImageryStreamingStats GetStats() const;

	DECLARE_MULTICAST_DELEGATE_OneParam(FOnImageTileLoaded, const FTileKey&);
	FOnImageTileLoaded OnImageTileLoaded;

private:
	friend class FGeoImageryLoadWork;

	struct FLoadResult
	{
		FTileKey Key;
		TSharedPtr<GeoWorld::Tiles::FImageTile> Tile;
		FString Error;
		double ReadSeconds = 0.0;
		double DecodeSeconds = 0.0;
	};

	/** I worker consegnano qui. Mpsc: molti produttori, un consumatore. */
	TQueue<FLoadResult, EQueueMode::Mpsc> CompletedLoads;

	FGeoImageryDataset Dataset;
	GeoWorld::Tiles::TTileCache<GeoWorld::Tiles::FImageTile> Cache;
	FGeoLoaderPool Pool;

	TSet<uint64> InFlight;

	int32 LoadThreadCount = 3;
	int32 CompletedCount = 0;
	int32 ErrorCount = 0;
	double TotalReadSeconds = 0.0;
	double TotalDecodeSeconds = 0.0;
};

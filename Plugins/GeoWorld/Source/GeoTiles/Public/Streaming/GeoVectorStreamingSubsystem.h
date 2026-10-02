// =============================================================================
//  GeoVectorStreamingSubsystem.h -- Caricamento asincrono delle tile di linee
//  (Fase 8). STRATO: UNREAL.
//
//  Stesso schema delle quote e delle ortofoto: pool di thread, cache LRU con
//  budget, coda di risultati svuotata dal game thread. Qui il worker legge il
//  file e lo interpreta (ParseVectorTile): non c'e' niente da decomprimere.
//
//  Le tile di linee sono poche e piccole (qualche decina di KB, al massimo
//  qualche centinaio nel centro di una citta'), e ognuna serve a molte tile di
//  terreno: una del livello 13 basta per tutte le tile di terreno dal 13 in giu'
//  che ci stanno dentro. Per questo la cache e' piccola e le tile ci restano.
// =============================================================================
#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"

#include "Streaming/GeoLoaderPool.h"
#include "Streaming/GeoTileStreamingSubsystem.h"   // per EGeoTileState
#include "Streaming/GeoVectorDataset.h"
#include "Tiles/TileCache.h"
#include "Tiles/VectorTileFormat.h"

#include "GeoVectorStreamingSubsystem.generated.h"

USTRUCT()
struct FGeoVectorStreamingStats
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

class FGeoVectorLoadWork;

UCLASS()
class GEOTILES_API UGeoVectorStreamingSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	using FTileKey = GeoWorld::Tiles::FTileKey;
	using FVectorTilePtr = GeoWorld::Tiles::TTileCache<GeoWorld::Tiles::FVectorTile>::FTilePtr;

	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual bool IsTickableInEditor() const override { return true; }

	bool OpenDataset(const FString& RootDirectory, FString& OutError);
	bool IsDatasetOpen() const { return Dataset.IsOpen(); }
	const FGeoVectorDataset& GetDataset() const { return Dataset; }

	/** Gia' in cache, oppure nullptr. Non avvia nessun caricamento. */
	FVectorTilePtr FindLoadedTile(const FTileKey& Key);

	/** Chiede una tile. Ritorna subito. Priorita': piu' basso = piu' urgente. */
	EGeoTileState RequestTile(const FTileKey& Key, int32 Priority = 0);

	void CancelPendingRequests();
	void ClearCache();

	FGeoVectorStreamingStats GetStats() const;

	DECLARE_MULTICAST_DELEGATE_OneParam(FOnVectorTileLoaded, const FTileKey&);
	FOnVectorTileLoaded OnVectorTileLoaded;

private:
	friend class FGeoVectorLoadWork;

	struct FLoadResult
	{
		FTileKey Key;
		TSharedPtr<GeoWorld::Tiles::FVectorTile> Tile;
		FString Error;
		double Seconds = 0.0;
		/** Annullato (teletrasporto, chiusura): non e' un errore del file. */
		bool bCancelled = false;
	};

	TQueue<FLoadResult, EQueueMode::Mpsc> CompletedLoads;

	FGeoVectorDataset Dataset;
	GeoWorld::Tiles::TTileCache<GeoWorld::Tiles::FVectorTile> Cache;
	FGeoLoaderPool Pool;
	TSet<uint64> InFlight;

	/** Tile che non si sono potute leggere: non si richiedono piu' (RequestTile -> Errore). */
	TSet<uint64> Failed;

	int32 LoadThreadCount = 2;
	int32 CompletedCount = 0;
	int32 ErrorCount = 0;
	double TotalSeconds = 0.0;
};

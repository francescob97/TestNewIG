// =============================================================================
//  GeoTileStreamingSubsystem.h -- Caricamento asincrono e cache. STRATO: UNREAL.
// =============================================================================
#pragma once

#include "CoreMinimal.h"
#include "Containers/Queue.h"
#include "Subsystems/WorldSubsystem.h"

#include "Tiles/TileCache.h"
#include "Tiles/TileKey.h"
#include "Unreal/GeoTileDataset.h"

#include "GeoTileStreamingSubsystem.generated.h"

class FGeoTileLoadWork;

/** Stato di una tile dal punto di vista di chi la chiede. */
UENUM()
enum class EGeoTileState : uint8
{
	Assente,      // non esiste nel dataset
	NonCaricata,  // esiste ma nessuno l'ha chiesta
	InCaricamento,
	Pronta,
	Errore
};

USTRUCT()
struct FGeoTileStreamingStats
{
	GENERATED_BODY()

	UPROPERTY() int32 TileCaricate = 0;
	UPROPERTY() int32 TileVisibili = 0;
	UPROPERTY() int32 RichiesteInCorso = 0;
	UPROPERTY() int32 RichiesteInCoda = 0;
	UPROPERTY() int32 Errori = 0;
	UPROPERTY() float MemoriaCacheMB = 0.0f;
	UPROPERTY() float BudgetCacheMB = 0.0f;
	UPROPERTY() float TassoHit = 0.0f;
	UPROPERTY() float TempoMedioCaricamentoMs = 0.0f;
	UPROPERTY() int32 Evizioni = 0;
};

/**
 * =============================================================================
 *  REGOLA NON NEGOZIABILE: NESSUN I/O SUL GAME THREAD
 * =============================================================================
 *  Una lettura da disco costa fra frazioni di millisecondo (cache del sistema
 *  operativo) e decine di millisecondi (disco meccanico, antivirus, rete). Un
 *  frame a 60 fps dura 16.6 ms: una sola lettura sfortunata lo fa saltare. Con
 *  centinaia di tile da caricare mentre la camera si muove, farlo sul game
 *  thread significa un'esperienza inutilizzabile.
 *
 *  Percio' il ciclo e':
 *    game thread   -> RequestTile() mette in coda e ritorna subito
 *    worker thread -> legge il file e lo decodifica
 *    worker thread -> deposita il risultato in una coda MPSC
 *    game thread   -> Tick() svuota la coda e riempie la cache
 *
 *  La cache e' toccata SOLO dal game thread, quindi non serve alcun lock sul
 *  percorso caldo. I worker non vedono ne' la cache ne' UObject.
 *
 * =============================================================================
 *  PERCHE' UN THREAD POOL NOSTRO E NON IL TASK GRAPH DI UNREAL
 * =============================================================================
 *  Il task graph e' pensato per lavoro che CALCOLA, dimensionato sui core. Le
 *  nostre attivita' invece si BLOCCANO su I/O: un thread che aspetta il disco
 *  non consuma CPU ma occupa uno slot. Mescolarle al task graph significa
 *  togliere worker a chi deve calcolare (animazione, fisica, particelle) per
 *  tenerli fermi in attesa del disco.
 *
 *  Con un pool dedicato il numero di thread si dimensiona sul PARALLELISMO DEL
 *  DISCO e non sui core, e una raffica di caricamenti non puo' affamare il
 *  resto del motore.
 */
UCLASS()
class GEOTILES_API UGeoTileStreamingSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	using FTileKey = GeoWorld::Tiles::FTileKey;
	using FTilePtr = GeoWorld::Tiles::FTileCache::FTilePtr;

	// --- UTickableWorldSubsystem ------------------------------------------
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual bool IsTickableInEditor() const override { return true; }

	// --- Dataset -----------------------------------------------------------
	bool OpenDataset(const FString& RootDirectory, FString& OutError);
	bool IsDatasetOpen() const { return Dataset.IsOpen(); }
	const FGeoTileDataset& GetDataset() const { return Dataset; }

	// --- Richiesta di tile -------------------------------------------------

	/**
	 * Tile gia' in cache, oppure nullptr. Non avvia nessun caricamento.
	 * E' l'accesso che il quadtree usa nel percorso caldo.
	 */
	FTilePtr FindLoadedTile(const FTileKey& Key);

	/**
	 * Chiede una tile. Ritorna subito.
	 *
	 * Priority: piu' ALTO viene servito prima. Il quadtree ci mettera' un
	 * valore derivato dall'errore su schermo, cosi' le tile che l'utente sta
	 * guardando arrivano prima di quelle ai bordi della vista.
	 */
	EGeoTileState RequestTile(const FTileKey& Key, int32 Priority = 0);

	EGeoTileState GetTileState(const FTileKey& Key) const;

	/** Annulla le richieste non ancora iniziate che non servono piu'. */
	void CancelPendingRequests();

	// --- Cache -------------------------------------------------------------
	void SetCacheBudgetMB(int32 Megabytes);
	int32 GetCacheBudgetMB() const;
	void ClearCache();
	void SetTilePinned(const FTileKey& Key, bool bPinned);

	FGeoTileStreamingStats GetStats() const;
	void ResetStats();

	// --- Visualizzazione di debug ------------------------------------------
	//
	// Senza il quadtree (Fase 4) nessuno chiede tile: la Fase 3 da sola non
	// produrrebbe niente da guardare. Questi due strumenti servono proprio a
	// renderla osservabile prima che esista chi la usa davvero.

	void SetDebugOverlayEnabled(bool bEnabled) { bShowDebugOverlay = bEnabled; }
	bool IsDebugOverlayEnabled() const { return bShowDebugOverlay; }

	void SetDebugDrawTiles(bool bEnabled) { bDrawTileBounds = bEnabled; }
	bool IsDebugDrawTilesEnabled() const { return bDrawTileBounds; }

	/**
	 * Chiede tutte le tile in un quadrato di (2*Radius+1) tile attorno al punto
	 * geografico dato. E' il sostituto provvisorio del quadtree: serve a vedere
	 * lo streaming al lavoro e a mettere sotto pressione la cache.
	 * Ritorna il numero di richieste avviate.
	 */
	int32 RequestTilesAround(double Latitude, double Longitude, int32 Level, int32 Radius);

	/** Segnalato sul GAME THREAD quando una tile diventa disponibile. */
	DECLARE_MULTICAST_DELEGATE_TwoParams(FOnTileLoaded, const FTileKey&, bool /*bSuccess*/);
	FOnTileLoaded OnTileLoaded;

private:
	friend class FGeoTileLoadWork;

	/** Risultato prodotto da un worker e consumato dal game thread. */
	struct FLoadResult
	{
		FTileKey Key;
		TSharedPtr<GeoWorld::Tiles::FHeightTile> Tile;
		FString Error;
		double DurationSeconds = 0.0;
	};

	void DrainCompletedLoads();

	FGeoTileDataset Dataset;
	GeoWorld::Tiles::FTileCache Cache{ 256ull * 1024 * 1024 };

	/**
	 * Coda multi-produttore / singolo-consumatore: molti worker scrivono, solo
	 * il game thread legge. E' senza lock, ed e' il motivo per cui consegnare i
	 * risultati non costa niente al frame.
	 */
	TQueue<FLoadResult, EQueueMode::Mpsc> CompletedLoads;

	/** Tile per cui esiste una richiesta in volo: evita di chiederle due volte. */
	TSet<uint64> InFlight;

	FQueuedThreadPool* LoadPool = nullptr;
	TArray<FGeoTileLoadWork*> PendingWork;
	mutable FCriticalSection PendingWorkLock;

	void DrawDebugOverlay();
	void DrawTileBounds() const;

	bool bShowDebugOverlay = false;
	bool bDrawTileBounds = false;

	// Statistiche
	int32 ErrorCount = 0;
	int32 CompletedCount = 0;
	double TotalLoadSeconds = 0.0;
};

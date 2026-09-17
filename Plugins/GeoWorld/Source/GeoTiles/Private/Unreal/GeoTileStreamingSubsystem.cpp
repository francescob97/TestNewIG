#include "Unreal/GeoTileStreamingSubsystem.h"

#include "GeoCoreModule.h"

#include "Engine/World.h"
#include "HAL/RunnableThread.h"
#include "Misc/FileHelper.h"
#include "Misc/QueuedThreadPool.h"
#include "Misc/ScopeLock.h"

using namespace GeoWorld::Tiles;

namespace
{
	FORCEINLINE uint64 PackKey(const FTileKey& Key)
	{
		return (static_cast<uint64>(Key.Level) << 58) ^ (static_cast<uint64>(Key.Y) << 29)
		     ^ static_cast<uint64>(Key.X);
	}

	/** Thread del pool. Non serve uno per core: il collo di bottiglia e' il disco. */
	constexpr int32 LoadThreadCount = 4;
}

/**
 * Unita' di lavoro eseguita su un thread del pool.
 *
 * NOTA UE: IQueuedWork ha DUE punti d'uscita e vanno gestiti entrambi.
 *   DoThreadedWork() -- il lavoro e' stato eseguito;
 *   Abandon()        -- il pool sta chiudendo, o il lavoro e' stato annullato
 *                       PRIMA di partire.
 * Dimenticare Abandon() e' la causa classica delle perdite di memoria alla
 * chiusura del gioco: gli oggetti in coda non vengono mai distrutti.
 */
class FGeoTileLoadWork : public IQueuedWork
{
public:
	FGeoTileLoadWork(UGeoTileStreamingSubsystem* InOwner, const FTileKey& InKey, const FString& InPath)
		: Owner(InOwner), Key(InKey), Path(InPath) {}

	virtual void DoThreadedWork() override
	{
		const double Started = FPlatformTime::Seconds();

		UGeoTileStreamingSubsystem::FLoadResult Result;
		Result.Key = Key;

		TArray<uint8> Blob;
		if (!FFileHelper::LoadFileToArray(Blob, *Path))
		{
			Result.Error = FString::Printf(TEXT("file non leggibile: %s"), *Path);
		}
		else
		{
			auto Tile = MakeShared<FHeightTile>();
			std::string DecodeError;
			if (DecodeTile(Blob.GetData(), static_cast<size_t>(Blob.Num()), Key, *Tile, DecodeError))
			{
				Result.Tile = Tile;
			}
			else
			{
				Result.Error = FString::Printf(TEXT("%hs"), DecodeError.c_str());
			}
		}

		Result.DurationSeconds = FPlatformTime::Seconds() - Started;

		// Unico punto di contatto con il game thread: una coda senza lock.
		// Da qui NON si tocca la cache, non si chiama nessun delegate e non si
		// guarda nessun UObject: sono tutti di competenza esclusiva del game
		// thread e il garbage collector puo' muoverli in qualunque momento.
		Owner->CompletedLoads.Enqueue(MoveTemp(Result));
		delete this;
	}

	virtual void Abandon() override
	{
		UGeoTileStreamingSubsystem::FLoadResult Result;
		Result.Key = Key;
		Result.Error = TEXT("annullata");
		Owner->CompletedLoads.Enqueue(MoveTemp(Result));
		delete this;
	}

private:
	UGeoTileStreamingSubsystem* Owner = nullptr;
	FTileKey Key;
	FString Path;
};

// ============================================================================
//  Ciclo di vita
// ============================================================================

bool UGeoTileStreamingSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	if (!Super::ShouldCreateSubsystem(Outer)) { return false; }
	const UWorld* World = Cast<UWorld>(Outer);
	return World && (World->WorldType == EWorldType::Game
	              || World->WorldType == EWorldType::PIE
	              || World->WorldType == EWorldType::Editor);
}

void UGeoTileStreamingSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	LoadPool = FQueuedThreadPool::Allocate();
	// TPri_BelowNormal: il caricamento delle tile non deve mai contendere la CPU
	// con il game thread. Meglio una tile che arriva un frame dopo che un frame
	// che salta.
	LoadPool->Create(LoadThreadCount, 128 * 1024, TPri_BelowNormal, TEXT("GeoTileLoadPool"));

	UE_LOG(LogGeoWorld, Log, TEXT("[GeoTiles] Streaming avviato: %d thread, cache %d MB"),
		LoadThreadCount, GetCacheBudgetMB());
}

void UGeoTileStreamingSubsystem::Deinitialize()
{
	if (LoadPool)
	{
		// Destroy() chiama Abandon() su tutto cio' che e' ancora in coda e
		// aspetta i lavori gia' partiti. Senza, si distruggerebbe il subsystem
		// mentre dei thread ne stanno ancora usando la coda.
		LoadPool->Destroy();
		delete LoadPool;
		LoadPool = nullptr;
	}

	CompletedLoads.Empty();
	InFlight.Empty();
	Cache.Clear();
	OnTileLoaded.Clear();

	Super::Deinitialize();
}

TStatId UGeoTileStreamingSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UGeoTileStreamingSubsystem, STATGROUP_Tickables);
}

void UGeoTileStreamingSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	DrainCompletedLoads();
}

void UGeoTileStreamingSubsystem::DrainCompletedLoads()
{
	// Si svuota tutta la coda: i risultati sono gia' pronti, rimandarli
	// aggiungerebbe solo latenza. Il costo per elemento e' un inserimento in
	// cache, cioe' nulla rispetto alla lettura che li ha prodotti.
	FLoadResult Result;
	while (CompletedLoads.Dequeue(Result))
	{
		InFlight.Remove(PackKey(Result.Key));

		if (Result.Tile.IsValid())
		{
			// shared_ptr const: la cache consegna tile immutabili, cosi' piu'
			// consumatori possono tenerle senza rischio di modifiche incrociate.
			Cache.Insert(Result.Key, std::shared_ptr<const FHeightTile>(
				new FHeightTile(*Result.Tile)));

			++CompletedCount;
			TotalLoadSeconds += Result.DurationSeconds;
			OnTileLoaded.Broadcast(Result.Key, true);
		}
		else
		{
			if (Result.Error != TEXT("annullata"))
			{
				++ErrorCount;
				UE_LOG(LogGeoWorld, Warning, TEXT("[GeoTiles] %u/%u/%u: %s"),
					Result.Key.Level, Result.Key.X, Result.Key.Y, *Result.Error);
			}
			OnTileLoaded.Broadcast(Result.Key, false);
		}
	}
}

// ============================================================================
//  Dataset e richieste
// ============================================================================

bool UGeoTileStreamingSubsystem::OpenDataset(const FString& RootDirectory, FString& OutError)
{
	ClearCache();
	if (!Dataset.Open(RootDirectory, OutError)) { return false; }

	// Gli indici dei livelli grossolani servono comunque e costano pochissimo:
	// caricarli subito evita un accesso al disco al primo frame utile.
	for (int32 Level = Dataset.GetMinLevel();
	     Level <= FMath::Min(Dataset.GetMinLevel() + 3, Dataset.GetMaxLevel()); ++Level)
	{
		FString IndexError;
		if (!Dataset.EnsureLevelIndex(Level, IndexError))
		{
			UE_LOG(LogGeoWorld, Warning, TEXT("[GeoTiles] %s"), *IndexError);
		}
	}
	return true;
}

UGeoTileStreamingSubsystem::FTilePtr UGeoTileStreamingSubsystem::FindLoadedTile(const FTileKey& Key)
{
	return Cache.Find(Key);
}

EGeoTileState UGeoTileStreamingSubsystem::GetTileState(const FTileKey& Key) const
{
	if (Cache.Peek(Key)) { return EGeoTileState::Pronta; }
	if (InFlight.Contains(PackKey(Key))) { return EGeoTileState::InCaricamento; }
	if (!Dataset.TileExists(Key)) { return EGeoTileState::Assente; }
	return EGeoTileState::NonCaricata;
}

EGeoTileState UGeoTileStreamingSubsystem::RequestTile(const FTileKey& Key, int32 Priority)
{
	if (Cache.Peek(Key)) { return EGeoTileState::Pronta; }

	const uint64 Packed = PackKey(Key);
	if (InFlight.Contains(Packed)) { return EGeoTileState::InCaricamento; }

	// L'indice del livello deve esserci: senza, non si puo' sapere se la tile
	// esiste e si finirebbe per tentare letture di file inesistenti.
	FString IndexError;
	if (!Dataset.EnsureLevelIndex(static_cast<int32>(Key.Level), IndexError))
	{
		return EGeoTileState::Errore;
	}
	if (!Dataset.TileExists(Key)) { return EGeoTileState::Assente; }

	if (!LoadPool) { return EGeoTileState::Errore; }

	InFlight.Add(Packed);

	// NOTA UE: AddQueuedWork prende la PROPRIETA' dell'oggetto. Si distrugge da
	// solo in DoThreadedWork o in Abandon: non va cancellato da qui.
	// La priorita' alta = servito prima, quindi si passa il negato perche' il
	// pool ordina per valore crescente.
	LoadPool->AddQueuedWork(new FGeoTileLoadWork(this, Key, Dataset.GetTileFilePath(Key)),
		static_cast<EQueuedWorkPriority>(FMath::Clamp(3 - Priority, 0, 6)));

	return EGeoTileState::InCaricamento;
}

void UGeoTileStreamingSubsystem::CancelPendingRequests()
{
	// I lavori gia' partiti non si interrompono: durano pochi millisecondi e
	// fermarli a meta' costerebbe piu' di quanto si risparmi. Quelli ancora in
	// coda invece si abbandonano.
	if (LoadPool)
	{
		LoadPool->Destroy();
		delete LoadPool;
		LoadPool = FQueuedThreadPool::Allocate();
		LoadPool->Create(LoadThreadCount, 128 * 1024, TPri_BelowNormal, TEXT("GeoTileLoadPool"));
	}
	DrainCompletedLoads();
	InFlight.Empty();
}

// ============================================================================
//  Cache e statistiche
// ============================================================================

void UGeoTileStreamingSubsystem::SetCacheBudgetMB(int32 Megabytes)
{
	Cache.SetBudgetBytes(static_cast<size_t>(FMath::Max(1, Megabytes)) * 1024ull * 1024ull);
}

int32 UGeoTileStreamingSubsystem::GetCacheBudgetMB() const
{
	return static_cast<int32>(Cache.GetStats().BudgetBytes / (1024ull * 1024ull));
}

void UGeoTileStreamingSubsystem::ClearCache() { Cache.Clear(); }

void UGeoTileStreamingSubsystem::SetTilePinned(const FTileKey& Key, bool bPinned)
{
	Cache.SetPinned(Key, bPinned);
}

FGeoTileStreamingStats UGeoTileStreamingSubsystem::GetStats() const
{
	const FCacheStats CacheStats = Cache.GetStats();

	FGeoTileStreamingStats Stats;
	Stats.TileCaricate = static_cast<int32>(CacheStats.ResidentTiles);
	Stats.TileVisibili = static_cast<int32>(CacheStats.PinnedTiles);
	Stats.RichiesteInCorso = InFlight.Num();
	Stats.Errori = ErrorCount;
	Stats.MemoriaCacheMB = static_cast<float>(CacheStats.GetUsedMegabytes());
	Stats.BudgetCacheMB = static_cast<float>(CacheStats.BudgetBytes) / (1024.0f * 1024.0f);
	Stats.TassoHit = static_cast<float>(CacheStats.GetHitRate());
	Stats.Evizioni = static_cast<int32>(CacheStats.Evictions);
	Stats.TempoMedioCaricamentoMs = (CompletedCount > 0)
		? static_cast<float>(TotalLoadSeconds / CompletedCount * 1000.0) : 0.0f;
	return Stats;
}

void UGeoTileStreamingSubsystem::ResetStats()
{
	ErrorCount = 0;
	CompletedCount = 0;
	TotalLoadSeconds = 0.0;
	Cache.ResetStatistics();
}

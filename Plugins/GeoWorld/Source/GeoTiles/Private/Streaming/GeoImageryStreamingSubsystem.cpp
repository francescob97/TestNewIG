#include "Streaming/GeoImageryStreamingSubsystem.h"

#include "GeoCoreModule.h"

#include "Engine/World.h"
#include "HAL/PlatformTime.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Misc/FileHelper.h"
#include "Modules/ModuleManager.h"

/**
 * Il lavoro che gira sul thread di caricamento.
 *
 * PERCHE' LA DECODIFICA STA QUI E NON SUL GAME THREAD. Un JPEG 256x256 costa
 * uno o due millisecondi. Sembrano pochi finche' non si moltiplicano per le
 * decine di tile che arrivano quando ci si sposta in fretta: diventerebbero
 * frame saltati. Qui invece il game thread riceve pixel gia' pronti e deve solo
 * copiarli in una texture.
 *
 * NOTA UE: IImageWrapper si usa da un thread qualunque, purche' ogni thread
 * abbia la PROPRIA istanza. Il modulo va pero' caricato sul game thread, ed e'
 * per questo che Initialize() lo fa esplicitamente invece di lasciarlo al primo
 * worker che passa.
 */
class FGeoImageryLoadWork : public IQueuedWork
{
public:
	FGeoImageryLoadWork(UGeoImageryStreamingSubsystem* InOwner,
	                    const GeoWorld::Tiles::FTileKey& InKey, const FString& InPath)
		: Owner(InOwner), Key(InKey), Path(InPath) {}

	virtual void DoThreadedWork() override
	{
		UGeoImageryStreamingSubsystem::FLoadResult Result;
		Result.Key = Key;

		const double ReadStarted = FPlatformTime::Seconds();

		TArray<uint8> Blob;
		if (!FFileHelper::LoadFileToArray(Blob, *Path))
		{
			Result.Error = FString::Printf(TEXT("non riesco a leggere %s"), *Path);
			Finish(MoveTemp(Result));
			return;
		}

		auto Tile = MakeShared<GeoWorld::Tiles::FImageTile>();
		const auto Parsed = GeoWorld::Tiles::ParseImageTile(Blob.GetData(), Blob.Num(), *Tile);
		if (Parsed != GeoWorld::Tiles::EImageTileParseResult::Ok)
		{
			Result.Error = FString::Printf(TEXT("%s: %s"), *Path,
				UTF8_TO_TCHAR(GeoWorld::Tiles::DescribeParseResult(Parsed)));
			Finish(MoveTemp(Result));
			return;
		}

		Result.ReadSeconds = FPlatformTime::Seconds() - ReadStarted;

		const double DecodeStarted = FPlatformTime::Seconds();
		if (!Decode(*Tile, Result.Error))
		{
			Finish(MoveTemp(Result));
			return;
		}
		Result.DecodeSeconds = FPlatformTime::Seconds() - DecodeStarted;

		// I byte compressi non servono piu': tenerli raddoppierebbe l'occupazione
		// della cache per niente.
		Tile->CompressedBytes.clear();
		Tile->CompressedBytes.shrink_to_fit();

		Result.Tile = Tile;
		Finish(MoveTemp(Result));
	}

	/**
	 * Chiamata quando il pool chiude o il lavoro viene annullato.
	 *
	 * Dimenticarla e' la causa classica delle perdite di memoria alla chiusura:
	 * AddQueuedWork ha preso la proprieta' dell'oggetto, e se nessuno lo
	 * distrugge resta li'.
	 */
	virtual void Abandon() override
	{
		UGeoImageryStreamingSubsystem::FLoadResult Result;
		Result.Key = Key;
		Result.Error = TEXT("caricamento annullato");
		Finish(MoveTemp(Result));
	}

private:
	static bool Decode(GeoWorld::Tiles::FImageTile& Tile, FString& OutError)
	{
		IImageWrapperModule& Module =
			FModuleManager::GetModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));

		const TSharedPtr<IImageWrapper> Wrapper = Module.CreateImageWrapper(EImageFormat::JPEG);
		if (!Wrapper.IsValid())
		{
			OutError = TEXT("nessun decoder JPEG disponibile");
			return false;
		}

		if (!Wrapper->SetCompressed(Tile.CompressedBytes.data(),
				static_cast<int64>(Tile.CompressedBytes.size())))
		{
			OutError = TEXT("il payload non e' un JPEG valido");
			return false;
		}

		TArray64<uint8> Raw;
		if (!Wrapper->GetRaw(ERGBFormat::BGRA, 8, Raw))
		{
			OutError = TEXT("decodifica JPEG fallita");
			return false;
		}

		const int64 Expected = static_cast<int64>(Tile.Width) * Tile.Height * 4;
		if (Raw.Num() != Expected)
		{
			OutError = FString::Printf(TEXT("decodificati %lld byte, attesi %lld"),
				static_cast<long long>(Raw.Num()), static_cast<long long>(Expected));
			return false;
		}

		Tile.Pixels.assign(Raw.GetData(), Raw.GetData() + Raw.Num());
		return true;
	}

	void Finish(UGeoImageryStreamingSubsystem::FLoadResult&& Result)
	{
		if (Owner.IsValid())
		{
			Owner->CompletedLoads.Enqueue(MoveTemp(Result));
		}
		delete this;
	}

	TWeakObjectPtr<UGeoImageryStreamingSubsystem> Owner;
	GeoWorld::Tiles::FTileKey Key;
	FString Path;
};

// ============================================================================
//  Ciclo di vita
// ============================================================================

bool UGeoImageryStreamingSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	if (!Super::ShouldCreateSubsystem(Outer)) { return false; }
	const UWorld* World = Cast<UWorld>(Outer);
	return World && (World->WorldType == EWorldType::Game
	              || World->WorldType == EWorldType::PIE
	              || World->WorldType == EWorldType::Editor);
}

void UGeoImageryStreamingSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	// Il modulo va caricato qui, sul game thread. Se lo facesse il primo worker
	// che passa, due worker potrebbero caricarlo insieme.
	FModuleManager::Get().LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));

	// Budget piu' alto di quello delle quote: una tile decodificata occupa
	// 256 KB (256 x 256 x 4 byte) contro i 66 KB di una tile di quote.
	//
	// Si chiama il setter invece di riassegnare l'intero oggetto: riassegnarlo
	// funzionerebbe, ma si appoggerebbe alla move-assignment implicita di una
	// classe che contiene una list e una unordered_map. Un giorno qualcuno
	// aggiunge un membro non assegnabile e il punto di rottura e' qui, lontano
	// dalla causa.
	Cache.SetBudgetBytes(512ull * 1024 * 1024);

	Pool.Startup(LoadThreadCount, TEXT("GeoImageryLoadPool"));

	UE_LOG(LogGeoWorld, Log, TEXT("[GeoImagery] Streaming avviato: %d thread, cache %d MB"),
		LoadThreadCount, GetCacheBudgetMB());
}

void UGeoImageryStreamingSubsystem::Deinitialize()
{
	Pool.Shutdown();

	CompletedLoads.Empty();
	InFlight.Empty();
	Cache.Clear();
	OnImageTileLoaded.Clear();

	Super::Deinitialize();
}

TStatId UGeoImageryStreamingSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UGeoImageryStreamingSubsystem, STATGROUP_Tickables);
}

void UGeoImageryStreamingSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	// Si svuota la coda dei worker. E' l'unico punto in cui la cache viene
	// scritta, ed e' sul game thread: per questo la cache non ha bisogno di
	// nessun lock.
	FLoadResult Result;
	while (CompletedLoads.Dequeue(Result))
	{
		InFlight.Remove(Result.Key.Pack());

		if (!Result.Error.IsEmpty() || !Result.Tile.IsValid())
		{
			++ErrorCount;
			UE_LOG(LogGeoWorld, Warning, TEXT("[GeoImagery] %d/%u/%u: %s"),
				Result.Key.Level, Result.Key.X, Result.Key.Y, *Result.Error);
			continue;
		}

		++CompletedCount;
		TotalReadSeconds += Result.ReadSeconds;
		TotalDecodeSeconds += Result.DecodeSeconds;

		Cache.Insert(Result.Key,
			std::shared_ptr<const GeoWorld::Tiles::FImageTile>(
				new GeoWorld::Tiles::FImageTile(MoveTemp(*Result.Tile))));

		OnImageTileLoaded.Broadcast(Result.Key);
	}
}

// ============================================================================
//  Dataset e richieste
// ============================================================================

bool UGeoImageryStreamingSubsystem::OpenDataset(const FString& RootDirectory, FString& OutError)
{
	if (!Dataset.Open(RootDirectory, OutError)) { return false; }

	ClearCache();

	// Si caricano subito tutti gli indici: sono pochi kilobyte per livello e
	// servono per sapere se una tile esiste PRIMA di chiederla.
	for (int32 Level = Dataset.GetMinLevel(); Level <= Dataset.GetMaxLevel(); ++Level)
	{
		FString LevelError;
		if (!Dataset.EnsureLevelIndex(Level, LevelError))
		{
			UE_LOG(LogGeoWorld, Warning, TEXT("[GeoImagery] livello %d senza indice: %s"),
				Level, *LevelError);
		}
	}
	return true;
}

UGeoImageryStreamingSubsystem::FImageTilePtr
UGeoImageryStreamingSubsystem::FindLoadedTile(const FTileKey& Key)
{
	return Cache.Find(Key);
}

EGeoTileState UGeoImageryStreamingSubsystem::RequestTile(const FTileKey& Key, int32 Priority)
{
	if (!Dataset.IsOpen()) { return EGeoTileState::Errore; }
	if (Cache.Peek(Key)) { return EGeoTileState::Pronta; }
	if (!Dataset.TileExists(Key)) { return EGeoTileState::Assente; }

	const uint64 Packed = Key.Pack();
	if (InFlight.Contains(Packed)) { return EGeoTileState::InCaricamento; }
	if (!Pool.IsRunning()) { return EGeoTileState::Errore; }

	InFlight.Add(Packed);
	Pool.AddWork(new FGeoImageryLoadWork(this, Key, Dataset.GetTileFilePath(Key)), Priority);

	return EGeoTileState::InCaricamento;
}

EGeoTileState UGeoImageryStreamingSubsystem::GetTileState(const FTileKey& Key) const
{
	if (Cache.Peek(Key)) { return EGeoTileState::Pronta; }
	if (InFlight.Contains(Key.Pack())) { return EGeoTileState::InCaricamento; }
	if (!Dataset.TileExists(Key)) { return EGeoTileState::Assente; }
	return EGeoTileState::NonCaricata;
}

void UGeoImageryStreamingSubsystem::CancelPendingRequests()
{
	Pool.CancelQueued();
	InFlight.Empty();
}

// ============================================================================
//  Cache
// ============================================================================

void UGeoImageryStreamingSubsystem::SetTilePinned(const FTileKey& Key, bool bPinned)
{
	Cache.SetPinned(Key, bPinned);
}

void UGeoImageryStreamingSubsystem::SetCacheBudgetMB(int32 Megabytes)
{
	Cache.SetBudgetBytes(static_cast<size_t>(FMath::Max(1, Megabytes)) * 1024ull * 1024ull);
}

int32 UGeoImageryStreamingSubsystem::GetCacheBudgetMB() const
{
	return static_cast<int32>(Cache.GetStats().BudgetBytes / (1024 * 1024));
}

void UGeoImageryStreamingSubsystem::ClearCache()
{
	Cache.Clear();
}

FGeoImageryStreamingStats UGeoImageryStreamingSubsystem::GetStats() const
{
	const GeoWorld::Tiles::FCacheStats CacheStats = Cache.GetStats();

	FGeoImageryStreamingStats Stats;
	Stats.TileResidenti = static_cast<int32>(CacheStats.ResidentTiles);
	Stats.TileInVolo = InFlight.Num();
	Stats.TilePinnate = static_cast<int32>(CacheStats.PinnedTiles);
	Stats.MemoriaMB = static_cast<float>(CacheStats.GetUsedMegabytes());
	Stats.BudgetMB = static_cast<float>(CacheStats.BudgetBytes) / (1024.0f * 1024.0f);
	Stats.HitRate = static_cast<float>(CacheStats.GetHitRate());
	Stats.CaricamentiTotali = CompletedCount;
	Stats.ErroriDiCaricamento = ErrorCount;
	Stats.TempoMedioCaricamentoMs = (CompletedCount > 0)
		? static_cast<float>(TotalReadSeconds / CompletedCount * 1000.0) : 0.0f;
	Stats.TempoMedioDecodificaMs = (CompletedCount > 0)
		? static_cast<float>(TotalDecodeSeconds / CompletedCount * 1000.0) : 0.0f;
	return Stats;
}

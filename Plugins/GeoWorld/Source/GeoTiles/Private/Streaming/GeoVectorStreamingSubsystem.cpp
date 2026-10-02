#include "Streaming/GeoVectorStreamingSubsystem.h"

#include "GeoCoreModule.h"

#include "Engine/World.h"
#include "HAL/PlatformMemory.h"
#include "HAL/PlatformTime.h"
#include "Misc/FileHelper.h"

/**
 * Il lavoro sul thread di caricamento: legge il file e lo interpreta.
 *
 * Il parsing e' qui e non sul game thread per lo stesso motivo della
 * decodifica dei JPEG: una tile di centro citta' ha decine di migliaia di punti,
 * e farne arrivare una ventina insieme dopo un salto costerebbe frame.
 */
class FGeoVectorLoadWork : public IQueuedWork
{
public:
	FGeoVectorLoadWork(UGeoVectorStreamingSubsystem* InOwner,
	                   const GeoWorld::Tiles::FTileKey& InKey, const FString& InPath)
		: Owner(InOwner), Key(InKey), Path(InPath) {}

	virtual void DoThreadedWork() override
	{
		UGeoVectorStreamingSubsystem::FLoadResult Result;
		Result.Key = Key;
		const double Started = FPlatformTime::Seconds();

		TArray<uint8> Blob;
		if (!FFileHelper::LoadFileToArray(Blob, *Path))
		{
			Result.Error = FString::Printf(TEXT("non riesco a leggere %s"), *Path);
			Finish(MoveTemp(Result));
			return;
		}

		auto Tile = MakeShared<GeoWorld::Tiles::FVectorTile>();
		const auto Parsed = GeoWorld::Tiles::ParseVectorTile(Blob.GetData(), Blob.Num(), *Tile);
		if (Parsed != GeoWorld::Tiles::EVectorTileParseResult::Ok)
		{
			Result.Error = FString::Printf(TEXT("%s: %s"), *Path,
				UTF8_TO_TCHAR(GeoWorld::Tiles::DescribeVectorParseResult(Parsed)));
			Finish(MoveTemp(Result));
			return;
		}
		if (!(Tile->Key == Key))
		{
			Result.Error = FString::Printf(TEXT("%s dichiara %u/%u/%u: file fuori posto"),
				*Path, Tile->Key.Level, Tile->Key.X, Tile->Key.Y);
			Finish(MoveTemp(Result));
			return;
		}

		Result.Seconds = FPlatformTime::Seconds() - Started;
		Result.Tile = Tile;
		Finish(MoveTemp(Result));
	}

	virtual void Abandon() override
	{
		UGeoVectorStreamingSubsystem::FLoadResult Result;
		Result.Key = Key;
		Result.Error = TEXT("caricamento annullato");
		Result.bCancelled = true;
		Finish(MoveTemp(Result));
	}

private:
	void Finish(UGeoVectorStreamingSubsystem::FLoadResult&& Result)
	{
		if (Owner.IsValid())
		{
			Owner->CompletedLoads.Enqueue(MoveTemp(Result));
		}
		delete this;
	}

	TWeakObjectPtr<UGeoVectorStreamingSubsystem> Owner;
	GeoWorld::Tiles::FTileKey Key;
	FString Path;
};

// ============================================================================

bool UGeoVectorStreamingSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	if (!Super::ShouldCreateSubsystem(Outer)) { return false; }
	const UWorld* World = Cast<UWorld>(Outer);
	return World && (World->WorldType == EWorldType::Game
	              || World->WorldType == EWorldType::PIE
	              || World->WorldType == EWorldType::Editor);
}

void UGeoVectorStreamingSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	// Una tile vettoriale in memoria pesa da pochi KB a qualche centinaio: con
	// 128 MB ci stanno migliaia di tile, cioe' un'area di centinaia di km.
	const uint32 MemoryGB = FPlatformMemory::GetConstants().TotalPhysicalGB;
	const uint64 BudgetMB = (MemoryGB <= 16) ? 128 : (MemoryGB <= 32) ? 256 : 512;
	Cache.SetBudgetBytes(BudgetMB * 1024ull * 1024ull);

	Pool.Startup(LoadThreadCount, TEXT("GeoVectorLoadPool"));
}

void UGeoVectorStreamingSubsystem::Deinitialize()
{
	Pool.Shutdown();
	CompletedLoads.Empty();
	InFlight.Empty();
	Failed.Empty();
	Cache.Clear();
	OnVectorTileLoaded.Clear();
	Super::Deinitialize();
}

TStatId UGeoVectorStreamingSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UGeoVectorStreamingSubsystem, STATGROUP_Tickables);
}

void UGeoVectorStreamingSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	FLoadResult Result;
	while (CompletedLoads.Dequeue(Result))
	{
		InFlight.Remove(Result.Key.Pack());

		if (Result.bCancelled) { continue; }

		if (!Result.Error.IsEmpty() || !Result.Tile.IsValid())
		{
			// Un file rovinato o mancante non si richiede di nuovo a ogni
			// frame: chi aspetta questa tile deve poter smettere di aspettare.
			Failed.Add(Result.Key.Pack());
			++ErrorCount;
			UE_LOG(LogGeoWorld, Warning, TEXT("[GeoRoads] %d/%u/%u: %s"),
				Result.Key.Level, Result.Key.X, Result.Key.Y, *Result.Error);
			continue;
		}

		++CompletedCount;
		TotalSeconds += Result.Seconds;
		Cache.Insert(Result.Key, std::shared_ptr<const GeoWorld::Tiles::FVectorTile>(
			new GeoWorld::Tiles::FVectorTile(MoveTemp(*Result.Tile))));
		OnVectorTileLoaded.Broadcast(Result.Key);
	}
}

bool UGeoVectorStreamingSubsystem::OpenDataset(const FString& RootDirectory, FString& OutError)
{
	CancelPendingRequests();
	if (!Dataset.Open(RootDirectory, OutError)) { return false; }
	ClearCache();
	return true;
}

UGeoVectorStreamingSubsystem::FVectorTilePtr
UGeoVectorStreamingSubsystem::FindLoadedTile(const FTileKey& Key)
{
	return Cache.Find(Key);
}

EGeoTileState UGeoVectorStreamingSubsystem::RequestTile(const FTileKey& Key, int32 Priority)
{
	if (!Dataset.IsOpen()) { return EGeoTileState::Errore; }
	if (Cache.Peek(Key)) { return EGeoTileState::Pronta; }
	if (!Dataset.TileExists(Key)) { return EGeoTileState::Assente; }

	const uint64 Packed = Key.Pack();
	if (Failed.Contains(Packed)) { return EGeoTileState::Errore; }
	if (InFlight.Contains(Packed)) { return EGeoTileState::InCaricamento; }
	if (!Pool.IsRunning()) { return EGeoTileState::Errore; }

	InFlight.Add(Packed);
	Pool.AddWork(new FGeoVectorLoadWork(this, Key, Dataset.GetTileFilePath(Key)), Priority);
	return EGeoTileState::InCaricamento;
}

void UGeoVectorStreamingSubsystem::CancelPendingRequests()
{
	Pool.CancelQueued();
	InFlight.Empty();
}

void UGeoVectorStreamingSubsystem::ClearCache()
{
	Cache.Clear();
	Failed.Empty();
}

FGeoVectorStreamingStats UGeoVectorStreamingSubsystem::GetStats() const
{
	const GeoWorld::Tiles::FCacheStats CacheStats = Cache.GetStats();

	FGeoVectorStreamingStats Stats;
	Stats.TileResidenti = static_cast<int32>(CacheStats.ResidentTiles);
	Stats.TileInVolo = InFlight.Num();
	Stats.MemoriaMB = static_cast<float>(CacheStats.GetUsedMegabytes());
	Stats.BudgetMB = static_cast<float>(CacheStats.BudgetBytes) / (1024.0f * 1024.0f);
	Stats.CaricamentiTotali = CompletedCount;
	Stats.ErroriDiCaricamento = ErrorCount;
	Stats.TempoMedioCaricamentoMs = (CompletedCount > 0)
		? static_cast<float>(TotalSeconds / CompletedCount * 1000.0) : 0.0f;
	return Stats;
}

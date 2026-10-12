#include "Streaming/GeoWaterStreamingSubsystem.h"

#include "GeoCoreModule.h"

#include "Engine/World.h"
#include "HAL/PlatformMemory.h"
#include "HAL/PlatformTime.h"
#include "Misc/Compression.h"
#include "Misc/FileHelper.h"

#include <string>

/**
 * Il lavoro sul thread di caricamento: legge, decomprime, interpreta.
 *
 * NOTA UE: FCompression::UncompressMemory con NAME_Zlib legge il formato zlib
 * "con l'intestazione" (quello di zlib.compress in Python): il buffer di
 * destinazione deve avere ESATTAMENTE la dimensione decompressa, che l'header
 * della tile permette di calcolare prima.
 */
class FGeoWaterLoadWork : public IQueuedWork
{
public:
	FGeoWaterLoadWork(UGeoWaterStreamingSubsystem* InOwner, const GeoWorld::Tiles::FTileKey& InKey, const FString& InPath)
		: Owner(InOwner), Key(InKey), Path(InPath) {}

	virtual void DoThreadedWork() override
	{
		UGeoWaterStreamingSubsystem::FLoadResult Result;
		Result.Key = Key;
		const double Started = FPlatformTime::Seconds();

		TArray<uint8> Blob;
		if (!FFileHelper::LoadFileToArray(Blob, *Path))
		{
			Result.Error = FString::Printf(TEXT("non riesco a leggere %s"), *Path);
			Finish(MoveTemp(Result));
			return;
		}

		GeoWorld::Tiles::FWaterTileHeader Header;
		std::string Error;
		if (!GeoWorld::Tiles::ParseWaterTileHeader(Blob.GetData(), Blob.Num(), Key, Header, Error))
		{
			Result.Error = FString::Printf(TEXT("%s: %s"), *Path, UTF8_TO_TCHAR(Error.c_str()));
			Finish(MoveTemp(Result));
			return;
		}

		TArray<uint8> Plain;
		Plain.SetNumUninitialized(static_cast<int32>(Header.ExpectedPlainBytes()));
		if (!FCompression::UncompressMemory(NAME_Zlib, Plain.GetData(), Plain.Num(),
				Blob.GetData() + GeoWorld::Tiles::WaterTileHeaderBytes, static_cast<int32>(Header.PayloadBytes)))
		{
			Result.Error = FString::Printf(TEXT("%s: decompressione zlib fallita"), *Path);
			Finish(MoveTemp(Result));
			return;
		}

		auto Tile = MakeShared<GeoWorld::Tiles::FWaterTile>();
		if (!GeoWorld::Tiles::DecodeWaterTilePayload(Header, Plain.GetData(), Plain.Num(), *Tile, Error))
		{
			Result.Error = FString::Printf(TEXT("%s: %s"), *Path, UTF8_TO_TCHAR(Error.c_str()));
			Finish(MoveTemp(Result));
			return;
		}

		Result.Seconds = FPlatformTime::Seconds() - Started;
		Result.Tile = Tile;
		Finish(MoveTemp(Result));
	}

	virtual void Abandon() override
	{
		UGeoWaterStreamingSubsystem::FLoadResult Result;
		Result.Key = Key;
		Result.Error = TEXT("caricamento annullato");
		Result.bCancelled = true;
		Finish(MoveTemp(Result));
	}

private:
	void Finish(UGeoWaterStreamingSubsystem::FLoadResult&& Result)
	{
		if (Owner.IsValid()) { Owner->CompletedLoads.Enqueue(MoveTemp(Result)); }
		delete this;
	}

	TWeakObjectPtr<UGeoWaterStreamingSubsystem> Owner;
	GeoWorld::Tiles::FTileKey Key;
	FString Path;
};

// ============================================================================

bool UGeoWaterStreamingSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	if (!Super::ShouldCreateSubsystem(Outer)) { return false; }
	const UWorld* World = Cast<UWorld>(Outer);
	return World && (World->WorldType == EWorldType::Game
	              || World->WorldType == EWorldType::PIE
	              || World->WorldType == EWorldType::Editor);
}

void UGeoWaterStreamingSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	// ~0,4 MB a tile: 256 MB sono ~600 tile, piu' di quante il terreno ne
	// tenga costruite su un portatile.
	const uint32 MemoryGB = FPlatformMemory::GetConstants().TotalPhysicalGB;
	const uint64 BudgetMB = (MemoryGB <= 16) ? 256 : (MemoryGB <= 32) ? 512 : 1024;
	Cache.SetBudgetBytes(BudgetMB * 1024ull * 1024ull);
	Pool.Startup(LoadThreadCount, TEXT("GeoWaterLoadPool"));
}

void UGeoWaterStreamingSubsystem::Deinitialize()
{
	Pool.Shutdown();
	CompletedLoads.Empty();
	InFlight.Empty();
	Failed.Empty();
	Cache.Clear();
	Super::Deinitialize();
}

TStatId UGeoWaterStreamingSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UGeoWaterStreamingSubsystem, STATGROUP_Tickables);
}

void UGeoWaterStreamingSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	FLoadResult Result;
	while (CompletedLoads.Dequeue(Result))
	{
		InFlight.Remove(Result.Key.Pack());
		if (Result.bCancelled) { continue; }
		if (!Result.Error.IsEmpty() || !Result.Tile.IsValid())
		{
			// Chi aspetta questa tile deve poter smettere di aspettare: una
			// tile illeggibile vale "niente acqua", non "aspetta per sempre".
			Failed.Add(Result.Key.Pack());
			++ErrorCount;
			UE_LOG(LogGeoWorld, Warning, TEXT("[GeoWater] %d/%u/%u: %s"),
				Result.Key.Level, Result.Key.X, Result.Key.Y, *Result.Error);
			continue;
		}
		++CompletedCount;
		TotalSeconds += Result.Seconds;
		Cache.Insert(Result.Key, std::shared_ptr<const GeoWorld::Tiles::FWaterTile>(
			new GeoWorld::Tiles::FWaterTile(MoveTemp(*Result.Tile))));
	}
}

bool UGeoWaterStreamingSubsystem::OpenDataset(const FString& RootDirectory, FString& OutError)
{
	CancelPendingRequests();
	if (!Dataset.Open(RootDirectory, OutError)) { return false; }
	ClearCache();
	return true;
}

void UGeoWaterStreamingSubsystem::CloseDataset()
{
	CancelPendingRequests();
	Dataset.Close();
	ClearCache();
}

UGeoWaterStreamingSubsystem::FWaterTilePtr UGeoWaterStreamingSubsystem::FindLoadedTile(const FTileKey& Key)
{
	return Cache.Find(Key);
}

EGeoTileState UGeoWaterStreamingSubsystem::RequestTile(const FTileKey& Key, int32 Priority)
{
	if (!Dataset.IsOpen()) { return EGeoTileState::Errore; }
	if (Cache.Peek(Key)) { return EGeoTileState::Pronta; }
	if (!Dataset.TileExists(Key)) { return EGeoTileState::Assente; }

	const uint64 Packed = Key.Pack();
	if (Failed.Contains(Packed)) { return EGeoTileState::Errore; }
	if (InFlight.Contains(Packed)) { return EGeoTileState::InCaricamento; }
	if (!Pool.IsRunning()) { return EGeoTileState::Errore; }

	InFlight.Add(Packed);
	Pool.AddWork(new FGeoWaterLoadWork(this, Key, Dataset.GetTileFilePath(Key)), Priority);
	return EGeoTileState::InCaricamento;
}

void UGeoWaterStreamingSubsystem::CancelPendingRequests()
{
	Pool.CancelQueued();
	InFlight.Empty();
}

void UGeoWaterStreamingSubsystem::ClearCache()
{
	Cache.Clear();
	Failed.Empty();
}

FGeoWaterStreamingStats UGeoWaterStreamingSubsystem::GetStats() const
{
	const GeoWorld::Tiles::FCacheStats CacheStats = Cache.GetStats();
	FGeoWaterStreamingStats Stats;
	Stats.TileResidenti = static_cast<int32>(CacheStats.ResidentTiles);
	Stats.TileInVolo = InFlight.Num();
	Stats.MemoriaMB = static_cast<float>(CacheStats.GetUsedMegabytes());
	Stats.BudgetMB = static_cast<float>(CacheStats.BudgetBytes) / (1024.0f * 1024.0f);
	Stats.CaricamentiTotali = CompletedCount;
	Stats.ErroriDiCaricamento = ErrorCount;
	Stats.TempoMedioCaricamentoMs = (CompletedCount > 0) ? static_cast<float>(TotalSeconds / CompletedCount * 1000.0) : 0.0f;
	return Stats;
}

#include "Streaming/GeoVegetationStreamingSubsystem.h"

#include "GeoCoreModule.h"

#include "Engine/World.h"
#include "HAL/PlatformTime.h"
#include "Misc/Compression.h"
#include "Misc/FileHelper.h"

#include <string>

/**
 * Il lavoro sul thread di caricamento: legge, decomprime, interpreta.
 *
 * NOTA UE: con NAME_Zlib il buffer di destinazione deve avere ESATTAMENTE la
 * dimensione decompressa (256 x 256 byte), che l'header permette di
 * controllare prima.
 */
class FGeoVegetationLoadWork : public IQueuedWork
{
public:
	FGeoVegetationLoadWork(UGeoVegetationStreamingSubsystem* InOwner, const GeoWorld::Tiles::FTileKey& InKey, const FString& InPath)
		: Owner(InOwner), Key(InKey), Path(InPath) {}

	virtual void DoThreadedWork() override
	{
		UGeoVegetationStreamingSubsystem::FLoadResult Result;
		Result.Key = Key;
		const double Started = FPlatformTime::Seconds();

		TArray<uint8> Blob;
		if (!FFileHelper::LoadFileToArray(Blob, *Path))
		{
			Result.Error = FString::Printf(TEXT("non riesco a leggere %s"), *Path);
			Finish(MoveTemp(Result));
			return;
		}
		GeoWorld::Tiles::FVegetationTileHeader Header;
		std::string Error;
		if (!GeoWorld::Tiles::ParseVegetationTileHeader(Blob.GetData(), Blob.Num(), Key, Header, Error))
		{
			Result.Error = FString::Printf(TEXT("%s: %s"), *Path, UTF8_TO_TCHAR(Error.c_str()));
			Finish(MoveTemp(Result));
			return;
		}
		TArray<uint8> Plain;
		Plain.SetNumUninitialized(static_cast<int32>(Header.ExpectedPlainBytes()));
		if (!FCompression::UncompressMemory(NAME_Zlib, Plain.GetData(), Plain.Num(),
				Blob.GetData() + GeoWorld::Tiles::VegetationTileHeaderBytes, static_cast<int32>(Header.PayloadBytes)))
		{
			Result.Error = FString::Printf(TEXT("%s: decompressione zlib fallita"), *Path);
			Finish(MoveTemp(Result));
			return;
		}
		auto Tile = MakeShared<GeoWorld::Tiles::FVegetationTile>();
		if (!GeoWorld::Tiles::DecodeVegetationTilePayload(Header, Plain.GetData(), Plain.Num(), *Tile, Error))
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
		UGeoVegetationStreamingSubsystem::FLoadResult Result;
		Result.Key = Key;
		Result.Error = TEXT("caricamento annullato");
		Result.bCancelled = true;
		Finish(MoveTemp(Result));
	}

private:
	void Finish(UGeoVegetationStreamingSubsystem::FLoadResult&& Result)
	{
		if (Owner.IsValid()) { Owner->CompletedLoads.Enqueue(MoveTemp(Result)); }
		delete this;
	}

	TWeakObjectPtr<UGeoVegetationStreamingSubsystem> Owner;
	GeoWorld::Tiles::FTileKey Key;
	FString Path;
};

// ============================================================================

bool UGeoVegetationStreamingSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	if (!Super::ShouldCreateSubsystem(Outer)) { return false; }
	const UWorld* World = Cast<UWorld>(Outer);
	return World && (World->WorldType == EWorldType::Game
	              || World->WorldType == EWorldType::PIE
	              || World->WorldType == EWorldType::Editor);
}

void UGeoVegetationStreamingSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	// 64 KB a tile: 32 MB sono 500 tile, cioe' 2.500 km^2. Avanza sempre.
	Cache.SetBudgetBytes(32ull * 1024ull * 1024ull);
	Pool.Startup(LoadThreadCount, TEXT("GeoVegetationLoadPool"));
}

void UGeoVegetationStreamingSubsystem::Deinitialize()
{
	Pool.Shutdown();
	CompletedLoads.Empty();
	InFlight.Empty();
	Failed.Empty();
	Cache.Clear();
	Super::Deinitialize();
}

TStatId UGeoVegetationStreamingSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UGeoVegetationStreamingSubsystem, STATGROUP_Tickables);
}

void UGeoVegetationStreamingSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	FLoadResult Result;
	while (CompletedLoads.Dequeue(Result))
	{
		InFlight.Remove(Result.Key.Pack());
		if (Result.bCancelled) { continue; }
		if (!Result.Error.IsEmpty() || !Result.Tile.IsValid())
		{
			// Una tile illeggibile vale "niente piante", non "aspetta per sempre".
			Failed.Add(Result.Key.Pack());
			++ErrorCount;
			UE_LOG(LogGeoWorld, Warning, TEXT("[GeoVegetation] %d/%u/%u: %s"),
				Result.Key.Level, Result.Key.X, Result.Key.Y, *Result.Error);
			continue;
		}
		++CompletedCount;
		TotalSeconds += Result.Seconds;
		Cache.Insert(Result.Key, std::shared_ptr<const GeoWorld::Tiles::FVegetationTile>(
			new GeoWorld::Tiles::FVegetationTile(MoveTemp(*Result.Tile))));
	}
}

bool UGeoVegetationStreamingSubsystem::OpenDataset(const FString& RootDirectory, FString& OutError)
{
	CancelPendingRequests();
	if (!Dataset.Open(RootDirectory, OutError)) { return false; }
	ClearCache();
	return true;
}

void UGeoVegetationStreamingSubsystem::CloseDataset()
{
	CancelPendingRequests();
	Dataset.Close();
	ClearCache();
}

EGeoTileState UGeoVegetationStreamingSubsystem::RequestTile(const FTileKey& Key, int32 Priority)
{
	if (!Dataset.IsOpen()) { return EGeoTileState::Errore; }
	if (Cache.Peek(Key)) { return EGeoTileState::Pronta; }
	if (!Dataset.TileExists(Key)) { return EGeoTileState::Assente; }

	const uint64 Packed = Key.Pack();
	if (Failed.Contains(Packed)) { return EGeoTileState::Errore; }
	if (InFlight.Contains(Packed)) { return EGeoTileState::InCaricamento; }
	if (!Pool.IsRunning()) { return EGeoTileState::Errore; }

	InFlight.Add(Packed);
	Pool.AddWork(new FGeoVegetationLoadWork(this, Key, Dataset.GetTileFilePath(Key)), Priority);
	return EGeoTileState::InCaricamento;
}

void UGeoVegetationStreamingSubsystem::CancelPendingRequests()
{
	Pool.CancelQueued();
	InFlight.Empty();
}

void UGeoVegetationStreamingSubsystem::ClearCache()
{
	Cache.Clear();
	Failed.Empty();
}

FGeoVegetationStreamingStats UGeoVegetationStreamingSubsystem::GetStats() const
{
	const GeoWorld::Tiles::FCacheStats CacheStats = Cache.GetStats();
	FGeoVegetationStreamingStats Stats;
	Stats.TileResidenti = static_cast<int32>(CacheStats.ResidentTiles);
	Stats.TileInVolo = InFlight.Num();
	Stats.MemoriaMB = static_cast<float>(CacheStats.GetUsedMegabytes());
	Stats.CaricamentiTotali = CompletedCount;
	Stats.ErroriDiCaricamento = ErrorCount;
	Stats.TempoMedioCaricamentoMs = (CompletedCount > 0) ? static_cast<float>(TotalSeconds / CompletedCount * 1000.0) : 0.0f;
	return Stats;
}

#include "Unreal/GeoTileDataset.h"

#include "GeoCoreModule.h"

#include "Dom/JsonObject.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

using namespace GeoWorld::Tiles;

namespace
{
	/** Chiave compatta per la TMap dell'indice. */
	FORCEINLINE uint64 PackXY(uint32 X, uint32 Y)
	{
		return (static_cast<uint64>(Y) << 32) | static_cast<uint64>(X);
	}
}

bool FGeoTileDataset::Open(const FString& InRootDirectory, FString& OutError)
{
	*this = FGeoTileDataset();
	RootDirectory = InRootDirectory;

	const FString ManifestPath = FPaths::Combine(RootDirectory, TEXT("manifest.json"));

	// NOTA UE: FFileHelper e non std::ifstream.
	// In Unreal i file passano da un filesystem VIRTUALE: nelle build
	// pacchettizzate i contenuti stanno dentro archivi .pak, e possono essere
	// compressi o cifrati. std::ifstream li cercherebbe sul disco reale e non li
	// troverebbe: funziona nell'editor e si rompe al packaging, che e' il
	// momento peggiore per scoprirlo.
	FString ManifestText;
	if (!FFileHelper::LoadFileToString(ManifestText, *ManifestPath))
	{
		OutError = FString::Printf(TEXT("manifest.json non leggibile in %s"), *RootDirectory);
		return false;
	}

	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ManifestText);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		OutError = TEXT("manifest.json non e' JSON valido");
		return false;
	}

	DatasetName = Root->GetStringField(TEXT("datasetName"));

	// --- Verifica di compatibilita' del formato. -------------------------
	// Il manifest dichiara la geometria delle tile; il codice C++ ha le sue
	// costanti. Se divergono, meglio rifiutarsi di aprire il dataset che
	// leggerlo storto: e' il caso di un dataset generato con una versione
	// diversa della pipeline.
	const TSharedPtr<FJsonObject>* TileFormat = nullptr;
	if (Root->TryGetObjectField(TEXT("tileFormat"), TileFormat) && TileFormat)
	{
		const int32 Posts = (*TileFormat)->GetIntegerField(TEXT("posts"));
		if (Posts != TilePosts)
		{
			OutError = FString::Printf(
				TEXT("il dataset ha tile da %d post, questo runtime ne legge %d"),
				Posts, TilePosts);
			return false;
		}
	}

	const TSharedPtr<FJsonObject>* Box = nullptr;
	if (Root->TryGetObjectField(TEXT("boundingBox"), Box) && Box)
	{
		West = (*Box)->GetNumberField(TEXT("west"));
		South = (*Box)->GetNumberField(TEXT("south"));
		East = (*Box)->GetNumberField(TEXT("east"));
		North = (*Box)->GetNumberField(TEXT("north"));
	}

	const TSharedPtr<FJsonObject>* Vertical = nullptr;
	if (Root->TryGetObjectField(TEXT("verticalDatum"), Vertical) && Vertical)
	{
		(*Vertical)->TryGetStringField(TEXT("verticalCrs"), VerticalDatum);
	}

	const TArray<TSharedPtr<FJsonValue>>* LevelArray = nullptr;
	if (!Root->TryGetArrayField(TEXT("levels"), LevelArray) || !LevelArray || LevelArray->Num() == 0)
	{
		OutError = TEXT("il manifest non elenca nessun livello");
		return false;
	}

	MinLevel = MAX_int32;
	MaxLevel = MIN_int32;
	for (const TSharedPtr<FJsonValue>& Value : *LevelArray)
	{
		const TSharedPtr<FJsonObject> Object = Value->AsObject();
		if (!Object.IsValid()) { continue; }

		FLevelInfo Info;
		Info.Level = Object->GetIntegerField(TEXT("level"));
		Info.TileCount = Object->GetIntegerField(TEXT("tile_count"));
		Info.MinHeight = Object->GetNumberField(TEXT("min_height"));
		Info.MaxHeight = Object->GetNumberField(TEXT("max_height"));

		MinLevel = FMath::Min(MinLevel, Info.Level);
		MaxLevel = FMath::Max(MaxLevel, Info.Level);
		Levels.Add(MoveTemp(Info));
	}

	Levels.Sort([](const FLevelInfo& A, const FLevelInfo& B) { return A.Level < B.Level; });
	bIsOpen = true;

	UE_LOG(LogGeoWorld, Log,
		TEXT("[GeoTiles] Dataset '%s' aperto: livelli %d..%d, datum verticale %s"),
		*DatasetName, MinLevel, MaxLevel, *VerticalDatum);
	return true;
}

void FGeoTileDataset::GetBoundingBox(double& OutWest, double& OutSouth,
                                     double& OutEast, double& OutNorth) const
{
	OutWest = West; OutSouth = South; OutEast = East; OutNorth = North;
}

FGeoTileDataset::FLevelInfo* FGeoTileDataset::FindLevel(int32 Level)
{
	return Levels.FindByPredicate([Level](const FLevelInfo& Info) { return Info.Level == Level; });
}

const FGeoTileDataset::FLevelInfo* FGeoTileDataset::FindLevel(int32 Level) const
{
	return Levels.FindByPredicate([Level](const FLevelInfo& Info) { return Info.Level == Level; });
}

bool FGeoTileDataset::EnsureLevelIndex(int32 Level, FString& OutError)
{
	FLevelInfo* Info = FindLevel(Level);
	if (!Info)
	{
		OutError = FString::Printf(TEXT("il dataset non ha il livello %d"), Level);
		return false;
	}
	if (Info->bIndexLoaded) { return true; }

	const FString Path = FPaths::Combine(RootDirectory, FString::FromInt(Level), TEXT("index.bin"));

	TArray<uint8> Blob;
	if (!FFileHelper::LoadFileToArray(Blob, *Path))
	{
		OutError = FString::Printf(TEXT("indice non leggibile: %s"), *Path);
		return false;
	}

	std::vector<FTileIndexEntry> Entries;
	std::string DecodeError;
	if (!DecodeLevelIndex(Blob.GetData(), static_cast<size_t>(Blob.Num()),
	                      static_cast<uint32_t>(Level), Entries, DecodeError))
	{
		OutError = FString::Printf(TEXT("indice del livello %d non valido: %hs"),
			Level, DecodeError.c_str());
		return false;
	}

	Info->Tiles.Empty(Entries.size());
	for (const FTileIndexEntry& Entry : Entries)
	{
		Info->Tiles.Add(PackXY(Entry.X, Entry.Y), TPair<float, float>(Entry.MinHeight, Entry.MaxHeight));
	}
	Info->bIndexLoaded = true;

	UE_LOG(LogGeoWorld, Verbose, TEXT("[GeoTiles] Indice del livello %d: %d tile (%.1f kB)"),
		Level, Info->Tiles.Num(), Info->Tiles.Num() * 16.0 / 1024.0);
	return true;
}

bool FGeoTileDataset::TileExists(const FTileKey& Key) const
{
	const FLevelInfo* Info = FindLevel(static_cast<int32>(Key.Level));
	return Info && Info->bIndexLoaded && Info->Tiles.Contains(PackXY(Key.X, Key.Y));
}

bool FGeoTileDataset::GetTileHeightRange(const FTileKey& Key, float& OutMin, float& OutMax) const
{
	const FLevelInfo* Info = FindLevel(static_cast<int32>(Key.Level));
	if (!Info || !Info->bIndexLoaded) { return false; }

	if (const TPair<float, float>* Range = Info->Tiles.Find(PackXY(Key.X, Key.Y)))
	{
		OutMin = Range->Key;
		OutMax = Range->Value;
		return true;
	}
	return false;
}

FString FGeoTileDataset::GetTileFilePath(const FTileKey& Key) const
{
	return FPaths::Combine(RootDirectory, FString::FromInt(Key.Level),
		FString::FromInt(Key.X), FString::Printf(TEXT("%u.ght"), Key.Y));
}

int64 FGeoTileDataset::GetIndexedTileCount() const
{
	int64 Total = 0;
	for (const FLevelInfo& Info : Levels) { Total += Info.Tiles.Num(); }
	return Total;
}

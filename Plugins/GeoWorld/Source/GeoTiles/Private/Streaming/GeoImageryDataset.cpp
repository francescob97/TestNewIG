#include "Streaming/GeoImageryDataset.h"

#include "GeoCoreModule.h"

#include "Dom/JsonObject.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
	FORCEINLINE uint64 PackXY(uint32 X, uint32 Y)
	{
		return (static_cast<uint64>(Y) << 32) | static_cast<uint64>(X);
	}
}

bool FGeoImageryDataset::Open(const FString& InRootDirectory, FString& OutError)
{
	bIsOpen = false;
	Levels.Reset();
	RootDirectory = InRootDirectory;
	FPaths::NormalizeDirectoryName(RootDirectory);

	const FString ManifestPath = FPaths::Combine(RootDirectory, TEXT("manifest.json"));

	// NOTA UE: FFileHelper e non std::ifstream. Il motore ha un file system
	// virtuale: dentro un .pak un percorso "normale" non esiste come file sul
	// disco, e std::ifstream fallirebbe in una build impacchettata pur avendo
	// funzionato per tutto lo sviluppo.
	FString Json;
	if (!FFileHelper::LoadFileToString(Json, *ManifestPath))
	{
		OutError = FString::Printf(TEXT("non riesco a leggere %s"), *ManifestPath);
		return false;
	}

	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		OutError = FString::Printf(TEXT("%s non e' JSON valido"), *ManifestPath);
		return false;
	}

	// Il controllo che evita l'errore piu' probabile: puntare il drappeggio a
	// una piramide di QUOTE. Senza, si leggerebbero indici col formato
	// sbagliato e il messaggio d'errore arriverebbe molto piu' tardi e molto
	// piu' oscuro.
	const FString Kind = Root->GetStringField(TEXT("datasetKind"));
	if (Kind != TEXT("imagery"))
	{
		OutError = FString::Printf(
			TEXT("%s non dichiara un dataset di immagini (datasetKind = '%s'). ")
			TEXT("Hai puntato a una piramide di quote?"), *ManifestPath, *Kind);
		return false;
	}

	DatasetName = Root->GetStringField(TEXT("datasetName"));

	const TSharedPtr<FJsonObject>* Box = nullptr;
	if (Root->TryGetObjectField(TEXT("boundingBox"), Box) && Box)
	{
		West = (*Box)->GetNumberField(TEXT("west"));
		South = (*Box)->GetNumberField(TEXT("south"));
		East = (*Box)->GetNumberField(TEXT("east"));
		North = (*Box)->GetNumberField(TEXT("north"));
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
		Info.Level = static_cast<int32>(Object->GetNumberField(TEXT("level")));
		Info.TileCount = static_cast<int32>(Object->GetNumberField(TEXT("tile_count")));
		Object->TryGetNumberField(TEXT("partial_tiles"), Info.PartialTiles);
		Object->TryGetNumberField(TEXT("pixel_size_m_lat"), Info.PixelSizeMetres);

		MinLevel = FMath::Min(MinLevel, Info.Level);
		MaxLevel = FMath::Max(MaxLevel, Info.Level);
		Levels.Add(MoveTemp(Info));
	}

	Levels.Sort([](const FLevelInfo& A, const FLevelInfo& B) { return A.Level < B.Level; });

	bIsOpen = true;
	UE_LOG(LogGeoWorld, Log,
		TEXT("[GeoImagery] '%s': livelli %d..%d, %d livelli indicizzabili"),
		*DatasetName, MinLevel, MaxLevel, Levels.Num());
	return true;
}

void FGeoImageryDataset::GetBoundingBox(double& OutWest, double& OutSouth,
                                        double& OutEast, double& OutNorth) const
{
	OutWest = West; OutSouth = South; OutEast = East; OutNorth = North;
}

FGeoImageryDataset::FLevelInfo* FGeoImageryDataset::FindLevel(int32 Level)
{
	return Levels.FindByPredicate([Level](const FLevelInfo& Info) { return Info.Level == Level; });
}

const FGeoImageryDataset::FLevelInfo* FGeoImageryDataset::FindLevel(int32 Level) const
{
	return Levels.FindByPredicate([Level](const FLevelInfo& Info) { return Info.Level == Level; });
}

bool FGeoImageryDataset::EnsureLevelIndex(int32 Level, FString& OutError)
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
		OutError = FString::Printf(TEXT("non riesco a leggere %s"), *Path);
		return false;
	}

	std::vector<GeoWorld::Tiles::FImageIndexEntry> Entries;
	const char* ParseError = nullptr;
	if (!GeoWorld::Tiles::ParseImageIndex(Blob.GetData(), Blob.Num(),
			static_cast<uint32>(Level), Entries, ParseError))
	{
		OutError = FString::Printf(TEXT("%s: %s"), *Path, UTF8_TO_TCHAR(ParseError));
		return false;
	}

	Info->Tiles.Reserve(static_cast<int32>(Entries.size()));
	for (const GeoWorld::Tiles::FImageIndexEntry& Entry : Entries)
	{
		Info->Tiles.Add(PackXY(Entry.X, Entry.Y), Entry.CoveragePercent);
	}
	Info->bIndexLoaded = true;

	UE_LOG(LogGeoWorld, Log, TEXT("[GeoImagery] indice del livello %d: %d tile"),
		Level, Info->Tiles.Num());
	return true;
}

bool FGeoImageryDataset::TileExists(const GeoWorld::Tiles::FTileKey& Key) const
{
	const FLevelInfo* Info = FindLevel(static_cast<int32>(Key.Level));
	if (!Info || !Info->bIndexLoaded) { return false; }
	return Info->Tiles.Contains(PackXY(Key.X, Key.Y));
}

bool FGeoImageryDataset::GetTileCoverage(const GeoWorld::Tiles::FTileKey& Key,
                                         uint8& OutPercent) const
{
	const FLevelInfo* Info = FindLevel(static_cast<int32>(Key.Level));
	if (!Info || !Info->bIndexLoaded) { return false; }
	if (const uint8* Found = Info->Tiles.Find(PackXY(Key.X, Key.Y)))
	{
		OutPercent = *Found;
		return true;
	}
	return false;
}

FString FGeoImageryDataset::GetTileFilePath(const GeoWorld::Tiles::FTileKey& Key) const
{
	return FPaths::Combine(RootDirectory,
		FString::FromInt(static_cast<int32>(Key.Level)),
		FString::FromInt(static_cast<int32>(Key.X)),
		FString::Printf(TEXT("%u.gim"), Key.Y));
}

int64 FGeoImageryDataset::GetIndexedTileCount() const
{
	int64 Total = 0;
	for (const FLevelInfo& Info : Levels) { Total += Info.Tiles.Num(); }
	return Total;
}

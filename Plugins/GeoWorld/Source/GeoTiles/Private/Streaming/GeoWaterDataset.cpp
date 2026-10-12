#include "Streaming/GeoWaterDataset.h"

#include "GeoCoreModule.h"

#include "Dom/JsonObject.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#include <string>
#include <vector>

void FGeoWaterDataset::Close()
{
	bIsOpen = false;
	Levels.Reset();
	Geoid = GeoWorld::Tiles::FGeoidGrid();
	MinLevel = MaxLevel = -1;
}

bool FGeoWaterDataset::Open(const FString& InRootDirectory, FString& OutError)
{
	Close();
	RootDirectory = InRootDirectory;
	FPaths::NormalizeDirectoryName(RootDirectory);

	const FString ManifestPath = FPaths::Combine(RootDirectory, TEXT("manifest.json"));
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

	// Lo sbaglio piu' probabile: la cartella delle quote, delle ortofoto o
	// delle strade. Lo si dice con il nome giusto.
	FString Kind;
	Root->TryGetStringField(TEXT("datasetKind"), Kind);
	if (Kind != TEXT("water"))
	{
		OutError = FString::Printf(
			TEXT("%s non dichiara un dataset d'acqua (datasetKind = '%s'). ")
			TEXT("L'acqua si fa con: python run.py build-water"), *ManifestPath, *Kind);
		return false;
	}
	Root->TryGetStringField(TEXT("datasetName"), DatasetName);

	const TSharedPtr<FJsonObject>* Box = nullptr;
	if (Root->TryGetObjectField(TEXT("boundingBox"), Box) && Box)
	{
		(*Box)->TryGetNumberField(TEXT("west"), West);
		(*Box)->TryGetNumberField(TEXT("south"), South);
		(*Box)->TryGetNumberField(TEXT("east"), East);
		(*Box)->TryGetNumberField(TEXT("north"), North);
	}

	const TArray<TSharedPtr<FJsonValue>>* LevelArray = nullptr;
	if (!Root->TryGetArrayField(TEXT("levels"), LevelArray) || !LevelArray || LevelArray->Num() == 0)
	{
		OutError = TEXT("il manifest non elenca nessun livello");
		return false;
	}

	for (const TSharedPtr<FJsonValue>& Value : *LevelArray)
	{
		const TSharedPtr<FJsonObject> Object = Value->AsObject();
		if (!Object.IsValid()) { continue; }
		FLevelInfo Info;
		Object->TryGetNumberField(TEXT("level"), Info.Level);

		// Gli indici si leggono tutti subito: servono a sapere se una tile di
		// terreno ha acqua PRIMA di costruirla, e sono piccoli.
		const FString Path = FPaths::Combine(RootDirectory, FString::FromInt(Info.Level), TEXT("index.bin"));
		TArray<uint8> Blob;
		std::vector<GeoWorld::Tiles::FWaterIndexEntry> Entries;
		std::string Error;
		if (!FFileHelper::LoadFileToArray(Blob, *Path)
			|| !GeoWorld::Tiles::ParseWaterIndex(Blob.GetData(), Blob.Num(), static_cast<uint32>(Info.Level), Entries, Error))
		{
			UE_LOG(LogGeoWorld, Warning, TEXT("[GeoWater] livello %d: indice illeggibile (%s)"),
				Info.Level, UTF8_TO_TCHAR(Error.c_str()));
			continue;
		}
		Info.Tiles.Reserve(static_cast<int32>(Entries.size()));
		for (const GeoWorld::Tiles::FWaterIndexEntry& Entry : Entries)
		{
			Info.Tiles.Add(GeoWorld::Tiles::FTileKey::PackXY(Entry.X, Entry.Y), Entry.Flags);
		}
		Levels.Add(MoveTemp(Info));
	}
	if (Levels.Num() == 0)
	{
		OutError = TEXT("nessun livello con un indice leggibile");
		return false;
	}
	Levels.Sort([](const FLevelInfo& A, const FLevelInfo& B) { return A.Level < B.Level; });
	MinLevel = Levels[0].Level;
	MaxLevel = Levels.Last().Level;

	// Il geoide e' facoltativo: senza, l'acqua funziona e le stampe non danno
	// la quota sul livello del mare.
	TArray<uint8> GeoidBlob;
	std::string GeoidError;
	if (FFileHelper::LoadFileToArray(GeoidBlob, *FPaths::Combine(RootDirectory, TEXT("geoid.bin")))
		&& !GeoWorld::Tiles::ParseGeoid(GeoidBlob.GetData(), GeoidBlob.Num(), Geoid, GeoidError))
	{
		UE_LOG(LogGeoWorld, Warning, TEXT("[GeoWater] geoid.bin: %s"), UTF8_TO_TCHAR(GeoidError.c_str()));
	}

	bIsOpen = true;
	UE_LOG(LogGeoWorld, Log, TEXT("[GeoWater] '%s': livelli %d..%d, %lld tile, geoide %s"),
		*DatasetName, MinLevel, MaxLevel, static_cast<long long>(GetIndexedTileCount()),
		Geoid.IsValid() ? TEXT("si'") : TEXT("no"));
	return true;
}

const FGeoWaterDataset::FLevelInfo* FGeoWaterDataset::FindLevel(int32 Level) const
{
	return Levels.FindByPredicate([Level](const FLevelInfo& Info) { return Info.Level == Level; });
}

bool FGeoWaterDataset::TileExists(const GeoWorld::Tiles::FTileKey& Key) const
{
	const FLevelInfo* Info = FindLevel(static_cast<int32>(Key.Level));
	return Info && Info->Tiles.Contains(GeoWorld::Tiles::FTileKey::PackXY(Key.X, Key.Y));
}

bool FGeoWaterDataset::ResolveWaterKey(const GeoWorld::Tiles::FTileKey& TerrainKey,
                                       GeoWorld::Tiles::FTileKey& OutWaterKey) const
{
	if (!bIsOpen) { return false; }
	const int32 Level = static_cast<int32>(TerrainKey.Level);
	if (Level < MinLevel) { return false; }

	// Dentro i livelli del dataset l'indice e' la verita': la pipeline ha
	// guardato tutte le tile di terreno di quel livello, e quelle senza acqua
	// non le ha scritte.
	if (Level <= MaxLevel)
	{
		if (!FindLevel(Level)) { return false; }
		OutWaterKey = TerrainKey;
		return TileExists(TerrainKey);
	}

	// Oltre il livello piu' fine: l'antenato, ritagliato (FWaterView).
	const uint32 Depth = static_cast<uint32>(Level - MaxLevel);
	OutWaterKey = GeoWorld::Tiles::FTileKey{ static_cast<uint32>(MaxLevel), TerrainKey.X >> Depth, TerrainKey.Y >> Depth };
	return TileExists(OutWaterKey);
}

FString FGeoWaterDataset::GetTileFilePath(const GeoWorld::Tiles::FTileKey& Key) const
{
	return FPaths::Combine(RootDirectory, FString::FromInt(static_cast<int32>(Key.Level)),
		FString::FromInt(static_cast<int32>(Key.X)), FString::Printf(TEXT("%u.gwa"), Key.Y));
}

void FGeoWaterDataset::GetBoundingBox(double& OutWest, double& OutSouth, double& OutEast, double& OutNorth) const
{
	OutWest = West; OutSouth = South; OutEast = East; OutNorth = North;
}

int64 FGeoWaterDataset::GetIndexedTileCount() const
{
	int64 Total = 0;
	for (const FLevelInfo& Info : Levels) { Total += Info.Tiles.Num(); }
	return Total;
}

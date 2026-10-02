#include "Streaming/GeoVectorDataset.h"

#include "GeoCoreModule.h"

#include "Dom/JsonObject.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

bool FGeoVectorDataset::Open(const FString& InRootDirectory, FString& OutError)
{
	bIsOpen = false;
	Levels.Reset();
	LevelNumbers.clear();
	ClassWarning.Reset();
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

	// Lo sbaglio piu' probabile: dare a geo.Roads.Open la cartella delle quote o
	// delle ortofoto. Meglio dirlo qui, con il nome giusto, che fallire dopo
	// leggendo un indice con il magic sbagliato.
	FString Kind;
	Root->TryGetStringField(TEXT("datasetKind"), Kind);
	if (Kind != TEXT("vector"))
	{
		OutError = FString::Printf(
			TEXT("%s non dichiara un dataset di strade (datasetKind = '%s'). ")
			TEXT("Hai puntato alle quote o alle ortofoto? Le strade si fanno con: python run.py build-roads"),
			*ManifestPath, *Kind);
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

	// Le classi: se la pipeline ne conosce una che questo runtime non sa
	// disegnare (pipeline piu' nuova del plugin), lo si dice invece di non
	// disegnarla in silenzio.
	const TArray<TSharedPtr<FJsonValue>>* ClassArray = nullptr;
	if (Root->TryGetArrayField(TEXT("classes"), ClassArray) && ClassArray)
	{
		for (const TSharedPtr<FJsonValue>& Value : *ClassArray)
		{
			const TSharedPtr<FJsonObject> Object = Value->AsObject();
			if (!Object.IsValid()) { continue; }
			int32 Id = 0;
			FString Name;
			Object->TryGetNumberField(TEXT("id"), Id);
			Object->TryGetStringField(TEXT("name"), Name);
			const FString Known = UTF8_TO_TCHAR(GeoWorld::Tiles::RoadClassName(
				static_cast<GeoWorld::Tiles::ERoadClass>(Id)));
			if (Known != Name)
			{
				ClassWarning += FString::Printf(TEXT("%s%s=%d"),
					ClassWarning.IsEmpty() ? TEXT("") : TEXT(", "), *Name, Id);
			}
		}
		if (!ClassWarning.IsEmpty())
		{
			ClassWarning = FString::Printf(
				TEXT("classi che questo plugin non conosce (pipeline piu' nuova?): %s"), *ClassWarning);
			UE_LOG(LogGeoWorld, Warning, TEXT("[GeoRoads] %s"), *ClassWarning);
		}
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
		Object->TryGetNumberField(TEXT("tile_count"), Info.TileCount);
		Object->TryGetNumberField(TEXT("feature_count"), Info.FeatureCount);

		// Gli indici si caricano tutti subito: qualche centinaio di KB anche per
		// l'Italia, e servono per sapere se una tile esiste PRIMA di chiederla.
		FString LevelError;
		if (!LoadLevelIndex(Info, LevelError))
		{
			UE_LOG(LogGeoWorld, Warning, TEXT("[GeoRoads] livello %d senza indice: %s"),
				Info.Level, *LevelError);
			continue;
		}
		Levels.Add(MoveTemp(Info));
	}

	if (Levels.Num() == 0)
	{
		OutError = TEXT("nessun livello con un indice leggibile");
		return false;
	}

	Levels.Sort([](const FLevelInfo& A, const FLevelInfo& B) { return A.Level < B.Level; });
	for (const FLevelInfo& Info : Levels) { LevelNumbers.push_back(static_cast<uint32_t>(Info.Level)); }

	bIsOpen = true;
	UE_LOG(LogGeoWorld, Log, TEXT("[GeoRoads] '%s': livelli %d..%d, %lld tile"),
		*DatasetName, GetMinLevel(), GetMaxLevel(), static_cast<long long>(GetIndexedTileCount()));
	return true;
}

bool FGeoVectorDataset::LoadLevelIndex(FLevelInfo& Info, FString& OutError)
{
	const FString Path = FPaths::Combine(RootDirectory, FString::FromInt(Info.Level), TEXT("index.bin"));

	TArray<uint8> Blob;
	if (!FFileHelper::LoadFileToArray(Blob, *Path))
	{
		OutError = FString::Printf(TEXT("non riesco a leggere %s"), *Path);
		return false;
	}

	std::vector<GeoWorld::Tiles::FVectorIndexEntry> Entries;
	const char* ParseError = nullptr;
	if (!GeoWorld::Tiles::ParseVectorIndex(Blob.GetData(), Blob.Num(),
			static_cast<uint32>(Info.Level), Entries, ParseError))
	{
		OutError = FString::Printf(TEXT("%s: %s"), *Path, UTF8_TO_TCHAR(ParseError));
		return false;
	}

	Info.Tiles.Reserve(static_cast<int32>(Entries.size()));
	for (const GeoWorld::Tiles::FVectorIndexEntry& Entry : Entries)
	{
		Info.Tiles.Add(GeoWorld::Tiles::FTileKey::PackXY(Entry.X, Entry.Y), Entry.FeatureCount);
	}
	Info.bIndexLoaded = true;
	return true;
}

void FGeoVectorDataset::GetBoundingBox(double& OutWest, double& OutSouth,
                                       double& OutEast, double& OutNorth) const
{
	OutWest = West; OutSouth = South; OutEast = East; OutNorth = North;
}

const FGeoVectorDataset::FLevelInfo* FGeoVectorDataset::FindLevel(int32 Level) const
{
	return Levels.FindByPredicate([Level](const FLevelInfo& Info) { return Info.Level == Level; });
}

bool FGeoVectorDataset::TileExists(const GeoWorld::Tiles::FTileKey& Key) const
{
	const FLevelInfo* Info = FindLevel(static_cast<int32>(Key.Level));
	return Info && Info->bIndexLoaded && Info->Tiles.Contains(GeoWorld::Tiles::FTileKey::PackXY(Key.X, Key.Y));
}

FString FGeoVectorDataset::GetTileFilePath(const GeoWorld::Tiles::FTileKey& Key) const
{
	return FPaths::Combine(RootDirectory,
		FString::FromInt(static_cast<int32>(Key.Level)),
		FString::FromInt(static_cast<int32>(Key.X)),
		FString::Printf(TEXT("%u.gvt"), Key.Y));
}

int64 FGeoVectorDataset::GetIndexedTileCount() const
{
	int64 Total = 0;
	for (const FLevelInfo& Info : Levels) { Total += Info.Tiles.Num(); }
	return Total;
}

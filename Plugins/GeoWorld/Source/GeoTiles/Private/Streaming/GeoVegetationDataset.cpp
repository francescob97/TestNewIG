#include "Streaming/GeoVegetationDataset.h"

#include "GeoCoreModule.h"

#include "Dom/JsonObject.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#include "Tiles/TilingScheme.h"

#include <string>
#include <vector>

void FGeoVegetationDataset::Close()
{
	bIsOpen = false;
	Tiles.Reset();
	Cover.Reset();
	Level = -1;
}

bool FGeoVegetationDataset::Open(const FString& InRootDirectory, FString& OutError)
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

	// Lo sbaglio piu' probabile: la cartella di un altro dataset. Si dice con il nome giusto.
	FString Kind;
	Root->TryGetStringField(TEXT("datasetKind"), Kind);
	if (Kind != TEXT("vegetation"))
	{
		OutError = FString::Printf(
			TEXT("%s non dichiara un dataset di vegetazione (datasetKind = '%s'). ")
			TEXT("La vegetazione si fa con: geoworld build-vegetation"), *ManifestPath, *Kind);
		return false;
	}
	Root->TryGetStringField(TEXT("datasetName"), DatasetName);
	if (!Root->TryGetNumberField(TEXT("level"), Level) || Level < 0 || Level > 24)
	{
		OutError = TEXT("il manifest non dice il livello delle tile (\"level\")");
		return false;
	}

	const TSharedPtr<FJsonObject>* Box = nullptr;
	if (Root->TryGetObjectField(TEXT("boundingBox"), Box) && Box)
	{
		(*Box)->TryGetNumberField(TEXT("west"), West);
		(*Box)->TryGetNumberField(TEXT("south"), South);
		(*Box)->TryGetNumberField(TEXT("east"), East);
		(*Box)->TryGetNumberField(TEXT("north"), North);
	}
	const TSharedPtr<FJsonObject>* CoverObject = nullptr;
	if (Root->TryGetObjectField(TEXT("cover"), CoverObject) && CoverObject)
	{
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*CoverObject)->Values)
		{
			double Fraction = 0.0;
			if (Pair.Value.IsValid() && Pair.Value->TryGetNumber(Fraction)) { Cover.Add(Pair.Key, Fraction); }
		}
	}

	// L'indice si legge subito: dice quali tile ci sono, e una che manca vuol
	// dire "niente piante" senza dover provare ad aprire il file.
	const FString IndexPath = FPaths::Combine(RootDirectory, FString::FromInt(Level), TEXT("index.bin"));
	TArray<uint8> Blob;
	if (!FFileHelper::LoadFileToArray(Blob, *IndexPath))
	{
		OutError = FString::Printf(TEXT("non riesco a leggere %s"), *IndexPath);
		return false;
	}
	std::vector<GeoWorld::Tiles::FVegetationIndexEntry> Entries;
	std::string Error;
	if (!GeoWorld::Tiles::ParseVegetationIndex(Blob.GetData(), Blob.Num(), static_cast<uint32>(Level), Entries, Error))
	{
		OutError = FString::Printf(TEXT("%s: %s"), *IndexPath, UTF8_TO_TCHAR(Error.c_str()));
		return false;
	}
	Tiles.Reserve(static_cast<int32>(Entries.size()));
	for (const GeoWorld::Tiles::FVegetationIndexEntry& Entry : Entries)
	{
		Tiles.Add(GeoWorld::Tiles::FTileKey::PackXY(Entry.X, Entry.Y), Entry.Flags);
	}

	bIsOpen = true;
	UE_LOG(LogGeoWorld, Log, TEXT("[GeoVegetation] '%s' (%s): livello %d, %d tile"),
		*DatasetName, *RootDirectory, Level, Tiles.Num());
	return true;
}

GeoWorld::Tiles::FTileKey FGeoVegetationDataset::KeyAt(double LonDeg, double LatDeg) const
{
	uint32 X = 0, Y = 0;
	GeoWorld::Tiles::TileForLonLat(static_cast<uint32>(FMath::Max(Level, 0)), LonDeg, LatDeg, X, Y);
	return GeoWorld::Tiles::FTileKey{ static_cast<uint32>(FMath::Max(Level, 0)), X, Y };
}

bool FGeoVegetationDataset::TileExists(const GeoWorld::Tiles::FTileKey& Key) const
{
	return bIsOpen && static_cast<int32>(Key.Level) == Level
		&& Tiles.Contains(GeoWorld::Tiles::FTileKey::PackXY(Key.X, Key.Y));
}

uint16 FGeoVegetationDataset::GetTileFlags(const GeoWorld::Tiles::FTileKey& Key) const
{
	if (!bIsOpen || static_cast<int32>(Key.Level) != Level) { return 0; }
	const uint16* Flags = Tiles.Find(GeoWorld::Tiles::FTileKey::PackXY(Key.X, Key.Y));
	return Flags ? *Flags : 0;
}

FString FGeoVegetationDataset::GetTileFilePath(const GeoWorld::Tiles::FTileKey& Key) const
{
	return FPaths::Combine(RootDirectory, FString::FromInt(static_cast<int32>(Key.Level)),
		FString::FromInt(static_cast<int32>(Key.X)), FString::Printf(TEXT("%u.gvg"), Key.Y));
}

void FGeoVegetationDataset::GetBoundingBox(double& OutWest, double& OutSouth, double& OutEast, double& OutNorth) const
{
	OutWest = West; OutSouth = South; OutEast = East; OutNorth = North;
}

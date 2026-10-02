// =============================================================================
//  GeoVectorDataset.h -- Un dataset di linee (strade, ferrovie) su disco.
//  STRATO: UNREAL.
//
//  Terzo gemello di FGeoTileDataset (quote) e FGeoImageryDataset (ortofoto).
//  Non una variante di uno dei due: l'indice qui porta il numero di linee per
//  tile, e i livelli non sono una piramide completa ma pochi livelli scelti
//  (di norma 10..13), ognuno con le sue classi di strade.
// =============================================================================
#pragma once

#include "CoreMinimal.h"

#include "Tiles/TileKey.h"
#include "Tiles/VectorTileFormat.h"

#include <cstdint>
#include <vector>

class GEOTILES_API FGeoVectorDataset
{
public:
	struct FLevelInfo
	{
		int32 Level = 0;
		int32 TileCount = 0;
		int64 FeatureCount = 0;
		bool bIndexLoaded = false;
		/** Chiave: FTileKey::PackXY. Valore: numero di linee nella tile. */
		TMap<uint64, uint32> Tiles;
	};

	/** Apre il dataset leggendo manifest.json e TUTTI gli indici (sono piccoli). */
	bool Open(const FString& InRootDirectory, FString& OutError);

	bool IsOpen() const { return bIsOpen; }
	const FString& GetRootDirectory() const { return RootDirectory; }
	const FString& GetDatasetName() const { return DatasetName; }
	void GetBoundingBox(double& OutWest, double& OutSouth, double& OutEast, double& OutNorth) const;

	/** I livelli presenti, in ordine crescente: e' cio' che ChooseVectorLevel vuole. */
	const std::vector<uint32_t>& GetLevelNumbers() const { return LevelNumbers; }
	int32 GetMinLevel() const { return LevelNumbers.empty() ? -1 : static_cast<int32>(LevelNumbers.front()); }
	int32 GetMaxLevel() const { return LevelNumbers.empty() ? -1 : static_cast<int32>(LevelNumbers.back()); }

	/** Esiste una tile a questa chiave? Una tile senza linee non esiste. */
	bool TileExists(const GeoWorld::Tiles::FTileKey& Key) const;

	FString GetTileFilePath(const GeoWorld::Tiles::FTileKey& Key) const;

	const TArray<FLevelInfo>& GetLevels() const { return Levels; }
	int64 GetIndexedTileCount() const;

	/** Le classi dichiarate dal manifest che il runtime non conosce (vuoto = tutto ok). */
	const FString& GetClassWarning() const { return ClassWarning; }

private:
	bool LoadLevelIndex(FLevelInfo& Info, FString& OutError);
	const FLevelInfo* FindLevel(int32 Level) const;

	FString RootDirectory;
	FString DatasetName;
	FString ClassWarning;
	bool bIsOpen = false;
	double West = 0.0, South = 0.0, East = 0.0, North = 0.0;

	TArray<FLevelInfo> Levels;
	std::vector<uint32_t> LevelNumbers;
};

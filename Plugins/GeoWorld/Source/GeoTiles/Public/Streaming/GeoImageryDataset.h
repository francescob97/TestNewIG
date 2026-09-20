// =============================================================================
//  GeoImageryDataset.h -- Un dataset di ortofoto su disco. STRATO: UNREAL.
//
//  E' il gemello di FGeoTileDataset, non una sua variante. Le due cose che
//  descrivono divergono davvero: l'indice delle quote porta min e max quota per
//  tile (servono al culling PRIMA di leggere il file), quello delle immagini
//  porta la copertura (serve a sapere se la tile vale la pena di essere
//  disegnata). Un record unico con campi dal significato variabile renderebbe
//  illeggibili tutti e due.
// =============================================================================
#pragma once

#include "CoreMinimal.h"

#include "Tiles/ImageTileFormat.h"
#include "Tiles/TileKey.h"

class GEOTILES_API FGeoImageryDataset
{
public:
	struct FLevelInfo
	{
		int32 Level = 0;
		int32 TileCount = 0;
		int32 PartialTiles = 0;
		double PixelSizeMetres = 0.0;
		bool bIndexLoaded = false;
		/** Chiave: (Y << 32) | X. Valore: copertura in percentuale. */
		TMap<uint64, uint8> Tiles;
	};

	/** Apre il dataset leggendo manifest.json. Non carica nessun indice. */
	bool Open(const FString& InRootDirectory, FString& OutError);

	bool IsOpen() const { return bIsOpen; }
	const FString& GetRootDirectory() const { return RootDirectory; }
	const FString& GetDatasetName() const { return DatasetName; }

	int32 GetMinLevel() const { return MinLevel; }
	int32 GetMaxLevel() const { return MaxLevel; }
	void GetBoundingBox(double& OutWest, double& OutSouth, double& OutEast, double& OutNorth) const;

	bool EnsureLevelIndex(int32 Level, FString& OutError);

	/** Esiste una tile a questa chiave? Richiede l'indice del livello caricato. */
	bool TileExists(const GeoWorld::Tiles::FTileKey& Key) const;

	/** Copertura della tile, senza leggerla da disco. */
	bool GetTileCoverage(const GeoWorld::Tiles::FTileKey& Key, uint8& OutPercent) const;

	FString GetTileFilePath(const GeoWorld::Tiles::FTileKey& Key) const;

	const TArray<FLevelInfo>& GetLevels() const { return Levels; }
	int64 GetIndexedTileCount() const;

private:
	FLevelInfo* FindLevel(int32 Level);
	const FLevelInfo* FindLevel(int32 Level) const;

	FString RootDirectory;
	FString DatasetName;
	bool bIsOpen = false;

	int32 MinLevel = 0;
	int32 MaxLevel = 0;
	double West = 0.0, South = 0.0, East = 0.0, North = 0.0;

	TArray<FLevelInfo> Levels;
};

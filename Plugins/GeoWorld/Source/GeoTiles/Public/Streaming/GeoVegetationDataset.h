// =============================================================================
//  GeoVegetationDataset.h -- Un dataset di vegetazione su disco (geoworld
//  build-vegetation). STRATO: UNREAL.
//
//  Il quinto gemello dopo quote, ortofoto, strade e acqua, e il piu' semplice:
//  tutte le tile stanno a UN livello (13 di default), e una tile che non c'e'
//  vuol dire "qui non cresce niente" (mare, ghiacciai, roccia: la pipeline non
//  le scrive). Il manifest e l'indice si leggono all'apertura; le tile le
//  carica UGeoVegetationStreamingSubsystem quando servono.
// =============================================================================
#pragma once

#include "CoreMinimal.h"

#include "Tiles/TileKey.h"
#include "Tiles/VegetationTileFormat.h"

class GEOTILES_API FGeoVegetationDataset
{
public:
	bool Open(const FString& InRootDirectory, FString& OutError);
	void Close();

	bool IsOpen() const { return bIsOpen; }
	const FString& GetRootDirectory() const { return RootDirectory; }
	const FString& GetDatasetName() const { return DatasetName; }
	void GetBoundingBox(double& OutWest, double& OutSouth, double& OutEast, double& OutNorth) const;

	/** Il livello di tutte le tile (13 di default). */
	int32 GetLevel() const { return Level; }
	int64 GetIndexedTileCount() const { return Tiles.Num(); }

	/** La tile di vegetazione che contiene un punto (gradi), anche se non esiste. */
	GeoWorld::Tiles::FTileKey KeyAt(double LonDeg, double LatDeg) const;

	/** La tile c'e' (altrimenti li' non cresce niente). */
	bool TileExists(const GeoWorld::Tiles::FTileKey& Key) const;
	/** I flag della tile (VegetationTileTrees, ...), 0 se non c'e'. */
	uint16 GetTileFlags(const GeoWorld::Tiles::FTileKey& Key) const;
	FString GetTileFilePath(const GeoWorld::Tiles::FTileKey& Key) const;

	/**
	 * Quanto di ogni classe c'e' nel dataset (dal manifest, "cover"): nome
	 * della classe -> frazione 0..1. Per le stampe.
	 */
	const TMap<FString, double>& GetCover() const { return Cover; }

private:
	FString RootDirectory;
	FString DatasetName;
	bool bIsOpen = false;
	double West = 0.0, South = 0.0, East = 0.0, North = 0.0;
	int32 Level = -1;
	/** Chiave: FTileKey::PackXY. Valore: i flag della tile. */
	TMap<uint64, uint16> Tiles;
	TMap<FString, double> Cover;
};

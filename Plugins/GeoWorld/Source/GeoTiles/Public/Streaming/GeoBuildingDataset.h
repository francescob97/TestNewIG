// =============================================================================
//  GeoBuildingDataset.h -- Un dataset di edifici su disco (geoworld
//  build-buildings). STRATO: UNREAL.
//
//  Il gemello della vegetazione: tutte le tile stanno a UN livello (15 di
//  default, ~600 m), e una tile che non c'e' vuol dire "qui nessun edificio".
//  Il manifest e l'indice si leggono all'apertura; le tile le carica
//  UGeoBuildingStreamingSubsystem quando servono.
// =============================================================================
#pragma once

#include "CoreMinimal.h"

#include "Tiles/TileKey.h"
#include "Tiles/BuildingTileFormat.h"

class GEOTILES_API FGeoBuildingDataset
{
public:
	bool Open(const FString& InRootDirectory, FString& OutError);
	void Close();

	bool IsOpen() const { return bIsOpen; }
	const FString& GetRootDirectory() const { return RootDirectory; }
	const FString& GetDatasetName() const { return DatasetName; }
	void GetBoundingBox(double& OutWest, double& OutSouth, double& OutEast, double& OutNorth) const;

	/** Il livello di tutte le tile (15 di default). */
	int32 GetLevel() const { return Level; }
	int64 GetIndexedTileCount() const { return Tiles.Num(); }

	/** La tile di edifici che contiene un punto (gradi), anche se non esiste. */
	GeoWorld::Tiles::FTileKey KeyAt(double LonDeg, double LatDeg) const;

	/** La tile c'e' (altrimenti li' nessun edificio). */
	bool TileExists(const GeoWorld::Tiles::FTileKey& Key) const;
	/** Quanti edifici ha la tile (dall'indice), 0 se non c'e'. */
	uint32 GetTileBuildingCount(const GeoWorld::Tiles::FTileKey& Key) const;
	int64 GetBuildingCount() const { return BuildingCount; }
	/** La tile con piu' edifici (il centro della citta', di solito). Falso se il dataset e' vuoto. */
	bool GetBusiestTile(GeoWorld::Tiles::FTileKey& OutKey) const;
	FString GetTileFilePath(const GeoWorld::Tiles::FTileKey& Key) const;

	/** Quanti edifici per classe (dal manifest, "byClass"): per le stampe. */
	const TMap<FString, double>& GetByClass() const { return ByClass; }

private:
	FString RootDirectory;
	FString DatasetName;
	bool bIsOpen = false;
	double West = 0.0, South = 0.0, East = 0.0, North = 0.0;
	int32 Level = -1;
	/** Chiave: FTileKey::PackXY. Valore: quanti edifici. */
	TMap<uint64, uint32> Tiles;
	TMap<FString, double> ByClass;
	int64 BuildingCount = 0;
};

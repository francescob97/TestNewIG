// =============================================================================
//  GeoTileDataset.h -- Un dataset di tile su disco. STRATO: UNREAL.
// =============================================================================
#pragma once

#include "CoreMinimal.h"
#include "Tiles/TileFormat.h"
#include "Tiles/TileKey.h"

/**
 * Apre una cartella prodotta dalla pipeline e risponde a due domande:
 * "questa tile esiste?" e "dove sta il suo file?".
 *
 * PERCHE' L'INDICE E' INDISPENSABILE
 * Il quadtree (Fase 4) deve decidere se raffinare PRIMA di aver caricato
 * alcunche'. Senza indice dovrebbe tentare una lettura su disco per ogni tile
 * candidata e interpretare il fallimento: un accesso al filesystem per scoprire
 * un'assenza, sul percorso critico, migliaia di volte per frame. Con l'indice
 * in memoria l'assenza costa una ricerca in una TMap.
 *
 * Costo in memoria: 16 byte per tile piu' l'overhead della mappa, quindi circa
 * 20 MB per il livello piu' fine dell'Italia intera. Si caricano solo i livelli
 * effettivamente usati.
 */
class GEOTILES_API FGeoTileDataset
{
public:
	struct FLevelInfo
	{
		int32 Level = 0;
		int32 TileCount = 0;
		double MinHeight = 0.0;
		double MaxHeight = 0.0;
		bool bIndexLoaded = false;
		/** Chiave: (Y << 32) | X. Valore: min/max quota della tile. */
		TMap<uint64, TPair<float, float>> Tiles;
	};

	/** Apre il dataset leggendo manifest.json. Non carica nessun indice. */
	bool Open(const FString& InRootDirectory, FString& OutError);

	bool IsOpen() const { return bIsOpen; }
	const FString& GetRootDirectory() const { return RootDirectory; }
	const FString& GetDatasetName() const { return DatasetName; }
	const FString& GetVerticalDatum() const { return VerticalDatum; }

	int32 GetMinLevel() const { return MinLevel; }
	int32 GetMaxLevel() const { return MaxLevel; }
	void GetBoundingBox(double& OutWest, double& OutSouth, double& OutEast, double& OutNorth) const;

	/** Carica l'indice di un livello, se non gia' in memoria. */
	bool EnsureLevelIndex(int32 Level, FString& OutError);

	/** Esiste una tile con dato a questa chiave? Richiede l'indice caricato. */
	bool TileExists(const GeoWorld::Tiles::FTileKey& Key) const;

	/** Min/max quota della tile senza leggerla da disco: serve al culling. */
	bool GetTileHeightRange(const GeoWorld::Tiles::FTileKey& Key,
	                        float& OutMin, float& OutMax) const;

	FString GetTileFilePath(const GeoWorld::Tiles::FTileKey& Key) const;

	const TArray<FLevelInfo>& GetLevels() const { return Levels; }
	int64 GetIndexedTileCount() const;

private:
	FLevelInfo* FindLevel(int32 Level);
	const FLevelInfo* FindLevel(int32 Level) const;

	FString RootDirectory;
	FString DatasetName;
	FString VerticalDatum;
	TArray<FLevelInfo> Levels;
	double West = 0.0, South = 0.0, East = 0.0, North = 0.0;
	int32 MinLevel = 0, MaxLevel = 0;
	bool bIsOpen = false;
};

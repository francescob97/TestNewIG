// =============================================================================
//  GeoWaterDataset.h -- Un dataset d'acqua (Fase 9) su disco.
//  STRATO: UNREAL.
//
//  Quarto gemello dopo quote, ortofoto e strade. Due differenze:
//
//  - Una tile d'acqua esiste solo dove c'e' acqua. Per una tile di terreno
//    quindi la domanda non e' "dove sta il file" ma "quale tile d'acqua mi
//    riguarda": la sua, se il dataset arriva al suo livello (e allora
//    l'assenza dall'indice vuol dire "qui niente acqua"); un antenato
//    ritagliato, se il dataset si ferma prima (ResolveWaterKey).
//  - Porta il GEOIDE dell'area (geoid.bin), che serve alla quota sul livello
//    del mare nelle stampe a schermo.
// =============================================================================
#pragma once

#include "CoreMinimal.h"

#include "Tiles/TileKey.h"
#include "Tiles/WaterTileFormat.h"

class GEOTILES_API FGeoWaterDataset
{
public:
	struct FLevelInfo
	{
		int32 Level = 0;
		/** Chiave: FTileKey::PackXY. Valore: i flag della tile (mare, lago, fiume...). */
		TMap<uint64, uint16> Tiles;
	};

	bool Open(const FString& InRootDirectory, FString& OutError);
	void Close();

	bool IsOpen() const { return bIsOpen; }
	const FString& GetRootDirectory() const { return RootDirectory; }
	const FString& GetDatasetName() const { return DatasetName; }
	void GetBoundingBox(double& OutWest, double& OutSouth, double& OutEast, double& OutNorth) const;
	int32 GetMinLevel() const { return MinLevel; }
	int32 GetMaxLevel() const { return MaxLevel; }
	int64 GetIndexedTileCount() const;

	/**
	 * La tile d'acqua che riguarda una tile di terreno. Falso = qui non c'e'
	 * acqua (non si aspetta niente). Vero = OutWaterKey e' la tile da usare:
	 * la stessa chiave, o un antenato al livello piu' fine del dataset.
	 */
	bool ResolveWaterKey(const GeoWorld::Tiles::FTileKey& TerrainKey, GeoWorld::Tiles::FTileKey& OutWaterKey) const;

	bool TileExists(const GeoWorld::Tiles::FTileKey& Key) const;
	FString GetTileFilePath(const GeoWorld::Tiles::FTileKey& Key) const;

	/** N del geoide (quota ellissoidica del livello del mare) in un punto, se il dataset lo copre. */
	bool SampleGeoid(double LonDeg, double LatDeg, double& OutUndulation) const { return Geoid.Sample(LonDeg, LatDeg, OutUndulation); }
	bool HasGeoid() const { return Geoid.IsValid(); }

private:
	const FLevelInfo* FindLevel(int32 Level) const;

	FString RootDirectory;
	FString DatasetName;
	bool bIsOpen = false;
	double West = 0.0, South = 0.0, East = 0.0, North = 0.0;
	int32 MinLevel = -1, MaxLevel = -1;
	TArray<FLevelInfo> Levels;
	GeoWorld::Tiles::FGeoidGrid Geoid;
};

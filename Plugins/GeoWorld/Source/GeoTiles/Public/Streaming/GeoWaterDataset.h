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
//  - Porta il MARE APERTO (opensea.bin): le tile che il terreno non ha e che
//    sono tutte mare. Il runtime ci costruisce terreno e acqua virtuali
//    (Tiles/OpenSea.h), cosi' al largo non c'e' un buco.
// =============================================================================
#pragma once

#include "CoreMinimal.h"

#include "Tiles/TileKey.h"
#include "Tiles/WaterTileFormat.h"

/**
 * Il mare aperto con il suo geoide: dati IMMUTABILI condivisi con i thread
 * che costruiscono le tile virtuali, che possono finire dopo una riapertura.
 */
struct FGeoOpenSeaData
{
	GeoWorld::Tiles::FOpenSea Sea;
	GeoWorld::Tiles::FGeoidGrid Geoid;
};
using FGeoOpenSeaPtr = TSharedPtr<const FGeoOpenSeaData, ESPMode::ThreadSafe>;

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

	/** Le regole della pipeline che l'ha fatto ("rulesVersion" nel manifest; 0 = non lo dice). */
	int32 GetRulesVersion() const { return RulesVersion; }

	/**
	 * Vuota se il dataset e' fatto con le regole di adesso (WaterRulesVersion);
	 * altrimenti la frase da dire in rosso, con la cartella e il comando per
	 * rifarlo. Un dataset vecchio si apre lo stesso: e' il dato a essere
	 * sbagliato, e vederlo aiuta a capire che e' lui.
	 */
	FString GetStaleReason() const;

	/** Il mare aperto (nullptr se il dataset non ce l'ha: fatto senza --sea). */
	FGeoOpenSeaPtr GetOpenSea() const { return OpenSea; }
	/** La tile, o un suo antenato, e' mare aperto (niente terreno, tutta acqua). */
	bool IsOpenSea(const GeoWorld::Tiles::FTileKey& Key) const { return OpenSea.IsValid() && OpenSea->Sea.Covers(Key); }

	/** N del geoide (quota ellissoidica del livello del mare) in un punto, se il dataset lo copre. */
	bool SampleGeoid(double LonDeg, double LatDeg, double& OutUndulation) const { return Geoid.Sample(LonDeg, LatDeg, OutUndulation); }
	bool HasGeoid() const { return Geoid.IsValid(); }

private:
	const FLevelInfo* FindLevel(int32 Level) const;
	void LoadOpenSea();

	FString RootDirectory;
	FString DatasetName;
	bool bIsOpen = false;
	double West = 0.0, South = 0.0, East = 0.0, North = 0.0;
	int32 MinLevel = -1, MaxLevel = -1;
	int32 RulesVersion = 0;
	TArray<FLevelInfo> Levels;
	GeoWorld::Tiles::FGeoidGrid Geoid;
	FGeoOpenSeaPtr OpenSea;
};

// =============================================================================
//  TileFormat.h -- Lettura del formato .ght e degli indici .bin.
//  STRATO: C++ PURO. Gemello di Pipeline/geoworld/tileformat.py e manifest.py.
// =============================================================================
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "Tiles/TileKey.h"
#include "Tiles/TilingScheme.h"

namespace GeoWorld::Tiles
{
	inline constexpr uint32_t TileMagic = 0x54485747u;   // "GWHT" little-endian
	inline constexpr uint32_t IndexMagic = 0x58495747u;  // "GWIX" little-endian
	inline constexpr uint16_t TileFormatVersion = 1;
	inline constexpr uint16_t IndexFormatVersion = 1;
	inline constexpr size_t TileHeaderBytes = 32;
	inline constexpr size_t IndexHeaderBytes = 16;
	inline constexpr size_t IndexRecordBytes = 16;

	inline constexpr uint16_t TileFlagHasFilledPosts = 1u << 0;

	inline constexpr size_t TileBytes =
		TileHeaderBytes + static_cast<size_t>(TilePosts) * TilePosts * sizeof(float);

	/**
	 * Una tile decodificata.
	 *
	 * Heights e' 129*129 float in metri ELLISSOIDICI, riga 0 a nord, colonna 0
	 * a ovest. Sono float e non double di proposito: qui siamo gia' dentro il
	 * dominio dove i valori sono piccoli (quote terrestri, non coordinate
	 * planetarie) e raddoppiare la memoria della cache non comprerebbe niente.
	 */
	struct FHeightTile
	{
		FTileKey Key;
		float MinHeight = 0.0f;
		float MaxHeight = 0.0f;
		bool bHasFilledPosts = false;
		std::vector<float> Heights;

		size_t GetByteSize() const
		{
			return sizeof(FHeightTile) + Heights.size() * sizeof(float);
		}

		float GetHeight(int32_t I, int32_t J) const
		{
			return Heights[static_cast<size_t>(J) * TilePosts + I];
		}

		bool IsValid() const
		{
			return Heights.size() == static_cast<size_t>(TilePosts) * TilePosts;
		}
	};

	/** Una voce dell'indice di livello: dice che la tile esiste e che quote ha. */
	struct FTileIndexEntry
	{
		uint32_t X = 0, Y = 0;
		float MinHeight = 0.0f, MaxHeight = 0.0f;
	};

	/**
	 * Decodifica una tile. Ritorna false e riempie OutError su QUALUNQUE
	 * incoerenza: magic, versione, dimensioni, chiave.
	 *
	 * ExpectedKey serve a intercettare il caso che capita davvero: una piramide
	 * rigenerata con parametri diversi che lascia sul disco i file vecchi. Senza
	 * questo controllo il runtime carica dati vecchi credendoli nuovi e il
	 * terreno risulta sottilmente sbagliato, che e' il tipo di bug che costa
	 * giorni.
	 */
	bool DecodeTile(const uint8_t* Data, size_t Size, const FTileKey& ExpectedKey,
	                FHeightTile& OutTile, std::string& OutError);

	/** Decodifica un indice di livello (<level>/index.bin). */
	bool DecodeLevelIndex(const uint8_t* Data, size_t Size, uint32_t ExpectedLevel,
	                      std::vector<FTileIndexEntry>& OutEntries, std::string& OutError);

	/** Percorso relativo alla radice del dataset: "<level>/<x>/<y>.ght". */
	std::string GetTileRelativePath(const FTileKey& Key);
}

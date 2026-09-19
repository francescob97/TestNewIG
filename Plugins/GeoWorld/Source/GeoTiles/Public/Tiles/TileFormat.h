// =============================================================================
//  TileFormat.h -- Lettura del formato .ght e degli indici .bin.
//  STRATO: C++ PURO. Gemello di Pipeline/geoworld/tileformat.py e manifest.py.
// =============================================================================
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include <cmath>
#include <cstring>

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

	// ======================================================================
	//  IMPLEMENTAZIONE
	//
	//  Lo strato puro e' HEADER-ONLY, e non per stile: in Unreal ogni modulo
	//  e' una DLL, e un simbolo definito in un .cpp non e' visibile agli altri
	//  moduli se non viene esportato con la macro API del modulo. Esportarlo
	//  significherebbe pero' mettere una macro del motore dentro lo strato che
	//  per definizione non deve sapere di stare dentro Unreal: proprio la
	//  perdita che la separazione in due strati esiste per evitare.
	//  Header-only risolve alla radice, e su funzioni matematiche di poche
	//  righe non costa niente.
	// ======================================================================

	namespace Detail
	{
		// Letture little-endian esplicite invece di un memcpy della struct.
		//
		// PERCHE': il formato su disco e' little-endian per definizione, e
		// leggerlo byte per byte lo rende indipendente dall'ordinamento della
		// macchina e, soprattutto, dal PADDING che il compilatore inserisce
		// nelle struct. Mappare una struct C++ su un buffer di file funziona
		// finche' non cambia compilatore o architettura, e poi smette di
		// funzionare in modo difficilissimo da diagnosticare.
		inline uint16_t ReadU16(const uint8_t* P) { return uint16_t(P[0]) | (uint16_t(P[1]) << 8); }
		inline uint32_t ReadU32(const uint8_t* P)
		{
			return uint32_t(P[0]) | (uint32_t(P[1]) << 8)
			     | (uint32_t(P[2]) << 16) | (uint32_t(P[3]) << 24);
		}
		inline float ReadF32(const uint8_t* P)
		{
			const uint32_t Bits = ReadU32(P);
			float Value;
			std::memcpy(&Value, &Bits, sizeof(Value));
			return Value;
		}
	}

	inline bool DecodeTile(const uint8_t* Data, size_t Size, const FTileKey& ExpectedKey,
	                FHeightTile& OutTile, std::string& OutError)
	{
		if (!Data || Size < TileHeaderBytes)
		{
			OutError = "file troncato: piu' corto dell'header";
			return false;
		}

		if (Detail::ReadU32(Data) != TileMagic)
		{
			OutError = "magic errato: non e' una tile GeoWorld";
			return false;
		}

		const uint16_t Version = Detail::ReadU16(Data + 4);
		if (Version != TileFormatVersion)
		{
			OutError = "versione del formato " + std::to_string(Version)
			         + ", questo codice legge la " + std::to_string(TileFormatVersion);
			return false;
		}

		const uint16_t Flags = Detail::ReadU16(Data + 6);
		const FTileKey Key{ Detail::ReadU32(Data + 8), Detail::ReadU32(Data + 12), Detail::ReadU32(Data + 16) };
		const uint16_t Width = Detail::ReadU16(Data + 20);
		const uint16_t Height = Detail::ReadU16(Data + 22);

		if (Key != ExpectedKey)
		{
			OutError = "la tile dichiara " + std::to_string(Key.Level) + "/"
			         + std::to_string(Key.X) + "/" + std::to_string(Key.Y)
			         + " ma ne era attesa un'altra: dataset incoerente sul disco";
			return false;
		}

		if (Width != TilePosts || Height != TilePosts)
		{
			OutError = "dimensione " + std::to_string(Width) + "x" + std::to_string(Height)
			         + ", attesa " + std::to_string(TilePosts) + "x" + std::to_string(TilePosts);
			return false;
		}

		const size_t PostCount = static_cast<size_t>(Width) * Height;
		if (Size != TileHeaderBytes + PostCount * sizeof(float))
		{
			OutError = "dimensione del file incoerente con l'header";
			return false;
		}

		OutTile.Key = Key;
		OutTile.MinHeight = Detail::ReadF32(Data + 24);
		OutTile.MaxHeight = Detail::ReadF32(Data + 28);
		OutTile.bHasFilledPosts = (Flags & TileFlagHasFilledPosts) != 0;

		OutTile.Heights.resize(PostCount);
		const uint8_t* Cursor = Data + TileHeaderBytes;
		for (size_t Index = 0; Index < PostCount; ++Index, Cursor += 4)
		{
			OutTile.Heights[Index] = Detail::ReadF32(Cursor);
		}

		// Un NaN qui significa che la pipeline ha scritto un post non riempito.
		// Lasciarlo passare produrrebbe triangoli degeneri in Fase 5, molto piu'
		// difficili da ricondurre alla causa di un errore in caricamento.
		for (float Value : OutTile.Heights)
		{
			if (!std::isfinite(Value))
			{
				OutError = "la tile contiene valori non finiti";
				return false;
			}
		}

		return true;
	}

	inline bool DecodeLevelIndex(const uint8_t* Data, size_t Size, uint32_t ExpectedLevel,
	                      std::vector<FTileIndexEntry>& OutEntries, std::string& OutError)
	{
		if (!Data || Size < IndexHeaderBytes)
		{
			OutError = "indice troncato";
			return false;
		}
		if (Detail::ReadU32(Data) != IndexMagic)
		{
			OutError = "magic dell'indice errato";
			return false;
		}
		if (Detail::ReadU16(Data + 4) != IndexFormatVersion)
		{
			OutError = "versione dell'indice non supportata";
			return false;
		}

		const uint32_t Level = Detail::ReadU32(Data + 8);
		if (Level != ExpectedLevel)
		{
			OutError = "l'indice dichiara il livello " + std::to_string(Level)
			         + ", atteso " + std::to_string(ExpectedLevel);
			return false;
		}

		const uint32_t Count = Detail::ReadU32(Data + 12);
		if (Size != IndexHeaderBytes + static_cast<size_t>(Count) * IndexRecordBytes)
		{
			OutError = "dimensione dell'indice incoerente con il numero di voci";
			return false;
		}

		OutEntries.clear();
		OutEntries.reserve(Count);
		const uint8_t* Cursor = Data + IndexHeaderBytes;
		for (uint32_t Index = 0; Index < Count; ++Index, Cursor += IndexRecordBytes)
		{
			OutEntries.push_back(FTileIndexEntry{
				Detail::ReadU32(Cursor), Detail::ReadU32(Cursor + 4),
				Detail::ReadF32(Cursor + 8), Detail::ReadF32(Cursor + 12) });
		}
		return true;
	}

	inline std::string GetTileRelativePath(const FTileKey& Key)
	{
		return std::to_string(Key.Level) + "/" + std::to_string(Key.X) + "/"
		     + std::to_string(Key.Y) + ".ght";
	}

}

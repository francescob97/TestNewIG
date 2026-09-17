#include "Tiles/TileFormat.h"

#include <cstring>
#include <cmath>

namespace GeoWorld::Tiles
{
	namespace
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

	bool DecodeTile(const uint8_t* Data, size_t Size, const FTileKey& ExpectedKey,
	                FHeightTile& OutTile, std::string& OutError)
	{
		if (!Data || Size < TileHeaderBytes)
		{
			OutError = "file troncato: piu' corto dell'header";
			return false;
		}

		if (ReadU32(Data) != TileMagic)
		{
			OutError = "magic errato: non e' una tile GeoWorld";
			return false;
		}

		const uint16_t Version = ReadU16(Data + 4);
		if (Version != TileFormatVersion)
		{
			OutError = "versione del formato " + std::to_string(Version)
			         + ", questo codice legge la " + std::to_string(TileFormatVersion);
			return false;
		}

		const uint16_t Flags = ReadU16(Data + 6);
		const FTileKey Key{ ReadU32(Data + 8), ReadU32(Data + 12), ReadU32(Data + 16) };
		const uint16_t Width = ReadU16(Data + 20);
		const uint16_t Height = ReadU16(Data + 22);

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
		OutTile.MinHeight = ReadF32(Data + 24);
		OutTile.MaxHeight = ReadF32(Data + 28);
		OutTile.bHasFilledPosts = (Flags & TileFlagHasFilledPosts) != 0;

		OutTile.Heights.resize(PostCount);
		const uint8_t* Cursor = Data + TileHeaderBytes;
		for (size_t Index = 0; Index < PostCount; ++Index, Cursor += 4)
		{
			OutTile.Heights[Index] = ReadF32(Cursor);
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

	bool DecodeLevelIndex(const uint8_t* Data, size_t Size, uint32_t ExpectedLevel,
	                      std::vector<FTileIndexEntry>& OutEntries, std::string& OutError)
	{
		if (!Data || Size < IndexHeaderBytes)
		{
			OutError = "indice troncato";
			return false;
		}
		if (ReadU32(Data) != IndexMagic)
		{
			OutError = "magic dell'indice errato";
			return false;
		}
		if (ReadU16(Data + 4) != IndexFormatVersion)
		{
			OutError = "versione dell'indice non supportata";
			return false;
		}

		const uint32_t Level = ReadU32(Data + 8);
		if (Level != ExpectedLevel)
		{
			OutError = "l'indice dichiara il livello " + std::to_string(Level)
			         + ", atteso " + std::to_string(ExpectedLevel);
			return false;
		}

		const uint32_t Count = ReadU32(Data + 12);
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
				ReadU32(Cursor), ReadU32(Cursor + 4),
				ReadF32(Cursor + 8), ReadF32(Cursor + 12) });
		}
		return true;
	}

	std::string GetTileRelativePath(const FTileKey& Key)
	{
		return std::to_string(Key.Level) + "/" + std::to_string(Key.X) + "/"
		     + std::to_string(Key.Y) + ".ght";
	}
}

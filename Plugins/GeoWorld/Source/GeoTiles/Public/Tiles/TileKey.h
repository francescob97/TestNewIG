// =============================================================================
//  TileKey.h -- Identita' di una tile. STRATO: C++ PURO (niente Unreal).
// =============================================================================
#pragma once

#include <cstdint>
#include <functional>

namespace GeoWorld::Tiles
{
	/**
	 * Identifica una tile nella piramide: livello + coordinate.
	 *
	 * Sta in 12 byte ed e' copiabile: viene passata per valore ovunque, anche
	 * attraverso il confine fra thread. Non contiene puntatori ne' allocazioni
	 * proprio perche' deve poter attraversare quel confine senza precauzioni.
	 */
	struct FTileKey
	{
		uint32_t Level = 0;
		uint32_t X = 0;
		uint32_t Y = 0;

		bool operator==(const FTileKey& Other) const
		{
			return Level == Other.Level && X == Other.X && Y == Other.Y;
		}
		bool operator!=(const FTileKey& Other) const { return !(*this == Other); }

		/** Il padre nella piramide. Al livello 0 non esiste: si ritorna se stessi. */
		FTileKey GetParent() const
		{
			return (Level == 0) ? *this : FTileKey{ Level - 1, X / 2, Y / 2 };
		}

		/** I figli sono (2x, 2y) + (0..1, 0..1). Indice 0..3 in ordine NO, NE, SO, SE. */
		FTileKey GetChild(int Index) const
		{
			return FTileKey{ Level + 1, X * 2 + static_cast<uint32_t>(Index & 1),
			                 Y * 2 + static_cast<uint32_t>((Index >> 1) & 1) };
		}

		/** Ordinamento totale: serve per usarla come chiave in mappe ordinate. */
		bool operator<(const FTileKey& Other) const
		{
			if (Level != Other.Level) return Level < Other.Level;
			if (Y != Other.Y)         return Y < Other.Y;
			return X < Other.X;
		}

		uint64_t ToHash() const
		{
			// Mescolamento a 64 bit (splitmix). Le chiavi consecutive differiscono
			// di 1 in X: senza mescolare, finirebbero tutte nello stesso bucket.
			uint64_t Value = (static_cast<uint64_t>(Level) << 58)
			               ^ (static_cast<uint64_t>(Y) << 29)
			               ^ static_cast<uint64_t>(X);
			Value ^= Value >> 30; Value *= 0xbf58476d1ce4e5b9ULL;
			Value ^= Value >> 27; Value *= 0x94d049bb133111ebULL;
			Value ^= Value >> 31;
			return Value;
		}
	};
}

namespace std
{
	template <> struct hash<GeoWorld::Tiles::FTileKey>
	{
		size_t operator()(const GeoWorld::Tiles::FTileKey& Key) const noexcept
		{
			return static_cast<size_t>(Key.ToHash());
		}
	};
}

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

		/**
		 * Impacchetta la chiave in un intero a 64 bit, per usarla come chiave di
		 * una TMap di Unreal (che non sa hashare una FTileKey da sola).
		 *
		 * PERCHE' STA QUI E NON NEI SINGOLI .CPP. Perche' c'era in cinque file
		 * diversi, ognuno con la propria copia in un namespace anonimo, e questo
		 * ha prodotto un errore di compilazione che qui non si poteva vedere:
		 * Unreal usa le UNITY BUILD, cioe' incolla piu' .cpp dello stesso modulo
		 * in una sola unita' di traduzione. Due namespace anonimi di file
		 * diversi diventano allora LO STESSO namespace, e due funzioni omonime
		 * si scontrano. Una definizione sola elimina il problema alla radice.
		 *
		 * NON E' INVERTIBILE, ed e' importante ricordarlo: uno XOR perde
		 * informazione. Chi ha bisogno di risalire alla tile deve conservare la
		 * FTileKey intera -- dimenticarlo e' gia' costato due bug, uno nella
		 * rimozione delle mesh e uno nella diagnostica.
		 *
		 * L'impacchettamento e' iniettivo fino al livello 20 (il massimo
		 * supportato): li' X sta in 21 bit, Y in 20, e i campi non si
		 * sovrappongono.
		 */
		uint64_t Pack() const
		{
			return (static_cast<uint64_t>(Level) << 58)
			     ^ (static_cast<uint64_t>(Y) << 29)
			     ^ static_cast<uint64_t>(X);
		}

		/** Impacchetta solo X e Y: per le mappe di un singolo livello. */
		static uint64_t PackXY(uint32_t InX, uint32_t InY)
		{
			return (static_cast<uint64_t>(InY) << 32) | static_cast<uint64_t>(InX);
		}

		uint64_t ToHash() const
		{
			// Mescolamento a 64 bit (splitmix). Le chiavi consecutive differiscono
			// di 1 in X: senza mescolare, finirebbero tutte nello stesso bucket.
			uint64_t Value = Pack();
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

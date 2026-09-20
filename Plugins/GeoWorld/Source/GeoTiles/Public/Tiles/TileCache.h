// =============================================================================
//  TileCache.h -- Cache LRU con budget in byte. STRATO: C++ PURO.
//
//  PERCHE' UN BUDGET IN BYTE E NON IN NUMERO DI TILE
//  Le tile di quota sono tutte grandi uguale (66.596 byte), quindi contarle
//  sarebbe equivalente. Ma dalla Fase 6 arrivano le tile di imagery, che sono
//  compresse e quindi di dimensione variabile: un limite a numero di elementi
//  diventerebbe un limite a memoria imprevedibile. Meglio fissare adesso la
//  semantica giusta, che e' anche quella che l'utente vuole configurare
//  ("usa al massimo 512 MB") invece di un conteggio senza significato.
//
//  PERCHE' NON E' THREAD-SAFE
//  Di proposito. La cache vive sul GAME THREAD e basta. I worker thread non la
//  toccano mai: producono dati e li consegnano via coda. Rendere thread-safe
//  una struttura che non ne ha bisogno significa pagare un lock su ogni
//  accesso del percorso caldo (la selezione LOD interroga la cache migliaia di
//  volte per frame) per una concorrenza che non esiste.
// =============================================================================
#pragma once

#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <utility>
#include <unordered_map>

#include "Tiles/TileFormat.h"
#include "Tiles/TileKey.h"

namespace GeoWorld::Tiles
{
	struct FCacheStats
	{
		uint64_t Hits = 0;
		uint64_t Misses = 0;
		uint64_t Evictions = 0;
		size_t UsedBytes = 0;
		size_t BudgetBytes = 0;
		size_t ResidentTiles = 0;
		size_t PinnedTiles = 0;

		double GetHitRate() const
		{
			const uint64_t Total = Hits + Misses;
			return (Total == 0) ? 0.0 : static_cast<double>(Hits) / static_cast<double>(Total);
		}
		double GetUsedMegabytes() const { return static_cast<double>(UsedBytes) / (1024.0 * 1024.0); }
	};

	/**
	 * Cache LRU di tile decodificate.
	 *
	 * Le tile si consegnano come shared_ptr: chi le usa (il quadtree, la mesh)
	 * puo' tenerne un riferimento e continuare a lavorarci anche se nel
	 * frattempo la cache le sfratta. Senza, uno sfratto durante la generazione
	 * di una mesh lascerebbe un puntatore pendente.
	 */
	/**
	 * =====================================================================
	 *  PERCHE' UN TEMPLATE (dalla Fase 6)
	 * =====================================================================
	 *  Fino alla Fase 5 questa classe conteneva solo FHeightTile. Con le
	 *  ortofoto serve la stessa identica cache per un payload diverso, e le
	 *  alternative erano tre:
	 *
	 *    - duplicare il file: due copie che divergono al primo bug corretto
	 *      in una sola delle due;
	 *    - un'interfaccia virtuale sul payload: una chiamata indiretta per
	 *      ogni accesso, in un percorso che la selezione LOD attraversa
	 *      migliaia di volte per frame;
	 *    - un template.
	 *
	 *  Il template non costa niente a runtime e qui non costa niente nemmeno
	 *  in leggibilita', perche' il codice della cache non guarda MAI dentro
	 *  il payload: gli chiede solo GetByteSize(). E' l'unico requisito.
	 *
	 *  FTileCache resta come alias, cosi' tutto il codice delle Fasi 3-5
	 *  continua a compilare senza una modifica.
	 */
	template <typename PayloadType>
	class TTileCache
	{
	public:
		using FTilePtr = std::shared_ptr<const PayloadType>;

		explicit TTileCache(size_t BudgetBytes = 256ull * 1024 * 1024)
			: Budget(BudgetBytes) {}

		/** Cerca senza modificare l'ordine LRU. Per statistiche e test. */
		FTilePtr Peek(const FTileKey& Key) const
		{
			const auto Found = Lookup.find(Key);
			return (Found == Lookup.end()) ? nullptr : Found->second->Tile;
		}

		/** Cerca e segna come usata di recente. E' l'accesso normale. */
		FTilePtr Find(const FTileKey& Key)
		{
			const auto Found = Lookup.find(Key);
			if (Found == Lookup.end())
			{
				++Stats.Misses;
				return nullptr;
			}
			++Stats.Hits;
			// splice sposta il nodo in testa senza allocare ne' copiare: e' il
			// motivo per cui si usa una std::list e non un vector.
			Entries.splice(Entries.begin(), Entries, Found->second);
			return Found->second->Tile;
		}

		/** Inserisce (o sostituisce) e sfratta finche' si rientra nel budget. */
		void Insert(const FTileKey& Key, FTilePtr Tile)
		{
			if (!Tile) { return; }

			Remove(Key);

			const size_t Size = Tile->GetByteSize();
			Entries.push_front(FEntry{ Key, std::move(Tile), Size, false });
			Lookup[Key] = Entries.begin();
			UsedBytes += Size;

			EvictToBudget();
		}

		bool Remove(const FTileKey& Key)
		{
			const auto Found = Lookup.find(Key);
			if (Found == Lookup.end()) { return false; }
			UsedBytes -= Found->second->SizeBytes;
			Entries.erase(Found->second);
			Lookup.erase(Found);
			return true;
		}

		/**
		 * "Pinnare" una tile la esclude dallo sfratto.
		 *
		 * Serve per le tile in uso ADESSO: quelle da cui il quadtree sta
		 * generando una mesh, e le radici della piramide, che se sfrattate
		 * costringerebbero a ricaricare da disco proprio l'unica cosa che serve
		 * sempre. Senza pin, una scorribanda della camera che riempie la cache
		 * potrebbe buttare fuori le tile che si stanno disegnando.
		 */
		void SetPinned(const FTileKey& Key, bool bPinned)
		{
			const auto Found = Lookup.find(Key);
			if (Found != Lookup.end()) { Found->second->bPinned = bPinned; }
		}

		void SetBudgetBytes(size_t NewBudget)
		{
			Budget = NewBudget;
			EvictToBudget();
		}

		void Clear()
		{
			Entries.clear();
			Lookup.clear();
			UsedBytes = 0;
		}

		FCacheStats GetStats() const
		{
			FCacheStats Result = Stats;
			Result.UsedBytes = UsedBytes;
			Result.BudgetBytes = Budget;
			Result.ResidentTiles = Lookup.size();
			Result.PinnedTiles = 0;
			for (const FEntry& Entry : Entries)
			{
				if (Entry.bPinned) { ++Result.PinnedTiles; }
			}
			return Result;
		}

		void ResetStatistics() { Stats.Hits = Stats.Misses = Stats.Evictions = 0; }

		/**
		 * Scorre le tile residenti, dalla piu' usata di recente alla meno.
		 *
		 * Serve al disegno di debug e alle statistiche. NON aggiorna l'ordine
		 * LRU di proposito: ispezionare la cache non deve cambiarne il
		 * comportamento, altrimenti accendere l'overlay modificherebbe quali
		 * tile vengono sfrattate e si finirebbe per osservare un sistema
		 * diverso da quello che si voleva osservare.
		 */
		template <typename FuncType>
		void ForEachResident(FuncType&& Function) const
		{
			for (const FEntry& Entry : Entries)
			{
				Function(Entry.Key, Entry.Tile, Entry.bPinned);
			}
		}

	private:
		struct FEntry
		{
			FTileKey Key;
			FTilePtr Tile;
			size_t SizeBytes = 0;
			bool bPinned = false;
		};

		void EvictToBudget()
		{
			// Si sfratta dalla coda, cioe' dalla tile usata meno di recente,
			// saltando quelle pinnate. Se fossero tutte pinnate il ciclo
			// finirebbe senza liberare niente: e' voluto, meglio sforare il
			// budget che buttare via una tile in uso.
			auto It = Entries.end();
			while (UsedBytes > Budget && It != Entries.begin())
			{
				--It;
				if (It->bPinned) { continue; }

				UsedBytes -= It->SizeBytes;
				Lookup.erase(It->Key);
				++Stats.Evictions;
				It = Entries.erase(It);
			}
		}

		std::list<FEntry> Entries;   // testa = usata di recente

		// NOTA C++: dentro un template, std::list<FEntry>::iterator dipende dal
		// parametro (FEntry contiene PayloadType) e il compilatore non sa da
		// solo che sia un TIPO: senza "typename" lo interpreta come un valore e
		// produce una cascata di errori che sembrano venire da tutta la classe.
		// E' l'unica riga che il passaggio a template ha richiesto di cambiare.
		using FEntryIterator = typename std::list<FEntry>::iterator;
		std::unordered_map<FTileKey, FEntryIterator> Lookup;   // O(1)
		size_t Budget = 0;
		size_t UsedBytes = 0;
		FCacheStats Stats;
	};

	/** La cache delle quote: il nome usato dalle Fasi 3, 4 e 5. */
	using FTileCache = TTileCache<FHeightTile>;

}

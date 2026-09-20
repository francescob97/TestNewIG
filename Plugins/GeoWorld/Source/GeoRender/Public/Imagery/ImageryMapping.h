// =============================================================================
//  ImageryMapping.h -- Quale immagine usare per una tile di terreno, e quale
//  pezzo di essa. STRATO: C++ PURO, header-only.
//
//  E' il cuore del drappeggio, ed e' tutto qui: una ventina di righe di
//  aritmetica intera. La versione di riferimento sta in Python, in
//  geoworld/imagecut.py, e i due risultati devono coincidere: i test di
//  entrambi i lati usano gli stessi esempi.
// =============================================================================
#pragma once

#include <cstdint>

#include "Tiles/TileKey.h"

namespace GeoWorld::Imagery
{
	using Tiles::FTileKey;

	/**
	 * Come campionare la texture di una tile di immagine per vestire una tile
	 * di terreno.
	 *
	 *     uv_texture = Offset + uv_mesh * Scale
	 *
	 * Nel caso normale -- immagine allo stesso livello del terreno -- Offset e'
	 * zero e Scale e' uno, e la formula non fa niente. Non e' un caso
	 * particolare da trattare a parte: e' la stessa formula che degenera.
	 */
	struct FDrapeTransform
	{
		FTileKey ImageKey;
		float OffsetU = 0.0f;
		float OffsetV = 0.0f;
		float Scale = 1.0f;

		bool IsIdentity() const { return Scale >= 1.0f; }
	};

	/** Tile antenata di livello `AncestorLevel` che contiene `Key`. */
	inline FTileKey AncestorOf(const FTileKey& Key, uint32_t AncestorLevel)
	{
		if (AncestorLevel >= Key.Level)
		{
			return Key;
		}
		const uint32_t Shift = Key.Level - AncestorLevel;
		return FTileKey{ AncestorLevel, Key.X >> Shift, Key.Y >> Shift };
	}

	/**
	 * Calcola il ritaglio.
	 *
	 * PERCHE' IN ARITMETICA INTERA. Gli offset sono sempre multipli esatti di
	 * 1/2^d, cioe' numeri che il float rappresenta senza errore. Calcolarli
	 * invece dai rettangoli geografici in gradi -- sottrazioni fra numeri
	 * grandi e simili -- darebbe valori quasi giusti, e "quasi" al bordo di una
	 * texture significa mezza riga di pixel presi dalla tile sbagliata.
	 */
	inline FDrapeTransform MakeDrapeTransform(const FTileKey& TerrainKey, uint32_t ImageLevel)
	{
		FDrapeTransform Result;

		if (ImageLevel >= TerrainKey.Level)
		{
			// L'immagine e' fine quanto il terreno o di piu': si usa quella allo
			// stesso livello, che copre lo stesso identico rettangolo.
			Result.ImageKey = TerrainKey;
			return Result;
		}

		const uint32_t Shift = TerrainKey.Level - ImageLevel;
		const uint32_t Divisions = 1u << Shift;
		const float Scale = 1.0f / static_cast<float>(Divisions);

		Result.ImageKey = FTileKey{ ImageLevel, TerrainKey.X >> Shift, TerrainKey.Y >> Shift };
		Result.OffsetU = static_cast<float>(TerrainKey.X & (Divisions - 1)) * Scale;
		Result.OffsetV = static_cast<float>(TerrainKey.Y & (Divisions - 1)) * Scale;
		Result.Scale = Scale;
		return Result;
	}

	/**
	 * Sa dire se una tile di immagine e' disponibile subito.
	 *
	 * Stessa forma di ITileAvailability della Fase 4, e per la stessa ragione:
	 * la matematica non deve sapere niente ne' della cache ne' del disco, cosi'
	 * i test le possono dare una disponibilita' finta e verificare le scelte.
	 */
	class IImageAvailability
	{
	public:
		virtual ~IImageAvailability() = default;

		/** La tile esiste nel dataset? (dall'indice, senza leggere il disco) */
		virtual bool ImageExists(const FTileKey& Key) const = 0;

		/** La tile e' gia' decodificata e pronta da usare? */
		virtual bool ImageIsResident(const FTileKey& Key) const = 0;
	};

	/**
	 * Sceglie la migliore immagine DISPONIBILE ORA per una tile di terreno.
	 *
	 * LA REGOLA, e perche' e' questa. Si parte dal livello del terreno (limitato
	 * al livello massimo del dataset di immagini) e si scende verso la radice
	 * finche' non si trova una tile gia' residente.
	 *
	 * Non si aspetta mai. E' la stessa scelta della Fase 4 sui buchi del
	 * quadtree: una texture grossolana adesso e' meglio di quella giusta fra tre
	 * frame, perche' l'alternativa non e' "aspettare", e' "far lampeggiare il
	 * grigio". Man mano che le tile arrivano, il drappeggio si affina da solo.
	 *
	 * `OutRequest` riceve la tile che sarebbe quella giusta, perche' il
	 * chiamante possa chiederla allo streaming: e' cosi' che il miglioramento
	 * accade davvero invece di restare grossolano per sempre.
	 */
	inline bool ChooseDrape(const FTileKey& TerrainKey,
	                        uint32_t ImageryMinLevel, uint32_t ImageryMaxLevel,
	                        const IImageAvailability& Availability,
	                        FDrapeTransform& OutDrape, FTileKey& OutRequest,
	                        bool& bOutHasRequest)
	{
		bOutHasRequest = false;

		const uint32_t Target = (TerrainKey.Level < ImageryMaxLevel)
			? TerrainKey.Level : ImageryMaxLevel;

		// La tile ideale: quella che si vorrebbe avere.
		const FTileKey Ideal = AncestorOf(TerrainKey, Target);
		if (Availability.ImageExists(Ideal) && !Availability.ImageIsResident(Ideal))
		{
			OutRequest = Ideal;
			bOutHasRequest = true;
		}

		// Si scende verso la radice cercando la prima gia' in memoria. Il
		// confronto e' con +1 e non con >= perche' Level e' senza segno e
		// ImageryMinLevel puo' essere zero.
		for (uint32_t Level = Target + 1; Level > ImageryMinLevel; --Level)
		{
			const FTileKey Candidate = AncestorOf(TerrainKey, Level - 1);
			if (Availability.ImageIsResident(Candidate))
			{
				OutDrape = MakeDrapeTransform(TerrainKey, Level - 1);
				OutDrape.ImageKey = Candidate;
				return true;
			}
		}

		return false;
	}
}

// =============================================================================
//  QuadtreeTypes.h -- Tipi della selezione LOD. STRATO: C++ PURO.
// =============================================================================
#pragma once

#include <cstdint>
#include <vector>

#include "Geo/Ellipsoid.h"
#include "Tiles/TileKey.h"

namespace GeoWorld::Quadtree
{
	using GeoWorld::Core::FEcef;
	using GeoWorld::Tiles::FTileKey;

	/**
	 * Volume di contenimento di una tile: una SFERA in ECEF.
	 *
	 * PERCHE' UNA SFERA E NON UNA SCATOLA ORIENTATA
	 * Una scatola orientata sarebbe piu' stretta e scarterebbe qualche tile in
	 * piu'. Ma il test sfera-contro-piano e' un prodotto scalare e un confronto,
	 * mentre scatola-contro-piano ne richiede otto o una proiezione sugli assi.
	 * Con decine di migliaia di nodi per frame la differenza si sente, e il
	 * guadagno di una scatola su una tile quasi quadrata e' modesto.
	 *
	 * La sfera e' costruita da 18 punti campione (3x3 sulla superficie, a quota
	 * minima e massima) e non dai soli otto spigoli: su una tile grande la
	 * superficie si INCURVA verso l'esterno rispetto agli spigoli, e una sfera
	 * costruita solo sugli spigoli lascerebbe fuori il rigonfiamento centrale.
	 */
	struct FTileBoundingVolume
	{
		FEcef Centre;
		double Radius = 0.0;

		/** Punti campione usati anche dal test dell'orizzonte. */
		FEcef Samples[18];
		int32_t SampleCount = 0;
	};

	/**
	 * Tutto cio' che serve per decidere, in ECEF e in double.
	 *
	 * Si tiene in ECEF e non in spazio Unreal di proposito: la selezione non
	 * deve dipendere da dove si trova l'origine in questo istante. Se dipendesse,
	 * un rebase cambierebbe l'insieme di tile selezionate a parita' di vista.
	 */
	struct FViewParameters
	{
		FEcef CameraEcef;

		// Base della camera in ECEF, ortonormale.
		FEcef Forward;
		FEcef Up;
		FEcef Right;

		double VerticalFovRad = 1.0;
		double AspectRatio = 1.777;
		double ScreenHeightPixels = 1080.0;
		double NearClipMetres = 1.0;

		/** Errore su schermo tollerato, in PIXEL. E' la manopola principale. */
		double MaxScreenSpaceError = 4.0;
	};

	/** Una tile scelta per il disegno. */
	struct FSelectedTile
	{
		FTileKey Key;
		double ScreenSpaceError = 0.0;
		double DistanceMetres = 0.0;
	};

	/** Una tile da chiedere al loader, con la sua urgenza. */
	struct FTileRequest
	{
		FTileKey Key;
		int32_t Priority = 0;
		double ScreenSpaceError = 0.0;
	};

	/** Esito di una passata di selezione. */
	struct FSelectionResult
	{
		std::vector<FSelectedTile> ToRender;
		std::vector<FTileRequest> ToLoad;

		int32_t NodesVisited = 0;
		int32_t CulledByFrustum = 0;
		int32_t CulledByHorizon = 0;
		int32_t CulledByMissing = 0;     // la tile non esiste nel dataset
		int32_t RefinedNodes = 0;
		double WorstScreenSpaceError = 0.0;

		void Reset()
		{
			ToRender.clear();
			ToLoad.clear();
			NodesVisited = CulledByFrustum = CulledByHorizon = 0;
			CulledByMissing = RefinedNodes = 0;
			WorstScreenSpaceError = 0.0;
		}
	};
}

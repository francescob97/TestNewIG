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
	using GeoWorld::Core::FEllipsoid;
	using GeoWorld::Core::FGeodetic;
	using GeoWorld::Core::GeodeticSurfaceNormal;
	using GeoWorld::Core::GeodeticToEcef;
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

		/**
		 * Quanto allargare il frustum PER LA SOLA SELEZIONE.
		 *
		 * =================================================================
		 *  PERCHE' UN MARGINE, E PERCHE' NON E' UN TRUCCO
		 * =================================================================
		 *  Con un frustum esatto, una tile viene chiesta allo streaming nel
		 *  momento in cui e' GIA' visibile. Ma fra la richiesta e il disegno
		 *  c'e' una lettura da disco, una decodifica e la costruzione della
		 *  mesh: diversi frame. Nel frattempo, al bordo dello schermo, non c'e'
		 *  niente da disegnare, e si vede il vuoto.
		 *
		 *  Ruotando la camera il bordo che entra e' sempre "appena chiesto", e
		 *  il vuoto lo segue: e' esattamente il bordo nero che si vede girando.
		 *
		 *  Il rimedio non e' caricare piu' in fretta, e' chiedere PRIMA. Un
		 *  frustum allargato del 20% seleziona una corona di tile appena fuori
		 *  dalla vista, che sono gia' pronte quando ci arrivi. Il renderer di
		 *  Unreal le scartera' comunque dal disegno: il costo e' qualche tile
		 *  in piu' in memoria, non un pixel in piu' a schermo.
		 *
		 *  1.0 disattiva il margine ed e' utile per una cosa sola: vedere il
		 *  problema che il margine risolve.
		 */
		double FrustumMarginFactor = 1.2;

		/**
		 * Scartare le tile fuori dal frustum durante la SELEZIONE?
		 *
		 * =================================================================
		 *  PERCHE' SI PUO' SPEGNERE, E PERCHE' IL MOTORE LO SPEGNE
		 * =================================================================
		 *  Con il frustum acceso, l'insieme di tile scelte dipende da DOVE SI
		 *  GUARDA. Girando la testa cambia la selezione, e ogni tile che entra
		 *  nel campo va letta, costruita e vestita mentre la si sta gia'
		 *  guardando: il bordo nero, gli scatti in rotazione, lo specchietto
		 *  retrovisore che non avrebbe niente da mostrare.
		 *
		 *  Spento, la selezione dipende SOLO dalla posizione: l'errore su
		 *  schermo e' funzione della distanza, e l'orizzonte e' funzione della
		 *  posizione. Ruotare la camera non cambia niente. A scartare cio' che
		 *  sta dietro ci pensa comunque il renderer di Unreal, che fa il suo
		 *  frustum culling per componente, sui bounds, a ogni frame: farlo
		 *  anche qui serviva solo a risparmiare memoria, e la memoria c'e'.
		 *
		 *  Il costo misurato (Roma, 3 km di quota, soglia 4 px): da ~230 a
		 *  ~630 tile, selezione sempre sotto il mezzo millisecondo.
		 *
		 *  Il default resta true perche' e' il comportamento della Fase 4 e i
		 *  suoi test lo presuppongono; il subsystem lo mette a false.
		 */
		bool bFrustumCulling = true;

		/**
		 * Quanto si tollera di piu', in errore su schermo, FUORI dalla vista.
		 *
		 * Con la selezione vista-indipendente le tile dietro la camera erano
		 * dettagliate quanto quelle davanti: tre volte la memoria per cose che
		 * nessuno guarda. Con un fattore 4 dietro si tengono tile ~4 volte
		 * piu' grossolane per lato (circa 16 volte meno triangoli): girando la
		 * testa c'e' SUBITO terreno (grossolano), e il dettaglio arriva in una
		 * frazione di secondo, perche' le mesh si costruiscono sui thread di
		 * lavoro. 1 = come prima, tutto uguale in ogni direzione.
		 *
		 * "Fuori dalla vista" si decide con il frustum allargato del margine
		 * (FrustumMarginFactor): il bordo dello schermo resta dettagliato.
		 * Vale solo se bFrustumCulling e' falso; altrimenti cio' che e' fuori
		 * non viene selezionato affatto.
		 */
		double OutOfViewErrorFactor = 1.0;

		/**
		 * Moltiplica l'errore geometrico di ogni livello. Serve quando la mesh
		 * di una tile NON usa tutti i suoi 129x129 post: con un post ogni 2,
		 * il passo della mesh e' il doppio, e l'errore che si commette pure.
		 * Senza questo fattore il LOD crederebbe di avere piu' dettaglio di
		 * quanto ne disegni davvero. Vedi FTileMeshParameters::Step.
		 */
		double GeometricErrorScale = 1.0;
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

// =============================================================================
//  TileSelector.h -- Attraversamento del quadtree e scelta del livello.
//  STRATO: C++ PURO.
// =============================================================================
#pragma once

#include "Quadtree/Culling.h"
#include "Quadtree/QuadtreeTypes.h"

namespace GeoWorld::Quadtree
{
	/**
	 * Cio' che il selettore ha bisogno di sapere sul dataset, senza conoscerlo.
	 *
	 * Tenere questa interfaccia astratta e' cio' che permette di testare la
	 * selezione con un dataset finto, deterministico, senza toccare il disco ne'
	 * Unreal: i test possono costruire una piramide immaginaria e verificare che
	 * l'attraversamento faccia esattamente quello che deve.
	 */
	class ITileAvailability
	{
	public:
		virtual ~ITileAvailability() = default;

		/** La tile esiste nel dataset? Deve rispondere SENZA toccare il disco. */
		virtual bool TileExists(const FTileKey& Key) const = 0;

		/** Intervallo di quote della tile, dall'indice. Serve al volume di contenimento. */
		virtual bool GetHeightRange(const FTileKey& Key, double& OutMin, double& OutMax) const = 0;

		/** La tile e' gia' in memoria e disegnabile? */
		virtual bool IsTileLoaded(const FTileKey& Key) const = 0;
	};

	/**
	 * Errore geometrico di un livello, in metri.
	 *
	 * E' il passo fra post a quel livello: raffinando al livello successivo il
	 * campionamento si dimezza, quindi l'errore che si toglie e' di quell'ordine.
	 *
	 * E' un'APPROSSIMAZIONE, ed e' giusto saperlo. L'errore geometrico vero di
	 * una tile sarebbe lo scostamento verticale massimo fra la sua superficie e
	 * quella dei figli: su una pianura e' quasi zero anche a livello grossolano,
	 * su una cresta alpina e' molto piu' grande del passo. Calcolarlo e'
	 * compito della pipeline, e richiederebbe un campo in piu' nell'header della
	 * tile (per questo il formato ha un numero di versione). Finche' non c'e',
	 * il passo e' la stima conservativa ragionevole: sovrastima in pianura,
	 * cioe' raffina un po' piu' del necessario, che e' il verso giusto in cui
	 * sbagliare.
	 */
	double GeometricErrorMetres(uint32_t Level);

	/**
	 * Errore su schermo, in PIXEL.
	 *
	 *     sse = errore_geometrico * altezza_schermo / (distanza * 2 * tan(fov/2))
	 *
	 * E' la proiezione prospettica dell'errore geometrico. La distanza si misura
	 * dalla SUPERFICIE del volume di contenimento, non dal suo centro: usando il
	 * centro, una tile enorme che contiene la camera darebbe distanza grande e
	 * quindi errore piccolo, e non verrebbe mai raffinata proprio quando si ha
	 * il naso sopra.
	 */
	double ComputeScreenSpaceError(double GeometricError, double DistanceMetres,
	                               const FViewParameters& View);

	/**
	 * Sceglie le tile da disegnare e quelle da chiedere.
	 *
	 * L'attraversamento e' ITERATIVO con uno stack esplicito, non ricorsivo: a
	 * livello 14 la profondita' e' 15, che ricorsivamente starebbe anche in
	 * piedi, ma lo stack esplicito rende il costo prevedibile e permette di
	 * imporre un tetto ai nodi visitati senza artifici.
	 */
	void SelectTiles(const FViewParameters& View, const ITileAvailability& Availability,
	                 uint32_t MinLevel, uint32_t MaxLevel,
	                 FSelectionResult& OutResult,
	                 const Core::FEllipsoid& Ellipsoid = Core::WGS84,
	                 int32_t MaxNodesToVisit = 200000);
}

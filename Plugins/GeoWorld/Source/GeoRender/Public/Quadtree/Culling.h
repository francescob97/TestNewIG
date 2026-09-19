// =============================================================================
//  Culling.h -- Scarto per frustum e per orizzonte. STRATO: C++ PURO.
// =============================================================================
#pragma once

#include "Quadtree/QuadtreeTypes.h"

namespace GeoWorld::Quadtree
{
	/**
	 * Costruisce il volume di contenimento di una tile dai suoi estremi
	 * geografici e dall'intervallo di quote, che l'indice del dataset fornisce
	 * SENZA leggere la tile da disco. E' questo che rende possibile scartare un
	 * nodo prima ancora di chiederlo.
	 */
	FTileBoundingVolume MakeTileBoundingVolume(
		double West, double South, double East, double North,
		double MinHeight, double MaxHeight,
		const Core::FEllipsoid& Ellipsoid = Core::WGS84);

	/** I cinque piani del frustum (quattro laterali + near), normali verso l'interno. */
	struct FFrustumPlanes
	{
		FEcef Normals[5];
		double Offsets[5] = { 0, 0, 0, 0, 0 };   // piano: dot(N, P) >= Offset
	};

	FFrustumPlanes MakeFrustumPlanes(const FViewParameters& View);

	/** false se la sfera e' interamente fuori da almeno un piano. */
	bool IsSphereInFrustum(const FFrustumPlanes& Planes, const FEcef& Centre, double Radius);

	/**
	 * Test di occlusione contro l'ellissoide: il punto sta oltre l'orizzonte?
	 *
	 * Si lavora nello "spazio scalato", cioe' dividendo le coordinate ECEF per i
	 * semiassi: li' l'ellissoide diventa una sfera di raggio 1 e il test
	 * dell'orizzonte torna a essere quello elementare della sfera. E' il
	 * classico test di Cesium.
	 */
	bool IsPointBelowHorizon(const FEcef& ScaledCamera, double CameraHorizonSquared,
	                         const FEcef& Point, const Core::FEllipsoid& Ellipsoid);

	/** Porta un punto ECEF nello spazio scalato dove l'ellissoide e' una sfera unitaria. */
	FEcef ToScaledSpace(const FEcef& Point, const Core::FEllipsoid& Ellipsoid);

	/**
	 * Quadrato della distanza dalla camera all'orizzonte, in spazio scalato.
	 * Negativo se la camera e' DENTRO l'ellissoide: in quel caso non si scarta
	 * niente, perche' l'orizzonte non e' definito.
	 */
	double ComputeCameraHorizonSquared(const FEcef& ScaledCamera);

	/**
	 * L'intera SFERA di contenimento sta oltre l'orizzonte?
	 *
	 * =========================================================================
	 *  PERCHE' NON SI TESTANO I PUNTI CAMPIONE
	 * =========================================================================
	 *  La prima versione testava i 18 campioni del volume e scartava se erano
	 *  occlusi tutti. Sembra conservativo e non lo e': i campioni sono punti, e
	 *  una tile grande puo' contenere zone visibili che non cadono su nessuno di
	 *  essi. Il caso reale, trovato dai test: una tile di livello 0 copre mezzo
	 *  pianeta e i suoi campioni stanno a longitudine 0, 90 e 180 gradi. Con la
	 *  camera sopra Roma erano tutti oltre l'orizzonte, e la tile che conteneva
	 *  la camera veniva scartata.
	 *
	 *  Qui si ragiona invece sulla sfera intera, in angoli visti dal centro del
	 *  pianeta:
	 *
	 *      theta  angolo fra la direzione della camera e quella del centro sfera
	 *      beta   semiangolo sotteso dalla sfera
	 *      alphaC angolo di visibilita' della camera:  acos(R / distanza_camera)
	 *      alphaT angolo di visibilita' del punto piu' lontano della sfera
	 *
	 *  La sfera e' interamente occlusa se  theta - beta > alphaC + alphaT.
	 *
	 *  Il pianeta e' approssimato con la sfera INSCRITTA (raggio polare). E'
	 *  deliberato: una sfera piu' piccola occlude meno, quindi al piu' si tiene
	 *  qualche tile che si sarebbe potuta scartare. L'errore va tenuto in questo
	 *  verso — tenere una tile di troppo costa lavoro, scartarne una visibile
	 *  apre un buco nel terreno.
	 */
	bool IsSphereBelowHorizon(const FEcef& Camera, const FEcef& Centre, double Radius,
	                          const Core::FEllipsoid& Ellipsoid);

	/** Comodita': applica IsSphereBelowHorizon al volume di una tile. */
	bool IsTileBelowHorizon(const FTileBoundingVolume& Volume, const FEcef& Camera,
	                        const Core::FEllipsoid& Ellipsoid);
}

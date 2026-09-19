// =============================================================================
//  TileSelector.h -- Attraversamento del quadtree e scelta del livello.
//  STRATO: C++ PURO.
// =============================================================================
#pragma once

#include "Quadtree/Culling.h"
#include "Quadtree/QuadtreeTypes.h"
#include "Tiles/TilingScheme.h"
#include <algorithm>
#include <cmath>
#include <vector>

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

	namespace Tiles = GeoWorld::Tiles;

	inline double GeometricErrorMetres(uint32_t Level)
	{
		// Passo fra post in metri, approssimazione sferica: serve a dimensionare
		// un errore, non a posizionare nulla.
		constexpr double MetresPerDegreeLat = 111132.0;
		return Tiles::PostSpacingDeg(Level) * MetresPerDegreeLat;
	}

	inline double ComputeScreenSpaceError(double GeometricError, double DistanceMetres,
	                               const FViewParameters& View)
	{
		const double Distance = std::max(DistanceMetres, View.NearClipMetres);
		const double FrustumHeightAtDistance =
			2.0 * Distance * std::tan(View.VerticalFovRad * 0.5);

		if (FrustumHeightAtDistance <= 0.0)
		{
			return 0.0;
		}
		return GeometricError * View.ScreenHeightPixels / FrustumHeightAtDistance;
	}

	namespace Detail
	{
		/** Priorita' per il loader: piu' alta = piu' urgente. */
		int32_t PriorityFromError(double ScreenSpaceError, double MaxError)
		{
			// Una tile con errore dieci volte oltre la soglia e' molto piu'
			// urgente di una appena sopra: la scala e' logaritmica perche' gli
			// errori si distribuiscono su ordini di grandezza.
			if (ScreenSpaceError <= MaxError) { return 0; }
			const double Ratio = ScreenSpaceError / std::max(MaxError, 1e-6);
			return static_cast<int32_t>(std::min(6.0, 1.0 + std::log2(Ratio)));
		}
	}

	inline void SelectTiles(const FViewParameters& View, const ITileAvailability& Availability,
	                 uint32_t MinLevel, uint32_t MaxLevel,
	                 FSelectionResult& OutResult, const FEllipsoid& Ellipsoid,
	                 int32_t MaxNodesToVisit)
	{
		OutResult.Reset();

		const FFrustumPlanes Planes = MakeFrustumPlanes(View);

		// Radici: tutte le tile del livello minimo. Al livello 0 sono due, e
		// scartarne una costa due test: non vale la pena di nessuna furbizia.
		std::vector<FTileKey> Stack;
		for (uint32_t Y = 0; Y < Tiles::TilesY(MinLevel); ++Y)
		{
			for (uint32_t X = 0; X < Tiles::TilesX(MinLevel); ++X)
			{
				Stack.push_back(FTileKey{ MinLevel, X, Y });
			}
		}

		while (!Stack.empty() && OutResult.NodesVisited < MaxNodesToVisit)
		{
			const FTileKey Key = Stack.back();
			Stack.pop_back();
			++OutResult.NodesVisited;

			// --- Esiste? ------------------------------------------------------
			// Prima domanda, e la piu' economica: viene dall'indice in memoria.
			// Un nodo che non esiste non ha nemmeno figli da considerare.
			double MinHeight = 0.0, MaxHeight = 0.0;
			if (!Availability.TileExists(Key) ||
			    !Availability.GetHeightRange(Key, MinHeight, MaxHeight))
			{
				++OutResult.CulledByMissing;
				continue;
			}

			const Tiles::FTileBounds Bounds = Tiles::GetTileBounds(Key.Level, Key.X, Key.Y);
			const FTileBoundingVolume Volume = MakeTileBoundingVolume(
				Bounds.West, Bounds.South, Bounds.East, Bounds.North,
				MinHeight, MaxHeight, Ellipsoid);

			// --- Fuori dalla vista? -------------------------------------------
			if (!IsSphereInFrustum(Planes, Volume.Centre, Volume.Radius))
			{
				++OutResult.CulledByFrustum;
				continue;
			}

			// --- Dietro la curvatura? -----------------------------------------
			// Questo test e' cio' che evita di considerare mezzo pianeta a ogni
			// frame: senza, tutte le tile dell'emisfero passerebbero il frustum
			// quando si guarda l'orizzonte.
			if (IsTileBelowHorizon(Volume, View.CameraEcef, Ellipsoid))
			{
				++OutResult.CulledByHorizon;
				continue;
			}

			// --- Quanto sbaglio se mi fermo qui? -------------------------------
			const double DistanceToCentre = (Volume.Centre - View.CameraEcef).Length();
			const double DistanceToSurface = std::max(0.0, DistanceToCentre - Volume.Radius);
			const double ScreenSpaceError = ComputeScreenSpaceError(
				GeometricErrorMetres(Key.Level), DistanceToSurface, View);

			const bool bAtMaxLevel = (Key.Level >= MaxLevel);
			const bool bErrorAcceptable = (ScreenSpaceError <= View.MaxScreenSpaceError);

			if (bErrorAcceptable || bAtMaxLevel)
			{
				OutResult.WorstScreenSpaceError =
					std::max(OutResult.WorstScreenSpaceError, ScreenSpaceError);

				if (Availability.IsTileLoaded(Key))
				{
					OutResult.ToRender.push_back(FSelectedTile{ Key, ScreenSpaceError, DistanceToSurface });
				}
				else
				{
					OutResult.ToLoad.push_back(FTileRequest{
						Key, Detail::PriorityFromError(ScreenSpaceError, View.MaxScreenSpaceError),
						ScreenSpaceError });
				}
				continue;
			}

			// --- Raffinare, ma solo se i figli ci sono gia' -------------------
			//
			// REGOLA CHE EVITA I BUCHI: non si scende finche' i figli non sono
			// caricati. Scendere subito significherebbe smettere di disegnare
			// questo nodo senza avere ancora niente da mettere al suo posto, e
			// il terreno sparirebbe per i frame necessari al caricamento.
			// Si chiedono i figli e INTANTO si continua a disegnare il padre:
			// sfocato, ma continuo.
			FTileKey Children[4];
			int32_t ExistingChildren = 0;
			int32_t LoadedChildren = 0;

			for (int32_t Index = 0; Index < 4; ++Index)
			{
				const FTileKey Child = Key.GetChild(Index);
				if (!Availability.TileExists(Child)) { continue; }

				Children[ExistingChildren++] = Child;
				if (Availability.IsTileLoaded(Child)) { ++LoadedChildren; }
			}

			if (ExistingChildren > 0 && LoadedChildren == ExistingChildren)
			{
				++OutResult.RefinedNodes;
				for (int32_t Index = 0; Index < ExistingChildren; ++Index)
				{
					Stack.push_back(Children[Index]);
				}
				continue;
			}

			// Figli non ancora pronti: chiedili e tieni il padre a schermo.
			const int32_t Priority = Detail::PriorityFromError(ScreenSpaceError, View.MaxScreenSpaceError);
			for (int32_t Index = 0; Index < ExistingChildren; ++Index)
			{
				if (!Availability.IsTileLoaded(Children[Index]))
				{
					OutResult.ToLoad.push_back(FTileRequest{
						Children[Index], Priority, ScreenSpaceError });
				}
			}

			OutResult.WorstScreenSpaceError =
				std::max(OutResult.WorstScreenSpaceError, ScreenSpaceError);

			if (Availability.IsTileLoaded(Key))
			{
				OutResult.ToRender.push_back(FSelectedTile{ Key, ScreenSpaceError, DistanceToSurface });
			}
			else
			{
				OutResult.ToLoad.push_back(FTileRequest{ Key, Priority + 1, ScreenSpaceError });
			}
		}

		// Le richieste piu' urgenti per prime: il loader ha una coda a priorita'
		// ma ordinare qui rende deterministico anche l'ordine di inserimento.
		std::stable_sort(OutResult.ToLoad.begin(), OutResult.ToLoad.end(),
			[](const FTileRequest& A, const FTileRequest& B)
			{
				return A.Priority > B.Priority;
			});
	}

}

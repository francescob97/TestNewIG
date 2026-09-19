#include "Quadtree/TileSelector.h"

#include "Tiles/TilingScheme.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace GeoWorld::Quadtree
{
	using namespace GeoWorld::Core;
	namespace Tiles = GeoWorld::Tiles;

	double GeometricErrorMetres(uint32_t Level)
	{
		// Passo fra post in metri, approssimazione sferica: serve a dimensionare
		// un errore, non a posizionare nulla.
		constexpr double MetresPerDegreeLat = 111132.0;
		return Tiles::PostSpacingDeg(Level) * MetresPerDegreeLat;
	}

	double ComputeScreenSpaceError(double GeometricError, double DistanceMetres,
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

	namespace
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

	void SelectTiles(const FViewParameters& View, const ITileAvailability& Availability,
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
						Key, PriorityFromError(ScreenSpaceError, View.MaxScreenSpaceError),
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
			const int32_t Priority = PriorityFromError(ScreenSpaceError, View.MaxScreenSpaceError);
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

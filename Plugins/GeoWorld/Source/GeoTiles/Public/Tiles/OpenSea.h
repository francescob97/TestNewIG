// =============================================================================
//  OpenSea.h -- Le tile VIRTUALI del mare aperto. STRATO: C++ PURO.
//
//  Al largo il dataset del terreno non ha tile: tilecut.py non scrive le tile
//  senza nemmeno un post di dato, e il quadtree, raffinando la tile costiera,
//  al posto delle figlie in mare lascerebbe un buco. Il dataset d'acqua dice
//  dove quei buchi sono mare (opensea.bin, FOpenSea) e qui si costruiscono le
//  due tile che mancano, in memoria, in qualche decimo di millisecondo:
//
//    - il TERRENO: il fondale, alla quota del geoide meno la profondita' della
//      griglia (la rampa della riva della pipeline, o la batimetria);
//    - l'ACQUA: superficie al geoide su ogni post, niente scavo (il fondale e'
//      gia' giu'), tutta lontana dalla riva, onde piene.
//
//  Il resto del runtime non sa che sono virtuali: una tile di terreno e' una
//  griglia di quote, una tile d'acqua e' una superficie con il suo raster.
// =============================================================================
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

#include "Tiles/TileFormat.h"
#include "Tiles/TileKey.h"
#include "Tiles/TilingScheme.h"
#include "Tiles/WaterTileFormat.h"

namespace GeoWorld::Tiles
{
	/**
	 * Il fondale dove la griglia non sa (attorno solo celle di terra): basso,
	 * perche' succede solo a ridosso della costa.
	 */
	inline constexpr double OpenSeaFallbackDepthM = 2.0;

	/** Longitudine e latitudine del post (I, J) di una tile, 0..128. */
	inline void OpenSeaPostLonLat(const FTileBounds& Bounds, int32_t I, int32_t J, double& OutLon, double& OutLat)
	{
		constexpr double Cells = static_cast<double>(TilePosts - 1);
		OutLon = Bounds.West + (Bounds.East - Bounds.West) * I / Cells;
		OutLat = Bounds.North - (Bounds.North - Bounds.South) * J / Cells;
	}

	/**
	 * Intervallo di quote di una tile virtuale SENZA costruirla: serve al
	 * quadtree, che lo chiede per ogni nodo che visita. Si guarda una griglia
	 * di 5 x 5 punti, con un metro di margine (fra i punti il geoide e il
	 * fondale possono scendere un poco). Falso se il geoide non copre la tile.
	 */
	inline bool OpenSeaHeightRange(const FTileKey& Key, const FOpenSea& Sea, const FGeoidGrid& Geoid,
	                               float& OutMin, float& OutMax)
	{
		const FTileBounds Bounds = GetTileBounds(Key.Level, Key.X, Key.Y);
		double Low = std::numeric_limits<double>::max(), High = -std::numeric_limits<double>::max();
		for (int32_t J = 0; J <= 4; ++J)
		{
			for (int32_t I = 0; I <= 4; ++I)
			{
				double Lon = 0.0, Lat = 0.0, N = 0.0;
				OpenSeaPostLonLat(Bounds, I * (TilePosts - 1) / 4, J * (TilePosts - 1) / 4, Lon, Lat);
				if (!Geoid.Sample(Lon, Lat, N)) { return false; }
				Low = std::min(Low, N - Sea.DepthAt(Lon, Lat, OpenSeaFallbackDepthM));
				High = std::max(High, N);
			}
		}
		OutMin = static_cast<float>(Low - 1.0);
		OutMax = static_cast<float>(High + 1.0);
		return true;
	}

	/** Il fondale: geoide meno profondita', su ogni post. Falso se il geoide non copre la tile. */
	inline bool BuildOpenSeaHeightTile(const FTileKey& Key, const FOpenSea& Sea, const FGeoidGrid& Geoid,
	                                   FHeightTile& Out)
	{
		const FTileBounds Bounds = GetTileBounds(Key.Level, Key.X, Key.Y);
		Out = FHeightTile();
		Out.Key = Key;
		// Come i post che tilecut riempie al livello del mare: non e' DEM.
		Out.bHasFilledPosts = true;
		Out.Heights.resize(static_cast<size_t>(TilePosts) * TilePosts);
		float Low = std::numeric_limits<float>::max(), High = -std::numeric_limits<float>::max();
		for (int32_t J = 0; J < TilePosts; ++J)
		{
			for (int32_t I = 0; I < TilePosts; ++I)
			{
				double Lon = 0.0, Lat = 0.0, N = 0.0;
				OpenSeaPostLonLat(Bounds, I, J, Lon, Lat);
				if (!Geoid.Sample(Lon, Lat, N)) { Out = FHeightTile(); return false; }
				const float Height = static_cast<float>(N - Sea.DepthAt(Lon, Lat, OpenSeaFallbackDepthM));
				Out.Heights[static_cast<size_t>(J) * TilePosts + I] = Height;
				Low = std::min(Low, Height);
				High = std::max(High, Height);
			}
		}
		Out.MinHeight = Low;
		Out.MaxHeight = High;
		return true;
	}

	/**
	 * L'acqua: superficie al geoide, niente scavo, SDF al massimo (lontano da
	 * ogni riva), onde piene, niente corrente. Falso se il geoide non copre la tile.
	 */
	inline bool BuildOpenSeaWaterTile(const FTileKey& Key, const FGeoidGrid& Geoid, FWaterTile& Out)
	{
		const FTileBounds Bounds = GetTileBounds(Key.Level, Key.X, Key.Y);
		const size_t Grid = static_cast<size_t>(WaterPosts) * WaterPosts;
		const size_t Pixels = static_cast<size_t>(WaterRaster) * WaterRaster;
		Out = FWaterTile();
		Out.Key = Key;
		Out.Flags = static_cast<uint16_t>(WaterTileSea | WaterTileAllWater);
		Out.Surface.resize(Grid);
		Out.Bed.assign(Grid, std::numeric_limits<float>::quiet_NaN());
		for (int32_t J = 0; J < WaterPosts; ++J)
		{
			for (int32_t I = 0; I < WaterPosts; ++I)
			{
				double Lon = 0.0, Lat = 0.0, N = 0.0;
				OpenSeaPostLonLat(Bounds, I, J, Lon, Lat);
				if (!Geoid.Sample(Lon, Lat, N)) { Out = FWaterTile(); return false; }
				Out.Surface[static_cast<size_t>(J) * WaterPosts + I] = static_cast<float>(N);
			}
		}
		Out.Sdf.assign(Pixels, static_cast<uint8_t>(255));
		Out.Wave.assign(Pixels, static_cast<uint8_t>(255));
		return true;
	}
}

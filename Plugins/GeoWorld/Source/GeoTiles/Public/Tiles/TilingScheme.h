// =============================================================================
//  TilingScheme.h -- Gemello C++ di Pipeline/geoworld/tiling.py.
//  STRATO: C++ PURO (niente Unreal).
//
//  Le due implementazioni DEVONO concordare: la pipeline decide dove sta ogni
//  post e il runtime deve ricostruire la stessa geometria. Per questo esiste
//  Plugins/GeoWorld/Source/GeoTiles/TestData/tiling_vectors.json, generato da
//  Python, contro cui i test di questo file si confrontano. Riscrivere la
//  matematica a memoria e' esattamente il modo in cui due implementazioni
//  divergono di mezzo pixel e nessuno capisce perche' il terreno e' spostato.
// =============================================================================
#pragma once

#include <cstdint>
#include <algorithm>
#include <cmath>

namespace GeoWorld::Tiles
{
	/** Post per lato di una tile: 129 = 2^7 + 1 (registrazione sui nodi). */
	inline constexpr int32_t TilePosts = 129;
	/** Intervalli per lato: 128. E' questo il numero che divide lo span. */
	inline constexpr int32_t TileCells = TilePosts - 1;

	inline constexpr double LonMin = -180.0, LonMax = 180.0;
	inline constexpr double LatMin = -90.0,  LatMax = 90.0;

	struct FTileBounds
	{
		double West = 0.0, South = 0.0, East = 0.0, North = 0.0;

		double SpanLon() const { return East - West; }
		double SpanLat() const { return North - South; }
		double CentreLon() const { return (West + East) * 0.5; }
		double CentreLat() const { return (South + North) * 0.5; }
	};

	inline uint32_t TilesX(uint32_t Level) { return 2u << Level; }
	inline uint32_t TilesY(uint32_t Level) { return 1u << Level; }

	inline double TileSpanDeg(uint32_t Level)
	{
		return 180.0 / static_cast<double>(1u << Level);
	}

	inline double PostSpacingDeg(uint32_t Level)
	{
		return TileSpanDeg(Level) / static_cast<double>(TileCells);
	}

	inline FTileBounds GetTileBounds(uint32_t Level, uint32_t X, uint32_t Y)
	{
		const double Span = TileSpanDeg(Level);
		const double West = LonMin + static_cast<double>(X) * Span;
		const double North = LatMax - static_cast<double>(Y) * Span;
		return FTileBounds{ West, North - Span, West + Span, North };
	}

	/**
	 * Posizione geografica del post (i, j) della tile.
	 * i cresce verso EST, j cresce verso SUD: j=0 e' la riga piu' a nord, che e'
	 * anche la prima riga memorizzata nel file. La coincidenza fra ordine di
	 * memorizzazione e direzione degli indici elimina un'inversione e con essa
	 * un'intera categoria di bug silenziosi.
	 */
	inline void GetPostLonLat(uint32_t Level, uint32_t X, uint32_t Y,
	                          int32_t I, int32_t J, double& OutLon, double& OutLat)
	{
		const FTileBounds Bounds = GetTileBounds(Level, X, Y);
		const double Spacing = PostSpacingDeg(Level);
		OutLon = Bounds.West + static_cast<double>(I) * Spacing;
		OutLat = Bounds.North - static_cast<double>(J) * Spacing;
	}

	/** Tile che contiene il punto. I bordi del dominio rientrano nell'ultima tile. */
	void TileForLonLat(uint32_t Level, double Lon, double Lat,
	                   uint32_t& OutX, uint32_t& OutY);

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

	inline void TileForLonLat(uint32_t Level, double Lon, double Lat,
	                   uint32_t& OutX, uint32_t& OutY)
	{
		const double Span = TileSpanDeg(Level);

		// floor e non troncamento: per longitudini negative il troncamento
		// arrotonda verso lo zero e sposta la tile di uno.
		double X = std::floor((Lon - LonMin) / Span);
		double Y = std::floor((LatMax - Lat) / Span);

		// Gli estremi del dominio (polo Nord, antimeridiano) cadrebbero in una
		// tile che non esiste: rientrano nell'ultima.
		X = std::max(0.0, std::min(X, static_cast<double>(TilesX(Level) - 1)));
		Y = std::max(0.0, std::min(Y, static_cast<double>(TilesY(Level) - 1)));

		OutX = static_cast<uint32_t>(X);
		OutY = static_cast<uint32_t>(Y);
	}

}

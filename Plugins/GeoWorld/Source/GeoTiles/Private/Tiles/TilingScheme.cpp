#include "Tiles/TilingScheme.h"

#include <algorithm>
#include <cmath>

namespace GeoWorld::Tiles
{
	void TileForLonLat(uint32_t Level, double Lon, double Lat,
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

// =============================================================================
//  VectorClasses.h -- Le classi delle linee (Fase 8). STRATO: C++ PURO.
//
//  I NUMERI SONO UN CONTRATTO CON LA PIPELINE
//  Stanno nelle tile su disco, scritti da Pipeline/geoworld/roadclasses.py. Un
//  test Python (tests/test_roads.py) legge QUESTO file e confronta ogni valore
//  con la tabella Python: cambiarne uno da una parte sola fa fallire i test,
//  invece di colorare le ferrovie come autostrade in silenzio.
//
//  Per lo stesso motivo il formato delle righe qui sotto e' rigido:
//      NomeClasse = numero,
//  una per riga. Il test le legge con un'espressione regolare.
// =============================================================================
#pragma once

#include <cstdint>

namespace GeoWorld::Tiles
{
	enum class ERoadClass : uint8_t
	{
		Unknown = 0,

		// Strade, dalla piu' importante. Una way di autostrada e' UNA
		// carreggiata: in OSM i due sensi di marcia sono due linee.
		Motorway = 1,
		Trunk = 2,
		Primary = 3,
		Secondary = 4,
		Tertiary = 5,
		Unclassified = 6,
		Residential = 7,
		LivingStreet = 8,
		Service = 9,
		Pedestrian = 10,
		Track = 11,
		Path = 12,

		// Ferrovie.
		Rail = 20,
		LightRail = 21,
		RailService = 22,

		// Aeroporti.
		Runway = 30,
		Taxiway = 31,
	};

	/** Il numero piu' alto usato: dimensiona le tabelle di stile. */
	inline constexpr int32_t RoadClassCount = 32;

	// Flag di una linea (un byte). Stessi valori di roadclasses.py.
	inline constexpr uint8_t RoadFlagBridge = 1 << 0;
	inline constexpr uint8_t RoadFlagTunnel = 1 << 1;
	inline constexpr uint8_t RoadFlagUnpaved = 1 << 2;
	inline constexpr uint8_t RoadFlagLink = 1 << 3;

	/** Il nome della pipeline (roadclasses.py), per i messaggi e i controlli. */
	inline const char* RoadClassName(ERoadClass Class)
	{
		switch (Class)
		{
		case ERoadClass::Motorway:     return "motorway";
		case ERoadClass::Trunk:        return "trunk";
		case ERoadClass::Primary:      return "primary";
		case ERoadClass::Secondary:    return "secondary";
		case ERoadClass::Tertiary:     return "tertiary";
		case ERoadClass::Unclassified: return "unclassified";
		case ERoadClass::Residential:  return "residential";
		case ERoadClass::LivingStreet: return "living_street";
		case ERoadClass::Service:      return "service";
		case ERoadClass::Pedestrian:   return "pedestrian";
		case ERoadClass::Track:        return "track";
		case ERoadClass::Path:         return "path";
		case ERoadClass::Rail:         return "rail";
		case ERoadClass::LightRail:    return "light_rail";
		case ERoadClass::RailService:  return "rail_service";
		case ERoadClass::Runway:       return "runway";
		case ERoadClass::Taxiway:      return "taxiway";
		case ERoadClass::Unknown:      break;
		}
		return "?";
	}
}

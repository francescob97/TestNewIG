// =============================================================================
//  BuildingClasses.h -- Le classi degli edifici e le forme dei tetti, come
//  stanno nelle tile .gbd. STRATO: C++ PURO.
//
//  I NUMERI SONO UN CONTRATTO con la pipeline (Pipeline/geoworld/bldclasses.py,
//  che li scrive): un test Python legge questo file e confronta. Un nome per
//  riga, nella forma "Nome = numero,": e' quella che il test sa leggere.
// =============================================================================
#pragma once

#include <cstdint>

namespace GeoWorld::Tiles
{
	enum class EBuildingClass : uint8_t
	{
		Other = 0,        // building=yes: un edificio e basta
		House = 1,        // case, villette, case a schiera
		Apartments = 2,   // condomini, palazzi residenziali
		Commercial = 3,   // negozi, uffici, alberghi
		Industrial = 4,   // capannoni, magazzini, fabbriche
		Religious = 5,    // chiese, cappelle, conventi
		Civic = 6,        // scuole, ospedali, municipi, stazioni
		Agricultural = 7, // fienili, stalle, serre
		Small = 8,        // box, baracche, chioschi
		Roof = 9,         // pensiline: un tetto senza muri
		Tower = 10,       // torri, campanili
		Historic = 11,    // castelli, palazzi storici, rovine
	};
	inline constexpr int32_t BuildingClassCount = 12;

	/** La forma del tetto. Auto: la decide il runtime dalla classe e dalla sagoma. */
	enum class ERoofShape : uint8_t
	{
		Auto = 0,
		Flat = 1,
		Gabled = 2,       // a due falde, con i timpani
		Hipped = 3,       // a padiglione: quattro falde
		Pyramidal = 4,    // a piramide, la punta al centro
		Dome = 5,         // a cupola
		Skillion = 6,     // a una falda sola (per ora: piano)
	};

	// Flag di un edificio (bldclasses.py, FLAG_*).
	inline constexpr uint8_t BuildingWallColour = 1 << 0;
	inline constexpr uint8_t BuildingRoofColour = 1 << 1;
	inline constexpr uint8_t BuildingPart = 1 << 2;
	/** L'altezza viene da height= (e comprende il tetto); altrimenti e' stimata, tetto escluso se "Auto". */
	inline constexpr uint8_t BuildingHeightMeasured = 1 << 3;
	inline constexpr uint8_t BuildingLevelsMeasured = 1 << 4;
	/**
	 * Un contorno che ha le sue parti (building:part): non si disegna (si
	 * disegnano le parti), resta per i monumenti. Le sue parti sono nella sua
	 * stessa tile.
	 */
	inline constexpr uint8_t BuildingOutline = 1 << 5;

	inline const char* BuildingClassName(uint8_t Class)
	{
		static const char* Names[BuildingClassCount] = { "edificio", "casa", "condominio", "commerciale",
			"industriale", "chiesa", "pubblico", "agricolo", "piccolo", "pensilina", "torre", "storico" };
		return Class < BuildingClassCount ? Names[Class] : "?";
	}
}

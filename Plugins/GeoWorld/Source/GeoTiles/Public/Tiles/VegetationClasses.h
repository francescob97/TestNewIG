// =============================================================================
//  VegetationClasses.h -- Le classi della copertura del suolo (la vegetazione).
//  STRATO: C++ PURO.
//
//  Ogni pixel di una tile di vegetazione (.gvg) dice che cosa copre il suolo
//  li': bosco, arbusti, prato, campi, citta'... Vengono da ESA WorldCover
//  (10 m, da Sentinel-1 e Sentinel-2), con i numeri riscritti nei nostri.
//
//  I NUMERI SONO UN CONTRATTO CON LA PIPELINE
//  Stanno nelle tile su disco, scritti da Pipeline/geoworld/vegclasses.py. Un
//  test Python (tests/test_vegetation.py) legge QUESTO file e confronta ogni
//  valore con la tabella Python: cambiarne uno da una parte sola fa fallire i
//  test, invece di far crescere i boschi sull'acqua in silenzio.
//
//  Per lo stesso motivo il formato delle righe qui sotto e' rigido:
//      NomeClasse = numero,
//  una per riga. Il test le legge con un'espressione regolare.
// =============================================================================
#pragma once

#include <cstdint>

namespace GeoWorld::Tiles
{
	enum class ELandCover : uint8_t
	{
		NoData = 0,       // fuori dalla mappa, o la mappa non lo sa
		Trees = 1,        // copertura arborea: boschi, parchi, filari fitti
		Shrubs = 2,       // arbusteti, macchia
		Grass = 3,        // prati, pascoli
		Crops = 4,        // campi coltivati
		Built = 5,        // costruito: citta', paesi, strade larghe
		Bare = 6,         // roccia, sabbia, suolo nudo
		Snow = 7,         // neve e ghiacciai perenni
		Water = 8,        // acqua
		Wetland = 9,      // zone umide con erba
		Mangrove = 10,    // mangrovie (fuori dall'Italia: per completezza)
		Moss = 11,        // muschi e licheni (tundra, alta quota)
	};

	inline constexpr int32_t LandCoverCount = 12;

	/** Il nome, per le stampe. */
	inline const char* LandCoverName(uint8_t Class)
	{
		switch (static_cast<ELandCover>(Class))
		{
		case ELandCover::Trees: return "bosco";
		case ELandCover::Shrubs: return "arbusti";
		case ELandCover::Grass: return "prato";
		case ELandCover::Crops: return "campi";
		case ELandCover::Built: return "costruito";
		case ELandCover::Bare: return "suolo nudo";
		case ELandCover::Snow: return "neve perenne";
		case ELandCover::Water: return "acqua";
		case ELandCover::Wetland: return "zona umida";
		case ELandCover::Mangrove: return "mangrovie";
		case ELandCover::Moss: return "muschi";
		default: return "nessun dato";
		}
	}
}

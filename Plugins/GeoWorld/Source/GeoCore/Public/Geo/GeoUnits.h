// =============================================================================
//  GeoUnits.h -- L'UNICO POSTO DEL PROGETTO DOVE COMPARE IL NUMERO 100.
//
//  Regola di progetto, non suggerimento:
//   - GeoCore, GeoTiles e la logica del quadtree parlano SOLO metri, in double.
//   - I centimetri (unita' Unreal) esistono unicamente dentro FGeoreference.
//   - Cercare "* 100" o "/ 100" nel resto del sorgente deve dare zero risultati.
//     C'e' un test automatico che lo verifica (GeoUnitsDisciplineTest), perche'
//     una regola che nessuno controlla e' una regola che decade in tre settimane.
//
//  PERCHE' 1 unita' = 1 cm e non 1 m: e' la convenzione di Unreal, cablata in
//  tutto il motore (velocita' di default dei pawn, scale dei mesh, gravita',
//  unita' della fisica, griglia dell'editor). Cambiarla si puo' ma si combatte
//  contro il motore per sempre. Meglio accettarla e isolarla qui.
// =============================================================================
#pragma once

namespace GeoWorld::Units
{
	// 1 unita' Unreal = 1 centimetro.
	inline constexpr double MetersToUu = 100.0;
	inline constexpr double UuToMeters = 1.0 / MetersToUu;

	inline constexpr double KilometersToUu = MetersToUu * 1000.0;
	inline constexpr double UuToKilometers = 1.0 / KilometersToUu;
}

// =============================================================================
//  Wind.h -- Il vento. STRATO: C++ PURO, header-only.
//
//  Il vento come lo dice un bollettino: velocita' e direzione. Il vento in
//  ogni punto, con quota, rilievi e raffiche, e' WindField.h; questo e' il
//  tipo che si scambiano il meteo, le onde e le stampe. E' un dato del MONDO
//  e non dell'acqua: domani lo leggeranno anche le nuvole, la vegetazione che
//  si piega, la pioggia obliqua.
//
//  CONVENZIONE: come i bollettini, la direzione e' quella DA CUI soffia, in
//  gradi dal nord in senso orario (270 = da ovest, ponente). La velocita' e'
//  quella a 10 m dal suolo, in m/s.
// =============================================================================
#pragma once

#include <cmath>

namespace GeoWorld::Weather
{
	struct FWind
	{
		double SpeedMs = 0.0;
		double FromDegrees = 270.0;

		/** Direzione VERSO cui soffia, come vettore unitario (nord, est). */
		void Toward(double& OutNorth, double& OutEast) const
		{
			const double Radians = (FromDegrees + 180.0) * 3.14159265358979323846 / 180.0;
			OutNorth = std::cos(Radians);
			OutEast = std::sin(Radians);
		}
	};
}

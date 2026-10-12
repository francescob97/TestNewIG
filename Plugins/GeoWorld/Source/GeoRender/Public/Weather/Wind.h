// =============================================================================
//  Wind.h -- Il vento. STRATO: C++ PURO, header-only.
//
//  Il primo pezzo del meteo (la fase "weather" verra' dopo, con nuvole,
//  pioggia, nebbia): oggi lo usa l'acqua per le onde. E' un dato del MONDO e
//  non dell'acqua, per questo sta qui: domani lo leggeranno anche le nuvole,
//  la vegetazione che si piega, la pioggia obliqua.
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

	/**
	 * Le raffiche: la velocita' media modulata da due oscillazioni lente con
	 * periodi che non si ripetono insieme (11,3 e 4,7 s), e la direzione che
	 * ondeggia di qualche grado (37 s). Liscio: il vento non scatta mai.
	 * `Gustiness` 0..1: 0 = vento costante, 0,3 = raffiche del 30%.
	 */
	inline FWind GustyWind(const FWind& Mean, double Gustiness, double TimeSeconds)
	{
		constexpr double TwoPi = 6.283185307179586;
		const double Pulse = 0.6 * std::sin(TwoPi * TimeSeconds / 11.3) + 0.4 * std::sin(TwoPi * TimeSeconds / 4.7 + 1.3);
		FWind Result = Mean;
		Result.SpeedMs = Mean.SpeedMs * (1.0 + Gustiness * Pulse);
		if (Result.SpeedMs < 0.0) { Result.SpeedMs = 0.0; }
		Result.FromDegrees = Mean.FromDegrees + 10.0 * Gustiness * std::sin(TwoPi * TimeSeconds / 37.0);
		return Result;
	}
}

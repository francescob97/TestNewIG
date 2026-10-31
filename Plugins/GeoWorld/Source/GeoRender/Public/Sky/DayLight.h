// =============================================================================
//  DayLight.h -- L'ora locale e la luce del giorno e della notte. STRATO: C++
//  PURO, header-only.
//
//  1. L'ORA LOCALE. Gli astri vanno calcolati in UT, ma le persone pensano
//     nell'ora dei loro orologi: in Italia l'ora dell'Europa centrale (UT+1,
//     CET) e d'estate l'ora legale (UT+2, CEST), dall'ultima domenica di marzo
//     all'ultima di ottobre, alle 01:00 UT (la regola europea dal 1996). Oppure
//     uno scostamento fisso, per il resto del mondo.
//
//  2. QUANTA LUCE C'E', rispetto al sole a mezzogiorno di una giornata limpida
//     (1 = il "sole del livello" di Unreal). Serve a tutto quello che Unreal
//     non illumina da solo: la pioggia (unlit), il colore della nebbia, i
//     fulmini, l'esposizione di notte.
//       - il sole sopra l'orizzonte: luce diretta attenuata dall'aria (la
//         massa d'aria di Kasten e Young) piu' la luce diffusa del cielo;
//       - il crepuscolo: un decimo ogni 3 gradi sotto l'orizzonte (circa
//         400 lux al tramonto, 3 lux a -6 gradi);
//       - la luna: la sua luce piena per la fase (la legge di Allen) e la sua
//         altezza; QUANTO vale la luna piena rispetto al sole e' una scelta:
//         dal vero sarebbe 1/400.000 (0,25 lux), cioe' 18 stop, piu' di quanto
//         l'esposizione automatica di un gioco recuperi. Il default e' 1/1000;
//       - un fondo di stelle e luce dell'aria.
//
//  3. LE LUCI DELLA CITTA' e LE STELLE: quando si accendono, e quante finestre
//     sono illuminate a che ora (tante la sera, poche alle quattro).
// =============================================================================
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

#include "Sky/Astronomy.h"

namespace GeoWorld::Sky
{
	// =========================================================================
	//  L'ora locale
	// =========================================================================

	enum class ETimeZoneKind : uint8_t
	{
		/** L'ora dell'Europa centrale, con l'ora legale d'estate (Italia). */
		CentralEurope,
		/** Uno scostamento fisso da UT, ore (l'ora solare di un altro paese). */
		Fixed,
	};

	struct FTimeZone
	{
		ETimeZoneKind Kind = ETimeZoneKind::CentralEurope;
		double FixedOffsetHours = 0.0;
	};

	/** L'ultima domenica del mese (marzo o ottobre: 31 giorni) alle 01:00 UT. */
	inline double LastSundayAt0100Ut(int32_t Year, int32_t Month)
	{
		const double LastDay = JulianDay(Year, Month, 31.0 + 1.0 / 24.0);
		return LastDay - DayOfWeek(LastDay);
	}

	/** Lo scostamento dell'ora locale da UT, ore, nell'istante dato. */
	inline double UtcOffsetHours(const FTimeZone& Zone, double JdUt)
	{
		if (Zone.Kind == ETimeZoneKind::Fixed) { return Zone.FixedOffsetHours; }
		const int32_t Year = CalendarDate(JdUt).Year;
		return (JdUt >= LastSundayAt0100Ut(Year, 3) && JdUt < LastSundayAt0100Ut(Year, 10)) ? 2.0 : 1.0;
	}

	/** Il giorno giuliano "locale": quello UT spostato dell'ora locale (per le date e gli orologi). */
	inline double LocalJulianDay(const FTimeZone& Zone, double JdUt)
	{
		return JdUt + UtcOffsetHours(Zone, JdUt) / 24.0;
	}

	/**
	 * Da un'ora locale a UT. Nell'ora del cambio d'autunno (le 2:30 ci sono due
	 * volte) vale la prima; in quella di primavera (le 2:30 non esistono) si
	 * va avanti di un'ora.
	 */
	inline double UtFromLocal(const FTimeZone& Zone, double LocalJd)
	{
		const double Guess = LocalJd - UtcOffsetHours(Zone, LocalJd - 2.0 / 24.0) / 24.0;
		return LocalJd - UtcOffsetHours(Zone, Guess) / 24.0;
	}

	/** L'ora locale del giorno, 0..24. */
	inline double LocalHour(const FTimeZone& Zone, double JdUt)
	{
		const double Local = LocalJulianDay(Zone, JdUt) + 0.5;
		return (Local - std::floor(Local)) * 24.0;
	}

	/** La mezzanotte locale (in UT) del giorno locale che contiene l'istante. */
	inline double LocalMidnightUt(const FTimeZone& Zone, double JdUt)
	{
		const FCalendarDate Date = CalendarDate(LocalJulianDay(Zone, JdUt));
		return UtFromLocal(Zone, JulianDay(Date.Year, Date.Month, static_cast<double>(Date.Day)));
	}

	/** "HH:MM" di un istante, nell'ora locale. */
	inline std::string FormatLocalClock(const FTimeZone& Zone, double JdUt)
	{
		const FCalendarDate Date = CalendarDate(LocalJulianDay(Zone, JdUt) + 30.0 / 86400.0);   // al minuto piu' vicino
		char Buffer[16];
		std::snprintf(Buffer, sizeof(Buffer), "%02d:%02d", Date.Hour, Date.Minute);
		return Buffer;
	}

	/** "AAAA-MM-GG" del giorno locale. */
	inline std::string FormatLocalDate(const FTimeZone& Zone, double JdUt)
	{
		const FCalendarDate Date = CalendarDate(LocalJulianDay(Zone, JdUt));
		char Buffer[24];
		std::snprintf(Buffer, sizeof(Buffer), "%04d-%02d-%02d", Date.Year, Date.Month, Date.Day);
		return Buffer;
	}

	/** Il nome dell'ora: "CEST", "CET", "UT+5,5". */
	inline std::string ZoneName(const FTimeZone& Zone, double JdUt)
	{
		if (Zone.Kind == ETimeZoneKind::CentralEurope) { return UtcOffsetHours(Zone, JdUt) > 1.5 ? "CEST" : "CET"; }
		char Buffer[24];
		std::snprintf(Buffer, sizeof(Buffer), "UT%+.1f", Zone.FixedOffsetHours);
		return Buffer;
	}

	namespace DayLightDetail
	{
		/**
		 * I numeri interi di un testo e i separatori fra uno e l'altro:
		 * "2026-06-21" -> 2026, 6, 21 e "-", "-". Falso se c'e' altro.
		 * (A mano e non con sscanf: il compilatore di Windows lo segnala.)
		 */
		inline bool SplitNumbers(const std::string& Text, int32_t* OutNumbers, char* OutSeparators, int32_t& InOutCount)
		{
			const int32_t Capacity = InOutCount;
			InOutCount = 0;
			size_t Index = 0;
			while (Index < Text.size() && Text[Index] == ' ') { ++Index; }
			while (Index < Text.size())
			{
				if (Text[Index] < '0' || Text[Index] > '9' || InOutCount >= Capacity) { return false; }
				int32_t Value = 0, Digits = 0;
				while (Index < Text.size() && Text[Index] >= '0' && Text[Index] <= '9' && Digits < 6)
				{
					Value = Value * 10 + (Text[Index] - '0');
					++Index;
					++Digits;
				}
				OutNumbers[InOutCount] = Value;
				OutSeparators[InOutCount] = Index < Text.size() ? Text[Index] : 0;
				++InOutCount;
				if (Index < Text.size()) { ++Index; }
				if (Index == Text.size() && OutSeparators[InOutCount - 1] != 0) { return false; }
			}
			return InOutCount > 0;
		}
	}

	/** "18:30", "6.15", "18" -> ore (0..24); falso se non si capisce. */
	inline bool ParseClock(const std::string& Text, double& OutHours)
	{
		int32_t Numbers[2] = { 0, 0 };
		char Separators[2] = { 0, 0 };
		int32_t Count = 2;
		if (!DayLightDetail::SplitNumbers(Text, Numbers, Separators, Count)) { return false; }
		if (Count == 2 && Separators[0] != ':' && Separators[0] != '.') { return false; }
		const int32_t Hours = Numbers[0], Minutes = Count == 2 ? Numbers[1] : 0;
		if (Hours > 24 || Minutes > 59 || (Hours == 24 && Minutes > 0)) { return false; }
		OutHours = Hours + Minutes / 60.0;
		return true;
	}

	/** "2026-06-21" (o con le barre) -> anno, mese, giorno; falso se non e' una data. */
	inline bool ParseDate(const std::string& Text, int32_t& OutYear, int32_t& OutMonth, int32_t& OutDay)
	{
		int32_t Numbers[3] = { 0, 0, 0 };
		char Separators[3] = { 0, 0, 0 };
		int32_t Count = 3;
		if (!DayLightDetail::SplitNumbers(Text, Numbers, Separators, Count) || Count != 3) { return false; }
		const int32_t Year = Numbers[0], Month = Numbers[1], Day = Numbers[2];
		if ((Separators[0] != '-' && Separators[0] != '/') || Separators[0] != Separators[1]
			|| Year < 1600 || Year > 2400 || Month < 1 || Month > 12 || Day < 1) { return false; }
		// Il giorno esiste? Il primo del mese dopo, meno uno.
		const FCalendarDate Last = CalendarDate(JulianDay(Month == 12 ? Year + 1 : Year, Month == 12 ? 1 : Month + 1, 1.0) - 1.0);
		if (Day > Last.Day) { return false; }
		OutYear = Year;
		OutMonth = Month;
		OutDay = Day;
		return true;
	}

	// =========================================================================
	//  La luce
	// =========================================================================

	struct FDaylightSettings
	{
		/** La luna piena allo zenit, rispetto al sole allo zenit (dal vero 2,5e-6). */
		double MoonRatio = 1.0e-3;
		/** Il cielo senza luna (stelle e luce dell'aria), rispetto alla luna piena. */
		double FloorShareOfMoon = 0.03;
	};

	namespace DayLightDetail
	{
		inline double Smooth(double Edge0, double Edge1, double X)
		{
			const double T = std::max(0.0, std::min(1.0, (X - Edge0) / (Edge1 - Edge0)));
			return T * T * (3.0 - 2.0 * T);
		}

		/** Luce diretta e diffusa su un piano orizzontale, senza normalizzare. */
		inline double RawDirectAndDiffuse(double AltitudeDeg)
		{
			if (AltitudeDeg <= 0.0) { return 0.0; }
			const double SinH = std::sin(AltitudeDeg * DegToRad);
			// Massa d'aria (Kasten e Young 1989): 1 allo zenit, ~38 all'orizzonte.
			const double AirMass = 1.0 / (SinH + 0.50572 * std::pow(AltitudeDeg + 6.07995, -1.6364));
			return SinH * std::exp(-0.15 * AirMass) + 0.12 * std::sqrt(SinH);
		}

		/** La stessa, 1 allo zenit. */
		inline double DirectAndDiffuse(double AltitudeDeg)
		{
			return RawDirectAndDiffuse(AltitudeDeg) / RawDirectAndDiffuse(90.0);
		}
	}

	/** Il sole: luce diretta, diffusa e del crepuscolo; 1 allo zenit, 0,004 al tramonto, 5e-5 a -6 gradi. */
	inline double SunlightShare(double SunAltitudeDeg)
	{
		const double Twilight = 4.0e-3 * std::pow(10.0, 0.32 * std::min(SunAltitudeDeg, 2.0));
		return std::max(DayLightDetail::DirectAndDiffuse(SunAltitudeDeg), SunAltitudeDeg < -25.0 ? 0.0 : Twilight);
	}

	/** Quanto e' luminosa la luna rispetto alla piena, dall'angolo di fase (legge di Allen, magnitudini). */
	inline double MoonPhaseBrightness(double PhaseAngleDeg)
	{
		const double I = std::fabs(PhaseAngleDeg);
		return std::pow(10.0, -0.4 * (0.026 * I + 4.0e-9 * I * I * I * I));
	}

	/** Quanta luce c'e' (1 = sole allo zenit): sole, crepuscolo, luna e fondo del cielo. */
	inline double RelativeLight(const FSkyBodies& Sky, const FDaylightSettings& Settings)
	{
		const double Moon = Settings.MoonRatio * MoonPhaseBrightness(Sky.MoonPhaseAngleDeg)
		                  * DayLightDetail::DirectAndDiffuse(Sky.Moon.AltitudeDeg);
		const double Floor = Settings.MoonRatio * Settings.FloorShareOfMoon;
		return std::min(1.0, SunlightShare(Sky.Sun.AltitudeDeg) + Moon + Floor);
	}

	/** Le luci della citta': 0 col sole alto, 1 quando il sole e' 6 gradi sotto l'orizzonte. */
	inline double CityLightsOn(double SunAltitudeDeg)
	{
		return 1.0 - DayLightDetail::Smooth(-6.0, 3.0, SunAltitudeDeg);
	}

	/** Le stelle: si vedono le prime col sole a -5 gradi, tutte a -16. */
	inline double StarVisibility(double SunAltitudeDeg)
	{
		return 1.0 - DayLightDetail::Smooth(-16.0, -5.0, SunAltitudeDeg);
	}

	/** Quanto e' "notte" per l'esposizione: 0 di giorno, 1 col sole a -12 gradi. */
	inline double NightBlend(double SunAltitudeDeg)
	{
		return 1.0 - DayLightDetail::Smooth(-12.0, -1.0, SunAltitudeDeg);
	}

	/**
	 * Quante finestre sono accese, all'ora locale data: tante la sera, poche
	 * nel cuore della notte, qualcuna all'alba. La sera conta solo con le
	 * luci della citta' accese (CityLightsOn).
	 */
	inline double LitWindowShare(double LocalHour)
	{
		static const double Share[25] = {
			0.30, 0.18, 0.10, 0.07, 0.06, 0.10, 0.22, 0.32, 0.25, 0.15, 0.12, 0.12, 0.12,
			0.12, 0.12, 0.12, 0.15, 0.30, 0.48, 0.62, 0.68, 0.66, 0.58, 0.45, 0.30 };
		double Hour = std::fmod(LocalHour, 24.0);
		if (Hour < 0.0) { Hour += 24.0; }
		const int32_t Index = static_cast<int32_t>(std::floor(Hour));
		const double Fraction = Hour - Index;
		return Share[Index] + (Share[Index + 1] - Share[Index]) * Fraction;
	}
}

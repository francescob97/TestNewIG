// =============================================================================
//  Astronomy.h -- Dove sono il sole e la luna, in ogni istante, visti da un
//  punto della Terra. STRATO: C++ PURO, header-only.
//
//  Gli algoritmi sono quelli di Jean Meeus, "Astronomical Algorithms" (2a
//  edizione, 1998), il testo di riferimento per chi calcola effemeridi senza
//  un osservatorio:
//
//    - il GIORNO GIULIANO (cap. 7): il tempo come un numero di giorni,
//      senza mesi ne' anni bisestili;
//    - il TEMPO SIDERALE (cap. 12): quanto e' girata la Terra rispetto alle
//      stelle;
//    - la NUTAZIONE (cap. 22, la versione a mezzo secondo d'arco);
//    - il SOLE (cap. 25, precisione ~0,01 gradi);
//    - la LUNA (cap. 47, i 60 + 60 termini piu' grandi: ~10 secondi d'arco);
//    - le COORDINATE ORIZZONTALI (cap. 13), la RIFRAZIONE (cap. 16), la
//      PARALLASSE della luna (cap. 40, qui in forma vettoriale) e la FASE
//      (cap. 48).
//
//  DUE TEMPI. Il moto dei pianeti va calcolato nel Tempo Terrestre (TT),
//  uniforme; la rotazione della Terra nel Tempo Universale (UT), quello degli
//  orologi. Differiscono di DeltaT (~69 s nel 2024). Le funzioni che vogliono
//  il TT hanno "Tt" nel nome del parametro.
//
//  Gli angoli sono in GRADI, come nelle tabelle di Meeus: e' il confine con i
//  dati. Dentro le formule si passa ai radianti.
// =============================================================================
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "Geo/GeoTypes.h"

namespace GeoWorld::Sky
{
	using Core::DegToRad;
	using Core::RadToDeg;

	/** Il giorno giuliano del 1 gennaio 2000 alle 12 TT: l'epoca J2000.0. */
	inline constexpr double J2000 = 2451545.0;
	/** Raggio equatoriale della Terra, km (per la parallasse della luna). */
	inline constexpr double EarthRadiusKm = 6378.14;
	inline constexpr double MoonRadiusKm = 1737.4;
	inline constexpr double AuKm = 149597870.7;

	namespace AstronomyDetail
	{
		/** Un angolo riportato fra 0 e 360 gradi. */
		inline double Wrap360(double Degrees)
		{
			const double Wrapped = std::fmod(Degrees, 360.0);
			return Wrapped < 0.0 ? Wrapped + 360.0 : Wrapped;
		}
		/** Un angolo riportato fra -180 e 180 gradi. */
		inline double Wrap180(double Degrees)
		{
			const double Wrapped = Wrap360(Degrees);
			return Wrapped > 180.0 ? Wrapped - 360.0 : Wrapped;
		}
		inline double SinD(double Degrees) { return std::sin(Degrees * DegToRad); }
		inline double CosD(double Degrees) { return std::cos(Degrees * DegToRad); }
	}

	// =========================================================================
	//  Il tempo
	// =========================================================================

	/** Una data del calendario gregoriano, con l'ora (UT o TT secondo chi la usa). */
	struct FCalendarDate
	{
		int32_t Year = 2000;
		int32_t Month = 1;
		int32_t Day = 1;
		int32_t Hour = 0;
		int32_t Minute = 0;
		double Second = 0.0;
	};

	/**
	 * Il giorno giuliano di una data gregoriana (Meeus 7.1). Il giorno
	 * giuliano comincia a MEZZOGIORNO: il 1 gennaio 2000 alle 12 e' 2451545,0.
	 */
	inline double JulianDay(int32_t Year, int32_t Month, double DayWithFraction)
	{
		if (Month <= 2)
		{
			Year -= 1;
			Month += 12;
		}
		// I secoli (interi: gli anni qui sono tutti dopo il 1582) e la correzione gregoriana.
		const int32_t Century = Year / 100;
		const int32_t B = 2 - Century + Century / 4;
		return std::floor(365.25 * (Year + 4716)) + std::floor(30.6001 * (Month + 1)) + DayWithFraction + B - 1524.5;
	}

	inline double JulianDay(const FCalendarDate& Date)
	{
		return JulianDay(Date.Year, Date.Month,
			Date.Day + (Date.Hour + (Date.Minute + Date.Second / 60.0) / 60.0) / 24.0);
	}

	/** La data gregoriana di un giorno giuliano (Meeus cap. 7), arrotondata al millisecondo. */
	inline FCalendarDate CalendarDate(double Jd)
	{
		// Al millisecondo, perche' 12:00 non diventi 11:59:59,9999.
		const double Rounded = std::round(Jd * 86400000.0) / 86400000.0;
		const double Z = std::floor(Rounded + 0.5);
		const double F = Rounded + 0.5 - Z;
		double A = Z;
		if (Z >= 2299161.0)
		{
			const double Alpha = std::floor((Z - 1867216.25) / 36524.25);
			A = Z + 1.0 + Alpha - std::floor(Alpha / 4.0);
		}
		const double B = A + 1524.0;
		const double C = std::floor((B - 122.1) / 365.25);
		const double D = std::floor(365.25 * C);
		const double E = std::floor((B - D) / 30.6001);
		const double DayWithFraction = B - D - std::floor(30.6001 * E) + F;

		FCalendarDate Out;
		Out.Day = static_cast<int32_t>(std::floor(DayWithFraction));
		Out.Month = static_cast<int32_t>(E < 14.0 ? E - 1.0 : E - 13.0);
		Out.Year = static_cast<int32_t>(Out.Month > 2 ? C - 4716.0 : C - 4715.0);
		double Seconds = (DayWithFraction - Out.Day) * 86400.0;
		Seconds = std::round(Seconds * 1000.0) / 1000.0;
		Out.Hour = static_cast<int32_t>(Seconds / 3600.0);
		Seconds -= Out.Hour * 3600.0;
		Out.Minute = static_cast<int32_t>(Seconds / 60.0);
		Out.Second = Seconds - Out.Minute * 60.0;
		return Out;
	}

	/** 0 domenica, 1 lunedi', ... 6 sabato, del giorno (UT) che contiene l'istante. */
	inline int32_t DayOfWeek(double Jd)
	{
		const int64_t Days = static_cast<int64_t>(std::floor(Jd + 1.5));
		return static_cast<int32_t>(((Days % 7) + 7) % 7);
	}

	/**
	 * DeltaT = TT - UT, in secondi: quanto la rotazione della Terra e' rimasta
	 * indietro rispetto agli orologi atomici. Misurato: 63,8 s nel 2000, 69,2 s
	 * nel 2024. Qui una retta fra i due (e prima del 2000 la pendenza degli
	 * anni '80 e '90); per la luna un errore di 10 s vale 5 secondi d'arco.
	 */
	inline double DeltaTSeconds(double JdUt)
	{
		const double Year = 2000.0 + (JdUt - J2000) / 365.25;
		if (Year >= 2000.0) { return 63.8 + 0.225 * (Year - 2000.0); }
		return std::max(29.0, 63.8 + 0.66 * (Year - 2000.0));
	}

	inline double TerrestrialTime(double JdUt) { return JdUt + DeltaTSeconds(JdUt) / 86400.0; }

	/** I secoli giuliani da J2000.0 (la variabile T di tutte le serie di Meeus). */
	inline double Centuries(double Jd) { return (Jd - J2000) / 36525.0; }

	/** Il tempo siderale medio a Greenwich (gradi) di un istante UT (Meeus 12.4). */
	inline double GreenwichMeanSiderealTime(double JdUt)
	{
		const double T = Centuries(JdUt);
		return AstronomyDetail::Wrap360(280.46061837 + 360.98564736629 * (JdUt - J2000)
			+ 0.000387933 * T * T - T * T * T / 38710000.0);
	}

	// =========================================================================
	//  Nutazione e obliquita' (Meeus cap. 22, precisione ~0,5")
	// =========================================================================

	struct FNutation
	{
		/** Nutazione in longitudine e in obliquita', gradi. */
		double LongitudeDeg = 0.0;
		double ObliquityDeg = 0.0;
		/** Obliquita' vera dell'eclittica (media + nutazione), gradi. */
		double TrueObliquityDeg = 23.4392911;
	};

	inline FNutation Nutation(double JdTt)
	{
		using namespace AstronomyDetail;
		const double T = Centuries(JdTt);
		const double Omega = 125.04452 - 1934.136261 * T;
		const double SunL = 280.4665 + 36000.7698 * T;
		const double MoonL = 218.3165 + 481267.8813 * T;
		FNutation Out;
		Out.LongitudeDeg = (-17.20 * SinD(Omega) - 1.32 * SinD(2.0 * SunL) - 0.23 * SinD(2.0 * MoonL) + 0.21 * SinD(2.0 * Omega)) / 3600.0;
		Out.ObliquityDeg = (9.20 * CosD(Omega) + 0.57 * CosD(2.0 * SunL) + 0.10 * CosD(2.0 * MoonL) - 0.09 * CosD(2.0 * Omega)) / 3600.0;
		const double Mean = 23.4392911 - 0.0130041667 * T - 1.6389e-7 * T * T + 5.0361e-7 * T * T * T;
		Out.TrueObliquityDeg = Mean + Out.ObliquityDeg;
		return Out;
	}

	/** Coordinate equatoriali: ascensione retta e declinazione (gradi), distanza (km). */
	struct FEquatorial
	{
		double RightAscensionDeg = 0.0;
		double DeclinationDeg = 0.0;
		double DistanceKm = 0.0;
		/** La longitudine e la latitudine eclittiche apparenti, gradi (per la fase). */
		double EclipticLonDeg = 0.0;
		double EclipticLatDeg = 0.0;
	};

	/** Dall'eclittica (longitudine, latitudine) all'equatore, con l'obliquita' data. */
	inline void EclipticToEquatorial(double LonDeg, double LatDeg, double ObliquityDeg, double& OutRaDeg, double& OutDecDeg)
	{
		using namespace AstronomyDetail;
		const double Lambda = LonDeg * DegToRad, Beta = LatDeg * DegToRad, Epsilon = ObliquityDeg * DegToRad;
		OutRaDeg = Wrap360(std::atan2(std::sin(Lambda) * std::cos(Epsilon) - std::tan(Beta) * std::sin(Epsilon), std::cos(Lambda)) * RadToDeg);
		OutDecDeg = std::asin(std::sin(Beta) * std::cos(Epsilon) + std::cos(Beta) * std::sin(Epsilon) * std::sin(Lambda)) * RadToDeg;
	}

	// =========================================================================
	//  Il sole (Meeus cap. 25)
	// =========================================================================

	/** La posizione apparente del sole (geocentrica) a un istante TT. */
	inline FEquatorial SunPosition(double JdTt)
	{
		using namespace AstronomyDetail;
		const double T = Centuries(JdTt);
		const double L0 = Wrap360(280.46646 + 36000.76983 * T + 0.0003032 * T * T);
		const double M = Wrap360(357.52911 + 35999.05029 * T - 0.0001537 * T * T);
		const double E = 0.016708634 - 0.000042037 * T - 0.0000001267 * T * T;
		const double C = (1.914602 - 0.004817 * T - 0.000014 * T * T) * SinD(M)
		               + (0.019993 - 0.000101 * T) * SinD(2.0 * M) + 0.000289 * SinD(3.0 * M);
		const double TrueLongitude = L0 + C;
		const double Anomaly = M + C;
		const double DistanceAu = 1.000001018 * (1.0 - E * E) / (1.0 + E * CosD(Anomaly));
		const FNutation Nut = Nutation(JdTt);
		// L'aberrazione (20,4898" / R) e la nutazione: la longitudine APPARENTE.
		const double Lambda = Wrap360(TrueLongitude - 20.4898 / 3600.0 / DistanceAu + Nut.LongitudeDeg);
		FEquatorial Out;
		EclipticToEquatorial(Lambda, 0.0, Nut.TrueObliquityDeg, Out.RightAscensionDeg, Out.DeclinationDeg);
		Out.DistanceKm = DistanceAu * AuKm;
		Out.EclipticLonDeg = Lambda;
		return Out;
	}

	/**
	 * L'equazione del tempo, minuti (Meeus 28.3): di quanto il sole vero
	 * anticipa il sole medio. Positiva: la meridiana e' avanti all'orologio.
	 */
	inline double EquationOfTimeMinutes(double JdTt)
	{
		using namespace AstronomyDetail;
		const double T = Centuries(JdTt);
		const double L0 = Wrap360(280.46646 + 36000.76983 * T + 0.0003032 * T * T) * DegToRad;
		const double M = Wrap360(357.52911 + 35999.05029 * T - 0.0001537 * T * T) * DegToRad;
		const double E = 0.016708634 - 0.000042037 * T - 0.0000001267 * T * T;
		const double Epsilon = Nutation(JdTt).TrueObliquityDeg * DegToRad;
		const double Y = std::tan(Epsilon / 2.0) * std::tan(Epsilon / 2.0);
		const double Radians = Y * std::sin(2.0 * L0) - 2.0 * E * std::sin(M) + 4.0 * E * Y * std::sin(M) * std::cos(2.0 * L0)
			- 0.5 * Y * Y * std::sin(4.0 * L0) - 1.25 * E * E * std::sin(2.0 * M);
		return Radians * RadToDeg * 4.0;
	}

	// =========================================================================
	//  La luna (Meeus cap. 47)
	// =========================================================================

	namespace AstronomyDetail
	{
		/** Un termine della serie: multipli di D, M, M', F e il coefficiente (1e-6 gradi, o km). */
		struct FMoonTerm
		{
			int8_t D, M, Mp, F;
			int32_t Sine, Cosine;
		};

		/** Tabella 47.A: longitudine (seni, 1e-6 gradi) e distanza (coseni, 1e-3 km). */
		inline const FMoonTerm* MoonLongitudeDistanceTerms(int32_t& OutCount)
		{
			static const FMoonTerm Terms[] = {
				{ 0, 0, 1, 0, 6288774, -20905355 }, { 2, 0, -1, 0, 1274027, -3699111 }, { 2, 0, 0, 0, 658314, -2955968 },
				{ 0, 0, 2, 0, 213618, -569925 }, { 0, 1, 0, 0, -185116, 48888 }, { 0, 0, 0, 2, -114332, -3149 },
				{ 2, 0, -2, 0, 58793, 246158 }, { 2, -1, -1, 0, 57066, -152138 }, { 2, 0, 1, 0, 53322, -170733 },
				{ 2, -1, 0, 0, 45758, -204586 }, { 0, 1, -1, 0, -40923, -129620 }, { 1, 0, 0, 0, -34720, 108743 },
				{ 0, 1, 1, 0, -30383, 104755 }, { 2, 0, 0, -2, 15327, 10321 }, { 0, 0, 1, 2, -12528, 0 },
				{ 0, 0, 1, -2, 10980, 79661 }, { 4, 0, -1, 0, 10675, -34782 }, { 0, 0, 3, 0, 10034, -23210 },
				{ 4, 0, -2, 0, 8548, -21636 }, { 2, 1, -1, 0, -7888, 24208 }, { 2, 1, 0, 0, -6766, 30824 },
				{ 1, 0, -1, 0, -5163, -8379 }, { 1, 1, 0, 0, 4987, -16675 }, { 2, -1, 1, 0, 4036, -12831 },
				{ 2, 0, 2, 0, 3994, -10445 }, { 4, 0, 0, 0, 3861, -11650 }, { 2, 0, -3, 0, 3665, 14403 },
				{ 0, 1, -2, 0, -2689, -7003 }, { 2, 0, -1, 2, -2602, 0 }, { 2, -1, -2, 0, 2390, 10056 },
				{ 1, 0, 1, 0, -2348, 6322 }, { 2, -2, 0, 0, 2236, -9884 }, { 0, 1, 2, 0, -2120, 5751 },
				{ 0, 2, 0, 0, -2069, 0 }, { 2, -2, -1, 0, 2048, -4950 }, { 2, 0, 1, -2, -1773, 4130 },
				{ 2, 0, 0, 2, -1595, 0 }, { 4, -1, -1, 0, 1215, -3958 }, { 0, 0, 2, 2, -1110, 0 },
				{ 3, 0, -1, 0, -892, 3258 }, { 2, 1, 1, 0, -810, 2616 }, { 4, -1, -2, 0, 759, -1897 },
				{ 0, 2, -1, 0, -713, -2117 }, { 2, 2, -1, 0, -700, 2354 }, { 2, 1, -2, 0, 691, 0 },
				{ 2, -1, 0, -2, 596, 0 }, { 4, 0, 1, 0, 549, -1423 }, { 0, 0, 4, 0, 537, -1117 },
				{ 4, -1, 0, 0, 520, -1571 }, { 1, 0, -2, 0, -487, -1739 }, { 2, 1, 0, -2, -399, 0 },
				{ 0, 0, 2, -2, -381, -4421 }, { 1, 1, 1, 0, 351, 0 }, { 3, 0, -2, 0, -340, 0 },
				{ 4, 0, -3, 0, 330, 0 }, { 2, -1, 2, 0, 327, 0 }, { 0, 2, 1, 0, -323, 1165 },
				{ 1, 1, -1, 0, 299, 0 }, { 2, 0, 3, 0, 294, 0 }, { 2, 0, -1, -2, 0, 8752 },
			};
			OutCount = static_cast<int32_t>(sizeof(Terms) / sizeof(Terms[0]));
			return Terms;
		}

		/** Tabella 47.B: latitudine (seni, 1e-6 gradi). */
		inline const FMoonTerm* MoonLatitudeTerms(int32_t& OutCount)
		{
			static const FMoonTerm Terms[] = {
				{ 0, 0, 0, 1, 5128122, 0 }, { 0, 0, 1, 1, 280602, 0 }, { 0, 0, 1, -1, 277693, 0 },
				{ 2, 0, 0, -1, 173237, 0 }, { 2, 0, -1, 1, 55413, 0 }, { 2, 0, -1, -1, 46271, 0 },
				{ 2, 0, 0, 1, 32573, 0 }, { 0, 0, 2, 1, 17198, 0 }, { 2, 0, 1, -1, 9266, 0 },
				{ 0, 0, 2, -1, 8822, 0 }, { 2, -1, 0, -1, 8216, 0 }, { 2, 0, -2, -1, 4324, 0 },
				{ 2, 0, 1, 1, 4200, 0 }, { 2, 1, 0, -1, -3359, 0 }, { 2, -1, -1, 1, 2463, 0 },
				{ 2, -1, 0, 1, 2211, 0 }, { 2, -1, -1, -1, 2065, 0 }, { 0, 1, -1, -1, -1870, 0 },
				{ 4, 0, -1, -1, 1828, 0 }, { 0, 1, 0, 1, -1794, 0 }, { 0, 0, 0, 3, -1749, 0 },
				{ 0, 1, -1, 1, -1565, 0 }, { 1, 0, 0, 1, -1491, 0 }, { 0, 1, 1, 1, -1475, 0 },
				{ 0, 1, 1, -1, -1410, 0 }, { 0, 1, 0, -1, -1344, 0 }, { 1, 0, 0, -1, -1335, 0 },
				{ 0, 0, 3, 1, 1107, 0 }, { 4, 0, 0, -1, 1021, 0 }, { 4, 0, -1, 1, 833, 0 },
				{ 0, 0, 1, -3, 777, 0 }, { 4, 0, -2, 1, 671, 0 }, { 2, 0, 0, -3, 607, 0 },
				{ 2, 0, 2, -1, 596, 0 }, { 2, -1, 1, -1, 491, 0 }, { 2, 0, -2, 1, -451, 0 },
				{ 0, 0, 3, -1, 439, 0 }, { 2, 0, 2, 1, 422, 0 }, { 2, 0, -3, -1, 421, 0 },
				{ 2, 1, -1, 1, -366, 0 }, { 2, 1, 0, 1, -351, 0 }, { 4, 0, 0, 1, 331, 0 },
				{ 2, -1, 1, 1, 315, 0 }, { 2, -2, 0, -1, 302, 0 }, { 0, 0, 1, 3, -283, 0 },
				{ 2, 1, 1, -1, -229, 0 }, { 1, 1, 0, -1, 223, 0 }, { 1, 1, 0, 1, 223, 0 },
				{ 0, 1, -2, -1, -220, 0 }, { 2, 1, -1, -1, -220, 0 }, { 1, 0, 1, 1, -185, 0 },
				{ 2, -1, -2, -1, 181, 0 }, { 0, 1, 2, 1, -177, 0 }, { 4, 0, -2, -1, 176, 0 },
				{ 4, -1, -1, -1, 166, 0 }, { 1, 0, 1, -1, -164, 0 }, { 4, 0, 1, -1, 132, 0 },
				{ 1, 0, -1, -1, -119, 0 }, { 4, -1, 0, -1, 115, 0 }, { 2, -2, 0, 1, 107, 0 },
			};
			OutCount = static_cast<int32_t>(sizeof(Terms) / sizeof(Terms[0]));
			return Terms;
		}
	}

	/** Gli argomenti e le somme della serie: per i test, che li confrontano con l'esempio 47.a di Meeus. */
	struct FMoonSeries
	{
		double LDeg = 0.0, DDeg = 0.0, MDeg = 0.0, MpDeg = 0.0, FDeg = 0.0, E = 1.0;
		/** Le somme, in 1e-6 gradi (longitudine, latitudine) e in metri (distanza). */
		double SumL = 0.0, SumB = 0.0, SumR = 0.0;
	};

	inline FMoonSeries MoonSeries(double JdTt)
	{
		using namespace AstronomyDetail;
		const double T = Centuries(JdTt);
		const double T2 = T * T, T3 = T2 * T, T4 = T3 * T;
		FMoonSeries S;
		S.LDeg = Wrap360(218.3164477 + 481267.88123421 * T - 0.0015786 * T2 + T3 / 538841.0 - T4 / 65194000.0);
		S.DDeg = Wrap360(297.8501921 + 445267.1114034 * T - 0.0018819 * T2 + T3 / 545868.0 - T4 / 113065000.0);
		S.MDeg = Wrap360(357.5291092 + 35999.0502909 * T - 0.0001536 * T2 + T3 / 24490000.0);
		S.MpDeg = Wrap360(134.9633964 + 477198.8675055 * T + 0.0087414 * T2 + T3 / 69699.0 - T4 / 14712000.0);
		S.FDeg = Wrap360(93.2720950 + 483202.0175233 * T - 0.0036539 * T2 - T3 / 3526000.0 + T4 / 863310000.0);
		S.E = 1.0 - 0.002516 * T - 0.0000074 * T2;
		const double A1 = 119.75 + 131.849 * T, A2 = 53.09 + 479264.290 * T, A3 = 313.45 + 481266.484 * T;

		auto Factor = [&S](int32_t MultipleOfM)
		{
			const int32_t Absolute = MultipleOfM < 0 ? -MultipleOfM : MultipleOfM;
			return Absolute == 1 ? S.E : (Absolute == 2 ? S.E * S.E : 1.0);
		};
		int32_t Count = 0;
		const FMoonTerm* Terms = MoonLongitudeDistanceTerms(Count);
		for (int32_t Index = 0; Index < Count; ++Index)
		{
			const FMoonTerm& Term = Terms[Index];
			const double Argument = Term.D * S.DDeg + Term.M * S.MDeg + Term.Mp * S.MpDeg + Term.F * S.FDeg;
			S.SumL += Term.Sine * Factor(Term.M) * SinD(Argument);
			S.SumR += Term.Cosine * Factor(Term.M) * CosD(Argument);
		}
		Terms = MoonLatitudeTerms(Count);
		for (int32_t Index = 0; Index < Count; ++Index)
		{
			const FMoonTerm& Term = Terms[Index];
			const double Argument = Term.D * S.DDeg + Term.M * S.MDeg + Term.Mp * S.MpDeg + Term.F * S.FDeg;
			S.SumB += Term.Sine * Factor(Term.M) * SinD(Argument);
		}
		// Le correzioni per Venere (A1), Giove (A2) e lo schiacciamento della Terra.
		S.SumL += 3958.0 * SinD(A1) + 1962.0 * SinD(S.LDeg - S.FDeg) + 318.0 * SinD(A2);
		S.SumB += -2235.0 * SinD(S.LDeg) + 382.0 * SinD(A3) + 175.0 * SinD(A1 - S.FDeg) + 175.0 * SinD(A1 + S.FDeg)
		        + 127.0 * SinD(S.LDeg - S.MpDeg) - 115.0 * SinD(S.LDeg + S.MpDeg);
		return S;
	}

	/** La posizione apparente della luna (geocentrica) a un istante TT. */
	inline FEquatorial MoonPosition(double JdTt)
	{
		using namespace AstronomyDetail;
		const FMoonSeries S = MoonSeries(JdTt);
		const FNutation Nut = Nutation(JdTt);
		FEquatorial Out;
		Out.EclipticLonDeg = Wrap360(S.LDeg + S.SumL / 1.0e6 + Nut.LongitudeDeg);
		Out.EclipticLatDeg = S.SumB / 1.0e6;
		Out.DistanceKm = 385000.56 + S.SumR / 1000.0;
		EclipticToEquatorial(Out.EclipticLonDeg, Out.EclipticLatDeg, Nut.TrueObliquityDeg, Out.RightAscensionDeg, Out.DeclinationDeg);
		return Out;
	}

	// =========================================================================
	//  Dal cielo all'orizzonte
	// =========================================================================

	/** Una direzione nel cielo locale: azimut dal nord verso est e altezza sull'orizzonte, gradi. */
	struct FHorizontal
	{
		double AzimuthDeg = 0.0;
		double AltitudeDeg = 0.0;

		/** Il versore nel frame locale (nord, est, alto): gli assi del mondo di Unreal all'origine. */
		void ToNorthEastUp(double Out[3]) const
		{
			const double Az = AzimuthDeg * DegToRad, Alt = AltitudeDeg * DegToRad;
			Out[0] = std::cos(Alt) * std::cos(Az);
			Out[1] = std::cos(Alt) * std::sin(Az);
			Out[2] = std::sin(Alt);
		}
	};

	/** Il tempo siderale LOCALE apparente, gradi: GMST + equazione degli equinozi + longitudine (est positiva). */
	inline double LocalSiderealTime(double JdUt, double LonDeg)
	{
		const FNutation Nut = Nutation(TerrestrialTime(JdUt));
		return AstronomyDetail::Wrap360(GreenwichMeanSiderealTime(JdUt)
			+ Nut.LongitudeDeg * std::cos(Nut.TrueObliquityDeg * DegToRad) + LonDeg);
	}

	/** Da ascensione retta e declinazione ad azimut e altezza (Meeus 13.5, 13.6, con l'azimut dal nord). */
	inline FHorizontal EquatorialToHorizontal(double RaDeg, double DecDeg, double LocalSiderealDeg, double LatDeg)
	{
		const double H = (LocalSiderealDeg - RaDeg) * DegToRad;
		const double Dec = DecDeg * DegToRad, Lat = LatDeg * DegToRad;
		FHorizontal Out;
		Out.AltitudeDeg = std::asin(std::sin(Lat) * std::sin(Dec) + std::cos(Lat) * std::cos(Dec) * std::cos(H)) * RadToDeg;
		Out.AzimuthDeg = AstronomyDetail::Wrap360(std::atan2(-std::cos(Dec) * std::sin(H),
			std::sin(Dec) * std::cos(Lat) - std::cos(Dec) * std::sin(Lat) * std::cos(H)) * RadToDeg);
		return Out;
	}

	/**
	 * La rifrazione dell'aria (formula di Saemundsson, Meeus 16.4), gradi:
	 * quanto un astro appare piu' alto di dove e'. Mezzo grado
	 * sull'orizzonte: il sole al tramonto e' gia' sotto quando lo vediamo
	 * toccarlo.
	 */
	inline double RefractionDeg(double TrueAltitudeDeg)
	{
		if (TrueAltitudeDeg < -2.0) { return 0.0; }
		const double H = std::max(TrueAltitudeDeg, -1.0);
		return 1.02 / std::tan((H + 10.3 / (H + 5.11)) * DegToRad) / 60.0;
	}

	/**
	 * La parallasse della luna: vista dalla superficie e non dal centro della
	 * Terra, la luna sta piu' in basso (fino a un grado all'orizzonte). In
	 * forma vettoriale, su una Terra sferica: la direzione topocentrica e'
	 * quella geocentrica per la distanza, meno il raggio verso lo zenit.
	 */
	inline FHorizontal Topocentric(const FHorizontal& Geocentric, double DistanceKm, double& OutDistanceKm)
	{
		double Unit[3];
		Geocentric.ToNorthEastUp(Unit);
		const double V[3] = { Unit[0] * DistanceKm, Unit[1] * DistanceKm, Unit[2] * DistanceKm - EarthRadiusKm };
		OutDistanceKm = std::sqrt(V[0] * V[0] + V[1] * V[1] + V[2] * V[2]);
		FHorizontal Out;
		Out.AltitudeDeg = std::asin(V[2] / OutDistanceKm) * RadToDeg;
		Out.AzimuthDeg = AstronomyDetail::Wrap360(std::atan2(V[1], V[0]) * RadToDeg);
		return Out;
	}

	// =========================================================================
	//  Il cielo visto da un punto, in un istante
	// =========================================================================

	struct FSkyBodies
	{
		double JdUt = J2000;
		/** Il sole e la luna come si VEDONO (con la rifrazione; la luna anche con la parallasse). */
		FHorizontal Sun, Moon;
		/** Il sole e la luna senza rifrazione (la geometria: le ombre la seguono, le stelle no). */
		FHorizontal SunTrue, MoonTrue;
		FEquatorial SunEquatorial, MoonEquatorial;
		/** La distanza della luna dall'osservatore, km, e il suo raggio apparente, gradi. */
		double MoonDistanceKm = 384400.0;
		double MoonRadiusDeg = 0.259;
		/** La parte illuminata del disco, 0 (luna nuova) .. 1 (piena), e l'angolo di fase (gradi). */
		double MoonIllumination = 0.0;
		double MoonPhaseAngleDeg = 180.0;
		/** Crescente (la parte illuminata cresce: si vede la sera). */
		bool bMoonWaxing = true;
		/** Il tempo siderale locale, gradi (per le stelle). */
		double LocalSiderealDeg = 0.0;
	};

	/** Sole e luna visti da (lat, lon) all'istante UT dato. */
	inline FSkyBodies ObserveSky(double JdUt, double LatDeg, double LonDeg)
	{
		using namespace AstronomyDetail;
		FSkyBodies Out;
		Out.JdUt = JdUt;
		const double JdTt = TerrestrialTime(JdUt);
		Out.LocalSiderealDeg = LocalSiderealTime(JdUt, LonDeg);
		Out.SunEquatorial = SunPosition(JdTt);
		Out.MoonEquatorial = MoonPosition(JdTt);

		Out.SunTrue = EquatorialToHorizontal(Out.SunEquatorial.RightAscensionDeg, Out.SunEquatorial.DeclinationDeg, Out.LocalSiderealDeg, LatDeg);
		Out.Sun = Out.SunTrue;
		Out.Sun.AltitudeDeg += RefractionDeg(Out.SunTrue.AltitudeDeg);

		const FHorizontal MoonGeocentric = EquatorialToHorizontal(Out.MoonEquatorial.RightAscensionDeg, Out.MoonEquatorial.DeclinationDeg,
			Out.LocalSiderealDeg, LatDeg);
		Out.MoonTrue = Topocentric(MoonGeocentric, Out.MoonEquatorial.DistanceKm, Out.MoonDistanceKm);
		Out.Moon = Out.MoonTrue;
		Out.Moon.AltitudeDeg += RefractionDeg(Out.MoonTrue.AltitudeDeg);
		Out.MoonRadiusDeg = std::asin(MoonRadiusKm / Out.MoonDistanceKm) * RadToDeg;

		// La fase (Meeus 48.2, 48.3): l'elongazione, poi l'angolo sole-luna-Terra.
		const double DecS = Out.SunEquatorial.DeclinationDeg * DegToRad, DecM = Out.MoonEquatorial.DeclinationDeg * DegToRad;
		const double CosPsi = std::sin(DecS) * std::sin(DecM)
			+ std::cos(DecS) * std::cos(DecM) * std::cos((Out.SunEquatorial.RightAscensionDeg - Out.MoonEquatorial.RightAscensionDeg) * DegToRad);
		const double Psi = std::acos(std::max(-1.0, std::min(1.0, CosPsi)));
		const double R = Out.SunEquatorial.DistanceKm, Delta = Out.MoonEquatorial.DistanceKm;
		const double PhaseAngle = std::atan2(R * std::sin(Psi), Delta - R * std::cos(Psi));
		Out.MoonPhaseAngleDeg = PhaseAngle * RadToDeg;
		Out.MoonIllumination = (1.0 + std::cos(PhaseAngle)) / 2.0;
		Out.bMoonWaxing = Wrap360(Out.MoonEquatorial.EclipticLonDeg - Out.SunEquatorial.EclipticLonDeg) < 180.0;
		return Out;
	}

	/**
	 * La rotazione dal cielo LOCALE (nord, est, alto: gli assi del mondo di
	 * Unreal) all'EQUATORIALE (x verso l'equinozio di primavera, z verso il
	 * polo nord celeste): con questa le stelle girano attorno alla stella
	 * polare un giro ogni giorno siderale. Out[riga][colonna]: la riga i dice
	 * la componente i equatoriale come prodotto scalare con (nord, est, alto).
	 *
	 * Le colonne sono le immagini dei tre assi locali: lo zenit ha declinazione
	 * pari alla latitudine e ascensione retta pari al tempo siderale; il punto
	 * nord dell'orizzonte sta sotto il polo; il punto est e' sull'equatore, sei
	 * ore avanti.
	 */
	inline void HorizonToEquatorial(double LatDeg, double LocalSiderealDeg, double Out[3][3])
	{
		const double Phi = LatDeg * DegToRad, Theta = LocalSiderealDeg * DegToRad;
		const double North[3] = { -std::sin(Phi) * std::cos(Theta), -std::sin(Phi) * std::sin(Theta), std::cos(Phi) };
		const double East[3] = { -std::sin(Theta), std::cos(Theta), 0.0 };
		const double Up[3] = { std::cos(Phi) * std::cos(Theta), std::cos(Phi) * std::sin(Theta), std::sin(Phi) };
		for (int32_t Row = 0; Row < 3; ++Row)
		{
			Out[Row][0] = North[Row];
			Out[Row][1] = East[Row];
			Out[Row][2] = Up[Row];
		}
	}

	// =========================================================================
	//  Levate e tramonti
	// =========================================================================

	/** Quale astro, e a che altezza vera si dice "sorto". */
	enum class ESkyEvent : uint8_t
	{
		Sunrise,          // il bordo del sole tocca l'orizzonte (-0,833 gradi: rifrazione e raggio)
		CivilDawn,        // -6 gradi: si legge all'aperto
		NauticalDawn,     // -12
		AstronomicalDawn, // -18: il cielo e' buio del tutto
		Moonrise,         // il bordo della luna (topocentrica) tocca l'orizzonte
	};

	inline double EventAltitudeDeg(ESkyEvent Event)
	{
		switch (Event)
		{
		case ESkyEvent::Sunrise: return -0.8333;
		case ESkyEvent::CivilDawn: return -6.0;
		case ESkyEvent::NauticalDawn: return -12.0;
		case ESkyEvent::AstronomicalDawn: return -18.0;
		case ESkyEvent::Moonrise: return -0.5667 - 0.26;
		}
		return -0.8333;
	}

	/** Un passaggio: quando (UT), e se sale (levata) o scende (tramonto). */
	struct FCrossing
	{
		double JdUt = 0.0;
		bool bRising = true;
	};

	/**
	 * Quando l'astro passa per l'altezza dell'evento, fra JdStart e JdStart +
	 * Days: si campiona ogni 10 minuti, poi si stringe ogni cambio di segno
	 * per bisezione fino al secondo. Nessuna formula speciale: funziona per il
	 * sole, i crepuscoli e la luna (che in un giorno si sposta di 13 gradi), e
	 * dice da solo quando non c'e' tramonto (estate oltre il circolo polare).
	 */
	inline std::vector<FCrossing> FindCrossings(ESkyEvent Event, double JdStart, double Days, double LatDeg, double LonDeg)
	{
		const bool bMoon = Event == ESkyEvent::Moonrise;
		const double Target = EventAltitudeDeg(Event);
		auto Height = [&](double Jd)
		{
			const double JdTt = TerrestrialTime(Jd);
			const double Lst = LocalSiderealTime(Jd, LonDeg);
			if (bMoon)
			{
				const FEquatorial Moon = MoonPosition(JdTt);
				double Distance = 0.0;
				return Topocentric(EquatorialToHorizontal(Moon.RightAscensionDeg, Moon.DeclinationDeg, Lst, LatDeg),
					Moon.DistanceKm, Distance).AltitudeDeg - Target;
			}
			const FEquatorial Sun = SunPosition(JdTt);
			return EquatorialToHorizontal(Sun.RightAscensionDeg, Sun.DeclinationDeg, Lst, LatDeg).AltitudeDeg - Target;
		};
		std::vector<FCrossing> Out;
		const double Step = 10.0 / 1440.0;
		double PreviousJd = JdStart;
		double Previous = Height(PreviousJd);
		const int32_t Steps = static_cast<int32_t>(std::ceil(Days / Step));
		for (int32_t Index = 1; Index <= Steps; ++Index)
		{
			const double Jd = JdStart + std::min(Days, Index * Step);
			const double Value = Height(Jd);
			if ((Previous < 0.0) != (Value < 0.0))
			{
				double Low = PreviousJd, High = Jd, LowValue = Previous;
				for (int32_t Iteration = 0; Iteration < 30 && (High - Low) > 0.5 / 86400.0; ++Iteration)
				{
					const double Middle = 0.5 * (Low + High);
					const double MiddleValue = Height(Middle);
					if ((MiddleValue < 0.0) == (LowValue < 0.0)) { Low = Middle; LowValue = MiddleValue; }
					else { High = Middle; }
				}
				Out.push_back(FCrossing{ 0.5 * (Low + High), Previous < 0.0 });
			}
			PreviousJd = Jd;
			Previous = Value;
		}
		return Out;
	}

	/** La prima levata e il primo tramonto in [JdStart, JdStart + 1); 0 se non c'e'. */
	inline void RiseAndSet(ESkyEvent Event, double JdStart, double LatDeg, double LonDeg, double& OutRiseJd, double& OutSetJd)
	{
		OutRiseJd = 0.0;
		OutSetJd = 0.0;
		for (const FCrossing& Crossing : FindCrossings(Event, JdStart, 1.0, LatDeg, LonDeg))
		{
			double& Slot = Crossing.bRising ? OutRiseJd : OutSetJd;
			if (Slot == 0.0) { Slot = Crossing.JdUt; }
		}
	}
}

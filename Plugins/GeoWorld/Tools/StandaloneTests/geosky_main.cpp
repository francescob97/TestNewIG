// =============================================================================
//  Test del cielo, senza Unreal: il tempo (giorni giuliani, tempo siderale,
//  ora legale), il sole e la luna confrontati con gli esempi del libro di
//  Meeus e con l'eclisse dell'8 aprile 2024, levate e tramonti, la rotazione
//  del cielo, la luce del giorno e della notte, lo shader del cielo notturno
//  compilato come C++.
// =============================================================================
#include "HlslShim.h"
#include "Sky/Astronomy.h"
#include "Sky/DayLight.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace GeoWorld::Sky;

static int GPassed = 0, GFailed = 0;

static void Check(bool bCondition, const std::string& What, const std::string& Detail = {})
{
	(bCondition ? GPassed : GFailed)++;
	std::printf("  [%s] %s%s%s\n", bCondition ? " ok " : "FAIL", What.c_str(),
		Detail.empty() ? "" : "  -> ", Detail.c_str());
}

static void Section(const char* Title) { std::printf("\n== %s ==\n", Title); }

static std::string Fmt(const char* Format, double A, double B = 0.0, double C = 0.0, double D = 0.0)
{
	char Buffer[240];
	std::snprintf(Buffer, sizeof(Buffer), Format, A, B, C, D);
	return Buffer;
}

static double UtAt(int32_t Year, int32_t Month, int32_t Day, double Hours)
{
	return JulianDay(Year, Month, Day + Hours / 24.0);
}

/** L'angolo fra due direzioni del cielo locale, gradi. */
static double Separation(const FHorizontal& A, const FHorizontal& B)
{
	double U[3], V[3];
	A.ToNorthEastUp(U);
	B.ToNorthEastUp(V);
	const double Dot = U[0] * V[0] + U[1] * V[1] + U[2] * V[2];
	return std::acos(std::max(-1.0, std::min(1.0, Dot))) * RadToDeg;
}

// Torino e Tromso.
static constexpr double TurinLat = 45.07, TurinLon = 7.68;
static constexpr double TromsoLat = 69.65, TromsoLon = 18.96;

// ===========================================================================

static void TestTime()
{
	Section("1. Il tempo: giorni giuliani, tempo siderale, ora legale");
	Check(JulianDay(2000, 1, 1.5) == 2451545.0 && std::fabs(JulianDay(1957, 10, 4.81) - 2436116.31) < 1e-6
		&& JulianDay(1987, 1, 27.0) == 2446822.5,
		"il giorno giuliano (Meeus 7.a, 7.b e J2000)");
	const FCalendarDate Back = CalendarDate(JulianDay(FCalendarDate{ 2024, 6, 21, 13, 45, 30.0 }));
	Check(Back.Year == 2024 && Back.Month == 6 && Back.Day == 21 && Back.Hour == 13 && Back.Minute == 45 && std::fabs(Back.Second - 30.0) < 1e-3
		&& DayOfWeek(JulianDay(2000, 1, 1.0)) == 6 && DayOfWeek(UtAt(2024, 4, 8, 18.0)) == 1,
		"andata e ritorno dal calendario; il 1/1/2000 era sabato, l'8/4/2024 lunedi'");
	Check(std::fabs(GreenwichMeanSiderealTime(2446895.5) - 197.693195) < 2e-6
		&& std::fabs(GreenwichMeanSiderealTime(UtAt(1987, 4, 10, 19.35)) - 128.7378734) < 2e-6,
		"il tempo siderale di Greenwich (Meeus 12.a e 12.b)",
		Fmt("%.6f e %.7f gradi", GreenwichMeanSiderealTime(2446895.5), GreenwichMeanSiderealTime(UtAt(1987, 4, 10, 19.35))));
	Check(std::fabs(DeltaTSeconds(UtAt(2024, 1, 1, 0.0)) - 69.2) < 0.5, "DeltaT nel 2024: ~69 s",
		Fmt("%.1f s", DeltaTSeconds(UtAt(2024, 1, 1, 0.0))));

	const FTimeZone Italy;
	const double SpringChange = LastSundayAt0100Ut(2024, 3), AutumnChange = LastSundayAt0100Ut(2024, 10);
	const FCalendarDate Spring = CalendarDate(SpringChange), Autumn = CalendarDate(AutumnChange);
	Check(Spring.Day == 31 && Spring.Hour == 1 && Autumn.Day == 27 && Autumn.Hour == 1
		&& UtcOffsetHours(Italy, SpringChange - 1e-4) == 1.0 && UtcOffsetHours(Italy, SpringChange + 1e-4) == 2.0
		&& UtcOffsetHours(Italy, AutumnChange - 1e-4) == 2.0 && UtcOffsetHours(Italy, AutumnChange + 1e-4) == 1.0,
		"l'ora legale nel 2024: dal 31 marzo al 27 ottobre, alle 01:00 UT");
	const double Noon = UtFromLocal(Italy, JulianDay(2024, 7, 1.5));
	const double Winter = UtFromLocal(Italy, JulianDay(2024, 1, 15.5));
	Check(std::fabs(CalendarDate(Noon).Hour - 10.0) < 1e-9 && CalendarDate(Winter).Hour == 11
		&& FormatLocalClock(Italy, Noon) == "12:00" && FormatLocalDate(Italy, UtAt(2024, 7, 1, 23.5)) == "2024-07-02"
		&& ZoneName(Italy, Noon) == "CEST" && ZoneName(Italy, Winter) == "CET",
		"mezzogiorno italiano: 10 UT d'estate, 11 d'inverno; alle 23:30 UT del 1 luglio in Italia e' gia' il 2");
	const double Midnight = LocalMidnightUt(Italy, UtAt(2024, 7, 1, 15.0));
	Check(std::fabs(LocalHour(Italy, Midnight)) < 1e-6 && CalendarDate(Midnight).Day == 30 && CalendarDate(Midnight).Hour == 22,
		"la mezzanotte locale del 1 luglio e' alle 22 UT del 30 giugno");
	double Hours = 0.0;
	int32_t Y = 0, M = 0, D = 0;
	Check(ParseClock("18:30", Hours) && std::fabs(Hours - 18.5) < 1e-9 && ParseClock("6", Hours) && Hours == 6.0
		&& !ParseClock("25:00", Hours) && !ParseClock("ieri", Hours)
		&& ParseDate("2024-02-29", Y, M, D) && D == 29 && !ParseDate("2023-02-29", Y, M, D) && !ParseDate("2024-13-01", Y, M, D)
		&& ParseDate("2026/06/21", Y, M, D) && M == 6,
		"le ore e le date scritte in console: le valide passano, il 29 febbraio solo negli anni bisestili");
}

// ===========================================================================

static void TestSun()
{
	Section("2. Il sole: posizione, equazione del tempo, levate e tramonti");
	const FEquatorial Sun = SunPosition(2448908.5);
	Check(std::fabs(Sun.RightAscensionDeg - 198.38083) < 0.002 && std::fabs(Sun.DeclinationDeg + 7.78507) < 0.002
		&& std::fabs(Sun.DistanceKm / AuKm - 0.99766) < 2e-5,
		"il sole del 13 ottobre 1992 (Meeus 25.a)",
		Fmt("AR %.5f, dec %.5f gradi, %.5f UA", Sun.RightAscensionDeg, Sun.DeclinationDeg, Sun.DistanceKm / AuKm));
	const double February = EquationOfTimeMinutes(TerrestrialTime(UtAt(2024, 2, 11, 12.0)));
	const double November = EquationOfTimeMinutes(TerrestrialTime(UtAt(2024, 11, 3, 12.0)));
	Check(std::fabs(February + 14.2) < 0.3 && std::fabs(November - 16.4) < 0.3,
		"l'equazione del tempo: -14 minuti l'11 febbraio, +16 il 3 novembre",
		Fmt("%.2f e %.2f minuti", February, November));
	const double Solstice = SunPosition(TerrestrialTime(UtAt(2024, 6, 20, 20.85))).DeclinationDeg;
	Check(std::fabs(Solstice - 23.436) < 0.01, "al solstizio (20 giugno 2024, 20:51 UT) il sole e' 23,44 gradi a nord",
		Fmt("%.4f gradi", Solstice));

	// Torino il 21 giugno: l'altezza massima e' 90 - 45,07 + 23,44 (piu' la rifrazione).
	double Highest = -90.0, HighestJd = 0.0;
	for (int Minute = 0; Minute < 24 * 60; ++Minute)
	{
		const double Jd = UtAt(2024, 6, 21, Minute / 60.0);
		const FSkyBodies Sky = ObserveSky(Jd, TurinLat, TurinLon);
		if (Sky.SunTrue.AltitudeDeg > Highest) { Highest = Sky.SunTrue.AltitudeDeg; HighestJd = Jd; }
	}
	const FCalendarDate NoonTime = CalendarDate(HighestJd);
	Check(std::fabs(Highest - (90.0 - TurinLat + 23.43)) < 0.1 && NoonTime.Hour == 11 && NoonTime.Minute >= 29 && NoonTime.Minute <= 33,
		"Torino, 21 giugno: a mezzogiorno vero (11:31 UT, 13:31 con l'ora legale) il sole e' a 68,4 gradi",
		Fmt("%.2f gradi alle 11:%02.0f UT", Highest, NoonTime.Minute));

	// Levata e tramonto, confrontati con la formula chiusa (cos H0).
	double Rise = 0.0, Set = 0.0;
	RiseAndSet(ESkyEvent::Sunrise, UtAt(2024, 6, 20, 22.0), TurinLat, TurinLon, Rise, Set);
	const double Dec = SunPosition(TerrestrialTime(HighestJd)).DeclinationDeg * DegToRad, Phi = TurinLat * DegToRad;
	const double CosH0 = (std::sin(-0.8333 * DegToRad) - std::sin(Phi) * std::sin(Dec)) / (std::cos(Phi) * std::cos(Dec));
	const double HalfDayHours = std::acos(CosH0) * RadToDeg / 15.0;
	const double ExpectedRise = HighestJd - HalfDayHours / 24.0, ExpectedSet = HighestJd + HalfDayHours / 24.0;
	Check(std::fabs(Rise - ExpectedRise) * 1440.0 < 2.0 && std::fabs(Set - ExpectedSet) * 1440.0 < 2.0,
		"Torino, 21 giugno: il sole sorge alle 5:42 e tramonta alle 21:20 (ora legale), come dice la formula",
		Fmt("levata %.1f, tramonto %.1f minuti dalla formula", (Rise - ExpectedRise) * 1440.0, (Set - ExpectedSet) * 1440.0));
	const FSkyBodies AtRise = ObserveSky(Rise, TurinLat, TurinLon);
	const double ExpectedAzimuth = std::acos(std::sin(Dec) / std::cos(Phi)) * RadToDeg;
	Check(std::fabs(AtRise.Sun.AltitudeDeg + 0.27) < 0.1 && std::fabs(AtRise.Sun.AzimuthDeg - ExpectedAzimuth) < 1.5,
		"alla levata il centro del sole si vede mezzo raggio sotto l'orizzonte (la rifrazione), a nord-est",
		Fmt("altezza apparente %.2f, azimut %.1f (atteso %.1f)", AtRise.Sun.AltitudeDeg, AtRise.Sun.AzimuthDeg, ExpectedAzimuth));

	double EquatorRise = 0.0, EquatorSet = 0.0;
	RiseAndSet(ESkyEvent::Sunrise, UtAt(2024, 3, 20, 0.0), 0.0, 0.0, EquatorRise, EquatorSet);
	const double DayLength = (EquatorSet - EquatorRise) * 24.0 * 60.0;
	Check(std::fabs(DayLength - 727.0) < 2.0, "all'equatore, all'equinozio, il giorno dura 12 ore e 7 minuti (non 12: rifrazione e raggio)",
		Fmt("%.0f minuti", DayLength));

	double SummerRise = 0.0, SummerSet = 0.0, WinterRise = 0.0, WinterSet = 0.0;
	RiseAndSet(ESkyEvent::Sunrise, UtAt(2024, 6, 21, 0.0), TromsoLat, TromsoLon, SummerRise, SummerSet);
	RiseAndSet(ESkyEvent::Sunrise, UtAt(2024, 12, 21, 0.0), TromsoLat, TromsoLon, WinterRise, WinterSet);
	const FSkyBodies Midnight = ObserveSky(UtAt(2024, 6, 21, 23.0), TromsoLat, TromsoLon);
	Check(SummerRise == 0.0 && SummerSet == 0.0 && WinterRise == 0.0 && WinterSet == 0.0 && Midnight.Sun.AltitudeDeg > 0.0,
		"a Tromso (69,6 gradi nord) d'estate il sole non tramonta e d'inverno non sorge",
		Fmt("altezza a mezzanotte d'estate %.1f gradi", Midnight.Sun.AltitudeDeg));
	double Civil = 0.0, CivilEnd = 0.0;
	RiseAndSet(ESkyEvent::CivilDawn, UtAt(2024, 6, 20, 22.0), TurinLat, TurinLon, Civil, CivilEnd);
	Check(Civil > 0.0 && Civil < Rise && CivilEnd > Set && (Rise - Civil) * 1440.0 > 30.0 && (Rise - Civil) * 1440.0 < 45.0,
		"il crepuscolo civile comincia una mezz'ora prima della levata",
		Fmt("%.0f minuti prima", (Rise - Civil) * 1440.0));
}

// ===========================================================================

static void TestMoon()
{
	Section("3. La luna: posizione, parallasse, fasi");
	const FMoonSeries S = MoonSeries(2448724.5);
	Check(std::fabs(S.LDeg - 134.290182) < 2e-6 && std::fabs(S.DDeg - 113.842304) < 2e-6 && std::fabs(S.MDeg - 97.643514) < 2e-6
		&& std::fabs(S.MpDeg - 5.150833) < 2e-6 && std::fabs(S.FDeg - 219.889721) < 2e-6 && std::fabs(S.E - 1.000194) < 2e-6,
		"gli argomenti della luna del 12 aprile 1992 (Meeus 47.a)");
	Check(std::fabs(S.SumL + 1127527.0) < 2.0 && std::fabs(S.SumB + 3229127.0) < 2.0 && std::fabs(S.SumR + 16590875.0) < 2.0,
		"le somme delle due tabelle: i 120 termini sono quelli del libro",
		Fmt("%.0f, %.0f, %.0f", S.SumL, S.SumB, S.SumR));
	const FEquatorial Moon = MoonPosition(2448724.5);
	Check(std::fabs(Moon.EclipticLonDeg - 133.167265) < 1e-4 && std::fabs(Moon.EclipticLatDeg + 3.229126) < 1e-5
		&& std::fabs(Moon.DistanceKm - 368409.7) < 0.5 && std::fabs(Moon.RightAscensionDeg - 134.688470) < 1e-4
		&& std::fabs(Moon.DeclinationDeg - 13.768368) < 1e-4,
		"la luna: longitudine apparente, latitudine, distanza, ascensione retta e declinazione",
		Fmt("AR %.6f, dec %.6f, %.1f km", Moon.RightAscensionDeg, Moon.DeclinationDeg, Moon.DistanceKm));

	// L'eclisse totale dell'8 aprile 2024: massimo alle 18:17:16 UT a 25,29 N, 104,14 O.
	const double Greatest = UtAt(2024, 4, 8, 18.0 + 17.0 / 60.0 + 16.0 / 3600.0);
	const FSkyBodies Eclipse = ObserveSky(Greatest, 25.29, -104.14);
	double Distance = 0.0;
	const FEquatorial MoonThen = MoonPosition(TerrestrialTime(Greatest));
	const FHorizontal Geocentric = EquatorialToHorizontal(MoonThen.RightAscensionDeg, MoonThen.DeclinationDeg, Eclipse.LocalSiderealDeg, 25.29);
	(void)Topocentric(Geocentric, MoonThen.DistanceKm, Distance);
	const double Topo = Separation(Eclipse.SunTrue, Eclipse.MoonTrue), Geo = Separation(Eclipse.SunTrue, Geocentric);
	// Dal centro della Terra la luna starebbe piu' in alto della parallasse: ~0,95 gradi per cos(70 gradi), 0,33.
	Check(Topo < 0.02 && Geo > 0.25 && Eclipse.MoonRadiusDeg > 0.27 && Eclipse.MoonIllumination < 0.001,
		"l'eclisse dell'8 aprile 2024 in Messico: luna e sole uno sull'altro (dal centro della Terra no: la parallasse)",
		Fmt("%.4f gradi dal posto, %.2f dal centro della Terra; raggio della luna %.3f", Topo, Geo, Eclipse.MoonRadiusDeg));
	const FSkyBodies Full = ObserveSky(UtAt(2024, 4, 23, 23.82), TurinLat, TurinLon);
	const FSkyBodies Waxing = ObserveSky(UtAt(2024, 4, 11, 20.0), TurinLat, TurinLon);
	const FSkyBodies Waning = ObserveSky(UtAt(2024, 4, 27, 3.0), TurinLat, TurinLon);
	Check(Full.MoonIllumination > 0.999 && Waxing.bMoonWaxing && !Waning.bMoonWaxing
		&& Waxing.MoonIllumination > 0.05 && Waxing.MoonIllumination < 0.25,
		"la luna piena del 23 aprile 2024; tre giorni dopo la nuova e' una falce crescente, dopo la piena calante",
		Fmt("piena %.4f, falce %.2f", Full.MoonIllumination, Waxing.MoonIllumination));
	std::vector<FCrossing> Crossings = FindCrossings(ESkyEvent::Moonrise, UtAt(2024, 4, 15, 0.0), 2.0, TurinLat, TurinLon);
	int Rises = 0, Sets = 0;
	for (const FCrossing& Crossing : Crossings) { (Crossing.bRising ? Rises : Sets)++; }
	Check(Rises >= 1 && Sets >= 1 && Rises <= 2 && Sets <= 2,
		"in due giorni la luna sorge e tramonta (circa 50 minuti piu' tardi ogni giorno)", Fmt("%.0f levate, %.0f tramonti", Rises, Sets));
}

// ===========================================================================

static void TestRotation()
{
	Section("4. Il cielo che gira: dal cielo locale all'equatoriale");
	const FSkyBodies Sky = ObserveSky(UtAt(2024, 6, 21, 8.0), TurinLat, TurinLon);
	double Rotation[3][3];
	HorizonToEquatorial(TurinLat, Sky.LocalSiderealDeg, Rotation);
	double Local[3];
	Sky.SunTrue.ToNorthEastUp(Local);
	double Equatorial[3];
	for (int Row = 0; Row < 3; ++Row)
	{
		Equatorial[Row] = Rotation[Row][0] * Local[0] + Rotation[Row][1] * Local[1] + Rotation[Row][2] * Local[2];
	}
	const double Ra = std::atan2(Equatorial[1], Equatorial[0]) * RadToDeg;
	const double Dec = std::asin(Equatorial[2]) * RadToDeg;
	const double RaError = std::fabs(std::remainder(Ra - Sky.SunEquatorial.RightAscensionDeg, 360.0));
	Check(RaError < 0.01 && std::fabs(Dec - Sky.SunEquatorial.DeclinationDeg) < 0.01,
		"la direzione del sole nel cielo locale, ruotata, torna la sua ascensione retta e declinazione",
		Fmt("errore %.4f e %.4f gradi", RaError, Dec - Sky.SunEquatorial.DeclinationDeg));
	const double Pole[3] = { std::cos(TurinLat * DegToRad), 0.0, std::sin(TurinLat * DegToRad) };
	const double PoleZ = Rotation[2][0] * Pole[0] + Rotation[2][1] * Pole[1] + Rotation[2][2] * Pole[2];
	const double Determinant = Rotation[0][0] * (Rotation[1][1] * Rotation[2][2] - Rotation[1][2] * Rotation[2][1])
		- Rotation[0][1] * (Rotation[1][0] * Rotation[2][2] - Rotation[1][2] * Rotation[2][0])
		+ Rotation[0][2] * (Rotation[1][0] * Rotation[2][1] - Rotation[1][1] * Rotation[2][0]);
	Check(std::fabs(PoleZ - 1.0) < 1e-12 && std::fabs(Determinant + 1.0) < 1e-12,
		"a nord, alto quanto la latitudine, c'e' il polo celeste; la rotazione inverte il verso (nord-est-alto e' sinistrorso)");
}

// ===========================================================================

static void TestLight()
{
	Section("5. La luce del giorno e della notte");
	bool bRising = true;
	for (double Altitude = -24.0; Altitude < 90.0; Altitude += 0.5)
	{
		bRising = bRising && SunlightShare(Altitude + 0.5) >= SunlightShare(Altitude);
	}
	Check(std::fabs(SunlightShare(90.0) - 1.0) < 1e-9 && std::fabs(SunlightShare(0.0) - 0.004) < 1e-4
		&& SunlightShare(-6.0) > 2e-5 && SunlightShare(-6.0) < 1e-4 && SunlightShare(30.0) > 0.4 && SunlightShare(30.0) < 0.6 && bRising,
		"il sole: 1 allo zenit, meta' a 30 gradi, 1/250 al tramonto, 1/20.000 a -6 gradi; sempre piu' luce salendo",
		Fmt("%.3f a 30 gradi, %.5f a 0, %.6f a -6", SunlightShare(30.0), SunlightShare(0.0), SunlightShare(-6.0)));
	Check(std::fabs(MoonPhaseBrightness(0.0) - 1.0) < 1e-12 && MoonPhaseBrightness(90.0) > 0.07 && MoonPhaseBrightness(90.0) < 0.11,
		"la luna al primo quarto fa un decimo della piena (non la meta')", Fmt("%.3f", MoonPhaseBrightness(90.0)));
	const FDaylightSettings Settings;
	FSkyBodies Night;
	Night.Sun.AltitudeDeg = -40.0;
	Night.Moon.AltitudeDeg = 90.0;
	Night.MoonPhaseAngleDeg = 0.0;
	const double FullMoonNight = RelativeLight(Night, Settings);
	Night.Moon.AltitudeDeg = -30.0;
	const double DarkNight = RelativeLight(Night, Settings);
	Check(std::fabs(FullMoonNight - Settings.MoonRatio * (1.0 + Settings.FloorShareOfMoon)) < 1e-9 && DarkNight < FullMoonNight / 20.0,
		"di notte la luna piena alta fa la luce scelta (1/1000 del sole); senza luna il fondo del cielo",
		Fmt("%.2e e %.2e", FullMoonNight, DarkNight));
	Check(CityLightsOn(40.0) == 0.0 && CityLightsOn(-6.0) == 1.0 && StarVisibility(-3.0) == 0.0 && StarVisibility(-17.0) == 1.0
		&& NightBlend(5.0) == 0.0 && NightBlend(-12.0) == 1.0 && LitWindowShare(20.0) > 0.6 && LitWindowShare(3.5) < 0.1
		&& std::fabs(LitWindowShare(24.0) - LitWindowShare(0.0)) < 1e-12,
		"le luci della citta' col sole sotto, le stelle col buio, tante finestre accese alle 20 e poche alle 3");
}

// ===========================================================================

#define GEO_NIGHT_SKY(...) __VA_ARGS__
static HlslShim::float3 RunNightSky(const HlslShim::float3& View, const HlslShim::float4& EqX, const HlslShim::float4& EqY,
                                    const HlslShim::float4& EqZ, const HlslShim::float4& Moon, const HlslShim::float4& Sun,
                                    const HlslShim::float4& Night)
{
	using namespace HlslShim;
#include "Sky/NightSkyShader.inl"
}
#undef GEO_NIGHT_SKY

struct FSkyInputs
{
	HlslShim::float4 EqX, EqY, EqZ;
	double Rotation[3][3];
};

static FSkyInputs RotationAt(double LatDeg, double LocalSiderealDeg)
{
	FSkyInputs Out;
	HorizonToEquatorial(LatDeg, LocalSiderealDeg, Out.Rotation);
	Out.EqX = HlslShim::float4(Out.Rotation[0][0], Out.Rotation[0][1], Out.Rotation[0][2], 0.0);
	Out.EqY = HlslShim::float4(Out.Rotation[1][0], Out.Rotation[1][1], Out.Rotation[1][2], 0.0);
	Out.EqZ = HlslShim::float4(Out.Rotation[2][0], Out.Rotation[2][1], Out.Rotation[2][2], 0.0);
	return Out;
}

/** La direzione del mondo (nord, est, alto) che guarda un punto del cielo equatoriale: la trasposta. */
static HlslShim::float3 WorldFromEquatorial(const FSkyInputs& Inputs, const double E[3])
{
	double W[3];
	for (int Column = 0; Column < 3; ++Column)
	{
		W[Column] = Inputs.Rotation[0][Column] * E[0] + Inputs.Rotation[1][Column] * E[1] + Inputs.Rotation[2][Column] * E[2];
	}
	return HlslShim::float3(W[0], W[1], W[2]);
}

static HlslShim::float3 ViewOf(const HlslShim::float3& Direction)
{
	return HlslShim::float3(-Direction.x, -Direction.y, -Direction.z);
}

static void TestShader()
{
	Section("6. Lo shader del cielo notturno (lo stesso testo del materiale, compilato come C++)");
	using HlslShim::float3;
	using HlslShim::float4;
	const double Lux = 10.0;
	const FSkyInputs Sky = RotationAt(TurinLat, 0.0);
	const float4 MoonDown(0.0, 0.0, -1.0, 0.0045), SunDown(0.0, 0.0, -1.0, 0.0);
	const float4 Dark(1.0, Lux, 0.0, 1.0), Day(0.0, Lux, 0.0, 1.0);

	// Una stella: si cerca sul cielo equatoriale, poi si guarda dal mondo.
	double Found[3] = { 0.0, 0.0, 0.0 };
	double Brightest = 0.0;
	int Stars = 0;
	for (int Index = 0; Index < 40000; ++Index)
	{
		const double Ra = (Index % 400) * 0.9 * DegToRad / 4.0, Dec = (Index / 400) * 0.25 * DegToRad;
		const double E[3] = { std::cos(Dec) * std::cos(Ra), std::cos(Dec) * std::sin(Ra), std::sin(Dec) };
		const float3 Glow = RunNightSky(ViewOf(WorldFromEquatorial(Sky, E)), Sky.EqX, Sky.EqY, Sky.EqZ, MoonDown, SunDown, Dark);
		if (Glow.x > 1e-6f) { ++Stars; }
		if (Glow.x > Brightest) { Brightest = Glow.x; Found[0] = E[0]; Found[1] = E[1]; Found[2] = E[2]; }
	}
	const float3 ByDay = RunNightSky(ViewOf(WorldFromEquatorial(Sky, Found)), Sky.EqX, Sky.EqY, Sky.EqZ, MoonDown, SunDown, Day);
	// Tre ore dopo (45 gradi di tempo siderale) la stessa stella sta altrove nel cielo locale, ma c'e'.
	const FSkyInputs Later = RotationAt(TurinLat, 45.0);
	const float3 Moved = RunNightSky(ViewOf(WorldFromEquatorial(Later, Found)), Later.EqX, Later.EqY, Later.EqZ, MoonDown, SunDown, Dark);
	const float3 OldPlace = RunNightSky(ViewOf(WorldFromEquatorial(Sky, Found)), Later.EqX, Later.EqY, Later.EqZ, MoonDown, SunDown, Dark);
	Check(Stars > 20 && Brightest > 0.0 && ByDay.x == 0.0f && std::fabs(Moved.x - Brightest) < 1e-3 * Brightest && OldPlace.x < 0.5 * Brightest,
		"di notte le stelle, di giorno no; tre ore dopo la stessa stella e' girata con il cielo",
		Fmt("%.0f punti su una striscia di cielo, la piu' luminosa %.4f", Stars, Brightest));

	// La Via Lattea: sul piano della galassia verso il centro, piu' che al polo galattico.
	const double Centre[3] = { -0.05487, -0.87341, -0.48386 }, GalacticPole[3] = { -0.86767, -0.19808, 0.45599 };
	const float3 Milky = RunNightSky(ViewOf(WorldFromEquatorial(Sky, Centre)), Sky.EqX, Sky.EqY, Sky.EqZ, MoonDown, SunDown, Dark);
	const float3 Above = RunNightSky(ViewOf(WorldFromEquatorial(Sky, GalacticPole)), Sky.EqX, Sky.EqY, Sky.EqZ, MoonDown, SunDown, Dark);
	Check(Milky.x > 10.0f * std::max(Above.x, 1e-9f) && Milky.x > 0.0f, "la Via Lattea verso il Sagittario, buio verso il polo della galassia",
		Fmt("%.2e contro %.2e", Milky.x, Above.x));

	// La luna, a 40 gradi d'altezza verso sud. Piena: il sole dall'altra parte.
	const float3 MoonDir(-std::cos(40.0 * DegToRad), 0.0, std::sin(40.0 * DegToRad));
	const float4 Moon(MoonDir.x, MoonDir.y, MoonDir.z, 0.0045);
	const float4 Opposite(-MoonDir.x, -MoonDir.y, -MoonDir.z, 0.0);
	const float4 Behind(MoonDir.x, MoonDir.y, MoonDir.z, 0.0);
	const float3 FullCentre = RunNightSky(ViewOf(MoonDir), Sky.EqX, Sky.EqY, Sky.EqZ, Moon, Opposite, Dark);
	const float3 NewCentre = RunNightSky(ViewOf(MoonDir), Sky.EqX, Sky.EqY, Sky.EqZ, Moon, Behind, Dark);
	// Primo quarto: il sole a 90 gradi, verso est. Il bordo verso est acceso, quello verso ovest scuro.
	const float4 East(0.0, 1.0, 0.0, 0.0);
	const float3 EastEdge = HlslShim::normalize(MoonDir + float3(0.0, 0.003, 0.0));
	const float3 WestEdge = HlslShim::normalize(MoonDir + float3(0.0, -0.003, 0.0));
	const float3 LitSide = RunNightSky(ViewOf(EastEdge), Sky.EqX, Sky.EqY, Sky.EqZ, Moon, East, Dark);
	const float3 DarkSide = RunNightSky(ViewOf(WestEdge), Sky.EqX, Sky.EqY, Sky.EqZ, Moon, East, Dark);
	const float3 Beside = RunNightSky(ViewOf(HlslShim::normalize(MoonDir + float3(0.0, 0.02, 0.0))), Sky.EqX, Sky.EqY, Sky.EqZ, Moon, Opposite, Dark);
	Check(FullCentre.x > 0.1f && NewCentre.x < 0.02f * FullCentre.x && LitSide.x > 10.0f * DarkSide.x && Beside.x < 0.05f * FullCentre.x,
		"la luna: piena tutta accesa, nuova solo la luce della Terra, al primo quarto il lato verso il sole; fuori dal disco solo l'alone",
		Fmt("piena %.3f, nuova %.4f, quarto %.3f / %.4f", FullCentre.x, NewCentre.x, LitSide.x, DarkSide.x));
	const float3 DayMoon = RunNightSky(ViewOf(MoonDir), Sky.EqX, Sky.EqY, Sky.EqZ, Moon, Opposite, Day);
	const float3 Cloudy = RunNightSky(ViewOf(MoonDir), Sky.EqX, Sky.EqY, Sky.EqZ, Moon, Opposite, float4(0.0, Lux, 0.0, 0.0));
	Check(DayMoon.x > 0.5f * FullCentre.x && Cloudy.x < 1e-6f, "di giorno la luna si vede lo stesso (pallida sul cielo); dietro le nuvole no");
}

int main()
{
	std::printf("=====================================================\n");
	std::printf(" GeoSky -- il sole, la luna, il giorno e la notte\n");
	std::printf("=====================================================\n");
	TestTime();
	TestSun();
	TestMoon();
	TestRotation();
	TestLight();
	TestShader();
	std::printf("\n=====================================================\n");
	std::printf(" RISULTATO: %d passati, %d falliti\n", GPassed, GFailed);
	std::printf("=====================================================\n");
	return GFailed == 0 ? 0 : 1;
}

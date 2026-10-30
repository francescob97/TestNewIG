// =============================================================================
//  geoweather_main.cpp -- Il vento (simulazione), senza Unreal.
//
//  Prova Weather/WindField.h: profilo e rugosita', rotazione con la quota,
//  venti in quota, rilievi (cresta, valle, ridosso, vento verticale),
//  turbolenza (intensita', coerenza, trasporto, continuita'), variabilita',
//  determinismo. E Weather/Precipitation.h: zero termico e fase per quota,
//  copertura e trasporto dei rovesci, grandine nei nuclei, visibilita',
//  velocita' di caduta, suolo bagnato e neve, particelle, mesh.
// =============================================================================

#include "Weather/Lightning.h"
#include "Weather/Precipitation.h"
#include "Weather/WindField.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace GeoWorld::Weather;

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

static double AngleDifference(double A, double B)
{
	double Difference = std::fmod(A - B + 540.0, 360.0) - 180.0;
	return Difference;
}

static FWindField MakeField(double Speed, double From, double Turbulence = 0.0, double Variability = 0.0)
{
	FWindSettings Settings;
	Settings.Reference = FWind{ Speed, From };
	Settings.Turbulence = Turbulence;
	Settings.Variability = Variability;
	FWindField Field;
	Field.SetSettings(Settings);
	return Field;
}

static FSurfaceInfluence Flat(double Roughness)
{
	FSurfaceInfluence Surface;
	Surface.RoughnessM = Roughness;
	return Surface;
}

static constexpr double Lat = 45.07, Lon = 7.69;

// ---------------------------------------------------------------------------

static void TestProfile()
{
	Section("1. Lo strato limite: profilo, rugosita', rotazione");
	const FWindField Field = MakeField(10.0, 240.0);

	const FWindSample Station = Field.Sample(Lat, Lon, 10.0, 260.0, Flat(RoughnessReference));
	Check(std::fabs(Station.MeanSpeed() - 10.0) < 1e-9 && std::fabs(AngleDifference(Station.MeanFromDegrees(), 240.0)) < 1e-9,
		"a 10 m sul prato della stazione: esattamente il vento impostato",
		Fmt("%.3f m/s da %.2f°", Station.MeanSpeed(), Station.MeanFromDegrees()));

	const double Sea = Field.Sample(Lat, Lon, 10.0, 10.0, Flat(RoughnessWater)).MeanSpeed();
	const double Land = Field.Sample(Lat, Lon, 10.0, 260.0, Flat(RoughnessLand)).MeanSpeed();
	const double City = Field.Sample(Lat, Lon, 10.0, 260.0, Flat(RoughnessUrban)).MeanSpeed();
	Check(Sea > 12.0 && Sea < 12.8 && Land > 8.7 && Land < 9.3 && City > 6.0 && City < 6.8,
		"sul mare piu' forte, in campagna e in citta' piu' debole (stesso tempo)",
		Fmt("mare %.2f, campagna %.2f, citta' %.2f m/s", Sea, Land, City));

	bool bMonotonic = true;
	double Previous = 0.0;
	for (double Height = 1.0; Height <= 600.0; Height *= 1.3)
	{
		const double Speed = Field.Sample(Lat, Lon, Height, 250.0 + Height, Flat(RoughnessLand)).MeanSpeed();
		bMonotonic = bMonotonic && Speed > Previous;
		Previous = Speed;
	}
	const FWind Gradient = Field.GradientWind(Lat);
	const FWindSample Top = Field.Sample(Lat, Lon, 600.0, 850.0, Flat(RoughnessLand));
	const FWindSample Above = Field.Sample(Lat, Lon, 3000.0, 3250.0, Flat(RoughnessLand));
	Check(bMonotonic && std::fabs(Top.MeanSpeed() - Gradient.SpeedMs) < 1e-6 && std::fabs(Above.MeanSpeed() - Gradient.SpeedMs) < 1e-6,
		"cresce con la quota fino al vento di gradiente, poi resta quello (senza venti in quota)",
		Fmt("gradiente %.2f m/s, 1,7 volte il riferimento", Gradient.SpeedMs));

	const double Veer = AngleDifference(Top.MeanFromDegrees(), Station.MeanFromDegrees());
	const double SeaTurn = AngleDifference(Gradient.FromDegrees, Field.Sample(Lat, Lon, 10.0, 10.0, Flat(RoughnessWater)).MeanFromDegrees());
	Check(Veer > 20.0 && Veer < 35.0 && SeaTurn > 7.0 && SeaTurn < 13.0,
		"emisfero nord: salendo il vento gira in senso orario (~27° sulla terra, ~10° sul mare)",
		Fmt("terra %.1f°, mare %.1f°", Veer, SeaTurn));
	const double South = AngleDifference(Field.Sample(-35.0, Lon, 600.0, 600.0, Flat(RoughnessReference)).MeanFromDegrees(),
		Field.Sample(-35.0, Lon, 10.0, 10.0, Flat(RoughnessReference)).MeanFromDegrees());
	Check(South < -20.0, "emisfero sud: al contrario", Fmt("%.1f°", South));

	const FWind SeaWind = Field.SurfaceWind(Lat, RoughnessWater);
	Check(std::fabs(SeaWind.SpeedMs - Sea) < 1e-9, "il vento del mare per le onde e' lo stesso del campo", Fmt("%.2f m/s", SeaWind.SpeedMs));
}

static void TestAloft()
{
	Section("2. I venti in quota");
	FWindSettings Settings;
	Settings.Reference = FWind{ 5.0, 240.0 };
	Settings.Turbulence = 0.0;
	Settings.Aloft = { FWindLayer{ 5000.0, 40.0, 300.0 }, FWindLayer{ 3000.0, 20.0, 270.0 } };   // in disordine apposta
	FWindField Field;
	Field.SetSettings(Settings);
	const double Ground = 200.0;
	auto At = [&](double Altitude) { return Field.Sample(Lat, Lon, Altitude - Ground, Altitude, Flat(RoughnessLand)); };

	const FWindSample Layer = At(3000.0);
	const FWindSample High = At(9000.0);
	Check(std::fabs(Layer.MeanSpeed() - 20.0) < 1e-6 && std::fabs(AngleDifference(Layer.MeanFromDegrees(), 270.0)) < 1e-6
		&& std::fabs(High.MeanSpeed() - 40.0) < 1e-6, "sullo strato il suo vento; sopra l'ultimo, l'ultimo",
		Fmt("3000 m: %.1f m/s da %.0f°; 9000 m: %.1f m/s", Layer.MeanSpeed(), Layer.MeanFromDegrees(), High.MeanSpeed()));
	const double Between = At(4000.0).MeanSpeed();
	Check(Between > 20.0 && Between < 40.0, "fra due strati si interpola", Fmt("4000 m: %.1f m/s", Between));

	const double Below = At(Ground + 599.0).MeanSpeed(), Over = At(Ground + 601.0).MeanSpeed();
	Check(std::fabs(Below - Over) < 0.1, "continuo in cima allo strato limite", Fmt("%.3f e %.3f m/s", Below, Over));
}

// --- I rilievi ---------------------------------------------------------------

/** Una collina tonda alta 300 m (sigma 800 m) e, piu' a est, una cresta ripida nord-sud. */
static bool Hills(double LonDeg, double LatDeg, double& OutHeight)
{
	const double North = (LatDeg - Lat) * 111132.0;
	const double East = (LonDeg - Lon) * 111320.0 * std::cos(Lat * WindDegToRad);
	const double Hill = 300.0 * std::exp(-(North * North + East * East) / (2.0 * 800.0 * 800.0));
	const double RidgeEast = East - 6000.0;
	const double Ridge = 400.0 * std::exp(-(RidgeEast * RidgeEast) / (2.0 * 300.0 * 300.0));
	OutHeight = 200.0 + Hill + Ridge;
	return true;
}

static double ToLon(double EastMetres) { return Lon + EastMetres / (111320.0 * std::cos(Lat * WindDegToRad)); }

static void TestOrography()
{
	Section("3. I rilievi: cresta, piede, ridosso, vento verticale");
	const FWindField Field = MakeField(10.0, 270.0);      // da ovest: soffia verso est
	const double TowardNorth = 0.0, TowardEast = 1.0;
	auto Influence = [&](double East) { return ComputeOrography(Hills, Lat, ToLon(East), TowardNorth, TowardEast, RoughnessLand); };
	auto Speed = [&](double East, double Height)
	{
		double Ground = 0.0;
		Hills(ToLon(East), Lat, Ground);
		return Field.Sample(Lat, ToLon(East), Height, Ground + Height, Influence(East)).MeanSpeed();
	};

	const FSurfaceInfluence Crest = Influence(0.0);
	const FSurfaceInfluence Plain = Influence(-3500.0);
	Check(Crest.SpeedUp > 1.2 && Crest.SpeedUp < 1.6 && std::fabs(Plain.SpeedUp - 1.0) < 0.05,
		"in cima alla collina il vento accelera, in pianura no",
		Fmt("cresta x%.2f, pianura x%.2f", Crest.SpeedUp, Plain.SpeedUp));
	Check(Speed(0.0, 10.0) > 1.2 * Speed(-3500.0, 10.0), "a 10 m: piu' vento in cresta",
		Fmt("%.1f contro %.1f m/s", Speed(0.0, 10.0), Speed(-3500.0, 10.0)));
	const double HighCrest = Speed(0.0, 2500.0), HighPlain = Speed(-3500.0, 2500.0);
	Check(std::fabs(HighCrest / HighPlain - 1.0) < 0.02, "salendo l'effetto si spegne",
		Fmt("a 2500 m: %.2f contro %.2f m/s", HighCrest, HighPlain));

	const FSurfaceInfluence Windward = Influence(-500.0);
	const FSurfaceInfluence Leeward = Influence(500.0);
	double Ground = 0.0;
	Hills(ToLon(-500.0), Lat, Ground);
	const double Up = Field.Sample(Lat, ToLon(-500.0), 30.0, Ground + 30.0, Windward).MeanUp;
	Hills(ToLon(500.0), Lat, Ground);
	const double Down = Field.Sample(Lat, ToLon(500.0), 30.0, Ground + 30.0, Leeward).MeanUp;
	Check(Windward.SlopeAlongWind > 0.1 && Up > 0.3 && Down < -0.3, "sopravvento l'aria sale, sottovento scende",
		Fmt("%.2f m/s su, %.2f m/s giu'", Up, Down));

	// La cresta ripida a 6 km: subito dietro (a est) si e' riparati.
	const FSurfaceInfluence Behind = Influence(6600.0);
	const FSurfaceInfluence Before = Influence(5400.0);
	Check(Behind.Shelter > 0.5 && Before.Shelter == 0.0, "sottovento a una cresta ripida: ridosso; sopravvento no",
		Fmt("dietro %.2f, davanti %.2f", Behind.Shelter, Before.Shelter));
	FWindField Gusty = MakeField(10.0, 270.0, 1.0);
	Hills(ToLon(6600.0), Lat, Ground);
	const FWindSample Lee = Gusty.Sample(Lat, ToLon(6600.0), 10.0, Ground + 10.0, Behind);
	Hills(ToLon(-3500.0), Lat, Ground);
	const FWindSample Open = Gusty.Sample(Lat, ToLon(-3500.0), 10.0, Ground + 10.0, Plain);
	Check(Lee.MeanSpeed() < 0.7 * Open.MeanSpeed() && Lee.SigmaU / Lee.MeanSpeed() > 1.8 * Open.SigmaU / Open.MeanSpeed(),
		"nel ridosso: meno vento, ma piu' rafficato",
		Fmt("%.1f m/s (turbolenza %.0f%%) contro %.1f m/s (%.0f%%)", Lee.MeanSpeed(), 100.0 * Lee.SigmaU / Lee.MeanSpeed(),
			Open.MeanSpeed(), 100.0 * Open.SigmaU / Open.MeanSpeed()));

	auto Unknown = [](double, double, double&) { return false; };
	const FSurfaceInfluence Blind = ComputeOrography(Unknown, Lat, Lon, TowardNorth, TowardEast, RoughnessWater);
	Check(Blind.SpeedUp == 1.0 && Blind.Shelter == 0.0 && Blind.SlopeAlongWind == 0.0 && Blind.RoughnessM == RoughnessWater,
		"senza quote: nessun effetto dei rilievi");
}

// --- La turbolenza -----------------------------------------------------------

static void TestTurbulence()
{
	Section("4. La turbolenza: intensita', coerenza, trasporto, continuita'");
	FWindField Field = MakeField(10.0, 270.0, 1.0);
	const FSurfaceInfluence Land = Flat(RoughnessReference);

	// Molti punti lontani fra loro: la componente lungo il vento ha la sigma attesa.
	double Sum = 0.0, Squares = 0.0, Expected = 0.0;
	const int Count = 4000;
	for (int Index = 0; Index < Count; ++Index)
	{
		const FWindSample Sample = Field.Sample(Lat + 0.013 * (Index % 70), Lon + 0.017 * (Index / 70), 10.0, 260.0, Land);
		const double Along = Sample.East - Sample.MeanEast;          // il vento va verso est
		Sum += Along;
		Squares += Along * Along;
		Expected = Sample.SigmaU;
	}
	const double Mean = Sum / Count, Sigma = std::sqrt(Squares / Count - Mean * Mean);
	Check(std::fabs(Mean) < 0.1 * Expected && std::fabs(Sigma / Expected - 1.0) < 0.2,
		"le raffiche: media zero, deviazione quella dell'atmosfera (Eurocodice: 1/ln(z/z0))",
		Fmt("media %.2f, sigma %.2f m/s (attesa %.2f, il %.0f%% del vento)", Mean, Sigma, Expected, 10.0 * Expected));

	// Coerenza: due punti vicini (1/10 della scala) si somigliano, due lontani no.
	auto Correlation = [&](double SeparationM)
	{
		double Xy = 0.0, Xx = 0.0, Yy = 0.0;
		for (int Index = 0; Index < 2000; ++Index)
		{
			const double La = Lat + 0.011 * (Index % 50), Lo = Lon + 0.013 * (Index / 50);
			const double A = Field.Sample(La, Lo, 10.0, 260.0, Land).East - Field.Sample(La, Lo, 10.0, 260.0, Land).MeanEast;
			const double La2 = La + SeparationM / 111132.0;
			const FWindSample Other = Field.Sample(La2, Lo, 10.0, 260.0, Land);
			const double B = Other.East - Other.MeanEast;
			Xy += A * B; Xx += A * A; Yy += B * B;
		}
		return Xy / std::sqrt(Xx * Yy);
	};
	const double Length = Field.Sample(Lat, Lon, 10.0, 260.0, Land).TurbulenceLengthM;
	const double Near = Correlation(0.1 * Length), Far = Correlation(5.0 * Length);
	// Von Karman: per r piccolo 1 - rho cresce come (r/L)^(2/3); a r = L/10 ~0,7-0,8.
	Check(Near > 0.65 && std::fabs(Far) < 0.25, "raffiche coerenti: vicine si somigliano, lontane no",
		Fmt("scala %.0f m: a %.0f m correlazione %.2f, ", Length, 0.1 * Length, Near) + Fmt("a %.0f m %.2f", 5.0 * Length, Far));

	// Trasporto (Taylor): la raffica che ora e' qui, fra due secondi e' sottovento.
	double Error = 0.0, Scale = 0.0;
	FWindField Moving = MakeField(10.0, 270.0, 1.0);
	std::vector<double> Before;
	const double Drift = 0.7 * Moving.GradientWind(Lat).SpeedMs;     // la velocita' di trasporto
	for (int Index = 0; Index < 300; ++Index)
	{
		const FWindSample Sample = Moving.Sample(Lat + 0.003 * Index, Lon, 10.0, 260.0, Land);
		Before.push_back(Sample.East - Sample.MeanEast);
	}
	Moving.Advance(2.0, Lat);
	// Verso dove? il gradiente gira di ~11° rispetto al vento al suolo: si segue la deriva vera.
	const FWind Gradient = Moving.GradientWind(Lat);
	double DriftNorth = 0.0, DriftEast = 0.0;
	WindDetail::ToVector(Drift, Gradient.FromDegrees, DriftNorth, DriftEast);
	for (int Index = 0; Index < 300; ++Index)
	{
		const double La = Lat + 0.003 * Index + DriftNorth * 2.0 / 111132.0;
		const double Lo = Lon + DriftEast * 2.0 / (111320.0 * std::cos(La * WindDegToRad));
		const FWindSample Sample = Moving.Sample(La, Lo, 10.0, 260.0, Land);
		const double Now = Sample.East - Sample.MeanEast;
		Error += (Now - Before[static_cast<size_t>(Index)]) * (Now - Before[static_cast<size_t>(Index)]);
		Scale += Before[static_cast<size_t>(Index)] * Before[static_cast<size_t>(Index)];
	}
	const double Relative = std::sqrt(Error / Scale);
	Check(Relative < 0.35, "turbolenza congelata: la raffica viaggia con il vento (e cambia piano)",
		Fmt("dopo 2 s, %.0f m sottovento: differenza %.0f%% della raffica", 2.0 * Drift, 100.0 * Relative));

	// Continuita' a 60 fps, anche con la variabilita' accesa e dopo un'ora di gioco.
	FWindField Live = MakeField(10.0, 270.0, 1.0, 1.0);
	Live.Advance(3600.0, Lat);
	double Previous = Live.Sample(Lat, Lon, 10.0, 260.0, Land).Speed(), Largest = 0.0, SigmaU = 0.0;
	for (int Frame = 0; Frame < 600; ++Frame)
	{
		Live.Advance(1.0 / 60.0, Lat);
		const FWindSample Sample = Live.Sample(Lat, Lon, 10.0, 260.0, Land);
		Largest = std::max(Largest, std::fabs(Sample.Speed() - Previous));
		Previous = Sample.Speed();
		SigmaU = Sample.SigmaU;
	}
	Check(Largest < 0.15 * SigmaU + 0.05, "nessun salto fra un frame e l'altro (dopo un'ora, con la variabilita')",
		Fmt("salto massimo %.3f m/s (sigma %.2f)", Largest, SigmaU));

	const FWindField Calm = MakeField(0.0, 0.0, 1.0);
	const FWindSample Still = Calm.Sample(Lat, Lon, 10.0, 260.0, Land);
	Check(Still.Speed() == 0.0 && Still.Up == 0.0 && std::isfinite(Still.MeanFromDegrees()), "calma: niente vento, niente NaN");

	Check(TurbulenceIntensity(10.0, RoughnessWater) < TurbulenceIntensity(10.0, RoughnessReference)
		&& TurbulenceIntensity(10.0, RoughnessReference) < TurbulenceIntensity(10.0, RoughnessUrban)
		&& TurbulenceIntensity(100.0, RoughnessLand) < TurbulenceIntensity(10.0, RoughnessLand),
		"piu' rafficato sopra le citta' e vicino al suolo, meno sul mare",
		Fmt("mare %.2f, prato %.2f, citta' %.2f", TurbulenceIntensity(10.0, RoughnessWater),
			TurbulenceIntensity(10.0, RoughnessReference), TurbulenceIntensity(10.0, RoughnessUrban)));
}

static void TestVariabilityAndDeterminism()
{
	Section("5. Il tempo che cambia, e lo stesso vento per tutti");
	FWindField Field = MakeField(10.0, 270.0, 0.0, 1.0);
	double Low = 1e9, High = 0.0, LowDir = 1e9, HighDir = -1e9, Step = 0.0;
	double Previous = Field.ReferenceWind().SpeedMs;
	for (int Minute = 0; Minute < 24 * 60; ++Minute)
	{
		Field.Advance(60.0, Lat);
		const FWind Wind = Field.ReferenceWind();
		Low = std::min(Low, Wind.SpeedMs);
		High = std::max(High, Wind.SpeedMs);
		const double Turn = AngleDifference(Wind.FromDegrees, 270.0);
		LowDir = std::min(LowDir, Turn);
		HighDir = std::max(HighDir, Turn);
		Step = std::max(Step, std::fabs(Wind.SpeedMs - Previous));
		Previous = Wind.SpeedMs;
	}
	Check(Low > 2.0 && High < 18.0 && High - Low > 3.0 && HighDir - LowDir > 15.0 && Step < 1.0,
		"in un giorno: velocita' e direzione derivano, senza scatti",
		Fmt("da %.1f a %.1f m/s, direzione %+.0f..%+.0f°", Low, High, LowDir, HighDir) + Fmt(", passo massimo %.2f m/s al minuto", Step));

	FWindField Still = MakeField(10.0, 270.0, 0.0, 0.0);
	Still.Advance(5000.0, Lat);
	Check(Still.ReferenceWind().SpeedMs == 10.0 && Still.ReferenceWind().FromDegrees == 270.0, "variabilita' 0: il tempo resta quello");

	FWindField A = MakeField(10.0, 200.0, 1.0, 0.5), B = MakeField(10.0, 200.0, 1.0, 0.5);
	A.Advance(123.4, Lat);
	B.Advance(123.4, Lat);
	const FWindSample Sa = A.Sample(Lat, Lon, 25.0, 280.0, Flat(RoughnessLand));
	const FWindSample Sb = B.Sample(Lat, Lon, 25.0, 280.0, Flat(RoughnessLand));
	FWindSettings Other = A.GetSettings();
	Other.Seed = 2;
	FWindField C;
	C.SetSettings(Other);
	C.Advance(123.4, Lat);
	const FWindSample Sc = C.Sample(Lat, Lon, 25.0, 280.0, Flat(RoughnessLand));
	Check(Sa.North == Sb.North && Sa.East == Sb.East && Sa.Up == Sb.Up && (Sc.North != Sa.North || Sc.East != Sa.East),
		"stesso seme e stesso istante: lo stesso vento; altro seme: altre raffiche");

	FWindSettings Clamped;
	Clamped.Reference = FWind{ 500.0, -90.0 };
	Clamped.Turbulence = 7.0;
	FWindField D;
	D.SetSettings(Clamped);
	Check(D.GetSettings().Reference.SpeedMs == 80.0 && D.GetSettings().Reference.FromDegrees == 270.0
		&& D.GetSettings().Turbulence == 2.0, "impostazioni fuori scala: riportate dentro");
}


// ===========================================================================
//  Il meteo: pioggia, neve, grandine (Weather/Precipitation.h)
// ===========================================================================

static FPrecipitationField MakeWeather(EPrecipitationKind Kind, double Rate, double Coverage = 1.0,
                                       double SeaLevelC = 15.0, double CellKm = 8.0)
{
	FPrecipitationSettings Settings;
	Settings.Kind = Kind;
	Settings.RateMmH = Rate;
	Settings.Coverage = Coverage;
	Settings.SeaLevelTemperatureC = SeaLevelC;
	Settings.CellSizeKm = CellKm;
	FPrecipitationField Field;
	Field.SetSettings(Settings);
	return Field;
}

static void TestPhase()
{
	Section("6. Cosa cade, per quota: zero termico e quota neve");
	const FPrecipitationField Field = MakeWeather(EPrecipitationKind::Auto, 3.0, 1.0, 9.75);
	Check(std::fabs(Field.FreezingLevelM() - 1500.0) < 1e-6 && std::fabs(Field.SnowLineM() - 1200.0) < 1e-6
		&& std::fabs(Field.TemperatureAt(3000.0) + 9.75) < 1e-9,
		"9,75 °C al mare e 6,5 °C/km: zero termico a 1500 m, quota neve a 1200 m",
		Fmt("zero termico %.0f m, quota neve %.0f m", Field.FreezingLevelM(), Field.SnowLineM()));

	const FPrecipitationSample Valley = Field.Sample(Lat, Lon, 300.0);
	const FPrecipitationSample Mixed = Field.Sample(Lat, Lon, 1200.0);
	const FPrecipitationSample Peak = Field.Sample(Lat, Lon, 2500.0);
	Check(Valley.IsFalling() && Valley.Rain == 1.0 && Valley.Snow == 0.0 && std::fabs(Mixed.Snow - 0.5) < 1e-9
		&& Peak.Snow == 1.0 && Peak.Rain == 0.0,
		"stesso tempo: pioggia in valle, mista alla quota neve, neve in cima",
		Fmt("300 m neve %.0f%%, 1200 m %.0f%%, 2500 m %.0f%%", 100.0 * Valley.Snow, 100.0 * Mixed.Snow, 100.0 * Peak.Snow)); /* percentuale, non-unita */

	const FPrecipitationSample Forced = MakeWeather(EPrecipitationKind::Snow, 2.0, 1.0, 25.0).Sample(Lat, Lon, 0.0);
	const FPrecipitationSample Warm = MakeWeather(EPrecipitationKind::Rain, 2.0, 1.0, -10.0).Sample(Lat, Lon, 3000.0);
	Check(Forced.Snow == 1.0 && Warm.Rain == 1.0, "neve imposta: neve anche a 25 °C; pioggia imposta: pioggia anche a -30 °C");

	const FPrecipitationField Frozen = MakeWeather(EPrecipitationKind::Auto, 1.0, 1.0, -2.0);
	Check(Frozen.FreezingLevelM() == 0.0 && Frozen.Sample(Lat, Lon, 0.0).Snow == 1.0, "sotto zero al mare: neve dappertutto");
	Check(!MakeWeather(EPrecipitationKind::Auto, 0.0).Sample(Lat, Lon, 0.0).IsFalling(), "intensita' zero: sereno");

	// Sopra le nuvole non cade niente: 6 km per la pioggia estesa, 10 per i rovesci.
	const double Below = Field.Sample(Lat, Lon, 4900.0).RateMmH, Fading = Field.Sample(Lat, Lon, 5500.0).RateMmH;
	Check(Below > 0.0 && std::fabs(Fading - 0.5 * Below) < 0.05 * Below && !Field.Sample(Lat, Lon, 6100.0).IsFalling()
		&& Field.CloudTopM() == 6000.0
		&& MakeWeather(EPrecipitationKind::Hail, 3.0, 1.0, 9.75).CloudTopM() == 10000.0
		&& MakeWeather(EPrecipitationKind::Auto, 3.0, 0.5, 9.75).CloudTopM() == 10000.0,
		"cima delle nuvole: estesa 6 km (sfuma dall'ultimo km), rovesci e grandine 10 km",
		Fmt("4900 m %.2f mm/h, 5500 m %.2f", Below, Fading));
}

static void TestCells()
{
	Section("7. I rovesci: copertura, nuclei, trasporto, grandine");
	// La copertura chiesta e' la frazione di suolo sotto la precipitazione.
	for (const double Coverage : { 0.2, 0.5, 0.8 })
	{
		const FPrecipitationField Field = MakeWeather(EPrecipitationKind::Auto, 10.0, Coverage, 15.0, 6.0);
		int Wet = 0, Total = 0;
		double Strongest = 0.0;
		for (int Row = 0; Row < 120; ++Row)
		{
			for (int Column = 0; Column < 120; ++Column)
			{
				const double Rate = Field.Sample(Lat + 0.02 * Row, Lon + 0.028 * Column, 0.0).RateMmH;
				Wet += Rate > 0.0 ? 1 : 0;
				++Total;
				Strongest = std::max(Strongest, Rate);
			}
		}
		const double Fraction = static_cast<double>(Wet) / Total;
		Check(std::fabs(Fraction - Coverage) < 0.08 && Strongest <= 10.0 + 1e-9 && Strongest > 9.0,
			Fmt("copertura %.1f: piove su quella frazione di suolo, e nei nuclei si arriva al massimo", Coverage),
			Fmt("%.0f%% del suolo, massimo %.1f mm/h", 100.0 * Fraction, Strongest));   /* percentuale, non-unita */
	}

	// Trasporto: dopo 10 minuti con 15 m/s verso est, il rovescio e' 9 km piu' a est (e cambiato poco).
	FPrecipitationField Moving = MakeWeather(EPrecipitationKind::Auto, 10.0, 0.4, 15.0, 6.0);
	std::vector<double> Before;
	for (int Index = 0; Index < 400; ++Index) { Before.push_back(Moving.Intensity(Lat + 0.01 * Index, Lon)); }
	Moving.Advance(600.0, 0.0, 15.0);
	double Same = 0.0, Other = 0.0, Moved = 0.0;
	for (int Index = 0; Index < 400; ++Index)
	{
		const double La = Lat + 0.01 * Index;
		const double Downwind = Moving.Intensity(La, Lon + 9000.0 / (111320.0 * std::cos(La * WindDegToRad)));
		Moved += std::fabs(Downwind - Before[static_cast<size_t>(Index)]);
		Same += std::fabs(Moving.Intensity(La, Lon) - Before[static_cast<size_t>(Index)]);
		Other += Before[static_cast<size_t>(Index)];
	}
	Check(Moved < 0.5 * Same && Other > 10.0, "i rovesci viaggiano con il vento in quota",
		Fmt("scarto seguendo il vento %.1f, restando fermi %.1f", Moved, Same));

	// La grandine cade solo nei nuclei, mai dove piove poco.
	const FPrecipitationField Storm = MakeWeather(EPrecipitationKind::Hail, 40.0, 0.3, 22.0, 5.0);
	double HailInCores = 0.0, HailAtEdges = 0.0;
	int Cores = 0, Edges = 0;
	for (int Index = 0; Index < 20000; ++Index)
	{
		const FPrecipitationSample Sample = Storm.Sample(Lat + 0.007 * (Index % 150), Lon + 0.009 * (Index / 150), 200.0);
		if (!Sample.IsFalling()) { continue; }
		if (Sample.RateMmH > 0.9 * 40.0) { HailInCores += Sample.Hail; ++Cores; }
		if (Sample.RateMmH < 0.4 * 40.0) { HailAtEdges += Sample.Hail; ++Edges; }
	}
	Check(Cores > 0 && Edges > 0 && HailInCores / Cores > 0.5 && HailAtEdges == 0.0,
		"grandine nei nuclei del temporale (fino al 60%), niente ai margini",
		Fmt("nei nuclei %.0f%%, ai margini %.0f%%", 100.0 * HailInCores / std::max(Cores, 1),     /* percentuale, non-unita */
			100.0 * HailAtEdges / std::max(Edges, 1)));   /* percentuale, non-unita */

	Check(std::fabs(PrecipitationDetail::InverseNormal(0.975) - 1.959964) < 1e-5
		&& std::fabs(PrecipitationDetail::InverseNormal(0.5)) < 1e-12, "l'inversa della normale (la soglia della copertura)");
}

static void TestVisibilityAndFall()
{
	Section("8. Visibilita' e caduta");
	const double Light = PrecipitationVisibilityM(1.0, 1.0, 0.0, 0.0);
	const double Heavy = PrecipitationVisibilityM(50.0, 1.0, 0.0, 0.0);
	const double Snow = PrecipitationVisibilityM(1.0, 0.0, 1.0, 0.0);
	Check(Light > 12000.0 && Light < 15000.0 && Heavy > 800.0 && Heavy < 1200.0 && Snow > 900.0 && Snow < 1100.0
		&& std::isinf(PrecipitationVisibilityM(0.0, 1.0, 0.0, 0.0)),
		"pioggia debole 13 km, a dirotto 1 km; 1 mm/h di neve: 1 km; sereno: nessun limite",
		Fmt("%.1f km, %.1f km, neve %.2f km", Light / 1000.0, Heavy / 1000.0, Snow / 1000.0));
	Check(RainFallSpeed(1.0) > 3.0 && RainFallSpeed(1.0) < 5.0 && RainFallSpeed(50.0) > 6.0 && RainFallSpeed(50.0) < 8.0
		&& SnowFallSpeed(-10.0) < 1.0 && SnowFallSpeed(1.0) > 1.4 && HailFallSpeed(0.01) == 14.5 && HailFallSpeed(0.02) > 20.0,
		"le gocce cadono a 4-7 m/s, i fiocchi a ~1, i chicchi da 1 cm a 14,5 (da 2 cm oltre 20)",
		Fmt("gocce %.1f e %.1f, fiocchi %.1f, chicchi %.1f m/s", RainFallSpeed(1.0), RainFallSpeed(50.0), SnowFallSpeed(-10.0), HailFallSpeed(0.02)));
}

static void TestGround()
{
	Section("9. Il suolo: bagnato, asciutto, neve che scende e fonde");
	FPrecipitationField Field = MakeWeather(EPrecipitationKind::Auto, 4.0, 1.0, 9.75);    // quota neve 1200 m
	FGroundWeather Ground;
	FPrecipitationSample Valley = Field.Sample(Lat, Lon, 200.0);
	for (int Minute = 0; Minute < 5; ++Minute) { Ground.Advance(60.0, Valley, Field); }
	const double WetAfterRain = Ground.GetWetness();
	// La pioggia in valle e' neve in montagna: la copertura scende verso la quota neve.
	for (int Minute = 0; Minute < 55; ++Minute) { Ground.Advance(60.0, Valley, Field); }
	const double CoverAfterHour = Ground.GetSnowCoverM();
	Check(WetAfterRain > 0.9 && CoverAfterHour < 1300.0 && CoverAfterHour >= 1200.0,
		"dopo 5 minuti di pioggia il suolo e' bagnato; dopo un'ora la neve e' scesa fino alla quota neve",
		Fmt("bagnato %.2f, neve sopra %.0f m", WetAfterRain, CoverAfterHour));

	FPrecipitationSettings Sunny = Field.GetSettings();
	Sunny.RateMmH = 0.0;
	Field.SetSettings(Sunny);
	const FPrecipitationSample Dry = Field.Sample(Lat, Lon, 200.0);
	for (int Minute = 0; Minute < 120; ++Minute) { Ground.Advance(60.0, Dry, Field); }
	Check(Ground.GetWetness() == 0.0 && Ground.GetSnowCoverM() > CoverAfterHour,
		"smette: in due ore il suolo si asciuga, e la neve risale fondendo dove fa piu' di zero",
		Fmt("bagnato %.2f, neve sopra %.0f m", Ground.GetWetness(), Ground.GetSnowCoverM()));

	FPrecipitationField Cold = MakeWeather(EPrecipitationKind::Auto, 0.0, 1.0, -5.0);
	FGroundWeather Winter;
	Winter.SetSnowCover(300.0);
	for (int Hour = 0; Hour < 24; ++Hour) { Winter.Advance(3600.0, Cold.Sample(Lat, Lon, 300.0), Cold); }
	Check(Winter.GetSnowCoverM() == 300.0, "sotto zero la neve non fonde");

	FPrecipitationField Blizzard = MakeWeather(EPrecipitationKind::Snow, 2.0);
	FGroundWeather Plain;
	for (int Minute = 0; Minute < 90; ++Minute) { Plain.Advance(60.0, Blizzard.Sample(Lat, Lon, 0.0), Blizzard); }
	Check(Plain.HasSnow() && Plain.GetSnowCoverM() < 0.0, "neve imposta: in un'ora e mezza imbianca fino al mare",
		Fmt("neve sopra %.0f m", Plain.GetSnowCoverM()));
}

static void TestParticles()
{
	Section("10. Le particelle attorno alla camera");
	FPrecipitationParticles Particles;
	FPrecipitationSample Mixed;
	Mixed.RateMmH = 10.0;
	Mixed.Rain = 0.5;
	Mixed.Snow = 0.3;
	Mixed.Hail = 0.2;
	Mixed.TemperatureC = 1.0;
	const double Camera[3] = { 1234.5, -987.6, 300.0 };
	Particles.Advance(1.0 / 60.0, Mixed, 4.0, 0.0, 0.0, Camera);
	const FParticleLayer& Rain = Particles.GetLayer(EParticleType::Rain);
	const FParticleLayer& Snow = Particles.GetLayer(EParticleType::Snow);
	const FParticleLayer& Hail = Particles.GetLayer(EParticleType::Hail);
	Check(Rain.Velocity[2] < -3.0 && Snow.Velocity[2] > -2.0 && Hail.Velocity[2] < Rain.Velocity[2]
		&& Snow.Velocity[0] == 4.0 && Rain.Velocity[0] < 4.0 && Hail.Velocity[0] < Rain.Velocity[0],
		"cadono ognuna alla sua velocita', e il vento porta di piu' i fiocchi",
		Fmt("verticale: gocce %.1f, fiocchi %.1f, chicchi %.1f m/s", Rain.Velocity[2], Snow.Velocity[2], Hail.Velocity[2]));
	double MixRain = 0.0, MixSnow = 0.0, MixHail = 0.0;
	Particles.GetMix(MixRain, MixSnow, MixHail);
	Check(std::fabs(MixRain + MixSnow + MixHail - 1.0) < 1e-12 && MixRain > 0.0 && MixSnow > 0.0 && MixHail > 0.0
		&& Rain.Size > 0.05 && Rain.Size < 0.4 && Hail.Size >= 0.005,
		"miscela: le tre particelle insieme; scia delle gocce fra 5 e 40 cm", Fmt("scia %.2f m, chicchi %.3f m", Rain.Size, Hail.Size));

	// Fase continua: muovendo la camera di 1 cm, la fase cambia di 1 cm (le particelle stanno ferme nel mondo).
	const double Before = Rain.Phase[0];
	const double Moved[3] = { Camera[0] + 0.01, Camera[1], Camera[2] };
	Particles.Advance(0.0, Mixed, 4.0, 0.0, 0.0, Moved);
	Check(std::fabs((Before - Particles.GetLayer(EParticleType::Rain).Phase[0]) - 0.01) < 1e-9
		&& std::fabs(Particles.GetLayer(EParticleType::Rain).Phase[0]) <= 0.5 * Particles.Box[0],
		"la camera che si sposta non trascina le particelle; la fase resta nella scatola");

	// Continuita' nel tempo: un frame sposta le gocce di v dt (modulo la scatola).
	const double Start = Particles.GetLayer(EParticleType::Rain).Phase[2];
	Particles.Advance(1.0 / 60.0, Mixed, 4.0, 0.0, 0.0, Moved);
	const double Step = FPrecipitationParticles::Wrap(Particles.GetLayer(EParticleType::Rain).Phase[2] - Start, Particles.Box[2]);
	Check(std::fabs(Step - Particles.GetLayer(EParticleType::Rain).Velocity[2] / 60.0) < 1e-9, "a ogni frame cadono di v dt, senza salti",
		Fmt("%.3f m", Step));

	FPrecipitationSample Clear;
	Particles.Advance(1.0 / 60.0, Clear, 4.0, 0.0, 0.0, Moved);
	Check(Particles.GetLayer(EParticleType::Rain).Density == 0.0 && Particles.GetLayer(EParticleType::Snow).Density == 0.0
		&& Particles.GetLayer(EParticleType::Hail).Density == 0.0, "sereno: nessuna particella");

	FPrecipitationMesh Mesh;
	BuildPrecipitationMesh(1000, Particles.Box, 3, Mesh);
	bool bShape = Mesh.Positions.size() == 12000 && Mesh.Indices.size() == 6000 && Mesh.Seeds.size() == 8000;
	for (size_t Quad = 0; bShape && Quad < 1000; ++Quad)
	{
		for (int Corner = 1; Corner < 4; ++Corner)
		{
			for (int Axis = 0; Axis < 3; ++Axis)
			{
				bShape = bShape && Mesh.Positions[(Quad * 4 + Corner) * 3 + Axis] == Mesh.Positions[Quad * 4 * 3 + Axis];
			}
		}
		for (int Axis = 0; Axis < 3; ++Axis)
		{
			bShape = bShape && std::fabs(Mesh.Positions[Quad * 12 + Axis]) <= 0.5 * Particles.Box[Axis];
		}
		bShape = bShape && Mesh.Seeds[Quad * 8] >= 0.0f && Mesh.Seeds[Quad * 8] < 1.0f;
	}
	Check(bShape, "la mesh: 4 vertici nello stesso punto per particella, tutti nella scatola, semi in [0, 1)");
}

// ===========================================================================
//  11-13. I fulmini
// ===========================================================================

static FLightningScheduler MakeLightning(double Activity, uint32_t Seed = 11)
{
	FLightningSettings Settings;
	Settings.Activity = Activity;
	Settings.Seed = Seed;
	FLightningScheduler Scheduler;
	Scheduler.SetSettings(Settings);
	return Scheduler;
}

/** Un'ora di fulmini attorno a (Lat, Lon), con la mappa della precipitazione ferma. */
static std::vector<FLightningStrike> OneHour(FLightningScheduler& Scheduler, const FPrecipitationField& Field)
{
	std::vector<FLightningStrike> Strikes;
	for (int Step = 0; Step < 7200; ++Step) { Scheduler.Advance(0.5, Lat, Lon, Field, Strikes); }
	return Strikes;
}

static double DistanceFromCentreM(double LatDeg, double LonDeg)
{
	const double North = (LatDeg - Lat) * 111132.0;
	const double East = (LonDeg - Lon) * 111320.0 * std::cos(Lat * WindDegToRad);
	return std::sqrt(North * North + East * East);
}

static void TestLightningWhere()
{
	Section("11. I fulmini: dove e quanti");
	const FPrecipitationField Storms = MakeWeather(EPrecipitationKind::Auto, 30.0, 0.4, 15.0, 6.0);

	FLightningScheduler Off = MakeLightning(0.0);
	Check(OneHour(Off, Storms).empty(), "attivita' zero: nessun fulmine");
	FLightningScheduler Dry = MakeLightning(1.0);
	Check(OneHour(Dry, MakeWeather(EPrecipitationKind::Auto, 0.0, 0.4)).empty(), "senza pioggia non fulmina");

	// Quanti ne aspettiamo: la densita' nei nuclei per quanto la mappa e' "nucleo", su tutto il cerchio.
	double CoreKm2 = 0.0;
	const int Grid = 240;
	const double Cell = 60000.0 / Grid;
	for (int Row = 0; Row < Grid; ++Row)
	{
		for (int Column = 0; Column < Grid; ++Column)
		{
			const double North = -30000.0 + (Row + 0.5) * Cell, East = -30000.0 + (Column + 0.5) * Cell;
			if (North * North + East * East > 30000.0 * 30000.0) { continue; }
			const double LatDeg = Lat + North / 111132.0, LonDeg = Lon + East / (111320.0 * std::cos(Lat * WindDegToRad));
			CoreKm2 += LightningDetail::ConvectiveWeight(Storms, LatDeg, LonDeg) * Cell * Cell / 1.0e6;
		}
	}
	const double Expected = CoreFlashDensityPerKm2PerMinute * CoreKm2 * 60.0;
	FLightningScheduler Normal = MakeLightning(1.0);
	const std::vector<FLightningStrike> Strikes = OneHour(Normal, Storms);
	Check(std::fabs(static_cast<double>(Strikes.size()) - Expected) < 0.06 * Expected,
		"in un'ora, quanti la densita' nei nuclei promette (0,25 al km2 al minuto)",
		Fmt("%.0f fulmini, attesi %.0f (nuclei: %.0f km2)", static_cast<double>(Strikes.size()), Expected, CoreKm2));

	double WeightOfStrikes = 0.0, Farthest = 0.0;
	int CloudToGround = 0;
	bool bAllInCores = true;
	for (const FLightningStrike& Strike : Strikes)
	{
		const double Weight = LightningDetail::ConvectiveWeight(Storms, Strike.LatDeg, Strike.LonDeg);
		bAllInCores = bAllInCores && Weight > 0.0;
		WeightOfStrikes += Weight;
		Farthest = std::max(Farthest, DistanceFromCentreM(Strike.LatDeg, Strike.LonDeg));
		CloudToGround += Strike.Kind == ELightningKind::CloudToGround ? 1 : 0;
	}
	const double MeanAreaWeight = CoreKm2 / (3.141592653589793 * 900.0);
	Check(bAllInCores && WeightOfStrikes / Strikes.size() > 3.0 * MeanAreaWeight && Farthest <= 30000.0,
		"tutti nei nuclei dei temporali, e entro 30 km",
		Fmt("nucleo medio dei fulmini %.2f, del cerchio %.2f, il piu' lontano a %.1f km",
			WeightOfStrikes / Strikes.size(), MeanAreaWeight, Farthest / 1000.0));
	const double Fraction = static_cast<double>(CloudToGround) / Strikes.size();
	Check(std::fabs(Fraction - 0.3) < 0.03, "circa uno su tre arriva al suolo, gli altri restano nella nube",
		Fmt("nube-suolo %.0f%%", 100.0 * Fraction)); /* percentuale, non-unita */

	FLightningScheduler Severe = MakeLightning(3.0);
	const size_t SevereCount = OneHour(Severe, Storms).size();
	FLightningScheduler Front = MakeLightning(1.0);
	const size_t FrontCount = OneHour(Front, MakeWeather(EPrecipitationKind::Auto, 30.0, 1.0)).size();
	Check(std::fabs(static_cast<double>(SevereCount) / Strikes.size() - 3.0) < 0.3 && FrontCount < Strikes.size() / 5,
		"attivita' 3: il triplo; una pioggia estesa (un fronte) fulmina poco",
		Fmt("x%.2f; fronte %.0f contro %.0f", static_cast<double>(SevereCount) / Strikes.size(),
			static_cast<double>(FrontCount), static_cast<double>(Strikes.size())));

	FLightningScheduler Again = MakeLightning(1.0);
	const std::vector<FLightningStrike> Repeat = OneHour(Again, Storms);
	Check(Repeat.size() == Strikes.size() && Repeat.front().LatDeg == Strikes.front().LatDeg
		&& Repeat.back().TimeSeconds == Strikes.back().TimeSeconds, "stesso seme, stessi fulmini");
}

static void TestLightningFlashAndShape()
{
	Section("12. Il lampo e il canale");
	FLightningScheduler Scheduler = MakeLightning(1.0);
	FLightningStrike Strike = Scheduler.Strike(ELightningKind::CloudToGround, Lat, Lon);
	for (int Attempt = 0; Attempt < 50 && Strike.Strokes < 3; ++Attempt)
	{
		Strike = Scheduler.Strike(ELightningKind::CloudToGround, Lat, Lon);
	}
	int Peaks = 0;
	double Previous = -1.0, Before = -1.0;
	for (double Time = -0.005; Time <= FlashDuration(Strike) + 0.05; Time += 0.001)
	{
		const double Light = FlashBrightness(Strike, Time);
		if (Before >= 0.0 && Previous > Before && Previous >= Light) { ++Peaks; }
		Before = Previous;
		Previous = Light;
	}
	const double Between = 0.5 * (Strike.StrokeTimes[0] + Strike.StrokeTimes[1]);
	Check(Strike.Strokes >= 3 && Peaks == Strike.Strokes && FlashBrightness(Strike, 0.0) >= 1.0
		&& FlashBrightness(Strike, Between) < 0.8 * FlashBrightness(Strike, Strike.StrokeTimes[1])
		&& FlashBrightness(Strike, FlashDuration(Strike) + 0.01) == 0.0 && FlashBrightness(Strike, -0.01) == 0.0,
		"il lampo sfarfalla: un picco per ogni colpo di ritorno, buio prima e dopo",
		Fmt("%.0f colpi, %.0f picchi, dura %.0f ms", Strike.Strokes, Peaks, 1000.0 * FlashDuration(Strike)));

	double Strokes = 0.0, Interval = 0.0, ShortestGap = 1.0, LongestGap = 0.0;
	int Gaps = 0;
	for (int Index = 0; Index < 2000; ++Index)
	{
		const FLightningStrike Sample = Scheduler.Strike(ELightningKind::CloudToGround, Lat, Lon);
		Strokes += Sample.Strokes;
		for (int Stroke = 1; Stroke < Sample.Strokes; ++Stroke)
		{
			const double Gap = Sample.StrokeTimes[Stroke] - Sample.StrokeTimes[Stroke - 1];
			Interval += Gap;
			ShortestGap = std::min(ShortestGap, Gap);
			LongestGap = std::max(LongestGap, Gap);
			++Gaps;
		}
	}
	Check(Strokes / 2000.0 > 2.8 && Strokes / 2000.0 < 4.0 && ShortestGap >= 0.03 && LongestGap <= 0.09,
		"in media 3 colpi per fulmine (fino a 8), a 30-90 ms l'uno dall'altro",
		Fmt("%.2f colpi, %.0f ms in media", Strokes / 2000.0, 1000.0 * Interval / Gaps));

	const double Height = 3000.0;
	std::vector<FBoltSegment> Channel;
	BuildLightningChannel(Height, 1234u, Channel);
	bool bContinuous = true;
	for (int Index = 0; Index + 1 < MainChannelSegments; ++Index)
	{
		for (int Axis = 0; Axis < 3; ++Axis) { bContinuous = bContinuous && Channel[Index].B[Axis] == Channel[Index + 1].A[Axis]; }
	}
	const FBoltSegment& Last = Channel[MainChannelSegments - 1];
	double LowestBranch = 1.0e9, Widest = 0.0, HeaviestBranch = 0.0;
	for (size_t Index = 0; Index < Channel.size(); ++Index)
	{
		const FBoltSegment& Segment = Channel[Index];
		for (int Axis = 0; Axis < 2; ++Axis) { Widest = std::max({ Widest, std::fabs(Segment.A[Axis]), std::fabs(Segment.B[Axis]) }); }
		if (Index >= static_cast<size_t>(MainChannelSegments))
		{
			LowestBranch = std::min({ LowestBranch, Segment.A[2], Segment.B[2] });
			HeaviestBranch = std::max(HeaviestBranch, Segment.Weight);
		}
	}
	Check(bContinuous && Channel[0].A[2] == Height && Last.B[0] == 0.0 && Last.B[1] == 0.0 && Last.B[2] == 0.0,
		"il canale principale e' un filo continuo dalla nube al punto colpito");
	Check(Channel.size() > static_cast<size_t>(MainChannelSegments) && LowestBranch > 0.1 * Height && HeaviestBranch <= 0.35
		&& Widest < 1.2 * Height && Channel.size() < 600,
		"i rami: piu' deboli, si fermano a mezz'aria; tutto entro una larghezza ragionevole",
		Fmt("%.0f pezzi, il ramo piu' basso a %.0f m, largo %.0f m", static_cast<double>(Channel.size()), LowestBranch, Widest));
	Check(Channel[0].Weight < 0.2 && Channel[MainChannelSegments / 2].Weight == 1.0,
		"in cima il canale sfuma (entra nella nube); a meta' e' pieno");
	std::vector<FBoltSegment> Same, Other;
	BuildLightningChannel(Height, 1234u, Same);
	BuildLightningChannel(Height, 1235u, Other);
	Check(Same.size() == Channel.size() && Same[40].A[0] == Channel[40].A[0] && Other[40].A[0] != Channel[40].A[0],
		"stesso seme, stessa forma; seme diverso, forma diversa");
}

/** Quante volte il segnale cambia segno al secondo, nei primi Seconds: alto = acuto. */
static double ZeroCrossingsPerSecond(const FThunder& Thunder, double Seconds)
{
	const size_t Count = std::min(Thunder.Samples.size(), static_cast<size_t>(Seconds * Thunder.SampleRate));
	int Crossings = 0;
	for (size_t Index = 1; Index < Count; ++Index)
	{
		Crossings += (Thunder.Samples[Index - 1] < 0.0f) != (Thunder.Samples[Index] < 0.0f) ? 1 : 0;
	}
	return Crossings / (static_cast<double>(Count) / Thunder.SampleRate);
}

static void TestThunder()
{
	Section("13. Il tuono");
	std::vector<FBoltSegment> Channel;
	BuildLightningChannel(3000.0, 77u, Channel);

	// A 3,43 km: tre secondi per chilometro, cioe' ~10 s a 15 °C.
	const double Far[3] = { 3430.0, 0.0, 2.0 };
	FThunder Thunder;
	const bool bHeard = SynthesizeThunder(Channel, Far, 15.0, 1u, 22050, Thunder);
	Check(bHeard && std::fabs(Thunder.DelaySeconds - Thunder.NearestM / 340.39) < 1e-3
		&& Thunder.DelaySeconds > 9.0 && Thunder.DelaySeconds < 10.5,
		"il tuono arriva dopo distanza / velocita' del suono: 3 secondi per chilometro",
		Fmt("a %.2f km: %.2f s", Thunder.NearestM / 1000.0, Thunder.DelaySeconds));

	bool bClean = true;
	for (float Sample : Thunder.Samples) { bClean = bClean && std::isfinite(Sample) && std::fabs(Sample) <= 0.951f; }
	FThunder Again;
	SynthesizeThunder(Channel, Far, 15.0, 1u, 22050, Again);
	Check(bClean && Again.Samples == Thunder.Samples, "campioni puliti in [-1, 1], e ripetibili");

	// Da vicino uno schiocco, da lontano un brontolio: l'aria si mangia gli acuti.
	const double Near[3] = { 300.0, 0.0, 2.0 };
	const double Distant[3] = { 12000.0, 0.0, 2.0 };
	FThunder Close, Rumble;
	SynthesizeThunder(Channel, Near, 15.0, 2u, 22050, Close);
	SynthesizeThunder(Channel, Distant, 15.0, 3u, 22050, Rumble);
	const double CloseRate = ZeroCrossingsPerSecond(Close, 0.3), RumbleRate = ZeroCrossingsPerSecond(Rumble, 0.3);
	Check(CloseRate > 3.0 * RumbleRate, "da vicino acuto (uno schiocco), da lontano basso (un brontolio)",
		Fmt("%.0f contro %.0f passaggi per lo zero al secondo", CloseRate, RumbleRate));

	// Il rombo: dura quanto la differenza di distanza fra i pezzi del canale.
	double Nearest = 1e9, Farthest = 0.0;
	for (const FBoltSegment& Segment : Channel)
	{
		const double Dx = 0.5 * (Segment.A[0] + Segment.B[0]) - Near[0], Dy = 0.5 * (Segment.A[1] + Segment.B[1]) - Near[1];
		const double Dz = 0.5 * (Segment.A[2] + Segment.B[2]) - Near[2];
		Nearest = std::min(Nearest, std::sqrt(Dx * Dx + Dy * Dy + Dz * Dz));
		Farthest = std::max(Farthest, std::sqrt(Dx * Dx + Dy * Dy + Dz * Dz));
	}
	const double Spread = (Farthest - Nearest) / 340.39;
	const double Length = static_cast<double>(Close.Samples.size()) / Close.SampleRate;
	Check(Spread > 5.0 && std::fabs(Length - (Spread + 1.5 + 2.5 * Nearest / 10000.0 + 0.3)) < 0.05,
		"il rombo dura quanto il suono ci mette a percorrere la differenza fra il pezzo piu' vicino e il piu' lontano",
		Fmt("canale da %.0f a %.0f m: %.1f s di rombo", Nearest, Farthest, Spread));

	FThunder TooFar;
	const double Away[3] = { 30000.0, 0.0, 2.0 };
	const bool bHeardFromAfar = SynthesizeThunder(Channel, Away, 15.0, 4u, 22050, TooFar);
	Check(Close.Loudness == 1.0 && std::fabs(Rumble.Loudness - 1000.0 / Rumble.NearestM) < 1e-9 && !bHeardFromAfar,
		"forte da vicino, piu' piano con la distanza; oltre 25 km non si sente",
		Fmt("volume a %.0f km: %.2f", Rumble.NearestM / 1000.0, Rumble.Loudness));
	Check(std::fabs(SpeedOfSound(15.0) - 340.39) < 0.01 && std::fabs(SpeedOfSound(-10.0) - 325.24) < 0.01,
		"la velocita' del suono cresce con la temperatura (340 m/s a 15 °C)");
}

int main()
{
	std::printf("=====================================================\n");
	std::printf(" GeoWeather -- il vento e il meteo, senza Unreal\n");
	std::printf("=====================================================\n");
	TestProfile();
	TestAloft();
	TestOrography();
	TestTurbulence();
	TestVariabilityAndDeterminism();
	TestPhase();
	TestCells();
	TestVisibilityAndFall();
	TestGround();
	TestParticles();
	TestLightningWhere();
	TestLightningFlashAndShape();
	TestThunder();
	std::printf("\n=====================================================\n");
	std::printf(" RISULTATO: %d passati, %d falliti\n", GPassed, GFailed);
	std::printf("=====================================================\n");
	return GFailed == 0 ? 0 : 1;
}

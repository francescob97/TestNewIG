// =============================================================================
//  geoweather_main.cpp -- Il vento (simulazione), senza Unreal.
//
//  Prova Weather/WindField.h: profilo e rugosita', rotazione con la quota,
//  venti in quota, rilievi (cresta, valle, ridosso, vento verticale),
//  turbolenza (intensita', coerenza, trasporto, continuita'), variabilita',
//  determinismo.
// =============================================================================

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

int main()
{
	std::printf("=====================================================\n");
	std::printf(" GeoWeather -- il vento, senza Unreal\n");
	std::printf("=====================================================\n");
	TestProfile();
	TestAloft();
	TestOrography();
	TestTurbulence();
	TestVariabilityAndDeterminism();
	std::printf("\n=====================================================\n");
	std::printf(" RISULTATO: %d passati, %d falliti\n", GPassed, GFailed);
	std::printf("=====================================================\n");
	return GFailed == 0 ? 0 : 1;
}

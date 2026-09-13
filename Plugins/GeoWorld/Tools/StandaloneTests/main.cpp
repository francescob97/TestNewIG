// =============================================================================
//  Test numerici dello strato puro di GeoCore. Nessuna dipendenza da Unreal.
//  Le soglie sono esplicite e volutamente strette: un test che passa sempre non
//  serve a niente.
// =============================================================================
#include "Geo/Georeference.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <random>
#include <string>
#include <vector>

using namespace GeoWorld::Core;
namespace Units = GeoWorld::Units;

// ---------------------------------------------------------------------------
//  Micro-framework: nessuna libreria esterna, output leggibile.
// ---------------------------------------------------------------------------
static int GNumFailed = 0;
static int GNumPassed = 0;

static void Check(bool bCondition, const std::string& What, const std::string& Detail = {})
{
	if (bCondition)
	{
		++GNumPassed;
		std::printf("  [ ok ] %s%s%s\n", What.c_str(),
			Detail.empty() ? "" : "  -> ", Detail.c_str());
	}
	else
	{
		++GNumFailed;
		std::printf("  [FAIL] %s%s%s\n", What.c_str(),
			Detail.empty() ? "" : "  -> ", Detail.c_str());
	}
}

static std::string Fmt(const char* Format, double Value)
{
	char Buffer[256];
	std::snprintf(Buffer, sizeof(Buffer), Format, Value);
	return Buffer;
}

static void Section(const char* Title)
{
	std::printf("\n== %s ==\n", Title);
}

static double EcefDistance(const FEcef& A, const FEcef& B)
{
	return (A - B).Length();
}

// ===========================================================================
//  TEST 1 -- Round-trip geodetiche -> ECEF -> geodetiche su griglia globale.
//
//  La metrica primaria e' l'errore di POSIZIONE in metri (ben condizionato
//  ovunque, poli compresi). Lat/lon si controllano a parte, escludendo la
//  fascia polare dove la longitudine e' matematicamente mal condizionata:
//  a 89.9999 gradi di latitudine un metro di spostamento vale gradi interi
//  di longitudine, quindi un errore in gradi li' non significa nulla.
// ===========================================================================
static void TestRoundTripGrid()
{
	Section("1. Round-trip geodetic -> ECEF -> geodetic (griglia globale)");

	double MaxPosErrM   = 0.0;
	double MaxLatErrDeg = 0.0;
	double MaxLonErrDeg = 0.0;
	double MaxHeightErr = 0.0;
	long   Count        = 0;

	for (int LatIdx = 0; LatIdx <= 180; ++LatIdx)
	{
		const double LatDeg = -90.0 + LatIdx * 1.0;

		for (int LonIdx = 0; LonIdx <= 120; ++LonIdx)
		{
			const double LonDeg = -180.0 + LonIdx * 3.0;

			for (double H : { -500.0, -100.0, 0.0, 1.0, 250.0, 1500.0, 4808.0, 9000.0 })
			{
				const FGeodetic In  = FGeodetic::FromDegrees(LatDeg, LonDeg, H);
				const FEcef     Ecef = GeodeticToEcef(In);
				const FGeodetic Out  = EcefToGeodetic(Ecef);

				// Errore di posizione: riconverto l'uscita e misuro la distanza.
				MaxPosErrM = std::max(MaxPosErrM, EcefDistance(Ecef, GeodeticToEcef(Out)));
				MaxHeightErr = std::max(MaxHeightErr, std::abs(Out.HeightM - In.HeightM));

				if (std::abs(LatDeg) < 89.5)
				{
					MaxLatErrDeg = std::max(MaxLatErrDeg, std::abs(Out.LatDeg() - In.LatDeg()));

					// Gestisce il salto +180 / -180.
					double LonErr = std::abs(Out.LonDeg() - In.LonDeg());
					if (LonErr > 180.0) { LonErr = 360.0 - LonErr; }
					MaxLonErrDeg = std::max(MaxLonErrDeg, LonErr);
				}
				++Count;
			}
		}
	}

	std::printf("  punti testati: %ld\n", Count);
	Check(MaxPosErrM < 1e-6,     "errore di posizione < 1e-6 m",  Fmt("max %.3e m",   MaxPosErrM));
	Check(MaxHeightErr < 1e-6,   "errore di quota < 1e-6 m",      Fmt("max %.3e m",   MaxHeightErr));
	Check(MaxLatErrDeg < 1e-11,  "errore di latitudine < 1e-11 deg",  Fmt("max %.3e deg", MaxLatErrDeg));
	Check(MaxLonErrDeg < 1e-11,  "errore di longitudine < 1e-11 deg", Fmt("max %.3e deg", MaxLonErrDeg));
}

// ===========================================================================
//  TEST 2 -- Bowring contro l'iterativo di riferimento.
//  Due algoritmi indipendenti, due punti di partenza diversi. Se concordano,
//  e' molto improbabile che abbiano lo stesso bug.
// ===========================================================================
static void TestBowringVsIterative()
{
	Section("2. Bowring (produzione) vs iterativo (riferimento)");

	double MaxLatDiffRad = 0.0;
	double MaxHeightDiff = 0.0;
	long   Count = 0;

	std::mt19937_64 Rng(20260913);
	std::uniform_real_distribution<double> LatDist(-89.999, 89.999);
	std::uniform_real_distribution<double> LonDist(-180.0, 180.0);
	std::uniform_real_distribution<double> HDist(-11000.0, 30000.0);

	for (int i = 0; i < 200000; ++i)
	{
		const FGeodetic In = FGeodetic::FromDegrees(LatDist(Rng), LonDist(Rng), HDist(Rng));
		const FEcef Ecef = GeodeticToEcef(In);

		const FGeodetic Fast = EcefToGeodetic(Ecef);
		const FGeodetic Ref  = EcefToGeodeticIterative(Ecef);

		MaxLatDiffRad = std::max(MaxLatDiffRad, std::abs(Fast.LatRad  - Ref.LatRad));
		MaxHeightDiff = std::max(MaxHeightDiff, std::abs(Fast.HeightM - Ref.HeightM));
		++Count;
	}

	// 1e-13 rad sul raggio terrestre vale circa 0.6 micrometri.
	std::printf("  punti casuali: %ld (quote -11 km .. +30 km)\n", Count);
	Check(MaxLatDiffRad < 1e-13, "differenza di latitudine < 1e-13 rad", Fmt("max %.3e rad", MaxLatDiffRad));
	Check(MaxHeightDiff < 1e-6,  "differenza di quota < 1e-6 m",         Fmt("max %.3e m",   MaxHeightDiff));
}

// ===========================================================================
//  TEST 3 -- Valori analitici noti. Sono gli unici test che verificano che non
//  ci sia un errore sistematico condiviso da entrambe le implementazioni.
// ===========================================================================
static void TestKnownValues()
{
	Section("3. Valori analitici noti");

	// Costanti derivate dell'ellissoide WGS84, contro i valori pubblicati.
	Check(std::abs(WGS84.B   - 6356752.314245179497)  < 1e-6,  "semiasse minore b", Fmt("%.9f m", WGS84.B));
	Check(std::abs(WGS84.E2  - 0.00669437999014133)   < 1e-15, "e^2",               Fmt("%.17f", WGS84.E2));
	Check(std::abs(WGS84.EP2 - 0.00673949674227643)   < 1e-15, "e'^2",              Fmt("%.17f", WGS84.EP2));

	// Equatore, meridiano di Greenwich, quota zero: deve dare esattamente (a,0,0).
	{
		const FEcef P = GeodeticToEcef(FGeodetic::FromDegrees(0.0, 0.0, 0.0));
		Check(std::abs(P.X - WGS84.A) < 1e-9 && std::abs(P.Y) < 1e-9 && std::abs(P.Z) < 1e-9,
			"lat 0, lon 0, h 0 -> (a, 0, 0)",
			Fmt("X = %.6f m", P.X));
	}

	// Polo Nord: deve dare esattamente (0,0,b).
	{
		const FEcef P = GeodeticToEcef(FGeodetic::FromDegrees(90.0, 0.0, 0.0));
		Check(std::abs(P.Z - WGS84.B) < 1e-9 && std::hypot(P.X, P.Y) < 1e-9,
			"polo Nord -> (0, 0, b)", Fmt("Z = %.9f m", P.Z));

		// ...e la conversione inversa deve tornare indietro senza dividere per zero.
		const FGeodetic G = EcefToGeodetic(P);
		Check(std::abs(G.LatDeg() - 90.0) < 1e-12 && std::abs(G.HeightM) < 1e-9,
			"polo Nord -> ECEF -> geodetiche (caso limite)",
			Fmt("h = %.3e m", G.HeightM));
	}

	// Equatore a 90 gradi Est.
	{
		const FEcef P = GeodeticToEcef(FGeodetic::FromDegrees(0.0, 90.0, 0.0));
		Check(std::abs(P.Y - WGS84.A) < 1e-6 && std::abs(P.X) < 1e-6 && std::abs(P.Z) < 1e-9,
			"lat 0, lon 90E -> (0, a, 0)", Fmt("Y = %.6f m", P.Y));
	}

	// Quota: salire di 1000 m sul polo aumenta |Z| di esattamente 1000 m.
	{
		const FEcef P0 = GeodeticToEcef(FGeodetic::FromDegrees(90.0, 0.0, 0.0));
		const FEcef P1 = GeodeticToEcef(FGeodetic::FromDegrees(90.0, 0.0, 1000.0));
		Check(std::abs((P1.Z - P0.Z) - 1000.0) < 1e-9, "1000 m di quota = 1000 m di ECEF al polo",
			Fmt("delta = %.9f m", P1.Z - P0.Z));
	}

	// La normale geodetica NON coincide con quella geocentrica: verifichiamo che
	// la differenza sia quella attesa (massima a 45 gradi, circa 11.5 primi).
	{
		const FGeodetic G = FGeodetic::FromDegrees(45.0, 0.0, 0.0);
		const FEcef Geodetic   = GeodeticSurfaceNormal(G);
		const FEcef Geocentric = GeodeticToEcef(G).Normalized();
		const double AngleDeg  = std::acos(Geodetic.Dot(Geocentric)) * RadToDeg;
		Check(AngleDeg > 0.18 && AngleDeg < 0.20,
			"normale geodetica != geocentrica a 45 deg (~11.5 primi)",
			Fmt("%.4f deg", AngleDeg));
	}
}

// ===========================================================================
//  TEST 4 -- Frame ENU: verifica FISICA, non tautologica.
//  Un punto a 1000 m a Nord sul piano tangente deve trovarsi 78.5 mm SOTTO
//  l'orizzonte locale: e' la curvatura terrestre, d^2/(2R).
// ===========================================================================
static void TestEnuFrame()
{
	Section("4. Frame ENU locale e curvatura terrestre");

	const FGeodetic Origin = FGeodetic::FromDegrees(45.0, 9.0, 0.0);
	const FEnuFrame Frame(Origin);

	// L'origine del frame mappa nell'origine.
	{
		const FEnu Z = Frame.GeodeticToEnu(Origin);
		Check(std::abs(Z.E) < 1e-9 && std::abs(Z.N) < 1e-9 && std::abs(Z.U) < 1e-9,
			"l'origine del frame mappa in (0,0,0)",
			Fmt("|ENU| = %.3e m", std::sqrt(Z.E * Z.E + Z.N * Z.N + Z.U * Z.U)));
	}

	// Un punto 1000 m a Nord lungo la superficie.
	{
		const FEnu P = Frame.EcefToEnu(Frame.EnuToEcef(FEnu{ 0.0, 1000.0, 0.0 }));
		Check(std::abs(P.N - 1000.0) < 1e-6 && std::abs(P.E) < 1e-6 && std::abs(P.U) < 1e-6,
			"round-trip ENU -> ECEF -> ENU", Fmt("N = %.9f m", P.N));
	}

	// LA verifica fisica: prendo il punto geodetico che sta 1000 m piu' a Nord
	// SULLA SUPERFICIE dell'ellissoide e guardo quanto e' sceso sotto il piano
	// tangente. Deve valere d^2/(2R) ~ 0.0785 m.
	{
		// 1000 m a Nord in latitudine: uso il raggio di curvatura meridiano M.
		const double SinLat = std::sin(Origin.LatRad);
		const double M = WGS84.A * (1.0 - WGS84.E2)
		               / std::pow(1.0 - WGS84.E2 * SinLat * SinLat, 1.5);
		const double DeltaLatRad = 1000.0 / M;

		const FGeodetic North1km = FGeodetic::FromRadians(
			Origin.LatRad + DeltaLatRad, Origin.LonRad, 0.0);

		const FEnu P = Frame.GeodeticToEnu(North1km);
		const double ExpectedDrop = 1000.0 * 1000.0 / (2.0 * M);

		Check(std::abs(P.N - 1000.0) < 0.01, "punto a 1000 m a Nord: N ~ 1000 m",
			Fmt("N = %.4f m", P.N));
		Check(std::abs(P.U + ExpectedDrop) < 0.002,
			"caduta per curvatura terrestre = d^2/(2R)",
			Fmt("U = %.4f m", P.U) + Fmt(" (atteso %.4f m)", -ExpectedDrop));
	}
}

// ===========================================================================
//  TEST 5 -- Ortonormalita' e handedness delle basi.
//  ENU deve avere det +1 (destrorsa), NEU det -1 (sinistrorsa, come Unreal).
// ===========================================================================
static void TestBasisProperties()
{
	Section("5. Basi locali: ortonormalita' e handedness");

	double MaxOrthoErr = 0.0;
	double MinEnuDet =  1e9, MaxEnuDet = -1e9;
	double MinNeuDet =  1e9, MaxNeuDet = -1e9;

	for (int LatIdx = -89; LatIdx <= 89; ++LatIdx)
	{
		for (int LonIdx = -180; LonIdx <= 180; LonIdx += 5)
		{
			const double Lat = LatIdx * DegToRad;
			const double Lon = LonIdx * DegToRad;

			for (const FMat3& B : { MakeEnuBasis(Lat, Lon), MakeNeuBasis(Lat, Lon) })
			{
				// B * B^T deve essere l'identita'.
				const FMat3 Prod = B * B.Transposed();
				for (int i = 0; i < 3; ++i)
				{
					for (int j = 0; j < 3; ++j)
					{
						const double Expected = (i == j) ? 1.0 : 0.0;
						MaxOrthoErr = std::max(MaxOrthoErr, std::abs(Prod.M[i][j] - Expected));
					}
				}
			}

			const double EnuDet = MakeEnuBasis(Lat, Lon).Determinant();
			const double NeuDet = MakeNeuBasis(Lat, Lon).Determinant();
			MinEnuDet = std::min(MinEnuDet, EnuDet); MaxEnuDet = std::max(MaxEnuDet, EnuDet);
			MinNeuDet = std::min(MinNeuDet, NeuDet); MaxNeuDet = std::max(MaxNeuDet, NeuDet);
		}
	}

	Check(MaxOrthoErr < 1e-15, "B * B^T = I (inversa = trasposta)", Fmt("max errore %.3e", MaxOrthoErr));
	Check(std::abs(MinEnuDet - 1.0) < 1e-14 && std::abs(MaxEnuDet - 1.0) < 1e-14,
		"base ENU destrorsa, det = +1", Fmt("det in [%.15f", MinEnuDet) + Fmt(", %.15f]", MaxEnuDet));
	Check(std::abs(MinNeuDet + 1.0) < 1e-14 && std::abs(MaxNeuDet + 1.0) < 1e-14,
		"base NEU sinistrorsa, det = -1 (riflessione per Unreal)",
		Fmt("det in [%.15f", MinNeuDet) + Fmt(", %.15f]", MaxNeuDet));
}

// ===========================================================================
//  TEST 6 -- Georeferenziazione: unita', assi, round-trip a 1000 km.
// ===========================================================================
static void TestGeoreference()
{
	Section("6. Georeferenziazione: unita' e orientamento assi");

	const FGeodetic Origin = FGeodetic::FromDegrees(41.8902, 12.4922, 40.0);   // Colosseo
	const FGeoreference Geo(Origin);

	// L'origine e' l'origine.
	{
		const FUnrealPos P = Geo.GeodeticToUnreal(Origin);
		Check(P.Length() < 1e-6, "l'origine mappa in (0,0,0) unita'", Fmt("|P| = %.3e uu", P.Length()));
	}

	// Unita': 1 metro = 100 unita', esattamente.
	{
		const FEcef OneMeterUp = Geo.GetOriginEcef() + GeodeticSurfaceNormal(Origin) * 1.0;
		const FUnrealPos P = Geo.EcefToUnreal(OneMeterUp);
		Check(std::abs(P.Z - 100.0) < 1e-6 && std::hypot(P.X, P.Y) < 1e-6,
			"1 metro verso l'alto locale = +100 unita' su Z", Fmt("Z = %.9f uu", P.Z));
	}

	// ASSI: Nord -> +X, Est -> +Y, Alto -> +Z.
	{
		const FMat3 Enu = MakeEnuBasis(Origin.LatRad, Origin.LonRad);
		const FEcef EastDir  = Enu.GetRow(0);
		const FEcef NorthDir = Enu.GetRow(1);

		const FUnrealPos N = Geo.EcefToUnreal(Geo.GetOriginEcef() + NorthDir * 1000.0);
		const FUnrealPos E = Geo.EcefToUnreal(Geo.GetOriginEcef() + EastDir  * 1000.0);

		Check(std::abs(N.X - 100000.0) < 1e-6 && std::abs(N.Y) < 1e-6,
			"1 km a NORD  -> +X (yaw 0 = Nord)", Fmt("X = %.3f uu", N.X));
		Check(std::abs(E.Y - 100000.0) < 1e-6 && std::abs(E.X) < 1e-6,
			"1 km a EST   -> +Y (yaw 90 = Est, orario)", Fmt("Y = %.3f uu", E.Y));
	}

	// Round-trip a 1000 km dall'origine.
	{
		// Milano dista ~480 km da Roma; per arrivare a 1000 km vado piu' a nord.
		const FGeodetic Far = FGeodetic::FromDegrees(50.8503, 4.3517, 100.0);   // Bruxelles
		const FUnrealPos P  = Geo.GeodeticToUnreal(Far);
		const FGeodetic Back = Geo.UnrealToGeodetic(P);

		const double DistKm = P.Length() * Units::UuToKilometers;
		const double ErrM   = EcefDistance(GeodeticToEcef(Far), GeodeticToEcef(Back));

		std::printf("  distanza dall'origine: %.1f km\n", DistKm);
		Check(DistKm > 1000.0, "punto di prova oltre 1000 km", Fmt("%.1f km", DistKm));
		Check(ErrM < 1e-6, "round-trip geodetic -> unreal -> geodetic < 1e-6 m",
			Fmt("errore %.3e m", ErrM));
	}

	// Verticale locale a distanza: due punti lontani NON hanno la stessa verticale.
	{
		const FGeodetic Milano = FGeodetic::FromDegrees(45.4641, 9.1919, 120.0);
		const FMat3 HereBasis = Geo.GetLocalNeuBasisInUnreal(Origin);
		const FMat3 ThereBasis = Geo.GetLocalNeuBasisInUnreal(Milano);

		const FEcef UpHere  = HereBasis.GetRow(2);
		const FEcef UpThere = ThereBasis.GetRow(2);
		const double AngleDeg = std::acos(UpHere.Dot(UpThere)) * RadToDeg;

		Check(std::abs(UpHere.X) < 1e-12 && std::abs(UpHere.Y) < 1e-12 && std::abs(UpHere.Z - 1.0) < 1e-12,
			"l'alto locale NELL'ORIGINE e' esattamente +Z");
		Check(AngleDeg > 3.0 && AngleDeg < 5.0,
			"l'alto locale a Milano e' inclinato rispetto a Roma", Fmt("%.3f deg", AngleDeg));
	}
}

// ===========================================================================
//  TEST 7 -- Invarianza al rebasing. E' IL test della Fase 1.
//  Mille cambi d'origine consecutivi non devono spostare di un nanometro un
//  punto geografico fisso. Se la posizione Unreal fosse lo stato primario e si
//  componessero le trasformazioni, qui si vedrebbe la deriva accumularsi.
// ===========================================================================
static void TestRebasingInvariance()
{
	Section("7. Invarianza al rebasing (1000 cambi d'origine)");

	// Il punto fisso: un angolo del Colosseo. La sua posizione ECEF e' l'unica
	// autorita' e non cambia mai.
	const FGeodetic Target    = FGeodetic::FromDegrees(41.8902, 12.4922, 40.0);
	const FEcef     TargetEcef = GeodeticToEcef(Target);

	FGeoreference Geo(FGeodetic::FromDegrees(41.8902, 12.4922, 40.0));

	std::mt19937_64 Rng(1234567);
	std::uniform_real_distribution<double> LatDist(35.0, 48.0);      // Italia + margine
	std::uniform_real_distribution<double> LonDist(6.0, 19.0);
	std::uniform_real_distribution<double> HDist(0.0, 12000.0);

	double MaxErrM = 0.0;
	double MaxErrUu = 0.0;

	for (int i = 0; i < 1000; ++i)
	{
		Geo.SetOrigin(FGeodetic::FromDegrees(LatDist(Rng), LonDist(Rng), HDist(Rng)));

		// Ricalcolo la posizione Unreal DALL'ECEF (mai dalla posizione precedente).
		const FUnrealPos P = Geo.EcefToUnreal(TargetEcef);

		// E torno indietro: devo ritrovare esattamente lo stesso punto del pianeta.
		const FEcef Back = Geo.UnrealToEcef(P);

		MaxErrM  = std::max(MaxErrM, EcefDistance(TargetEcef, Back));
		MaxErrUu = std::max(MaxErrUu, EcefDistance(TargetEcef, Back) * Units::MetersToUu);
	}

	Check(MaxErrM < 1e-7, "nessuna deriva dopo 1000 rebase", Fmt("max errore %.3e m", MaxErrM));
	Check(MaxErrUu < 1e-5, "errore in unita' Unreal trascurabile", Fmt("max %.3e uu", MaxErrUu));
}

// ===========================================================================
//  TEST 8 -- Perche' il rebasing serve ancora, in numeri.
//  Non e' un'asserzione sul nostro codice: e' la dimostrazione quantitativa
//  che i float non bastano a distanza di raggio terrestre, cioe' la ragione
//  per cui esiste tutta questa architettura.
// ===========================================================================
static void TestFloatPrecisionRationale()
{
	Section("8. Perche' serve il rebasing: precisione float vs double");

	const FGeodetic Rome = FGeodetic::FromDegrees(41.8902, 12.4922, 40.0);
	const FEcef P = GeodeticToEcef(Rome);

	// Coordinata Unreal ASSOLUTA (cioe' se l'origine fosse il centro della Terra).
	const double AbsoluteUu = P.Length() * Units::MetersToUu;

	// ULP di un float a quella magnitudine.
	const float  AsFloat = static_cast<float>(AbsoluteUu);
	const float  NextFloat = std::nextafter(AsFloat, AsFloat * 2.0f);
	const double FloatUlpUu = static_cast<double>(NextFloat) - static_cast<double>(AsFloat);

	// ULP di un double alla stessa magnitudine.
	const double NextDouble = std::nextafter(AbsoluteUu, AbsoluteUu * 2.0);
	const double DoubleUlpUu = NextDouble - AbsoluteUu;

	std::printf("  posizione assoluta di Roma: %.1f unita' (%.0f km dal geocentro)\n",
		AbsoluteUu, P.Length() / 1000.0);
	std::printf("  ULP float  a quella distanza: %.4f unita' (%.4f m)\n",
		FloatUlpUu, FloatUlpUu * Units::UuToMeters);
	std::printf("  ULP double a quella distanza: %.3e unita' (%.3e m)\n",
		DoubleUlpUu, DoubleUlpUu * Units::UuToMeters);

	Check(FloatUlpUu > 10.0,
		"un float a distanza terrestre ha ULP > 10 cm: inutilizzabile per i vertici",
		Fmt("%.2f cm", FloatUlpUu));
	Check(DoubleUlpUu * Units::UuToMeters < 1e-8,
		"un double ha ULP sotto il nanometro: adatto allo spazio mondo",
		Fmt("%.3e m", DoubleUlpUu * Units::UuToMeters));

	// E con il rebasing attivo: entro 10 km dall'origine, quanto vale un float?
	{
		const float NearFloat = 1000000.0f;   // 10 km in unita' Unreal
		const float NextNear = std::nextafter(NearFloat, NearFloat * 2.0f);
		const double NearUlpMm = (static_cast<double>(NextNear) - static_cast<double>(NearFloat))
		                       * Units::UuToMeters * 1000.0;
		std::printf("  con rebasing (soglia 10 km) l'ULP float scende a %.4f mm\n", NearUlpMm);
		Check(NearUlpMm < 2.0, "entro la soglia di rebasing il float e' millimetrico",
			Fmt("%.4f mm", NearUlpMm));
	}
}


// ===========================================================================
//  TEST 9 -- La base di orientamento locale deve essere una ROTAZIONE PROPRIA.
//
//  Perche' e' cruciale e non ovvio: la matrice ECEF->Unreal ha det -1 (e' la
//  riflessione destrorso->sinistrorso) e anche la base NEU locale in ECEF ha
//  det -1. Il loro prodotto ha quindi det +1. Se cosi' non fosse, passare quella
//  matrice a FMatrix::ToQuat() di Unreal produrrebbe un quaternione senza senso:
//  i quaternioni rappresentano solo rotazioni proprie, e ToQuat() su una matrice
//  con det -1 non fallisce, restituisce silenziosamente spazzatura.
//  In altre parole: la riflessione avviene UNA VOLTA SOLA, al confine della
//  georeferenziazione, e dentro lo spazio di Unreal tutto e' rotazione pura.
// ===========================================================================
static void TestLocalBasisIsProperRotation()
{
	Section("9. La base di orientamento locale e' una rotazione propria (det +1)");

	double MinDet = 1e9, MaxDet = -1e9, MaxOrthoErr = 0.0;

	const FGeoreference Geo(FGeodetic::FromDegrees(41.8902, 12.4922, 40.0));

	for (int LatIdx = -85; LatIdx <= 85; LatIdx += 5)
	{
		for (int LonIdx = -180; LonIdx <= 180; LonIdx += 15)
		{
			const FMat3 B = Geo.GetLocalNeuBasisInUnreal(
				FGeodetic::FromDegrees(LatIdx, LonIdx, 0.0));

			const double Det = B.Determinant();
			MinDet = std::min(MinDet, Det);
			MaxDet = std::max(MaxDet, Det);

			const FMat3 Prod = B * B.Transposed();
			for (int i = 0; i < 3; ++i)
				for (int j = 0; j < 3; ++j)
					MaxOrthoErr = std::max(MaxOrthoErr, std::abs(Prod.M[i][j] - ((i == j) ? 1.0 : 0.0)));
		}
	}

	Check(std::abs(MinDet - 1.0) < 1e-14 && std::abs(MaxDet - 1.0) < 1e-14,
		"det = +1 ovunque (rotazione propria, valida per FMatrix::ToQuat)",
		Fmt("det in [%.15f", MinDet) + Fmt(", %.15f]", MaxDet));
	Check(MaxOrthoErr < 1e-14, "base ortonormale", Fmt("max errore %.3e", MaxOrthoErr));
}

// ===========================================================================
int main()
{
	std::printf("=====================================================\n");
	std::printf(" GeoCore -- test dello strato puro (senza Unreal)\n");
	std::printf("=====================================================\n");

	TestRoundTripGrid();
	TestBowringVsIterative();
	TestKnownValues();
	TestEnuFrame();
	TestBasisProperties();
	TestGeoreference();
	TestRebasingInvariance();
	TestFloatPrecisionRationale();
	TestLocalBasisIsProperRotation();

	std::printf("\n=====================================================\n");
	std::printf(" RISULTATO: %d passati, %d falliti\n", GNumPassed, GNumFailed);
	std::printf("=====================================================\n");

	return (GNumFailed == 0) ? 0 : 1;
}

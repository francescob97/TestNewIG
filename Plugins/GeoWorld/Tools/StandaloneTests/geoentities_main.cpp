// =============================================================================
//  Test delle entita' (Fase 7), senza Unreal: il quaternione, le tre
//  trappole delle convenzioni (NED/NEU, gli angoli di CIGI e di DIS, gli assi
//  del corpo e quelli dell'attore), l'estrapolazione, il rientro, il tempo
//  (l'ora di DIS, l'orologio della simulazione), il registro, la tabella dei
//  modelli, la memoria condivisa (anche con due thread veri), e la fixture
//  scritta dal generatore Python: se Python e C++ non sono d'accordo su una
//  convenzione, l'estrapolazione di qui non ritrova le posizioni di la'.
// =============================================================================
#include "Entities/DeadReckoning.h"
#include "Entities/EntityFrames.h"
#include "Entities/EntityModels.h"
#include "Entities/EntityRegistry.h"
#include "Entities/EntityState.h"
#include "Entities/SharedMemoryFormat.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace GeoWorld::Entities;
using GeoWorld::Core::FGeodetic;
using GeoWorld::Core::DegToRad;
using GeoWorld::Core::RadToDeg;

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

static double Distance(const FEcef& A, const FEcef& B) { return (A - B).Length(); }

static double MatrixError(const FMat3& A, const FMat3& B)
{
	double Out = 0.0;
	for (int I = 0; I < 3; ++I)
		for (int J = 0; J < 3; ++J)
			Out = std::max(Out, std::fabs(A.M[I][J] - B.M[I][J]));
	return Out;
}

/** L'angolo fra due orientamenti, gradi. */
static double AngleBetween(const FQuatD& A, const FQuatD& B) { return (A * B.Conjugate()).AngleRad() * RadToDeg; }

static double WrapDeg(double Degrees)
{
	while (Degrees > 180.0) { Degrees -= 360.0; }
	while (Degrees <= -180.0) { Degrees += 360.0; }
	return Degrees;
}

static FEcef At(double LatDeg, double LonDeg, double Height)
{
	return GeoWorld::Core::GeodeticToEcef(FGeodetic::FromDegrees(LatDeg, LonDeg, Height));
}

static constexpr double TurinLat = 45.07, TurinLon = 7.68;

// ===========================================================================

static void TestQuaternion()
{
	Section("1. Il quaternione");
	const FQuatD Q = FQuatD::FromAxisAngle(FEcef{ 0.0, 0.0, 1.0 }, 90.0 * DegToRad);
	const FEcef X = Q.Rotate(FEcef{ 1.0, 0.0, 0.0 });
	Check(Distance(X, FEcef{ 0.0, 1.0, 0.0 }) < 1e-12, "90 gradi attorno a Z portano X su Y (rotazione attiva, mano destra)");

	std::mt19937 Random(7);
	std::uniform_real_distribution<double> Uniform(-1.0, 1.0);
	double WorstRotate = 0.0, WorstRound = 0.0, WorstCompose = 0.0;
	for (int Trial = 0; Trial < 2000; ++Trial)
	{
		const FEcef Axis{ Uniform(Random), Uniform(Random), Uniform(Random) };
		// Anche vicino a 180 gradi, dove Shepperd cambia ramo.
		const double Angle = Trial < 200 ? 3.14159 - 1e-4 * Trial : 3.2 * Uniform(Random);
		const FQuatD A = FQuatD::FromAxisAngle(Axis, Angle);
		const FQuatD B = FQuatD::FromAxisAngle(FEcef{ Uniform(Random), Uniform(Random), Uniform(Random) }, 3.0 * Uniform(Random));
		const FEcef V{ Uniform(Random), Uniform(Random), Uniform(Random) };
		WorstRotate = std::max(WorstRotate, Distance(A.Rotate(V), A.ToMatrix().Transform(V)));
		WorstRound = std::max(WorstRound, AngleBetween(FQuatD::FromMatrix(A.ToMatrix()), A));
		WorstCompose = std::max(WorstCompose, Distance((A * B).Rotate(V), A.Rotate(B.Rotate(V))));
	}
	Check(WorstRotate < 1e-12, "Rotate e la matrice fanno la stessa cosa (2000 rotazioni a caso)", Fmt("%.1e", WorstRotate));
	Check(WorstRound < 1e-6, "dalla matrice al quaternione e ritorno, anche a 180 gradi", Fmt("%.1e gradi", WorstRound));
	Check(WorstCompose < 1e-12, "A * B applica prima B, poi A", Fmt("%.1e", WorstCompose));
	Check(std::fabs(Q.ToMatrix().Determinant() - 1.0) < 1e-12, "la matrice di un quaternione ha determinante +1");

	const FQuatD Half = FQuatD::Slerp(FQuatD{}, Q, 0.5);
	Check(std::fabs(Half.AngleRad() * RadToDeg - 45.0) < 1e-9 && AngleBetween(FQuatD::Slerp(FQuatD{}, Q, 1.0), Q) < 1e-6
		&& AngleBetween(FQuatD::Slerp(FQuatD{}, Q, 0.0), FQuatD{}) < 1e-6,
		"slerp: a meta' strada 45 gradi, agli estremi i due capi");
	const FQuatD Minus{ -Q.W, -Q.X, -Q.Y, -Q.Z };
	Check(std::fabs(FQuatD::Slerp(FQuatD{}, Minus, 0.5).AngleRad() * RadToDeg - 45.0) < 1e-9,
		"slerp prende la via corta anche con il quaternione di segno opposto (la stessa rotazione)");
}

static void TestTrapNed()
{
	Section("2. Trappola 1: NED non e' NEU");
	const double Lat = TurinLat * DegToRad, Lon = TurinLon * DegToRad;
	const FMat3 Ned = MakeNedBasis(Lat, Lon);
	const FMat3 Neu = GeoWorld::Core::MakeNeuBasis(Lat, Lon);
	Check(std::fabs(Ned.Determinant() - 1.0) < 1e-12 && std::fabs(Neu.Determinant() + 1.0) < 1e-12,
		"NED e' destrorso (det +1), NEU sinistrorso (det -1)");
	const FEcef Up = GeoWorld::Core::GeodeticSurfaceNormal(FGeodetic::FromDegrees(TurinLat, TurinLon, 0.0));
	const FEcef Sinking = NedToEcef(Lat, Lon, FEcef{ 0.0, 0.0, 5.0 });
	Check(std::fabs(Sinking.Dot(Up) + 5.0) < 1e-12, "una velocita' NED di +5 sulla terza componente SCENDE di 5 m/s",
		Fmt("verso l'alto: %.3f m/s", Sinking.Dot(Up)));
	const FEcef Ecef{ 3.0, -7.0, 2.0 };
	Check(Distance(NedToEcef(Lat, Lon, EcefToNed(Lat, Lon, Ecef)), Ecef) < 1e-12, "da ECEF a NED e ritorno");
	const FEcef North = NedToEcef(Lat, Lon, FEcef{ 1.0, 0.0, 0.0 });
	Check(Distance(North, Neu.GetRow(0)) < 1e-12 && Distance(NedToEcef(Lat, Lon, FEcef{ 0.0, 1.0, 0.0 }), Neu.GetRow(1)) < 1e-12,
		"Nord ed Est sono gli stessi di MakeNeuBasis: cambia solo la terza");
}

static void TestEuler()
{
	Section("3. Eulero 3-2-1: imbardata, beccheggio, rollio");
	std::mt19937 Random(11);
	std::uniform_real_distribution<double> Uniform(-1.0, 1.0);
	double Worst = 0.0;
	for (int Trial = 0; Trial < 2000; ++Trial)
	{
		const FEuler In{ 3.1 * Uniform(Random), 1.55 * Uniform(Random), 3.1 * Uniform(Random) };
		const FEuler Out = EulerFromMatrix(MatrixFromEuler(In));
		Worst = std::max({ Worst, std::fabs(Out.Yaw - In.Yaw), std::fabs(Out.Pitch - In.Pitch), std::fabs(Out.Roll - In.Roll) });
	}
	Check(Worst < 1e-9, "dagli angoli alla matrice e ritorno (2000 assetti a caso, beccheggio sotto 89 gradi)", Fmt("%.1e rad", Worst));

	const FEuler Vertical{ 30.0 * DegToRad, 90.0 * DegToRad, 20.0 * DegToRad };
	const FEuler Back = EulerFromMatrix(MatrixFromEuler(Vertical));
	Check(MatrixError(MatrixFromEuler(Back), MatrixFromEuler(Vertical)) < 1e-6 && Back.Roll == 0.0,
		"a beccheggio 90 (gimbal lock) gli angoli cambiano ma l'assetto e' lo stesso; il rollio va a zero",
		Fmt("imbardata %.1f, rollio %.1f", Back.Yaw * RadToDeg, Back.Roll * RadToDeg));

	// I versi, nel NED locale: imbardata a destra (est), muso in su, ala destra in giu'.
	const FMat3 East = MatrixFromEuler(FEuler{ 90.0 * DegToRad, 0.0, 0.0 });
	const FMat3 Up = MatrixFromEuler(FEuler{ 0.0, 10.0 * DegToRad, 0.0 });
	const FMat3 Rolled = MatrixFromEuler(FEuler{ 0.0, 0.0, 30.0 * DegToRad });
	Check(std::fabs(East.M[1][0] - 1.0) < 1e-12, "imbardata +90: il muso (X del corpo) punta a est");
	Check(Up.M[2][0] < -0.17, "beccheggio +10: il muso sale (componente Giu' negativa)", Fmt("giu' %.3f", Up.M[2][0]));
	Check(Rolled.M[2][1] > 0.49, "rollio +30: l'ala destra (Y del corpo) scende", Fmt("giu' %.3f", Rolled.M[2][1]));
}

static void TestTrapDisCigi()
{
	Section("4. Trappola 2: gli angoli di CIGI (locali) e quelli di DIS (ECEF)");
	// All'equatore, a Greenwich: Nord = Z dell'ECEF, Est = Y, Giu' = -X.
	const FEuler Dis00 = DisFromLocalEuler(0.0, 0.0, FEuler{});
	Check(std::fabs(Dis00.Pitch * RadToDeg + 90.0) < 1e-6,
		"un aereo livellato verso nord a lat 0, lon 0 ha theta DIS = -90 (il caso dei libri)",
		Fmt("psi %.2f theta %.2f phi %.2f", Dis00.Yaw * RadToDeg, Dis00.Pitch * RadToDeg, Dis00.Roll * RadToDeg));

	const double RomeLat = 41.9 * DegToRad, RomeLon = 12.5 * DegToRad;
	const double TokyoLat = 35.68 * DegToRad, TokyoLon = 139.69 * DegToRad;
	const FEuler North{};
	const FEuler DisRome = DisFromLocalEuler(RomeLat, RomeLon, North);
	const FEuler DisTokyo = DisFromLocalEuler(TokyoLat, TokyoLon, North);
	const double Gap = std::fabs(WrapDeg((DisRome.Yaw - DisTokyo.Yaw) * RadToDeg)) + std::fabs((DisRome.Pitch - DisTokyo.Pitch) * RadToDeg);
	Check(Gap > 30.0, "verso nord a Roma e a Tokyo: imbardata CIGI 0 per tutti e due, psi/theta DIS diversissimi",
		Fmt("Roma psi %.1f theta %.1f, Tokyo psi %.1f theta %.1f", DisRome.Yaw * RadToDeg, DisRome.Pitch * RadToDeg,
			DisTokyo.Yaw * RadToDeg, DisTokyo.Pitch * RadToDeg));

	// Il muso punta davvero al nord locale in tutti e due i posti.
	const FEcef NoseRome = OrientationFromDisEuler(DisRome).Rotate(FEcef{ 1.0, 0.0, 0.0 });
	const FEcef NoseTokyo = OrientationFromDisEuler(DisTokyo).Rotate(FEcef{ 1.0, 0.0, 0.0 });
	Check(Distance(NoseRome, MakeNedBasis(RomeLat, RomeLon).GetRow(0)) < 1e-9
		&& Distance(NoseTokyo, MakeNedBasis(TokyoLat, TokyoLon).GetRow(0)) < 1e-9,
		"dagli angoli DIS il muso torna al nord locale, a Roma e a Tokyo");

	std::mt19937 Random(3);
	std::uniform_real_distribution<double> Uniform(-1.0, 1.0);
	double Worst = 0.0;
	for (int Trial = 0; Trial < 1000; ++Trial)
	{
		const double Lat = 1.5 * Uniform(Random), Lon = 3.1 * Uniform(Random);
		const FEuler Local{ 3.1 * Uniform(Random), 1.5 * Uniform(Random), 3.1 * Uniform(Random) };
		const FEuler Back = LocalFromDisEuler(Lat, Lon, DisFromLocalEuler(Lat, Lon, Local));
		Worst = std::max({ Worst, std::fabs(Back.Yaw - Local.Yaw), std::fabs(Back.Pitch - Local.Pitch), std::fabs(Back.Roll - Local.Roll) });
	}
	Check(Worst < 1e-8, "locale -> DIS -> locale, in 1000 punti e assetti a caso (il round-trip della sezione 4)", Fmt("%.1e rad", Worst));
}

static void TestTrapBodyAxes()
{
	Section("5. Trappola 3: dagli assi del corpo a quelli dell'attore");
	const double Lat = TurinLat * DegToRad, Lon = TurinLon * DegToRad;
	const auto Axes = [&](double Yaw, double Pitch, double Roll)
	{
		return ActorAxesNeu(Lat, Lon, OrientationFromLocalEuler(Lat, Lon, FEuler{ Yaw * DegToRad, Pitch * DegToRad, Roll * DegToRad }));
	};
	const FMat3 Level = Axes(0.0, 0.0, 0.0);
	Check(MatrixError(Level, FMat3{}) < 1e-12, "livellato verso nord: gli assi dell'attore sono quelli del NEU (avanti N, destra E, alto U)");
	Check(std::fabs(Axes(37.0, -12.0, 71.0).Determinant() - 1.0) < 1e-12,
		"la matrice dell'attore nel NEU e' una rotazione vera (det +1): diventa un FQuat senza riflessioni");
	const FMat3 East = Axes(90.0, 0.0, 0.0);
	Check(Distance(East.GetRow(0), FEcef{ 0.0, 1.0, 0.0 }) < 1e-12 && Distance(East.GetRow(1), FEcef{ -1.0, 0.0, 0.0 }) < 1e-12,
		"imbardata 90: avanti a est, la destra a sud");
	const FMat3 Climb = Axes(0.0, 10.0, 0.0);
	Check(std::fabs(Climb.M[0][2] - std::sin(10.0 * DegToRad)) < 1e-12, "beccheggio +10: il muso dell'attore sale (Z alto positivo)");
	const FMat3 Bank = Axes(0.0, 0.0, 30.0);
	Check(std::fabs(Bank.M[1][2] + 0.5) < 1e-12 && std::fabs(Bank.M[2][1] - 0.5) < 1e-12,
		"rollio +30: l'ala destra scende e il tetto si inclina a destra (verso est)");

	// Nello spazio di Unreal: l'origine a Roma, l'entita' a Torino (la verticale
	// di Torino e' inclinata di ~4 gradi su quella di Roma).
	const FMat3 RomeBasis = GeoWorld::Core::MakeNeuBasis(41.9 * DegToRad, 12.5 * DegToRad);
	const FQuatD Somewhere = OrientationFromLocalEuler(Lat, Lon, FEuler{ 37.0 * DegToRad, -12.0 * DegToRad, 71.0 * DegToRad });
	const FMat3 InUnreal = ActorAxesInUnreal(RomeBasis, Somewhere);
	const FMat3 TurinBasis = GeoWorld::Core::MakeNeuBasis(Lat, Lon);
	Check(std::fabs(InUnreal.Determinant() - 1.0) < 1e-12
		&& MatrixError(ActorAxesInUnreal(TurinBasis, Somewhere), ActorAxesNeu(Lat, Lon, Somewhere)) < 1e-12,
		"gli assi direttamente in Unreal: det +1 con l'origine lontana, e con l'origine sul posto sono quelli NEU");
	const FMat3 LevelHere = ActorAxesInUnreal(RomeBasis, OrientationFromLocalEuler(Lat, Lon, FEuler{}));
	Check(Distance(LevelHere.GetRow(2), RomeBasis.Transform(TurinBasis.GetRow(2))) < 1e-12 && LevelHere.M[2][2] < 0.9985,
		"un aereo livellato a Torino, visto da un'origine a Roma: il tetto e' la verticale di Torino, non quella di Roma",
		Fmt("inclinazione %.2f gradi", std::acos(LevelHere.M[2][2]) * RadToDeg));

	// Le velocita' angolari: con le componenti dell'attore, v' = w x v deve tornare.
	const FQuatD Q = OrientationFromLocalEuler(Lat, Lon, FEuler{ 20.0 * DegToRad, 5.0 * DegToRad, 25.0 * DegToRad });
	const FEcef Rates{ 0.3, -0.2, 0.25 };
	const double Dt = 1e-5;
	const FMat3 Before = ActorAxesNeu(Lat, Lon, Q);
	const FMat3 After = ActorAxesNeu(Lat, Lon, RotateByBodyRates(Q, Rates, Dt));
	const FEcef Actor = BodyRatesToActor(Rates);
	double Worst = 0.0;
	for (int Axis = 0; Axis < 3; ++Axis)
	{
		// L'asse dopo, scritto negli assi dell'attore di prima.
		const FEcef Moved = Before.Transform(After.GetRow(Axis));
		const FEcef Unit{ Axis == 0 ? 1.0 : 0.0, Axis == 1 ? 1.0 : 0.0, Axis == 2 ? 1.0 : 0.0 };
		const FEcef Expected = Unit + Cross(Actor, Unit) * Dt;
		Worst = std::max(Worst, Distance(Moved, Expected) / Dt);
	}
	Check(Worst < 1e-4, "le velocita' angolari dell'attore sono (-p, -q, +r): v' = w x v torna sui tre assi", Fmt("%.1e", Worst));
	Check(BodyRatesToActor(FEcef{ 0.1, 0.0, 0.0 }).X < 0.0, "un rollio a destra di DIS e' negativo per l'attore (la frase della sezione 4)");
}

static FEntityState Flying(double SpeedNorth, double YawRateDeg)
{
	const double Lat = TurinLat * DegToRad, Lon = TurinLon * DegToRad;
	FEntityState S;
	S.Position = At(TurinLat, TurinLon, 1000.0);
	S.Orientation = OrientationFromLocalEuler(Lat, Lon, FEuler{});
	S.Velocity = NedToEcef(Lat, Lon, FEcef{ SpeedNorth, 0.0, 0.0 });
	// Una virata livellata attorno al Giu' del corpo (ali dritte: per il test basta).
	S.AngularVelocity = FEcef{ 0.0, 0.0, YawRateDeg * DegToRad };
	S.Time = 100.0;
	return S;
}

static void TestDeadReckoning()
{
	Section("6. L'estrapolazione (dead reckoning)");
	const double Lat = TurinLat * DegToRad, Lon = TurinLon * DegToRad;
	FEntityState S = Flying(100.0, 0.0);
	S.Acceleration = NedToEcef(Lat, Lon, FEcef{ 2.0, 0.0, 0.0 });
	const FEntityState Still = Extrapolate(S, EDeadReckoning::Static, 10.0);
	Check(Distance(Still.Position, S.Position) == 0.0 && Still.Time == 110.0, "1 fermo: non si muove, ma il tempo avanza");
	const FEntityState Fpw = Extrapolate(S, EDeadReckoning::FPW, 10.0);
	const FEntityState Rvw = Extrapolate(S, EDeadReckoning::RVW, 10.0);
	Check(std::fabs(Distance(Fpw.Position, S.Position) - 1000.0) < 1e-6, "2 FPW: solo la velocita' (100 m/s per 10 s = 1000 m)");
	Check(std::fabs(Distance(Rvw.Position, S.Position) - 1100.0) < 1e-6 && std::fabs(Rvw.Velocity.Length() - 120.0) < 1e-9,
		"4 RVW: piu' l'accelerazione (+ 2*10²/2 = 100 m), la velocita' cresce a 120");
	const FEntityState Back = Extrapolate(S, EDeadReckoning::FPW, -0.5);
	Check(std::fabs(Distance(Back.Position, S.Position) - 50.0) < 1e-9, "anche all'indietro (mezzo secondo prima: 50 m)");

	// Una virata a destra di 6 gradi al secondo a 100 m/s: un cerchio di raggio v/w.
	const FEntityState Turn = Flying(100.0, 6.0);
	const double Radius = 100.0 / (6.0 * DegToRad);
	const FEntityState Half = Extrapolate(Turn, EDeadReckoning::RPB, 30.0);
	const FEntityState Full = Extrapolate(Turn, EDeadReckoning::RPB, 60.0);
	const FEcef HalfNed = EcefToNed(Lat, Lon, Half.Position - Turn.Position);
	Check(std::fabs(HalfNed.Y - 2.0 * Radius) < 1e-3 && std::fabs(HalfNed.X) < 1e-3,
		"7 RPB: dopo mezzo giro e' a 2r verso destra (est), la velocita' gira con il corpo",
		Fmt("est %.4f m (2r = %.4f), nord %.4f", HalfNed.Y, 2.0 * Radius, HalfNed.X));
	Check(Distance(Full.Position, Turn.Position) < 1e-3, "dopo un giro intero torna al punto di partenza",
		Fmt("%.2e m", Distance(Full.Position, Turn.Position)));
	const FEcef NoseHalf = EcefToNed(Lat, Lon, Half.Orientation.Rotate(FEcef{ 1.0, 0.0, 0.0 }));
	Check(NoseHalf.X < -0.999, "e dopo mezzo giro il muso guarda a sud");
	const FEntityState Straight = Extrapolate(Turn, EDeadReckoning::RPW, 30.0);
	Check(std::fabs(Distance(Straight.Position, Turn.Position) - 3000.0) < 1e-6,
		"3 RPW con la stessa virata: l'assetto gira ma la velocita' del MONDO va dritta (3000 m)");
	const FEntityState Fixed = Extrapolate(Turn, EDeadReckoning::FPB, 30.0);
	Check(AngleBetween(Fixed.Orientation, Turn.Orientation) < 1e-12 && std::fabs(Distance(Fixed.Position, Turn.Position) - 3000.0) < 1e-6,
		"6 FPB: orientamento fermo, e senza rotazione il corpo e' il mondo");

	// La stessa virata nel mondo (RVW) con l'accelerazione centripeta: su un
	// secondo l'errore e' del terzo ordine.
	FEntityState Centripetal = Turn;
	const double Omega = 6.0 * DegToRad;
	Centripetal.Acceleration = NedToEcef(Lat, Lon, FEcef{ 0.0, 100.0 * Omega, 0.0 });
	const FEntityState World = Extrapolate(Centripetal, EDeadReckoning::RVW, 1.0);
	const FEntityState Body = Extrapolate(Centripetal, EDeadReckoning::RVB, 1.0);
	const FEntityState Exact = Extrapolate(Turn, EDeadReckoning::RPB, 1.0);
	Check(Distance(World.Position, Exact.Position) < 0.25 && Distance(Body.Position, Exact.Position) < 1e-6,
		"virata con la centripeta: RVW sbaglia di v w² t³/6 (18 cm in un secondo), RVB non conta la centripeta due volte",
		Fmt("RVW %.4f m, RVB %.2e m", Distance(World.Position, Exact.Position), Distance(Body.Position, Exact.Position)));
	Check(Distance(Body.Acceleration, NedToEcef(Lat, Lon, FEcef{ -100.0 * Omega * std::sin(Omega), 100.0 * Omega * std::cos(Omega), 0.0 })) < 1e-6,
		"e l'accelerazione estrapolata e' ancora la centripeta, girata col corpo");
}

static void TestBlend()
{
	Section("7. Il rientro: niente scatti, niente deriva");
	const double Lat = TurinLat * DegToRad, Lon = TurinLon * DegToRad;
	FTrackSettings Settings;
	Settings.BlendSeconds = 0.5;
	FEntityTrack Track;
	FEntityState S = Flying(100.0, 0.0);
	Track.Update(S, EDeadReckoning::FPW, S.Time, Settings);
	const FDrawnPose Mid = Track.Evaluate(S.Time + 0.2, Settings);
	Check(std::fabs(Distance(Mid.Position, S.Position) - 20.0) < 1e-6 && Mid.BlendRemaining == 0.0, "il primo stato: nessun rientro, estrapola e basta");

	// Il nuovo stato a t+1: 10 m piu' a est di quello che pensavamo, e girato di 4 gradi.
	FEntityState Next = Extrapolate(S, EDeadReckoning::FPW, 1.0);
	Next.Position = Next.Position + NedToEcef(Lat, Lon, FEcef{ 0.0, 10.0, 0.0 });
	Next.Orientation = OrientationFromLocalEuler(Lat, Lon, FEuler{ 4.0 * DegToRad, 0.0, 0.0 });
	const double Arrive = S.Time + 1.05;
	const FDrawnPose Drawn = Track.Evaluate(Arrive, Settings);
	Track.Update(Next, EDeadReckoning::FPW, Arrive, Settings);
	const FDrawnPose JustAfter = Track.Evaluate(Arrive, Settings);
	Check(Distance(JustAfter.Position, Drawn.Position) < 1e-6 && AngleBetween(JustAfter.Orientation, Drawn.Orientation) < 1e-6,
		"all'arrivo dell'aggiornamento il disegno NON salta (posizione e assetto uguali a prima)",
		Fmt("%.2e m, %.2e gradi", Distance(JustAfter.Position, Drawn.Position), AngleBetween(JustAfter.Orientation, Drawn.Orientation)));
	Check(std::fabs(Track.LastCorrectionMetres() - 10.0) < 1e-6, "l'errore da smaltire e' quello vero (10 m)");
	const FEntityState TruthLater = Extrapolate(Next, EDeadReckoning::FPW, Arrive + 0.6 - Next.Time);
	const FDrawnPose Later = Track.Evaluate(Arrive + 0.6, Settings);
	Check(Distance(Later.Position, TruthLater.Position) < 1e-6 && AngleBetween(Later.Orientation, TruthLater.Orientation) < 1e-6,
		"finito il rientro (0,5 s) il disegno e' sulla verita'");
	const FEntityState TruthHalf = Extrapolate(Next, EDeadReckoning::FPW, Arrive + 0.25 - Next.Time);
	const FDrawnPose HalfWay = Track.Evaluate(Arrive + 0.25, Settings);
	Check(std::fabs(Distance(HalfWay.Position, TruthHalf.Position) - 5.0) < 1e-6
		&& std::fabs(AngleBetween(HalfWay.Orientation, TruthHalf.Orientation) - 2.0) < 1e-3,
		"a meta' rientro meta' errore (la curva e' simmetrica): 5 m e 2 gradi");

	// Oltre i 100 m si salta.
	FEntityState Far = Extrapolate(Next, EDeadReckoning::FPW, 1.0);
	Far.Position = Far.Position + NedToEcef(Lat, Lon, FEcef{ 500.0, 0.0, 0.0 });
	Track.Update(Far, EDeadReckoning::FPW, Far.Time, Settings);
	Check(Distance(Track.Evaluate(Far.Time, Settings).Position, Far.Position) < 1e-6 && Track.Snaps() == 1,
		"un errore di 500 m (un riposizionamento) si salta subito, e si conta");

	Settings.BlendSeconds = 0.0;
	FEntityState Again = Extrapolate(Far, EDeadReckoning::FPW, 1.0);
	Again.Position = Again.Position + NedToEcef(Lat, Lon, FEcef{ 0.0, 3.0, 0.0 });
	Track.Update(Again, EDeadReckoning::FPW, Again.Time, Settings);
	Check(Distance(Track.Evaluate(Again.Time, Settings).Position, Again.Position) < 1e-6, "con geo.Entities.Blend 0 si salta sempre");

	Settings.MaxExtrapolationSeconds = 5.0;
	const FDrawnPose Old = Track.Evaluate(Again.Time + 60.0, Settings);
	Check(Old.bStale && std::fabs(Distance(Old.Position, Again.Position) - 500.0) < 1e-6,
		"senza notizie per un minuto si smette di inventare dopo 5 s (e lo si segnala)");
}

static void TestTime()
{
	Section("8. Il tempo: l'ora di DIS, l'orologio della simulazione");
	Check(std::fabs(DisTimestampSeconds(0xFFFFFFFEu) - 3600.0) < 1e-5 && DisTimestampSeconds(2u) == DisTimeUnitSeconds,
		"il timestamp DIS: 2^31 unita' per un'ora (~1,68 microsecondi l'una)", Fmt("%.3f us", DisTimeUnitSeconds * 1e6));
	Check(DisTimestampIsAbsolute(0x00000001u) && !DisTimestampIsAbsolute(0x00000002u), "il bit basso dice assoluto o relativo");

	FHourUnwrapper Unwrap;
	const double A = Unwrap.Unwrap(3590.0), B = Unwrap.Unwrap(3599.0), C = Unwrap.Unwrap(2.0), D = Unwrap.Unwrap(10.0);
	const double Late = Unwrap.Unwrap(3599.5), E = Unwrap.Unwrap(11.0);
	Check(A == 3590.0 && B == 3599.0 && C == 3602.0 && D == 3610.0 && E == 3611.0, "allo scoccare dell'ora il tempo continua (3599 -> 3602)");
	Check(Late == 3599.5 && Unwrap.HoursCounted() == 1, "un pacchetto in ritardo dell'ora prima resta nell'ora prima, senza contare un'ora in piu'");
	double Previous = E;
	bool bMonotonic = true;
	for (int Step = 0; Step < 3 * 3600; Step += 7)
	{
		const double Now = Unwrap.Unwrap(std::fmod(11.0 + Step, 3600.0));
		bMonotonic = bMonotonic && Now >= Previous;
		Previous = Now;
	}
	Check(bMonotonic && Unwrap.HoursCounted() == 4, "tre ore di passi da 7 s: sempre avanti, quattro giri contati");

	FClockSync Clock;
	std::mt19937 Random(5);
	std::uniform_real_distribution<double> Latency(0.002, 0.040);
	for (int Step = 0; Step < 200; ++Step)
	{
		const double Sim = 50.0 + 0.1 * Step;
		Clock.Observe(Sim, Sim + 1000.0 + Latency(Random));
	}
	Check(std::fabs(Clock.OffsetSeconds() - 1000.0) < 0.01 && Clock.ResetCount() == 0,
		"lo scarto fra i due orologi: il minimo, cioe' il pacchetto piu' veloce (ritardi da 2 a 40 ms)",
		Fmt("%.4f s", Clock.OffsetSeconds() - 1000.0));
	Clock.Observe(0.0, 1030.0);
	Check(Clock.ResetCount() == 1 && std::fabs(Clock.SimNow(1031.0) - 1.0) < 1e-9,
		"la simulazione riavviata (il tempo torna a zero): si riparte da capo");
}

static void TestRegistry()
{
	Section("9. Il registro");
	FEntityRegistry Registry;
	Registry.Settings.BlendSeconds = 0.0;
	FEntityUpdate U;
	U.Key = FEntityKey{ 1, 42 };
	U.Label = "AEREO";
	U.State = Flying(100.0, 0.0);
	U.State.Time = 10.0;
	U.ReceivedSeconds = 500.0;
	U.Algorithm = EDeadReckoning::FPW;
	Check(Registry.Apply(U) == EApplyResult::Created && Registry.Num() == 1, "un'entita' nuova");
	FEntityUpdate Same = U;
	Same.Key.Source = 2;
	Check(Registry.Apply(Same) == EApplyResult::Created && Registry.Num() == 2,
		"lo stesso numero da un'altra sorgente e' un'altra entita' (la chiave e' sorgente + numero)");
	Check(GetTypeHash(U.Key) == GetTypeHash(FEntityKey{ 1, 42 }) && GetTypeHash(U.Key) != GetTypeHash(Same.Key),
		"GetTypeHash (per le TMap di Unreal): uguale per la stessa chiave, diverso fra le sorgenti");

	const FEntityRecord* Record = Registry.Find(U.Key);
	const FDrawnPose Pose = Registry.PoseAt(*Record, 500.5);
	Check(std::fabs(Distance(Pose.Position, U.State.Position) - 50.0) < 1e-6,
		"mezzo secondo nostro dopo l'arrivo e' mezzo secondo di simulazione: 50 m piu' avanti");

	FEntityUpdate Next = U;
	Next.State.Time = 11.0;
	Next.ReceivedSeconds = 501.0;
	Check(Registry.Apply(Next) == EApplyResult::Updated && Registry.Find(U.Key)->Updates == 2, "un aggiornamento");
	FEntityUpdate Old = U;
	Old.State.Time = 10.5;
	Old.ReceivedSeconds = 501.1;
	Check(Registry.Apply(Old) == EApplyResult::Ignored && Registry.Find(U.Key)->Late == 1, "un pacchetto in disordine (piu' vecchio) si scarta e si conta");

	FEntityUpdate Restart = U;
	Restart.State.Time = 0.5;
	Restart.ReceivedSeconds = 502.0;
	Check(Registry.Apply(Restart) == EApplyResult::Updated && Registry.Find(U.Key)->Track.Last().Time == 0.5,
		"il tempo che torna indietro di molto e' una simulazione riavviata: si riparte");

	const std::vector<FEntityKey> Gone = Registry.Expire(503.5, 2.0);
	Check(Gone.size() == 1 && Gone[0] == Same.Key && Registry.Num() == 1, "chi tace da piu' del tempo massimo se ne va");
	FEntityUpdate Remove;
	Remove.Key = U.Key;
	Remove.bRemove = true;
	Check(Registry.Apply(Remove) == EApplyResult::Removed && Registry.Num() == 0, "e chi dice 'me ne vado' se ne va subito");

	// Le quote sul livello del mare: si alzano dell'ondulazione.
	Registry.GeoidUndulation = [](const FGeodetic&) { return 48.0; };
	FEntityUpdate Msl = U;
	Msl.Key = FEntityKey{ 3, 1 };
	Msl.bAboveSeaLevel = true;
	Msl.State.Position = At(TurinLat, TurinLon, 300.0);
	Registry.Apply(Msl);
	const FGeodetic Raised = GeoWorld::Core::EcefToGeodetic(Registry.Find(Msl.Key)->Track.Last().Position);
	Check(std::fabs(Raised.HeightM - 348.0) < 1e-6 && std::fabs(Raised.LatRad * RadToDeg - TurinLat) < 1e-9,
		"300 m sul mare a Torino (ondulazione 48) sono 348 sull'ellissoide, nello stesso punto");
	Check(Registry.RemoveSource(3).size() == 1 && Registry.Num() == 0, "una sorgente che si ferma porta via le sue entita'");
}

static void TestModels()
{
	Section("10. La tabella dei modelli");
	const std::string Text =
		"# i modelli di prova\n"
		"1.1.105.1          box 7 3.5 2.4     # i carri italiani\n"
		"1.1.*.1.*.*.*      box 6.5 3.4 2.3\n"
		"1.2.*.21           heli 12 11 3.5\n"
		"cigi 12            /Game/Veicoli/SM_Camion.SM_Camion\n"
		"1.2.*.1            plane 15 10 4.5\n"
		"1.2.*.1            plane 16 11 5     # la correzione in fondo vince\n"
		"1.x.3              box 1 1 1\n"
		"1.1.1.1.1.1.1.1    box 1 1 1\n"
		"1.3                sphere 3\n"
		"1.4                box 1 -2 1\n"
		"cigi\n";
	FModelTable Table;
	std::vector<FModelTableError> Errors;
	const bool bOk = Table.Parse(Text, Errors);
	Check(!bOk && Errors.size() == 5 && Errors[0].Line == 8 && Errors[1].Line == 9 && Errors[4].Line == 12,
		"le righe sbagliate si saltano e si elencano con il numero (8, 9, 10, 11, 12)",
		Errors.empty() ? std::string() : Errors[0].Message + " / " + Errors[2].Message);
	Check(Table.NumFromFile() == 6, "le sei buone restano");

	FEntityKind Ariete{ 1, 1, 105, 1, 2, 0, 0 };
	FEntityKind Abrams{ 1, 1, 225, 1, 1, 3, 0 };
	FEntityKind Truck{ 1, 1, 105, 7, 0, 0, 0 };
	FEntityKind Fighter{ 1, 2, 105, 1, 0, 0, 0 };
	FEntityKind Ship{ 1, 3, 105, 1, 0, 0, 0 };
	FEntityKind Strange{ 9, 9, 9, 9, 9, 9, 9 };
	const FModelMatch A = Table.Find(Ariete), B = Table.Find(Abrams), C = Table.Find(Truck), D = Table.Find(Fighter);
	Check(A.Rule && A.Rule->Line == 2 && !A.bFallback, "un carro italiano: vince la regola piu' specifica (riga 2)");
	Check(B.Rule && B.Rule->Line == 3, "un carro americano: la regola di tutti i carri (riga 3)");
	Check(C.Rule && C.bFallback && C.Rule->Model.Text == "box 4.5 1.8 1.5", "un camion senza regola: la forma di serie della terra, e si dice che e' un ripiego");
	Check(D.Rule && D.Rule->Line == 7 && D.Rule->Model.Parts.size() == 4, "due regole uguali: vince l'ultima; un aereo e' fatto di quattro scatole");
	Check(Table.Find(Ship).bFallback && Table.Find(Strange).Rule && Table.Find(Strange).Rule->Model.Text == "box 3 3 3",
		"una nave (la riga sbagliata non conta) e un tipo mai visto: il ripiego, mai niente");
	const FModelMatch Cigi = Table.Find(Ariete, 12);
	Check(Cigi.Rule && Cigi.Rule->Model.IsAsset() && Cigi.Rule->Model.Asset == "/Game/Veicoli/SM_Camion.SM_Camion",
		"l'indice di modello di CIGI viene prima del tipo; un asset di Unreal");
	Check(Table.Find(Ariete, 13).Rule->Line == 2, "un indice CIGI sconosciuto ricade sul tipo");

	FModelSpec Box;
	std::string Error;
	FModelTable::ParseModel({ "box", "4", "2", "1.5" }, Box, Error);
	Check(Box.Parts.size() == 1 && Box.Parts[0].CenterZ == 0.75 && std::fabs(Box.Extent() - 4.0) < 1e-12,
		"una scatola appoggiata sull'origine (il centro a meta' altezza)");
	FModelSpec Heli;
	FModelTable::ParseModel({ "heli", "12", "11", "3.5" }, Heli, Error);
	Check(Heli.Parts.size() == 3 && Heli.Extent() >= 11.0, "un elicottero: cabina, trave di coda, rotore");
}

static void TestSharedMemory()
{
	Section("11. La memoria condivisa");
	using namespace SharedMemory;
	Check(BytesFor(DefaultCapacity) == 64 + 128 * 1024, "64 byte d'intestazione + 128 per entita'");

	std::vector<uint8_t> Memory(BytesFor(8), 0);
	FHeader Header;
	Header.Capacity = 8;
	Header.Count = 2;
	Header.Flags = FlagAltitudeMsl;
	Header.Sequence = 6;
	Header.SimTime = 12.25;
	WriteHeader(Memory.data(), Header);
	FRecord R;
	R.Id = 0x1122334455667788ull;
	R.Kind = FEntityKind{ 1, 2, 105, 1, 2, 3, 4 };
	R.Flags = RecordFlagClamp;
	R.DeadReckoning = 4;
	R.Force = 2;
	R.ModelId = -1;
	R.LatDeg = TurinLat;
	R.LonDeg = TurinLon;
	R.AltM = 1234.5;
	R.YawDeg = 90.0f;
	R.VelEast = 50.0f;
	R.VelDown = -3.0f;
	R.RateR = 0.1f;
	R.Label = "UN NOME LUNGHISSIMO CHE NON CI STA DENTRO";
	WriteRecord(Memory.data() + HeaderBytes, R);
	R.Id = 2;
	R.Label = "B";
	WriteRecord(Memory.data() + HeaderBytes + RecordBytes, R);

	FHeader ReadBack;
	std::vector<FRecord> Records;
	std::string Error;
	const bool bRead = ReadSnapshot(Memory.data(), Memory.size(), ReadBack, Records, Error);
	Check(bRead && ReadBack.Count == 2 && ReadBack.SimTime == 12.25 && ReadBack.AltitudeAboveSeaLevel() && Records.size() == 2,
		"scritta e riletta: l'intestazione", Error);
	Check(bRead && Records[0].Id == 0x1122334455667788ull && Records[0].Kind == R.Kind && Records[0].ModelId == -1
		&& Records[0].LatDeg == TurinLat && Records[0].Label.size() == 31 && Records[1].Label == "B",
		"e le entita' (l'identificatore a 64 bit, il tipo, il -1, il nome tagliato a 31 byte)");

	std::vector<uint8_t> Copy;
	Check(CopyConsistent(Memory.data(), Memory.size(), Copy, Error) == ECopyResult::Ok && Copy.size() == HeaderBytes + 2 * RecordBytes,
		"una copia coerente prende solo le entita' che ci sono");
	Memory[SequenceOffset] = 7;
	Check(CopyConsistent(Memory.data(), Memory.size(), Copy, Error) == ECopyResult::Busy, "sequenza dispari: chi scrive e' a meta', si riprova");
	Memory[SequenceOffset] = 8;
	Memory[0] = 'X';
	Check(CopyConsistent(Memory.data(), Memory.size(), Copy, Error) == ECopyResult::Invalid, "un'altra memoria (manca 'GENT'): non e' la nostra");
	Memory[0] = 'G';

	// La traduzione: angoli locali come CIGI, velocita' NED.
	const FEntityUpdate U = ToUpdate(Records[0], ReadBack, 7, 99.0);
	const double Lat = TurinLat * DegToRad, Lon = TurinLon * DegToRad;
	const FEcef Up = GeoWorld::Core::GeodeticSurfaceNormal(FGeodetic::FromDegrees(TurinLat, TurinLon, 0.0));
	const FEcef Nose = EcefToNed(Lat, Lon, U.State.Orientation.Rotate(FEcef{ 1.0, 0.0, 0.0 }));
	Check(U.Key == FEntityKey{ 7, 0x1122334455667788ull } && U.bAboveSeaLevel && U.bClampToGround && U.Force == EForce::Opposing
		&& U.Algorithm == EDeadReckoning::RVW && U.State.Time == 12.25 && U.ReceivedSeconds == 99.0,
		"il record diventa un aggiornamento: chiave, bandiere, fazione, algoritmo, tempo");
	Check(std::fabs(U.State.Velocity.Dot(Up) - 3.0) < 1e-5 && std::fabs(Nose.Y - 1.0) < 1e-6,
		"velocita' Giu' -3 vuol dire che sale di 3 m/s; imbardata 90 punta a est (trappole 1 e 2)");

	// Due thread veri: uno scrive di continuo, l'altro legge. Ogni fotografia
	// che il lettore accetta deve essere intera: tutte le entita' con lo
	// stesso numero di giro.
	std::vector<uint8_t> Shared(BytesFor(64), 0);
	std::atomic<bool> bStop{ false };
	std::thread Writer([&]()
	{
		FHeader H;
		H.Capacity = 64;
		H.Count = 64;
		uint64_t Round = 0;
		uint64_t Sequence = 0;
		WriteHeader(Shared.data(), H);
		while (!bStop.load(std::memory_order_relaxed))
		{
			++Round;
			Detail::WriteLe(Shared.data() + SequenceOffset, ++Sequence, 8);   // dispari: sto scrivendo
			std::atomic_thread_fence(std::memory_order_seq_cst);
			for (uint32_t Index = 0; Index < 64; ++Index)
			{
				FRecord Rec;
				Rec.Id = Round;
				Rec.LatDeg = static_cast<double>(Round);
				Rec.AltM = static_cast<double>(Round) * 2.0;
				WriteRecord(Shared.data() + HeaderBytes + RecordBytes * Index, Rec);
			}
			Detail::WriteF64(Shared.data() + 32, static_cast<double>(Round));
			std::atomic_thread_fence(std::memory_order_seq_cst);
			Detail::WriteLe(Shared.data() + SequenceOffset, ++Sequence, 8);   // pari: fatto
			// Un simulatore vero scrive a 10-60 Hz; qui molto piu' spesso, ma
			// lasciando a chi legge un attimo per trovarla ferma.
			std::this_thread::sleep_for(std::chrono::microseconds(20));
		}
	});
	int Ok = 0, Retry = 0, Broken = 0;
	const auto Start = std::chrono::steady_clock::now();
	std::vector<uint8_t> Snapshot;
	while (std::chrono::steady_clock::now() - Start < std::chrono::milliseconds(300))
	{
		std::string Why;
		const ECopyResult Result = CopyConsistent(Shared.data(), Shared.size(), Snapshot, Why);
		if (Result != ECopyResult::Ok) { ++Retry; continue; }
		FHeader H;
		std::vector<FRecord> Recs;
		if (!ReadSnapshot(Snapshot.data(), Snapshot.size(), H, Recs, Why)) { ++Retry; continue; }
		bool bWhole = true;
		for (const FRecord& Rec : Recs)
		{
			bWhole = bWhole && Rec.Id == static_cast<uint64_t>(H.SimTime) && Rec.LatDeg == H.SimTime && Rec.AltM == 2.0 * H.SimTime;
		}
		(bWhole ? Ok : Broken)++;
	}
	bStop = true;
	Writer.join();
	Check(Ok > 100 && Broken == 0, "un thread scrive, uno legge per 300 ms: nessuna fotografia a meta'",
		Fmt("%.0f intere, %.0f riprovate, %.0f rotte", Ok, Retry, Broken));
}

static std::vector<uint8_t> ReadFile(const std::string& Path)
{
	std::ifstream File(Path, std::ios::binary);
	return std::vector<uint8_t>((std::istreambuf_iterator<char>(File)), std::istreambuf_iterator<char>());
}

static void TestFixture(const std::string& Path)
{
	Section("12. La fixture del generatore Python (geoworld demo-entities)");
	using namespace SharedMemory;
	const std::vector<uint8_t> Bytes = ReadFile(Path);
	// Due fotografie una dietro l'altra, a 10,0 e 10,5 secondi.
	const size_t One = Bytes.size() / 2;
	FHeader H0, H1;
	std::vector<FRecord> R0, R1;
	std::string Error;
	const bool bRead = Bytes.size() >= 2 * HeaderBytes && Bytes.size() % 2 == 0
		&& ReadSnapshot(Bytes.data(), One, H0, R0, Error) && ReadSnapshot(Bytes.data() + One, One, H1, R1, Error);
	Check(bRead && R0.size() == 7 && R1.size() == 7 && H0.SimTime == 10.0 && H1.SimTime == 10.5 && H0.AltitudeAboveSeaLevel(),
		"due fotografie (10,0 e 10,5 s) di sette entita', quote sul livello del mare", bRead ? Fmt("%.0f byte", Bytes.size()) : Error);
	if (!bRead || R0.size() != R1.size()) { return; }

	int Planes = 0, Helis = 0, Cars = 0;
	for (size_t Index = 0; Index < R0.size(); ++Index)
	{
		const FRecord& A = R0[Index];
		const FRecord& B = R1[Index];
		const FEntityUpdate UA = ToUpdate(A, H0, 1, 0.0);
		const FEntityUpdate UB = ToUpdate(B, H1, 1, 0.0);
		const FGeodetic G = GeoWorld::Core::EcefToGeodetic(UA.State.Position);
		const FEcef Offset = EcefToNed(TurinLat * DegToRad, TurinLon * DegToRad, UA.State.Position - At(TurinLat, TurinLon, G.HeightM));
		const bool bNear = std::hypot(Offset.X, Offset.Y) < 10000.0;

		// La rotta della velocita' e' l'imbardata: Python e C++ d'accordo su NED e sul verso degli angoli.
		const double Course = std::atan2(A.VelEast, A.VelNorth) * RadToDeg;
		const bool bCourse = std::hypot(A.VelNorth, A.VelEast) < 1.0 || std::fabs(WrapDeg(Course - A.YawDeg)) < 0.01;

		// L'estrapolazione di C++ ritrova la posizione che Python ha calcolato mezzo secondo dopo.
		const FEntityState Guess = Extrapolate(UA.State, UA.Algorithm, 0.5);
		const double Miss = Distance(Guess.Position, UB.State.Position);
		const bool bRotating = RotatesOrientation(UA.Algorithm);
		const double Tolerance = UsesAcceleration(UA.Algorithm) ? 0.02 : 0.2;
		const double Turn = AngleBetween(Guess.Orientation, UB.State.Orientation);

		std::string What = A.Label + " (" + A.Kind.Text() + ", " + DeadReckoningName(UA.Algorithm) + ")";
		Check(bNear && bCourse && Miss < Tolerance && (!bRotating || Turn < 0.05),
			What + ": vicino a Torino, rotta = imbardata, estrapolato su Python",
			Fmt("%.4f m, %.4f gradi, rotta %.2f vs imbardata %.2f", Miss, Turn, Course, A.YawDeg));

		if (A.Kind.Domain == 2 && A.Kind.Category == 21) { ++Helis; }
		else if (A.Kind.Domain == 2) { ++Planes; }
		else if (A.Kind.Domain == 1 && UA.bClampToGround) { ++Cars; }

		if (A.Kind.Domain == 2 && A.Kind.Category != 21)
		{
			// Una virata coordinata: il rollio e' atan(v²/(r g)) e il corpo gira attorno al suo Giu' inclinato.
			const double Speed = std::hypot(A.VelNorth, A.VelEast);
			const double Accel = std::hypot(A.AccNorth, A.AccEast);
			const double Bank = std::atan(Accel / 9.80665) * RadToDeg;
			const double YawRate = Accel / Speed;
			const double Expected = (A.RateR > 0.0f ? 1.0 : -1.0) * Bank;
			// Entro 0,1 gradi: il rollio e' misurato nel NED del punto, che a
			// qualche km dal centro della scena e' inclinato di qualche
			// centesimo di grado rispetto a quello dove Python fa il cerchio.
			Check(std::fabs(A.RollDeg - Expected) < 0.1 && std::fabs(std::hypot(A.RateQ, A.RateR) - YawRate) < 1e-5,
				A.Label + ": virata coordinata (rollio verso l'interno della curva, q e r dal rateo di virata)",
				Fmt("rollio %.2f, atteso %.2f", A.RollDeg, Expected));
		}
	}
	Check(Planes == 2 && Helis == 1 && Cars == 4, "due aerei, un elicottero, quattro auto appoggiate al terreno");
}

int main(int ArgCount, char** Arguments)
{
	const std::string Fixture = (ArgCount > 1) ? Arguments[1] : "Plugins/GeoWorld/Source/GeoTiles/TestData/entities_fixture.bin";
	std::printf("=====================================================\n");
	std::printf(" GeoEntities -- lo stato, i frame, il tempo, i modelli\n");
	std::printf("=====================================================\n");
	TestQuaternion();
	TestTrapNed();
	TestEuler();
	TestTrapDisCigi();
	TestTrapBodyAxes();
	TestDeadReckoning();
	TestBlend();
	TestTime();
	TestRegistry();
	TestModels();
	TestSharedMemory();
	TestFixture(Fixture);
	std::printf("\n=====================================================\n");
	std::printf(" RISULTATO: %d passati, %d falliti\n", GPassed, GFailed);
	std::printf("=====================================================\n");
	return GFailed == 0 ? 0 : 1;
}

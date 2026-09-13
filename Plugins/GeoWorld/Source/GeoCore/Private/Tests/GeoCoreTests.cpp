// =============================================================================
//  Test di automazione di Unreal per GeoCore.
//
//  Sono il gemello dei test in Plugins/GeoWorld/Tools/StandaloneTests: stesse
//  asserzioni, stesse soglie. I due esistono entrambi perche' verificano cose
//  diverse:
//    - l'harness standalone verifica la MATEMATICA, gira in un secondo su
//      qualunque macchina e non richiede Unreal;
//    - questi verificano che il PONTE verso Unreal (FVector, FQuat, il
//      subsystem) non introduca errori, cosa che l'harness non puo' vedere.
//
//  COME LANCIARLI
//   - dall'editor:  Window -> Test Automation, filtrare "GeoWorld", Start Tests
//   - da riga di comando (headless):
//       UnrealEditor-Cmd.exe <progetto>.uproject ^
//         -ExecCmds="Automation RunTests GeoWorld; Quit" ^
//         -unattended -nopause -nullrhi -log
//
//  NOTA UE: WITH_DEV_AUTOMATION_TESTS e' 0 nelle build Shipping, quindi tutto
//  questo file sparisce dal binario finale. Va sempre messa questa guardia
//  attorno ai test, altrimenti la build di Shipping non compila.
// =============================================================================

#include "Misc/AutomationTest.h"

#include "Geo/Georeference.h"
#include "Unreal/GeoreferenceSnapshot.h"
#include "Unreal/GeoWorldTypes.h"

#if WITH_DEV_AUTOMATION_TESTS

using namespace GeoWorld::Core;

namespace GeoTestUtils
{
	/**
	 * NOTA UE sui FLAG dei test:
	 *   EditorContext -> gira nell'editor (i nostri test sono matematica pura,
	 *                    non serve un mondo di gioco).
	 *   EngineFilter  -> categoria "Engine" nella finestra Test Automation.
	 *
	 * Se una versione futura di Unreal rinominasse queste costanti, la
	 * correzione e' una riga sola: e' il motivo per cui sono raccolte qui invece
	 * di essere ripetute in ogni macro.
	 */
	// "auto" e non un tipo esplicito: fra UE 5.4 e 5.5 EAutomationTestFlags e'
	// passato da namespace di costanti uint32 a enum class. Con auto il codice
	// compila in entrambi i casi senza doverlo toccare.
	static constexpr auto TestFlags =
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

	static double EcefDistance(const FEcef& A, const FEcef& B)
	{
		return (A - B).Length();
	}
}

// ============================================================================
//  1. Round-trip geodetiche -> ECEF -> geodetiche
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeoCoreRoundTripTest,
	"GeoWorld.GeoCore.EllipsoidRoundTrip", GeoTestUtils::TestFlags)

bool FGeoCoreRoundTripTest::RunTest(const FString& Parameters)
{
	double MaxPositionErrorM = 0.0;
	double MaxHeightErrorM   = 0.0;

	// Griglia piu' rada di quella dell'harness standalone: qui interessa la
	// non-regressione, non la caccia al caso patologico. Un test di automazione
	// che impiega dieci secondi e' un test che si finisce per disattivare.
	for (int32 LatIndex = -90; LatIndex <= 90; LatIndex += 3)
	{
		for (int32 LonIndex = -180; LonIndex <= 180; LonIndex += 9)
		{
			for (double Height : { -500.0, 0.0, 1500.0, 9000.0 })
			{
				const FGeodetic In   = FGeodetic::FromDegrees(LatIndex, LonIndex, Height);
				const FEcef     Ecef = GeodeticToEcef(In);
				const FGeodetic Out  = EcefToGeodetic(Ecef);

				MaxPositionErrorM = FMath::Max(MaxPositionErrorM,
					GeoTestUtils::EcefDistance(Ecef, GeodeticToEcef(Out)));
				MaxHeightErrorM = FMath::Max(MaxHeightErrorM, FMath::Abs(Out.HeightM - In.HeightM));
			}
		}
	}

	TestTrue(FString::Printf(TEXT("Errore di posizione %.3e m < 1e-6 m"), MaxPositionErrorM),
		MaxPositionErrorM < 1e-6);
	TestTrue(FString::Printf(TEXT("Errore di quota %.3e m < 1e-6 m"), MaxHeightErrorM),
		MaxHeightErrorM < 1e-6);

	return true;
}

// ============================================================================
//  2. Bowring (produzione) contro l'iterativo di riferimento
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeoCoreBowringVsIterativeTest,
	"GeoWorld.GeoCore.BowringVsIterative", GeoTestUtils::TestFlags)

bool FGeoCoreBowringVsIterativeTest::RunTest(const FString& Parameters)
{
	// Seme fisso: un test che fallisce solo ogni tanto e' peggio di nessun test.
	FRandomStream Random(20260913);

	double MaxLatDifferenceRad = 0.0;
	double MaxHeightDifferenceM = 0.0;

	for (int32 Index = 0; Index < 20000; ++Index)
	{
		const FGeodetic In = FGeodetic::FromDegrees(
			Random.FRandRange(-89.999f, 89.999f),
			Random.FRandRange(-180.0f, 180.0f),
			Random.FRandRange(-11000.0f, 30000.0f));

		const FEcef Ecef = GeodeticToEcef(In);

		const FGeodetic Fast      = EcefToGeodetic(Ecef);
		const FGeodetic Reference = EcefToGeodeticIterative(Ecef);

		MaxLatDifferenceRad  = FMath::Max(MaxLatDifferenceRad,  FMath::Abs(Fast.LatRad  - Reference.LatRad));
		MaxHeightDifferenceM = FMath::Max(MaxHeightDifferenceM, FMath::Abs(Fast.HeightM - Reference.HeightM));
	}

	// 1e-13 rad sul raggio terrestre valgono circa 0.6 micrometri.
	TestTrue(FString::Printf(TEXT("Differenza di latitudine %.3e rad < 1e-13"), MaxLatDifferenceRad),
		MaxLatDifferenceRad < 1e-13);
	TestTrue(FString::Printf(TEXT("Differenza di quota %.3e m < 1e-6"), MaxHeightDifferenceM),
		MaxHeightDifferenceM < 1e-6);

	return true;
}

// ============================================================================
//  3. Valori analitici noti
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeoCoreKnownValuesTest,
	"GeoWorld.GeoCore.KnownValues", GeoTestUtils::TestFlags)

bool FGeoCoreKnownValuesTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("Semiasse minore WGS84 = 6356752.314245 m"),
		FMath::Abs(WGS84.B - 6356752.314245179497) < 1e-6);

	// Equatore / Greenwich / quota zero -> (a, 0, 0)
	{
		const FEcef Position = GeodeticToEcef(FGeodetic::FromDegrees(0.0, 0.0, 0.0));
		TestTrue(TEXT("lat 0, lon 0, h 0 -> (a, 0, 0)"),
			FMath::Abs(Position.X - WGS84.A) < 1e-9
			&& FMath::Abs(Position.Y) < 1e-9
			&& FMath::Abs(Position.Z) < 1e-9);
	}

	// Polo Nord: caso limite in cui la longitudine e' indefinita e le formule
	// ingenue dividono per zero.
	{
		const FEcef Position = GeodeticToEcef(FGeodetic::FromDegrees(90.0, 0.0, 0.0));
		TestTrue(TEXT("polo Nord -> (0, 0, b)"),
			FMath::Abs(Position.Z - WGS84.B) < 1e-9 && FMath::Sqrt(Position.X * Position.X + Position.Y * Position.Y) < 1e-9);

		const FGeodetic Back = EcefToGeodetic(Position);
		TestTrue(TEXT("polo Nord -> geodetiche senza divisione per zero"),
			FMath::Abs(Back.LatDeg() - 90.0) < 1e-12 && FMath::Abs(Back.HeightM) < 1e-9);
	}

	// La normale geodetica non coincide con quella geocentrica: ~11.5 primi a 45 gradi.
	{
		const FGeodetic At45 = FGeodetic::FromDegrees(45.0, 0.0, 0.0);
		const double AngleDeg = FMath::Acos(
			GeodeticSurfaceNormal(At45).Dot(GeodeticToEcef(At45).Normalized())) * RadToDeg;
		TestTrue(FString::Printf(TEXT("Normale geodetica vs geocentrica a 45 deg: %.4f deg"), AngleDeg),
			AngleDeg > 0.18 && AngleDeg < 0.20);
	}

	return true;
}

// ============================================================================
//  4. Frame ENU: la caduta per curvatura terrestre
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeoCoreEnuCurvatureTest,
	"GeoWorld.GeoCore.EnuCurvature", GeoTestUtils::TestFlags)

bool FGeoCoreEnuCurvatureTest::RunTest(const FString& Parameters)
{
	const FGeodetic Origin = FGeodetic::FromDegrees(45.0, 9.0, 0.0);
	const FEnuFrame Frame(Origin);

	// Raggio di curvatura meridiano alla latitudine dell'origine.
	const double SinLat = FMath::Sin(Origin.LatRad);
	const double MeridionalRadius = WGS84.A * (1.0 - WGS84.E2)
		/ FMath::Pow(1.0 - WGS84.E2 * SinLat * SinLat, 1.5);

	const FGeodetic OneKmNorth = FGeodetic::FromRadians(
		Origin.LatRad + 1000.0 / MeridionalRadius, Origin.LonRad, 0.0);

	const FEnu Local = Frame.GeodeticToEnu(OneKmNorth);
	const double ExpectedDropM = 1000.0 * 1000.0 / (2.0 * MeridionalRadius);

	TestTrue(FString::Printf(TEXT("Componente Nord %.4f m ~ 1000 m"), Local.N),
		FMath::Abs(Local.N - 1000.0) < 0.01);

	// La verifica che conta: e' fisica, non tautologica. Un punto a 1 km di
	// distanza sta ~78.5 mm sotto il piano tangente. Se questo numero cambia,
	// abbiamo sbagliato la normale o il frame.
	TestTrue(FString::Printf(TEXT("Caduta per curvatura %.4f m ~ %.4f m attesi"), Local.U, -ExpectedDropM),
		FMath::Abs(Local.U + ExpectedDropM) < 0.002);

	return true;
}

// ============================================================================
//  5. Handedness delle basi: e' la difesa contro il mondo specchiato
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeoCoreBasisHandednessTest,
	"GeoWorld.GeoCore.BasisHandedness", GeoTestUtils::TestFlags)

bool FGeoCoreBasisHandednessTest::RunTest(const FString& Parameters)
{
	double MaxOrthonormalityError = 0.0;
	bool bEnuAlwaysRightHanded = true;
	bool bNeuAlwaysLeftHanded  = true;

	for (int32 LatIndex = -85; LatIndex <= 85; LatIndex += 5)
	{
		for (int32 LonIndex = -180; LonIndex <= 180; LonIndex += 15)
		{
			const double Lat = LatIndex * DegToRad;
			const double Lon = LonIndex * DegToRad;

			const FMat3 Enu = MakeEnuBasis(Lat, Lon);
			const FMat3 Neu = MakeNeuBasis(Lat, Lon);

			bEnuAlwaysRightHanded &= FMath::Abs(Enu.Determinant() - 1.0) < 1e-14;
			bNeuAlwaysLeftHanded  &= FMath::Abs(Neu.Determinant() + 1.0) < 1e-14;

			const FMat3 Product = Enu * Enu.Transposed();
			for (int32 i = 0; i < 3; ++i)
			{
				for (int32 j = 0; j < 3; ++j)
				{
					MaxOrthonormalityError = FMath::Max(MaxOrthonormalityError,
						FMath::Abs(Product.M[i][j] - ((i == j) ? 1.0 : 0.0)));
				}
			}
		}
	}

	TestTrue(TEXT("Base ENU destrorsa ovunque (det +1)"), bEnuAlwaysRightHanded);
	TestTrue(TEXT("Base NEU sinistrorsa ovunque (det -1): e' la riflessione per Unreal"),
		bNeuAlwaysLeftHanded);
	TestTrue(FString::Printf(TEXT("Ortonormalita' entro %.3e"), MaxOrthonormalityError),
		MaxOrthonormalityError < 1e-14);

	return true;
}

// ============================================================================
//  6. Georeferenziazione: unita', assi, e il ponte verso FVector/FQuat.
//     E' la parte che l'harness standalone non puo' verificare.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeoCoreUnrealBridgeTest,
	"GeoWorld.GeoCore.UnrealBridge", GeoTestUtils::TestFlags)

bool FGeoCoreUnrealBridgeTest::RunTest(const FString& Parameters)
{
	const FGeodetic Origin = FGeodetic::FromDegrees(41.890210, 12.492231, 40.0);   // Colosseo

	FGeoreferenceSnapshot Snapshot;
	Snapshot.Georeference.SetOrigin(Origin);

	// --- L'origine e' l'origine ---
	TestTrue(TEXT("L'origine mappa in (0,0,0)"),
		Snapshot.GeodeticToUnreal(Origin).Size() < 1e-6);

	// --- Unita': 1 metro = 100 unita' ---
	{
		const FEcef OneMeterUp = Snapshot.Georeference.GetOriginEcef()
			+ GeodeticSurfaceNormal(Origin) * 1.0;
		const FVector Position = Snapshot.EcefToUnreal(OneMeterUp);

		TestTrue(FString::Printf(TEXT("1 metro in alto = %.6f unita' (atteso 100)"), Position.Z),
			FMath::Abs(Position.Z - 100.0) < 1e-5 && Position.Size2D() < 1e-5);
	}

	// --- Assi: Nord -> +X, Est -> +Y (yaw = azimuth della bussola) ---
	{
		const FMat3 Enu = MakeEnuBasis(Origin.LatRad, Origin.LonRad);
		const FEcef OriginEcef = Snapshot.Georeference.GetOriginEcef();

		const FVector North = Snapshot.EcefToUnreal(OriginEcef + Enu.GetRow(1) * 1000.0);
		const FVector East  = Snapshot.EcefToUnreal(OriginEcef + Enu.GetRow(0) * 1000.0);

		TestTrue(FString::Printf(TEXT("1 km a Nord -> +X (%.1f, %.1f)"), North.X, North.Y),
			FMath::Abs(North.X - 100000.0) < 1e-3 && FMath::Abs(North.Y) < 1e-3);
		TestTrue(FString::Printf(TEXT("1 km a Est -> +Y (%.1f, %.1f)"), East.X, East.Y),
			FMath::Abs(East.Y - 100000.0) < 1e-3 && FMath::Abs(East.X) < 1e-3);
	}

	// --- Il quaternione di orientamento locale ---
	{
		// Nell'origine, la base locale coincide con quella del mondo: rotazione
		// identita'.
		const FQuat AtOrigin = Snapshot.GetLocalNeuRotation(Origin);
		TestTrue(FString::Printf(TEXT("Rotazione nell'origine ~ identita' (angolo %.3e rad)"),
			AtOrigin.GetAngle()), AtOrigin.GetAngle() < 1e-9);

		// A Milano (circa 480 km) la verticale locale e' inclinata di ~4.3 gradi.
		const FGeodetic Milan = FGeodetic::FromDegrees(45.464200, 9.190000, 120.0);
		const FQuat AtMilan = Snapshot.GetLocalNeuRotation(Milan);

		// Il quaternione DEVE essere normalizzato: se la matrice di partenza non
		// fosse una rotazione propria, ToQuat() restituirebbe silenziosamente
		// spazzatura ed e' qui che ce ne accorgeremmo.
		TestTrue(FString::Printf(TEXT("Quaternione normalizzato (|q| = %.15f)"), AtMilan.Size()),
			FMath::Abs(AtMilan.Size() - 1.0) < 1e-12);

		const double TiltDeg = FMath::RadiansToDegrees(
			FMath::Acos(FVector::DotProduct(AtMilan.GetUpVector(), FVector::UpVector)));
		TestTrue(FString::Printf(TEXT("Verticale locale a Milano inclinata di %.3f deg"), TiltDeg),
			TiltDeg > 3.0 && TiltDeg < 5.0);

		// L'asse Z del quaternione deve coincidere con la normale geodetica di
		// Milano portata in spazio Unreal.
		const FMat3 MilanBasis = Snapshot.Georeference.GetLocalNeuBasisInUnreal(Milan);
		const FEcef ExpectedUp = MilanBasis.GetRow(2);
		const FVector ActualUp = AtMilan.GetUpVector();

		TestTrue(TEXT("L'asse Z del quaternione e' la normale geodetica locale"),
			FMath::Abs(ActualUp.X - ExpectedUp.X) < 1e-9
			&& FMath::Abs(ActualUp.Y - ExpectedUp.Y) < 1e-9
			&& FMath::Abs(ActualUp.Z - ExpectedUp.Z) < 1e-9);
	}

	// --- Round-trip oltre 1000 km ---
	{
		const FGeodetic Far = FGeodetic::FromDegrees(50.850300, 4.351700, 100.0);   // Bruxelles
		const FVector   Position = Snapshot.GeodeticToUnreal(Far);
		const FGeodetic Back     = Snapshot.UnrealToGeodetic(Position);

		const double DistanceKm = Snapshot.DistanceFromOriginMeters(Position) / 1000.0;
		const double ErrorM = GeoTestUtils::EcefDistance(GeodeticToEcef(Far), GeodeticToEcef(Back));

		TestTrue(FString::Printf(TEXT("Punto di prova a %.1f km"), DistanceKm), DistanceKm > 1000.0);
		TestTrue(FString::Printf(TEXT("Round-trip a %.0f km: errore %.3e m"), DistanceKm, ErrorM),
			ErrorM < 1e-6);
	}

	return true;
}

// ============================================================================
//  7. Invarianza al rebasing: il test centrale della Fase 1
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeoCoreRebasingInvarianceTest,
	"GeoWorld.GeoCore.RebasingInvariance", GeoTestUtils::TestFlags)

bool FGeoCoreRebasingInvarianceTest::RunTest(const FString& Parameters)
{
	// Un punto geografico fisso. La sua posizione ECEF e' l'unica autorita' e
	// non cambia mai, qualunque cosa faccia l'origine del mondo.
	const FGeodetic Target = FGeodetic::FromDegrees(41.890210, 12.492231, 40.0);
	const FEcef TargetEcef = GeodeticToEcef(Target);

	FGeoreferenceSnapshot Snapshot;
	Snapshot.Georeference.SetOrigin(Target);

	FRandomStream Random(987654321);
	double MaxErrorM = 0.0;
	double MaxUnrealDriftUu = 0.0;

	for (int32 Index = 0; Index < 1000; ++Index)
	{
		// Origini sparse su tutta l'Italia, con quote fino alla quota di crociera.
		Snapshot.Georeference.SetOrigin(FGeodetic::FromDegrees(
			Random.FRandRange(35.0f, 48.0f),
			Random.FRandRange(6.0f, 19.0f),
			Random.FRandRange(0.0f, 12000.0f)));
		++Snapshot.Generation;

		// La posizione Unreal si RICALCOLA dall'ECEF, mai dalla precedente:
		// e' questo che impedisce alla deriva di accumularsi.
		const FVector Position = Snapshot.EcefToUnreal(TargetEcef);
		const FEcef   Back     = Snapshot.UnrealToEcef(Position);

		MaxErrorM = FMath::Max(MaxErrorM, GeoTestUtils::EcefDistance(TargetEcef, Back));
	}

	// Controllo finale: torno all'origine iniziale e la posizione Unreal deve
	// essere IDENTICA a quella di partenza, bit per bit o quasi.
	Snapshot.Georeference.SetOrigin(Target);
	MaxUnrealDriftUu = Snapshot.EcefToUnreal(TargetEcef).Size();

	TestTrue(FString::Printf(TEXT("Nessuna deriva dopo 1000 rebase: %.3e m"), MaxErrorM),
		MaxErrorM < 1e-7);
	TestTrue(FString::Printf(TEXT("Tornando all'origine iniziale il punto e' a %.3e uu da (0,0,0)"),
		MaxUnrealDriftUu), MaxUnrealDriftUu < 1e-6);

	return true;
}

// ============================================================================
//  8. Disciplina delle unita': FGeoCoordinate <-> FGeodetic
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeoCoreCoordinateTypeTest,
	"GeoWorld.GeoCore.CoordinateType", GeoTestUtils::TestFlags)

bool FGeoCoreCoordinateTypeTest::RunTest(const FString& Parameters)
{
	const FGeoCoordinate Original(41.890210, 12.492231, 40.0);
	const FGeoCoordinate RoundTripped = FGeoCoordinate::FromGeodetic(Original.ToGeodetic());

	TestTrue(TEXT("Round-trip gradi -> radianti -> gradi"),
		FMath::Abs(RoundTripped.Latitude  - Original.Latitude)  < 1e-12
		&& FMath::Abs(RoundTripped.Longitude - Original.Longitude) < 1e-12
		&& FMath::Abs(RoundTripped.HeightMeters - Original.HeightMeters) < 1e-12);

	// Difesa contro lo scambio lat/lon: le factory rendono l'errore impossibile
	// da scrivere per sbaglio, ma verifichiamo che non siano invertite a monte.
	const FGeodetic Geodetic = FGeoCoordinate(45.0, 9.0, 0.0).ToGeodetic();
	TestTrue(TEXT("Latitudine e longitudine non sono scambiate"),
		FMath::Abs(Geodetic.LatDeg() - 45.0) < 1e-12
		&& FMath::Abs(Geodetic.LonDeg() - 9.0) < 1e-12);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS

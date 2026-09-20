// =============================================================================
//  Test dello strato puro del quadtree (Fase 4), senza Unreal.
// =============================================================================
#include "Quadtree/Culling.h"
#include "Quadtree/TileSelector.h"
#include "Tiles/TilingScheme.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>
#include <string>

using namespace GeoWorld;
using namespace GeoWorld::Quadtree;
using GeoWorld::Core::FEcef;
using GeoWorld::Core::FGeodetic;
using GeoWorld::Core::WGS84;
using GeoWorld::Tiles::FTileKey;

static int GPassed = 0, GFailed = 0;

static void Check(bool bCondition, const std::string& What, const std::string& Detail = {})
{
	(bCondition ? GPassed : GFailed)++;
	std::printf("  [%s] %s%s%s\n", bCondition ? " ok " : "FAIL", What.c_str(),
		Detail.empty() ? "" : "  -> ", Detail.c_str());
}

static void Section(const char* Title) { std::printf("\n== %s ==\n", Title); }

static std::string Fmt(const char* Format, double Value)
{
	char Buffer[160];
	std::snprintf(Buffer, sizeof(Buffer), Format, Value);
	return Buffer;
}

/** Camera che guarda verso il basso su un punto, da una quota data. */
static FViewParameters MakeDownwardView(double LatDeg, double LonDeg, double AltitudeM)
{
	FViewParameters View;
	View.CameraEcef = GeodeticToEcef(FGeodetic::FromDegrees(LatDeg, LonDeg, AltitudeM));

	const FEcef Up = GeodeticSurfaceNormal(FGeodetic::FromDegrees(LatDeg, LonDeg, 0.0));
	const FEcef East{ -std::sin(LonDeg * Core::DegToRad), std::cos(LonDeg * Core::DegToRad), 0.0 };
	const FEcef North{ -std::sin(LatDeg * Core::DegToRad) * std::cos(LonDeg * Core::DegToRad),
	                   -std::sin(LatDeg * Core::DegToRad) * std::sin(LonDeg * Core::DegToRad),
	                    std::cos(LatDeg * Core::DegToRad) };

	View.Forward = FEcef{ -Up.X, -Up.Y, -Up.Z };   // verso il basso
	View.Up = North;
	View.Right = East;
	View.VerticalFovRad = 60.0 * Core::DegToRad;
	View.AspectRatio = 16.0 / 9.0;
	View.ScreenHeightPixels = 1080.0;
	View.MaxScreenSpaceError = 4.0;

	// Frustum ESATTO, senza margine. Il margine di precaricamento (1.2 di
	// default nel motore) serve a chiedere le tile prima che entrino in vista,
	// e allarga apposta il frustum: un test che misura la geometria vera deve
	// spegnerlo, altrimenti misura il margine. I test del margine se lo
	// impostano da soli.
	View.FrustumMarginFactor = 1.0;
	return View;
}

/** Camera all'altezza data che guarda ORIZZONTALMENTE verso nord. */
static FViewParameters MakeHorizontalView(double LatDeg, double LonDeg, double AltitudeM)
{
	FViewParameters View = MakeDownwardView(LatDeg, LonDeg, AltitudeM);
	const FEcef Up = GeodeticSurfaceNormal(FGeodetic::FromDegrees(LatDeg, LonDeg, 0.0));
	View.Forward = View.Up;        // il nord locale
	View.Up = Up;
	return View;
}

// ===========================================================================
static void TestBoundingVolume()
{
	Section("1. Volume di contenimento");

	const auto Bounds = Tiles::GetTileBounds(10, 1094, 273);
	const FTileBoundingVolume Volume = MakeTileBoundingVolume(
		Bounds.West, Bounds.South, Bounds.East, Bounds.North, 0.0, 2000.0);

	bool bAllInside = true;
	double Worst = 0.0;
	for (int Index = 0; Index < Volume.SampleCount; ++Index)
	{
		const double Distance = (Volume.Samples[Index] - Volume.Centre).Length();
		Worst = std::max(Worst, Distance - Volume.Radius);
		if (Distance > Volume.Radius + 1e-6) { bAllInside = false; }
	}
	Check(bAllInside, "tutti i campioni stanno dentro la sfera", Fmt("scarto max %.3e m", Worst));
	Check(Volume.SampleCount == 18, "18 campioni (3x3 su due quote)",
		std::to_string(Volume.SampleCount));

	// I campioni dei figli devono stare dentro la sfera del padre: e' la
	// condizione perche' scartare un padre possa scartare l'intero sottoalbero.
	const FTileKey Parent{ 10, 1094, 273 };
	bool bChildrenInside = true;
	double WorstChild = 0.0;
	for (int ChildIndex = 0; ChildIndex < 4; ++ChildIndex)
	{
		const FTileKey Child = Parent.GetChild(ChildIndex);
		const auto ChildBounds = Tiles::GetTileBounds(Child.Level, Child.X, Child.Y);
		const FTileBoundingVolume ChildVolume = MakeTileBoundingVolume(
			ChildBounds.West, ChildBounds.South, ChildBounds.East, ChildBounds.North, 0.0, 2000.0);

		for (int Index = 0; Index < ChildVolume.SampleCount; ++Index)
		{
			const double Distance = (ChildVolume.Samples[Index] - Volume.Centre).Length();
			WorstChild = std::max(WorstChild, Distance - Volume.Radius);
			if (Distance > Volume.Radius + 1.0) { bChildrenInside = false; }
		}
	}
	Check(bChildrenInside, "i campioni dei figli stanno nella sfera del padre",
		Fmt("scarto max %.3f m", WorstChild));

	// Una tile grande deve avere un raggio dell'ordine della sua estensione.
	const auto Big = Tiles::GetTileBounds(2, 2, 1);
	const FTileBoundingVolume BigVolume = MakeTileBoundingVolume(
		Big.West, Big.South, Big.East, Big.North, 0.0, 0.0);
	Check(BigVolume.Radius > 1.0e6 && BigVolume.Radius < 5.0e6,
		"il raggio di una tile di 45 gradi e' dell'ordine dei 1000 km",
		Fmt("%.0f km", BigVolume.Radius / 1000.0));
}

// ===========================================================================
static void TestFrustum()
{
	Section("2. Frustum culling");

	const FViewParameters View = MakeDownwardView(41.89, 12.49, 10000.0);
	const FFrustumPlanes Planes = MakeFrustumPlanes(View);

	// Un punto 5 km davanti (cioe' sotto) deve essere dentro.
	const FEcef Ahead = View.CameraEcef + View.Forward * 5000.0;
	Check(IsSphereInFrustum(Planes, Ahead, 1.0), "un punto davanti e' dentro il frustum");

	// Un punto 5 km DIETRO deve essere fuori (piano near).
	const FEcef Behind = View.CameraEcef - View.Forward * 5000.0;
	Check(!IsSphereInFrustum(Planes, Behind, 1.0), "un punto dietro e' scartato");

	// Un punto molto di lato: con 60 gradi verticali e 16:9, il semiangolo
	// orizzontale e' ~46.8 gradi. A 5 km di distanza il bordo sta a ~5.3 km.
	const FEcef FarSide = View.CameraEcef + View.Forward * 5000.0 + View.Right * 20000.0;
	Check(!IsSphereInFrustum(Planes, FarSide, 1.0), "un punto molto di lato e' scartato");

	// La stessa posizione, ma con una sfera abbastanza grande da entrare.
	Check(IsSphereInFrustum(Planes, FarSide, 30000.0),
		"una sfera grande che interseca il frustum viene tenuta");

	// Controllo del semiangolo orizzontale: un punto appena dentro e uno appena
	// fuori il bordo calcolato analiticamente.
	const double HalfV = View.VerticalFovRad * 0.5;
	const double HalfH = std::atan(std::tan(HalfV) * View.AspectRatio);
	const double EdgeOffset = 5000.0 * std::tan(HalfH);

	const FEcef JustInside = View.CameraEcef + View.Forward * 5000.0 + View.Right * (EdgeOffset * 0.98);
	const FEcef JustOutside = View.CameraEcef + View.Forward * 5000.0 + View.Right * (EdgeOffset * 1.02);
	Check(IsSphereInFrustum(Planes, JustInside, 0.0) && !IsSphereInFrustum(Planes, JustOutside, 0.0),
		"il bordo laterale coincide con il semiangolo analitico",
		Fmt("semiangolo %.2f gradi", HalfH * Core::RadToDeg));
}

// ===========================================================================
static void TestHorizon()
{
	Section("3. Horizon culling sull'ellissoide");

	// Camera a 1000 m su Roma. Distanza dell'orizzonte: sqrt(2*R*h) ~ 113 km.
	const double Altitude = 1000.0;
	const FEcef Camera = GeodeticToEcef(FGeodetic::FromDegrees(41.89, 12.49, Altitude));
	const FEcef ScaledCamera = ToScaledSpace(Camera, WGS84);
	const double HorizonSquared = ComputeCameraHorizonSquared(ScaledCamera);

	Check(HorizonSquared > 0.0, "camera fuori dall'ellissoide: orizzonte definito");

	auto PointAtDistanceNorth = [](double Km, double Height)
	{
		// ~111.132 km per grado di latitudine.
		return GeodeticToEcef(FGeodetic::FromDegrees(41.89 + Km / 111.132, 12.49, Height));
	};

	// Sotto l'orizzonte teorico: visibile.
	Check(!IsPointBelowHorizon(ScaledCamera, HorizonSquared, PointAtDistanceNorth(50.0, 0.0), WGS84),
		"un punto al livello del mare a 50 km e' visibile (orizzonte ~113 km)");

	// Oltre l'orizzonte: occluso.
	Check(IsPointBelowHorizon(ScaledCamera, HorizonSquared, PointAtDistanceNorth(300.0, 0.0), WGS84),
		"un punto al livello del mare a 300 km e' oltre l'orizzonte");

	// LA verifica che conta: una montagna alta, alla stessa distanza, deve
	// restare visibile. L'orizzonte di un punto a 10 km di quota vale a sua
	// volta sqrt(2*R*H) ~ 356 km, quindi 113 + 356 = 469 km di portata totale.
	// E' la seconda condizione del test: senza, si scarterebbero le cime lontane.
	Check(!IsPointBelowHorizon(ScaledCamera, HorizonSquared, PointAtDistanceNorth(300.0, 10000.0), WGS84),
		"una vetta a 10 km di quota a 300 km resta visibile");

	Check(IsPointBelowHorizon(ScaledCamera, HorizonSquared, PointAtDistanceNorth(600.0, 10000.0), WGS84),
		"la stessa vetta a 600 km e' oltre la portata combinata (~469 km)");

	// Gli antipodi sono sempre occlusi.
	Check(IsPointBelowHorizon(ScaledCamera, HorizonSquared,
		GeodeticToEcef(FGeodetic::FromDegrees(-41.89, -167.51, 0.0)), WGS84),
		"gli antipodi sono occlusi");

	// Trovare empiricamente la distanza d'orizzonte e confrontarla con la teoria.
	double Found = 0.0;
	for (double Km = 1.0; Km < 400.0; Km += 0.5)
	{
		if (IsPointBelowHorizon(ScaledCamera, HorizonSquared, PointAtDistanceNorth(Km, 0.0), WGS84))
		{
			Found = Km;
			break;
		}
	}
	const double Theoretical = std::sqrt(2.0 * 6371000.0 * Altitude) / 1000.0;
	Check(std::abs(Found - Theoretical) < 3.0,
		"la distanza d'orizzonte misurata coincide con sqrt(2Rh)",
		Fmt("misurata %.1f km", Found) + Fmt(", teorica %.1f km", Theoretical));

	// --- Test sulla SFERA di contenimento, che e' quello usato davvero -------
	//
	// REGRESSIONE: la prima versione testava i 18 punti campione e scartava se
	// erano occlusi tutti. Una tile di livello 0 copre mezzo pianeta e i suoi
	// campioni stanno a longitudine 0, 90 e 180: con la camera sopra Roma
	// risultavano tutti oltre l'orizzonte, e la tile che CONTENEVA la camera
	// veniva scartata. Tutta la selezione restituiva zero tile.
	{
		const auto RootBounds = Tiles::GetTileBounds(0, 1, 0);
		const FTileBoundingVolume RootVolume = MakeTileBoundingVolume(
			RootBounds.West, RootBounds.South, RootBounds.East, RootBounds.North, 0.0, 2000.0);
		Check(!IsTileBelowHorizon(RootVolume, Camera, WGS84),
			"la tile di livello 0 che contiene la camera NON viene scartata");
	}

	// Una tile piccola dall'altra parte del pianeta deve invece sparire.
	{
		const uint32_t Level = 10;
		uint32_t X = 0, Y = 0;
		Tiles::TileForLonLat(Level, -167.51, -41.89, X, Y);   // antipodi di Roma
		const auto Antipode = Tiles::GetTileBounds(Level, X, Y);
		const FTileBoundingVolume AntipodeVolume = MakeTileBoundingVolume(
			Antipode.West, Antipode.South, Antipode.East, Antipode.North, 0.0, 2000.0);
		Check(IsTileBelowHorizon(AntipodeVolume, Camera, WGS84),
			"una tile agli antipodi viene scartata");
	}

	// Una tile sotto la camera resta ovviamente visibile.
	{
		const uint32_t Level = 10;
		uint32_t X = 0, Y = 0;
		Tiles::TileForLonLat(Level, 12.49, 41.89, X, Y);
		const auto Below = Tiles::GetTileBounds(Level, X, Y);
		const FTileBoundingVolume BelowVolume = MakeTileBoundingVolume(
			Below.West, Below.South, Below.East, Below.North, 0.0, 2000.0);
		Check(!IsTileBelowHorizon(BelowVolume, Camera, WGS84),
			"la tile sotto la camera resta visibile");
	}

	// Camera dentro l'ellissoide: non si scarta niente.
	const FEcef Inside = GeodeticToEcef(FGeodetic::FromDegrees(41.89, 12.49, -100.0));
	const FEcef ScaledInside = ToScaledSpace(Inside, WGS84);
	Check(!IsPointBelowHorizon(ScaledInside, ComputeCameraHorizonSquared(ScaledInside),
		PointAtDistanceNorth(5000.0, 0.0), WGS84),
		"camera sotto la superficie: nessuno scarto per orizzonte");
}

// ===========================================================================
static void TestScreenSpaceError()
{
	Section("4. Errore su schermo");

	FViewParameters View = MakeDownwardView(41.89, 12.49, 10000.0);
	View.VerticalFovRad = 60.0 * Core::DegToRad;
	View.ScreenHeightPixels = 1080.0;

	const double Error14 = GeometricErrorMetres(14);
	const double Error13 = GeometricErrorMetres(13);
	Check(std::abs(Error13 / Error14 - 2.0) < 1e-9,
		"l'errore geometrico raddoppia salendo di un livello",
		Fmt("L14 = %.2f m", Error14) + Fmt(", L13 = %.2f m", Error13));
	Check(std::abs(Error14 - 9.54) < 0.05, "L14 ha errore geometrico ~9.5 m",
		Fmt("%.3f m", Error14));

	const double Near = ComputeScreenSpaceError(Error14, 1000.0, View);
	const double Far = ComputeScreenSpaceError(Error14, 2000.0, View);
	Check(std::abs(Near / Far - 2.0) < 1e-9, "raddoppiare la distanza dimezza l'errore su schermo",
		Fmt("%.2f px", Near) + Fmt(" -> %.2f px", Far));

	// Verifica analitica: a 1000 m, con fov 60 e 1080 px, l'altezza del frustum
	// vale 2*1000*tan(30) = 1154.7 m, quindi un metro proietta 1080/1154.7 px.
	const double PixelsPerMetre = View.ScreenHeightPixels / (2.0 * 1000.0 * std::tan(30.0 * Core::DegToRad));
	Check(std::abs(ComputeScreenSpaceError(1.0, 1000.0, View) - PixelsPerMetre) < 1e-9,
		"la formula coincide con la proiezione prospettica calcolata a mano",
		Fmt("%.4f px per metro", PixelsPerMetre));

	// Distanza zero: non deve dividere per zero.
	Check(std::isfinite(ComputeScreenSpaceError(Error14, 0.0, View)),
		"distanza nulla non produce infiniti");
}

// ===========================================================================
//  Dataset finto: una piramide su un riquadro geografico, con un insieme
//  configurabile di tile "caricate".
// ===========================================================================
class FFakeDataset : public ITileAvailability
{
public:
	FFakeDataset(double InWest, double InSouth, double InEast, double InNorth,
	             uint32_t InMinLevel, uint32_t InMaxLevel)
		: West(InWest), South(InSouth), East(InEast), North(InNorth)
		, MinLevel(InMinLevel), MaxLevel(InMaxLevel) {}

	bool TileExists(const FTileKey& Key) const override
	{
		if (Key.Level < MinLevel || Key.Level > MaxLevel) { return false; }
		const auto Bounds = Tiles::GetTileBounds(Key.Level, Key.X, Key.Y);
		return !(Bounds.East < West || Bounds.West > East
		      || Bounds.North < South || Bounds.South > North);
	}

	bool GetHeightRange(const FTileKey& Key, double& OutMin, double& OutMax) const override
	{
		if (!TileExists(Key)) { return false; }
		OutMin = 0.0;
		OutMax = 2000.0;
		return true;
	}

	bool IsTileLoaded(const FTileKey& Key) const override
	{
		return bEverythingLoaded || Loaded.count(Key) > 0;
	}

	void MarkLoaded(const FTileKey& Key) { Loaded.insert(Key); }
	void LoadEverything() { bEverythingLoaded = true; }

private:
	double West, South, East, North;
	uint32_t MinLevel, MaxLevel;
	std::set<FTileKey> Loaded;
	bool bEverythingLoaded = false;
};

static void TestSelection()
{
	Section("5. Selezione: attraversamento e raffinamento");

	// Un riquadro attorno a Roma, livelli 0..14.
	FFakeDataset Dataset(12.0, 41.0, 13.0, 42.0, 0, 14);
	Dataset.LoadEverything();

	// --- Soglia enorme: nessun raffinamento ---
	{
		FViewParameters View = MakeDownwardView(41.5, 12.5, 500000.0);
		View.MaxScreenSpaceError = 1.0e9;

		FSelectionResult Result;
		SelectTiles(View, Dataset, 0, 14, Result);

		bool bAllRoots = !Result.ToRender.empty();
		for (const FSelectedTile& Tile : Result.ToRender) { if (Tile.Key.Level != 0) { bAllRoots = false; } }
		Check(bAllRoots, "soglia enorme: si disegnano solo le radici",
			std::to_string(Result.ToRender.size()) + " tile, livello 0");
	}

	// --- Soglia severa da vicino: si scende al livello massimo ---
	{
		FViewParameters View = MakeDownwardView(41.5, 12.5, 2000.0);
		View.MaxScreenSpaceError = 2.0;

		FSelectionResult Result;
		SelectTiles(View, Dataset, 0, 14, Result);

		uint32_t Deepest = 0;
		for (const FSelectedTile& Tile : Result.ToRender) { Deepest = std::max(Deepest, Tile.Key.Level); }
		Check(Deepest == 14, "da 2 km di quota con soglia 2 px si raggiunge il livello 14",
			"livello piu' profondo " + std::to_string(Deepest));
		Check(Result.WorstScreenSpaceError > 0.0, "l'errore peggiore e' riportato",
			Fmt("%.2f px", Result.WorstScreenSpaceError));
	}

	// --- Il livello scelto cala allontanandosi ---
	{
		uint32_t Previous = 99;
		bool bMonotonic = true;
		std::string Trace;
		for (double Altitude : { 2000.0, 20000.0, 200000.0, 2000000.0 })
		{
			FViewParameters View = MakeDownwardView(41.5, 12.5, Altitude);
			View.MaxScreenSpaceError = 4.0;

			FSelectionResult Result;
			SelectTiles(View, Dataset, 0, 14, Result);

			uint32_t Deepest = 0;
			for (const FSelectedTile& Tile : Result.ToRender) { Deepest = std::max(Deepest, Tile.Key.Level); }
			Trace += std::to_string(static_cast<int>(Altitude / 1000)) + "km:L"
			      + std::to_string(Deepest) + "  ";
			if (Deepest > Previous) { bMonotonic = false; }
			Previous = Deepest;
		}
		Check(bMonotonic, "salendo di quota il livello selezionato non aumenta mai", Trace);
	}

	// --- Frustum: guardando a nord non si seleziona cio' che sta a sud ---
	{
		FViewParameters View = MakeHorizontalView(41.5, 12.5, 3000.0);
		View.MaxScreenSpaceError = 4.0;

		FSelectionResult Result;
		SelectTiles(View, Dataset, 0, 14, Result);

		bool bNothingBehind = true;
		for (const FSelectedTile& Tile : Result.ToRender)
		{
			const auto Bounds = Tiles::GetTileBounds(Tile.Key.Level, Tile.Key.X, Tile.Key.Y);
			if (Bounds.North < 41.4) { bNothingBehind = false; }   // interamente a sud
		}
		Check(bNothingBehind, "guardando a nord non si seleziona niente interamente a sud");
		Check(Result.CulledByFrustum > 0, "il frustum ha scartato qualcosa",
			std::to_string(Result.CulledByFrustum) + " nodi");
	}

	// --- Niente buchi: se i figli non sono caricati si tiene il padre ---
	{
		FFakeDataset Partial(12.0, 41.0, 13.0, 42.0, 0, 14);
		// Solo i primi livelli sono in memoria.
		for (uint32_t Level = 0; Level <= 6; ++Level)
		{
			for (uint32_t Y = 0; Y < Tiles::TilesY(Level); ++Y)
			{
				for (uint32_t X = 0; X < Tiles::TilesX(Level); ++X)
				{
					Partial.MarkLoaded(FTileKey{ Level, X, Y });
				}
			}
		}

		FViewParameters View = MakeDownwardView(41.5, 12.5, 2000.0);
		View.MaxScreenSpaceError = 2.0;

		FSelectionResult Result;
		SelectTiles(View, Partial, 0, 14, Result);

		uint32_t Deepest = 0;
		for (const FSelectedTile& Tile : Result.ToRender) { Deepest = std::max(Deepest, Tile.Key.Level); }

		Check(!Result.ToRender.empty(),
			"con i figli non caricati si disegna comunque qualcosa (niente buchi)",
			std::to_string(Result.ToRender.size()) + " tile, livello max " + std::to_string(Deepest));
		Check(Deepest <= 6, "non si scende sotto cio' che e' effettivamente in memoria",
			"livello max " + std::to_string(Deepest));
		Check(!Result.ToLoad.empty(), "i figli mancanti sono stati richiesti",
			std::to_string(Result.ToLoad.size()) + " richieste");

		// Le richieste sono ordinate per urgenza decrescente.
		bool bSorted = true;
		for (size_t Index = 1; Index < Result.ToLoad.size(); ++Index)
		{
			if (Result.ToLoad[Index - 1].Priority < Result.ToLoad[Index].Priority) { bSorted = false; }
		}
		Check(bSorted, "le richieste sono ordinate per priorita' decrescente");
	}

	// --- Una tile fuori dal dataset non produce richieste ---
	{
		FViewParameters View = MakeDownwardView(41.5, 12.5, 5000.0);
		FSelectionResult Result;
		SelectTiles(View, Dataset, 0, 14, Result);
		Check(Result.CulledByMissing > 0, "i nodi inesistenti sono contati e non richiesti",
			std::to_string(Result.CulledByMissing) + " nodi");
	}

	// --- Il numero di nodi visitati resta ragionevole ---
	{
		FViewParameters View = MakeDownwardView(41.5, 12.5, 3000.0);
		View.MaxScreenSpaceError = 4.0;
		FSelectionResult Result;
		SelectTiles(View, Dataset, 0, 14, Result);
		Check(Result.NodesVisited < 5000, "l'attraversamento visita pochi nodi",
			std::to_string(Result.NodesVisited) + " nodi, "
			+ std::to_string(Result.ToRender.size()) + " disegnate");
	}
}

// ===========================================================================
int main()
{
	std::printf("=====================================================\n");
	std::printf(" Quadtree -- test dello strato puro (senza Unreal)\n");
	std::printf("=====================================================\n");

	TestBoundingVolume();
	TestFrustum();
	TestHorizon();
	TestScreenSpaceError();
	TestSelection();

	// ------------------------------------------------------------------
	Section("6. Margine del frustum: chiedere PRIMA che serva");

	{
		FViewParameters View = MakeDownwardView(41.89, 12.49, 10000.0);
		View.FrustumMarginFactor = 1.0;

		// Un punto appena FUORI dal bordo laterale: con frustum esatto e' scartato.
		const double HalfH = std::atan(std::tan(View.VerticalFovRad * 0.5) * View.AspectRatio);
		const double JustOutside = HalfH * 1.08;

		// Direzione gia' unitaria: Forward e Right sono ortonormali, e
		// cos^2 + sin^2 = 1.
		const FEcef Direction =
			View.Forward * std::cos(JustOutside) + View.Right * std::sin(JustOutside);
		const FEcef Point = View.CameraEcef + Direction * 50000.0;

		const FFrustumPlanes Exact = MakeFrustumPlanes(View);
		Check(!IsSphereInFrustum(Exact, Point, 1.0),
			"con margine 1.0 una tile appena fuori dal bordo viene scartata");

		View.FrustumMarginFactor = 1.2;
		const FFrustumPlanes Widened = MakeFrustumPlanes(View);
		Check(IsSphereInFrustum(Widened, Point, 1.0),
			"con margine 1.2 la stessa tile viene selezionata, e quindi caricata prima");
	}

	{
		// Il margine non deve spostare cio' che sta DENTRO: una tile al centro
		// resta dentro con qualunque margine.
		FViewParameters View = MakeDownwardView(41.89, 12.49, 10000.0);
		const FEcef Ahead = View.CameraEcef + View.Forward * 50000.0;

		bool bAlwaysInside = true;
		for (double Margin : { 1.0, 1.2, 1.5, 2.0 })
		{
			View.FrustumMarginFactor = Margin;
			if (!IsSphereInFrustum(MakeFrustumPlanes(View), Ahead, 1.0)) { bAlwaysInside = false; }
		}
		Check(bAlwaysInside, "cio' che e' davanti resta dentro con qualunque margine");
	}

	{
		// Un margine assurdo non deve produrre piani degeneri.
		FViewParameters View = MakeDownwardView(41.89, 12.49, 10000.0);
		View.FrustumMarginFactor = 50.0;
		const FFrustumPlanes Planes = MakeFrustumPlanes(View);

		bool bNormalsFinite = true;
		for (int32_t Index = 0; Index < 5; ++Index)
		{
			const FEcef& N = Planes.Normals[Index];
			const double Length = std::sqrt(N.X * N.X + N.Y * N.Y + N.Z * N.Z);
			if (!std::isfinite(Length) || std::fabs(Length - 1.0) > 1e-9) { bNormalsFinite = false; }
		}
		Check(bNormalsFinite, "un margine assurdo non degenera i piani (semiangolo limitato a 85 gradi)");

		// E cio' che sta DIETRO la camera resta fuori: il piano vicino non si allarga.
		const FEcef Behind = View.CameraEcef - View.Forward * 50000.0;
		Check(!IsSphereInFrustum(Planes, Behind, 1.0),
			"e quello che sta dietro resta dietro");
	}

	std::printf("\n=====================================================\n");
	std::printf(" RISULTATO: %d passati, %d falliti\n", GPassed, GFailed);
	std::printf("=====================================================\n");
	return GFailed == 0 ? 0 : 1;
}

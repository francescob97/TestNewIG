// =============================================================================
//  Test degli edifici, senza Unreal: il formato .gbd (anche un file scritto
//  dalla pipeline vera), la triangolazione con i cortili, muri e tetti,
//  colori e finestre, il materiale compilato come C++.
// =============================================================================
#include "HlslShim.h"
#include "Buildings/BuildingMesh.h"
#include "Tiles/BuildingTileFormat.h"

#include <zlib.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace GeoWorld;
using namespace GeoWorld::Buildings;
using Tiles::FTileKey;
using BuildingMeshDetail::FPoint;

static int GPassed = 0, GFailed = 0, GSkipped = 0;

static void Check(bool bCondition, const std::string& What, const std::string& Detail = {})
{
	(bCondition ? GPassed : GFailed)++;
	std::printf("  [%s] %s%s%s\n", bCondition ? " ok " : "FAIL", What.c_str(),
		Detail.empty() ? "" : "  -> ", Detail.c_str());
}

static void Skip(const std::string& What, const std::string& Why)
{
	++GSkipped;
	std::printf("  [skip] %s  -> %s\n", What.c_str(), Why.c_str());
}

static void Section(const char* Title) { std::printf("\n== %s ==\n", Title); }

static std::string Fmt(const char* Format, double A, double B = 0.0, double C = 0.0, double D = 0.0)
{
	char Buffer[240];
	std::snprintf(Buffer, sizeof(Buffer), Format, A, B, C, D);
	return Buffer;
}

// Torino: 45,07° N, 7,68° E.
static constexpr double Lat = 45.07, Lon = 7.68;
static const double MetresEast = 111320.0 * std::cos(Lat * 3.14159265358979323846 / 180.0);

static FTileKey KeyAt(uint32_t Level, double LonDeg, double LatDeg)
{
	uint32_t X = 0, Y = 0;
	Tiles::TileForLonLat(Level, LonDeg, LatDeg, X, Y);
	return FTileKey{ Level, X, Y };
}

/** Un uint32 dell'header (la chiave del file di prova sta li'). */
static uint32_t HeaderWord(const std::vector<uint8_t>& Bytes, size_t At) { return Tiles::BuildingDetail::ReadU32(Bytes.data() + At); }

/** Legge un .gbd come il runtime: header, zlib, payload. */
static bool LoadTile(const std::vector<uint8_t>& Bytes, const FTileKey& Key, Tiles::FBuildingTile& Out, std::string& Error)
{
	Tiles::FBuildingTileHeader Header;
	if (!Tiles::ParseBuildingTileHeader(Bytes.data(), Bytes.size(), Key, Header, Error)) { return false; }
	std::vector<uint8_t> Plain(Header.PlainBytes);
	uLongf PlainSize = static_cast<uLongf>(Plain.size());
	if (uncompress(Plain.data(), &PlainSize, Bytes.data() + Tiles::BuildingTileHeaderBytes, Header.PayloadBytes) != Z_OK)
	{
		Error = "zlib";
		return false;
	}
	return Tiles::DecodeBuildingTilePayload(Header, Plain.data(), PlainSize, Out, Error);
}

/** Una sagoma rettangolare (Est x Nord metri) con l'angolo sud-ovest a (E0, N0) metri dal centro di prova. */
static Tiles::FBuildingRing Box(double E0, double N0, double WidthEast, double DepthNorth, bool bClockwise = false)
{
	Tiles::FBuildingRing Ring;
	const double Corners[4][2] = { { E0, N0 }, { E0 + WidthEast, N0 }, { E0 + WidthEast, N0 + DepthNorth }, { E0, N0 + DepthNorth } };
	for (int Index = 0; Index < 4; ++Index)
	{
		const int Corner = bClockwise ? 3 - Index : Index;
		Ring.Lon.push_back(Lon + Corners[Corner][0] / MetresEast);
		Ring.Lat.push_back(Lat + Corners[Corner][1] / 111132.0);
	}
	return Ring;
}

/** Una tile di quote che vale Height(lon, lat) su ogni post. */
template <typename FunctionType>
static std::shared_ptr<const Tiles::FHeightTile> MakeHeights(const FTileKey& Key, FunctionType&& Height)
{
	auto Tile = std::make_shared<Tiles::FHeightTile>();
	Tile->Key = Key;
	Tile->Heights.resize(static_cast<size_t>(Tiles::TilePosts) * Tiles::TilePosts);
	const Tiles::FTileBounds Bounds = Tiles::GetTileBounds(Key.Level, Key.X, Key.Y);
	const double Spacing = Tiles::PostSpacingDeg(Key.Level);
	for (int32_t J = 0; J < Tiles::TilePosts; ++J)
	{
		for (int32_t I = 0; I < Tiles::TilePosts; ++I)
		{
			Tile->Heights[static_cast<size_t>(J) * Tiles::TilePosts + I] = static_cast<float>(Height(Bounds.West + I * Spacing, Bounds.North - J * Spacing));
		}
	}
	return Tile;
}

static double TriangleArea(const std::vector<FPoint>& Points, const std::array<uint32_t, 3>& T)
{
	return 0.5 * BuildingMeshDetail::Cross(Points[T[0]], Points[T[1]], Points[T[2]]);
}

/** Le facce girano verso fuori: la normale geometrica sta dalla parte delle normali dei vertici. */
static double FacingShare(const FBuildingMesh& Mesh)
{
	int Agree = 0;
	for (size_t Triangle = 0; Triangle < Mesh.TriangleCount(); ++Triangle)
	{
		const uint32_t* I = &Mesh.Indices[Triangle * 3];
		auto At = [&Mesh](uint32_t V, int C) { return static_cast<double>(Mesh.Positions[V * 3 + C]); };
		const double Ax = At(I[1], 0) - At(I[0], 0), Ay = At(I[1], 1) - At(I[0], 1), Az = At(I[1], 2) - At(I[0], 2);
		const double Bx = At(I[2], 0) - At(I[0], 0), By = At(I[2], 1) - At(I[0], 1), Bz = At(I[2], 2) - At(I[0], 2);
		const double Fx = Ay * Bz - Az * By, Fy = Az * Bx - Ax * Bz, Fz = Ax * By - Ay * Bx;
		double Nx = 0.0, Ny = 0.0, Nz = 0.0;
		for (int Corner = 0; Corner < 3; ++Corner)
		{
			Nx += Mesh.Normals[I[Corner] * 3 + 0];
			Ny += Mesh.Normals[I[Corner] * 3 + 1];
			Nz += Mesh.Normals[I[Corner] * 3 + 2];
		}
		Agree += (Fx * Nx + Fy * Ny + Fz * Nz > 0.0) ? 1 : 0;
	}
	return Mesh.TriangleCount() ? static_cast<double>(Agree) / Mesh.TriangleCount() : 0.0;
}

// ===========================================================================

static void TestFormat(const char* FixturePath)
{
	Section("1. Il formato .gbd: header, edifici, anelli, indice");
	if (!FixturePath)
	{
		Skip("una tile scritta dalla pipeline (Python) si legge uguale qui", "manca il percorso del file di prova");
		return;
	}
	std::ifstream File(FixturePath, std::ios::binary);
	const std::vector<uint8_t> Bytes((std::istreambuf_iterator<char>(File)), std::istreambuf_iterator<char>());
	if (Bytes.size() < static_cast<size_t>(Tiles::BuildingTileHeaderBytes))
	{
		Skip("una tile scritta dalla pipeline (Python) si legge uguale qui", std::string("non leggo ") + FixturePath);
		return;
	}
	// La chiave si legge dall'header stesso (il file di prova sta fuori dalla piramide).
	const FTileKey Key{ HeaderWord(Bytes, 8), HeaderWord(Bytes, 12), HeaderWord(Bytes, 16) };
	Tiles::FBuildingTile Tile;
	std::string Error;
	const bool bRead = LoadTile(Bytes, Key, Tile, Error);
	bool bShape = bRead && Tile.Buildings.size() == 3;
	double HouseWidth = 0.0, HouseDepth = 0.0;
	if (bShape)
	{
		const Tiles::FBuilding& House = Tile.Buildings[0];
		const Tiles::FBuilding& Palace = Tile.Buildings[1];
		const Tiles::FBuilding& Church = Tile.Buildings[2];
		const auto& Ring = House.Rings[0];
		HouseWidth = (*std::max_element(Ring.Lon.begin(), Ring.Lon.end()) - *std::min_element(Ring.Lon.begin(), Ring.Lon.end())) * MetresEast;
		HouseDepth = (*std::max_element(Ring.Lat.begin(), Ring.Lat.end()) - *std::min_element(Ring.Lat.begin(), Ring.Lat.end())) * 111132.0;
		bShape = House.Class == 1 && House.Levels == 2 && std::fabs(House.HeightM - 6.5f) < 1e-4f
			&& House.Roof == static_cast<uint8_t>(Tiles::ERoofShape::Gabled) && House.OsmKind() == 1 && House.OsmNumber() == 1001
			&& Palace.Rings.size() == 2 && Palace.OsmKind() == 2 && std::fabs(Palace.HeightM - 19.5f) < 1e-4f
			&& (Church.Flags & Tiles::BuildingWallColour) && Church.WallColour[0] == 225 && Church.Wikidata == 12345
			&& Church.Rings[0].Size() == 5;
	}
	Check(bRead && bShape && std::fabs(HouseWidth - 20.0) < 0.5 && std::fabs(HouseDepth - 10.0) < 0.5,
		"una tile scritta dalla pipeline (Python) si legge uguale qui: casa 20 x 10 m, palazzo con il cortile, chiesa colorata",
		bRead ? Fmt("la casa %.1f x %.1f m", HouseWidth, HouseDepth) : Error);

	Tiles::FBuildingTile Other;
	std::string WrongKey, Truncated;
	const bool bWrongKey = !LoadTile(Bytes, FTileKey{ Key.Level, Key.X + 1, Key.Y }, Other, WrongKey);
	std::vector<uint8_t> Short(Bytes.begin(), Bytes.end() - 3);
	const bool bShort = !LoadTile(Short, Key, Other, Truncated);
	Check(bWrongKey && bShort, "si rifiutano una chiave sbagliata e un file troncato", WrongKey + " / " + Truncated);

	// L'indice: header di 16 byte, record di 16.
	std::vector<uint8_t> Index(16 + 32, 0);
	auto Put32 = [&Index](size_t At, uint32_t Value) { for (int Octet = 0; Octet < 4; ++Octet) { Index[At + Octet] = static_cast<uint8_t>((Value >> (8 * Octet)) & 0xFF); } };
	Put32(0, Tiles::BuildingIndexMagic);
	Put32(8, 15);
	Put32(12, 2);
	Put32(16, 34166); Put32(20, 8179); Put32(24, 3); Put32(28, 158);
	Put32(32, 34167); Put32(36, 8179); Put32(40, 900); Put32(44, 40000);
	std::vector<Tiles::FBuildingIndexEntry> Entries;
	std::string IndexError;
	const bool bIndex = Tiles::ParseBuildingIndex(Index.data(), Index.size(), 15, Entries, IndexError)
		&& Entries.size() == 2 && Entries[1].Count == 900 && Entries[0].Bytes == 158;
	Check(bIndex && !Tiles::ParseBuildingIndex(Index.data(), Index.size(), 14, Entries, IndexError),
		"l'indice: le tile con quanti edifici; un livello sbagliato si rifiuta");
}

// ===========================================================================

static void TestTriangulation()
{
	Section("2. La triangolazione: contorni storti, cortili");
	using BuildingMeshDetail::Triangulate;
	auto Run = [](const std::vector<std::vector<FPoint>>& Rings, double& OutArea, bool& bCcw, size_t& OutCount)
	{
		std::vector<FPoint> Points;
		std::vector<std::array<size_t, 2>> Spans;
		for (const auto& Ring : Rings) { Spans.push_back({ Points.size(), Ring.size() }); Points.insert(Points.end(), Ring.begin(), Ring.end()); }
		std::vector<std::array<uint32_t, 3>> Triangles;
		Triangulate(Points, Spans, Triangles);
		OutArea = 0.0;
		bCcw = true;
		for (const auto& T : Triangles)
		{
			const double A = TriangleArea(Points, T);
			OutArea += A;
			bCcw = bCcw && A > 0.0;
		}
		OutCount = Triangles.size();
	};

	double Area = 0.0;
	bool bCcw = false;
	size_t Count = 0;
	Run({ { { 0, 0 }, { 30, 0 }, { 30, 10 }, { 10, 10 }, { 10, 30 }, { 0, 30 } } }, Area, bCcw, Count);
	Check(std::fabs(Area - 500.0) < 1e-6 && bCcw && Count == 4, "una L: 4 triangoli, tutta l'area, nessuno rovesciato", Fmt("area %.1f m^2", Area));

	// Il palazzo a corte: 40 x 40 con un cortile 16 x 16 (orario).
	Run({ { { 0, 0 }, { 40, 0 }, { 40, 40 }, { 0, 40 } }, { { 12, 12 }, { 12, 28 }, { 28, 28 }, { 28, 12 } } }, Area, bCcw, Count);
	Check(std::fabs(Area - (1600.0 - 256.0)) < 1e-6 && bCcw && Count == 8, "un palazzo a corte: il cortile resta vuoto",
		Fmt("area %.1f m^2 (attesa 1344), %.0f triangoli", Area, static_cast<double>(Count)));

	Run({ { { 0, 0 }, { 60, 0 }, { 60, 30 }, { 0, 30 } }, { { 5, 5 }, { 5, 12 }, { 15, 12 }, { 15, 5 } },
	      { { 40, 10 }, { 40, 25 }, { 55, 25 }, { 55, 10 } } }, Area, bCcw, Count);
	Check(std::fabs(Area - (1800.0 - 70.0 - 225.0)) < 1e-6 && bCcw, "due cortili", Fmt("area %.1f m^2 (attesa 1505)", Area));

	// Una sagoma da rilievo: vertici quasi allineati e lati di pochi centimetri.
	std::vector<FPoint> Noisy;
	for (int Step = 0; Step <= 20; ++Step) { Noisy.push_back({ Step * 1.0, (Step % 2) * 0.02 }); }
	for (int Step = 0; Step <= 20; ++Step) { Noisy.push_back({ 20.0 - Step * 1.0, 12.0 + (Step % 3) * 0.01 }); }
	const std::vector<FPoint> Clean = BuildingMeshDetail::Clean(Noisy, 0.3);
	Run({ Noisy }, Area, bCcw, Count);
	Check(Clean.size() <= 6 && std::fabs(Area - 240.0) < 1.0 && bCcw,
		"una sagoma rumorosa (42 vertici quasi allineati): la pulizia la riporta a un rettangolo, la triangolazione regge",
		Fmt("%.0f vertici dopo la pulizia, area %.1f m^2", static_cast<double>(Clean.size()), Area));

	// Una stella (non convessa) di 24 punte: tante orecchie da scegliere bene.
	std::vector<FPoint> Star;
	double StarArea = 0.0;
	for (int Point = 0; Point < 48; ++Point)
	{
		const double Angle = Point * 3.14159265358979323846 / 24.0;
		const double Radius = (Point % 2) ? 10.0 : 25.0;
		Star.push_back({ Radius * std::cos(Angle), Radius * std::sin(Angle) });
	}
	StarArea = BuildingMeshDetail::SignedArea(Star);
	Run({ Star }, Area, bCcw, Count);
	Check(std::fabs(Area - StarArea) < 1e-6 && bCcw && Count == 46, "una stella di 24 punte: 46 triangoli, nessuno fuori",
		Fmt("area %.1f contro %.1f", Area, StarArea));
}

// ===========================================================================

static Tiles::FBuilding MakeBuilding(uint8_t Class, float HeightM, uint8_t Levels, uint8_t Flags, uint64_t Id,
                                     std::vector<Tiles::FBuildingRing> Rings, uint8_t Roof = 0)
{
	Tiles::FBuilding Building;
	Building.Class = Class;
	Building.HeightM = HeightM;
	Building.Levels = Levels;
	Building.Flags = Flags;
	Building.OsmId = (1ull << 62) | Id;
	Building.Roof = Roof;
	Building.Rings = std::move(Rings);
	return Building;
}

struct FBounds3
{
	double MinZ = 1e300, MaxZ = -1e300;
	int Walls = 0, Roofs = 0, RoofUp = 0;
	bool bWallsVertical = true;
};

static FBounds3 Measure(const FBuildingMesh& Mesh)
{
	FBounds3 Out;
	for (size_t Vertex = 0; Vertex < Mesh.VertexCount(); ++Vertex)
	{
		Out.MinZ = std::min(Out.MinZ, static_cast<double>(Mesh.Positions[Vertex * 3 + 2]));
		Out.MaxZ = std::max(Out.MaxZ, static_cast<double>(Mesh.Positions[Vertex * 3 + 2]));
		const bool bWall = Mesh.Colours[Vertex * 4 + 3] == 255;
		(bWall ? Out.Walls : Out.Roofs)++;
		if (bWall) { Out.bWallsVertical = Out.bWallsVertical && std::fabs(Mesh.Normals[Vertex * 3 + 2]) < 1e-4f; }
		else { Out.RoofUp += Mesh.Normals[Vertex * 3 + 2] > 0.0f ? 1 : 0; }
	}
	return Out;
}

static void TestBuildings()
{
	Section("3. Gli edifici: muri, tetti, altezze, colori, finestre");
	const FTileKey Terrain = KeyAt(13, Lon, Lat);
	Cells::FGroundSurfaces Flat;
	Flat.Add(MakeHeights(Terrain, [](double, double) { return 300.0; }), 2);
	const Cells::FCellFrame Frame = Cells::FCellFrame::At(Lat, Lon);
	FBuildingDetail Detail;

	// La casa 20 x 10 m, due piani, tetto a due falde (dai tag), altezza stimata.
	FBuildingMesh House;
	const Tiles::FBuilding HouseData = MakeBuilding(1, 6.5f, 2, Tiles::BuildingLevelsMeasured, 101, { Box(0, 0, 20, 10) },
		static_cast<uint8_t>(Tiles::ERoofShape::Gabled));
	bool bSloped = false;
	const bool bHouse = BuildBuilding(HouseData, Frame, Flat, Detail, House, &bSloped);
	const FBounds3 HouseBox = Measure(House);
	// Falda a 24°: colmo 0,45 x meta' del lato corto = 2,25 m sopra i 6,5 m di muri.
	Check(bHouse && bSloped && std::fabs(HouseBox.MinZ - (300.0 - 1.5)) < 0.02 && std::fabs(HouseBox.MaxZ - (300.0 + 6.5 + 2.25)) < 0.02
		&& HouseBox.bWallsVertical && HouseBox.RoofUp == HouseBox.Roofs && FacingShare(House) == 1.0,
		"una casa: muri verticali da 1,5 m sotto il suolo, tetto a due falde sopra i 6,5 m stimati, tutte le facce verso fuori",
		Fmt("da %.2f a %.2f m, %.0f triangoli", HouseBox.MinZ, HouseBox.MaxZ, static_cast<double>(House.TriangleCount())));
	const float FloorM = House.Style[0];
	Check(std::fabs(FloorM - 3.25f) < 1e-4f && House.Facade[1] < 0.0f,
		"le finestre: un piano alto 3,25 m (6,5 m in due piani); la facciata comincia sotto il suolo, dove il materiale non ne mette",
		Fmt("piano %.2f m", FloorM));

	// Un condominio alto 18 m MISURATI: il tetto sta dentro i 18 m.
	FBuildingMesh Tall;
	BuildBuilding(MakeBuilding(2, 18.0f, 6, Tiles::BuildingHeightMeasured, 102, { Box(40, 0, 24, 12) }), Frame, Flat, Detail, Tall);
	const FBounds3 TallBox = Measure(Tall);
	Check(std::fabs(TallBox.MaxZ - 318.0) < 0.02, "un'altezza misurata (height=18) e' la cima: tetto compreso", Fmt("cima %.2f m", TallBox.MaxZ));

	// Un palazzo a corte: tetto piano, niente tetto sul cortile, muri del cortile verso il cortile.
	FBuildingMesh Palace;
	BuildBuilding(MakeBuilding(2, 15.5f, 5, Tiles::BuildingLevelsMeasured, 103, { Box(0, 60, 40, 40), Box(12, 72, 16, 16, true) }),
		Frame, Flat, Detail, Palace);
	bool bCourtyardEmpty = true, bCourtWallsInward = true;
	const double CourtNorth = 80.0, CourtEast = 20.0;
	for (size_t Triangle = 0; Triangle < Palace.TriangleCount(); ++Triangle)
	{
		const uint32_t* I = &Palace.Indices[Triangle * 3];
		if (Palace.Colours[I[0] * 4 + 3] != 0) { continue; }
		double N = 0.0, E = 0.0;
		for (int Corner = 0; Corner < 3; ++Corner) { N += Palace.Positions[I[Corner] * 3]; E += Palace.Positions[I[Corner] * 3 + 1]; }
		N /= 3.0; E /= 3.0;
		if (std::fabs(N - CourtNorth) < 7.5 && std::fabs(E - CourtEast) < 7.5) { bCourtyardEmpty = false; }
	}
	for (size_t Vertex = 0; Vertex < Palace.VertexCount(); ++Vertex)
	{
		const double N = Palace.Positions[Vertex * 3], E = Palace.Positions[Vertex * 3 + 1];
		if (Palace.Colours[Vertex * 4 + 3] != 255 || std::fabs(N - CourtNorth) > 8.2 || std::fabs(E - CourtEast) > 8.2) { continue; }
		// Un vertice del muro del cortile: la normale punta verso il centro del cortile.
		const double ToN = CourtNorth - N, ToE = CourtEast - E;
		if (ToN * Palace.Normals[Vertex * 3] + ToE * Palace.Normals[Vertex * 3 + 1] <= 0.0) { bCourtWallsInward = false; }
	}
	Check(bCourtyardEmpty && bCourtWallsInward && FacingShare(Palace) == 1.0,
		"un palazzo a corte: il tetto non copre il cortile, i muri del cortile guardano il cortile");

	// Una chiesa a cupola (pianta ottagonale) e un campanile a piramide.
	Tiles::FBuildingRing Octagon;
	for (int Side = 0; Side < 8; ++Side)
	{
		const double Angle = Side * 3.14159265358979323846 / 4.0;
		Octagon.Lon.push_back(Lon + (100.0 + 10.0 * std::cos(Angle)) / MetresEast);
		Octagon.Lat.push_back(Lat + (10.0 * std::sin(Angle)) / 111132.0);
	}
	FBuildingMesh Dome, Spire;
	BuildBuilding(MakeBuilding(5, 20.0f, 1, 0, 104, { Octagon }, static_cast<uint8_t>(Tiles::ERoofShape::Dome)), Frame, Flat, Detail, Dome);
	BuildBuilding(MakeBuilding(10, 30.0f, 8, 0, 105, { Box(130, 0, 6, 6) }), Frame, Flat, Detail, Spire);
	const FBounds3 DomeBox = Measure(Dome), SpireBox = Measure(Spire);
	Check(DomeBox.MaxZ > 300.0 + 20.0 + 8.0 && FacingShare(Dome) > 0.98 && SpireBox.MaxZ > 300.0 + 30.0 + 3.0 && FacingShare(Spire) == 1.0,
		"una cupola sull'ottagono, una piramide sul campanile (scelta dalla classe Torre)",
		Fmt("cupola fino a %.1f m, guglia fino a %.1f m", DomeBox.MaxZ - 300.0, SpireBox.MaxZ - 300.0));

	// Su un pendio: la base e' la quota piu' bassa, i muri a valle arrivano al suolo.
	const double Rise = 0.2;
	Cells::FGroundSurfaces Ramp;
	Ramp.Add(MakeHeights(Terrain, [Rise](double LonDeg, double) { return 300.0 + Rise * (LonDeg - Lon) * MetresEast; }), 1);
	FBuildingMesh Slope;
	BuildBuilding(MakeBuilding(1, 6.5f, 2, 0, 106, { Box(0, 0, 20, 10) }), Frame, Ramp, Detail, Slope);
	const FBounds3 SlopeBox = Measure(Slope);
	Check(std::fabs(SlopeBox.MinZ - (300.0 - 1.5)) < 0.05, "su un pendio i muri partono dalla quota piu' bassa del contorno",
		Fmt("fondo a %.2f m (suolo da 300 a 304)", SlopeBox.MinZ));

	// I colori: quello dei tag, o la tavolozza (sempre lo stesso per lo stesso edificio).
	FBuildingMesh Red, Again;
	Tiles::FBuilding Coloured = MakeBuilding(0, 9.0f, 3, Tiles::BuildingWallColour, 107, { Box(0, 200, 10, 10) });
	Coloured.WallColour[0] = 255; Coloured.WallColour[1] = 0; Coloured.WallColour[2] = 0;
	BuildBuilding(Coloured, Frame, Flat, Detail, Red);
	BuildBuilding(HouseData, Frame, Flat, Detail, Again);
	bool bRed = false;
	for (size_t Vertex = 0; Vertex < Red.VertexCount(); ++Vertex)
	{
		if (Red.Colours[Vertex * 4 + 3] == 255) { bRed = Red.Colours[Vertex * 4] == 255 && Red.Colours[Vertex * 4 + 1] == 0; break; }
	}
	Check(bRed && Again.Colours == House.Colours, "il colore dei tag vince; senza, lo stesso edificio ha sempre lo stesso colore");

	// Una pensilina: alta e sottile, niente finestre, con il fondo visibile da sotto.
	FBuildingMesh Canopy;
	Tiles::FBuilding CanopyData = MakeBuilding(9, 4.0f, 1, 0, 108, { Box(60, 60, 15, 6) });
	CanopyData.MinHeightM = 3.4f;
	BuildBuilding(CanopyData, Frame, Flat, Detail, Canopy);
	const FBounds3 CanopyBox = Measure(Canopy);
	bool bNoWindows = true, bUnderside = false;
	for (size_t Vertex = 0; Vertex < Canopy.VertexCount(); ++Vertex)
	{
		bNoWindows = bNoWindows && Canopy.Style[Vertex * 2] == 0.0f;
		bUnderside = bUnderside || Canopy.Normals[Vertex * 3 + 2] < -0.99f;
	}
	Check(std::fabs(CanopyBox.MinZ - 303.4) < 0.02 && std::fabs(CanopyBox.MaxZ - 304.0) < 0.02 && bNoWindows && bUnderside,
		"una pensilina: da 3,4 a 4 m, senza finestre, con il fondo che guarda in giu'");
}

// ===========================================================================

static void TestCell(const char* FixturePath)
{
	Section("4. Una tile intera: dettaglio, monumenti, quanto costa");
	const FTileKey Terrain = KeyAt(13, Lon, Lat);
	Cells::FGroundSurfaces Flat;
	Flat.Add(MakeHeights(Terrain, [](double, double) { return 250.0; }), 2);

	// Mille edifici a caso: case, condomini, capannoni, qualcuno a L.
	Tiles::FBuildingTile Tile;
	Tile.Key = KeyAt(15, Lon, Lat);
	const Tiles::FTileBounds Bounds = Tiles::GetTileBounds(Tile.Key.Level, Tile.Key.X, Tile.Key.Y);
	const Cells::FCellFrame Frame = Cells::FCellFrame::At(Bounds.CentreLat(), Bounds.CentreLon());
	uint64_t Seed = 1;
	auto Next = [&Seed]() { Seed = Seed * 6364136223846793005ull + 1442695040888963407ull; return static_cast<double>(Seed >> 11) / 9007199254740992.0; };
	for (int Index = 0; Index < 1000; ++Index)
	{
		const double E = (Next() - 0.5) * 380.0, N = (Next() - 0.5) * 560.0;
		const double W = 6.0 + 20.0 * Next(), D = 6.0 + 14.0 * Next();
		Tiles::FBuildingRing Ring;
		const double Corners[4][2] = { { E, N }, { E + W, N }, { E + W, N + D }, { E, N + D } };
		for (const auto& Corner : Corners)
		{
			Ring.Lon.push_back(Bounds.CentreLon() + Corner[0] / MetresEast);
			Ring.Lat.push_back(Bounds.CentreLat() + Corner[1] / 111132.0);
		}
		const uint8_t Class = static_cast<uint8_t>(Index % 3 == 0 ? 2 : (Index % 7 == 0 ? 4 : 1));
		Tile.Buildings.push_back(MakeBuilding(Class, Class == 2 ? 15.5f : 6.5f, Class == 2 ? 5 : 2, 0, 5000 + Index, { Ring }));
	}
	Tile.Buildings[17].Wikidata = 42;

	FBuildingMesh Mesh;
	std::vector<FLandmarkSpot> Spots;
	FBuildingCellStats Stats;
	const auto Started = std::chrono::steady_clock::now();
	BuildBuildingCell(Tile, Frame, Flat, FBuildingDetail(), {}, { 42 }, Mesh, Spots, Stats);
	const double Seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - Started).count();
	Check(Stats.Built == 999 && Stats.Landmarks == 1 && Spots.size() == 1 && Mesh.TriangleCount() < 999u * 24u && FacingShare(Mesh) == 1.0,
		"mille edifici in una tile: tutti costruiti (meno il monumento), al piu' 24 triangoli l'uno, tutte le facce verso fuori",
		Fmt("%.0f triangoli, %.0f tetti a falde, %.1f ms", static_cast<double>(Mesh.TriangleCount()), Stats.SlopedRoofs, Seconds * 1000.0));
	const Tiles::FBuilding& Landmark = Tile.Buildings[17];
	const double LongEast = std::fabs(Landmark.Rings[0].Lon[1] - Landmark.Rings[0].Lon[0]) * MetresEast;
	const double LongNorth = std::fabs(Landmark.Rings[0].Lat[2] - Landmark.Rings[0].Lat[1]) * 111132.0;
	const double ExpectedYaw = LongEast >= LongNorth ? 3.14159265358979323846 / 2.0 : 0.0;
	Check(!Spots.empty() && std::fabs(Spots[0].Local[2] - 250.0) < 0.05
		&& std::fabs(std::fabs(std::sin(Spots[0].YawRad - ExpectedYaw))) < 0.02,
		"il monumento (Wikidata 42) non si estrude: torna il suo posto, a terra, girato come il lato lungo",
		Spots.empty() ? std::string() : Fmt("quota %.2f m, direzione %.1f gradi", Spots[0].Local[2], Spots[0].YawRad * 180.0 / 3.14159265358979323846));

	FBuildingMesh Far;
	FBuildingDetail Distant;
	Distant.MinAreaM2 = 150.0;
	Distant.bWindows = false;
	Distant.SimplifyM = 2.0;
	FBuildingCellStats FarStats;
	BuildBuildingCell(Tile, Frame, Flat, Distant, {}, {}, Far, Spots, FarStats);
	bool bNoWindows = true;
	for (size_t Vertex = 0; Vertex < Far.VertexCount(); ++Vertex) { bNoWindows = bNoWindows && Far.Style[Vertex * 2] == 0.0f; }
	Check(FarStats.Built < Stats.Built && FarStats.Skipped > 0 && bNoWindows && Far.TriangleCount() < Mesh.TriangleCount(),
		"da lontano: via gli edifici sotto i 150 m^2, niente finestre",
		Fmt("%.0f edifici invece di %.0f, %.0f triangoli", FarStats.Built, Stats.Built, static_cast<double>(Far.TriangleCount())));

	// Una chiesa fatta a parti (building:part): il contorno porta il wikidata,
	// le parti (navata e campanile) le altezze. Senza monumento si disegnano
	// le parti; con il monumento sul contorno, il modello e niente parti.
	Tiles::FBuildingTile Church;
	Church.Key = Tile.Key;
	Tiles::FBuilding Outline = MakeBuilding(5, 12.0f, 0, Tiles::BuildingOutline, 103, { Box(100, 0, 20, 40) });
	Outline.Wikidata = 7;
	Church.Buildings.push_back(Outline);
	Church.Buildings.push_back(MakeBuilding(0, 15.0f, 0, Tiles::BuildingPart | Tiles::BuildingHeightMeasured, 104, { Box(100, 0, 20, 32) }));
	Church.Buildings.push_back(MakeBuilding(0, 40.0f, 0, Tiles::BuildingPart | Tiles::BuildingHeightMeasured, 105, { Box(110, 32, 8, 8) }));
	Church.Buildings.push_back(MakeBuilding(1, 6.5f, 2, 0, 101, { Box(0, 0, 20, 10) }));
	FBuildingMesh ChurchMesh;
	FBuildingCellStats Plain, Replaced;
	BuildBuildingCell(Church, Frame, Flat, FBuildingDetail(), {}, {}, ChurchMesh, Spots, Plain);
	const size_t PlainSpots = Spots.size();
	BuildBuildingCell(Church, Frame, Flat, FBuildingDetail(), {}, { 7 }, ChurchMesh, Spots, Replaced);
	Check(Plain.Built == 3 && Plain.Landmarks == 0 && PlainSpots == 0
		&& Replaced.Built == 1 && Replaced.Landmarks == 1 && Replaced.HiddenParts == 2 && Spots.size() == 1,
		"una chiesa a parti: senza monumento le parti (non il contorno); con il monumento sul contorno (Q7) il modello, e le parti spariscono",
		Fmt("%.0f edifici, poi %.0f edifici e %.0f parti tolte", Plain.Built, Replaced.Built, Replaced.HiddenParts));
	const bool bByPart = [&]()
	{
		FBuildingCellStats ByPart;
		BuildBuildingCell(Church, Frame, Flat, FBuildingDetail(), { (1ull << 62) | 105 }, {}, ChurchMesh, Spots, ByPart);
		return ByPart.Built == 2 && ByPart.Landmarks == 1 && ByPart.HiddenParts == 0;
	}();
	Check(bByPart, "un monumento su una parte sola (way/105, il campanile): le altre parti restano");

	// La tile della pipeline, su terreno piano.
	if (FixturePath)
	{
		std::ifstream File(FixturePath, std::ios::binary);
		const std::vector<uint8_t> Bytes((std::istreambuf_iterator<char>(File)), std::istreambuf_iterator<char>());
		if (Bytes.size() > 32)
		{
			const FTileKey Key{ HeaderWord(Bytes, 8), HeaderWord(Bytes, 12), HeaderWord(Bytes, 16) };
			Tiles::FBuildingTile Fixture;
			std::string Error;
			FBuildingMesh FixtureMesh;
			FBuildingCellStats FixtureStats;
			const Tiles::FTileBounds FixtureBounds = Tiles::GetTileBounds(Key.Level, Key.X, Key.Y);
			Cells::FGroundSurfaces Ground;
			Ground.Add(MakeHeights(KeyAt(13, FixtureBounds.CentreLon(), FixtureBounds.CentreLat()), [](double, double) { return 240.0; }), 2);
			const bool bLoaded = LoadTile(Bytes, Key, Fixture, Error);
			if (bLoaded)
			{
				BuildBuildingCell(Fixture, Cells::FCellFrame::At(FixtureBounds.CentreLat(), FixtureBounds.CentreLon()), Ground,
					FBuildingDetail(), {}, {}, FixtureMesh, Spots, FixtureStats);
			}
			Check(bLoaded && FixtureStats.Built == 3 && FacingShare(FixtureMesh) > 0.99,
				"la tile della pipeline diventa tre edifici", Fmt("%.0f triangoli", static_cast<double>(FixtureMesh.TriangleCount())));
		}
	}
}

// ===========================================================================

#define GEO_BUILDING_COLOUR(...) __VA_ARGS__
#define GEO_BUILDING_LIGHTS(...)
static HlslShim::float4 RunColour(const HlslShim::float4& Paint, const HlslShim::float2& Facade, const HlslShim::float2& Style,
                                  const HlslShim::float3& Normal, const HlslShim::float3& Pos, const HlslShim::float4& Ground,
                                  const HlslShim::float4& Frame, float UuPerMetre)
{
	using namespace HlslShim;
#include "Buildings/BuildingShader.inl"
}
#undef GEO_BUILDING_COLOUR
#undef GEO_BUILDING_LIGHTS

#define GEO_BUILDING_COLOUR(...)
#define GEO_BUILDING_LIGHTS(...) __VA_ARGS__
static HlslShim::float3 RunLights(const HlslShim::float4& Paint, const HlslShim::float2& Facade, const HlslShim::float2& Style,
                                  const HlslShim::float3& Normal, const HlslShim::float4& Night)
{
	using namespace HlslShim;
#include "Buildings/BuildingShader.inl"
}
#undef GEO_BUILDING_COLOUR
#undef GEO_BUILDING_LIGHTS

static void TestShader()
{
	Section("5. Il materiale (lo stesso testo del materiale, compilato come C++)");
	using HlslShim::float2;
	using HlslShim::float3;
	using HlslShim::float4;
	const float4 Plaster(225.0 / 255.0, 200.0 / 255.0, 150.0 / 255.0, 1.0);
	const float4 Dry(0.0, 100000.0, 150.0, 0.0), Frame(0.0, 6371000.0, 0.0, 0.0);
	const float3 Side(1.0, 0.0, 0.0), Up(0.0, 0.0, 1.0), Low(0.0, 0.0, 30000.0);
	// Una campata da 3,2 + 0,8 x 0,5 = 3,6 m; un piano 3 m. Centro della finestra: 1,8 m lungo, 1,68 m sopra il piano.
	const float4 Window = RunColour(Plaster, float2(1.8, 3.0 + 1.68), float2(3.0, 0.5), Side, Low, Dry, Frame, 100.0f);
	const float4 Pier = RunColour(Plaster, float2(0.2, 3.0 + 1.68), float2(3.0, 0.5), Side, Low, Dry, Frame, 100.0f);
	const float4 Underground = RunColour(Plaster, float2(1.8, -1.32), float2(3.0, 0.5), Side, Low, Dry, Frame, 100.0f);
	const float4 Blank = RunColour(Plaster, float2(1.8, 3.0 + 1.68), float2(0.0, 0.5), Side, Low, Dry, Frame, 100.0f);
	const float Expected = (225.0f / 255.0f) * (225.0f / 255.0f);
	Check(Window.x < 0.06f && Window.w < 0.2f && std::fabs(Pier.x - Expected) < 1e-4f && std::fabs(Underground.x - Expected) < 1e-4f
		&& std::fabs(Blank.x - Expected) < 1e-4f,
		"le finestre: vetro scuro e lucido al centro della campata, intonaco fra una e l'altra; niente sotto il suolo ne' senza piani",
		Fmt("vetro %.3f (ruvidita' %.2f), intonaco %.3f", Window.x, Window.w, Pier.x));

	const float4 Roof(170.0 / 255.0, 85.0 / 255.0, 60.0 / 255.0, 0.0);
	const float4 Snowy(0.0, 200.0, 50.0, 1.0);
	const float4 RoofSnow = RunColour(Roof, float2(0.0, 0.0), float2(0.0, 0.5), Up, Low, Snowy, Frame, 100.0f);
	const float4 WallSnow = RunColour(Plaster, float2(0.2, 2.0), float2(3.0, 0.5), Side, Low, Snowy, Frame, 100.0f);
	const float4 Wet(1.0, 100000.0, 150.0, 0.0);
	const float4 RoofWet = RunColour(Roof, float2(0.0, 0.0), float2(0.0, 0.5), Up, Low, Wet, Frame, 100.0f);
	const float4 WallWet = RunColour(Plaster, float2(0.2, 2.0), float2(3.0, 0.5), Side, Low, Wet, Frame, 100.0f);
	const float RoofDry = (170.0f / 255.0f) * (170.0f / 255.0f);
	Check(RoofSnow.x > 0.7f && std::fabs(WallSnow.x - Expected) < 1e-4f && RoofWet.x < 0.7f * RoofDry && WallWet.x > 0.8f * Expected,
		"la neve sui tetti e non sui muri; il bagnato scurisce i tetti piu' dei muri",
		Fmt("tetto innevato %.2f; bagnato: tetto %.0f%%, muro %.0f%% del colore asciutto", RoofSnow.x,   /* percentuale, non-unita */
			100.0 * RoofWet.x / RoofDry, 100.0 * WallWet.x / Expected));   /* percentuale, non-unita */

	// Le finestre accese: una facciata di 20 campate per 6 piani, di notte con
	// meta' delle finestre accese, poi di giorno (quota zero).
	const float DayLux = 10.0f;
	int Lit = 0, Windows = 0;
	bool bWarm = true, bPiersDark = true;
	for (int Column = 0; Column < 20; ++Column)
	{
		for (int Storey = 0; Storey < 6; ++Storey)
		{
			const float2 Centre(1.8 + 3.6 * Column, 3.0 * Storey + 1.68);
			const float3 Glow = RunLights(Plaster, Centre, float2(3.0, 0.5), Side, float4(0.0, DayLux, 0.5, 0.0));
			++Windows;
			if (Glow.x > 0.0f) { ++Lit; bWarm = bWarm && Glow.x >= Glow.z * 0.7f; }
			const float3 Between = RunLights(Plaster, float2(0.2 + 3.6 * Column, 3.0 * Storey + 1.68), float2(3.0, 0.5), Side,
				float4(0.0, DayLux, 0.5, 0.0));
			bPiersDark = bPiersDark && Between.x == 0.0f && Between.y == 0.0f && Between.z == 0.0f;
		}
	}
	const float3 Daytime = RunLights(Plaster, float2(1.8, 1.68), float2(3.0, 0.5), Side, float4(0.0, DayLux, 0.0, 0.0));
	const float3 Roofless = RunLights(Roof, float2(1.8, 1.68), float2(3.0, 0.5), Up, float4(0.0, DayLux, 1.0, 0.0));
	const float3 Basement = RunLights(Plaster, float2(1.8, -1.32), float2(3.0, 0.5), Side, float4(0.0, DayLux, 1.0, 0.0));
	Check(Lit > Windows * 35 / 100 && Lit < Windows * 65 / 100 && bPiersDark && bWarm && Daytime.x == 0.0f && Roofless.x == 0.0f
		&& Basement.x == 0.0f,
		"di notte meta' delle finestre accese (sempre le stesse), mai fra una e l'altra, mai sui tetti ne' sotto il suolo; di giorno spente",
		Fmt("%.0f accese su %.0f", Lit, Windows));
	int StillLit = 0;
	bool bSubset = true;
	for (int Column = 0; Column < 20; ++Column)
	{
		for (int Storey = 0; Storey < 6; ++Storey)
		{
			const float2 Centre(1.8 + 3.6 * Column, 3.0 * Storey + 1.68);
			const bool bLate = RunLights(Plaster, Centre, float2(3.0, 0.5), Side, float4(0.0, DayLux, 0.1, 0.0)).x > 0.0f;
			const bool bEvening = RunLights(Plaster, Centre, float2(3.0, 0.5), Side, float4(0.0, DayLux, 0.5, 0.0)).x > 0.0f;
			StillLit += bLate ? 1 : 0;
			bSubset = bSubset && (!bLate || bEvening);
		}
	}
	Check(bSubset && StillLit < Lit, "a notte fonda restano accese poche finestre, tutte fra quelle accese la sera",
		Fmt("%.0f su %.0f", StillLit, Lit));
}

int main(int ArgumentCount, char** Arguments)
{
	std::printf("=====================================================\n");
	std::printf(" GeoBuildings -- dalle sagome agli edifici\n");
	std::printf("=====================================================\n");
	const char* Fixture = ArgumentCount > 1 ? Arguments[1] : nullptr;
	TestFormat(Fixture);
	TestTriangulation();
	TestBuildings();
	TestCell(Fixture);
	TestShader();
	std::printf("\n=====================================================\n");
	std::printf(" RISULTATO: %d passati, %d falliti, %d saltati\n", GPassed, GFailed, GSkipped);
	std::printf("=====================================================\n");
	return GFailed == 0 ? 0 : 1;
}

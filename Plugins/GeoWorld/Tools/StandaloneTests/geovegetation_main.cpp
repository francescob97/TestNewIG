// =============================================================================
//  Test della vegetazione, senza Unreal: il formato .gvg (anche un file scritto
//  dalla pipeline vera), la griglia degli alberi, chi cresce dove, le forme
//  degli alberi, il materiale (colore, neve, vento) compilato come C++, le
//  celle attorno alla camera (quali, a che banda, su che terreno).
//
//  La decompressione zlib qui la fa la zlib di sistema; in Unreal la fa
//  FCompression. Lo strato puro riceve il payload gia' decompresso.
// =============================================================================
#include "HlslShim.h"
#include "Tiles/VegetationTileFormat.h"
#include "Vegetation/TreeMesh.h"
#include "Vegetation/TreePlacement.h"
#include "Vegetation/VegetationCells.h"

#include <zlib.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <vector>

using namespace GeoWorld;
using namespace GeoWorld::Vegetation;
using Tiles::ELandCover;
using Tiles::FTileKey;

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

static uint8_t Cover(ELandCover Class) { return static_cast<uint8_t>(Class); }

// Torino: 45,07° N, 7,68° E.
static constexpr double Lat = 45.07, Lon = 7.68;

/** Scrive una tile .gvg in memoria, come la pipeline: header, zlib. */
static std::vector<uint8_t> MakeTileBytes(const FTileKey& Key, const std::vector<uint8_t>& Classes, uint16_t Flags)
{
	uLongf Packed = compressBound(static_cast<uLong>(Classes.size()));
	std::vector<uint8_t> Payload(Packed);
	compress2(Payload.data(), &Packed, Classes.data(), static_cast<uLong>(Classes.size()), 6);
	Payload.resize(Packed);
	std::vector<uint8_t> Bytes(Tiles::VegetationTileHeaderBytes, 0);
	auto Put16 = [&Bytes](size_t At, uint16_t Value) { Bytes[At] = Value & 0xFF; Bytes[At + 1] = Value >> 8; };
	auto Put32 = [&Bytes](size_t At, uint32_t Value) { for (int Octet = 0; Octet < 4; ++Octet) { Bytes[At + Octet] = (Value >> (8 * Octet)) & 0xFF; } };
	Put32(0, Tiles::VegetationTileMagic);
	Put16(4, Tiles::VegetationTileVersion);
	Put16(6, Flags);
	Put32(8, Key.Level);
	Put32(12, Key.X);
	Put32(16, Key.Y);
	Put16(20, Tiles::VegetationRaster);
	Put32(24, static_cast<uint32_t>(Payload.size()));
	Bytes.insert(Bytes.end(), Payload.begin(), Payload.end());
	return Bytes;
}

/** Legge un .gvg come farebbe il runtime: header, zlib, payload. */
static bool LoadTile(const std::vector<uint8_t>& Bytes, const FTileKey& Key, Tiles::FVegetationTile& Out, std::string& Error)
{
	Tiles::FVegetationTileHeader Header;
	if (!Tiles::ParseVegetationTileHeader(Bytes.data(), Bytes.size(), Key, Header, Error)) { return false; }
	std::vector<uint8_t> Plain(Header.ExpectedPlainBytes());
	uLongf PlainSize = static_cast<uLongf>(Plain.size());
	if (uncompress(Plain.data(), &PlainSize, Bytes.data() + Tiles::VegetationTileHeaderBytes, Header.PayloadBytes) != Z_OK)
	{
		Error = "zlib";
		return false;
	}
	Plain.resize(PlainSize);
	return Tiles::DecodeVegetationTilePayload(Header, Plain.data(), Plain.size(), Out, Error);
}

// ===========================================================================
//  1. Il formato
// ===========================================================================

static void TestFormat(const char* FixturePath)
{
	Section("1. Il formato .gvg: header, classi, indice");
	uint32_t X = 0, Y = 0;
	Tiles::TileForLonLat(13, Lon, Lat, X, Y);
	const FTileKey Key{ 13, X, Y };
	// Una tile meta' bosco (a ovest) e meta' campi, con una riga d'acqua in cima.
	std::vector<uint8_t> Classes(static_cast<size_t>(Tiles::VegetationRaster) * Tiles::VegetationRaster);
	for (int Row = 0; Row < Tiles::VegetationRaster; ++Row)
	{
		for (int Column = 0; Column < Tiles::VegetationRaster; ++Column)
		{
			Classes[static_cast<size_t>(Row) * Tiles::VegetationRaster + Column] = Row == 0 ? Cover(ELandCover::Water)
				: Column < 128 ? Cover(ELandCover::Trees) : Cover(ELandCover::Crops);
		}
	}
	const std::vector<uint8_t> Bytes = MakeTileBytes(Key, Classes, Tiles::VegetationTileTrees);
	Tiles::FVegetationTile Tile;
	std::string Error;
	const bool bLoaded = LoadTile(Bytes, Key, Tile, Error);
	const Tiles::FTileBounds Bounds = Tiles::GetTileBounds(13, X, Y);
	const double Span = Tiles::TileSpanDeg(13);
	const double Pixel = Span / Tiles::VegetationRaster;
	Check(bLoaded && Tile.IsValid()
		&& Tile.ClassAt(Bounds.North - 0.5 * Pixel, Bounds.West + 0.5 * Pixel) == Cover(ELandCover::Water)
		&& Tile.ClassAt(Bounds.CentreLat(), Bounds.West + 0.25 * Span) == Cover(ELandCover::Trees)
		&& Tile.ClassAt(Bounds.CentreLat(), Bounds.West + 0.75 * Span) == Cover(ELandCover::Crops)
		&& Tile.ClassAt(Bounds.North + 0.001, Bounds.CentreLon()) == Cover(ELandCover::NoData),
		"header, zlib, classi: righe dal nord, colonne dall'ovest; fuori dalla tile nessun dato", Error);

	Tiles::FVegetationTileHeader Header;
	std::string WrongKey, Short, Magic;
	FTileKey Other = Key;
	Other.X += 1;
	std::vector<uint8_t> Truncated(Bytes.begin(), Bytes.end() - 3);
	std::vector<uint8_t> Garbage = Bytes;
	Garbage[0] = 'X';
	const bool bWrongKey = !Tiles::ParseVegetationTileHeader(Bytes.data(), Bytes.size(), Other, Header, WrongKey);
	const bool bShort = !Tiles::ParseVegetationTileHeader(Truncated.data(), Truncated.size(), Key, Header, Short);
	const bool bMagic = !Tiles::ParseVegetationTileHeader(Garbage.data(), Garbage.size(), Key, Header, Magic);
	Check(bWrongKey && bShort && bMagic, "si rifiutano una chiave sbagliata, un file troncato, un file che non e' una tile",
		WrongKey + " / " + Short + " / " + Magic);

	std::vector<uint8_t> Index(16 + 16, 0);
	auto Put32 = [&Index](size_t At, uint32_t Value) { for (int Octet = 0; Octet < 4; ++Octet) { Index[At + Octet] = (Value >> (8 * Octet)) & 0xFF; } };
	Put32(0, Tiles::VegetationIndexMagic);
	Put32(8, 13);
	Put32(12, 1);
	Put32(16, X);
	Put32(20, Y);
	Index[24] = Tiles::VegetationTileTrees;
	Put32(28, static_cast<uint32_t>(Bytes.size()));
	std::vector<Tiles::FVegetationIndexEntry> Entries;
	std::string IndexError;
	const bool bIndex = Tiles::ParseVegetationIndex(Index.data(), Index.size(), 13, Entries, IndexError);
	Check(bIndex && Entries.size() == 1 && Entries[0].X == X && Entries[0].Y == Y && Entries[0].Flags == Tiles::VegetationTileTrees,
		"l'indice: le tile che esistono, con i loro flag", IndexError);

	// Un file scritto dalla pipeline vera (Python): lo stesso formato dalle due parti.
	std::ifstream File(FixturePath ? FixturePath : "", std::ios::binary);
	if (!FixturePath || !File)
	{
		Skip("una tile scritta dalla pipeline", "file di prova non trovato (passalo come argomento)");
		return;
	}
	const std::vector<uint8_t> Fixture((std::istreambuf_iterator<char>(File)), std::istreambuf_iterator<char>());
	Tiles::FVegetationTileHeader FixtureHeader;
	std::string FixtureError;
	// La chiave si legge dall'header stesso (il file di prova sta fuori dalla piramide).
	const FTileKey FixtureKey = Fixture.size() > 20
		? FTileKey{ Tiles::VegetationDetail::ReadU32(&Fixture[8]), Tiles::VegetationDetail::ReadU32(&Fixture[12]),
		            Tiles::VegetationDetail::ReadU32(&Fixture[16]) }
		: FTileKey{};
	const bool bHeader = Fixture.size() > 20
		&& Tiles::ParseVegetationTileHeader(Fixture.data(), Fixture.size(), FixtureKey, FixtureHeader, FixtureError);
	Tiles::FVegetationTile FromPython;
	const bool bFixture = bHeader && LoadTile(Fixture, FixtureHeader.Key, FromPython, FixtureError);
	// Il file di prova: bosco nella meta' ovest, prato nella meta' est, acqua nell'ultima riga.
	const Tiles::FTileBounds FixtureBounds = Tiles::GetTileBounds(FixtureHeader.Key.Level, FixtureHeader.Key.X, FixtureHeader.Key.Y);
	const double FixtureSpan = Tiles::TileSpanDeg(FixtureHeader.Key.Level);
	Check(bFixture && FromPython.ClassAt(FixtureBounds.CentreLat(), FixtureBounds.West + 0.25 * FixtureSpan) == Cover(ELandCover::Trees)
		&& FromPython.ClassAt(FixtureBounds.CentreLat(), FixtureBounds.West + 0.75 * FixtureSpan) == Cover(ELandCover::Grass)
		&& FromPython.ClassAt(FixtureBounds.South + 0.1 * FixtureSpan / Tiles::VegetationRaster, FixtureBounds.CentreLon()) == Cover(ELandCover::Water)
		&& (FromPython.Flags & Tiles::VegetationTileTrees) != 0,
		"una tile scritta dalla pipeline (Python) si legge uguale qui", FixtureError);
}

// ===========================================================================
//  2. La griglia
// ===========================================================================

static void TestGrid()
{
	Section("2. La griglia degli alberi: fissa nel mondo, senza cuciture");
	FPlacementSettings Settings;
	const double Half = 0.004;   // ~450 m
	std::set<uint64_t> Whole, Left, Right;
	int Count = 0;
	ForEachGridPoint(Lat - Half, Lon - Half, Lat + Half, Lon + Half, Settings,
		[&](int64_t, int64_t, double, double, uint64_t Hash) { Whole.insert(Hash); ++Count; });
	ForEachGridPoint(Lat - Half, Lon - Half, Lat + Half, Lon, Settings,
		[&](int64_t, int64_t, double, double, uint64_t Hash) { Left.insert(Hash); });
	ForEachGridPoint(Lat - Half, Lon, Lat + Half, Lon + Half, Settings,
		[&](int64_t, int64_t, double, double, uint64_t Hash) { Right.insert(Hash); });
	std::set<uint64_t> Union = Left;
	Union.insert(Right.begin(), Right.end());
	const double Area = (2.0 * Half * 111132.0) * (2.0 * Half * 111320.0 * std::cos(Lat * 3.14159265358979323846 / 180.0));
	const double Expected = Area / (Settings.SpacingM * Settings.SpacingM);
	Check(Union == Whole && Left.size() + Right.size() == Whole.size() && std::fabs(Count - Expected) < 0.02 * Expected,
		"due celle vicine hanno insieme esattamente i punti del rettangolo intero, nessuno due volte; uno ogni 8 x 8 m",
		Fmt("%.0f punti, attesi %.0f", Count, Expected));

	std::vector<double> First, Second;
	ForEachGridPoint(Lat, Lon, Lat + 0.001, Lon + 0.001, Settings,
		[&](int64_t, int64_t, double PointLat, double PointLon, uint64_t) { First.push_back(PointLat); First.push_back(PointLon); });
	ForEachGridPoint(Lat, Lon, Lat + 0.001, Lon + 0.001, Settings,
		[&](int64_t, int64_t, double PointLat, double PointLon, uint64_t) { Second.push_back(PointLat); Second.push_back(PointLon); });
	double Closest = 1.0e9;
	for (size_t A = 0; A + 1 < First.size(); A += 2)
	{
		for (size_t B = A + 2; B + 1 < First.size(); B += 2)
		{
			const double North = (First[A] - First[B]) * 111132.0;
			const double East = (First[A + 1] - First[B + 1]) * 111320.0 * std::cos(Lat * 3.14159265358979323846 / 180.0);
			Closest = std::min(Closest, std::sqrt(North * North + East * East));
		}
	}
	Check(First == Second && !First.empty() && Closest > 0.15 * Settings.SpacingM,
		"sempre gli stessi punti, mossi a caso dentro la loro casella: mai due alberi sullo stesso tronco",
		Fmt("i due piu' vicini a %.1f m", Closest));
}

// ===========================================================================
//  3-4. Chi cresce dove
// ===========================================================================

struct FPlot
{
	std::vector<FTree> Trees;
	int Count(ESpecies Species) const
	{
		return static_cast<int>(std::count_if(Trees.begin(), Trees.end(), [Species](const FTree& Tree) { return Tree.Species == Species; }));
	}
	double Share(ESpecies Species) const { return Trees.empty() ? 0.0 : static_cast<double>(Count(Species)) / Trees.size(); }
	/** La quota di una specie fra gli ALBERI (senza gli arbusti). */
	double TreeShare(ESpecies Species) const
	{
		const int Standing = static_cast<int>(Trees.size()) - Count(ESpecies::Shrub);
		return Standing > 0 ? static_cast<double>(Count(Species)) / Standing : 0.0;
	}
};

/** Un quadrato di ~900 m tutto di una classe, a una quota e pendenza, alla latitudine data. */
static FPlot Plant(ELandCover Class, double AltitudeM, double SlopeDeg = 5.0, double LatDeg = Lat, double Density = 1.0)
{
	FPlacementSettings Settings;
	Settings.Density = Density;
	FPlot Plot;
	PlaceTrees(LatDeg - 0.004, Lon - 0.006, LatDeg + 0.004, Lon + 0.006, Settings,
		[Class](double, double) { return Cover(Class); },
		[AltitudeM, SlopeDeg](double, double, double& Height, double& Slope) { Height = AltitudeM + 47.0; Slope = SlopeDeg; return true; },
		47.0, Plot.Trees);
	return Plot;
}

static void TestPlacement()
{
	Section("3. Chi cresce dove: la copertura del suolo, la quota, la pendenza");
	int Points = 0;
	FPlacementSettings Settings;
	ForEachGridPoint(Lat - 0.004, Lon - 0.006, Lat + 0.004, Lon + 0.006, Settings, [&](int64_t, int64_t, double, double, uint64_t) { ++Points; });

	const FPlot Forest = Plant(ELandCover::Trees, 500.0);
	const double Trees = static_cast<double>(Forest.Trees.size() - Forest.Count(ESpecies::Shrub)) / Points;
	const FPlot Meadow = Plant(ELandCover::Grass, 500.0);
	const FPlot Water = Plant(ELandCover::Water, 500.0);
	const FPlot Town = Plant(ELandCover::Built, 500.0);
	Check(std::fabs(Trees - 0.85) < 0.02 && Meadow.Trees.size() < Forest.Trees.size() / 10 && !Meadow.Trees.empty()
		&& Water.Trees.empty() && !Town.Trees.empty() && Town.Trees.size() < Forest.Trees.size() / 15,
		"il bosco ha un albero sull'85% dei punti; il prato qualche arbusto, la citta' qualche albero, l'acqua niente",
		Fmt("bosco %.0f%%, prato %.0f, citta' %.0f, acqua %.0f", 100.0 * Trees, Meadow.Trees.size(), Town.Trees.size(), Water.Trees.size()));   /* percentuale, non-unita */

	const FPlot Hills = Plant(ELandCover::Trees, 400.0);
	const FPlot Mountain = Plant(ELandCover::Trees, 1800.0);
	const FPlot Treeline = Plant(ELandCover::Trees, 2300.0);
	const FPlot Summit = Plant(ELandCover::Trees, 2900.0);
	Check(Hills.TreeShare(ESpecies::Conifer) < 0.15 && Hills.TreeShare(ESpecies::Broadleaf) > 0.85 && Mountain.TreeShare(ESpecies::Conifer) > 0.9
		&& Treeline.Share(ESpecies::Shrub) == 1.0 && !Treeline.Trees.empty() && Summit.Trees.empty(),
		"in collina latifoglie, in montagna conifere; sopra il limite del bosco solo arbusti, in cima niente",
		Fmt("conifere fra gli alberi a 400 m %.0f%%, a 1.800 m %.0f%%; arbusti a 2.300 m %.0f", 100.0 * Hills.TreeShare(ESpecies::Conifer),   /* percentuale, non-unita */
			100.0 * Mountain.TreeShare(ESpecies::Conifer), Treeline.Trees.size()));   /* percentuale, non-unita */

	const FPlot Rome = Plant(ELandCover::Trees, 80.0, 5.0, 41.9);
	const FPlot Calabria = Plant(ELandCover::Trees, 1500.0, 5.0, 39.0);
	const FPlot Alps = Plant(ELandCover::Trees, 1500.0, 5.0, 46.0);
	Check(Rome.TreeShare(ESpecies::UmbrellaPine) > 0.1 && Hills.Count(ESpecies::UmbrellaPine) == 0
		&& Calabria.TreeShare(ESpecies::Conifer) < Alps.TreeShare(ESpecies::Conifer) - 0.3,
		"pini domestici nelle pianure del centro-sud, non al nord; a sud le conifere cominciano piu' in alto",
		Fmt("pini a Roma %.0f%%; conifere a 1.500 m: Calabria %.0f%%, Alpi %.0f%%", 100.0 * Rome.TreeShare(ESpecies::UmbrellaPine),   /* percentuale, non-unita */
			100.0 * Calabria.TreeShare(ESpecies::Conifer), 100.0 * Alps.TreeShare(ESpecies::Conifer)));   /* percentuale, non-unita */

	const FPlot Cliff = Plant(ELandCover::Trees, 500.0, 45.0);
	const FPlot Wall = Plant(ELandCover::Trees, 500.0, 55.0);
	const FPlot Sparse = Plant(ELandCover::Trees, 500.0, 5.0, Lat, 0.5);
	Check(Cliff.Share(ESpecies::Shrub) == 1.0 && !Cliff.Trees.empty() && Wall.Trees.empty()
		&& std::fabs(static_cast<double>(Sparse.Trees.size()) / Forest.Trees.size() - 0.5) < 0.03,
		"oltre 40° solo arbusti, oltre 50° niente; la densita' a meta' dimezza gli alberi",
		Fmt("a 45°: %.0f arbusti; densita' 0,5: %.2f", Cliff.Trees.size(), static_cast<double>(Sparse.Trees.size()) / Forest.Trees.size()));

	Section("4. Quanto sono grandi, di che verde, sempre uguali");
	bool bSizes = true, bColours = true;
	double ThinMean = 0.0;
	for (const FTree& Tree : Hills.Trees)
	{
		const double Ratio = Tree.WidthM / Tree.HeightM;
		switch (Tree.Species)
		{
		case ESpecies::Broadleaf: bSizes = bSizes && Tree.HeightM >= 9.0f && Tree.HeightM <= 21.0f && Ratio >= 0.59 && Ratio <= 0.86; break;
		case ESpecies::Conifer: bSizes = bSizes && Tree.HeightM >= 14.0f && Tree.HeightM <= 30.0f && Ratio >= 0.29 && Ratio <= 0.43; break;
		case ESpecies::Shrub: bSizes = bSizes && Tree.HeightM >= 1.0f && Tree.HeightM <= 3.2f; break;
		default: break;
		}
		bColours = bColours && Tree.Colour[1] > Tree.Colour[0] && Tree.Colour[1] > Tree.Colour[2] && Tree.Colour[1] < 0.2f;
		ThinMean += Tree.Thin;
	}
	ThinMean /= std::max<size_t>(1, Hills.Trees.size());
	double LowHeight = 0.0, HighHeight = 0.0;
	const FPlot NearLine = Plant(ELandCover::Trees, 2050.0);
	for (const FTree& Tree : Mountain.Trees) { LowHeight += Tree.HeightM / Mountain.Trees.size(); }
	for (const FTree& Tree : NearLine.Trees) { HighHeight += Tree.HeightM / NearLine.Trees.size(); }
	const FPlot Again = Plant(ELandCover::Trees, 400.0);
	bool bSame = Again.Trees.size() == Hills.Trees.size();
	for (size_t Index = 0; bSame && Index < Again.Trees.size(); ++Index)
	{
		bSame = Again.Trees[Index].Id == Hills.Trees[Index].Id && Again.Trees[Index].HeightM == Hills.Trees[Index].HeightM
			&& Again.Trees[Index].Colour[1] == Hills.Trees[Index].Colour[1];
	}
	Check(bSizes && bColours && std::fabs(ThinMean - 0.5) < 0.03 && HighHeight < 0.75 * LowHeight && bSame,
		"altezze e chiome per specie; sempre verdi; vicino al limite del bosco piu' bassi; due volte lo stesso bosco",
		Fmt("alti in media %.1f m a 1.800 m, %.1f m a 2.050 m", LowHeight, HighHeight));

	// Le fasce di distanza: un albero tenuto da lontano c'e' anche da vicino.
	const float Mid = BandFraction(1), Far = BandFraction(2);
	int KeptFar = 0, KeptMid = 0, Lost = 0;
	for (const FTree& Tree : Hills.Trees)
	{
		const bool bFar = Tree.Thin < Far, bMid = Tree.Thin < Mid;
		KeptFar += bFar ? 1 : 0;
		KeptMid += bMid ? 1 : 0;
		Lost += (bFar && !bMid) ? 1 : 0;
	}
	Check(BandFraction(0) == 1.0f && Lost == 0 && std::fabs(static_cast<double>(KeptMid) / Hills.Trees.size() - 0.5) < 0.03
		&& std::fabs(static_cast<double>(KeptFar) / Hills.Trees.size() - 0.25) < 0.03,
		"da lontano meta' e poi un quarto degli alberi, sempre gli stessi: avvicinandosi se ne aggiungono, nessuno sparisce",
		Fmt("tenuti %.0f%% e %.0f%%", 100.0 * KeptMid / Hills.Trees.size(), 100.0 * KeptFar / Hills.Trees.size()));   /* percentuale, non-unita */

	// Il frame della cella: il centro e' l'origine, 100 m a nord sono x = 100.
	const FCellFrame Frame = FCellFrame::At(Lat, Lon);
	double Origin[3], North[3], Up[3];
	Frame.Local(Lat, Lon, 0.0, Origin);
	Frame.Local(Lat + 100.0 / 111132.0, Lon, 0.0, North);
	Frame.Local(Lat, Lon, 250.0, Up);
	Check(std::fabs(Origin[0]) < 1e-6 && std::fabs(Origin[2]) < 1e-6 && std::fabs(North[0] - 100.0) < 0.5 && std::fabs(North[1]) < 0.01
		&& North[2] < 0.0 && North[2] > -0.01 && std::fabs(Up[2] - 250.0) < 1e-6,
		"il frame della cella: nord, est, alto in metri; 100 m a nord la Terra scende di un millimetro",
		Fmt("100 m a nord: (%.2f, %.3f, %.4f)", North[0], North[1], North[2]));
}

// ===========================================================================
//  5. Le forme
// ===========================================================================

static void TestMeshes()
{
	Section("5. Le forme degli alberi: dentro il cubo unitario, girate verso fuori, leggere");
	bool bBox = true, bIndices = true, bNormals = true, bFacing = true, bFoliage = true, bBudget = true;
	std::string Counts;
	for (int32_t Species = 0; Species < SpeciesCount; ++Species)
	{
		for (const ETreeDetail Detail : { ETreeDetail::Near, ETreeDetail::Far })
		{
			for (uint32_t Variant = 0; Variant < static_cast<uint32_t>(VariantCount); ++Variant)
			{
				FTreeMesh Mesh;
				BuildTreeMesh(static_cast<ESpecies>(Species), Detail, Variant, Mesh);
				const size_t Vertices = Mesh.VertexCount();
				for (size_t Index = 0; Index < Vertices; ++Index)
				{
					const float* P = &Mesh.Positions[Index * 3];
					const float* N = &Mesh.Normals[Index * 3];
					bBox = bBox && P[0] >= -0.5f && P[0] <= 0.5f && P[1] >= -0.5f && P[1] <= 0.5f && P[2] >= 0.0f && P[2] <= 1.0f;
					bNormals = bNormals && std::fabs(std::sqrt(N[0] * N[0] + N[1] * N[1] + N[2] * N[2]) - 1.0f) < 1e-4f;
				}
				// La normale geometrica di ogni triangolo sta dalla parte delle normali dei suoi vertici.
				int Agree = 0;
				for (size_t Triangle = 0; Triangle < Mesh.TriangleCount(); ++Triangle)
				{
					const uint32_t* I = &Mesh.Indices[Triangle * 3];
					bIndices = bIndices && I[0] < Vertices && I[1] < Vertices && I[2] < Vertices;
					if (!bIndices) { break; }
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
				bFacing = bFacing && Agree >= static_cast<int>(0.97 * Mesh.TriangleCount());
				// La chioma e' segnata come chioma, e in alto ce n'e' sempre.
				bool bCrown = false;
				for (size_t Index = 0; Index < Vertices; ++Index)
				{
					bCrown = bCrown || (Mesh.Colours[Index * 4] == 255 && Mesh.Positions[Index * 3 + 2] > 0.7f);
				}
				bFoliage = bFoliage && bCrown;
				const size_t Budget = Detail == ETreeDetail::Near ? 400 : 40;
				bBudget = bBudget && Mesh.TriangleCount() <= Budget && Mesh.TriangleCount() >= 16;
				if (Variant == 0)
				{
					Counts += (Detail == ETreeDetail::Near ? std::string(Counts.empty() ? "" : ", ") + SpeciesName(static_cast<ESpecies>(Species)) + " " : std::string("/"))
						+ std::to_string(Mesh.TriangleCount());
				}
			}
		}
	}
	Check(bBox && bIndices && bNormals, "ogni forma sta nel cubo unitario (alta 1, larga 1), indici validi, normali unitarie");
	Check(bFacing, "i triangoli girano come il terreno: la normale geometrica (B-A)x(C-A) punta fuori, con le normali dei vertici");
	Check(bFoliage && bBudget, "la chioma e' segnata nel colore dei vertici; vicino fino a 400 triangoli, lontano fino a 40",
		"triangoli vicino/lontano: " + Counts);

	FTreeMesh A, B, C;
	BuildTreeMesh(ESpecies::Broadleaf, ETreeDetail::Near, 0, A);
	BuildTreeMesh(ESpecies::Broadleaf, ETreeDetail::Near, 0, B);
	BuildTreeMesh(ESpecies::Broadleaf, ETreeDetail::Near, 1, C);
	// Il tronco: corteccia (R = 0) solo in basso, sotto la chioma.
	bool bTrunkLow = true;
	for (size_t Index = 0; Index < A.VertexCount(); ++Index)
	{
		if (A.Colours[Index * 4] == 0) { bTrunkLow = bTrunkLow && A.Positions[Index * 3 + 2] <= 0.5f; }
	}
	Check(A.Positions == B.Positions && A.Positions != C.Positions && bTrunkLow,
		"stessa variante, stessa forma; l'altra variante e' diversa; il tronco sta sotto la chioma");
}

// ===========================================================================
//  6. Il materiale, compilato come C++
// ===========================================================================

static HlslShim::float4 RunColour(HlslShim::float3 Leaf, HlslShim::float4 Paint, HlslShim::float3 Normal, HlslShim::float3 Pos,
                                  HlslShim::float4 Ground, HlslShim::float4 Frame, float UuPerMetre)
{
	using namespace HlslShim;
#define GEO_VEGETATION_COLOUR(...) __VA_ARGS__
#define GEO_VEGETATION_WIND(...)
#include "Vegetation/VegetationShader.inl"
#undef GEO_VEGETATION_COLOUR
#undef GEO_VEGETATION_WIND
}

static HlslShim::float3 RunWind(HlslShim::float4 Paint, HlslShim::float4 Wind, float Size, float Phase, float UuPerMetre)
{
	using namespace HlslShim;
#define GEO_VEGETATION_COLOUR(...)
#define GEO_VEGETATION_WIND(...) __VA_ARGS__
#include "Vegetation/VegetationShader.inl"
#undef GEO_VEGETATION_COLOUR
#undef GEO_VEGETATION_WIND
}

static void TestShader()
{
	Section("6. Il materiale (lo stesso testo del materiale, compilato come C++)");
	using HlslShim::float3;
	using HlslShim::float4;
	const float3 Leaf(0.07, 0.12, 0.04);
	const float4 NoSnow(0.0, 1.0e5, 150.0, 0.0), Frame(47.0, 6371000.0, 0.0, 0.0);
	const float4 Crown = RunColour(Leaf, float4(1.0, 1.0, 0.8, 1.0), float3(0.0, 0.0, 1.0), float3(0.0, 0.0, 80000.0), NoSnow, Frame, 100.0f);
	const float4 Inner = RunColour(Leaf, float4(1.0, 0.4, 0.5, 1.0), float3(0.0, 0.0, 1.0), float3(0.0, 0.0, 80000.0), NoSnow, Frame, 100.0f);
	const float4 Bark = RunColour(Leaf, float4(0.0, 1.0, 0.2, 1.0), float3(1.0, 0.0, 0.0), float3(0.0, 0.0, 80000.0), NoSnow, Frame, 100.0f);
	Check(std::fabs(Crown.y - 0.12f) < 1e-5f && Inner.y < Crown.y && Bark.x > Bark.y && Bark.y > Bark.z && Bark.w > Crown.w,
		"la chioma ha il verde dell'albero, piu' scuro dentro; il tronco e' marrone e piu' ruvido",
		Fmt("chioma %.3f, dentro %.3f; tronco rosso %.3f, verde %.3f", Crown.y, Inner.y, Bark.x, Bark.y));

	const float4 Snowy(0.0, 1000.0, 150.0, 1.0);
	const float4 SnowTop = RunColour(Leaf, float4(1.0, 1.0, 0.9, 1.0), float3(0.0, 0.0, 1.0), float3(0.0, 0.0, 120000.0), Snowy, Frame, 100.0f);
	const float4 SnowSide = RunColour(Leaf, float4(1.0, 1.0, 0.5, 1.0), float3(1.0, 0.0, 0.0), float3(0.0, 0.0, 120000.0), Snowy, Frame, 100.0f);
	const float4 SnowLow = RunColour(Leaf, float4(1.0, 1.0, 0.9, 1.0), float3(0.0, 0.0, 1.0), float3(0.0, 0.0, 70000.0), Snowy, Frame, 100.0f);
	const float4 SnowBark = RunColour(Leaf, float4(0.0, 1.0, 0.2, 1.0), float3(0.0, 0.0, 1.0), float3(0.0, 0.0, 120000.0), Snowy, Frame, 100.0f);
	Check(SnowTop.x > 0.7f && SnowSide.x < 0.1f && SnowLow.y == Crown.y && SnowBark.x < 0.2f,
		"la neve: bianca sulla chioma che guarda il cielo sopra la quota della copertura; non sui fianchi, non sotto, non sui tronchi",
		Fmt("in cima %.2f, di fianco %.2f, tronco %.2f", SnowTop.x, SnowSide.x, SnowBark.x));

	const float4 Calm(0.0, 0.0, 0.0, 10.0), Breeze(5.0, 0.0, 0.0, 10.0), Gale(10.0, 0.0, 0.0, 10.0), Westerly(0.0, 10.0, 0.0, 10.0);
	const float3 Still = RunWind(float4(1.0, 1.0, 1.0, 1.0), Calm, 20.0f, 0.3f, 100.0f);
	const float3 Base = RunWind(float4(0.0, 1.0, 0.0, 1.0), Gale, 20.0f, 0.3f, 100.0f);
	const float3 TipBreeze = RunWind(float4(1.0, 1.0, 1.0, 1.0), Breeze, 20.0f, 0.3f, 100.0f);
	const float3 TipGale = RunWind(float4(1.0, 1.0, 1.0, 1.0), Gale, 20.0f, 0.3f, 100.0f);
	const float3 Middle = RunWind(float4(1.0, 1.0, 0.5, 1.0), Gale, 20.0f, 0.3f, 100.0f);
	const float3 East = RunWind(float4(1.0, 1.0, 1.0, 1.0), Westerly, 20.0f, 0.3f, 100.0f);
	double Lowest = 1.0e9, Highest = -1.0e9;
	for (int Step = 0; Step < 200; ++Step)
	{
		const float3 Now = RunWind(float4(1.0, 1.0, 1.0, 1.0), float4(10.0, 0.0, 0.0, 0.05 * Step), 20.0f, 0.3f, 100.0f);
		Lowest = std::min(Lowest, static_cast<double>(Now.x));
		Highest = std::max(Highest, static_cast<double>(Now.x));
	}
	Check(Still.x == 0.0f && Still.y == 0.0f && Base.x == 0.0f && TipGale.x > 3.5f * TipBreeze.x && std::fabs(Middle.x - 0.25f * TipGale.x) < 1e-3f
		&& East.y > 0.0f && std::fabs(East.x) < 1e-4f && Highest > 1.5 * Lowest && Lowest > 0.0 && Highest / 100.0 < 2.0,
		"il vento: niente con la calma, la base non si muove; il doppio del vento piega quattro volte; sottovento, e oscilla",
		Fmt("la cima di un albero di 20 m: %.2f m con 5 m/s, fra %.2f e %.2f m con 10 m/s", TipBreeze.x / 100.0, Lowest / 100.0, Highest / 100.0));
}

// ===========================================================================
//  7. Le celle attorno alla camera
// ===========================================================================

/** Una tile di quote: Height(lon, lat) su ogni post. */
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
			Tile->Heights[static_cast<size_t>(J) * Tiles::TilePosts + I] =
				static_cast<float>(Height(Bounds.West + I * Spacing, Bounds.North - J * Spacing));
		}
	}
	return Tile;
}

static FTileKey KeyAt(uint32_t Level, double LonDeg, double LatDeg)
{
	uint32_t X = 0, Y = 0;
	Tiles::TileForLonLat(Level, LonDeg, LatDeg, X, Y);
	return FTileKey{ Level, X, Y };
}

static void TestCells()
{
	Section("7. Le celle attorno alla camera: quali, a che banda, su che terreno");

	// --- Quali celle -------------------------------------------------------------
	std::vector<FCellRef> Cells;
	CellsAround(Lat, Lon, 2.0, 2500.0, CellLevel, Cells);
	const FTileKey Under = KeyAt(CellLevel, Lon, Lat);
	bool bSorted = true, bInside = true;
	for (size_t Index = 0; Index < Cells.size(); ++Index)
	{
		if (Index > 0 && Cells[Index].DistanceM < Cells[Index - 1].DistanceM) { bSorted = false; }
		if (Cells[Index].DistanceM > 2500.0) { bInside = false; }
	}
	// Il cerchio di 2,5 km tocca ~ (pi * 2,5^2 + bordo) / 0,26 km^2 celle.
	Check(!Cells.empty() && Cells[0].Key == Under && Cells[0].DistanceM < 3.0 && bSorted && bInside
		&& Cells.size() > 80 && Cells.size() < 140,
		"le celle entro 2,5 km: dalla piu' vicina (quella sotto la camera), tutte dentro il raggio",
		Fmt("%.0f celle del livello 15", static_cast<double>(Cells.size())));

	std::vector<FCellRef> High, Higher;
	CellsAround(Lat, Lon, 1500.0, 2500.0, CellLevel, High);
	CellsAround(Lat, Lon, 3000.0, 2500.0, CellLevel, Higher);
	Check(!High.empty() && High.size() < Cells.size() && High[0].DistanceM >= 1500.0 && Higher.empty(),
		"da 1.500 m di quota meno celle (la quota conta nella distanza); da 3.000 m nessuna",
		Fmt("%.0f celle a 1.500 m", static_cast<double>(High.size())));

	// --- Le bande, e l'isteresi --------------------------------------------------
	FBandSettings Bands;
	Check(BandFor(600.0, Bands, -1) == 0 && BandFor(800.0, Bands, -1) == 1 && BandFor(1600.0, Bands, -1) == 2
		&& BandFor(2600.0, Bands, -1) == BandOutside,
		"una cella nuova: vicino fino a 700 m, medio fino a 1.500, lontano fino a 2.500, poi fuori");
	Check(BandFor(740.0, Bands, 0) == 0 && BandFor(790.0, Bands, 0) == 1 && BandFor(660.0, Bands, 1) == 1
		&& BandFor(620.0, Bands, 1) == 0 && BandFor(2450.0, Bands, BandOutside) == BandOutside
		&& BandFor(2400.0, Bands, BandOutside) == 2 && BandFor(2560.0, Bands, 2) == 2,
		"l'isteresi: si cambia banda solo 75 m oltre il confine, in tutte e due le direzioni");
	int Changes = 0;
	int32_t Band = BandFor(700.0, Bands, -1);
	for (int Step = 0; Step < 200; ++Step)
	{
		const int32_t Next = BandFor(700.0 + 40.0 * std::sin(Step * 0.3), Bands, Band);
		if (Next != Band) { ++Changes; }
		Band = Next;
	}
	Check(Changes == 0, "una camera che va avanti e indietro di 40 m sul confine non fa cambiare banda alla cella",
		Fmt("%.0f cambi", static_cast<double>(Changes)));
	FBandSettings Wide;
	Wide.SetFarM(4000.0);
	Check(std::fabs(Wide.NearM - 1120.0) < 1e-6 && std::fabs(Wide.MidM - 2400.0) < 1e-6 && Wide.FarM == 4000.0,
		"geo.Vegetation.Radius allarga le tre bande insieme");

	// --- La firma del terreno ------------------------------------------------------
	const FTileKey A{ 14, 17082, 4089 }, B{ 14, 17083, 4089 }, C{ 15, 34164, 8178 };
	const uint64_t Signature = SurfaceSignature({ A, B }, 2);
	Check(Signature != 0 && Signature == SurfaceSignature({ B, A }, 2) && Signature != SurfaceSignature({ A, C }, 2)
		&& Signature != SurfaceSignature({ A, B }, 1) && SurfaceSignature({}, 2) == 0,
		"la firma del terreno: non dipende dall'ordine, cambia se cambia una tile o il passo; senza terreno e' 0");

	// --- Il terreno disegnato: quota, pendenza, la tile piu' fine ---------------------
	const FTileKey Parent = KeyAt(13, Lon, Lat);
	const Tiles::FTileBounds ParentBounds = Tiles::GetTileBounds(Parent.Level, Parent.X, Parent.Y);
	const double MetresEast = 111320.0 * std::cos(Lat * 3.14159265358979323846 / 180.0);
	// Una rampa verso est: mezzo metro ogni metro, cioe' 26,6 gradi.
	auto Ramp = [&](double LonDeg, double) { return 300.0 + 0.5 * (LonDeg - ParentBounds.West) * MetresEast; };
	FGroundSurfaces Ground;
	Ground.Add(MakeHeights(Parent, Ramp), 2);
	double Height = 0.0, Slope = 0.0;
	const bool bOn = Ground.GroundAt(Lat, Lon, Height, Slope);
	double Outside = 0.0, OutsideSlope = 0.0;
	const bool bOff = Ground.GroundAt(ParentBounds.North + 0.01, Lon, Outside, OutsideSlope);
	Check(bOn && std::fabs(Height - Ramp(Lon, Lat)) < 0.5 && std::fabs(Slope - 26.57) < 0.3 && !bOff,
		"il terreno disegnato: la quota della rampa, la sua pendenza; fuori dalle tile niente",
		Fmt("quota %.2f m (attesa %.2f), pendenza %.2f gradi", Height, Ramp(Lon, Lat), Slope));

	const FTileKey Child = KeyAt(14, Lon, Lat);
	const Tiles::FTileBounds ChildBounds = Tiles::GetTileBounds(Child.Level, Child.X, Child.Y);
	Ground.Add(MakeHeights(Child, [](double, double) { return 800.0; }), 2);
	double InChild = 0.0, InParent = 0.0, Ignored = 0.0;
	Ground.GroundAt(ChildBounds.CentreLat(), ChildBounds.CentreLon(), InChild, Ignored);
	const double ParentOnlyLat = (ChildBounds.South > ParentBounds.CentreLat()) ? ParentBounds.South + 0.001 : ParentBounds.North - 0.001;
	Ground.GroundAt(ParentOnlyLat, ChildBounds.CentreLon(), InParent, Ignored);
	Check(std::fabs(InChild - 800.0) < 1e-3 && std::fabs(InParent - 800.0) > 100.0,
		"dove ci sono due tile vale la piu' fine (e' quella disegnata); fuori dalla figlia, il padre",
		Fmt("nella figlia %.0f m, fuori %.0f m", InChild, InParent));

	// --- Una cella intera: il lavoro del worker ----------------------------------------
	const FTileKey CellKey = KeyAt(CellLevel, Lon, Lat);
	const Tiles::FTileBounds Cell = Tiles::GetTileBounds(CellKey.Level, CellKey.X, CellKey.Y);
	Tiles::FVegetationTile Woods;
	Woods.Key = KeyAt(13, Lon, Lat);
	Woods.Classes.assign(static_cast<size_t>(Tiles::VegetationRaster) * Tiles::VegetationRaster, Cover(ELandCover::Trees));
	FGroundSurfaces Flat;
	Flat.Add(MakeHeights(Parent, [](double, double) { return 345.0; }), 2);
	FPlacementSettings Settings;
	std::vector<FTreeInstance> Instances;
	BuildCellInstances(Cell, Settings, Woods, Flat, 47.0, Instances);

	const double CellArea = (Cell.North - Cell.South) * 111132.0 * (Cell.East - Cell.West) * MetresEast;
	const double Expected = CellArea / (Settings.SpacingM * Settings.SpacingM) * 0.93;
	const double HalfNorth = 0.5 * (Cell.North - Cell.South) * 111132.0 + 1.0;
	const double HalfEast = 0.5 * (Cell.East - Cell.West) * MetresEast + 1.0;
	bool bInCell = true, bOnGround = true, bShapes = true;
	int Kept = 0;
	std::set<int32_t> Shapes;
	for (const FTreeInstance& Tree : Instances)
	{
		if (std::fabs(Tree.Local[0]) > HalfNorth || std::fabs(Tree.Local[1]) > HalfEast) { bInCell = false; }
		// Il frame e' tangente al centro: a 400 m dal centro il suolo scende di ~1 cm.
		if (std::fabs(Tree.Local[2] - 345.0) > 0.1) { bOnGround = false; }
		const int32_t Shape = ShapeIndex(Tree.Species, Tree.Variant);
		if (Shape < 0 || Shape >= ShapeCount) { bShapes = false; }
		Shapes.insert(Shape);
		if (Tree.Thin < BandFraction(1)) { ++Kept; }
	}
	const double KeptShare = Instances.empty() ? 0.0 : static_cast<double>(Kept) / Instances.size();
	Check(std::fabs(Instances.size() / Expected - 1.0) < 0.05 && bInCell && bOnGround && bShapes && Shapes.size() >= 3,
		"un bosco in pianura: un albero (o un arbusto) sul 93% dei punti, tutti nella cella, tutti appoggiati sul suolo",   /* percentuale, non-unita */
		Fmt("%.0f alberi su %.0f attesi, %.0f forme", static_cast<double>(Instances.size()), Expected, static_cast<double>(Shapes.size())));
	Check(std::fabs(KeptShare - 0.5) < 0.03,
		"nella banda media resta la meta' degli alberi, scelti dal loro numero Thin",
		Fmt("%.1f%%", 100.0 * KeptShare));   /* percentuale, non-unita */

	std::vector<FTreeInstance> Floating;
	BuildCellInstances(Cell, Settings, Woods, FGroundSurfaces(), 47.0, Floating);
	Check(Floating.empty(), "senza terreno sotto nessun albero: la cella aspetta che il terreno ci sia");

	float Data[InstanceDataFloats];
	FTreeInstance Sample;
	Sample.Colour[0] = 0.1f; Sample.Colour[1] = 0.2f; Sample.Colour[2] = 0.3f; Sample.HeightM = 17.0f; Sample.Random = 0.25f;
	InstanceData(Sample, Data);
	Check(Data[0] == 0.1f && Data[1] == 0.2f && Data[2] == 0.3f && Data[3] == 17.0f && Data[4] == 0.25f,
		"i dati per istanza del materiale: verde (0-2), altezza (3), caso (4), come li legge M_GeoVegetation");
}

int main(int ArgumentCount, char** Arguments)
{
	std::printf("=====================================================\n");
	std::printf(" GeoVegetation -- dove crescono gli alberi, e come\n");
	std::printf("=====================================================\n");
	TestFormat(ArgumentCount > 1 ? Arguments[1] : nullptr);
	TestGrid();
	TestPlacement();
	TestMeshes();
	TestShader();
	TestCells();
	std::printf("\n=====================================================\n");
	std::printf(" RISULTATO: %d passati, %d falliti, %d saltati\n", GPassed, GFailed, GSkipped);
	std::printf("=====================================================\n");
	return GFailed == 0 ? 0 : 1;
}

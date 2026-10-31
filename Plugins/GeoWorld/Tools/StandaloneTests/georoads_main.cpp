// =============================================================================
//  Test delle strade (Fase 8), senza Unreal: formato .gvt e rasterizzatore.
//
//  Il formato si prova su un file SCRITTO DALLA PIPELINE PYTHON
//  (Source/GeoTiles/TestData/vector_fixture.gvt): e' l'unico modo di sapere
//  che i due lati leggono e scrivono la stessa cosa. Un test Python controlla
//  a sua volta che quel file coincida con quello che la pipeline rigenera.
//
//  Il rasterizzatore si prova MISURANDO: la larghezza di una strada si ricava
//  sommando la copertura dei pixel lungo una sezione, e deve tornare in metri
//  quella scritta nella tile, in orizzontale come in verticale.
// =============================================================================
#include "HlslShim.h"
#include "Roads/RoadMesh.h"
#include "Roads/RoadRasterizer.h"
#include "Roads/StreetLights.h"
#include "Tiles/ImageTileFormat.h"
#include "Tiles/VectorTileFormat.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace GeoWorld;
using namespace GeoWorld::Roads;
using Tiles::ERoadClass;
using Tiles::FTileKey;
using Tiles::FVectorTile;

static int GPassed = 0, GFailed = 0, GSkipped = 0;

static void Check(bool bCondition, const std::string& What, const std::string& Detail = {})
{
	(bCondition ? GPassed : GFailed)++;
	std::printf("  [%s] %s%s%s\n", bCondition ? " ok " : "FAIL", What.c_str(),
		Detail.empty() ? "" : "  -> ", Detail.c_str());
}

static void Section(const char* Title) { std::printf("\n== %s ==\n", Title); }

static std::string Fmt(const char* Format, double A, double B = 0.0, double C = 0.0)
{
	char Buffer[200];
	std::snprintf(Buffer, sizeof(Buffer), Format, A, B, C);
	return Buffer;
}

// --- Scrittura di una tile, per costruire i casi di prova ---------------------
//
// E' lo stesso layout di vectorformat.py. Si passa dal parser vero invece di
// riempire la struttura a mano: cosi' anche i rettangoli delle linee (calcolati
// alla lettura) sono quelli che vedra' il runtime.

struct FTestLine
{
	ERoadClass Class;
	uint8_t Flags;
	int8_t Layer;
	uint16_t WidthDm;
	std::vector<std::pair<int, int>> Points;
	/** Il byte dell'illuminazione (RoadLit*): 0 nelle tile di prima. */
	uint8_t Lit = 0;
};

static void PutU16(std::vector<uint8_t>& Out, uint32_t V) { Out.push_back(V & 0xFF); Out.push_back((V >> 8) & 0xFF); }
static void PutU32(std::vector<uint8_t>& Out, uint32_t V) { for (int I = 0; I < 4; ++I) { Out.push_back((V >> (8 * I)) & 0xFF); } }

static std::vector<uint8_t> EncodeTile(const FTileKey& Key, const std::vector<FTestLine>& Lines)
{
	uint32_t Points = 0;
	for (const FTestLine& Line : Lines) { Points += static_cast<uint32_t>(Line.Points.size()); }

	std::vector<uint8_t> Out;
	PutU32(Out, Tiles::VectorTileMagic);
	PutU16(Out, 1); PutU16(Out, 0);
	PutU32(Out, Key.Level); PutU32(Out, Key.X); PutU32(Out, Key.Y);
	PutU16(Out, Tiles::VectorTileExtent); PutU16(Out, Tiles::VectorTileExtent / 8);
	PutU32(Out, static_cast<uint32_t>(Lines.size())); PutU32(Out, Points);
	for (const FTestLine& Line : Lines)
	{
		Out.push_back(static_cast<uint8_t>(Line.Class));
		Out.push_back(Line.Flags);
		Out.push_back(static_cast<uint8_t>(Line.Layer));
		Out.push_back(Line.Lit);
		PutU16(Out, Line.WidthDm); PutU16(Out, 0);
		PutU32(Out, static_cast<uint32_t>(Line.Points.size()));
	}
	for (const FTestLine& Line : Lines)
	{
		for (const auto& Point : Line.Points)
		{
			PutU16(Out, static_cast<uint16_t>(static_cast<int16_t>(Point.first)));
			PutU16(Out, static_cast<uint16_t>(static_cast<int16_t>(Point.second)));
		}
	}
	return Out;
}

static FVectorTile MakeTile(const FTileKey& Key, const std::vector<FTestLine>& Lines)
{
	const std::vector<uint8_t> Bytes = EncodeTile(Key, Lines);
	FVectorTile Tile;
	Tiles::ParseVectorTile(Bytes.data(), Bytes.size(), Tile);
	return Tile;
}

static float AlphaAt(const std::vector<uint8_t>& Pixels, int32_t Size, int32_t X, int32_t Y)
{
	return Pixels[(static_cast<size_t>(Y) * Size + X) * 4 + 3] / 255.0f;
}

// Torino: tile di livello 13 della fixture.
static const FTileKey TorinoKey{ 13, 8538, 2044 };
static const int32_t Half = Tiles::VectorTileExtent / 2;

// ===========================================================================
//  1. Il formato, sul file della pipeline
// ===========================================================================
static void TestFixture(const std::string& Path)
{
	Section("1. Tile scritta dalla pipeline Python (vector_fixture.gvt)");

	std::ifstream File(Path, std::ios::binary);
	if (!File)
	{
		++GSkipped;
		std::printf("  [skip] %s non trovato: lancia i test dalla radice del repository\n", Path.c_str());
		return;
	}
	const std::vector<uint8_t> Bytes((std::istreambuf_iterator<char>(File)), std::istreambuf_iterator<char>());

	FVectorTile Tile;
	const auto Result = Tiles::ParseVectorTile(Bytes.data(), Bytes.size(), Tile);
	Check(Result == Tiles::EVectorTileParseResult::Ok, "la tile si legge",
		Tiles::DescribeVectorParseResult(Result));
	if (Result != Tiles::EVectorTileParseResult::Ok) { return; }

	Check(Tile.Key == TorinoKey, "chiave 13/8538/2044 (sopra Torino)");
	Check(Tile.Extent == 16384 && Tile.Buffer == 2048, "extent 16384, buffer 2048");
	Check(Tile.Features.size() == 3 && Tile.Points.size() == 7, "3 linee, 7 punti");

	const auto& Motorway = Tile.Features[0];
	Check(Motorway.Class == ERoadClass::Motorway && Motorway.WidthDecimetres == 110,
		"linea 0: autostrada larga 11,0 m");
	Check(Motorway.MinX == 0 && Motorway.MaxX == 16384 && Motorway.MinY == Half && Motorway.MaxY == Half,
		"linea 0: da bordo a bordo, a meta' tile (rettangolo calcolato alla lettura)");

	const auto& Rail = Tile.Features[1];
	Check(Rail.Class == ERoadClass::Rail && Rail.IsBridge() && Rail.Layer == 1,
		"linea 1: ferrovia su ponte, layer 1");
	Check(Rail.MinY == -1000 && Rail.MaxY == 16384 + 1000,
		"linea 1: esce dalla tile nel buffer (coordinate negative lette con il segno)");

	const auto& Path3 = Tile.Features[2];
	Check(Path3.Class == ERoadClass::Path && Path3.IsUnpaved() && Path3.PointCount == 3
		&& Path3.FirstPoint == 4, "linea 2: sentiero sterrato di 3 punti, dal punto 4");
	Check(Tile.Points[6].X == 6000 && Tile.Points[6].Y == 6000, "ultimo punto (6000, 6000)");
	Check(std::string(Tiles::RoadClassName(Rail.Class)) == "rail", "il nome della classe e' quello della pipeline");
}

// ===========================================================================
//  2. File rovinati
// ===========================================================================
static void TestBrokenFiles()
{
	Section("2. File rovinati: errori chiari, niente letture fuori dal buffer");

	std::vector<uint8_t> Good = EncodeTile(TorinoKey, { { ERoadClass::Primary, 0, 0, 75, { {0, 0}, {100, 100} } } });
	FVectorTile Tile;

	Check(Tiles::ParseVectorTile(Good.data(), Good.size(), Tile) == Tiles::EVectorTileParseResult::Ok,
		"la tile costruita dal test si legge");

	std::vector<uint8_t> Short(Good.begin(), Good.end() - 1);
	Check(Tiles::ParseVectorTile(Short.data(), Short.size(), Tile) == Tiles::EVectorTileParseResult::WrongSize,
		"un byte in meno: dimensione incoerente");

	std::vector<uint8_t> Magic = Good;
	Magic[2] = 'I';
	Check(Tiles::ParseVectorTile(Magic.data(), Magic.size(), Tile) == Tiles::EVectorTileParseResult::WrongMagic,
		"magic sbagliato (tile di immagine?)");

	std::vector<uint8_t> Lying = Good;
	Lying[32 + 8] = 9;     // la linea dichiara 9 punti, ce ne sono 2
	Check(Tiles::ParseVectorTile(Lying.data(), Lying.size(), Tile) == Tiles::EVectorTileParseResult::InconsistentPoints,
		"una linea che dichiara piu' punti di quelli presenti");

	Check(Tiles::ParseVectorTile(Good.data(), 10, Tile) == Tiles::EVectorTileParseResult::TooShort,
		"piu' corto dell'header");

	// Indice: due voci.
	std::vector<uint8_t> Index;
	PutU32(Index, Tiles::VectorIndexMagic); PutU16(Index, 1); PutU16(Index, 0);
	PutU32(Index, 13); PutU32(Index, 2);
	PutU32(Index, 8538); PutU32(Index, 2044); PutU32(Index, 3);
	PutU32(Index, 8539); PutU32(Index, 2044); PutU32(Index, 7);
	std::vector<Tiles::FVectorIndexEntry> Entries;
	const char* Error = nullptr;
	Check(Tiles::ParseVectorIndex(Index.data(), Index.size(), 13, Entries, Error)
		&& Entries.size() == 2 && Entries[1].FeatureCount == 7, "indice di due voci");
	Check(!Tiles::ParseVectorIndex(Index.data(), Index.size(), 12, Entries, Error) && Error,
		"indice di un altro livello: rifiutato", Error ? Error : "");
}

// ===========================================================================
//  3. Quale tile vettoriale per una tile di terreno
// ===========================================================================
static void TestChoice()
{
	Section("3. Livello e finestra della tile vettoriale");

	const std::vector<uint32_t> Levels{ 10, 11, 12, 13 };
	Check(ChooseVectorLevel(9, Levels) == -1, "terreno al livello 9: nessuna tile vettoriale");
	Check(ChooseVectorLevel(10, Levels) == 10, "terreno al 10: la tile del 10");
	Check(ChooseVectorLevel(12, Levels) == 12, "terreno al 12: la tile del 12");
	Check(ChooseVectorLevel(16, Levels) == 13, "terreno al 16: la tile del 13, di cui si disegna un pezzo");

	const FTileKey Terrain{ 15, 8538 * 4 + 3, 2044 * 4 + 1 };
	const FVectorWindow Window = MakeVectorWindow(Terrain, 13, 16384);
	Check(Window.VectorKey == TorinoKey, "la tile 15 ha come antenata vettoriale la 13 di Torino");
	Check(Window.Span == 4096.0 && Window.U0 == 3 * 4096.0 && Window.V0 == 4096.0,
		"finestra: un quarto di lato, colonna 3, riga 1",
		Fmt("span %.0f u0 %.0f v0 %.0f", Window.Span, Window.U0, Window.V0));

	const FVectorWindow Same = MakeVectorWindow(TorinoKey, 13, 16384);
	Check(Same.U0 == 0.0 && Same.V0 == 0.0 && Same.Span == 16384.0, "stesso livello: la tile intera");

	const FRoadRasterRequest Request = MakeRasterRequest(TorinoKey, 13, 16384, 512);
	const double RatioXY = Request.MetresPerUnitX / Request.MetresPerUnitY;
	Check(std::fabs(RatioXY - std::cos(45.077 * 3.14159265358979 / 180.0) * 111320.0 / 111132.0) < 0.002,
		"a 45 gradi un'unita' in x e' lunga il 71% di una in y", Fmt("x/y = %.4f", RatioXY));
}

// ===========================================================================
//  4. Larghezze misurate
// ===========================================================================
static double ColumnCoverage(const std::vector<uint8_t>& Pixels, int32_t Size, int32_t Column)
{
	double Sum = 0.0;
	for (int32_t Row = 0; Row < Size; ++Row) { Sum += AlphaAt(Pixels, Size, Column, Row); }
	return Sum;
}

static double RowCoverage(const std::vector<uint8_t>& Pixels, int32_t Size, int32_t Row)
{
	double Sum = 0.0;
	for (int32_t Column = 0; Column < Size; ++Column) { Sum += AlphaAt(Pixels, Size, Column, Row); }
	return Sum;
}

static void TestWidths()
{
	Section("4. La larghezza di una strada, misurata in metri");

	// Stile di prova: opaco, cosi' la copertura e' l'alfa.
	FRoadStyle Style = MakeRoadStyle(ERoadStyle::Realistic);
	for (FRoadStyleEntry& Entry : Style.Classes) { if (Entry.Opacity > 0.0f) { Entry.Opacity = 1.0f; } }

	const int32_t Size = 512;
	const FRoadRasterRequest Request = MakeRasterRequest(TorinoKey, 13, 16384, Size);
	const double PixelX = Request.Window.Span / Size * Request.MetresPerUnitX;
	const double PixelY = Request.Window.Span / Size * Request.MetresPerUnitY;
	std::printf("  pixel sul terreno: %.2f m (est) x %.2f m (sud)\n", PixelX, PixelY);

	// Orizzontale, 10 m: la sezione verticale deve sommare 10 m / pixel in y.
	{
		const FVectorTile Tile = MakeTile(TorinoKey, { { ERoadClass::Primary, 0, 0, 100, { {-500, Half}, {16384 + 500, Half} } } });
		std::vector<uint8_t> Pixels;
		RasterizeRoads(Tile, Request, Style, Pixels);
		const double Metres = ColumnCoverage(Pixels, Size, Size / 3) * PixelY;
		Check(std::fabs(Metres - 10.0) < 0.15, "strada orizzontale da 10 m: sezione di 10 m",
			Fmt("%.3f m", Metres));
		Check(AlphaAt(Pixels, Size, Size / 3, Size / 2 - 1) > 0.99f || AlphaAt(Pixels, Size, Size / 3, Size / 2) > 0.99f,
			"al centro e' piena");
		Check(AlphaAt(Pixels, Size, Size / 3, Size / 2 - 10) == 0.0f, "a 10 pixel di distanza non c'e' niente");
	}

	// Verticale, 10 m: la sezione orizzontale deve dare ANCHE 10 m, pur con
	// pixel di larghezza diversa in x.
	{
		const FVectorTile Tile = MakeTile(TorinoKey, { { ERoadClass::Primary, 0, 0, 100, { {Half, -500}, {Half, 16384 + 500} } } });
		std::vector<uint8_t> Pixels;
		RasterizeRoads(Tile, Request, Style, Pixels);
		const double Metres = RowCoverage(Pixels, Size, Size / 3) * PixelX;
		Check(std::fabs(Metres - 10.0) < 0.15, "strada verticale da 10 m: 10 m anche in x (pixel non quadrati)",
			Fmt("%.3f m", Metres));
	}

	// In diagonale a 45 gradi SUL TERRENO (non in unita' locali): la sezione
	// perpendicolare deve valere 10 m. Lungo una colonna la strada e' piu'
	// larga di un fattore 1/cos(45).
	{
		const double Units = 6000.0;
		const int DeltaU = static_cast<int>(Units);
		const int DeltaV = static_cast<int>(Units * Request.MetresPerUnitX / Request.MetresPerUnitY);
		const FVectorTile Tile = MakeTile(TorinoKey, { { ERoadClass::Primary, 0, 0, 100,
			{ {Half - DeltaU, Half - DeltaV}, {Half + DeltaU, Half + DeltaV} } } });
		std::vector<uint8_t> Pixels;
		RasterizeRoads(Tile, Request, Style, Pixels);
		const double AlongColumn = ColumnCoverage(Pixels, Size, Size / 2) * PixelY;
		Check(std::fabs(AlongColumn * std::cos(3.14159265358979 / 4.0) - 10.0) < 0.3,
			"strada a 45 gradi: 10 m perpendicolari", Fmt("%.3f m", AlongColumn * std::cos(3.14159265358979 / 4.0)));
	}

	// Sottile: un quarto di pixel. Deve coprire un quarto di pixel, non mezzo.
	{
		const uint16_t QuarterPixelDm = static_cast<uint16_t>(std::lround(PixelY * 0.25 * 10.0));
		const FVectorTile Tile = MakeTile(TorinoKey, { { ERoadClass::Primary, 0, 0, QuarterPixelDm, { {-500, Half}, {16384 + 500, Half} } } });
		std::vector<uint8_t> Pixels;
		RasterizeRoads(Tile, Request, Style, Pixels);
		const double Pixels1 = ColumnCoverage(Pixels, Size, Size / 4);
		Check(std::fabs(Pixels1 - QuarterPixelDm * 0.1 / PixelY) < 0.03,
			"strada larga un quarto di pixel: copertura totale un quarto di pixel",
			Fmt("%.3f pixel", Pixels1));
	}

	// Lo stile "mappa" la allarga ad almeno un pixel e mezzo.
	{
		const FRoadStyle Map = MakeRoadStyle(ERoadStyle::Map);
		const FVectorTile Tile = MakeTile(TorinoKey, { { ERoadClass::Primary, 0, 0, 10, { {-500, Half}, {16384 + 500, Half} } } });
		std::vector<uint8_t> Pixels;
		RasterizeRoads(Tile, Request, Map, Pixels);
		const double Covered = ColumnCoverage(Pixels, Size, Size / 4);
		Check(Covered >= 1.45, "stile mappa: una strada di 1 m diventa larga 1,5 pixel", Fmt("%.2f pixel", Covered));
	}
}

// ===========================================================================
//  5. Composizione, giunzioni, sterrato
// ===========================================================================
static void TestComposition()
{
	Section("5. Composizione: ordine, giunzioni, premoltiplicato");

	const int32_t Size = 256;
	const FRoadRasterRequest Request = MakeRasterRequest(TorinoKey, 13, 16384, Size);
	const FRoadStyle Map = MakeRoadStyle(ERoadStyle::Map);

	// Due strade che si incrociano: al centro vince la seconda (sopra).
	{
		const FVectorTile Tile = MakeTile(TorinoKey, {
			{ ERoadClass::Residential, 0, 0, 200, { {0, Half}, {16384, Half} } },
			{ ERoadClass::Motorway, 0, 0, 200, { {Half, 0}, {Half, 16384} } } });
		std::vector<uint8_t> Pixels;
		RasterizeRoads(Tile, Request, Map, Pixels);
		const uint8_t* Centre = Pixels.data() + (static_cast<size_t>(Size / 2) * Size + Size / 2) * 4;
		// Autostrada (233,115,45): rosso forte, blu debole. Residenziale bianca.
		Check(Centre[2] > 200 && Centre[0] < 120, "all'incrocio il colore e' quello della linea disegnata dopo",
			Fmt("R %.0f G %.0f B %.0f", Centre[2], Centre[1], Centre[0]));
	}

	// Una spezzata con un angolo stretto: nel vertice due segmenti si
	// sovrappongono. Con la somma l'alfa supererebbe l'opacita'.
	{
		FRoadStyle Style = MakeRoadStyle(ERoadStyle::Realistic);
		const FVectorTile Tile = MakeTile(TorinoKey, { { ERoadClass::Residential, 0, 0, 300,
			{ {2000, 2000}, {Half, Half}, {2600, 2000} } } });
		std::vector<uint8_t> Pixels;
		RasterizeRoads(Tile, Request, Style, Pixels);
		uint8_t MaxAlpha = 0;
		for (size_t Pixel = 0; Pixel < Pixels.size() / 4; ++Pixel) { MaxAlpha = std::max(MaxAlpha, Pixels[Pixel * 4 + 3]); }
		const float Expected = Style.Classes[static_cast<int>(ERoadClass::Residential)].Opacity * 255.0f;
		Check(std::fabs(MaxAlpha - Expected) <= 1.0f, "nel vertice l'alfa non supera l'opacita' (massimo, non somma)",
			Fmt("max %.0f, opacita' %.0f", MaxAlpha, Expected));

		// Premoltiplicato: in luce lineare nessun canale supera l'alfa.
		bool bOk = true;
		for (size_t Pixel = 0; Pixel < Pixels.size() / 4 && bOk; ++Pixel)
		{
			const float Alpha = Pixels[Pixel * 4 + 3] / 255.0f;
			for (int Channel = 0; Channel < 3; ++Channel)
			{
				if (Tiles::Detail::SrgbToLinear(Pixels[Pixel * 4 + Channel]) > Alpha + 0.01f) { bOk = false; }
			}
		}
		Check(bOk, "colore premoltiplicato: lineare(colore) <= alfa in ogni pixel");
	}

	// Sterrato: stesso tratturo, colore diverso.
	{
		FRoadStyle Style = MakeRoadStyle(ERoadStyle::Realistic);
		const FVectorTile Paved = MakeTile(TorinoKey, { { ERoadClass::Service, 0, 0, 300, { {0, Half}, {16384, Half} } } });
		const FVectorTile Dirt = MakeTile(TorinoKey, { { ERoadClass::Service, Tiles::RoadFlagUnpaved, 0, 300, { {0, Half}, {16384, Half} } } });
		std::vector<uint8_t> A, B;
		RasterizeRoads(Paved, Request, Style, A);
		RasterizeRoads(Dirt, Request, Style, B);
		const size_t Centre = (static_cast<size_t>(Size / 2) * Size + Size / 2) * 4;
		Check(B[Centre + 2] > B[Centre + 0] + 10 && std::abs(A[Centre + 2] - A[Centre + 0]) < 6,
			"lo sterrato e' marrone, l'asfalto grigio");
	}

	// Una tile senza niente nella finestra: nessuna immagine da creare.
	{
		const FVectorTile Tile = MakeTile(TorinoKey, { { ERoadClass::Primary, 0, 0, 70, { {100, 100}, {900, 100} } } });
		const FRoadRasterRequest Corner = MakeRasterRequest(FTileKey{ 15, 8538 * 4 + 3, 2044 * 4 + 3 }, 13, 16384, 256);
		std::vector<uint8_t> Pixels;
		FRoadRasterStats Stats;
		const bool bAnything = RasterizeRoads(Tile, Corner, Map, Pixels, &Stats);
		Check(!bAnything && Stats.FeaturesSkipped == 1, "linea lontana dalla finestra: scartata, immagine vuota");
	}
}

// ===========================================================================
//  6. Una tile figlia e' il pezzo giusto della madre
// ===========================================================================
static void TestChildMatchesParent()
{
	Section("6. Tile di terreno piu' profonde: il ritaglio combacia");

	// La madre a 512 pixel e ognuna delle quattro figlie a 256 hanno pixel
	// della stessa dimensione negli stessi punti: devono coincidere. E' il test
	// che dice se la finestra (MakeVectorWindow) e' giusta al pixel.
	const FVectorTile Tile = MakeTile(TorinoKey, {
		{ ERoadClass::Primary, 0, 0, 120, { {1000, 3000}, {9000, 12000}, {15000, 7000} } },
		{ ERoadClass::Rail, 0, 0, 45, { {-800, 15000}, {17000, 500} } },
		{ ERoadClass::Path, Tiles::RoadFlagUnpaved, 0, 20, { {8100, 8300}, {8200, 8100} } } });
	const FRoadStyle Style = MakeRoadStyle(ERoadStyle::Realistic);

	std::vector<uint8_t> Parent;
	RasterizeRoads(Tile, MakeRasterRequest(TorinoKey, 13, 16384, 512), Style, Parent);

	int WorstDifference = 0;
	for (int Child = 0; Child < 4; ++Child)
	{
		const FTileKey ChildKey = TorinoKey.GetChild(Child);
		std::vector<uint8_t> Pixels;
		RasterizeRoads(Tile, MakeRasterRequest(ChildKey, 13, 16384, 256), Style, Pixels);
		const int OffsetX = (Child & 1) * 256;
		const int OffsetY = ((Child >> 1) & 1) * 256;
		for (int Row = 0; Row < 256; ++Row)
		{
			for (int Column = 0; Column < 256; ++Column)
			{
				for (int Channel = 0; Channel < 4; ++Channel)
				{
					const int A = Pixels[(static_cast<size_t>(Row) * 256 + Column) * 4 + Channel];
					const int B = Parent[(static_cast<size_t>(Row + OffsetY) * 512 + Column + OffsetX) * 4 + Channel];
					WorstDifference = std::max(WorstDifference, std::abs(A - B));
				}
			}
		}
	}
	// Un livello di differenza e' ammesso: la scala in longitudine si calcola
	// al centro di ogni tile, e quello della figlia e' spostato di mezzo km.
	Check(WorstDifference <= 2, "le quattro figlie coincidono con i quarti della madre",
		Fmt("differenza massima %.0f su 255", WorstDifference));
}

// ===========================================================================
//  7. Mipmap e costo
// ===========================================================================
static void TestMipsAndCost()
{
	Section("7. Mipmap del premoltiplicato e costo di una tile densa");

	const FRoadStyle Style = MakeRoadStyle(ERoadStyle::Realistic);
	const FVectorTile Tile = MakeTile(TorinoKey, { { ERoadClass::Primary, 0, 0, 300, { {0, Half}, {16384, Half} } } });
	std::vector<uint8_t> Pixels;
	RasterizeRoads(Tile, MakeRasterRequest(TorinoKey, 13, 16384, 256), Style, Pixels);

	double MeanAlpha = 0.0;
	for (size_t Pixel = 0; Pixel < Pixels.size() / 4; ++Pixel) { MeanAlpha += Pixels[Pixel * 4 + 3]; }
	MeanAlpha /= static_cast<double>(Pixels.size() / 4);

	std::vector<std::vector<uint8_t>> Mips;
	Tiles::BuildMipChain(Pixels, 256, 256, Mips);
	Check(Mips.size() == 8 && Mips.back().size() == 4, "catena di 8 mip fino a 1x1");
	Check(std::fabs(Mips.back()[3] - MeanAlpha) <= 1.0, "l'alfa del mip 1x1 e' la media dell'immagine",
		Fmt("%.0f contro %.1f", Mips.back()[3], MeanAlpha));

	// Una tile "di citta'": 3000 linee corte sparse. Il costo va guardato,
	// non solo controllato: e' quello che gira nei worker a ogni tile nuova.
	std::vector<FTestLine> Dense;
	uint32_t Seed = 12345;
	auto Next = [&Seed]() { Seed = Seed * 1664525u + 1013904223u; return static_cast<int>((Seed >> 8) % 16000); };
	for (int Index = 0; Index < 3000; ++Index)
	{
		const int X = Next(), Y = Next();
		Dense.push_back({ ERoadClass::Residential, 0, 0, 55, { {X, Y}, {X + 120, Y + 40}, {X + 200, Y + 160} } });
	}
	const FVectorTile City = MakeTile(TorinoKey, Dense);
	for (const int32_t Size : { 256, 512, 1024 })
	{
		const auto Start = std::chrono::steady_clock::now();
		FRoadRasterStats Stats;
		RasterizeRoads(City, MakeRasterRequest(TorinoKey, 13, 16384, Size), Style, Pixels, &Stats);
		const double Ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - Start).count();
		std::printf("  %4d px: %.1f ms, %d linee, %lld pezzi di segmento, %lld pixel valutati\n",
			Size, Ms, Stats.FeaturesDrawn, static_cast<long long>(Stats.SegmentsDrawn),
			static_cast<long long>(Stats.PixelsEvaluated));
		if (Size == 512) { Check(Ms < 250.0, "3000 linee a 512 pixel in meno di 250 ms", Fmt("%.1f ms", Ms)); }
	}
}


// ===========================================================================
//  8-11. Strade 3D (RoadMesh.h)
// ===========================================================================

// Una tile di terreno sintetica sopra Torino, livello 14, con una funzione di
// quota data. La tile vettoriale e' la 13 della fixture: la 14 ne e' un quarto.
static const FTileKey Terrain14{ 14, 8538 * 2, 2044 * 2 };

template <typename HeightFunction>
static Tiles::FHeightTile MakeTerrain(HeightFunction Height)
{
	Tiles::FHeightTile Tile;
	Tile.Key = Terrain14;
	Tile.Heights.resize(static_cast<size_t>(Tiles::TilePosts) * Tiles::TilePosts);
	Tile.MinHeight = 1e9f;
	Tile.MaxHeight = -1e9f;
	for (int J = 0; J < Tiles::TilePosts; ++J)
	{
		for (int I = 0; I < Tiles::TilePosts; ++I)
		{
			const float Value = static_cast<float>(Height(I, J));
			Tile.Heights[static_cast<size_t>(J) * Tiles::TilePosts + I] = Value;
			Tile.MinHeight = std::min(Tile.MinHeight, Value);
			Tile.MaxHeight = std::max(Tile.MaxHeight, Value);
		}
	}
	return Tile;
}

// Riporta un vertice della mesh (frame locale) in geodetiche.
static Core::FGeodetic VertexGeodetic(const Mesh::FTileMeshData& MeshData, size_t Vertex)
{
	const Core::FEcef OriginEcef = Core::GeodeticToEcef(MeshData.Origin, Core::WGS84);
	const Core::FMat3 ToEcef = Mesh::Detail::MakeLocalNeu(MeshData.Origin).Transposed();
	const Core::FEcef Local{ MeshData.Positions[Vertex * 3 + 0], MeshData.Positions[Vertex * 3 + 1],
	                         MeshData.Positions[Vertex * 3 + 2] };
	return Core::EcefToGeodetic(ToEcef.Transform(Local) + OriginEcef, Core::WGS84);
}

// Tutte le linee della tile vettoriale nel quarto nord-ovest (la tile 14).
static FVectorTile RoadsInNorthWestQuarter(const std::vector<FTestLine>& Lines)
{
	return MakeTile(TorinoKey, Lines);
}

static void TestSurfaceSampler()
{
	Section("8. La quota della superficie disegnata (FSurfaceSampler)");

	// Terreno ondulato: non un piano, cosi' i due triangoli di ogni cella
	// danno risultati diversi e la diagonale conta.
	const Tiles::FHeightTile Tile = MakeTerrain([](int I, int J)
		{ return 300.0 + 40.0 * std::sin(I * 0.21) * std::cos(J * 0.17) + 0.5 * I; });

	const FSurfaceSampler Surface(Tile, 2);
	const Tiles::FTileBounds Bounds = Tiles::GetTileBounds(Tile.Key.Level, Tile.Key.X, Tile.Key.Y);
	const double Spacing = Tiles::TileSpanDeg(Tile.Key.Level) / Surface.GetCells();

	double WorstPost = 0.0;
	for (int J = 0; J <= Surface.GetCells(); J += 7)
	{
		for (int I = 0; I <= Surface.GetCells(); I += 5)
		{
			const double H = Surface.HeightAt(Bounds.West + I * Spacing, Bounds.North - J * Spacing);
			WorstPost = std::max(WorstPost, std::fabs(H - Tile.GetHeight(I * 2, J * 2)));
		}
	}
	Check(WorstPost < 1e-3, "sui post della mesh (passo 2) la quota e' quella del post", Fmt("%.6f m", WorstPost));

	// Al centro di una cella, sulla diagonale NO-SE, i due triangoli danno la
	// media dei due post della diagonale: e' la diagonale di BuildTileMesh.
	const int I = 10, J = 20;
	const double Centre = Surface.HeightAt(Bounds.West + (I + 0.5) * Spacing, Bounds.North - (J + 0.5) * Spacing);
	const double Diagonal = 0.5 * (Tile.GetHeight(I * 2, J * 2) + Tile.GetHeight((I + 1) * 2, (J + 1) * 2));
	Check(std::fabs(Centre - Diagonal) < 1e-3, "al centro di una cella: media della diagonale nord-ovest / sud-est",
		Fmt("%.3f contro %.3f", Centre, Diagonal));

	// E la mesh vera, costruita da BuildTileMesh, ha i vertici sulla stessa
	// superficie: si confronta un vertice della mesh con il campionatore.
	Mesh::FTileMeshParameters Parameters;
	Parameters.Step = 2;
	Mesh::FTileMeshData Terrain;
	Mesh::BuildTileMesh(Tile, Parameters, Terrain);
	const Core::FGeodetic Vertex = VertexGeodetic(Terrain, 33 * 65 + 21);
	const double Sampled = Surface.HeightAt(Vertex.LonRad / Core::DegToRad, Vertex.LatRad / Core::DegToRad);
	Check(std::fabs(Sampled - Vertex.HeightM) < 0.01, "un vertice di BuildTileMesh sta sulla superficie campionata",
		Fmt("%.4f m di scarto", Sampled - Vertex.HeightM));
}

// Quanto un vertice della mesh delle strade sta sopra il terreno disegnato.
static double AboveSurface(const Mesh::FTileMeshData& MeshData, size_t Vertex, const FSurfaceSampler& Surface)
{
	const Core::FGeodetic Point = VertexGeodetic(MeshData, Vertex);
	return Point.HeightM - Surface.HeightAt(Point.LonRad / Core::DegToRad, Point.LatRad / Core::DegToRad);
}

// Il vertice usa la superficie `Surface` dell'atlante? (dalla sua U)
static bool UsesSurface(const Mesh::FTileMeshData& MeshData, size_t Vertex, ERoadSurface Surface)
{
	float U0 = 0.0f, U1 = 0.0f;
	SurfaceUvRange(Surface, U0, U1);
	const float U = MeshData.UVs[Vertex * 2];
	return U >= U0 - 1e-5f && U <= U1 + 1e-5f;
}

// Spostamento in metri (nord, est) fra due vertici.
static void NorthEast(const Mesh::FTileMeshData& MeshData, size_t A, size_t B, double& North, double& East)
{
	const Core::FGeodetic PA = VertexGeodetic(MeshData, A);
	const Core::FGeodetic PB = VertexGeodetic(MeshData, B);
	North = (PB.LatRad - PA.LatRad) / Core::DegToRad * 111132.0;
	East = (PB.LonRad - PA.LonRad) / Core::DegToRad * 111320.0 * std::cos(PA.LatRad);
}

static void TestRoadOnTerrain()
{
	Section("9. Strade 3D posate sul terreno: carreggiata in piano, scarpate");

	const Tiles::FHeightTile Tile = MakeTerrain([](int I, int J)
		{ return 300.0 + 35.0 * std::sin(I * 0.13) * std::cos(J * 0.11) + 0.8 * J; });
	const FSurfaceSampler Surface(Tile, 2);

	// Una primaria in diagonale e una terziaria curva, nel quarto NO della
	// tile vettoriale del 13 (cioe' dentro la tile 14), piu' una che esce.
	const FVectorTile Vectors = RoadsInNorthWestQuarter({
		{ ERoadClass::Primary, 0, 0, 80, { {500, 600}, {3000, 2500}, {7500, 7000} } },
		{ ERoadClass::Tertiary, 0, 0, 55, { {1000, 7000}, {2500, 6000}, {4000, 6500}, {6000, 5200} } },
		{ ERoadClass::Primary, 0, 0, 80, { {4000, 4000}, {12000, 4100} } } });

	const FVectorWindow Window = MakeVectorWindow(Terrain14, 13, 16384);
	const FRoad3DStyle Style = MakeRoad3DStyle();
	FRoadMeshParameters Parameters;
	Parameters.Step = 2;

	Mesh::FTileMeshData Roads;
	FRoadMeshStats Stats;
	BuildRoadMesh(Tile, Surface, nullptr, Vectors, Window, Style, Parameters, Roads, &Stats);
	Check(Stats.Features == 3 && Roads.TriangleCount > 100, "tre strade, centinaia di triangoli",
		Fmt("%.0f strade, %.0f triangoli, %.0f sezioni", Stats.Features, Roads.TriangleCount, Stats.CrossSections));

	// Stessa origine della mesh del terreno: e' cio' che permette al componente
	// della strada di usare la trasformazione della tile.
	Mesh::FTileMeshParameters TerrainParameters;
	TerrainParameters.Step = 2;
	Mesh::FTileMeshData Terrain;
	Mesh::BuildTileMesh(Tile, TerrainParameters, Terrain);
	Check(Roads.Origin.LatRad == Terrain.Origin.LatRad && Roads.Origin.LonRad == Terrain.Origin.LonRad
		&& Roads.Origin.HeightM == Terrain.Origin.HeightM, "origine identica a quella della mesh del terreno");

	// La carreggiata non e' MAI sotto il terreno: sta al suo punto piu' alto
	// piu' il sollevamento. I piedi delle scarpate invece entrano nel
	// terreno di FootSink: niente fessure fra strada e prato.
	double DeckLowest = 1e9, DeckHighest = -1e9, FootLowest = 1e9;
	double WestMost = 1e9, EastMost = -1e9;
	const Tiles::FTileBounds Bounds = Tiles::GetTileBounds(Tile.Key.Level, Tile.Key.X, Tile.Key.Y);
	for (size_t Vertex = 0; Vertex < Roads.Positions.size() / 3; ++Vertex)
	{
		const double Above = AboveSurface(Roads, Vertex, Surface);
		if (UsesSurface(Roads, Vertex, ERoadSurface::MajorAsphalt))
		{
			DeckLowest = std::min(DeckLowest, Above);
			DeckHighest = std::max(DeckHighest, Above);
		}
		if (UsesSurface(Roads, Vertex, ERoadSurface::Verge)) { FootLowest = std::min(FootLowest, Above); }
		const double Lon = VertexGeodetic(Roads, Vertex).LonRad / Core::DegToRad;
		WestMost = std::min(WestMost, Lon);
		EastMost = std::max(EastMost, Lon);
	}
	Check(DeckLowest >= Parameters.Lift - 1e-3, "nessun punto della carreggiata sotto il terreno (+ sollevamento)",
		Fmt("minimo %.3f m sopra", DeckLowest));
	// Ma neanche sospesa piu' di quanto il pendio impone: su questo terreno,
	// ripidissimo (fino al 65% di traverso), il bordo a valle di una strada da
	// 8 m puo' stare al massimo ~5 m sopra il suo terreno, e la scarpata lo
	// raccorda. Su pendii veri (10-30%) sono decine di centimetri.
	Check(DeckHighest < Parameters.Lift + 0.25 + 0.65 * 8.0, "ne' sospesa piu' di quanto impone il pendio",
		Fmt("massimo %.2f m sopra", DeckHighest));
	Check(std::fabs(FootLowest + Parameters.FootSink) < 0.01, "il piede della scarpata entra nel terreno",
		Fmt("%.3f m", FootLowest));

	// La strada che esce a est si ferma al bordo (piu' strada e scarpata).
	const double ReachDeg = (4.0 + Parameters.VergeMaxWidth) / (111320.0 * std::cos(Bounds.CentreLat() * Core::DegToRad));
	Check(EastMost <= Bounds.East + ReachDeg && WestMost >= Bounds.West - ReachDeg,
		"la strada che esce dalla tile si ferma al bordo");

	// Larghezza e pendenza trasversale: sulla primaria dritta il primo
	// quadrilatero e' la carreggiata. I suoi due bordi distano 8 m e stanno
	// alla STESSA quota, anche su un terreno in pendenza: in piano di traverso.
	const FVectorTile Straight = RoadsInNorthWestQuarter({ { ERoadClass::Primary, 0, 0, 80, { {1000, 4000}, {7000, 4000} } } });
	Mesh::FTileMeshData Single;
	BuildRoadMesh(Tile, Surface, nullptr, Straight, Window, Style, Parameters, Single);
	double North = 0.0, East = 0.0;
	NorthEast(Single, 0, 1, North, East);
	const double Width = std::sqrt(North * North + East * East);
	Check(std::fabs(Width - 8.0) < 0.05, "una primaria da 8 m e' larga 8 m", Fmt("%.3f m", Width));
	const double Tilt = VertexGeodetic(Single, 0).HeightM - VertexGeodetic(Single, 1).HeightM;
	Check(std::fabs(Tilt) < 0.01, "la carreggiata e' in piano di traverso (il terreno sale di 0,8 m per post)",
		Fmt("%.4f m fra i due bordi", Tilt));

	// Le facce guardano dalla stessa parte del terreno. Con l'inversione
	// attiva (default) la normale geometrica "matematica" punta in giu', come
	// quella dei triangoli del terreno; senza, in su. Le facce verticali
	// (cordoli, guardrail, parapetti) non contano: guardano di lato.
	auto CountUp = [](const Mesh::FTileMeshData& MeshData)
	{
		int Up = 0, Down = 0;
		for (size_t Triangle = 0; Triangle < MeshData.Indices.size() / 3; ++Triangle)
		{
			const float* A = &MeshData.Positions[MeshData.Indices[Triangle * 3 + 0] * 3];
			const float* B = &MeshData.Positions[MeshData.Indices[Triangle * 3 + 1] * 3];
			const float* C = &MeshData.Positions[MeshData.Indices[Triangle * 3 + 2] * 3];
			const double E1[3] = { B[0] - A[0], B[1] - A[1], B[2] - A[2] };
			const double E2[3] = { C[0] - A[0], C[1] - A[1], C[2] - A[2] };
			const double N[3] = { E1[1] * E2[2] - E1[2] * E2[1], E1[2] * E2[0] - E1[0] * E2[2], E1[0] * E2[1] - E1[1] * E2[0] };
			const double Len = std::sqrt(N[0] * N[0] + N[1] * N[1] + N[2] * N[2]);
			if (Len <= 0.0 || std::fabs(N[2]) < 0.5 * Len) { continue; }
			(N[2] > 0.0 ? Up : Down)++;
		}
		return std::make_pair(Up, Down);
	};
	const auto RoadFaces = CountUp(Roads);
	const auto TerrainFaces = CountUp(Terrain);
	Check(RoadFaces.first == 0 && TerrainFaces.first == 0,
		"stesso verso delle facce del terreno (inversione attiva)",
		Fmt("strade %.0f su, %.0f giu'", RoadFaces.first, RoadFaces.second));
	FRoadMeshParameters Unflipped = Parameters;
	Unflipped.bFlipWinding = false;
	Mesh::FTileMeshData RoadsUp;
	BuildRoadMesh(Tile, Surface, nullptr, Vectors, Window, Style, Unflipped, RoadsUp);
	Check(CountUp(RoadsUp).second == 0, "senza inversione, tutte in su");

	// Agli incroci la classe piu' importante sta piu' in alto: la primaria e
	// la residenziale non si contendono lo stesso piano.
	const double LiftPrimary = Parameters.Lift + Parameters.LiftPerTier * Style.Classes[static_cast<int>(ERoadClass::Primary)].Tier;
	const double LiftResidential = Parameters.Lift + Parameters.LiftPerTier * Style.Classes[static_cast<int>(ERoadClass::Residential)].Tier;
	Check(LiftPrimary - LiftResidential >= 0.05, "agli incroci la primaria sta almeno 5 cm sopra la residenziale",
		Fmt("%.2f m", LiftPrimary - LiftResidential));

	// I sentieri restano dipinti: in 3D sarebbero nastri sotto il pixel.
	Mesh::FTileMeshData PathMesh;
	BuildRoadMesh(Tile, Surface, nullptr, RoadsInNorthWestQuarter({ { ERoadClass::Path, 0, 0, 15, { {1000, 1000}, {5000, 5000} } } }),
		Window, Style, Parameters, PathMesh);
	Check(PathMesh.TriangleCount == 0, "i sentieri non diventano 3D");
}

static void TestBridge()
{
	Section("10. I ponti stanno sopra la valle, con parapetti e pile");

	// Una valle a V nord-sud al centro della tile, profonda 60 m.
	const Tiles::FHeightTile Tile = MakeTerrain([](int I, int) { return 400.0 - 60.0 * std::max(0.0, 1.0 - std::fabs(I - 64) / 20.0); });
	const FSurfaceSampler Surface(Tile, 2);
	const FVectorWindow Window = MakeVectorWindow(Terrain14, 13, 16384);

	// La stessa strada est-ovest, una volta su ponte e una volta no. Va da un
	// versante all'altro (la tile 14 e' larga 8192 unita' della 13).
	const std::vector<std::pair<int, int>> Line{ {1000, 4000}, {7000, 4000} };
	const FVectorTile Bridge = RoadsInNorthWestQuarter({ { ERoadClass::Secondary, Tiles::RoadFlagBridge, 1, 70, Line } });
	const FVectorTile Road = RoadsInNorthWestQuarter({ { ERoadClass::Secondary, 0, 0, 70, Line } });

	FRoadMeshParameters Parameters;
	Mesh::FTileMeshData BridgeMesh, RoadMesh;
	FRoadMeshStats Stats;
	BuildRoadMesh(Tile, Surface, nullptr, Bridge, Window, MakeRoad3DStyle(), Parameters, BridgeMesh, &Stats);
	BuildRoadMesh(Tile, Surface, nullptr, Road, Window, MakeRoad3DStyle(), Parameters, RoadMesh);

	auto Extreme = [](const Mesh::FTileMeshData& MeshData, ERoadSurface Material, bool bLowest)
	{
		double Value = bLowest ? 1e9 : -1e9;
		for (size_t Vertex = 0; Vertex < MeshData.Positions.size() / 3; ++Vertex)
		{
			if (!UsesSurface(MeshData, Vertex, Material)) { continue; }
			const double H = VertexGeodetic(MeshData, Vertex).HeightM;
			Value = bLowest ? std::min(Value, H) : std::max(Value, H);
		}
		return Value;
	};
	const double RoadLow = Extreme(RoadMesh, ERoadSurface::MajorAsphalt, true);
	const double DeckLow = Extreme(BridgeMesh, ERoadSurface::MajorAsphalt, true);
	const double DeckHigh = Extreme(BridgeMesh, ERoadSurface::MajorAsphalt, false);
	Check(RoadLow < 345.0, "la strada normale scende nella valle", Fmt("minimo %.1f m", RoadLow));
	Check(DeckLow > 395.0, "il ponte resta all'altezza delle spalle", Fmt("impalcato minimo %.1f m", DeckLow));

	// Il cemento del ponte va dal fondo dell'impalcato (1,2 m sotto) alla
	// cima del parapetto (1 m sopra).
	const double ConcreteHigh = Extreme(BridgeMesh, ERoadSurface::Concrete, false);
	Check(std::fabs(ConcreteHigh - DeckHigh - Parameters.ParapetHeight) < 0.01, "parapetti alti un metro",
		Fmt("%.2f m sopra l'impalcato", ConcreteHigh - DeckHigh));
	Check(Stats.Bridges == 1 && BridgeMesh.TriangleCount > 2 * RoadMesh.TriangleCount,
		"il ponte ha anche parapetti, fianchi e fondo",
		Fmt("%.0f triangoli contro %.0f", BridgeMesh.TriangleCount, RoadMesh.TriangleCount));

	// Le pile: dove l'impalcato sta alto sulla valle, e da nessun'altra parte.
	const double ConcreteLow = Extreme(BridgeMesh, ERoadSurface::Concrete, true);
	Check(Stats.Piers > 0 && ConcreteLow < 345.0, "pile fino al fondo della valle",
		Fmt("%.0f pile, la piu' bassa a %.1f m", Stats.Piers, ConcreteLow));
}

static void TestSidewalksAndJunctions()
{
	Section("11. Marciapiedi, incroci, guardrail");

	const Tiles::FHeightTile Flat = MakeTerrain([](int, int) { return 250.0; });
	const FSurfaceSampler Surface(Flat, 2);
	const FVectorWindow Window = MakeVectorWindow(Terrain14, 13, 16384);
	const FRoad3DStyle Style = MakeRoad3DStyle();
	FRoadMeshParameters Parameters;

	// Una residenziale senza tag: marciapiedi da tutte e due le parti, un
	// cordolo sopra la carreggiata.
	Mesh::FTileMeshData Street;
	FRoadMeshStats Stats;
	BuildRoadMesh(Flat, Surface, nullptr, RoadsInNorthWestQuarter({ { ERoadClass::Residential, 0, 0, 60, { {1000, 4000}, {7000, 4000} } } }),
		Window, Style, Parameters, Street, &Stats);
	double PavingHeight = -1.0, AsphaltHeight = -1.0;
	int North = 0, South = 0;
	const double CentreLat = VertexGeodetic(Street, 0).LatRad;   // la carreggiata e' simmetrica: si usa la media dopo
	double DeckLatSum = 0.0;
	int DeckCount = 0;
	for (size_t Vertex = 0; Vertex < Street.Positions.size() / 3; ++Vertex)
	{
		if (UsesSurface(Street, Vertex, ERoadSurface::MinorAsphalt))
		{
			AsphaltHeight = VertexGeodetic(Street, Vertex).HeightM;
			DeckLatSum += VertexGeodetic(Street, Vertex).LatRad;
			++DeckCount;
		}
	}
	const double AxisLat = DeckCount > 0 ? DeckLatSum / DeckCount : CentreLat;
	for (size_t Vertex = 0; Vertex < Street.Positions.size() / 3; ++Vertex)
	{
		if (!UsesSurface(Street, Vertex, ERoadSurface::Paving)) { continue; }
		const Core::FGeodetic Point = VertexGeodetic(Street, Vertex);
		PavingHeight = Point.HeightM;
		(Point.LatRad > AxisLat ? North : South)++;
	}
	Check(North > 0 && South > 0 && Stats.SidewalkSections > 0, "residenziale senza tag: marciapiedi sui due lati",
		Fmt("%.0f vertici a nord, %.0f a sud", North, South));
	Check(std::fabs(PavingHeight - AsphaltHeight - Parameters.KerbHeight) < 0.005, "il marciapiede sta un cordolo (15 cm) sopra",
		Fmt("%.3f m", PavingHeight - AsphaltHeight));

	// Il tag OSM decide il lato, nel verso della linea: verso est, la DESTRA
	// e' il sud.
	Mesh::FTileMeshData RightOnly;
	BuildRoadMesh(Flat, Surface, nullptr,
		RoadsInNorthWestQuarter({ { ERoadClass::Secondary, Tiles::RoadFlagSidewalkKnown | Tiles::RoadFlagSidewalkRight, 0, 70,
			{ {1000, 4000}, {7000, 4000} } } }), Window, Style, Parameters, RightOnly);
	North = South = 0;
	for (size_t Vertex = 0; Vertex < RightOnly.Positions.size() / 3; ++Vertex)
	{
		if (UsesSurface(RightOnly, Vertex, ERoadSurface::Paving))
		{
			(VertexGeodetic(RightOnly, Vertex).LatRad > AxisLat ? North : South)++;
		}
	}
	Check(North == 0 && South > 0, "sidewalk=right verso est: il marciapiede e' a sud",
		Fmt("%.0f a nord, %.0f a sud", North, South));

	// Un incrocio: due residenziali a croce. I marciapiedi si fermano dove
	// incontrano l'altra carreggiata, invece di attraversarla.
	Mesh::FTileMeshData Cross;
	FRoadMeshStats CrossStats;
	BuildRoadMesh(Flat, Surface, nullptr, RoadsInNorthWestQuarter({
			{ ERoadClass::Residential, 0, 0, 60, { {1000, 4000}, {7000, 4000} } },
			{ ERoadClass::Residential, 0, 0, 60, { {4000, 1000}, {4000, 7000} } } }),
		Window, Style, Parameters, Cross, &CrossStats);
	// Dove sta l'asse nord-sud: la media delle longitudini del suo asfalto
	// non serve, basta il punto 4000 della tile 13 in metri dal bordo ovest.
	const Tiles::FTileBounds Bounds = Tiles::GetTileBounds(Flat.Key.Level, Flat.Key.X, Flat.Key.Y);
	const double AxisLon = Bounds.West + (Bounds.East - Bounds.West) * (4000.0 / 8192.0);
	const double AxisLatDeg = Bounds.North - (Bounds.North - Bounds.South) * (4000.0 / 8192.0);
	const double MetresLon = 111320.0 * std::cos(Bounds.CentreLat() * Core::DegToRad);
	int InsideOther = 0;
	for (size_t Vertex = 0; Vertex < Cross.Positions.size() / 3; ++Vertex)
	{
		if (!UsesSurface(Cross, Vertex, ERoadSurface::Paving)) { continue; }
		const Core::FGeodetic Point = VertexGeodetic(Cross, Vertex);
		const double FromNS = std::fabs(Point.LonRad / Core::DegToRad - AxisLon) * MetresLon;
		const double FromEW = std::fabs(Point.LatRad / Core::DegToRad - AxisLatDeg) * 111132.0;
		// Dentro una carreggiata (3 m dall'asse) e fuori dalla propria.
		if ((FromNS < 2.9 && FromEW > 3.1) || (FromEW < 2.9 && FromNS > 3.1)) { ++InsideOther; }
	}
	Check(CrossStats.JunctionCuts > 0 && InsideOther == 0, "all'incrocio i marciapiedi non attraversano l'altra strada",
		Fmt("%.0f tratti tolti, %.0f vertici di marciapiede in mezzo alla strada", CrossStats.JunctionCuts, InsideOther));

	// Le autostrade hanno il guardrail, le residenziali no.
	Mesh::FTileMeshData Motorway;
	BuildRoadMesh(Flat, Surface, nullptr, RoadsInNorthWestQuarter({ { ERoadClass::Motorway, 0, 0, 110, { {1000, 4000}, {7000, 4000} } } }),
		Window, Style, Parameters, Motorway);
	auto CountSurface = [](const Mesh::FTileMeshData& MeshData, ERoadSurface Material)
	{
		int Count = 0;
		for (size_t Vertex = 0; Vertex < MeshData.Positions.size() / 3; ++Vertex) { Count += UsesSurface(MeshData, Vertex, Material) ? 1 : 0; }
		return Count;
	};
	Check(CountSurface(Motorway, ERoadSurface::Steel) > 0 && CountSurface(Street, ERoadSurface::Steel) == 0,
		"guardrail sull'autostrada, non sulla residenziale");

	// Il geomorphing: con la superficie del padre ogni vertice ha il suo
	// spostamento; quello dei ponti e' zero (non seguono il terreno).
	const Tiles::FHeightTile Bumpy = MakeTerrain([](int I, int J) { return 250.0 + 3.0 * std::sin(I * 0.7) * std::cos(J * 0.5); });
	const FSurfaceSampler BumpySurface(Bumpy, 2);
	Mesh::FTileMeshData Morphing;
	BuildRoadMesh(Bumpy, BumpySurface, nullptr, RoadsInNorthWestQuarter({ { ERoadClass::Residential, 0, 0, 60, { {1000, 4000}, {7000, 4000} } } }),
		Window, Style, Parameters, Morphing, nullptr, &Surface);
	double LargestDelta = 0.0;
	for (const float Delta : Morphing.MorphDeltas) { LargestDelta = std::max(LargestDelta, std::fabs(static_cast<double>(Delta))); }
	Check(Morphing.MorphDeltas.size() == Morphing.Positions.size() / 3 && Morphing.ParentNormals.size() == Morphing.Normals.size()
		&& LargestDelta > 0.5 && LargestDelta < 3.01,
		"strade e geomorphing: uno spostamento per vertice, quanto il terreno",
		Fmt("massimo %.2f m", LargestDelta));
	Mesh::FTileMeshData NoMorph;
	BuildRoadMesh(Bumpy, BumpySurface, nullptr, RoadsInNorthWestQuarter({ { ERoadClass::Residential, 0, 0, 60, { {1000, 4000}, {7000, 4000} } } }),
		Window, Style, Parameters, NoMorph);
	Check(NoMorph.MorphDeltas.empty(), "senza il padre, niente dati di morphing (il provider usa zero)");
}

static void TestAtlasAndCost()
{
	Section("12. Atlante delle superfici e costo di una tile di citta'");

	std::vector<uint8_t> Atlas;
	BuildRoadAtlas(Atlas);
	Check(Atlas.size() == static_cast<size_t>(RoadAtlasWidth) * RoadAtlasHeight * 4, "atlante 1024 x 512 BGRA");

	auto Pixel = [&Atlas](int X, int Y) { return Atlas.data() + (static_cast<size_t>(Y) * RoadAtlasWidth + X) * 4; };
	const int Strip = RoadAtlasStripPixels;
	const int Guard = RoadAtlasGuardPixels;
	const int Content = RoadAtlasContentPixels;
	Check(Pixel(Guard + Content / 2, 10)[2] > 150 && Pixel(Guard + Content / 2, 300)[2] < 110,
		"asfalto principale: mezzeria bianca a tratti (4,5 m su 12)");
	Check(Pixel(Guard + 2, 100)[2] > 200, "asfalto principale: striscia bianca di bordo");
	Check(Pixel(Guard + 6, 100)[3] > 190 && Pixel(Guard + 2, 100)[3] < 160,
		"ruvidita' nell'alfa: l'asfalto e' opaco, la vernice meno");
	const int RailColumn = 3 * Strip + Guard + static_cast<int>(Content * 0.5 - 1.435 / 4.5 * Content * 0.5);
	Check(Pixel(RailColumn, 30)[2] > 145 && Pixel(RailColumn, 30)[3] < 110, "ferrovia: la rotaia e' acciaio, e luccica",
		Fmt("colonna %.0f: R %.0f, ruvidita' %.0f", RailColumn, Pixel(RailColumn, 30)[2], Pixel(RailColumn, 30)[3]));
	Check(std::abs(Pixel(1 * Strip + Guard + 5, 50)[2] - Pixel(1 * Strip + Guard + 5, 50)[0]) < 8, "asfalto semplice: grigio neutro");

	// La banda di guardia ripete il bordo della striscia.
	bool bGuardOk = true;
	for (int S = 0; S < 11; ++S)
	{
		for (int Column = 0; Column < Guard; ++Column)
		{
			for (int Channel = 0; Channel < 4; ++Channel)
			{
				bGuardOk &= Pixel(S * Strip + Column, 77)[Channel] == Pixel(S * Strip + Guard, 77)[Channel];
				bGuardOk &= Pixel((S + 1) * Strip - 1 - Column, 77)[Channel] == Pixel((S + 1) * Strip - 1 - Guard, 77)[Channel];
			}
		}
	}
	Check(bGuardOk, "bande di guardia: 8 pixel per lato che ripetono il bordo");

	float U0 = 0.0f, U1 = 0.0f;
	SurfaceUvRange(ERoadSurface::Rail, U0, U1);
	Check(U0 * RoadAtlasWidth >= 3 * Strip + Guard && U1 * RoadAtlasWidth <= 4 * Strip - Guard,
		"la U della ferrovia sta nella parte utile della sua striscia");

	// Le mipmap a blocchi 2x2 non mescolano due strisce: al mip 4 (strisce da
	// 4 pixel) la striscia del cemento ha ancora il colore del cemento.
	std::vector<std::vector<uint8_t>> Mips;
	Tiles::BuildMipChain(Atlas, RoadAtlasWidth, RoadAtlasHeight, Mips);
	const std::vector<uint8_t>& Mip4 = Mips[3];
	const int Mip4Width = RoadAtlasWidth / 16;
	const int ConcreteRed = Pixel(4 * Strip + Guard + 20, 40)[2];
	const int Mip4Red = Mip4[(static_cast<size_t>(5) * Mip4Width + 4 * (Strip / 16)) * 4 + 2];
	Check(std::abs(Mip4Red - ConcreteRed) < 15, "mip 4: il bordo della striscia del cemento e' ancora cemento",
		Fmt("%.0f contro %.0f", Mip4Red, ConcreteRed));

	// Il costo: una tile di citta' con 3000 strade corte, tutte con i
	// marciapiedi e piene di incroci.
	const Tiles::FHeightTile Tile = MakeTerrain([](int I, int J) { return 240.0 + 0.3 * I + 0.2 * J; });
	const FSurfaceSampler Surface(Tile, 2);
	std::vector<FTestLine> Dense;
	uint32_t Seed = 777;
	auto Next = [&Seed]() { Seed = Seed * 1664525u + 1013904223u; return static_cast<int>((Seed >> 8) % 8000); };
	for (int Index = 0; Index < 3000; ++Index)
	{
		const int X = Next(), Y = Next();
		Dense.push_back({ ERoadClass::Residential, 0, 0, 55, { {X, Y}, {X + 150, Y + 60}, {X + 260, Y + 190} } });
	}
	const FVectorTile City = MakeTile(TorinoKey, Dense);
	const auto Start = std::chrono::steady_clock::now();
	Mesh::FTileMeshData Roads;
	FRoadMeshStats Stats;
	BuildRoadMesh(Tile, Surface, nullptr, City, MakeVectorWindow(Terrain14, 13, 16384), MakeRoad3DStyle(),
		FRoadMeshParameters{}, Roads, &Stats, &Surface);
	const double Ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - Start).count();
	std::printf("  %d strade, %d sezioni, %d triangoli, %d tratti tolti agli incroci, %.1f ms\n",
		Stats.Features, Stats.CrossSections, Stats.Triangles, Stats.JunctionCuts, Ms);
	Check(Ms < 1500.0, "3000 strade in meno di 1,5 s (su un worker)", Fmt("%.1f ms", Ms));
	Check(Stats.Triangles < 600000, "meno di 600.000 triangoli", Fmt("%.0f", Stats.Triangles));
}

// ===========================================================================
//  13. I lampioni: quali strade, dove (Roads/StreetLights.h)
// ===========================================================================

namespace LightsTest
{
	using namespace GeoWorld::StreetLights;

	const FTileMetric& Metric()
	{
		static const FTileMetric Value = FTileMetric::For(TorinoKey, Tiles::VectorTileExtent);
		return Value;
	}
	int UX(double Metres) { return static_cast<int>(std::lround(Metres / Metric().MetresPerUnitX)); }
	int UY(double Metres) { return static_cast<int>(std::lround(Metres / Metric().MetresPerUnitY)); }

	/** Una linea dritta in metri della tile (x est, y sud), in Pieces pezzi uguali. */
	FTestLine Line(ERoadClass Class, double WidthM, double X0, double Y0, double X1, double Y1, uint8_t Lit = 0,
	               uint8_t Flags = 0, int Pieces = 1)
	{
		FTestLine Out{ Class, Flags, 0, static_cast<uint16_t>(std::lround(WidthM * 10.0)), {}, Lit };
		for (int Piece = 0; Piece <= Pieces; ++Piece)
		{
			const double T = static_cast<double>(Piece) / Pieces;
			Out.Points.emplace_back(UX(X0 + (X1 - X0) * T), UY(Y0 + (Y1 - Y0) * T));
		}
		return Out;
	}

	/** Il punto di strada di un lampione, in metri della tile. */
	void AnchorMetres(const FStreetLamp& Lamp, double& OutX, double& OutY)
	{
		const Tiles::FTileBounds& B = Metric().Bounds;
		OutX = (Lamp.AnchorLonDeg - B.West) / B.SpanLon() * Metric().WidthM();
		OutY = (B.North - Lamp.AnchorLatDeg) / B.SpanLat() * Metric().HeightM();
	}
	void PoleMetres(const FStreetLamp& Lamp, double& OutX, double& OutY)
	{
		const Tiles::FTileBounds& B = Metric().Bounds;
		OutX = (Lamp.LonDeg - B.West) / B.SpanLon() * Metric().WidthM();
		OutY = (B.North - Lamp.LatDeg) / B.SpanLat() * Metric().HeightM();
	}

	double DistanceToSegment(double X, double Y, double X0, double Y0, double X1, double Y1)
	{
		const double Dx = X1 - X0, Dy = Y1 - Y0;
		const double T = std::clamp(((X - X0) * Dx + (Y - Y0) * Dy) / std::max(1e-9, Dx * Dx + Dy * Dy), 0.0, 1.0);
		return std::hypot(X - (X0 + T * Dx), Y - (Y0 + T * Dy));
	}

	/** I lampioni nati sulla strada dritta (X0,Y0)-(X1,Y1): il loro punto di strada ci sta sopra. */
	std::vector<FStreetLamp> On(const std::vector<FStreetLamp>& Lamps, double X0, double Y0, double X1, double Y1)
	{
		std::vector<FStreetLamp> Out;
		for (const FStreetLamp& Lamp : Lamps)
		{
			double X = 0.0, Y = 0.0;
			AnchorMetres(Lamp, X, Y);
			if (DistanceToSegment(X, Y, X0, Y0, X1, Y1) < 0.3) { Out.push_back(Lamp); }
		}
		return Out;
	}

	/** Le posizioni lungo la strada (dal suo inizio), ordinate. */
	std::vector<double> Along(const std::vector<FStreetLamp>& Lamps, double X0, double Y0, double X1, double Y1)
	{
		std::vector<double> Out;
		const double Length = std::hypot(X1 - X0, Y1 - Y0);
		for (const FStreetLamp& Lamp : Lamps)
		{
			double X = 0.0, Y = 0.0;
			AnchorMetres(Lamp, X, Y);
			Out.push_back(((X - X0) * (X1 - X0) + (Y - Y0) * (Y1 - Y0)) / Length);
		}
		std::sort(Out.begin(), Out.end());
		return Out;
	}

	/**
	 * Il paese di prova, nel quarto nord-ovest della tile: cinque vie da
	 * ovest a est e quattro da nord a sud (residenziali, 5,5 m), una
	 * provinciale larga che lo attraversa e prosegue in campagna, una
	 * terziaria media da nord a sud.
	 */
	std::vector<FTestLine> Town()
	{
		std::vector<FTestLine> Lines;
		for (const double Y : { 300.0, 420.0, 540.0, 660.0, 780.0 })
		{
			Lines.push_back(Line(ERoadClass::Residential, 5.5, 200.0, Y, 800.0, Y));
		}
		for (const double X : { 200.0, 400.0, 600.0, 800.0 })
		{
			Lines.push_back(Line(ERoadClass::Residential, 5.5, X, 300.0, X, 780.0));
		}
		return Lines;
	}
}

static void TestStreetLightPlacement()
{
	using namespace LightsTest;
	Section("13. I lampioni: quali strade e dove");

	// Il byte dell'illuminazione nel record.
	{
		std::vector<FTestLine> Lines{ Line(ERoadClass::Residential, 5.5, 0, 0, 100, 0, Tiles::RoadLitYes),
		                             Line(ERoadClass::Residential, 5.5, 0, 10, 100, 10, Tiles::RoadLitNo),
		                             Line(ERoadClass::Residential, 5.5, 0, 20, 100, 20, 0),
		                             Line(ERoadClass::Residential, 5.5, 0, 30, 100, 30, 9) };
		const FVectorTile Tile = MakeTile(TorinoKey, Lines);
		Check(Tile.Features.size() == 4 && Tile.Features[0].Lit == Tiles::RoadLitYes && Tile.Features[1].Lit == Tiles::RoadLitNo
			&& Tile.Features[2].Lit == Tiles::RoadLitUnknown && Tile.Features[3].Lit == Tiles::RoadLitUnknown,
			"il byte del tag lit si legge (si', no, non si sa; un valore strano e' 'non si sa')");
	}

	FStreetLightSettings Settings;
	std::vector<FTestLine> Lines = Town();
	// La provinciale: larga 18 m, da ovest a est a y = 480, dentro il paese e poi in campagna.
	Lines.push_back(Line(ERoadClass::Primary, 18.0, 0.0, 480.0, 1700.0, 480.0));
	// La terziaria media, 12 m, da nord a sud a x = 500.
	Lines.push_back(Line(ERoadClass::Tertiary, 12.0, 500.0, 250.0, 500.0, 850.0));
	// Una via spezzata in dieci pezzi da 15 m (x = 300, fra le vie a y = 300 e 450).
	Lines.push_back(Line(ERoadClass::Residential, 5.5, 300.0, 300.0, 300.0, 450.0, 0, 0, 10));
	// Una via con lit=no, una sterrata, un ponte, un sentiero con lit=yes, uno senza, un tratturo: nel paese.
	Lines.push_back(Line(ERoadClass::Residential, 5.5, 220.0, 720.0, 380.0, 720.0, Tiles::RoadLitNo));
	Lines.push_back(Line(ERoadClass::Residential, 5.5, 420.0, 720.0, 580.0, 720.0, 0, Tiles::RoadFlagUnpaved));
	Lines.push_back(Line(ERoadClass::Residential, 5.5, 620.0, 720.0, 780.0, 720.0, 0, Tiles::RoadFlagBridge));
	Lines.push_back(Line(ERoadClass::Path, 1.5, 220.0, 600.0, 380.0, 600.0, Tiles::RoadLitYes));
	Lines.push_back(Line(ERoadClass::Path, 1.5, 420.0, 600.0, 580.0, 600.0));
	Lines.push_back(Line(ERoadClass::Track, 3.0, 620.0, 600.0, 780.0, 600.0));
	// In campagna: un'autostrada senza tag, una con lit=yes, una stradina residenziale isolata di 100 m.
	Lines.push_back(Line(ERoadClass::Motorway, 11.0, 0.0, 1800.0, 1700.0, 1800.0));
	Lines.push_back(Line(ERoadClass::Motorway, 11.0, 0.0, 2000.0, 1700.0, 2000.0, Tiles::RoadLitYes));
	Lines.push_back(Line(ERoadClass::Residential, 5.5, 1400.0, 2200.0, 1500.0, 2200.0));
	const FVectorTile Tile = MakeTile(TorinoKey, Lines);

	FUrbanDensity Density;
	Density.Build(Tile, Metric(), Settings.DensityRadiusM);
	const double InTown = Density.At(UX(500.0), UY(540.0));
	const double Country = Density.At(UX(1400.0), UY(1000.0));
	Check(InTown > 8.0 && Country < 0.5, "la rete di casa: fitta in paese, nulla in campagna",
		Fmt("%.1f e %.2f km/km2", InTown, Country));

	std::vector<FStreetLamp> Lamps;
	FPlacementStats Stats;
	PlaceTileLamps(Tile, Settings, Lamps, &Stats);
	std::printf("  %d linee illuminabili, %d catene, %d lampioni (sbraccio %d, palo %d, lanterna %d), %d tolti agli incroci, %d al buio\n",
		Stats.LitFeatures, Stats.Chains, Stats.Lamps, Stats.ByKind[0], Stats.ByKind[1], Stats.ByKind[2], Stats.Blocked, Stats.Dark);

	// Una via di quartiere: un palo ogni ~26 m, sul marciapiede, lo sbraccio verso la strada.
	const std::vector<FStreetLamp> Street = On(Lamps, 200.0, 660.0, 800.0, 660.0);
	const std::vector<double> Positions = Along(Street, 200.0, 660.0, 800.0, 660.0);
	double MinGap = 1e9, MaxGap = 0.0;
	for (size_t Index = 1; Index < Positions.size(); ++Index)
	{
		MinGap = std::min(MinGap, Positions[Index] - Positions[Index - 1]);
		MaxGap = std::max(MaxGap, Positions[Index] - Positions[Index - 1]);
	}
	Check(Street.size() >= 18 && Street.size() <= 24, "via di 600 m: una ventina di lampioni", Fmt("%.0f", static_cast<double>(Street.size())));
	Check(MinGap > 25.0 && MaxGap < 2.0 * 26.0 + 1.0, "passo di 26 m (al piu' uno saltato a un incrocio)", Fmt("%.1f .. %.1f m", MinGap, MaxGap));
	bool bPosts = !Street.empty(), bOffset = !Street.empty(), bArm = !Street.empty(), bOneSide = !Street.empty();
	for (const FStreetLamp& Lamp : Street)
	{
		double X = 0.0, Y = 0.0, AX = 0.0, AY = 0.0;
		PoleMetres(Lamp, X, Y);
		AnchorMetres(Lamp, AX, AY);
		bPosts = bPosts && Lamp.Kind == ELampKind::Post && std::fabs(Lamp.HeightM - 7.5) < 1e-3;
		// 2,75 di mezza carreggiata + 0,5 sul marciapiede (le residenziali lo hanno di default).
		bOffset = bOffset && std::fabs(std::fabs(Y - AY) - 3.25) < 0.01;
		// Lo sbraccio guarda la strada: la luce e' piu' vicina al centro della base.
		const double LightNorth = std::cos(Lamp.YawRad), LightSouth = -LightNorth;
		bArm = bArm && ((Y > 660.0) == (LightSouth < 0.0));
		bOneSide = bOneSide && (Y > 660.0);
		(void)AX;
	}
	Check(bPosts, "pali medi di 7,5 m");
	Check(bOffset, "il palo a 3,25 m dal centro (mezza carreggiata + mezzo metro)");
	Check(bArm, "lo sbraccio guarda la strada");
	Check(bOneSide, "tutti dallo stesso lato (la destra nel verso della linea, verso est: il sud)");

	// La provinciale larga: a coppie in paese, niente in campagna.
	const std::vector<FStreetLamp> Main = On(Lamps, 0.0, 480.0, 1700.0, 480.0);
	int32_t TownLamps = 0, CountryLamps = 0, North = 0, South = 0;
	for (const FStreetLamp& Lamp : Main)
	{
		double X = 0.0, Y = 0.0, PX = 0.0, PY = 0.0;
		AnchorMetres(Lamp, X, Y);
		PoleMetres(Lamp, PX, PY);
		TownLamps += (X > 150.0 && X < 850.0) ? 1 : 0;
		CountryLamps += (X > 1150.0) ? 1 : 0;
		(PY < 480.0 ? North : South)++;
	}
	Check(TownLamps >= 30 && CountryLamps == 0, "la provinciale: illuminata in paese, buia in campagna",
		Fmt("%.0f in paese, %.0f in campagna", TownLamps, CountryLamps));
	Check(North == South && Main.size() > 0 && Main.front().Kind == ELampKind::Arm, "larga 18 m: a coppie, pali con lo sbraccio",
		Fmt("%.0f a nord, %.0f a sud", North, South));

	// La terziaria media: alternati.
	const std::vector<FStreetLamp> Medium = On(Lamps, 500.0, 250.0, 500.0, 850.0);
	std::vector<std::pair<double, int>> Sides;
	for (const FStreetLamp& Lamp : Medium)
	{
		double X = 0.0, Y = 0.0, PX = 0.0, PY = 0.0;
		AnchorMetres(Lamp, X, Y);
		PoleMetres(Lamp, PX, PY);
		Sides.emplace_back(Y, PX > 500.0 ? 1 : -1);
	}
	std::sort(Sides.begin(), Sides.end());
	int32_t Alternations = 0, Pairs = 0;
	for (size_t Index = 1; Index < Sides.size(); ++Index)
	{
		if (Sides[Index].first - Sides[Index - 1].first < 31.0) { ++Pairs; Alternations += Sides[Index].second != Sides[Index - 1].second ? 1 : 0; }
	}
	Check(Medium.size() >= 10 && Pairs > 0 && Alternations == Pairs, "larga 12 m: alternati (uno a destra, il successivo a sinistra)",
		Fmt("%.0f lampioni, %.0f cambi su %.0f", static_cast<double>(Medium.size()), Alternations, Pairs));

	// Mai su un'altra carreggiata.
	bool bClear = true;
	for (const FStreetLamp& Lamp : Lamps)
	{
		double X = 0.0, Y = 0.0;
		PoleMetres(Lamp, X, Y);
		for (const FTestLine& Road : Lines)
		{
			if (Road.Class == ERoadClass::Path) { continue; }
			const double HalfWidth = Road.WidthDm * 0.05;
			const double X0 = Road.Points.front().first * Metric().MetresPerUnitX, Y0 = Road.Points.front().second * Metric().MetresPerUnitY;
			const double X1 = Road.Points.back().first * Metric().MetresPerUnitX, Y1 = Road.Points.back().second * Metric().MetresPerUnitY;
			if (DistanceToSegment(X, Y, X0, Y0, X1, Y1) < HalfWidth) { bClear = false; }
		}
	}
	Check(bClear && Stats.Blocked > 0, "nessun palo su una carreggiata (agli incroci si salta)", Fmt("%.0f saltati", Stats.Blocked));

	// La via spezzata in dieci pezzi da 15 m: ricucita, si cammina intera.
	const std::vector<double> Broken = Along(On(Lamps, 300.0, 300.0, 300.0, 450.0), 300.0, 300.0, 300.0, 450.0);
	double BrokenGap = 1e9;
	for (size_t Index = 1; Index < Broken.size(); ++Index) { BrokenGap = std::min(BrokenGap, Broken[Index] - Broken[Index - 1]); }
	Check(Broken.size() >= 5 && Broken.size() <= 6 && BrokenGap > 25.0, "una via spezzata in 10 pezzi: ricucita, 26 m di passo (non uno ogni pezzo)",
		Fmt("%.0f lampioni, passo minimo %.1f m", static_cast<double>(Broken.size()), BrokenGap));

	Check(On(Lamps, 220.0, 720.0, 380.0, 720.0).empty(), "lit=no: niente lampioni");
	Check(On(Lamps, 420.0, 720.0, 580.0, 720.0).empty(), "sterrata senza tag: niente");
	Check(On(Lamps, 620.0, 720.0, 780.0, 720.0).empty(), "sul ponte: niente (la luce starebbe in fondo alla valle)");
	const std::vector<FStreetLamp> LitPath = On(Lamps, 220.0, 600.0, 380.0, 600.0);
	Check(!LitPath.empty() && LitPath.front().Kind == ELampKind::Lantern, "sentiero con lit=yes: lanterne",
		Fmt("%.0f", static_cast<double>(LitPath.size())));
	Check(On(Lamps, 420.0, 600.0, 580.0, 600.0).empty() && On(Lamps, 620.0, 600.0, 780.0, 600.0).empty(),
		"sentiero e tratturo senza tag: niente");
	Check(On(Lamps, 0.0, 1800.0, 1700.0, 1800.0).empty(), "autostrada senza tag: buia");
	const std::vector<FStreetLamp> Motorway = On(Lamps, 0.0, 2000.0, 1700.0, 2000.0);
	Check(Motorway.size() >= 35 && Motorway.front().Kind == ELampKind::Arm && std::fabs(Motorway.front().HeightM - 12.0) < 1e-3,
		"autostrada con lit=yes: pali da 12 m ogni 42 m", Fmt("%.0f", static_cast<double>(Motorway.size())));
	Check(On(Lamps, 1400.0, 2200.0, 1500.0, 2200.0).empty(), "una stradina isolata di 100 m in campagna: buia");

	// Le soglie e i colori.
	bool bSwitch = true, bColours = true;
	for (const FStreetLamp& Lamp : Lamps)
	{
		bSwitch = bSwitch && Lamp.SwitchLux >= 15.0f && Lamp.SwitchLux <= 60.0f;
		const bool bLed = Lamp.Colour != ELampColour::Sodium;
		if (bLed) { bColours = bColours && (Lamp.Kind == ELampKind::Arm) == (Lamp.Colour == ELampColour::NeutralLed); }
	}
	Check(bSwitch, "le fotocellule fra 15 e 60 lux");
	Check(bColours, "LED neutro sulle principali, caldo nei quartieri");
	int32_t Led = 0, Cells = 0;
	for (int Row = 0; Row < 60; ++Row)
	{
		for (int Column = 0; Column < 60; ++Column)
		{
			FLampStyle Style;
			Style.Kind = ELampKind::Post;
			Led += StreetDetail::ChooseColour(40.0 + Row * 0.02, 8.0 + Column * 0.02, Style, Settings) != ELampColour::Sodium ? 1 : 0;
			++Cells;
		}
	}
	const double LedShare = static_cast<double>(Led) / Cells;
	Check(LedShare > 0.71 && LedShare < 0.79, "tre comuni su quattro sono passati ai LED", Fmt("%.3f", LedShare));
	Check(StreetDetail::CircuitSwitchLux(45.0001, 7.6501, 1) == StreetDetail::CircuitSwitchLux(45.0002, 7.6503, 1),
		"due lampioni vicini, stesso circuito: si accendono insieme");

	std::vector<FStreetLamp> Again;
	PlaceTileLamps(Tile, Settings, Again);
	bool bSame = Again.size() == Lamps.size();
	for (size_t Index = 0; bSame && Index < Lamps.size(); ++Index)
	{
		bSame = Again[Index].LatDeg == Lamps[Index].LatDeg && Again[Index].LonDeg == Lamps[Index].LonDeg && Again[Index].SwitchLux == Lamps[Index].SwitchLux;
	}
	Check(bSame, "deterministico: due volte, gli stessi lampioni");

	// Il confine fra due tile: la stessa via nelle due, ognuna con il suo buffer.
	{
		const FTileKey EastKey{ TorinoKey.Level, TorinoKey.X + 1, TorinoKey.Y };
		const int32_t Extent = Tiles::VectorTileExtent;
		FTestLine West{ ERoadClass::Residential, 0, 0, 55, { { UX(1000.0), UY(1000.0) }, { Extent + 2000, UY(1000.0) } } };
		FTestLine East{ ERoadClass::Residential, 0, 0, 55, { { -2000, UY(1000.0) }, { UX(1200.0), UY(1000.0) } } };
		std::vector<FStreetLamp> A, B;
		PlaceTileLamps(MakeTile(TorinoKey, { West }), Settings, A);
		PlaceTileLamps(MakeTile(EastKey, { East }), Settings, B);
		std::vector<double> Xs;
		for (const FStreetLamp& Lamp : A) { double X = 0.0, Y = 0.0; AnchorMetres(Lamp, X, Y); Xs.push_back(X); }
		const FTileMetric EastMetric = FTileMetric::For(EastKey, Extent);
		for (const FStreetLamp& Lamp : B) { Xs.push_back(Metric().WidthM() + (Lamp.AnchorLonDeg - EastMetric.Bounds.West) / EastMetric.Bounds.SpanLon() * EastMetric.WidthM()); }
		std::sort(Xs.begin(), Xs.end());
		double Low = 1e9, High = 0.0;
		bool bBothSides = false;
		for (size_t Index = 1; Index < Xs.size(); ++Index)
		{
			Low = std::min(Low, Xs[Index] - Xs[Index - 1]);
			High = std::max(High, Xs[Index] - Xs[Index - 1]);
			bBothSides = bBothSides || (Xs[Index - 1] < Metric().WidthM() && Xs[Index] > Metric().WidthM());
		}
		Check(bBothSides && Low >= 13.0 - 1e-6 && High <= 39.0 + 1e-6, "sul confine fra due tile: nessun doppione, buco al piu' di un passo e mezzo",
			Fmt("passo fra %.1f e %.1f m (26 m)", Low, High));
	}
}

// ===========================================================================
//  14. I lampioni: quando si accendono, le celle, le forme, i materiali
// ===========================================================================

#define GEO_STREETLIGHT_SPRITE(...) __VA_ARGS__
#define GEO_STREETLIGHT_GLOW(...)
#define GEO_STREETLIGHT_LAMP(...)
static HlslShim::float3 RunLampSprite(const HlslShim::float3& Pos, const HlslShim::float3& Cam, const HlslShim::float4& Corner,
                                      const HlslShim::float4& View, float UuPerMetre)
{
	using namespace HlslShim;
#include "Roads/StreetLightShader.inl"
}
#undef GEO_STREETLIGHT_SPRITE
#undef GEO_STREETLIGHT_GLOW
#undef GEO_STREETLIGHT_LAMP

#define GEO_STREETLIGHT_SPRITE(...)
#define GEO_STREETLIGHT_GLOW(...) __VA_ARGS__
#define GEO_STREETLIGHT_LAMP(...)
static HlslShim::float3 RunLampGlow(const HlslShim::float4& Corner, const HlslShim::float3& Pos, const HlslShim::float3& Cam,
                                    const HlslShim::float3& Tint, float Switch, const HlslShim::float4& Lights,
                                    const HlslShim::float4& View, float UuPerMetre)
{
	using namespace HlslShim;
#include "Roads/StreetLightShader.inl"
}
#undef GEO_STREETLIGHT_SPRITE
#undef GEO_STREETLIGHT_GLOW
#undef GEO_STREETLIGHT_LAMP

#define GEO_STREETLIGHT_SPRITE(...)
#define GEO_STREETLIGHT_GLOW(...)
#define GEO_STREETLIGHT_LAMP(...) __VA_ARGS__
static HlslShim::float3 RunLampGlass(const HlslShim::float4& Paint, const HlslShim::float3& Tint, float Switch, const HlslShim::float4& Lights)
{
	using namespace HlslShim;
#include "Roads/StreetLightShader.inl"
}
#undef GEO_STREETLIGHT_SPRITE
#undef GEO_STREETLIGHT_GLOW
#undef GEO_STREETLIGHT_LAMP

static void TestStreetLightTimesAndShapes()
{
	using namespace LightsTest;
	Section("14. I lampioni: quando si accendono, le celle, le forme, i materiali");

	// La luce che vede la fotocellula.
	const double Noon = AmbientLux(60.0, 0.0), Sunset = AmbientLux(0.0, 0.0), Dusk = AmbientLux(-3.0, 0.0);
	Check(Noon > 50000.0 && Sunset > 300.0 && Sunset < 600.0 && Dusk > 30.0 && Dusk < 70.0,
		"la luce: giorno pieno, tramonto ~440 lux, a -3 gradi ~50", Fmt("%.0f, %.0f, %.0f lux", Noon, Sunset, Dusk));
	Check(std::fabs(AmbientLux(-3.0, 1.0) / Dusk - 0.25) < 1e-9, "con il cielo coperto un quarto della luce");
	Check(LampOnLevel(10.0, 30.0, false) == 1.0 && LampOnLevel(40.0, 30.0, false) == 0.0
		&& LampOnLevel(32.0, 30.0, false) > 0.0 && LampOnLevel(32.0, 30.0, false) < 1.0,
		"acceso sotto la soglia, spento sopra il 15% in piu', una rampa in mezzo");
	Check(LampOnLevel(40.0, 30.0, true) == 1.0, "la mattina si spegne a una luce piu' alta (isteresi)");

	// Torino, 21 giugno 2026: il tramonto, poi i circuiti che si accendono.
	const double LatDeg = 45.07, LonDeg = 7.68;
	const double Start = Sky::JulianDay(2026, 6, 21.0) + 18.0 / 24.0;      // 18:00 UT, 20:00 locali
	double SunsetJd = 0.0, On60 = 0.0, On15 = 0.0, OnCloudy = 0.0;
	for (int Minute = 0; Minute < 240; ++Minute)
	{
		const double Jd = Start + Minute / 1440.0;
		const double Altitude = SunAltitudeDeg(Jd, LatDeg, LonDeg);
		if (SunsetJd == 0.0 && Altitude < -0.833) { SunsetJd = Jd; }
		const double Light = AmbientLux(Altitude, 0.0);
		if (On60 == 0.0 && LampOnLevel(Light, 60.0, false) >= 1.0) { On60 = Jd; }
		if (On15 == 0.0 && LampOnLevel(Light, 15.0, false) >= 1.0) { On15 = Jd; }
		if (OnCloudy == 0.0 && LampOnLevel(AmbientLux(Altitude, 1.0), 60.0, false) >= 1.0) { OnCloudy = Jd; }
	}
	const double After60 = (On60 - SunsetJd) * 1440.0, After15 = (On15 - SunsetJd) * 1440.0, AfterCloudy = (OnCloudy - SunsetJd) * 1440.0;
	Check(SunsetJd > 0.0 && After60 > 5.0 && After60 < 25.0, "il circuito piu' sensibile (60 lux) si accende 5-25 minuti dopo il tramonto",
		Fmt("%.0f minuti (tramonto alle %.2f UT)", After60, std::fmod(SunsetJd + 0.5, 1.0) * 24.0));
	Check(After15 > After60 && After15 < 45.0, "il meno sensibile (15 lux) poco dopo", Fmt("%.0f minuti", After15));
	Check(AfterCloudy < After60, "con il cielo coperto prima", Fmt("%.0f minuti", AfterCloudy));
	Check(IsMorning(Sky::JulianDay(2026, 6, 21.0) + 4.0 / 24.0, LatDeg, LonDeg)
		&& !IsMorning(Sky::JulianDay(2026, 6, 21.0) + 18.0 / 24.0, LatDeg, LonDeg), "la mattina il sole sale, la sera scende");

	Check(NightDimming(21.0, 0.3) == 1.0 && std::fabs(NightDimming(2.0, 0.3) - 0.7) < 1e-9
		&& std::fabs(NightDimming(0.0, 0.3) - 0.85) < 1e-9 && std::fabs(NightDimming(5.0, 0.3) - 0.85) < 1e-9 && NightDimming(12.0, 0.3) == 1.0,
		"la riduzione del flusso: piena la sera, -30% dalle 00:30 alle 04:30, rampe di un'ora");

	const FSpotLight Spot = SpotFor(8.0, 0.7, 0.2, 1.0, 1.0);
	Check(std::fabs(Spot.Candela / 64.0 - 0.14) < 1e-9 && Spot.RadiusM == 36.0, "la luce vera: al piede (8 m sotto) la luce della scena per l'Output");
	Check(SpotFor(8.0, 0.7, 0.2, 0.0, 1.0).Candela == 0.0, "spento: zero");
	Check(std::fabs(FootLux(10.0, 1.0) - 0.2) < 1e-12, "al piede 1/50 del sole del livello (venti lune piene)");

	// Le luci vere: i piu' vicini, con il peso che scende in fondo.
	std::vector<FLightCandidate> Candidates;
	for (int Index = 0; Index < 200; ++Index) { Candidates.push_back(FLightCandidate{ static_cast<uint64_t>(1000 + Index), 500.0 - 2.0 * Index }); }
	std::vector<FLightChoice> Choice;
	SelectLights(Candidates, 32, Choice);
	bool bNearest = Choice.size() == 32;
	for (const FLightChoice& Item : Choice) { bNearest = bNearest && Candidates[Item.Candidate].DistanceM <= 500.0 - 2.0 * 168.0; }
	Check(bNearest && Choice.front().Id == 1199 && Choice.front().Weight == 1.0 && Choice.back().Weight < 0.15,
		"32 luci vere sui lampioni piu' vicini, sfumate in fondo alla lista", Fmt("ultimo peso %.3f", Choice.back().Weight));

	// Le celle: i quattro quarti (livello 14) della tile dividono i lampioni, sul terreno.
	{
		const std::vector<FTestLine> Lines = Town();
		std::vector<FStreetLamp> Lamps;
		PlaceTileLamps(MakeTile(TorinoKey, Lines), FStreetLightSettings{}, Lamps);
		auto Flat = std::make_shared<Tiles::FHeightTile>();
		Flat->Key = TorinoKey;
		Flat->Heights.assign(static_cast<size_t>(Tiles::TilePosts) * Tiles::TilePosts, 250.0f);
		Flat->MinHeight = Flat->MaxHeight = 250.0f;
		Cells::FGroundSurfaces Ground;
		Ground.Add(Flat, 1);
		size_t Total = 0;
		bool bHeights = true, bUnique = true;
		std::vector<uint64_t> Ids;
		for (uint32_t Y = 0; Y < 2; ++Y)
		{
			for (uint32_t X = 0; X < 2; ++X)
			{
				const FTileKey CellKey{ 14, TorinoKey.X * 2 + X, TorinoKey.Y * 2 + Y };
				std::vector<FLampInstance> Instances;
				BuildCellLamps(Tiles::GetTileBounds(CellKey.Level, CellKey.X, CellKey.Y), Lamps, Ground, Instances);
				Total += Instances.size();
				for (const FLampInstance& Instance : Instances)
				{
					bHeights = bHeights && std::fabs(Instance.Base[2] - 250.0f) < 1.0f && std::fabs(Instance.Light[2] - Instance.Base[2] - Instance.HeightM) < 1e-3f
						&& std::fabs(std::hypot(Instance.Light[0] - Instance.Base[0], Instance.Light[1] - Instance.Base[1]) - 0.7f * Instance.Scale) < 1e-3f;
					Ids.push_back(Instance.Id);
				}
			}
		}
		std::sort(Ids.begin(), Ids.end());
		bUnique = std::adjacent_find(Ids.begin(), Ids.end()) == Ids.end();
		Check(Total == Lamps.size() && !Lamps.empty(), "le quattro celle della tile: ogni lampione in una sola",
			Fmt("%.0f lampioni, %.0f nelle celle", static_cast<double>(Lamps.size()), static_cast<double>(Total)));
		Check(bHeights, "sul terreno disegnato; la luce in cima al palo, sporgente di 0,7 m");
		Check(bUnique, "ogni lampione ha il suo numero (le luci vere lo seguono)");
	}

	// Le forme.
	bool bShapes = true;
	for (int32_t Kind = 0; Kind < LampKindCount; ++Kind)
	{
		Vegetation::FTreeMesh Near, Far;
		BuildLampMesh(static_cast<ELampKind>(Kind), false, Near);
		BuildLampMesh(static_cast<ELampKind>(Kind), true, Far);
		const FLampShape Shape = LampShape(static_cast<ELampKind>(Kind));
		bool bIndices = !Near.Indices.empty() && Near.Indices.size() % 3 == 0;
		for (const uint32_t Index : Near.Indices) { bIndices = bIndices && Index < Near.VertexCount(); }
		// Il vetro: vertici con A = 255, attorno al punto della luce.
		double GlassBelow = 1e9, Top = -1e9;
		bool bGlassNearLight = false;
		for (size_t Vertex = 0; Vertex < Near.VertexCount(); ++Vertex)
		{
			Top = std::max(Top, static_cast<double>(Near.Positions[Vertex * 3 + 2]));
			if (Near.Colours[Vertex * 4 + 3] != 255) { continue; }
			const double Dx = Near.Positions[Vertex * 3] - Shape.ReachM, Dz = Near.Positions[Vertex * 3 + 2] - Shape.LightHeightM;
			bGlassNearLight = bGlassNearLight || (std::fabs(Dx) < 0.4 && std::fabs(Dz) < 0.3);
			GlassBelow = std::min(GlassBelow, static_cast<double>(Near.Positions[Vertex * 3 + 2]));
		}
		const bool bShape = bIndices && bGlassNearLight && Far.TriangleCount() < Near.TriangleCount() && Top < Shape.LightHeightM + 0.6
			&& GlassBelow > Shape.LightHeightM - 0.3;
		std::printf("  %-9s %4zu triangoli (lontano %zu), luce a %.2f m, sporge %.1f m\n", LampKindName(static_cast<ELampKind>(Kind)),
			Near.TriangleCount(), Far.TriangleCount(), Shape.LightHeightM, Shape.ReachM);
		bShapes = bShapes && bShape;
	}
	Check(bShapes, "le tre forme: indici validi, il vetro attorno alla luce, il LOD lontano piu' leggero");
	Vegetation::FTreeMesh Quad;
	BuildSpriteQuad(Quad);
	Check(Quad.VertexCount() == 4 && Quad.TriangleCount() == 2 && Quad.Colours[4] == 255 && Quad.Colours[9] == 255 && Quad.Colours[0] == 0,
		"il quadrato dei punti luce: quattro angoli scritti nel colore");

	// Il materiale: il punto si apre verso la camera.
	using HlslShim::float3;
	using HlslShim::float4;
	const float Uu = 100.0f;
	const float4 View(0.001, 2.5, 0.8, 2.0);
	const float3 Lamp(0.0, 0.0, 0.0);
	auto Corners = [&](const float3& Cam, float3 Out[4])
	{
		const float4 C[4] = { float4(0, 0, 0, 0), float4(1, 0, 0, 0), float4(1, 1, 0, 0), float4(0, 1, 0, 0) };
		for (int Index = 0; Index < 4; ++Index) { Out[Index] = RunLampSprite(Lamp, Cam, C[Index], View, Uu); }
	};
	auto Distance = [](const float3& A, const float3& B) { return std::sqrt((A.x - B.x) * (A.x - B.x) + (A.y - B.y) * (A.y - B.y) + (A.z - B.z) * (A.z - B.z)); };
	{
		float3 Far[4], Close[4];
		Corners(float3(0.0, 100000.0, 30000.0), Far);       // a ~1 km
		Corners(float3(0.0, 2000.0, 0.0), Close);            // a 20 m
		const double FarM = std::sqrt(1000.0 * 1000.0 + 300.0 * 300.0);
		const double FarSide = Distance(Far[0], Far[1]) / Uu, CloseSide = Distance(Close[0], Close[1]) / Uu;
		const double Expected = FarM * 0.001 * 2.5;
		// Il quadrato e' perpendicolare alla vista (meno l'avvicinamento, che e' lungo la vista).
		const double Dx = 0.0, Dy = 100000.0 / (FarM * Uu), Dz = 30000.0 / (FarM * Uu);
		const float3 Mid((Far[0].x + Far[2].x) * 0.5, (Far[0].y + Far[2].y) * 0.5, (Far[0].z + Far[2].z) * 0.5);
		const float3 Edge(Far[1].x - Far[0].x, Far[1].y - Far[0].y, Far[1].z - Far[0].z);
		const double Facing = (Edge.x * Dx + Edge.y * Dy + Edge.z * Dz) / std::max(1e-6, static_cast<double>(Distance(Far[0], Far[1])));
		const double Pull = (Mid.x * Dx + Mid.y * Dy + Mid.z * Dz) / Uu;
		Check(std::fabs(FarSide - Expected) < 0.01 * Expected && std::fabs(CloseSide - 0.8) < 1e-3,
			"il punto: 2,5 pixel da lontano, 80 cm da vicino", Fmt("%.2f m a 1 km, %.2f m a 20 m", FarSide, CloseSide));
		Check(std::fabs(Facing) < 1e-4 && std::fabs(Pull - 2.0 * Expected) < 0.01 * Expected, "girato verso la camera, avvicinato di due punti",
			Fmt("avvicinato %.2f m", Pull));
		float3 Above[4];
		Corners(float3(0.0, 0.0, 50000.0), Above);
		const double AboveSide = Distance(Above[0], Above[1]) / Uu;
		Check(std::isfinite(AboveSide) && std::fabs(AboveSide - 500.0 * 0.0025) < 0.01, "anche visto da sopra (niente divisioni per zero)",
			Fmt("%.2f m", AboveSide));
	}

	// Il bagliore: acceso con la stessa formula di LampOnLevel.
	{
		const float3 Tint(1.0, 0.8, 0.5);
		const float3 Cam(0.0, 30000.0, 0.0);                  // a 300 m
		const float4 Centre(0.5, 0.5, 0, 0), Edge(0.9, 0.5, 0, 0);
		auto Glow = [&](const float4& Corner, double Ambient, double Factor, const float3& Camera)
		{
			return RunLampGlow(Corner, Lamp, Camera, Tint, 30.0f, float4(Ambient, Factor, 0.2, 1.0), View, Uu).x;
		};
		const double Night = Glow(Centre, 0.0, 1.0, Cam);
		Check(Night > 0.0 && Glow(Edge, 0.0, 1.0, Cam) < Night && Glow(Edge, 0.0, 1.0, Cam) > 0.0, "di notte: nucleo e alone");
		Check(Glow(Centre, 100000.0, 1.0, Cam) == 0.0, "di giorno: niente");
		bool bSameRamp = true;
		for (const double Ambient : { 20.0, 31.0, 33.0, 34.0, 36.0 })
		{
			bSameRamp = bSameRamp && std::fabs(Glow(Centre, Ambient, 1.0, Cam) / Night - LampOnLevel(Ambient, 30.0, false)) < 1e-4;
			bSameRamp = bSameRamp && std::fabs(Glow(Centre, Ambient * 1.6, 1.6, Cam) / Night - LampOnLevel(Ambient * 1.6, 30.0, true)) < 1e-4;
		}
		Check(bSameRamp, "il materiale si accende con la formula del C++ (sera e mattina)");
		const double FarGlow = Glow(Centre, 0.0, 1.0, float3(0.0, 500000.0, 0.0));
		Check(FarGlow > 0.0 && FarGlow < 0.3 * Night, "a 5 km piu' debole per pixel, ma c'e'", Fmt("%.3f del vicino", FarGlow / Night));
		Check(Glow(Centre, 0.0, 1.0, float3(0.0, 300.0, 0.0)) == 0.0, "a 3 m sparisce: c'e' il vetro del palo");

		const float4 Glass(0.9, 0.9, 0.9, 1.0), Metal(0.4, 0.4, 0.4, 0.0);
		const double Lit = RunLampGlass(Glass, Tint, 30.0f, float4(0.0, 1.0, 0.2, 0.7)).x;
		Check(std::fabs(Lit - 1.0 * 0.2 * 30.0 * 0.7) < 1e-4 && RunLampGlass(Metal, Tint, 30.0f, float4(0.0, 1.0, 0.2, 0.7)).x == 0.0
			&& RunLampGlass(Glass, Tint, 30.0f, float4(100000.0, 1.0, 0.2, 0.7)).x == 0.0, "il vetro del palo: acceso di notte, il metallo mai");
	}

	// Il costo: una tile di citta' fitta.
	{
		std::vector<FTestLine> Grid;
		for (int Index = 0; Index < 40; ++Index)
		{
			Grid.push_back(Line(ERoadClass::Residential, 5.5, 0.0, 30.0 + Index * 60.0, 1730.0, 30.0 + Index * 60.0, 0, 0, 20));
		}
		for (int Index = 0; Index < 28; ++Index)
		{
			Grid.push_back(Line(ERoadClass::Residential, 5.5, 30.0 + Index * 60.0, 0.0, 30.0 + Index * 60.0, 2440.0, 0, 0, 20));
		}
		const FVectorTile Dense = MakeTile(TorinoKey, Grid);
		std::vector<FStreetLamp> Lamps;
		const auto Started = std::chrono::steady_clock::now();
		PlaceTileLamps(Dense, FStreetLightSettings{}, Lamps);
		const double Ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - Started).count();
		std::printf("  citta' fitta: %zu lampioni in %.1f ms\n", Lamps.size(), Ms);
		Check(Lamps.size() > 4000 && Ms < 400.0, "una tile di citta' (68 vie, 1360 pezzi): migliaia di lampioni in meno di 0,4 s",
			Fmt("%.0f in %.1f ms", static_cast<double>(Lamps.size()), Ms));
	}
}

// ===========================================================================
int main(int ArgCount, char** Arguments)
{
	std::printf("=====================================================\n");
	std::printf(" GeoRoads -- strade sul terreno (Fase 8), senza Unreal\n");
	std::printf("=====================================================\n");

	const std::string FixturePath = (ArgCount > 1) ? Arguments[1]
		: "Plugins/GeoWorld/Source/GeoTiles/TestData/vector_fixture.gvt";

	TestFixture(FixturePath);
	TestBrokenFiles();
	TestChoice();
	TestWidths();
	TestComposition();
	TestChildMatchesParent();
	TestMipsAndCost();
	TestSurfaceSampler();
	TestRoadOnTerrain();
	TestBridge();
	TestSidewalksAndJunctions();
	TestAtlasAndCost();
	TestStreetLightPlacement();
	TestStreetLightTimesAndShapes();

	std::printf("\n=====================================================\n");
	std::printf(" RISULTATO: %d passati, %d falliti, %d saltati\n", GPassed, GFailed, GSkipped);
	std::printf("=====================================================\n");
	return GFailed == 0 ? 0 : 1;
}

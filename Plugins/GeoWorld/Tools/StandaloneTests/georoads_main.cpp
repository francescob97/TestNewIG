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
#include "Roads/RoadRasterizer.h"
#include "Tiles/ImageTileFormat.h"
#include "Tiles/VectorTileFormat.h"

#include <chrono>
#include <cmath>
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
		Out.push_back(0);
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

	std::printf("\n=====================================================\n");
	std::printf(" RISULTATO: %d passati, %d falliti, %d saltati\n", GPassed, GFailed, GSkipped);
	std::printf("=====================================================\n");
	return GFailed == 0 ? 0 : 1;
}

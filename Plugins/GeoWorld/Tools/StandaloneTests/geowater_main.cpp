// =============================================================================
//  Test dell'acqua (Fase 9), senza Unreal: il formato .gwa letto da un file
//  scritto dalla pipeline vera, lo scavo, la mesh, il raster, le onde dal
//  vento, le texture generate.
//
//  La decompressione zlib qui la fa la zlib di sistema; in Unreal la fa
//  FCompression. Lo strato puro riceve il payload gia' decompresso in tutti e
//  due i casi.
// =============================================================================
#include "Mesh/TileMesh.h"
#include "Tiles/WaterTileFormat.h"
#include "Water/WaterMesh.h"
#include "Water/WaterTextures.h"
#include "Water/WaveSpectrum.h"

#include <zlib.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace GeoWorld;
using namespace GeoWorld::Water;
using Tiles::FTileKey;
using Tiles::FWaterTile;

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

static const FTileKey FixtureKey{ 14, 17085, 4093 };

/** Legge un .gwa come farebbe il runtime: header, zlib, payload. */
static bool LoadWaterTile(const std::vector<uint8_t>& Bytes, const FTileKey& Key, FWaterTile& Out, std::string& Error)
{
	Tiles::FWaterTileHeader Header;
	if (!Tiles::ParseWaterTileHeader(Bytes.data(), Bytes.size(), Key, Header, Error)) { return false; }
	std::vector<uint8_t> Plain(Header.ExpectedPlainBytes());
	uLongf PlainSize = static_cast<uLongf>(Plain.size());
	if (uncompress(Plain.data(), &PlainSize, Bytes.data() + Tiles::WaterTileHeaderBytes, Header.PayloadBytes) != Z_OK)
	{
		Error = "zlib";
		return false;
	}
	return Tiles::DecodeWaterTilePayload(Header, Plain.data(), PlainSize, Out, Error);
}

static Tiles::FHeightTile FlatTerrain(const FTileKey& Key, float Height)
{
	Tiles::FHeightTile Tile;
	Tile.Key = Key;
	Tile.Heights.assign(static_cast<size_t>(Tiles::TilePosts) * Tiles::TilePosts, Height);
	Tile.MinHeight = Tile.MaxHeight = Height;
	return Tile;
}

// ===========================================================================
static bool TestFixture(const std::string& Path, FWaterTile& Fixture)
{
	Section("1. Il formato .gwa, letto da un file scritto dalla pipeline");

	std::ifstream File(Path, std::ios::binary);
	if (!File)
	{
		std::printf("  [skip] %s non trovato: lancia dalla radice del repository\n", Path.c_str());
		++GSkipped;
		return false;
	}
	const std::vector<uint8_t> Bytes((std::istreambuf_iterator<char>(File)), std::istreambuf_iterator<char>());

	std::string Error;
	const bool bLoaded = LoadWaterTile(Bytes, FixtureKey, Fixture, Error);
	Check(bLoaded && Fixture.IsValid(), "header, zlib e payload", Error);
	if (!bLoaded) { return false; }

	Check(Fixture.HasFlow() && (Fixture.Flags & Tiles::WaterTileLake) && (Fixture.Flags & Tiles::WaterTileRiver),
		"flag: lago, fiume, corrente");
	Check(std::fabs(Fixture.SurfaceAt(10, 10) - 230.4f) < 1e-3f && std::isnan(Fixture.SurfaceAt(100, 10)),
		"superficie: 230,4 m a ovest, niente a est", Fmt("%.2f", Fixture.SurfaceAt(10, 10)));
	Check(std::isfinite(Fixture.BedAt(0, 5)) && Fixture.BedAt(0, 5) < Fixture.BedAt(60, 5) && std::isnan(Fixture.BedAt(64, 5)),
		"fondale: piu' profondo lontano dalla riva, assente a terra");
	Check(Fixture.SdfPixels(127, 50) > 0.0f && Fixture.SdfPixels(128, 50) < 0.0f
		&& std::fabs(Fixture.SdfPixels(127, 50) - 0.5f) < 0.07f,
		"SDF: la riva fra le colonne 127 e 128", Fmt("%.3f / %.3f", Fixture.SdfPixels(127, 50), Fixture.SdfPixels(128, 50)));
	Check(Fixture.FlowU[static_cast<size_t>(125) * Tiles::WaterRaster + 30] == 100 && Fixture.FlowU[30] == 0,
		"corrente: 1 m/s verso est sulle righe del fiume");
	Check(Fixture.Wave[30] == 51 && Fixture.Wave[200] == 0, "onde: 0,2 sull'acqua, 0 a terra");

	const double Mpp = Tiles::WaterMetresPerPixel(FixtureKey.Level, FixtureKey.Y);
	Check(std::fabs(Mpp - 4.01) < 0.02, "metri per pixel come la pipeline (4,01 su Torino al livello 14)", Fmt("%.3f", Mpp));
	return true;
}

static void TestBrokenFiles()
{
	Section("2. File rotti, indice, geoide");

	std::vector<uint8_t> Bytes(64, 0);
	std::memcpy(Bytes.data(), "GWVT", 4);
	Tiles::FWaterTileHeader Header;
	std::string Error;
	Check(!Tiles::ParseWaterTileHeader(Bytes.data(), Bytes.size(), FixtureKey, Header, Error), "magic sbagliato", Error);

	std::memcpy(Bytes.data(), "GWWA", 4);
	Bytes[4] = 1;                       // versione
	Bytes[20] = 129; Bytes[22] = 0; Bytes[23] = 1;   // posts 129, raster 256
	Bytes[8] = 14;
	Check(!Tiles::ParseWaterTileHeader(Bytes.data(), Bytes.size(), FixtureKey, Header, Error), "chiave diversa dal percorso", Error);

	// Un indice di due voci.
	std::vector<uint8_t> Index(16 + 2 * 16, 0);
	const uint32_t Words[] = { 0x49575747u, 1u, 14u, 2u };
	std::memcpy(Index.data(), &Words[0], 4);
	std::memcpy(Index.data() + 4, &Words[1], 2);
	std::memcpy(Index.data() + 8, &Words[2], 4);
	std::memcpy(Index.data() + 12, &Words[3], 4);
	const uint32_t Record[] = { 17085, 4093 };
	std::memcpy(Index.data() + 16, Record, 8);
	Index[16 + 8] = Tiles::WaterTileSea | Tiles::WaterTileFlow;
	std::vector<Tiles::FWaterIndexEntry> Entries;
	Error.clear();
	const bool bIndex = Tiles::ParseWaterIndex(Index.data(), Index.size(), 14, Entries, Error);
	Check(bIndex && Entries.size() == 2
		&& Entries[0].X == 17085 && Entries[0].Flags == (Tiles::WaterTileSea | Tiles::WaterTileFlow),
		"indice: voci e flag", Error);
	Check(!Tiles::ParseWaterIndex(Index.data(), Index.size(), 13, Entries, Error), "indice di un altro livello rifiutato");

	// Un geoide 3 x 2: 40, 41, 42 / 43, 44, 45.
	std::vector<uint8_t> Geoid(40 + 6 * 4, 0);
	const uint32_t GeoidMagic = Tiles::GeoidMagic;
	std::memcpy(Geoid.data(), &GeoidMagic, 4);
	Geoid[4] = 1;
	const uint32_t Columns = 3, Rows = 2;
	std::memcpy(Geoid.data() + 8, &Columns, 4);
	std::memcpy(Geoid.data() + 12, &Rows, 4);
	const double Origin[3] = { 7.0, 46.0, 0.5 };
	std::memcpy(Geoid.data() + 16, Origin, 24);
	const float Values[6] = { 40, 41, 42, 43, 44, 45 };
	std::memcpy(Geoid.data() + 40, Values, sizeof(Values));
	Tiles::FGeoidGrid Grid;
	double N = 0.0;
	const bool bGeoid = Tiles::ParseGeoid(Geoid.data(), Geoid.size(), Grid, Error) && Grid.Sample(7.25, 45.75, N);
	Check(bGeoid && std::fabs(N - 42.0) < 1e-9,
		"geoide: bilineare fra i nodi", Fmt("N = %.3f", N));
	Check(!Grid.Sample(6.0, 45.75, N), "geoide: fuori dalla griglia, niente numero");
}

static void TestViewAndCarve(const FWaterTile& Fixture)
{
	Section("3. Vista, scavo del terreno");

	const FWaterView Same = FWaterView::Make(Fixture, FixtureKey);
	Check(Same.IsValid() && Same.Scale == 1.0, "la tile dello stesso livello: finestra intera");
	Check(std::fabs(Same.SurfaceAtPost(10, 10) - 230.4f) < 1e-3f && std::isnan(Same.SurfaceAtPost(120, 10)),
		"superficie sui post");

	// Una figlia (livello 15) del quarto nord-ovest: la riva (U 0,5 del padre)
	// e' fuori; il quarto nord-est della figlia di destra la contiene a U = 0.
	const FTileKey ChildWest{ 15, FixtureKey.X * 2, FixtureKey.Y * 2 };
	const FTileKey ChildEast{ 15, FixtureKey.X * 2 + 1, FixtureKey.Y * 2 };
	const FWaterView West = FWaterView::Make(Fixture, ChildWest);
	const FWaterView East = FWaterView::Make(Fixture, ChildEast);
	Check(West.IsValid() && West.Scale == 0.5 && East.OffsetU == 0.5, "una figlia: mezza finestra, l'offset giusto");
	Check(std::fabs(West.SurfaceAtPost(128, 0) - 230.4f) < 1e-3f, "la superficie della figlia viene dal padre");
	// Distanza dalla riva in pixel della FIGLIA: il doppio di quelli del padre.
	const float ParentSdf = Same.SdfPixelsAt(0.4, 0.2);
	const float ChildSdf = West.SdfPixelsAt(0.8, 0.4);
	Check(std::fabs(ChildSdf - 2.0f * ParentSdf) < 0.05f, "la distanza si riscala nei pixel della figlia",
		Fmt("%.2f px contro %.2f", ChildSdf, ParentSdf));
	Check(!FWaterView::Make(Fixture, FTileKey{ 15, 1, 1 }).IsValid(), "una tile che non discende: vista non valida");

	Tiles::FHeightTile Terrain = FlatTerrain(FixtureKey, 240.0f);
	const int32_t Carved = CarveHeights(Terrain, Same);
	const float AtWest = Terrain.GetHeight(5, 40);
	Check(Carved > 7000 && AtWest < 230.4f && AtWest == Fixture.BedAt(5, 40), "lo scavo porta il terreno al fondale",
		Fmt("%.0f post, %.2f m", Carved, AtWest));
	Check(Terrain.GetHeight(100, 40) == 240.0f, "a terra il terreno non si tocca");
	Check(Terrain.MinHeight < 230.0f && Terrain.MaxHeight == 240.0f, "quote estreme aggiornate (servono all'origine della mesh)");

	Tiles::FHeightTile Low = FlatTerrain(FixtureKey, 100.0f);
	Check(CarveHeights(Low, Same) == 0, "un terreno gia' sotto il fondale non sale");
}

static void TestMesh(const FWaterTile& Fixture)
{
	Section("4. La superficie dell'acqua");

	const FWaterView View = FWaterView::Make(Fixture, FixtureKey);
	Tiles::FHeightTile Terrain = FlatTerrain(FixtureKey, 240.0f);
	CarveHeights(Terrain, View);

	FWaterMeshParameters Parameters;
	Parameters.Step = 2;
	Mesh::FTileMeshData Water;
	BuildWaterMesh(Terrain, View, Parameters, Water);

	Mesh::FTileMeshParameters TerrainParameters;
	TerrainParameters.Step = 2;
	Mesh::FTileMeshData TerrainMesh;
	Mesh::BuildTileMesh(Terrain, TerrainParameters, TerrainMesh);
	Check(Water.TriangleCount > 1000 && Water.Origin.HeightM == TerrainMesh.Origin.HeightM
		&& Water.Origin.LatRad == TerrainMesh.Origin.LatRad, "stessa origine della mesh del terreno scavato",
		Fmt("%.0f triangoli", Water.TriangleCount));

	// Le quote: i vertici di sopra stanno a 230,4 m, quelli delle gonne 2 m sotto.
	const Core::FEcef OriginEcef = Core::GeodeticToEcef(Water.Origin, Core::WGS84);
	const Core::FMat3 ToEcef = Mesh::Detail::MakeLocalNeu(Water.Origin).Transposed();
	double MaxU = 0.0, HighestTop = -1e9, LowestTop = 1e9;
	int Skirt = 0;
	for (size_t Vertex = 0; Vertex < Water.Positions.size() / 3; ++Vertex)
	{
		const Core::FEcef Local{ Water.Positions[Vertex * 3], Water.Positions[Vertex * 3 + 1], Water.Positions[Vertex * 3 + 2] };
		const double Height = Core::EcefToGeodetic(ToEcef.Transform(Local) + OriginEcef, Core::WGS84).HeightM;
		if (Height < 229.0) { ++Skirt; continue; }
		HighestTop = std::max(HighestTop, Height);
		LowestTop = std::min(LowestTop, Height);
		MaxU = std::max(MaxU, static_cast<double>(Water.UVs[Vertex * 2]));
	}
	Check(std::fabs(HighestTop - 230.4) < 0.01 && std::fabs(LowestTop - 230.4) < 0.01, "la superficie e' piana a 230,4 m",
		Fmt("da %.3f a %.3f", LowestTop, HighestTop));
	Check(MaxU <= 0.5 + 1.0 / 64.0 + 1e-6, "solo le celle vicine all'acqua (ovest), al massimo una cella oltre la riva",
		Fmt("U massimo %.4f", MaxU));
	Check(Skirt > 0, "gonne sui bordi della tile", Fmt("%.0f vertici", Skirt));

	// Verso delle facce: come il terreno (con l'inversione, la normale
	// "matematica" punta in giu').
	int Up = 0, Down = 0;
	for (size_t Triangle = 0; Triangle < Water.Indices.size() / 3; ++Triangle)
	{
		const float* A = &Water.Positions[Water.Indices[Triangle * 3] * 3];
		const float* B = &Water.Positions[Water.Indices[Triangle * 3 + 1] * 3];
		const float* C = &Water.Positions[Water.Indices[Triangle * 3 + 2] * 3];
		const double Z = (B[0] - A[0]) * (C[1] - A[1]) - (B[1] - A[1]) * (C[0] - A[0]);
		const double Vertical = std::fabs((B[2] - A[2])) + std::fabs((C[2] - A[2]));
		if (Vertical > 1.0) { continue; }    // le gonne
		(Z > 0.0 ? Up : Down)++;
	}
	Check(Up == 0 && Down > 0, "stesso verso delle facce del terreno", Fmt("%.0f su, %.0f giu'", Up, Down));

	// Una tile di terra (nessuna superficie): niente mesh.
	FWaterTile Dry = Fixture;
	std::fill(Dry.Surface.begin(), Dry.Surface.end(), std::nanf(""));
	Mesh::FTileMeshData None;
	BuildWaterMesh(Terrain, FWaterView::Make(Dry, FixtureKey), Parameters, None);
	Check(None.TriangleCount == 0, "senza superficie, niente mesh");
}

static void TestRaster(const FWaterTile& Fixture)
{
	Section("5. Il raster per la scheda video");

	std::vector<uint8_t> Pixels;
	BuildWaterRasterPixels(FWaterView::Make(Fixture, FixtureKey), Pixels);
	const uint8_t* River = Pixels.data() + (static_cast<size_t>(125) * Tiles::WaterRaster + 30) * 4;
	Check(Pixels.size() == static_cast<size_t>(256 * 256 * 4) && River[2] == Fixture.Sdf[static_cast<size_t>(125) * 256 + 30]
		&& River[1] == 51 && River[0] == 228 && River[3] == 128,
		"BGRA: R distanza, G onde, B corrente est + 128, A corrente sud + 128",
		Fmt("R %.0f G %.0f B %.0f", River[2], River[1], River[0]));

	// La figlia di destra vede la riva al suo bordo ovest, con distanze doppie.
	std::vector<uint8_t> Child;
	BuildWaterRasterPixels(FWaterView::Make(Fixture, FTileKey{ 15, FixtureKey.X * 2 + 1, FixtureKey.Y * 2 }), Child);
	const double Left = (Child[(static_cast<size_t>(10) * 256 + 0) * 4 + 2] - 128.0) / 8.0;
	const double Inside = (Child[(static_cast<size_t>(10) * 256 + 20) * 4 + 2] - 128.0) / 8.0;
	Check(std::fabs(Left + 0.5) < 0.6 && Inside < -15.0, "ricampionata dall'antenato, distanze nei pixel della figlia",
		Fmt("%.2f sul bordo, %.2f a 20 pixel", Left, Inside));
}

static void TestWaves()
{
	Section("6. Dal vento alle onde");

	Check(std::fabs(SignificantWaveHeight(8.0) - 1.37) < 0.02 && std::fabs(PeakWavelength(8.0) - 53.3) < 0.5,
		"Pierson-Moskowitz: 8 m/s -> Hs 1,37 m, picco 53 m",
		Fmt("Hs %.2f m, picco %.1f m", SignificantWaveHeight(8.0), PeakWavelength(8.0)));

	for (const double Speed : { 3.0, 8.0, 15.0 })
	{
		const FWaveSet Waves = BuildWaveSet(FWind{ Speed, 270.0 });
		double SumSquares = 0.0, SteepSum = 0.0, WorstAngle = 0.0;
		bool bDispersion = true;
		for (const FGerstnerWave& Wave : Waves)
		{
			SumSquares += Wave.Amplitude * Wave.Amplitude;
			SteepSum += Wave.Steepness * Wave.Number() * Wave.Amplitude;
			// Vento da ovest: le onde vanno verso est (0, 1).
			WorstAngle = std::max(WorstAngle, std::acos(std::min(1.0, Wave.DirEast)) * 180.0 / 3.14159265358979);
			bDispersion &= std::fabs(Wave.Speed() - std::sqrt(Gravity / Wave.Number())) < 1e-9;
		}
		const double Hs = 4.0 * std::sqrt(SumSquares / 2.0);
		Check(std::fabs(Hs - SignificantWaveHeight(Speed)) < 0.01 * SignificantWaveHeight(Speed) + 1e-6,
			Fmt("%.0f m/s: l'energia delle sei onde da' l'Hs dello spettro", Speed), Fmt("%.3f m", Hs));
		Check(SteepSum <= 0.85 + 1e-9, Fmt("%.0f m/s: niente creste ripiegate (somma Q k A <= 0,85)", Speed), Fmt("%.3f", SteepSum));
		Check(WorstAngle <= 40.0 + 1e-6 && bDispersion, Fmt("%.0f m/s: verso cui soffia, +-40 gradi; w = sqrt(g k)", Speed));
	}
	const FWaveSet Calm = BuildWaveSet(FWind{ 0.0, 0.0 });
	Check(Calm[0].Amplitude == 0.0 && Calm[0].Wavelength >= 1.0, "calma piatta: niente onde, nessuna divisione per zero");
	Check(WhitecapAmount(4.0) == 0.0 && WhitecapAmount(12.0) == 1.0 && WhitecapAmount(8.0) > 0.2,
		"schiuma sulle creste: da 5 a 12 m/s");

	// Il mare che cambia: niente salti, e niente mare che "scorre". Si segue
	// un punto VICINO e uno LONTANO (7 km): se le onde cambiassero lunghezza
	// mentre ci sono, il punto lontano salterebbe di metri a ogni frame.
	FWaveField Field;
	Field.SetTargetWind(FWind{ 8.0, 270.0 });
	Field.SnapToTarget();
	for (int Step = 0; Step < 100; ++Step) { Field.Advance(1.0 / 60.0); }
	Field.SetTargetWind(FWind{ 14.0, 300.0 });
	double WorstNear = 0.0, WorstFar = 0.0;
	bool bSwapOnlyAtZero = true;
	double Near[3], Far[3];
	Field.Displacement(120.0, -40.0, Near[0], Near[1], Near[2]);
	Field.Displacement(5200.0, 4900.0, Far[0], Far[1], Far[2]);
	for (int Step = 0; Step < 1200; ++Step)
	{
		const FWaveSet Before = Field.GetWaves();
		Field.Advance(1.0 / 60.0);
		const FWaveSet& After = Field.GetWaves();
		for (int Index = 0; Index < GerstnerWaveCount; ++Index)
		{
			const bool bChanged = Before[Index].Wavelength != After[Index].Wavelength
				|| Before[Index].DirNorth != After[Index].DirNorth;
			if (bChanged && (Before[Index].Amplitude != 0.0 || After[Index].Amplitude != 0.0)) { bSwapOnlyAtZero = false; }
		}
		double NowNear[3], NowFar[3];
		Field.Displacement(120.0, -40.0, NowNear[0], NowNear[1], NowNear[2]);
		Field.Displacement(5200.0, 4900.0, NowFar[0], NowFar[1], NowFar[2]);
		for (int Axis = 0; Axis < 3; ++Axis)
		{
			WorstNear = std::max(WorstNear, std::fabs(NowNear[Axis] - Near[Axis]));
			WorstFar = std::max(WorstFar, std::fabs(NowFar[Axis] - Far[Axis]));
			Near[Axis] = NowNear[Axis];
			Far[Axis] = NowFar[Axis];
		}
		if (Step == 599)
		{
			Check(Field.GetWind().SpeedMs > 11.0 && Field.GetWind().SpeedMs < 14.0,
				"il vento del mare arriva al nuovo valore in qualche secondo", Fmt("%.1f m/s dopo 10 s", Field.GetWind().SpeedMs));
		}
	}
	Check(bSwapOnlyAtZero, "un'onda cambia lunghezza o direzione SOLO ad ampiezza zero");
	Check(WorstNear < 0.06 && WorstFar < 0.06, "cambio di vento: nessun salto, ne' vicino ne' a 7 km",
		Fmt("%.3f m vicino, %.3f m lontano (al frame, il peggiore)", WorstNear, WorstFar));
	const FWaveSet Wanted = BuildWaveSet(FWind{ 14.0, 300.0 });
	double WorstRatio = 0.0, WorstAmplitude = 0.0;
	for (int Index = 0; Index < GerstnerWaveCount; ++Index)
	{
		WorstRatio = std::max(WorstRatio, std::fabs(Field.GetWaves()[Index].Wavelength / Wanted[Index].Wavelength - 1.0));
		WorstAmplitude = std::max(WorstAmplitude, std::fabs(Field.GetWaves()[Index].Amplitude - Wanted[Index].Amplitude));
	}
	Check(Field.GetSwappingCount() == 0 && WorstRatio < 1e-12 && WorstAmplitude < 0.01,
		"dopo 20 s il mare e' quello del vento nuovo", Fmt("lunghezze %.1e, ampiezze %.3f m", WorstRatio, WorstAmplitude));

	// Un vento che cambia POCO (qui l'1%) non sostituisce le onde: cambia
	// solo l'ampiezza, con dolcezza.
	FWaveField Steady;
	Steady.SetTargetWind(FWind{ 10.0, 200.0 });
	Steady.SnapToTarget();
	const double LengthBefore = Steady.GetWaves()[0].Wavelength;
	Steady.SetTargetWind(FWind{ 10.1, 200.0 });
	for (int Step = 0; Step < 600; ++Step) { Steady.Advance(1.0 / 60.0); }
	Check(Steady.GetWaves()[0].Wavelength == LengthBefore && Steady.GetSwappingCount() == 0
		&& Steady.GetWaves()[0].Amplitude > BuildWaveSet(FWind{ 10.0, 200.0 })[0].Amplitude,
		"un ritocco del vento cambia le ampiezze, non le onde");

	FWaveField Turning;
	Turning.SetTargetWind(FWind{ 8.0, 350.0 });
	Turning.SnapToTarget();
	Turning.SetTargetWind(FWind{ 8.0, 10.0 });
	Turning.Advance(4.0);
	const double From = Turning.GetWind().FromDegrees;
	Check(From > 355.0 || From < 5.0, "da 350 a 10 gradi passando per il nord, non per il sud", Fmt("%.1f gradi", From));

	// Il rebase: il mondo si sposta di (Dn, De); lo stesso punto della Terra
	// ha le coordinate di prima meno lo spostamento, e la superficie li' e'
	// la stessa.
	const double Dn = 8312.5, De = -2640.25;
	double Before[3], After[3];
	Field.Displacement(500.0, 700.0, Before[0], Before[1], Before[2]);
	Field.ApplyOriginShift(Dn, De);
	Field.Displacement(500.0 - Dn, 700.0 - De, After[0], After[1], After[2]);
	Check(std::fabs(Before[2] - After[2]) < 1e-6 && std::fabs(Before[0] - After[0]) < 1e-6,
		"rebase: le onde restano ferme rispetto alla Terra", Fmt("%.2e m", std::fabs(Before[2] - After[2])));
}

static void TestTextures()
{
	Section("7. Le texture generate");

	std::vector<uint8_t> Normal;
	BuildWaterNormalTexture(Normal);
	const int Size = WaterTextureSize;
	auto At = [&Normal, Size](int X, int Y, int Channel) { return static_cast<int>(Normal[(static_cast<size_t>(Y) * Size + X) * 4 + Channel]); };

	double Edge = 0.0, Interior = 0.0, MeanZ = 0.0, WorstLength = 0.0;
	for (int Y = 0; Y < Size; ++Y)
	{
		Edge += std::abs(At(0, Y, 2) - At(Size - 1, Y, 2));
		Interior += std::abs(At(100, Y, 2) - At(101, Y, 2));
	}
	for (int Y = 0; Y < Size; Y += 3)
	{
		for (int X = 0; X < Size; X += 3)
		{
			const double Nx = At(X, Y, 2) / 127.5 - 1.0, Ny = At(X, Y, 1) / 127.5 - 1.0, Nz = At(X, Y, 0) / 127.5 - 1.0;
			WorstLength = std::max(WorstLength, std::fabs(std::sqrt(Nx * Nx + Ny * Ny + Nz * Nz) - 1.0));
			MeanZ += Nz;
		}
	}
	MeanZ /= (Size / 3 + 1) * (Size / 3 + 1);
	Check(Edge <= 2.0 * Interior + Size, "normale: si ripete senza cucitura (il bordo come l'interno)",
		Fmt("bordo %.0f, interno %.0f", Edge, Interior));
	Check(WorstLength < 0.03 && MeanZ > 0.8, "normale: vettori unitari, in media verso l'alto",
		Fmt("scarto %.3f, z medio %.2f", WorstLength, MeanZ));

	std::vector<uint8_t> Foam;
	BuildWaterFoamTexture(Foam);
	double FoamEdge = 0.0, FoamInterior = 0.0, Mean = 0.0;
	for (int Y = 0; Y < Size; ++Y)
	{
		FoamEdge += std::abs(Foam[(static_cast<size_t>(Y) * Size) * 4] - Foam[(static_cast<size_t>(Y) * Size + Size - 1) * 4]);
		FoamInterior += std::abs(Foam[(static_cast<size_t>(Y) * Size + 50) * 4] - Foam[(static_cast<size_t>(Y) * Size + 51) * 4]);
		for (int X = 0; X < Size; ++X) { Mean += Foam[(static_cast<size_t>(Y) * Size + X) * 4]; }
	}
	Mean /= Size * Size * 255.0;
	Check(FoamEdge <= 2.0 * FoamInterior + Size && Mean > 0.1 && Mean < 0.7, "schiuma: si ripete, ne' vuota ne' piena",
		Fmt("media %.2f", Mean));
}

int main(int ArgCount, char** Arguments)
{
	std::printf("=====================================================\n");
	std::printf(" GeoWater -- l'acqua (Fase 9), senza Unreal\n");
	std::printf("=====================================================\n");

	const std::string Path = (ArgCount > 1) ? Arguments[1] : "Plugins/GeoWorld/Source/GeoTiles/TestData/water_fixture.gwa";
	FWaterTile Fixture;
	const bool bFixture = TestFixture(Path, Fixture);
	TestBrokenFiles();
	if (bFixture)
	{
		TestViewAndCarve(Fixture);
		TestMesh(Fixture);
		TestRaster(Fixture);
	}
	TestWaves();
	TestTextures();

	std::printf("\n=====================================================\n");
	std::printf(" RISULTATO: %d passati, %d falliti, %d saltati\n", GPassed, GFailed, GSkipped);
	std::printf("=====================================================\n");
	return GFailed == 0 ? 0 : 1;
}

// =============================================================================
//  Test del generatore di mesh (Fase 5), senza Unreal.
// =============================================================================
#include "Mesh/TileMesh.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

using namespace GeoWorld;
using namespace GeoWorld::Mesh;
using Core::FEcef;
using Core::FGeodetic;
using Core::WGS84;
using Tiles::FHeightTile;
using Tiles::FTileKey;

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

/** Tile di prova: quota funzione continua di lon/lat, cosi' due tile adiacenti
 *  concordano ESATTAMENTE sul bordo condiviso, come garantisce la pipeline. */
static float TerrainHeight(double Lon, double Lat)
{
	return static_cast<float>(400.0
		+ 250.0 * std::sin(Lon * 30.0) * std::cos(Lat * 27.0)
		+ 60.0 * std::sin(Lat * 111.0));
}

static FHeightTile MakeTile(const FTileKey& Key, bool bFlat = false)
{
	FHeightTile Tile;
	Tile.Key = Key;
	Tile.Heights.resize(static_cast<size_t>(Tiles::TilePosts) * Tiles::TilePosts);

	const auto Bounds = Tiles::GetTileBounds(Key.Level, Key.X, Key.Y);
	const double Spacing = Tiles::PostSpacingDeg(Key.Level);

	float Minimum = 1e30f, Maximum = -1e30f;
	for (int32_t J = 0; J < Tiles::TilePosts; ++J)
	{
		for (int32_t I = 0; I < Tiles::TilePosts; ++I)
		{
			const double Lon = Bounds.West + I * Spacing;
			const double Lat = Bounds.North - J * Spacing;
			const float Height = bFlat ? 500.0f : TerrainHeight(Lon, Lat);
			Tile.Heights[static_cast<size_t>(J) * Tiles::TilePosts + I] = Height;
			Minimum = std::min(Minimum, Height);
			Maximum = std::max(Maximum, Height);
		}
	}
	Tile.MinHeight = Minimum;
	Tile.MaxHeight = Maximum;
	return Tile;
}

/** Riporta un vertice dal frame locale della mesh alle coordinate ECEF. */
static FEcef LocalToEcef(const FTileMeshData& Mesh, size_t VertexIndex)
{
	const FEcef OriginEcef = Core::GeodeticToEcef(Mesh.Origin, WGS84);
	const Core::FMat3 LocalToEcefBasis =
		Core::MakeNeuBasis(Mesh.Origin.LatRad, Mesh.Origin.LonRad).Transposed();

	const FEcef Local{ Mesh.Positions[VertexIndex * 3 + 0],
	                   Mesh.Positions[VertexIndex * 3 + 1],
	                   Mesh.Positions[VertexIndex * 3 + 2] };
	return LocalToEcefBasis.Transform(Local) + OriginEcef;
}

// ===========================================================================
static void TestCounts()
{
	Section("1. Conteggi e integrita'");

	FTileMeshData Mesh;
	FTileMeshParameters Parameters;
	BuildTileMesh(MakeTile(FTileKey{ 12, 2190, 547 }), Parameters, Mesh);

	constexpr uint32_t Posts = Tiles::TilePosts;
	constexpr uint32_t Cells = Tiles::TileCells;

	Check(Mesh.InteriorVertexCount == Posts * Posts, "129x129 post interni",
		std::to_string(Mesh.InteriorVertexCount));
	Check(Mesh.SkirtVertexCount == 4 * Posts, "quattro bordi di gonna da 129 vertici",
		std::to_string(Mesh.SkirtVertexCount));
	Check(Mesh.TriangleCount == Cells * Cells * 2 + 4 * Cells * 2,
		"triangoli: 128x128x2 di superficie piu' 4x128x2 di gonna",
		std::to_string(Mesh.TriangleCount));
	Check(Mesh.IsValid(), "gli array hanno lunghezze coerenti");

	const uint32_t TotalVertices = Mesh.InteriorVertexCount + Mesh.SkirtVertexCount;
	uint32_t WorstIndex = 0;
	for (uint32_t Index : Mesh.Indices) { WorstIndex = std::max(WorstIndex, Index); }
	Check(WorstIndex < TotalVertices, "nessun indice fuori dall'array dei vertici",
		std::to_string(WorstIndex) + " < " + std::to_string(TotalVertices));

	// Senza gonna: solo la superficie.
	FTileMeshData NoSkirt;
	FTileMeshParameters NoSkirtParameters;
	NoSkirtParameters.bGenerateSkirt = false;
	BuildTileMesh(MakeTile(FTileKey{ 12, 2190, 547 }), NoSkirtParameters, NoSkirt);
	Check(NoSkirt.SkirtVertexCount == 0 && NoSkirt.TriangleCount == Cells * Cells * 2,
		"disattivando la gonna restano solo i triangoli di superficie",
		std::to_string(NoSkirt.TriangleCount));
}

// ===========================================================================
static void TestLocalFrame()
{
	Section("2. Frame locale: i float restano piccoli");

	for (uint32_t Level : { 8u, 12u, 14u })
	{
		FTileMeshData Mesh;
		FTileMeshParameters Parameters;
		BuildTileMesh(MakeTile(FTileKey{ Level, 1u << Level, 1u << (Level - 1) }), Parameters, Mesh);

		double WorstMagnitude = 0.0;
		for (uint32_t Vertex = 0; Vertex < Mesh.InteriorVertexCount; ++Vertex)
		{
			for (int Axis = 0; Axis < 3; ++Axis)
			{
				WorstMagnitude = std::max(WorstMagnitude,
					std::abs(static_cast<double>(Mesh.Positions[Vertex * 3 + Axis])));
			}
		}

		// Meta' della diagonale della tile, piu' margine per la quota.
		const double TileSpanMetres = Tiles::TileSpanDeg(Level) * 111132.0;
		Check(WorstMagnitude < TileSpanMetres, "livello " + std::to_string(Level)
			+ ": i vertici restano dentro la tile",
			Fmt("max %.0f m", WorstMagnitude) + Fmt(", tile %.0f m", TileSpanMetres));

		// Precisione del float a quella magnitudine: e' il motivo del frame locale.
		const float AsFloat = static_cast<float>(WorstMagnitude);
		const double Ulp = static_cast<double>(std::nextafterf(AsFloat, AsFloat * 2.0f)) - AsFloat;
		Check(Ulp < 1.0, "livello " + std::to_string(Level) + ": ULP del float sotto il metro",
			Fmt("%.4f m", Ulp));

		Check(Mesh.BoundingRadiusMetres >= WorstMagnitude,
			"il raggio dichiarato contiene i vertici",
			Fmt("%.0f m", Mesh.BoundingRadiusMetres));
	}
}

// ===========================================================================
static void TestNormalsAndUVs()
{
	Section("3. Normali e coordinate di texture");

	FTileMeshData Mesh;
	FTileMeshParameters Parameters;
	BuildTileMesh(MakeTile(FTileKey{ 12, 2190, 547 }), Parameters, Mesh);

	double WorstLengthError = 0.0;
	for (uint32_t Vertex = 0; Vertex < Mesh.InteriorVertexCount; ++Vertex)
	{
		const double X = Mesh.Normals[Vertex * 3 + 0];
		const double Y = Mesh.Normals[Vertex * 3 + 1];
		const double Z = Mesh.Normals[Vertex * 3 + 2];
		WorstLengthError = std::max(WorstLengthError,
			std::abs(std::sqrt(X * X + Y * Y + Z * Z) - 1.0));
	}
	Check(WorstLengthError < 1e-5, "tutte le normali sono unitarie",
		Fmt("errore max %.3e", WorstLengthError));

	// Su terreno piatto devono puntare verso l'alto locale, cioe' +Z.
	FTileMeshData Flat;
	BuildTileMesh(MakeTile(FTileKey{ 12, 2190, 547 }, /*bFlat=*/true), Parameters, Flat);

	double WorstUpDeviation = 0.0;
	for (uint32_t Vertex = 0; Vertex < Flat.InteriorVertexCount; ++Vertex)
	{
		WorstUpDeviation = std::max(WorstUpDeviation,
			std::abs(1.0 - Flat.Normals[Vertex * 3 + 2]));
	}
	Check(WorstUpDeviation < 1e-3, "su terreno piatto le normali puntano in alto (+Z)",
		Fmt("scarto max %.3e", WorstUpDeviation));

	// UV: gli angoli devono essere esattamente 0 e 1.
	const uint32_t Posts = Tiles::TilePosts;
	const uint32_t Cells = Tiles::TileCells;
	const bool bCorners =
		Mesh.UVs[0] == 0.0f && Mesh.UVs[1] == 0.0f &&
		Mesh.UVs[Cells * 2] == 1.0f && Mesh.UVs[Cells * 2 + 1] == 0.0f &&
		Mesh.UVs[(static_cast<size_t>(Cells) * Posts) * 2 + 1] == 1.0f;
	Check(bCorners, "le UV coprono esattamente [0,1] sugli angoli");

	bool bInRange = true;
	for (float Value : Mesh.UVs) { if (Value < 0.0f || Value > 1.0f) { bInRange = false; } }
	Check(bInRange, "tutte le UV stanno in [0,1]");
}

// ===========================================================================
static void TestSkirt()
{
	Section("4. Gonne");

	const FTileKey Key{ 13, 4380, 1094 };
	FTileMeshData Mesh;
	FTileMeshParameters Parameters;
	BuildTileMesh(MakeTile(Key), Parameters, Mesh);

	const double Depth = ComputeSkirtDepth(Key.Level);
	Check(Depth > 100.0 && Depth < 200.0,
		"profondita' al livello 13 dell'ordine del centinaio di metri", Fmt("%.1f m", Depth));

	// Ogni vertice di gonna sta esattamente Depth sotto il suo post di bordo,
	// sulle altre due componenti identico.
	const uint32_t Posts = Tiles::TilePosts;
	const uint32_t Cells = Tiles::TileCells;
	double WorstDrop = 0.0, WorstLateral = 0.0;

	struct FEdge { int32_t StartI, StartJ, StepI, StepJ; };
	const FEdge Edges[4] = { { 0, 0, 1, 0 }, { (int32_t)Cells, 0, 0, 1 },
	                         { (int32_t)Cells, (int32_t)Cells, -1, 0 }, { 0, (int32_t)Cells, 0, -1 } };

	for (int EdgeIndex = 0; EdgeIndex < 4; ++EdgeIndex)
	{
		for (int32_t Step = 0; Step <= (int32_t)Cells; ++Step)
		{
			const int32_t I = Edges[EdgeIndex].StartI + Edges[EdgeIndex].StepI * Step;
			const int32_t J = Edges[EdgeIndex].StartJ + Edges[EdgeIndex].StepJ * Step;
			const size_t Source = static_cast<size_t>(J) * Posts + I;
			const size_t Target = Mesh.InteriorVertexCount + EdgeIndex * Posts + Step;

			WorstDrop = std::max(WorstDrop, std::abs(
				(Mesh.Positions[Source * 3 + 2] - Mesh.Positions[Target * 3 + 2]) - Depth));
			WorstLateral = std::max({ WorstLateral,
				static_cast<double>(std::abs(Mesh.Positions[Source * 3 + 0]
				                           - Mesh.Positions[Target * 3 + 0])),
				static_cast<double>(std::abs(Mesh.Positions[Source * 3 + 1]
				                           - Mesh.Positions[Target * 3 + 1])) });
		}
	}
	Check(WorstDrop < 0.05, "ogni vertice di gonna scende esattamente della profondita'",
		Fmt("scarto max %.4f m", WorstDrop));
	Check(WorstLateral < 1e-6, "la gonna e' verticale: nessuno scostamento laterale",
		Fmt("max %.3e m", WorstLateral));

	// La profondita' deve raddoppiare salendo di un livello, come il passo.
	Check(std::abs(ComputeSkirtDepth(12) / ComputeSkirtDepth(13) - 2.0) < 1e-9,
		"la profondita' raddoppia per livello, come l'errore geometrico");
}

// ===========================================================================
static void TestSeamsBetweenAdjacentTiles()
{
	Section("5. Giunzioni: i bordi condivisi coincidono nello spazio");

	// E' LA verifica della fase. Due tile adiacenti hanno frame locali DIVERSI,
	// origini diverse e orientamenti diversi. Se la costruzione e' corretta, i
	// vertici del bordo condiviso devono comunque cadere nello STESSO punto del
	// pianeta: altrimenti si apre una crepa fra tile dello stesso livello, che
	// nessuna gonna puo' chiudere perche' sarebbe un buco a geometria corretta.
	const uint32_t Level = 13;
	const FTileKey Left{ Level, 4380, 1094 };
	const FTileKey Right{ Level, 4381, 1094 };
	const FTileKey Below{ Level, 4380, 1095 };

	FTileMeshParameters Parameters;
	FTileMeshData LeftMesh, RightMesh, BelowMesh;
	BuildTileMesh(MakeTile(Left), Parameters, LeftMesh);
	BuildTileMesh(MakeTile(Right), Parameters, RightMesh);
	BuildTileMesh(MakeTile(Below), Parameters, BelowMesh);

	constexpr uint32_t Posts = Tiles::TilePosts;
	constexpr uint32_t Cells = Tiles::TileCells;

	double WorstHorizontal = 0.0;
	for (uint32_t J = 0; J < Posts; ++J)
	{
		const FEcef A = LocalToEcef(LeftMesh, static_cast<size_t>(J) * Posts + Cells);
		const FEcef B = LocalToEcef(RightMesh, static_cast<size_t>(J) * Posts + 0);
		WorstHorizontal = std::max(WorstHorizontal, (A - B).Length());
	}
	Check(WorstHorizontal < 0.02, "giunzione EST-OVEST: i 129 vertici coincidono",
		Fmt("scarto max %.4f m", WorstHorizontal));

	double WorstVertical = 0.0;
	for (uint32_t I = 0; I < Posts; ++I)
	{
		const FEcef A = LocalToEcef(LeftMesh, static_cast<size_t>(Cells) * Posts + I);
		const FEcef B = LocalToEcef(BelowMesh, I);
		WorstVertical = std::max(WorstVertical, (A - B).Length());
	}
	Check(WorstVertical < 0.02, "giunzione NORD-SUD: i 129 vertici coincidono",
		Fmt("scarto max %.4f m", WorstVertical));

	std::printf("  (lo scarto residuo e' la precisione del float sui vertici,\n"
	            "   non un disallineamento: le quote sorgente sono identiche)\n");
}

// ===========================================================================
static void TestWinding()
{
	Section("6. Orientamento dei triangoli");

	FTileMeshData Mesh;
	FTileMeshParameters Parameters;
	BuildTileMesh(MakeTile(FTileKey{ 12, 2190, 547 }, /*bFlat=*/true), Parameters, Mesh);

	// Su terreno piatto tutti i triangoli di superficie devono avere la stessa
	// orientazione: se anche uno fosse invertito, comparirebbe un buco.
	const uint32_t SurfaceTriangles = Tiles::TileCells * Tiles::TileCells * 2;
	int Upward = 0, Downward = 0;

	for (uint32_t Triangle = 0; Triangle < SurfaceTriangles; ++Triangle)
	{
		const uint32_t A = Mesh.Indices[Triangle * 3 + 0];
		const uint32_t B = Mesh.Indices[Triangle * 3 + 1];
		const uint32_t C = Mesh.Indices[Triangle * 3 + 2];

		const double ABx = Mesh.Positions[B * 3 + 0] - Mesh.Positions[A * 3 + 0];
		const double ABy = Mesh.Positions[B * 3 + 1] - Mesh.Positions[A * 3 + 1];
		const double ACx = Mesh.Positions[C * 3 + 0] - Mesh.Positions[A * 3 + 0];
		const double ACy = Mesh.Positions[C * 3 + 1] - Mesh.Positions[A * 3 + 1];

		(ABx * ACy - ABy * ACx > 0.0 ? Upward : Downward)++;
	}

	Check(Upward == 0 || Downward == 0, "tutti i triangoli hanno la stessa orientazione",
		std::to_string(Upward) + " in un verso, " + std::to_string(Downward) + " nell'altro");

	// Invertendo il parametro si invertono tutti.
	FTileMeshParameters Flipped = Parameters;
	Flipped.bFlipWinding = !Parameters.bFlipWinding;
	FTileMeshData FlippedMesh;
	BuildTileMesh(MakeTile(FTileKey{ 12, 2190, 547 }, true), Flipped, FlippedMesh);

	Check(Mesh.Indices[1] == FlippedMesh.Indices[2] && Mesh.Indices[2] == FlippedMesh.Indices[1],
		"bFlipWinding inverte davvero l'ordine dei vertici");
}

// ===========================================================================
static void TestCost()
{
	Section("7. Costo");

	FTileMeshData Mesh;
	FTileMeshParameters Parameters;
	BuildTileMesh(MakeTile(FTileKey{ 14, 8760, 2188 }), Parameters, Mesh);

	const double Megabytes = Mesh.GetByteSize() / (1024.0 * 1024.0);
	std::printf("  una tile: %u vertici, %u triangoli, %.2f MB in memoria\n",
		Mesh.InteriorVertexCount + Mesh.SkirtVertexCount, Mesh.TriangleCount, Megabytes);
	std::printf("  con 200 tile a schermo: %.0f MB di geometria\n", Megabytes * 200.0);

	Check(Megabytes < 1.0, "una tile sta sotto il megabyte", Fmt("%.3f MB", Megabytes));
}


// ===========================================================================
static void TestMeshStep()
{
	Section("8. Passo della mesh: un post ogni N");

	// Il passo 2 e' il default del motore dopo la prima prova su un portatile:
	// un quarto dei triangoli per tile, con il LOD che raddoppia l'errore
	// geometrico per compensare.
	const FTileKey Key{ 13, 4380, 1094 };
	const FHeightTile Tile = MakeTile(Key);

	FTileMeshParameters Full;
	FTileMeshParameters Half;
	Half.Step = 2;
	FTileMeshData FullMesh, HalfMesh;
	BuildTileMesh(Tile, Full, FullMesh);
	BuildTileMesh(Tile, Half, HalfMesh);

	Check(HalfMesh.IsValid() && HalfMesh.InteriorVertexCount == 65u * 65u,
		"passo 2: 65x65 post", std::to_string(HalfMesh.InteriorVertexCount) + " vertici interni");
	Check(HalfMesh.TriangleCount == 64u * 64u * 2u + 4u * 64u * 2u,
		"passo 2: 8.704 triangoli con le gonne, un quarto di 33.792",
		std::to_string(HalfMesh.TriangleCount));

	// I post della mesh rada sono ESATTAMENTE post della mesh piena: stessa
	// posizione, non un'interpolazione. Si confrontano in ECEF, perche' le
	// origini locali coincidono solo se coincidono le quote estreme.
	double Worst = 0.0;
	for (uint32_t J = 0; J < 65; ++J)
	{
		for (uint32_t I = 0; I < 65; ++I)
		{
			const FEcef A = LocalToEcef(HalfMesh, static_cast<size_t>(J) * 65 + I);
			const FEcef B = LocalToEcef(FullMesh, static_cast<size_t>(J * 2) * Tiles::TilePosts + I * 2);
			Worst = std::max(Worst, (A - B).Length());
		}
	}
	Check(Worst < 0.02, "ogni vertice della mesh rada e' un post vero della tile",
		Fmt("scarto max %.4f m", Worst));

	// Le UV coprono comunque [0,1]: l'ortofoto si drappeggia uguale.
	const float LastU = HalfMesh.UVs[64 * 2 + 0];
	const float LastV = HalfMesh.UVs[(static_cast<size_t>(64) * 65 + 64) * 2 + 1];
	Check(LastU == 1.0f && LastV == 1.0f, "le UV vanno da 0 a 1 anche con il passo 2");

	// Giunzione fra due tile vicine, entrambe col passo 2: i bordi coincidono.
	const FTileKey Right{ 13, 4381, 1094 };
	FTileMeshData RightMesh;
	BuildTileMesh(MakeTile(Right), Half, RightMesh);
	double WorstSeam = 0.0;
	for (uint32_t J = 0; J < 65; ++J)
	{
		const FEcef A = LocalToEcef(HalfMesh, static_cast<size_t>(J) * 65 + 64);
		const FEcef B = LocalToEcef(RightMesh, static_cast<size_t>(J) * 65 + 0);
		WorstSeam = std::max(WorstSeam, (A - B).Length());
	}
	Check(WorstSeam < 0.02, "giunzione EST-OVEST con il passo 2: i 65 vertici coincidono",
		Fmt("scarto max %.4f m", WorstSeam));

	// Un passo che non divide 128 non deve produrre una griglia sbilenca.
	FTileMeshParameters Odd;
	Odd.Step = 3;
	FTileMeshData OddMesh;
	BuildTileMesh(Tile, Odd, OddMesh);
	Check(OddMesh.InteriorVertexCount == FullMesh.InteriorVertexCount,
		"un passo che non divide 128 (3) ricade sul passo 1");
}

// ===========================================================================

// ===========================================================================
//  9. Geomorphing: le figlie nascono con la forma e la luce del padre
// ===========================================================================
static void TestMorphTargets()
{
	Section("9. Geomorphing: bersagli verso la superficie del padre");

	const FTileKey ParentKey{ 12, 2190, 547 };
	const FTileKey ChildKey = ParentKey.GetChild(3);      // quarto sud-est
	const FHeightTile Parent = MakeTile(ParentKey);
	const FHeightTile Child = MakeTile(ChildKey);

	FTileMeshParameters Parameters;
	Parameters.Step = 2;
	FTileMeshData Mesh;
	BuildTileMesh(Child, Parameters, Mesh);
	ComputeMorphTargets(Child, Parent, Parameters.Step, Mesh);

	const size_t Vertices = Mesh.Positions.size() / 3;
	Check(Mesh.MorphDeltas.size() == Vertices && Mesh.ParentNormals.size() == Vertices * 3,
		"un delta e una normale del padre per ogni vertice, gonne comprese");

	// Il vertice spostato di -delta lungo l'alto locale deve stare SULLA
	// superficie disegnata del padre: e' la forma che c'era a schermo.
	const FSurfaceSampler ParentSurface(Parent, Parameters.Step);
	double Worst = 0.0, LargestDelta = 0.0;
	for (size_t Vertex = 0; Vertex < Mesh.InteriorVertexCount; Vertex += 37)
	{
		const FEcef OriginEcef = Core::GeodeticToEcef(Mesh.Origin, WGS84);
		const Core::FMat3 ToEcef = Core::MakeNeuBasis(Mesh.Origin.LatRad, Mesh.Origin.LonRad).Transposed();
		const FEcef Morphed{ Mesh.Positions[Vertex * 3 + 0], Mesh.Positions[Vertex * 3 + 1],
		                     Mesh.Positions[Vertex * 3 + 2] - Mesh.MorphDeltas[Vertex] };
		const FGeodetic Point = Core::EcefToGeodetic(ToEcef.Transform(Morphed) + OriginEcef, WGS84);
		const double Surface = ParentSurface.HeightAt(Point.LonRad / Core::DegToRad, Point.LatRad / Core::DegToRad);
		Worst = std::max(Worst, std::fabs(Point.HeightM - Surface));
		LargestDelta = std::max(LargestDelta, std::fabs(static_cast<double>(Mesh.MorphDeltas[Vertex])));
	}
	Check(Worst < 0.05, "il vertice spostato di -delta sta sulla superficie del padre",
		Fmt("scarto massimo %.4f m", Worst));
	Check(LargestDelta > 0.01, "su un terreno vero i delta non sono tutti zero", Fmt("delta massimo %.3f m", LargestDelta));

	// Gonne: stesso delta del post di bordo da cui scendono (il primo
	// vertice di gonna e' l'angolo nord-ovest).
	Check(Mesh.MorphDeltas[Mesh.InteriorVertexCount] == Mesh.MorphDeltas[0],
		"la gonna si sposta con il suo post di bordo");

	// Su un terreno che E' la superficie del padre, nessuno spostamento e la
	// stessa luce: il morphing non deve inventare niente.
	FHeightTile Same = Child;
	{
		const auto Bounds = Tiles::GetTileBounds(ChildKey.Level, ChildKey.X, ChildKey.Y);
		const double Spacing = Tiles::PostSpacingDeg(ChildKey.Level);
		for (int32_t J = 0; J < Tiles::TilePosts; ++J)
		{
			for (int32_t I = 0; I < Tiles::TilePosts; ++I)
			{
				Same.Heights[static_cast<size_t>(J) * Tiles::TilePosts + I] =
					static_cast<float>(ParentSurface.HeightAt(Bounds.West + I * Spacing, Bounds.North - J * Spacing));
			}
		}
	}
	FTileMeshData SameMesh;
	BuildTileMesh(Same, Parameters, SameMesh);
	ComputeMorphTargets(Same, Parent, Parameters.Step, SameMesh);
	double SameWorst = 0.0, AngleSum = 0.0;
	int Samples = 0;
	for (size_t Vertex = 0; Vertex < SameMesh.InteriorVertexCount; Vertex += 13)
	{
		SameWorst = std::max(SameWorst, std::fabs(static_cast<double>(SameMesh.MorphDeltas[Vertex])));
		const double Dot = SameMesh.Normals[Vertex * 3 + 0] * SameMesh.ParentNormals[Vertex * 3 + 0]
		                 + SameMesh.Normals[Vertex * 3 + 1] * SameMesh.ParentNormals[Vertex * 3 + 1]
		                 + SameMesh.Normals[Vertex * 3 + 2] * SameMesh.ParentNormals[Vertex * 3 + 2];
		AngleSum += std::acos(std::min(1.0, Dot)) * 180.0 / 3.14159265358979;
		++Samples;
	}
	Check(SameWorst < 0.01, "figlia uguale al padre: delta nulli", Fmt("%.4f m", SameWorst));
	Check(AngleSum / Samples < 3.0, "figlia uguale al padre: stessa luce (normali entro pochi gradi)",
		Fmt("%.2f gradi in media", AngleSum / Samples));

	// Dati incoerenti (passo diverso da quello della mesh): niente morphing,
	// non dati sbagliati.
	FTileMeshData Wrong;
	BuildTileMesh(Child, Parameters, Wrong);
	ComputeMorphTargets(Child, Parent, 4, Wrong);
	Check(Wrong.MorphDeltas.empty(), "passo incoerente: nessun bersaglio");
}

int main()
{
	std::printf("=====================================================\n");
	std::printf(" Mesh -- test dello strato puro (senza Unreal)\n");
	std::printf("=====================================================\n");

	TestCounts();
	TestLocalFrame();
	TestNormalsAndUVs();
	TestSkirt();
	TestSeamsBetweenAdjacentTiles();
	TestWinding();
	TestCost();
	TestMeshStep();
	TestMorphTargets();

	std::printf("\n=====================================================\n");
	std::printf(" RISULTATO: %d passati, %d falliti\n", GPassed, GFailed);
	std::printf("=====================================================\n");
	return GFailed == 0 ? 0 : 1;
}

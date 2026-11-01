// =============================================================================
//  Test dei modelli (docs/modelli-design.md, M1), senza Unreal: le
//  trasformazioni e gli assi dei formati, le immagini SGI, i percorsi delle
//  texture, OBJ con i materiali, OpenFlight (un file costruito qui byte per
//  byte con gli spostamenti della specifica: cubo in piedi, palette, DOF,
//  switch, LOD, sequenza, riferimento esterno, mesh a strisce, istanze, punti
//  luce, un record continuato), il rig (assi dell'attore, triangoli verso
//  fuori, UV, parti, DOF che gira attorno al suo perno), le regole "file"
//  della tabella dei modelli.
// =============================================================================
#include "Entities/EntityModels.h"
#include "Models/ModelData.h"
#include "Models/ModelFiles.h"
#include "Models/ModelLoader.h"
#include "Models/ModelRig.h"
#include "Models/ObjReader.h"
#include "Models/OpenFlightReader.h"
#include "Models/SgiImage.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

using namespace GeoWorld::Models;

static int GPassed = 0, GFailed = 0;

static void Check(bool bCondition, const std::string& What, const std::string& Detail = {})
{
	(bCondition ? GPassed : GFailed)++;
	std::printf("  [%s] %s%s%s\n", bCondition ? " ok " : "FAIL", What.c_str(), Detail.empty() ? "" : "  -> ", Detail.c_str());
}

static void Section(const char* Title) { std::printf("\n== %s ==\n", Title); }

static std::string Fmt(const char* Format, double A, double B = 0.0, double C = 0.0, double D = 0.0)
{
	char Buffer[240];
	std::snprintf(Buffer, sizeof(Buffer), Format, A, B, C, D);
	return Buffer;
}

// --- Una cartella finta in memoria ------------------------------------------------

struct FFakeDisk
{
	std::map<std::string, std::vector<uint8_t>> Files;

	void Put(const std::string& Path, const std::string& Text) { Files[Path] = std::vector<uint8_t>(Text.begin(), Text.end()); }
	void Put(const std::string& Path, const std::vector<uint8_t>& Bytes) { Files[Path] = Bytes; }

	FFileAccess Access()
	{
		FFileAccess A;
		A.Read = [this](const std::string& Path, std::vector<uint8_t>& Out)
		{
			const auto It = Files.find(NormalisePath(Path));
			if (It == Files.end()) { return false; }
			Out = It->second;
			return true;
		};
		A.Exists = [this](const std::string& Path) { return Files.count(NormalisePath(Path)) > 0; };
		return A;
	}
};

// --- Un file OpenFlight, record per record (big-endian, spostamenti della specifica) ---

struct FFlt
{
	std::vector<uint8_t> Bytes;
	size_t Start = 0;

	void Begin(uint16_t Opcode) { Start = Bytes.size(); U16(Opcode); U16(0); }
	void End()
	{
		const size_t Length = Bytes.size() - Start;
		Bytes[Start + 2] = static_cast<uint8_t>(Length >> 8);
		Bytes[Start + 3] = static_cast<uint8_t>(Length);
	}
	/** Riempie di zeri fino allo spostamento dato, dall'inizio del record. */
	void At(size_t Offset) { while (Bytes.size() < Start + Offset) { Bytes.push_back(0); } }
	void U8(uint32_t V) { Bytes.push_back(static_cast<uint8_t>(V)); }
	void U16(uint32_t V) { U8(V >> 8); U8(V); }
	void U32(uint32_t V) { U8(V >> 24); U8(V >> 16); U8(V >> 8); U8(V); }
	void F32(float V) { uint32_t B; std::memcpy(&B, &V, 4); U32(B); }
	void F64(double V) { uint64_t B; std::memcpy(&B, &V, 8); U32(static_cast<uint32_t>(B >> 32)); U32(static_cast<uint32_t>(B)); }
	void Text(const std::string& S, size_t Size) { for (size_t I = 0; I < Size; ++I) { U8(I < S.size() ? static_cast<uint8_t>(S[I]) : 0); } }
	void Abgr(uint8_t R, uint8_t G, uint8_t B) { U8(255); U8(B); U8(G); U8(R); }

	void Simple(uint16_t Opcode) { Begin(Opcode); End(); }
	void Push() { Simple(10); }
	void Pop() { Simple(11); }

	void Header(uint8_t Units)
	{
		Begin(1);
		At(4); Text("db", 8);
		At(12); U32(1640);
		At(62); U8(Units);
		At(324);
		End();
	}

	void Group(const std::string& Name, uint32_t Flags = 0, float Loop = 0.0f)
	{
		Begin(2);
		At(4); Text(Name, 8);
		At(16); U32(Flags);
		At(36); F32(Loop);
		At(44);
		End();
	}

	void Object(const std::string& Name)
	{
		Begin(4);
		At(4); Text(Name, 8);
		At(28);
		End();
	}

	struct FFace
	{
		uint8_t DrawType = 0;
		bool bTexWhite = false;
		int16_t Texture = -1;
		int16_t Material = -1;
		int32_t IrMaterial = 0;
		uint16_t Transparency = 0;
		uint32_t Flags = 0;
		uint8_t LightMode = 0;
		uint8_t Packed[3] = { 255, 255, 255 };
		uint32_t ColorIndex = 127;   // colore 0 a intensita' piena
	};

	void Face(const std::string& Name, const FFace& F, bool bMesh = false)
	{
		const size_t S = bMesh ? 4 : 0;
		Begin(bMesh ? 84 : 5);
		At(4); Text(Name, 8);
		At(18 + S); U8(F.DrawType); U8(F.bTexWhite ? 1 : 0);
		At(26 + S); U16(0xFFFF); U16(static_cast<uint16_t>(F.Texture)); U16(static_cast<uint16_t>(F.Material));
		At(36 + S); U32(static_cast<uint32_t>(F.IrMaterial)); U16(F.Transparency);
		At(44 + S); U32(F.Flags); U8(F.LightMode);
		At(56 + S); Abgr(F.Packed[0], F.Packed[1], F.Packed[2]); Abgr(0, 0, 0);
		At(64 + S); U16(0xFFFF);
		At(68 + S); U32(F.ColorIndex); U32(0);
		At(78 + S); U16(0xFFFF);
		End();
	}

	/** Un vertice con colore, normale e UV (70), o solo colore e UV (71). Ritorna lo spostamento nella palette. */
	size_t Vertex(size_t Palette, const double P[3], const float* Normal, float U, float V)
	{
		const size_t Offset = Bytes.size() - Palette;
		Begin(Normal ? 70 : 71);
		At(4); U16(0); U16(0);
		At(8); F64(P[0]); F64(P[1]); F64(P[2]);
		if (Normal) { F32(Normal[0]); F32(Normal[1]); F32(Normal[2]); }
		F32(U); F32(V);
		Abgr(255, 255, 255); U32(0);
		if (Normal) { U32(0); }
		End();
		return Offset;
	}

	void VertexList(const std::vector<size_t>& Offsets)
	{
		Begin(72);
		for (size_t O : Offsets) { U32(static_cast<uint32_t>(O)); }
		End();
	}

	void Matrix(double Tx, double Ty, double Tz)
	{
		Begin(49);
		const float M[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, static_cast<float>(Tx), static_cast<float>(Ty), static_cast<float>(Tz), 1 };
		for (float V : M) { F32(V); }
		End();
	}
};

static double Distance3(const float A[3], const double B[3])
{
	return std::sqrt((A[0] - B[0]) * (A[0] - B[0]) + (A[1] - B[1]) * (A[1] - B[1]) + (A[2] - B[2]) * (A[2] - B[2]));
}

/** Ogni triangolo: (B - A) x (C - A) con le coordinate cosi' come sono, d'accordo con la normale dei vertici? */
static bool WindingOutward(const FRigSection& S, double* WorstCos = nullptr)
{
	double Worst = 1.0;
	for (size_t I = 0; I + 2 < S.Indices.size(); I += 3)
	{
		const FRigVertex& A = S.Vertices[S.Indices[I]];
		const FRigVertex& B = S.Vertices[S.Indices[I + 1]];
		const FRigVertex& C = S.Vertices[S.Indices[I + 2]];
		const double U[3] = { B.P[0] - A.P[0], B.P[1] - A.P[1], B.P[2] - A.P[2] };
		const double V[3] = { C.P[0] - A.P[0], C.P[1] - A.P[1], C.P[2] - A.P[2] };
		const double X[3] = { U[1] * V[2] - U[2] * V[1], U[2] * V[0] - U[0] * V[2], U[0] * V[1] - U[1] * V[0] };
		const double L = std::sqrt(X[0] * X[0] + X[1] * X[1] + X[2] * X[2]);
		if (L < 1e-12) { continue; }
		const double Cos = (X[0] * A.N[0] + X[1] * A.N[1] + X[2] * A.N[2]) / L;
		Worst = std::min(Worst, Cos);
	}
	if (WorstCos) { *WorstCos = Worst; }
	return Worst > 0.99;
}

// ===========================================================================

static void TestAffineAndAxes()
{
	Section("1. Le trasformazioni e gli assi dei formati");
	FAffine A = FAffine::Translation(1, 2, 3) * FAffine::Rotation(2, 0.7) * FAffine::Scale(2, 3, 4);
	const FAffine I = A * A.Inverse();
	double Worst = 0.0;
	for (int R = 0; R < 3; ++R)
	{
		for (int C = 0; C < 3; ++C) { Worst = std::max(Worst, std::fabs(I.M[R][C] - (R == C ? 1.0 : 0.0))); }
		Worst = std::max(Worst, std::fabs(I.T[R]));
	}
	Check(Worst < 1e-12, "l'inversa di una trasformazione con scala", Fmt("%.1e", Worst));

	struct FCase { const char* Name; FAxisConvention Axes; double Forward[3], Right[3], Up[3]; };
	const FCase Cases[] = {
		{ "OpenFlight (avanti +Y, alto +Z, destra +X)", FAxisConvention::OpenFlight(), { 0, 1, 0 }, { 1, 0, 0 }, { 0, 0, 1 } },
		{ "OBJ (avanti -Z, alto +Y, destra +X)", FAxisConvention::Obj(), { 0, 0, -1 }, { 1, 0, 0 }, { 0, 1, 0 } },
		{ "glTF (avanti +Z, alto +Y, destra -X)", FAxisConvention::Gltf(), { 0, 0, 1 }, { -1, 0, 0 }, { 0, 1, 0 } },
	};
	for (const FCase& C : Cases)
	{
		const FAffine M = ToActorAxes(C.Axes);
		double F[3], R[3], U[3];
		M.ApplyVector(C.Forward, F);
		M.ApplyVector(C.Right, R);
		M.ApplyVector(C.Up, U);
		Check(F[0] == 1 && R[1] == 1 && U[2] == 1 && M.Determinant() == -1.0,
			std::string(C.Name) + ": all'attore avanti X, destra Y, alto Z; determinante -1 (una riflessione)");
	}
	EAxis Axis;
	Check(ParseAxis("-z", Axis) && Axis == EAxis::NegZ && ParseAxis("Y", Axis) && Axis == EAxis::PosY && !ParseAxis("w", Axis),
		"gli assi scritti: -z, Y; 'w' no");
}

static std::vector<uint8_t> SgiHeader(uint8_t Storage, uint16_t W, uint16_t H, uint16_t Channels)
{
	std::vector<uint8_t> B(512, 0);
	B[0] = 0x01; B[1] = 0xDA; B[2] = Storage; B[3] = 1;
	B[4] = 0; B[5] = 3;
	B[6] = static_cast<uint8_t>(W >> 8); B[7] = static_cast<uint8_t>(W);
	B[8] = static_cast<uint8_t>(H >> 8); B[9] = static_cast<uint8_t>(H);
	B[10] = static_cast<uint8_t>(Channels >> 8); B[11] = static_cast<uint8_t>(Channels);
	return B;
}

static void TestSgi()
{
	Section("2. Le immagini SGI (le texture di OpenFlight)");
	// 2x2 RGB senza compressione. Righe dal BASSO: la prima riga del file e' quella di sotto.
	std::vector<uint8_t> Raw = SgiHeader(0, 2, 2, 3);
	const uint8_t Planes[3][4] = { { 10, 20, 30, 40 }, { 50, 60, 70, 80 }, { 90, 100, 110, 120 } };
	for (const auto& Plane : Planes) { for (uint8_t V : Plane) { Raw.push_back(V); } }
	FDecodedImage Image;
	std::string Error;
	const bool bRaw = DecodeSgiImage(Raw, Image, Error);
	// In alto a sinistra nell'uscita = la riga 1 del file (quella di sopra), colonna 0: (30, 70, 110).
	Check(bRaw && Image.Width == 2 && Image.Height == 2 && Image.Rgba[0] == 30 && Image.Rgba[1] == 70 && Image.Rgba[2] == 110
		&& Image.Rgba[3] == 255 && !Image.bHasAlpha && Image.Rgba[8] == 10,
		"senza compressione, 2x2 RGB: le righe girate (dall'alto), l'alfa piena", Error);

	// 4x1 RGBA in RLE: rosso ripetuto, verde letterale, blu ripetuto, alfa con un buco.
	std::vector<uint8_t> Rle = SgiHeader(1, 4, 1, 4);
	const std::vector<std::vector<uint8_t>> Lines = {
		{ 0x04, 200, 0x00 },                    // 4 volte 200
		{ 0x84, 1, 2, 3, 4, 0x00 },             // 4 letterali
		{ 0x02, 7, 0x02, 9, 0x00 },             // 2 volte 7, 2 volte 9
		{ 0x03, 255, 0x01, 0, 0x00 } };         // alfa 255 255 255 0
	const size_t Tables = 512 + Lines.size() * 8;
	std::vector<uint32_t> Starts;
	size_t At = Tables;
	for (const auto& L : Lines) { Starts.push_back(static_cast<uint32_t>(At)); At += L.size(); }
	for (uint32_t S : Starts) { Rle.push_back(static_cast<uint8_t>(S >> 24)); Rle.push_back(static_cast<uint8_t>(S >> 16)); Rle.push_back(static_cast<uint8_t>(S >> 8)); Rle.push_back(static_cast<uint8_t>(S)); }
	for (const auto& L : Lines) { const uint32_t N = static_cast<uint32_t>(L.size()); Rle.push_back(0); Rle.push_back(0); Rle.push_back(static_cast<uint8_t>(N >> 8)); Rle.push_back(static_cast<uint8_t>(N)); }
	for (const auto& L : Lines) { Rle.insert(Rle.end(), L.begin(), L.end()); }
	const bool bRle = DecodeSgiImage(Rle, Image, Error);
	Check(bRle && Image.Rgba[0] == 200 && Image.Rgba[1] == 1 && Image.Rgba[2] == 7 && Image.Rgba[3] == 255
		&& Image.Rgba[13] == 4 && Image.Rgba[14] == 9 && Image.Rgba[15] == 0 && Image.bHasAlpha,
		"RLE a 4 canali: ripetizioni e letterali, e un pixel trasparente (il materiale dovra' tagliare)", Error);
	std::vector<uint8_t> Broken(Rle.begin(), Rle.begin() + static_cast<std::ptrdiff_t>(Tables + 3));
	Check(!DecodeSgiImage(Broken, Image, Error) && !DecodeSgiImage(std::vector<uint8_t>(600, 0), Image, Error),
		"un'immagine troncata, o un file che non e' SGI: un errore, non un crash", Error);
}

static void TestPaths()
{
	Section("3. I percorsi: dove sono le texture");
	Check(NormalisePath("D:\\Modelli\\\\carro\\x.rgb") == "D:/Modelli/carro/x.rgb" && JoinPath("/a/b/c", "../tex/x.rgb") == "/a/b/tex/x.rgb"
		&& DirectoryOf("/a/b/c.flt") == "/a/b" && FileNameOf("C:\\x\\y.RGB") == "y.RGB" && ExtensionOf("C:/x/y.RGB") == "rgb"
		&& IsAbsolutePath("C:/x") && IsAbsolutePath("/x") && !IsAbsolutePath("x/y"),
		"barre, cartelle, '..', nome, estensione, assoluto");
	FFakeDisk Disk;
	Disk.Put("/db/carri/carro.flt", std::string("x"));
	Disk.Put("/db/carri/textures/mimetica.rgb", std::string("x"));
	Disk.Put("/db/comuni/ruote.rgb", std::string("x"));
	Disk.Put("/db/carri/cingolo.rgb", std::string("x"));
	const FFileAccess Files = Disk.Access();
	const std::unordered_map<std::string, std::string> Index = { { "ruote.rgb", "/db/comuni/ruote.rgb" } };
	Check(ResolveReferencedFile("D:\\Lavoro\\Texture\\mimetica.rgb", "/db/carri", Files) == "/db/carri/textures/mimetica.rgb"
		&& ResolveReferencedFile("cingolo.rgb", "/db/carri", Files) == "/db/carri/cingolo.rgb"
		&& ResolveReferencedFile("/usr/people/mario/RUOTE.rgb", "/db/carri", Files, &Index) == "/db/comuni/ruote.rgb"
		&& ResolveReferencedFile("nessuno.rgb", "/db/carri", Files, &Index).empty(),
		"un percorso di un'altra macchina: textures/ accanto al modello; il nome accanto; l'indice per nome (senza maiuscole); niente se non c'e'");
}

static void TestObj()
{
	Section("4. OBJ con i materiali");
	FFakeDisk Disk;
	Disk.Put("/m/cubo/cubo.mtl",
		"newmtl rosso\nKd 1 0 0\nmap_Kd -s 1 1 1 tex/vernice.png\nnewmtl vetro\nKd 0.2 0.2 0.2\nd 0.4\nKe 0.5 0.5 0\n");
	Disk.Put("/m/cubo/tex/vernice.png", std::string("x"));
	// Un cubo di lato 2 (Y in alto, -Z avanti): facce antiorarie viste da fuori, una quadrangolare, indici negativi.
	Disk.Put("/m/cubo/cubo.obj",
		"# cubo\nmtllib cubo.mtl\no Cubo\n"
		"v -1 -1 -1\nv 1 -1 -1\nv 1 1 -1\nv -1 1 -1\nv -1 -1 1\nv 1 -1 1\nv 1 1 1\nv -1 1 1\n"
		"vt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\n"
		"usemtl rosso\n"
		"f 5/1 6/2 7/3 8/4\n"      // +Z (dietro)
		"f 2/1 1/2 4/3 3/4\n"      // -Z (davanti)
		"f 6 2 3 7\n"              // +X (destra)
		"f 1 5 8 4\n"              // -X
		"f -1 -2 3 4\n"            // +Y (alto): 8 7 3 4
		"usemtl vetro\n"
		"f 1 2 6 5\n");            // -Y (basso)
	FModel Model;
	std::string Error;
	const bool bOk = ReadObj("/m/cubo/cubo.obj", Disk.Access(), Model, Error);
	Check(bOk && Model.TriangleCount() == 12 && Model.Materials.size() == 2 && Model.Textures.size() == 1
		&& Model.Textures[0].Resolved == "/m/cubo/tex/vernice.png",
		"sei facce (12 triangoli), due materiali, la texture trovata (le opzioni prima del nome saltate)", Error);
	const FModelRig Rig = BuildRig(Model);
	bool bOutward = !Rig.Parts.empty();
	double Worst = 1.0;
	bool bCentreOut = true;
	for (const FRigPart& Part : Rig.Parts)
	{
		for (const FRigSection& S : Part.Sections)
		{
			double W;
			bOutward = bOutward && WindingOutward(S, &W);
			Worst = std::min(Worst, W);
			for (const FRigVertex& V : S.Vertices)
			{
				// La normale punta via dal centro del cubo.
				bCentreOut = bCentreOut && (V.P[0] * V.N[0] + V.P[1] * V.N[1] + V.P[2] * V.N[2]) > 0.0;
			}
		}
	}
	Check(bOutward && bCentreOut, "nell'attore i triangoli guardano fuori (calcolati con le coordinate) e le normali escono dal cubo",
		Fmt("coseno peggiore %.3f", Worst));
	// La faccia +Y del file (alto) e' la +Z dell'attore; la -Z del file (avanti) e' la +X dell'attore.
	bool bTop = false, bFront = false;
	for (const FRigSection& S : Rig.Parts[0].Sections)
	{
		for (const FRigVertex& V : S.Vertices)
		{
			if (V.N[2] > 0.99f && std::fabs(V.P[2] - 1.0f) < 1e-6f) { bTop = true; }
			if (V.N[0] > 0.99f && std::fabs(V.P[0] - 1.0f) < 1e-6f) { bFront = true; }
		}
	}
	Check(bTop && bFront, "l'alto del file (+Y) e' l'alto dell'attore, l'avanti (-Z) e' il suo avanti (+X)");
	const FModelMaterial& Glass = Model.Materials[1];
	const FRigSection* Red = nullptr;
	for (const FRigSection& S : Rig.Parts[0].Sections) { if (Rig.Materials[static_cast<size_t>(S.Material)].Texture == 0) { Red = &S; } }
	Check(Glass.bTranslucent && std::fabs(Glass.Emissive[0] - 0.5f) < 1e-6f && Red && Red->Vertices[0].Rgba[0] == 255 && Red->Vertices[0].Rgba[1] == 0,
		"il vetro trasparente e luminoso, il rosso nei colori dei vertici");
	bool bUv = false;
	if (Red) { for (const FRigVertex& V : Red->Vertices) { if (V.UV[0] == 0.0f && V.UV[1] == 1.0f) { bUv = true; } } }
	Check(bUv, "le UV girate: (0, 0) di OBJ (in basso) e' (0, 1) di Unreal (in alto)");

	FRigOptions Options;
	Options.Scale = 0.5;
	Options.YawDeg = 90.0;
	Options.Offset[2] = 1.0;
	const FModelRig Turned = BuildRig(Model, Options);
	Check(std::fabs(Turned.BoundsMin[2] - 0.5) < 1e-6 && std::fabs(Turned.BoundsMax[0] - 0.5) < 1e-6 && std::fabs(Turned.Extent() - 1.0) < 1e-6,
		"le correzioni della tabella: meta' scala, girato, alzato di 1 m", Fmt("z da %.3f, x fino a %.3f", Turned.BoundsMin[2], Turned.BoundsMax[0]));
}

/** Il file OpenFlight di prova. */
static void BuildTank(FFakeDisk& Disk)
{
	FFlt F;
	F.Header(4);   // PIEDI
	// Palette dei colori: 0 bianco, 1 rosso.
	F.Begin(32);
	F.At(132);
	F.Abgr(255, 255, 255);
	F.Abgr(255, 0, 0);
	F.End();
	// Una texture di un'altra macchina; un materiale grigio, un po' luminoso.
	F.Begin(64);
	F.At(4); F.Text("C:/vecchia/macchina/skin.rgb", 200);
	F.At(204); F.U32(0); F.U32(0); F.U32(0);
	F.End();
	F.Begin(113);
	F.At(4); F.U32(0); F.Text("vernice", 12);
	F.At(36); F.F32(0.5f); F.F32(0.5f); F.F32(0.5f);
	F.At(60); F.F32(0.2f); F.F32(0.2f); F.F32(0.2f);
	F.F32(64.0f); F.F32(1.0f);
	F.At(84);
	F.End();

	// La palette dei vertici: il cubo (lato 10 piedi), la torretta, le altre forme.
	const size_t Palette = F.Bytes.size();
	F.Begin(67); F.U32(0); F.End();
	std::vector<size_t> Cube;
	for (int I = 0; I < 8; ++I)
	{
		const double P[3] = { (I & 1) ? 5.0 : -5.0, (I & 2) ? 5.0 : -5.0, (I & 4) ? 5.0 : -5.0 };
		Cube.push_back(F.Vertex(Palette, P, nullptr, (I & 1) ? 1.0f : 0.0f, (I & 2) ? 1.0f : 0.25f));
	}
	const auto Tri = [&](double X, double Y, double Z, double Size)
	{
		const double A[3] = { X, Y, Z }, B[3] = { X + Size, Y, Z }, C[3] = { X, Y + Size, Z };
		return std::vector<size_t>{ F.Vertex(Palette, A, nullptr, 0, 0), F.Vertex(Palette, B, nullptr, 1, 0), F.Vertex(Palette, C, nullptr, 0, 1) };
	};
	const std::vector<size_t> Gun = Tri(0.0, 8.0, 5.0, 1.0);          // davanti alla torretta (Y = avanti)
	const std::vector<size_t> Intact = Tri(-20.0, 0.0, 0.0, 1.0);
	const std::vector<size_t> Wreck = Tri(-30.0, 0.0, 0.0, 1.0);
	const std::vector<size_t> FarTri = Tri(40.0, 0.0, 0.0, 1.0);
	std::vector<std::vector<size_t>> Frames;
	for (int I = 0; I < 3; ++I) { Frames.push_back(Tri(0.0, -20.0 - 5.0 * I, 0.0, 1.0)); }
	const std::vector<size_t> Glass = Tri(0.0, 0.0, 20.0, 1.0);
	const std::vector<size_t> Hidden = Tri(0.0, 0.0, 30.0, 1.0);
	const std::vector<size_t> Shared = Tri(0.0, 0.0, 0.0, 2.0);
	const std::vector<size_t> Lights = Tri(0.0, 0.0, 40.0, 1.0);
	// Vertice con la normale (69/70): per i colori dei vertici.
	const float Up[3] = { 0, 0, 1 };
	const double Lp[3] = { 1, 1, 50 };
	const size_t WithNormal = F.Vertex(Palette, Lp, Up, 0, 0);
	// La lunghezza totale della palette.
	const uint32_t PaletteLength = static_cast<uint32_t>(F.Bytes.size() - Palette);
	F.Bytes[Palette + 4] = static_cast<uint8_t>(PaletteLength >> 24); F.Bytes[Palette + 5] = static_cast<uint8_t>(PaletteLength >> 16);
	F.Bytes[Palette + 6] = static_cast<uint8_t>(PaletteLength >> 8); F.Bytes[Palette + 7] = static_cast<uint8_t>(PaletteLength);
	(void)WithNormal;

	// Le istanze: definite qui, usate due volte sotto.
	F.Begin(62); F.U16(0); F.U16(7); F.End();
	F.Push();
	F.Object("inst");
	F.Push();
	F.Face("s", FFlt::FFace{});
	F.Push(); F.VertexList(Shared); F.Pop();
	F.Pop();
	F.Pop();

	F.Push();   // i figli dell'intestazione
	F.Group("corpo");
	F.Push();
	F.Object("scafo");
	F.Push();
	// Le sei facce del cubo, antiorarie viste da fuori; quella sopra testurizzata (texwhite) con la vernice.
	const int Faces[6][4] = { { 4, 5, 7, 6 }, { 0, 2, 3, 1 }, { 1, 3, 7, 5 }, { 0, 4, 6, 2 }, { 2, 6, 7, 3 }, { 0, 1, 5, 4 } };
	for (int I = 0; I < 6; ++I)
	{
		FFlt::FFace Face;
		if (I == 0) { Face.Texture = 0; Face.bTexWhite = true; Face.Material = 0; Face.IrMaterial = 42; Face.ColorIndex = (1u << 7) | 127u; }
		else if (I == 1) { Face.ColorIndex = (1u << 7) | 127u; }             // rosso pieno
		else if (I == 2) { Face.ColorIndex = (1u << 7) | 63u; }              // rosso a meta'
		else if (I == 3) { Face.Flags = Flt::Bit(3); Face.Packed[0] = 0; Face.Packed[1] = 255; Face.Packed[2] = 0; }   // verde impacchettato
		else if (I == 4) { Face.Material = 0; }                                // bianco * 0,5
		F.Face("c" + std::to_string(I), Face);
		F.Push();
		std::vector<size_t> List;
		for (int C = 0; C < 4; ++C) { List.push_back(Cube[static_cast<size_t>(Faces[I][C])]); }
		if (I == 5)
		{
			// Un record spezzato: la lista dei vertici continua in un record 23.
			F.VertexList({ List[0], List[1] });
			F.Begin(23); F.U32(static_cast<uint32_t>(List[2])); F.U32(static_cast<uint32_t>(List[3])); F.End();
		}
		else { F.VertexList(List); }
		F.Pop();
	}
	// Una faccia trasparente a due lati, una nascosta, due punti luce.
	FFlt::FFace GlassFace;
	GlassFace.DrawType = 1;
	GlassFace.Transparency = 32768;
	F.Face("vetro", GlassFace);
	F.Push(); F.VertexList(Glass); F.Pop();
	FFlt::FFace HiddenFace;
	HiddenFace.Flags = Flt::Bit(5);
	F.Face("nascosta", HiddenFace);
	F.Push(); F.VertexList(Hidden); F.Pop();
	FFlt::FFace LightFace;
	LightFace.DrawType = 8;
	F.Face("luci", LightFace);
	F.Push(); F.VertexList({ Lights[0], Lights[1] }); F.Pop();
	F.Pop();   // scafo

	// La torretta: un DOF con l'origine sopra lo scafo, assi del modello.
	F.Begin(14);
	F.At(4); F.Text("torr", 8);
	F.At(16); F.F64(0); F.F64(0); F.F64(5);
	F.F64(1); F.F64(0); F.F64(5);
	F.F64(0); F.F64(1); F.F64(5);
	F.At(184); F.F64(-10); F.F64(20); F.F64(0); F.F64(1);       // beccheggio (At va solo avanti: in ordine)
	F.At(248); F.F64(-180); F.F64(180); F.F64(0); F.F64(1);     // imbardata
	F.At(376); F.U32(0); F.U32(0);
	F.End();
	// Il nome lungo.
	F.Begin(33); F.Text("torretta_principale", 20); F.End();
	F.Push();
	F.Object("canna");
	F.Push();
	F.Face("g", FFlt::FFace{});
	F.Push(); F.VertexList(Gun); F.Pop();
	F.Pop();
	F.Pop();

	// Lo switch del danno: intatto (figlio 0), distrutto (figlio 1).
	F.Begin(96);
	F.At(4); F.Text("danno", 8);
	F.At(16); F.U32(0); F.U32(1); F.U32(2); F.U32(0x1); F.U32(0x2);
	F.End();
	F.Push();
	F.Group("intatto");
	F.Push(); F.Face("i", FFlt::FFace{}); F.Push(); F.VertexList(Intact); F.Pop(); F.Pop();
	F.Group("rotto");
	F.Push(); F.Face("r", FFlt::FFace{}); F.Push(); F.VertexList(Wreck); F.Pop(); F.Pop();
	F.Pop();

	// Un LOD: visibile da 0 a 1000 piedi dal centro.
	F.Begin(73);
	F.At(4); F.Text("vicino", 8);
	F.At(16); F.F64(1000.0); F.F64(0.0);
	F.At(40); F.F64(0); F.F64(0); F.F64(0);
	F.At(80);
	F.End();
	F.Push(); F.Face("l", FFlt::FFace{}); F.Push(); F.VertexList(FarTri); F.Pop(); F.Pop();

	// Una sequenza (gruppo animato in avanti): tre fotogrammi.
	F.Group("rotore", Flt::Bit(1), 1.5f);
	F.Push();
	for (int I = 0; I < 3; ++I)
	{
		F.Object("fot" + std::to_string(I));
		F.Push(); F.Face("q", FFlt::FFace{}); F.Push(); F.VertexList(Frames[static_cast<size_t>(I)]); F.Pop(); F.Pop();
	}
	F.Pop();

	// Una mesh: pool locale (posizione, colore, normale, UV) e una striscia di 4 vertici.
	F.Face("mesh", FFlt::FFace{}, true);
	F.Push();
	F.Begin(85);
	F.U32(4); F.U32(Flt::Bit(0) | Flt::Bit(2) | Flt::Bit(3) | Flt::Bit(4));
	const double Strip[4][3] = { { 0, 0, 60 }, { 10, 0, 60 }, { 0, 10, 60 }, { 10, 10, 60 } };
	for (const auto& P : Strip) { F.F64(P[0]); F.F64(P[1]); F.F64(P[2]); F.Abgr(255, 255, 255); F.F32(0); F.F32(0); F.F32(1); F.F32(0); F.F32(0); }
	F.End();
	F.Begin(86); F.U16(1); F.U16(2); F.U32(4); F.U16(0); F.U16(1); F.U16(2); F.U16(3); F.End();
	F.Pop();

	// Due usi dell'istanza 7, spostati.
	F.Begin(61); F.U16(0); F.U16(7); F.End();
	F.Matrix(100.0, 0.0, 0.0);
	F.Begin(61); F.U16(0); F.U16(7); F.End();
	F.Matrix(200.0, 0.0, 0.0);

	// Un riferimento esterno, spostato di 100 piedi a destra (+X).
	F.Begin(63);
	F.At(4); F.Text("ruota.flt", 200);
	F.At(216);
	F.End();
	F.Matrix(100.0, 0.0, 0.0);

	F.Pop();   // corpo
	F.Pop();   // intestazione
	Disk.Put("/modelli/carro/carro.flt", F.Bytes);
	Disk.Put("/modelli/carro/textures/skin.rgb", std::string("x"));

	// Il file della ruota: metri, un triangolo.
	FFlt W;
	W.Header(0);
	const size_t WPalette = W.Bytes.size();
	W.Begin(67); W.U32(0); W.End();
	const double A[3] = { 0, 0, 0 }, B[3] = { 1, 0, 0 }, C[3] = { 0, 1, 0 };
	const std::vector<size_t> WheelTri = { W.Vertex(WPalette, A, nullptr, 0, 0), W.Vertex(WPalette, B, nullptr, 0, 0), W.Vertex(WPalette, C, nullptr, 0, 0) };
	const uint32_t WLength = static_cast<uint32_t>(W.Bytes.size() - WPalette);
	W.Bytes[WPalette + 6] = static_cast<uint8_t>(WLength >> 8); W.Bytes[WPalette + 7] = static_cast<uint8_t>(WLength);
	W.Push(); W.Object("ruota"); W.Push(); W.Face("w", FFlt::FFace{}); W.Push(); W.VertexList(WheelTri); W.Pop(); W.Pop(); W.Pop();
	Disk.Put("/modelli/carro/ruota.flt", W.Bytes);
}

static const FModelNode* FindNode(const FModel& M, const std::string& Name)
{
	for (const FModelNode& N : M.Nodes) { if (N.Name == Name) { return &N; } }
	return nullptr;
}

static void TestOpenFlight()
{
	Section("5. OpenFlight");
	FFakeDisk Disk;
	BuildTank(Disk);
	FModel Model;
	std::string Error;
	const bool bOk = ReadOpenFlightFile("/modelli/carro/carro.flt", Disk.Access(), Model, Error);
	Check(bOk && Model.Format == "OpenFlight 16.4", "si legge: la versione 16.4", bOk ? Model.Format : Error);
	if (!bOk) { return; }
	for (const std::string& W : Model.Warnings) { std::printf("        avviso: %s\n", W.c_str()); }

	const FModelNode* Turret = FindNode(Model, "torretta_principale");
	const FModelNode* Damage = FindNode(Model, "danno");
	const FModelNode* Lod = FindNode(Model, "vicino");
	const FModelNode* Rotor = FindNode(Model, "rotore");
	Check(Turret && Turret->Kind == ENodeKind::Dof && Damage && Damage->Kind == ENodeKind::Switch && Damage->MaskCount() == 2
		&& Lod && Lod->Kind == ENodeKind::Lod && Rotor && Rotor->Kind == ENodeKind::Sequence && Rotor->Children.size() == 3,
		"la gerarchia: DOF (con il nome lungo), switch a due maschere, LOD, sequenza di tre fotogrammi");
	Check(Turret && std::fabs(Turret->Dof.Frame.T[2] - 1.524) < 1e-9 && Turret->Dof.Rotate[2].Min == -180.0 && Turret->Dof.Rotate[0].Max == 20.0,
		"il DOF: l'origine a 5 piedi = 1,524 m, i limiti dell'imbardata e del beccheggio");
	Check(Lod && std::fabs(Lod->SwitchIn - 304.8) < 1e-9 && Lod->SwitchOut == 0.0, "il LOD: 1000 piedi = 304,8 m");
	Check(Model.Textures.size() == 1 && Model.Textures[0].Resolved == "/modelli/carro/textures/skin.rgb",
		"la texture di 'C:/vecchia/macchina' trovata in textures/ accanto al modello");
	Check(Model.LightPoints.size() == 2, "i due punti luce tenuti da parte (per le luci, M7)");

	// I colori e i materiali del cubo.
	const FModelNode* Hull = FindNode(Model, "scafo");
	std::map<std::string, std::vector<int>> Colours;
	bool bTextured = false, bIr = false, bTwoSided = false, bTranslucent = false, bEmissive = false;
	int Triangles = 0;
	if (Hull)
	{
		for (const int32_t MeshIndex : Hull->Meshes)
		{
			const FModelMesh& Mesh = Model.Meshes[static_cast<size_t>(MeshIndex)];
			const FModelMaterial& M = Model.Materials[static_cast<size_t>(Mesh.Material)];
			Triangles += static_cast<int>(Mesh.TriangleCount());
			bTextured = bTextured || M.Texture == 0;
			bIr = bIr || M.IrMaterial == 42;
			bTwoSided = bTwoSided || M.bTwoSided;
			bTranslucent = bTranslucent || M.bTranslucent;
			bEmissive = bEmissive || std::fabs(M.Emissive[0] - 0.2f) < 1e-6f;
			for (const FModelVertex& V : Mesh.Vertices)
			{
				Colours[std::to_string(V.Rgba[0]) + "," + std::to_string(V.Rgba[1]) + "," + std::to_string(V.Rgba[2])].push_back(V.Rgba[3]);
			}
		}
	}
	Check(Triangles == 13, "lo scafo: 6 facce = 12 triangoli, piu' il vetro; la faccia nascosta no; i punti luce no", Fmt("%.0f triangoli", Triangles));
	Check(bTextured && bIr && bTwoSided && bTranslucent && bEmissive,
		"la faccia testurizzata (codice IR 42, materiale luminoso), il vetro a due lati e trasparente");
	Check(Colours.count("255,0,0") && Colours.count("126,0,0") && Colours.count("0,255,0") && Colours.count("128,128,128"),
		"i colori: rosso dalla palette, rosso a intensita' 63/127, verde impacchettato (ABGR), bianco per il materiale a 0,5");
	Check(Colours.count("128,128,128") && Colours["128,128,128"].size() >= 8,
		"la faccia testurizzata con texwhite: bianca per il materiale (non rossa)");

	// Il rig.
	const FModelRig Rig = BuildRig(Model);
	const int32_t TurretPart = Rig.FindPart("torretta_principale");
	const int32_t Intact = Rig.FindPart("danno/intatto");
	const int32_t Wreck = Rig.FindPart("danno/rotto");
	const int32_t Near = Rig.FindPart("vicino");
	const int32_t Frame0 = Rig.FindPart("rotore/fot0");
	Check(TurretPart > 0 && Intact > 0 && Wreck > 0 && Near > 0 && Frame0 > 0 && Rig.Switches.size() == 2 && Rig.DofCount() == 1,
		"le parti: radice, il DOF, i due figli dello switch, il LOD, i fotogrammi; due selettori (lo switch e la sequenza)",
		Fmt("%.0f parti", static_cast<double>(Rig.Parts.size())));

	// Il cubo nell'attore: lato 3,048 m, triangoli verso fuori.
	bool bOut = true;
	double Worst = 1.0;
	for (const FRigSection& S : Rig.Parts[0].Sections)
	{
		if (Rig.Materials[static_cast<size_t>(S.Material)].bTwoSided) { continue; }
		double W;
		bOut = bOut && WindingOutward(S, &W);
		Worst = std::min(Worst, W);
	}
	Check(bOut, "nell'attore i triangoli dello scafo guardano fuori", Fmt("coseno peggiore %.3f", Worst));
	// La faccia di sopra (quella con la texture): a 5 piedi = 1,524 m, larga da -1,524 a +1,524.
	bool bTopFace = false, bTopRight = true;
	for (const FRigSection& S : Rig.Parts[0].Sections)
	{
		if (Rig.Materials[static_cast<size_t>(S.Material)].Texture != 0) { continue; }
		bTopFace = !S.Vertices.empty();
		for (const FRigVertex& V : S.Vertices)
		{
			bTopRight = bTopRight && std::fabs(V.P[2] - 1.524f) < 1e-4f && std::fabs(std::fabs(V.P[0]) - 1.524f) < 1e-4f
				&& std::fabs(std::fabs(V.P[1]) - 1.524f) < 1e-4f && V.N[2] > 0.999f;
		}
	}
	Check(bTopFace && bTopRight, "il cubo di 10 piedi: la faccia di sopra a 1,524 m, larga 3,048 m, con la normale in su");

	// Il DOF: a riposo la canna e' avanti (X dell'attore); con imbardata +90 (matematica, OpenFlight) va a SINISTRA.
	if (TurretPart > 0)
	{
		const FRigPart& P = Rig.Parts[static_cast<size_t>(TurretPart)];
		const FRigVertex& Muzzle = P.Sections[0].Vertices[0];   // (0, 8, 5) piedi del file
		const double In[3] = { Muzzle.P[0], Muzzle.P[1], Muzzle.P[2] };
		double Rest[3], Turned[3], Pivot[3];
		FDofValues V = FDofValues::Current(P.Dof);
		PartTransform(P, V).ApplyPoint(In, Rest);
		V.Rotate[2] = 90.0;
		PartTransform(P, V).ApplyPoint(In, Turned);
		const double PivotPart[3] = { 0.0, 0.0, 1.524 };
		PartTransform(P, V).ApplyPoint(PivotPart, Pivot);
		const double Expected[3] = { 0.0, -2.4384, 1.524 };
		const float Turnedf[3] = { static_cast<float>(Turned[0]), static_cast<float>(Turned[1]), static_cast<float>(Turned[2]) };
		Check(std::fabs(Rest[0] - 2.4384) < 1e-6 && std::fabs(Rest[1]) < 1e-6, "a riposo la canna e' 8 piedi avanti (X dell'attore)",
			Fmt("%.4f %.4f %.4f", Rest[0], Rest[1], Rest[2]));
		Check(Distance3(Turnedf, Expected) < 1e-6, "imbardata del DOF +90: la canna va a SINISTRA (gli angoli di OpenFlight sono matematici)",
			Fmt("%.4f %.4f %.4f", Turned[0], Turned[1], Turned[2]));
		Check(std::fabs(Pivot[2] - 1.524) < 1e-9 && std::fabs(Pivot[0]) < 1e-9 && std::fabs(Pivot[1]) < 1e-9,
			"e il perno (l'origine del DOF) non si muove");
		const FDofValues Clamped = ClampDof(P.Dof, FDofValues{ { 0, 0, 0 }, { 45.0, 0.0, 0.0 }, { 1, 1, 1 } });
		Check(Clamped.Rotate[0] == 20.0, "il beccheggio oltre il limite si ferma a 20 gradi");
	}

	// Switch, LOD, sequenza.
	FRigState State = DefaultRigState(Rig);
	const double Here[3] = { 100.0, 0.0, 0.0 }, Far[3] = { 400.0, 0.0, 0.0 };
	Check(Intact > 0 && IsPartVisible(Rig, Rig.Parts[static_cast<size_t>(Intact)], State, Here)
		&& !IsPartVisible(Rig, Rig.Parts[static_cast<size_t>(Wreck)], State, Here),
		"lo switch del danno: con la maschera 0 si vede l'intatto, non il rotto");
	const int32_t DamageSwitch = Rig.FindSwitch("danno");
	if (DamageSwitch >= 0) { State.Masks[static_cast<size_t>(DamageSwitch)] = 1; }
	Check(DamageSwitch >= 0 && !IsPartVisible(Rig, Rig.Parts[static_cast<size_t>(Intact)], State, Here)
		&& IsPartVisible(Rig, Rig.Parts[static_cast<size_t>(Wreck)], State, Here),
		"con la maschera 1 il contrario");
	Check(Near > 0 && IsPartVisible(Rig, Rig.Parts[static_cast<size_t>(Near)], State, Here) && !IsPartVisible(Rig, Rig.Parts[static_cast<size_t>(Near)], State, Far),
		"il LOD: visibile a 100 m dal centro, non a 400 (304,8 m)");
	const int32_t Frame1 = Rig.FindPart("rotore/fot1");
	Check(Frame0 > 0 && Frame1 > 0 && IsPartVisible(Rig, Rig.Parts[static_cast<size_t>(Frame0)], State, Here)
		&& !IsPartVisible(Rig, Rig.Parts[static_cast<size_t>(Frame1)], State, Here),
		"la sequenza: si vede il fotogramma 0 (le farà girare M3)");

	// Mesh, istanze, riferimento esterno, UV.
	int MeshTriangles = 0;
	bool bMeshUp = false;
	int SharedCopies = 0;
	bool bWheel = false;
	bool bUvFlipped = false;
	for (const FRigSection& S : Rig.Parts[0].Sections)
	{
		for (size_t I = 0; I < S.Vertices.size(); ++I)
		{
			const FRigVertex& V = S.Vertices[I];
			if (std::fabs(V.P[2] - 18.288f) < 1e-3f && V.N[2] > 0.99f) { bMeshUp = true; }
			// Le istanze: il triangolo (0,0,0)-(2,0,0) piedi spostato di 100 e 200 piedi a destra: Y dell'attore 30,48 e 60,96.
			if (std::fabs(V.P[0]) < 1e-4f && std::fabs(V.P[2]) < 1e-4f && (std::fabs(V.P[1] - 30.48f) < 1e-3f || std::fabs(V.P[1] - 60.96f) < 1e-3f)) { ++SharedCopies; }
			// La ruota (metri) spostata di 100 piedi a destra.
			if (std::fabs(V.P[1] - 31.48f) < 1e-3f && std::fabs(V.P[0]) < 1e-4f) { bWheel = true; }
			if (std::fabs(V.UV[1] - 0.75f) < 1e-6f) { bUvFlipped = true; }
		}
		for (size_t I = 0; I + 2 < S.Indices.size(); I += 3)
		{
			if (std::fabs(S.Vertices[S.Indices[I]].P[2] - 18.288f) < 1e-3f) { ++MeshTriangles; }
		}
		double W;
		if (!Rig.Materials[static_cast<size_t>(S.Material)].bTwoSided) { bOut = bOut && WindingOutward(S, &W); }
	}
	Check(MeshTriangles == 2 && bMeshUp && bOut, "la mesh: una striscia di 4 vertici = 2 triangoli, verso l'alto, girati bene");
	Check(SharedCopies >= 2, "l'istanza usata due volte, nei due posti delle matrici (100 e 200 piedi)", Fmt("%.0f vertici", SharedCopies));
	Check(bWheel, "il riferimento esterno (un .flt in metri) sotto il suo nodo, spostato di 100 piedi = 30,48 m a destra");
	Check(bUvFlipped, "le UV girate: V = 0,25 del file e' 0,75 in Unreal");
	Check(Rig.Lights.size() == 2, "i punti luce nel rig");
}

static void TestLoaderAndTable()
{
	Section("6. Il caricatore e la tabella dei modelli");
	FFakeDisk Disk;
	FModel Model;
	std::string Error;
	Check(!LoadModelFile("/x/aereo.fbx", Disk.Access(), Model, Error) && Error.find("glTF") != std::string::npos,
		"un .fbx: dice di convertirlo (FBX e' chiuso)", Error);
	Check(!LoadModelFile("/x/aereo.ive", Disk.Access(), Model, Error) && Error.find("osgconv") != std::string::npos,
		"un .ive: osgconv, in .flt", Error);
	Disk.Put("/x/finto.flt", std::string("non sono un modello"));
	Check(!LoadModelFile("/x/finto.flt", Disk.Access(), Model, Error), "un .flt che non e' OpenFlight: un errore", Error);

	using GeoWorld::Entities::FModelTable;
	using GeoWorld::Entities::FModelTableError;
	using GeoWorld::Entities::FEntityKind;
	FModelTable Table;
	std::vector<FModelTableError> Errors;
	Table.Parse(
		"1.2.105.1   F16/f16.flt yaw 90\n"
		"1.1.105.81  \"auto rossa/auto.obj\" scale 0.01 up z forward x offset 0 0 -0.3\n"
		"1.1.105.1   carro.flt scale 0\n"
		"1.1.105.2   carro.flt up z\n"
		"1.1.105.3   carro.flt up z forward -z\n"
		"1.1.105.4   carro.flt colore rosso\n", Errors);
	const auto F16 = Table.Find(FEntityKind{ 1, 2, 105, 1, 0, 0, 0 });
	const auto Car = Table.Find(FEntityKind{ 1, 1, 105, 81, 0, 0, 0 });
	Check(F16.Rule && F16.Rule->Model.IsFile() && F16.Rule->Model.File == "F16/f16.flt" && F16.Rule->Model.FileOptions.YawDeg == 90.0,
		"una regola file: il percorso e l'imbardata");
	Check(Car.Rule && Car.Rule->Model.File == "auto rossa/auto.obj" && Car.Rule->Model.FileOptions.Scale == 0.01
		&& Car.Rule->Model.FileOptions.bOverrideAxes && Car.Rule->Model.FileOptions.Axes.Up == EAxis::PosZ
		&& Car.Rule->Model.FileOptions.Axes.Forward == EAxis::PosX && Car.Rule->Model.FileOptions.Offset[2] == -0.3,
		"fra virgolette con gli spazi; scala, assi, spostamento");
	Check(Errors.size() == 4 && Errors[0].Line == 3 && Errors[1].Line == 4 && Errors[2].Line == 5 && Errors[3].Line == 6,
		"le correzioni sbagliate: scala 0, up senza forward, due volte lo stesso asse, un'opzione che non c'e'",
		Errors.empty() ? std::string() : Errors[0].Message + " / " + Errors[3].Message);
}

int main()
{
	std::printf("=====================================================\n");
	std::printf(" GeoModels -- OBJ, OpenFlight, SGI, il rig\n");
	std::printf("=====================================================\n");
	TestAffineAndAxes();
	TestSgi();
	TestPaths();
	TestObj();
	TestOpenFlight();
	TestLoaderAndTable();
	std::printf("\n=====================================================\n");
	std::printf(" RISULTATO: %d passati, %d falliti\n", GPassed, GFailed);
	std::printf("=====================================================\n");
	return GFailed == 0 ? 0 : 1;
}

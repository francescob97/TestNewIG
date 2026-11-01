// =============================================================================
//  Test di glTF (docs/modelli-design.md, M2), senza Unreal: il lettore JSON,
//  i quaternioni e le curve delle animazioni, e quattro modelli glTF costruiti
//  qui (documento e buffer byte per byte, come la specifica 2.0):
//
//    1. una freccia (.gltf + .bin esterno con lo spazio nel nome): gli assi
//       (+Z avanti, +Y alto, -X destra), i triangoli verso fuori, le normali
//       piatte, le UV non girate, i colori lineari, i materiali PBR;
//    2. un carro (.glb): la gerarchia, una torretta animata (rotazione
//       lineare, traslazione a gradino, cubica), un osso vuoto, la texture
//       DENTRO il file, il ritaglio dell'alfa, una luce;
//    3. uno scheletro: la mesh nella posa di riposo, nel sistema della radice;
//    4. interi quantizzati, strisce, ventagli, una parte "sparse", le linee;
//
//  e i file sbagliati (compressione Draco, glTF 1.0, .glb troncato, accessor
//  fuori dal buffer, indici oltre i vertici, buffer che manca, JSON rotto):
//  un errore che dice cosa fare, mai un crash.
//
//  E un .glb scritto da BLENDER (TestData/blender_tank.glb, fatto da
//  make_blender_tank.py): un esportatore vero, con un'animazione vera.
//
//      geogltf_tests [Source/GeoTiles/TestData/blender_tank.glb]
// =============================================================================
#include "Entities/EntityModels.h"
#include "Models/GltfReader.h"
#include "Models/Json.h"
#include "Models/ModelData.h"
#include "Models/ModelFiles.h"
#include "Models/ModelLoader.h"
#include "Models/ModelRig.h"

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
	if (bCondition) { ++GPassed; std::printf("  [ ok ] %s%s%s\n", What.c_str(), Detail.empty() ? "" : "  -> ", Detail.c_str()); }
	else { ++GFailed; std::printf("  [FAIL] %s%s%s\n", What.c_str(), Detail.empty() ? "" : "  -> ", Detail.c_str()); }
}

static void Section(const char* Title) { std::printf("\n== %s ==\n", Title); }

static std::string Fmt(const char* Format, double A, double B = 0.0, double C = 0.0, double D = 0.0)
{
	char Buffer[240];
	std::snprintf(Buffer, sizeof(Buffer), Format, A, B, C, D);
	return Buffer;
}

static bool Near(double A, double B, double Tolerance = 1e-6) { return std::fabs(A - B) <= Tolerance; }

static bool Near3(const double A[3], double X, double Y, double Z, double Tolerance = 1e-5)
{
	return Near(A[0], X, Tolerance) && Near(A[1], Y, Tolerance) && Near(A[2], Z, Tolerance);
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

// --- Un glTF, pezzo per pezzo ------------------------------------------------------

static std::string Base64(const std::vector<uint8_t>& In)
{
	static const char* Table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	std::string Out;
	for (size_t I = 0; I < In.size(); I += 3)
	{
		const uint32_t B0 = In[I], B1 = I + 1 < In.size() ? In[I + 1] : 0, B2 = I + 2 < In.size() ? In[I + 2] : 0;
		const uint32_t V = B0 << 16 | B1 << 8 | B2;
		Out += Table[V >> 18 & 63];
		Out += Table[V >> 12 & 63];
		Out += I + 1 < In.size() ? Table[V >> 6 & 63] : '=';
		Out += I + 2 < In.size() ? Table[V & 63] : '=';
	}
	return Out;
}

template <typename T>
static std::vector<uint8_t> RawBytes(const std::vector<T>& Values)
{
	std::vector<uint8_t> Out(Values.size() * sizeof(T));
	if (!Out.empty()) { std::memcpy(Out.data(), Values.data(), Out.size()); }
	return Out;
}

struct FGltf
{
	std::vector<uint8_t> Bin;
	std::vector<std::string> Views, Accessors;

	int View(const std::vector<uint8_t>& Bytes, int Stride = 0)
	{
		while (Bin.size() % 4 != 0) { Bin.push_back(0); }
		const size_t Offset = Bin.size();
		Bin.insert(Bin.end(), Bytes.begin(), Bytes.end());
		std::string V = "{\"buffer\":0,\"byteOffset\":" + std::to_string(Offset) + ",\"byteLength\":" + std::to_string(Bytes.size());
		if (Stride > 0) { V += ",\"byteStride\":" + std::to_string(Stride); }
		Views.push_back(V + "}");
		return static_cast<int>(Views.size()) - 1;
	}

	int Accessor(int ViewIndex, int ComponentType, const char* Type, size_t Count, bool bNormalized = false, const std::string& Extra = std::string())
	{
		std::string A = "{";
		if (ViewIndex >= 0) { A += "\"bufferView\":" + std::to_string(ViewIndex) + ","; }
		A += "\"componentType\":" + std::to_string(ComponentType) + ",\"type\":\"" + Type + "\",\"count\":" + std::to_string(Count);
		if (bNormalized) { A += ",\"normalized\":true"; }
		A += Extra + "}";
		Accessors.push_back(A);
		return static_cast<int>(Accessors.size()) - 1;
	}

	int Floats(const std::vector<float>& V, const char* Type, int Components)
	{
		return Accessor(View(RawBytes(V)), 5126, Type, V.size() / static_cast<size_t>(Components));
	}

	int Indices(const std::vector<uint16_t>& V) { return Accessor(View(RawBytes(V)), 5123, "SCALAR", V.size()); }

	std::string Json(const std::string& BufferUri, const std::string& Rest) const
	{
		std::string Buffer = "{\"byteLength\":" + std::to_string(Bin.size());
		if (!BufferUri.empty()) { Buffer += ",\"uri\":\"" + BufferUri + "\""; }
		Buffer += "}";
		std::string Out = "{\"asset\":{\"version\":\"2.0\",\"generator\":\"test di GeoWorld\"},\"buffers\":[" + Buffer + "],\"bufferViews\":[";
		for (size_t I = 0; I < Views.size(); ++I) { Out += (I ? "," : "") + Views[I]; }
		Out += "],\"accessors\":[";
		for (size_t I = 0; I < Accessors.size(); ++I) { Out += (I ? "," : "") + Accessors[I]; }
		Out += "]";
		if (!Rest.empty()) { Out += "," + Rest; }
		return Out + "}";
	}

	std::string DataUri() const { return "data:application/octet-stream;base64," + Base64(Bin); }

	std::vector<uint8_t> Glb(const std::string& Rest) const
	{
		std::string J = Json(std::string(), Rest);
		while (J.size() % 4 != 0) { J += ' '; }
		std::vector<uint8_t> B = Bin;
		while (B.size() % 4 != 0) { B.push_back(0); }
		std::vector<uint8_t> Out;
		const auto U32 = [&Out](uint32_t V) { for (int I = 0; I < 4; ++I) { Out.push_back(static_cast<uint8_t>(V >> (8 * I))); } };
		U32(0x46546C67u);
		U32(2);
		U32(static_cast<uint32_t>(12 + 8 + J.size() + 8 + B.size()));
		U32(static_cast<uint32_t>(J.size()));
		U32(0x4E4F534Au);
		Out.insert(Out.end(), J.begin(), J.end());
		U32(static_cast<uint32_t>(B.size()));
		U32(0x004E4942u);
		Out.insert(Out.end(), B.begin(), B.end());
		return Out;
	}
};

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

/** Il vertice di una parte piu' avanti (X massima, nel sistema della parte). */
static const FRigVertex* Foremost(const FRigPart& Part)
{
	const FRigVertex* Best = nullptr;
	for (const FRigSection& S : Part.Sections)
	{
		for (const FRigVertex& V : S.Vertices) { if (!Best || V.P[0] > Best->P[0]) { Best = &V; } }
	}
	return Best;
}

/** Dove sta un vertice di una parte nella radice, con quello stato. */
static void InRoot(const FModelRig& Rig, const FRigState& State, int32_t Part, const FRigVertex& V, double Out[3])
{
	const double P[3] = { V.P[0], V.P[1], V.P[2] };
	PartToRoot(Rig, State, Part).ApplyPoint(P, Out);
}

// ===========================================================================

static void TestJson()
{
	Section("1. Il lettore JSON");
	Json::FValue V;
	std::string Error;
	const std::string Text =
		"\xEF\xBB\xBF{ \"a\" : [1, -2.5e2, 0.125, true, false, null],\n"
		"  \"s\": \"\\u00e8 \\\"q\\\" \\\\ \\n\", \"u\": \"\\ud83d\\ude00\", \"n\": {\"x\": {}}, \"pi\": 3.14159265358979, \"tiny\": 1e-7, \"z\": -0.0 }";
	const bool bOk = Json::Parse(Text, V, Error);
	Check(bOk && V["a"].Size() == 6 && V["a"][static_cast<size_t>(1)].Number == -250.0 && V["a"][static_cast<size_t>(2)].Number == 0.125
		&& V["a"][static_cast<size_t>(3)].Bool && !V["a"][static_cast<size_t>(4)].Bool && V["a"][static_cast<size_t>(5)].IsNull(),
		"numeri, booleani, null (e il BOM all'inizio)", Error);
	Check(V["s"].String == "\xC3\xA8 \"q\" \\ \n" && V["u"].String == "\xF0\x9F\x98\x80",
		"gli escape: \\u00e8 in UTF-8, le virgolette, la barra, la coppia surrogata (una faccina, 4 byte)");
	Check(Near(V["pi"].Number, 3.14159265358979, 1e-15) && Near(V["tiny"].Number, 1e-7, 1e-22) && V["n"]["x"].IsObject(),
		"i numeri letti a mano (senza strtod e il suo locale): 15 cifre giuste", Fmt("%.17g", V["pi"].Number));
	Check(V["manca"]["ancora"][static_cast<size_t>(3)].IsNull() && V["a"][static_cast<size_t>(2)].IndexOr() == -1 && V["a"][static_cast<size_t>(0)].IndexOr() == 1,
		"l'accesso tollerante: quello che non c'e' e' null; 0.125 non e' un indice, 1 si'");
	Json::FValue Bad;
	std::string E1, E2, E3, E4;
	const bool bBad1 = !Json::Parse(std::string("[1, 2"), Bad, E1);
	const bool bBad2 = !Json::Parse(std::string("{\"a\" 1}"), Bad, E2);
	const bool bBad3 = !Json::Parse(std::string("1 2"), Bad, E3);
	const bool bBad4 = !Json::Parse(std::string(300, '['), Bad, E4);
	Check(bBad1 && bBad2 && bBad3 && bBad4 && E2.find("riga 1") != std::string::npos && E4.find("annidato") != std::string::npos,
		"i documenti sbagliati: un errore con riga e colonna; 300 parentesi non fanno saltare lo stack", E2 + " / " + E4);
}

static void TestMath()
{
	Section("2. Quaternioni, pose, curve");
	const double Angle = 0.9;
	const double Q[4] = { 0, std::sin(Angle / 2), 0, std::cos(Angle / 2) };
	const FAffine A = QuaternionMatrix(Q), B = FAffine::Rotation(1, Angle);
	double Worst = 0.0;
	for (int I = 0; I < 3; ++I) { for (int J = 0; J < 3; ++J) { Worst = std::max(Worst, std::fabs(A.M[I][J] - B.M[I][J])); } }
	Check(Worst < 1e-12, "il quaternione di una rotazione attorno a Y e' la stessa matrice", Fmt("%.1e", Worst));

	FTrs Pose;
	Pose.T[0] = 1.5; Pose.T[1] = -2; Pose.T[2] = 0.25;
	const double Axis[3] = { 0.3, -0.5, 0.81 };
	const double L = std::sqrt(Axis[0] * Axis[0] + Axis[1] * Axis[1] + Axis[2] * Axis[2]);
	for (int I = 0; I < 3; ++I) { Pose.R[I] = std::sin(1.1) * Axis[I] / L; }
	Pose.R[3] = std::cos(1.1);
	Pose.S[0] = 2; Pose.S[1] = 0.5; Pose.S[2] = 3;
	const FAffine M = TrsMatrix(Pose);
	const FAffine Back = TrsMatrix(MatrixToTrs(M));
	Worst = 0.0;
	for (int I = 0; I < 3; ++I) { for (int J = 0; J < 3; ++J) { Worst = std::max(Worst, std::fabs(M.M[I][J] - Back.M[I][J])); } Worst = std::max(Worst, std::fabs(M.T[I] - Back.T[I])); }
	Check(Worst < 1e-12, "da matrice a traslazione/rotazione/scala e ritorno", Fmt("%.1e", Worst));

	const double Q0[4] = { 0, 0, 0, 1 };
	const double Q90[4] = { 0, 0, std::sin(3.14159265358979 / 4), std::cos(3.14159265358979 / 4) };
	const double Q90Negated[4] = { -Q90[0], -Q90[1], -Q90[2], -Q90[3] };
	double Mid[4], MidNegated[4];
	Slerp(Q0, Q90, 0.5, Mid);
	Slerp(Q0, Q90Negated, 0.5, MidNegated);
	const double Expected = std::sin(3.14159265358979 / 8);
	Check(Near(Mid[2], Expected, 1e-12) && Near(std::fabs(MidNegated[2]), Expected, 1e-12) && MidNegated[3] > 0.0,
		"slerp: a meta' fra 0 e 90 gradi sono 45; con il quaternione opposto (stessa rotazione) prende la via corta");

	FAnimCurve Curve;
	Curve.Path = EAnimPath::Translation;
	Curve.Times = { 1.0f, 3.0f };
	Curve.Values = { 0, 0, 0, 10, -4, 2 };
	double V0[4], V1[4], V2[4];
	SampleCurve(Curve, 2.0, V0);
	SampleCurve(Curve, 0.0, V1);
	SampleCurve(Curve, 9.0, V2);
	Check(Near3(V0, 5, -2, 1) && Near3(V1, 0, 0, 0) && Near3(V2, 10, -4, 2),
		"una curva lineare: a meta', prima dell'inizio (la prima chiave), dopo la fine (l'ultima)");
}

// --- 3. La freccia: .gltf con il .bin accanto ---------------------------------------

static void TestArrow()
{
	Section("3. Una freccia in .gltf: assi, triangoli, UV, colori, materiali");
	FGltf G;
	// Il triangolo piatto: la punta verso +Z (l'avanti di glTF), antiorario visto da +Y (l'alto).
	const int Pos = G.Floats({ 1, 0, -2, -1, 0, -2, 0, 0, 3 }, "VEC3", 3);
	const int Nor = G.Floats({ 0, 1, 0, 0, 1, 0, 0, 1, 0 }, "VEC3", 3);
	const int Uv = G.Floats({ 0, 0, 1, 0, 0.5f, 1 }, "VEC2", 2);
	const int Idx = G.Indices({ 0, 1, 2 });
	// La pinna: in piedi sul piano x = 0, verso +X (la SINISTRA di glTF), senza normali.
	const int Fin = G.Floats({ 0, 0, -2, 0, 1.5f, -2, 0, 0, 0 }, "VEC3", 3);
	const std::string Rest =
		"\"nodes\":[{\"name\":\"freccia\",\"mesh\":0}],\"scenes\":[{\"nodes\":[0]}],"
		"\"meshes\":[{\"primitives\":["
		"{\"attributes\":{\"POSITION\":" + std::to_string(Pos) + ",\"NORMAL\":" + std::to_string(Nor) + ",\"TEXCOORD_0\":" + std::to_string(Uv) + "},\"indices\":" + std::to_string(Idx) + ",\"material\":0},"
		"{\"attributes\":{\"POSITION\":" + std::to_string(Fin) + "}}]}],"
		"\"materials\":[{\"name\":\"rosso\",\"pbrMetallicRoughness\":{\"baseColorFactor\":[0.25,0,0,1],\"metallicFactor\":0,\"roughnessFactor\":0.5,"
		"\"baseColorTexture\":{\"index\":0}}}],"
		"\"textures\":[{\"source\":0}],\"images\":[{\"uri\":\"tex/rosso%201.png\"}]";
	FFakeDisk Disk;
	Disk.Put("/m/freccia/freccia.gltf", G.Json("dati%20freccia.bin", Rest));
	Disk.Put("/m/freccia/dati freccia.bin", G.Bin);
	Disk.Put("/m/freccia/tex/rosso 1.png", std::string("finto png"));
	FModel Model;
	std::string Error;
	const bool bOk = LoadModelFile("/m/freccia/freccia.gltf", Disk.Access(), Model, Error);
	Check(bOk && Model.Format.rfind("glTF 2.0", 0) == 0 && Model.TriangleCount() == 2,
		"si legge: il .bin accanto (\"dati%20freccia.bin\" e' \"dati freccia.bin\"), due triangoli", bOk ? Model.Format : Error);
	if (!bOk) { return; }
	Check(Model.Textures.size() == 1 && Model.Textures[0].Resolved == "/m/freccia/tex/rosso 1.png",
		"la texture con lo spazio nel nome, trovata", Model.Textures.empty() ? std::string() : Model.Textures[0].Resolved);

	const FModelRig Rig = BuildRig(Model);
	Check(Rig.Parts.size() == 1, "una parte sola (niente si muove)", Fmt("%.0f parti", static_cast<double>(Rig.Parts.size())));
	const FRigPart& Root = Rig.Parts[0];
	const FRigSection* Flat = nullptr;
	const FRigSection* Fin2 = nullptr;
	for (const FRigSection& S : Root.Sections) { (Rig.Materials[static_cast<size_t>(S.Material)].Name == "rosso" ? Flat : Fin2) = &S; }
	if (!Flat || !Fin2) { Check(false, "le due sezioni (rosso e default)"); return; }
	bool bTip = false, bRight = false, bTop = false;
	for (const FRigVertex& V : Flat->Vertices)
	{
		const double P[3] = { V.P[0], V.P[1], V.P[2] };
		if (Near3(P, 3, 0, 0)) { bTip = true; Check(Near(V.UV[0], 0.5) && Near(V.UV[1], 1.0), "le UV come sono: glTF ha l'origine in alto come Unreal (la punta resta V = 1)"); }
		if (Near3(P, -2, -1, 0)) { bRight = true; }
	}
	for (const FRigVertex& V : Fin2->Vertices) { const double P[3] = { V.P[0], V.P[1], V.P[2] }; if (Near3(P, -2, 0, 1.5)) { bTop = true; } }
	Check(bTip && bTop, "l'avanti del file (+Z) e' l'avanti dell'attore (+X), l'alto (+Y) e' l'alto (+Z): la punta a 3 m, la pinna alta 1,5 m");
	Check(bRight, "e +X di glTF e' la SINISTRA: il vertice (1, 0, -2) va a (-2, -1, 0) nell'attore (Y dell'attore e' la destra)");
	double WorstFlat = 0.0, WorstFin = 0.0;
	const bool bOutFlat = WindingOutward(*Flat, &WorstFlat);
	const bool bOutFin = WindingOutward(*Fin2, &WorstFin);
	Check(bOutFlat && bOutFin && Flat->Vertices[0].N[2] > 0.99f && Fin2->Vertices[0].N[1] < -0.99f,
		"triangoli verso fuori nell'attore; il piatto guarda in su, la pinna (normali calcolate, piatte) a sinistra",
		Fmt("coseni peggiori %.3f %.3f", WorstFlat, WorstFin));
	const FModelMaterial& Red = Rig.Materials[static_cast<size_t>(Flat->Material)];
	const FModelMaterial& Default = Rig.Materials[static_cast<size_t>(Fin2->Material)];
	Check(Flat->Vertices[0].Rgba[0] == 128 && Flat->Vertices[0].Rgba[1] == 0 && Flat->Vertices[0].Rgba[3] == 255,
		"il colore di base lineare 0,25 diventa 128 nei vertici (radice quadrata: il materiale lo rieleva al quadrato)",
		Fmt("%.0f", Flat->Vertices[0].Rgba[0]));
	Check(Red.Metallic == 0.0f && Red.Roughness == 0.5f && Red.AlphaMode == EAlphaMode::Opaque && Red.Texture == 0
		&& Default.Metallic == 1.0f && Default.Roughness == 1.0f,
		"i materiali PBR: metallo e rugosita' del file; senza materiale quello di default di glTF (metallo 1, rugosita' 1)");
}

// --- 4. Il carro: .glb con la gerarchia, le animazioni, la texture dentro -------------

static std::vector<uint8_t> BuildTank(int& OutAnimations)
{
	FGltf G;
	const int HullPos = G.Floats({ 1, 0, -1, -1, 0, -1, 0, 0, 1 }, "VEC3", 3);
	const int Up = G.Floats({ 0, 1, 0, 0, 1, 0, 0, 1, 0 }, "VEC3", 3);
	const int HullUv = G.Floats({ 0, 0, 1, 0, 0.5f, 1 }, "VEC2", 2);
	const int Tri = G.Indices({ 0, 1, 2 });
	const int GunPos = G.Floats({ 0.1f, 0, 0, -0.1f, 0, 0, 0, 0, 2 }, "VEC3", 3);
	const std::vector<uint8_t> Png = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n', 'f', 'i', 'n', 't', 'o' };
	const int Image = G.View(Png);
	// Le animazioni: la torretta gira di 120 gradi in 2 s (lineare), sale a gradino, scivola con una cubica.
	const int TwoSeconds = G.Floats({ 0, 2 }, "SCALAR", 1);
	const double S60 = std::sin(3.14159265358979 / 3), C60 = std::cos(3.14159265358979 / 3);
	const int Turn = G.Floats({ 0, 0, 0, 1, 0, static_cast<float>(S60), 0, static_cast<float>(C60) }, "VEC4", 4);
	const int Bone = G.Floats({ 0, 0, 0, 0, 5, 0 }, "VEC3", 3);
	const int OneSecond = G.Floats({ 0, 1 }, "SCALAR", 1);
	const int Steps = G.Floats({ 0, 1, 0, 0, 3, 0 }, "VEC3", 3);
	const int Cubic = G.Floats({ 0, 0, 0, 0, 1, 0, 2, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0 }, "VEC3", 3);
	OutAnimations = 3;
	const std::string Rest =
		"\"nodes\":["
		"{\"name\":\"scafo\",\"mesh\":0,\"children\":[1,3]},"
		"{\"name\":\"torretta\",\"translation\":[0,1,0],\"mesh\":1,\"children\":[2]},"
		"{\"name\":\"osso\"},"
		"{\"name\":\"luce\",\"translation\":[0,2,0],\"extensions\":{\"KHR_lights_punctual\":{\"light\":0}}}],"
		"\"scenes\":[{\"nodes\":[0]}],\"scene\":0,"
		"\"meshes\":["
		"{\"name\":\"scafo\",\"primitives\":[{\"attributes\":{\"POSITION\":" + std::to_string(HullPos) + ",\"NORMAL\":" + std::to_string(Up)
		+ ",\"TEXCOORD_0\":" + std::to_string(HullUv) + "},\"indices\":" + std::to_string(Tri) + ",\"material\":0}]},"
		"{\"name\":\"canna\",\"primitives\":[{\"attributes\":{\"POSITION\":" + std::to_string(GunPos) + ",\"NORMAL\":" + std::to_string(Up)
		+ "},\"indices\":" + std::to_string(Tri) + "}]}],"
		"\"materials\":[{\"name\":\"mimetica\",\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0},\"metallicFactor\":0.0},"
		"\"alphaMode\":\"MASK\",\"alphaCutoff\":0.3,\"doubleSided\":true}],"
		"\"textures\":[{\"source\":0}],"
		"\"images\":[{\"bufferView\":" + std::to_string(Image) + ",\"mimeType\":\"image/png\",\"name\":\"mimetica\"}],"
		"\"extensionsUsed\":[\"KHR_lights_punctual\"],"
		"\"extensions\":{\"KHR_lights_punctual\":{\"lights\":[{\"type\":\"spot\",\"color\":[1,0,0],\"intensity\":5}]}},"
		"\"animations\":["
		"{\"name\":\"gira\",\"samplers\":[{\"input\":" + std::to_string(TwoSeconds) + ",\"output\":" + std::to_string(Turn) + ",\"interpolation\":\"LINEAR\"},"
		"{\"input\":" + std::to_string(OneSecond) + ",\"output\":" + std::to_string(Bone) + "}],"
		"\"channels\":[{\"sampler\":0,\"target\":{\"node\":1,\"path\":\"rotation\"}},{\"sampler\":1,\"target\":{\"node\":2,\"path\":\"translation\"}}]},"
		"{\"name\":\"sali\",\"samplers\":[{\"input\":" + std::to_string(OneSecond) + ",\"output\":" + std::to_string(Steps) + ",\"interpolation\":\"STEP\"}],"
		"\"channels\":[{\"sampler\":0,\"target\":{\"node\":1,\"path\":\"translation\"}}]},"
		"{\"name\":\"cubica\",\"samplers\":[{\"input\":" + std::to_string(OneSecond) + ",\"output\":" + std::to_string(Cubic) + ",\"interpolation\":\"CUBICSPLINE\"}],"
		"\"channels\":[{\"sampler\":0,\"target\":{\"node\":1,\"path\":\"translation\"}}]}]";
	return G.Glb(Rest);
}

static void TestTank()
{
	Section("4. Un carro in .glb: gerarchia, animazioni, texture dentro il file, luce");
	int AnimationCount = 0;
	const std::vector<uint8_t> Glb = BuildTank(AnimationCount);
	FFakeDisk Disk;
	Disk.Put("/m/carro/carro.glb", Glb);
	FModel Model;
	std::string Error;
	const bool bOk = LoadModelFile("/m/carro/carro.glb", Disk.Access(), Model, Error);
	Check(bOk && Model.TriangleCount() == 2 && Model.Animations.size() == static_cast<size_t>(AnimationCount),
		"si legge: il pezzo JSON e il pezzo binario; due triangoli, tre animazioni", bOk ? Model.Format : Error);
	if (!bOk) { return; }
	const FModelTexture& Texture = Model.Textures.empty() ? FModelTexture{} : Model.Textures[0];
	Check(Model.Textures.size() == 1 && Texture.Embedded.size() == 13 && Texture.Embedded[1] == 'P' && Texture.MimeType == "image/png"
		&& Texture.Resolved.empty() && Texture.IsAvailable() && Texture.Path == "carro.glb#mimetica",
		"la texture DENTRO il .glb: i suoi byte, il tipo, nessun file da cercare", Texture.Path);
	const FModelMaterial* Camo = nullptr;
	for (const FModelMaterial& M : Model.Materials) { if (M.Name == "mimetica") { Camo = &M; } }
	Check(Camo && Camo->AlphaMode == EAlphaMode::Mask && Near(Camo->AlphaCutoff, 0.3) && Camo->bTwoSided && Camo->Metallic == 0.0f && Camo->Roughness == 1.0f,
		"il materiale: ritaglio sotto 0,3, due facce, metallo 0, rugosita' assente = 1");
	Check(Model.LightPoints.size() == 1 && Model.LightPoints[0].Rgba[0] == 255 && Model.LightPoints[0].Rgba[1] == 0 && Model.LightPoints[0].Directionality == 1,
		"la luce di KHR_lights_punctual: un punto luce rosso, direzionale (un faro), per M7");

	const FModelRig Rig = BuildRig(Model);
	const int32_t Turret = Rig.FindPart("torretta");
	Check(Rig.Parts.size() == 2 && Turret == 1 && Rig.Parts[1].Kind == EPartKind::Animated && Rig.AnimatedCount() == 1,
		"le parti: la radice (lo scafo, fermo, cotto nei vertici) e la torretta animata; l'osso vuoto se ne va",
		Fmt("%.0f parti", static_cast<double>(Rig.Parts.size())));
	Check(Rig.Animations.size() == 3 && Rig.Animations[0].Channels.size() == 1 && Near(Rig.Animations[0].Duration, 2.0)
		&& Rig.FindAnimation("sali") == 1 && Rig.FindAnimation("2") == 2 && Rig.FindAnimation("vola") == -1,
		"le animazioni nel rig: il canale dell'osso tolto con lui; per nome e per numero");
	Check(Rig.Lights.size() == 1 && Rig.Lights[0].Part == 0 && Near(Rig.Lights[0].P[2], 2.0) && Near(Rig.Lights[0].P[0], 0.0),
		"la luce a 2 m d'altezza, nella radice");
	if (Turret != 1) { return; }
	const FRigVertex* Tip = Foremost(Rig.Parts[1]);
	if (!Tip) { Check(false, "la canna"); return; }

	FRigState State = DefaultRigState(Rig);
	double P[3];
	InRoot(Rig, State, Turret, *Tip, P);
	Check(Near3(P, 2, 0, 1), "a riposo la punta della canna e' 2 m avanti e 1 m su (la torretta sta a +1 in Y di glTF)",
		Fmt("%.3f %.3f %.3f", P[0], P[1], P[2]));
	ApplyAnimation(Rig, 0, 1.0, State);
	InRoot(Rig, State, Turret, *Tip, P);
	Check(Near3(P, 1, -std::sqrt(3.0), 1), "'gira' a 1 s: 60 gradi attorno all'alto, verso SINISTRA (gli angoli di glTF sono matematici)",
		Fmt("%.3f %.3f %.3f", P[0], P[1], P[2]));
	double Pivot[3];
	FRigVertex Origin;
	Origin.P[0] = Origin.P[1] = Origin.P[2] = 0.0f;
	InRoot(Rig, State, Turret, Origin, Pivot);
	Check(Near3(Pivot, 0, 0, 1), "e la torretta gira sul suo perno: l'origine del nodo resta a 1 m d'altezza");
	ApplyAnimation(Rig, 0, 5.0, State);
	InRoot(Rig, State, Turret, *Tip, P);
	Check(Near3(P, -1, -std::sqrt(3.0), 1), "dopo la fine si ferma all'ultima chiave (120 gradi)", Fmt("%.3f %.3f %.3f", P[0], P[1], P[2]));

	FRigState Steps = DefaultRigState(Rig);
	ApplyAnimation(Rig, 1, 0.99, Steps);
	InRoot(Rig, Steps, Turret, *Tip, P);
	const bool bBefore = Near3(P, 2, 0, 1);
	ApplyAnimation(Rig, 1, 1.0, Steps);
	InRoot(Rig, Steps, Turret, *Tip, P);
	Check(bBefore && Near3(P, 2, 0, 3), "'sali' (a gradino): fino a 1 s resta giu', a 1 s salta a 3 m");

	FRigState Smooth = DefaultRigState(Rig);
	ApplyAnimation(Rig, 2, 0.5, Smooth);
	InRoot(Rig, Smooth, Turret, *Tip, P);
	Check(Near3(P, 2, -0.75, 1), "'cubica' a meta': 0,75 (Hermite, con la tangente d'uscita 2), cioe' 0,75 m a sinistra",
		Fmt("%.4f %.4f %.4f", P[0], P[1], P[2]));
}

// --- 5. Lo scheletro: la posa di riposo -----------------------------------------------

static void TestSkin()
{
	Section("5. Uno scheletro: la mesh nella posa di riposo");
	FGltf G;
	const int Pos = G.Floats({ 0, 0, 1, 0.1f, 0, 0, -0.1f, 0, 0 }, "VEC3", 3);
	const int Nor = G.Floats({ 0, 1, 0, 0, 1, 0, 0, 1, 0 }, "VEC3", 3);
	const int Joints = G.Accessor(G.View({ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }), 5121, "VEC4", 3);
	const int Weights = G.Floats({ 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0 }, "VEC4", 4);
	const int Inverse = G.Floats({ 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 }, "MAT4", 16);
	const double S45 = std::sin(3.14159265358979 / 4), C45 = std::cos(3.14159265358979 / 4);
	const std::string Rest =
		"\"nodes\":[{\"name\":\"corpo\",\"mesh\":0,\"skin\":0,\"translation\":[100,0,0]},"
		"{\"name\":\"anca\",\"translation\":[0,2,0],\"rotation\":[0," + Fmt("%.17g", S45) + ",0," + Fmt("%.17g", C45) + "]}],"
		"\"scenes\":[{\"nodes\":[0,1]}],"
		"\"skins\":[{\"joints\":[1],\"inverseBindMatrices\":" + std::to_string(Inverse) + "}],"
		"\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":" + std::to_string(Pos) + ",\"NORMAL\":" + std::to_string(Nor)
		+ ",\"JOINTS_0\":" + std::to_string(Joints) + ",\"WEIGHTS_0\":" + std::to_string(Weights) + "}}]}]";
	FFakeDisk Disk;
	Disk.Put("/m/omino.gltf", G.Json(G.DataUri(), Rest));
	FModel Model;
	std::string Error;
	const bool bOk = LoadModelFile("/m/omino.gltf", Disk.Access(), Model, Error);
	Check(bOk, "si legge (il buffer in un data: base64)", Error);
	if (!bOk) { return; }
	bool bWarned = false;
	for (const std::string& W : Model.Warnings) { bWarned = bWarned || W.find("scheletro") != std::string::npos; }
	const FModelRig Rig = BuildRig(Model);
	bool bFound = false;
	for (const FRigSection& S : Rig.Parts[0].Sections)
	{
		for (const FRigVertex& V : S.Vertices) { const double P[3] = { V.P[0], V.P[1], V.P[2] }; if (Near3(P, 0, -1, 2)) { bFound = true; } }
	}
	Check(bFound && bWarned, "il vertice (0, 0, 1), legato all'anca (2 m su, girata di 90 gradi), va a (1, 2, 0) di glTF: "
		"1 m a sinistra e 2 su; la traslazione di 100 m del nodo della mesh si ignora (come vuole glTF); e lo si dice");
}

// --- 6. Interi quantizzati, strisce, ventagli, sparse ---------------------------------

static void TestQuantized()
{
	Section("6. Interi quantizzati, strisce, ventagli, sparse, linee");
	FGltf G;
	// Le posizioni in short normalizzati (32767 = 1), 6 byte per vertice piu' 2 di riempimento: passo 8.
	std::vector<uint8_t> Shorts;
	const int16_t Corners[4][3] = { { 0, 0, 0 }, { 0, 0, 32767 }, { 32767, 0, 0 }, { 32767, 0, 32767 } };
	for (const auto& C : Corners)
	{
		for (const int16_t V : C) { Shorts.push_back(static_cast<uint8_t>(V & 0xFF)); Shorts.push_back(static_cast<uint8_t>((V >> 8) & 0xFF)); }
		Shorts.push_back(0); Shorts.push_back(0);
	}
	const int StripPos = G.Accessor(G.View(Shorts, 8), 5122, "VEC3", 4, true);
	const int StripUv = G.Accessor(G.View({ 0, 0, 0, 0, 0, 255, 0, 0, 255, 0, 0, 0, 128, 255, 0, 0 }, 4), 5121, "VEC2", 4, true);
	// Il ventaglio: il quarto vertice e' sbagliato nella base, lo corregge la parte sparse.
	const int FanView = G.View(RawBytes(std::vector<float>{ 0, 0, 0, 0, 0, 1, 1, 0, 1, 5, 5, 5 }));
	const int SparseIndex = G.View({ 3 });
	const int SparseValue = G.View(RawBytes(std::vector<float>{ 1, 0, 0 }));
	const int FanPos = G.Accessor(FanView, 5126, "VEC3", 4, false,
		",\"sparse\":{\"count\":1,\"indices\":{\"bufferView\":" + std::to_string(SparseIndex) + ",\"componentType\":5121},\"values\":{\"bufferView\":"
		+ std::to_string(SparseValue) + "}}");
	const std::string Rest =
		"\"extensionsUsed\":[\"KHR_mesh_quantization\"],\"extensionsRequired\":[\"KHR_mesh_quantization\"],"
		"\"nodes\":[{\"name\":\"lastra\",\"mesh\":0,\"scale\":[2,2,2]}],"
		"\"meshes\":[{\"primitives\":["
		"{\"attributes\":{\"POSITION\":" + std::to_string(StripPos) + ",\"TEXCOORD_0\":" + std::to_string(StripUv) + "},\"mode\":5},"
		"{\"attributes\":{\"POSITION\":" + std::to_string(FanPos) + "},\"mode\":6},"
		"{\"attributes\":{\"POSITION\":" + std::to_string(FanPos) + "},\"mode\":1}]}]";
	FFakeDisk Disk;
	Disk.Put("/m/lastra.glb", G.Glb(Rest));
	FModel Model;
	std::string Error;
	const bool bOk = LoadModelFile("/m/lastra.glb", Disk.Access(), Model, Error);
	bool bLines = false;
	for (const std::string& W : Model.Warnings) { bLines = bLines || W.find("linee") != std::string::npos; }
	Check(bOk && Model.TriangleCount() == 4 && bLines, "la striscia (2 triangoli) e il ventaglio (2); le linee si saltano e lo si dice; "
		"nessuna scena: i nodi senza padre", bOk ? Fmt("%.0f triangoli", static_cast<double>(Model.TriangleCount())) : Error);
	if (!bOk) { return; }
	const FModelRig Rig = BuildRig(Model);
	bool bAllUp = true, bOut = true, bFanFixed = false, bStripCorner = false, bUv = false;
	for (const FRigSection& S : Rig.Parts[0].Sections)
	{
		bOut = bOut && WindingOutward(S);
		for (const FRigVertex& V : S.Vertices)
		{
			bAllUp = bAllUp && V.N[2] > 0.99f;
			const double P[3] = { V.P[0], V.P[1], V.P[2] };
			if (Near3(P, 0, -2, 0)) { bFanFixed = true; }
			if (Near3(P, 2, -2, 0) && Near(V.UV[0], 128.0 / 255.0, 1e-6) && Near(V.UV[1], 1.0)) { bStripCorner = true; bUv = true; }
		}
	}
	Check(bAllUp && bOut, "le normali piatte calcolate guardano in su e i triangoli (anche quelli dispari della striscia) verso fuori");
	Check(bStripCorner && bUv, "short normalizzati: 32767 = 1 m, per la scala del nodo (2) = 2 m; le UV da 0..255: 128 = 0,502");
	Check(bFanFixed, "la parte sparse: il quarto vertice del ventaglio corretto da (5, 5, 5) a (1, 0, 0)");
}

// --- 7. I file sbagliati --------------------------------------------------------------

static void TestErrors()
{
	Section("7. I file sbagliati: un errore che dice cosa fare");
	FFakeDisk Disk;
	FModel Model;
	std::string E1, E2, E3, E4, E5, E6, E7;
	Disk.Put("/e/draco.gltf", std::string("{\"asset\":{\"version\":\"2.0\"},\"extensionsUsed\":[\"KHR_draco_mesh_compression\"],"
		"\"extensionsRequired\":[\"KHR_draco_mesh_compression\"]}"));
	Disk.Put("/e/vecchio.gltf", std::string("{\"asset\":{\"version\":\"1.0\"}}"));
	int Unused = 0;
	std::vector<uint8_t> Cut = BuildTank(Unused);
	Cut.resize(Cut.size() - 10);
	Disk.Put("/e/tagliato.glb", Cut);
	FGltf Short;
	const int Pos = Short.Floats({ 0, 0, 0, 1, 0, 0, 0, 1, 0 }, "VEC3", 3);
	const int TooMany = Short.Accessor(0, 5126, "VEC3", 100);
	const int Idx = Short.Indices({ 0, 1, 7 });
	Disk.Put("/e/oltre.gltf", Short.Json(Short.DataUri(), "\"nodes\":[{\"mesh\":0}],\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":"
		+ std::to_string(TooMany) + "}}]}]"));
	Disk.Put("/e/indici.gltf", Short.Json(Short.DataUri(), "\"nodes\":[{\"mesh\":0}],\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":"
		+ std::to_string(Pos) + "},\"indices\":" + std::to_string(Idx) + "}]}]"));
	Disk.Put("/e/manca.gltf", Short.Json("manca.bin", ""));
	Disk.Put("/e/rotto.gltf", std::string("{\"asset\":\n  {\"version\": }"));
	const bool bDraco = !LoadModelFile("/e/draco.gltf", Disk.Access(), Model, E1) && E1.find("compressa") != std::string::npos;
	const bool bOld = !LoadModelFile("/e/vecchio.gltf", Disk.Access(), Model, E2) && E2.find("2.0") != std::string::npos;
	const bool bCut = !LoadModelFile("/e/tagliato.glb", Disk.Access(), Model, E3) && E3.find("troncato") != std::string::npos;
	const bool bOutside = !LoadModelFile("/e/oltre.gltf", Disk.Access(), Model, E4) && E4.find("esce") != std::string::npos;
	const bool bIndex = !LoadModelFile("/e/indici.gltf", Disk.Access(), Model, E5) && E5.find("indice") != std::string::npos;
	const bool bMissing = !LoadModelFile("/e/manca.gltf", Disk.Access(), Model, E6) && E6.find("manca.bin") != std::string::npos;
	const bool bBroken = !LoadModelFile("/e/rotto.gltf", Disk.Access(), Model, E7) && E7.find("riga 2") != std::string::npos;
	Check(bDraco, "Draco: dice di riesportare senza compressione", E1);
	Check(bOld, "glTF 1.0: dice di riesportare in 2.0", E2);
	Check(bCut, "un .glb tagliato", E3);
	Check(bOutside && bIndex, "un accessor oltre il suo buffer, un indice oltre i vertici", E4 + " / " + E5);
	Check(bMissing && bBroken, "il .bin che manca; il JSON rotto (con la riga)", E6 + " / " + E7);
}

// --- 8. Il caricatore e la tabella dei modelli ----------------------------------------

static void TestTable()
{
	Section("8. Il caricatore e la tabella: glTF si legge, 'anim'");
	Check(IsReadableModelExtension("gltf") && IsReadableModelExtension("glb") && !IsReadableModelExtension("fbx"),
		"gltf e glb si leggono direttamente; fbx no");
	FModel Model;
	std::string Error;
	FFakeDisk Disk;
	Check(!LoadModelFile("/x/aereo.fbx", Disk.Access(), Model, Error) && Error.find("convert-models") != std::string::npos,
		"un .fbx: dice di usare 'geoworld convert-models'", Error);
	using GeoWorld::Entities::FModelTable;
	using GeoWorld::Entities::FModelTableError;
	using GeoWorld::Entities::FEntityKind;
	FModelTable Table;
	std::vector<FModelTableError> Errors;
	Table.Parse("1.1.105.12  radar/radar.glb anim ruota yaw 180\n1.1.105.13  radar/radar.glb anim\n", Errors);
	const auto Radar = Table.Find(FEntityKind{ 1, 1, 105, 12, 0, 0, 0 });
	Check(Radar.Rule && Radar.Rule->Model.File == "radar/radar.glb" && Radar.Rule->Model.Animation == "ruota"
		&& Radar.Rule->Model.FileOptions.YawDeg == 180.0 && Errors.size() == 1 && Errors[0].Line == 2,
		"'anim ruota': l'animazione che gira sempre; 'anim' senza nome e' un errore di riga 2",
		Errors.empty() ? std::string() : Errors[0].Message);
}

// --- 9. Un .glb vero, scritto da Blender -------------------------------------------

static bool ReadDiskFile(const std::string& Path, std::vector<uint8_t>& Out)
{
	FILE* File = std::fopen(Path.c_str(), "rb");
	if (!File) { return false; }
	Out.clear();
	uint8_t Buffer[65536];
	size_t Read = 0;
	while ((Read = std::fread(Buffer, 1, sizeof(Buffer), File)) > 0) { Out.insert(Out.end(), Buffer, Buffer + Read); }
	std::fclose(File);
	return true;
}

static void TestBlenderFile(const char* Path)
{
	Section("9. Un .glb scritto da Blender (un esportatore vero)");
	std::vector<uint8_t> Bytes;
	if (!Path || !ReadDiskFile(Path, Bytes))
	{
		std::printf("  (saltato: geogltf_tests <Source/GeoTiles/TestData/blender_tank.glb>)\n");
		return;
	}
	FFakeDisk Disk;
	Disk.Put("/b/blender_tank.glb", Bytes);
	FModel Model;
	std::string Error;
	const bool bOk = LoadModelFile("/b/blender_tank.glb", Disk.Access(), Model, Error);
	Check(bOk && Model.Format.find("Blender") != std::string::npos && Model.TriangleCount() == 148,
		"si legge: il generatore e' Blender, 148 triangoli", bOk ? Model.Format : Error);
	if (!bOk) { return; }
	const FModelRig Rig = BuildRig(Model);
	const int32_t Turret = Rig.FindPart("torretta");
	Check(Rig.Parts.size() == 2 && Turret == 1 && Rig.Parts[1].Kind == EPartKind::Animated && Rig.Animations.size() == 1
		&& Near(Rig.Animations[0].Duration, 49.0 / 24.0, 1e-4),
		"lo scafo fermo e la torretta animata (la canna, figlia ferma, cotta dentro); un'animazione di 49 fotogrammi a 24 al secondo",
		Rig.Animations.empty() ? std::string() : Fmt("%.4f s", Rig.Animations[0].Duration));
	Check(Near(Rig.BoundsMin[0], -3.0, 1e-4) && Near(Rig.BoundsMax[0], 5.0, 1e-4) && Near(Rig.BoundsMin[2], 0.0, 1e-4) && Near(Rig.BoundsMax[2], 2.3, 1e-4),
		"le misure: da -3 a +5 m in avanti (la canna esce dal davanti), da 0 a 2,3 m in alto: Blender -Y e' il nostro avanti, Z l'alto",
		Fmt("%.2f..%.2f, %.2f..%.2f", Rig.BoundsMin[0], Rig.BoundsMax[0], Rig.BoundsMin[2], Rig.BoundsMax[2]));
	size_t Against = 0, Total = 0;
	for (const FRigPart& Part : Rig.Parts)
	{
		for (const FRigSection& S : Part.Sections)
		{
			for (size_t I = 0; I + 2 < S.Indices.size(); I += 3)
			{
				const FRigVertex& A = S.Vertices[S.Indices[I]];
				const FRigVertex& B = S.Vertices[S.Indices[I + 1]];
				const FRigVertex& C = S.Vertices[S.Indices[I + 2]];
				const double U[3] = { B.P[0] - A.P[0], B.P[1] - A.P[1], B.P[2] - A.P[2] };
				const double V[3] = { C.P[0] - A.P[0], C.P[1] - A.P[1], C.P[2] - A.P[2] };
				const double X[3] = { U[1] * V[2] - U[2] * V[1], U[2] * V[0] - U[0] * V[2], U[0] * V[1] - U[1] * V[0] };
				++Total;
				if (X[0] * A.N[0] + X[1] * A.N[1] + X[2] * A.N[2] < 0.0) { ++Against; }
			}
		}
	}
	Check(Against == 0 && Total == 148, "nessun triangolo girato contro le sue normali", Fmt("%.0f su %.0f", static_cast<double>(Against), static_cast<double>(Total)));
	if (Turret != 1) { return; }
	const FRigVertex* Tip = Foremost(Rig.Parts[1]);
	if (!Tip) { return; }
	FRigState State = DefaultRigState(Rig);
	double Start[3], End[3];
	ApplyAnimation(Rig, 0, 0.0, State);
	InRoot(Rig, State, Turret, *Tip, Start);
	ApplyAnimation(Rig, 0, Rig.Animations[0].Duration, State);
	InRoot(Rig, State, Turret, *Tip, End);
	Check(Near(Start[0], 5.0, 1e-3) && Near(End[1], -5.0, 1e-3) && Near(End[0], Start[1], 1e-3) && Near(Start[2], End[2], 1e-6),
		"+90 gradi di Blender: la canna, partita in avanti, finisce a SINISTRA, alla stessa altezza",
		Fmt("da %.2f %.2f a %.2f %.2f", Start[0], Start[1], End[0], End[1]));
}

int main(int ArgCount, char** Arguments)
{
	std::printf("=====================================================\n");
	std::printf(" GeoModels -- glTF 2.0: JSON, .gltf, .glb, animazioni\n");
	std::printf("=====================================================\n");
	TestJson();
	TestMath();
	TestArrow();
	TestTank();
	TestSkin();
	TestQuantized();
	TestErrors();
	TestTable();
	TestBlenderFile(ArgCount > 1 ? Arguments[1] : nullptr);
	std::printf("\n=====================================================\n");
	std::printf(" RISULTATO: %d passati, %d falliti\n", GPassed, GFailed);
	std::printf("=====================================================\n");
	return GFailed == 0 ? 0 : 1;
}

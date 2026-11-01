// =============================================================================
//  geomodel_info -- Che cosa c'e' dentro un modello, senza Unreal.
//
//      geomodel_info D:\db\Models\carro\carro.flt [cartella dei modelli]
//      geomodel_info D:\modelli\radar.glb
//
//  Lo legge con gli stessi lettori del motore (Models/*.h) e stampa: il
//  formato, gli avvisi, l'albero dei nodi (con i DOF e i loro limiti, gli
//  switch e le loro maschere, i LOD e le loro distanze), i materiali (con i
//  codici IR di OpenFlight), le texture (trovate o no), e il rig: le parti, i
//  triangoli, le misure in metri negli assi dell'attore. E' il primo controllo
//  da fare su un modello del proprio database prima di provarlo in Unreal.
//
//  Il secondo argomento (facoltativo) e' la cartella dove cercare le texture
//  per nome, come fa geo.Entities.ModelFolder.
// =============================================================================
#include "Models/ModelLoader.h"
#include "Models/ModelRig.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <unordered_map>
#include <vector>

using namespace GeoWorld::Models;

static void PrintNode(const FModel& Model, int32_t Index, int Depth)
{
	const FModelNode& N = Model.Nodes[static_cast<size_t>(Index)];
	size_t Triangles = 0;
	for (const int32_t Mesh : N.Meshes) { Triangles += Model.Meshes[static_cast<size_t>(Mesh)].TriangleCount(); }
	std::printf("%*s%s '%s'", Depth * 2, "", NodeKindName(N.Kind), N.Name.c_str());
	if (Triangles > 0) { std::printf(", %zu triangoli", Triangles); }
	if (N.bHasMatrix) { std::printf(", matrice (spostamento %.2f %.2f %.2f m)", N.Local.T[0], N.Local.T[1], N.Local.T[2]); }
	if (N.Kind == ENodeKind::Dof)
	{
		const FDof& D = N.Dof;
		std::printf(", origine %.3f %.3f %.3f m", D.Frame.T[0], D.Frame.T[1], D.Frame.T[2]);
		static const char* Names[3] = { "beccheggio", "rollio", "imbardata" };
		for (int I = 0; I < 3; ++I)
		{
			if (D.Rotate[I].Max > D.Rotate[I].Min) { std::printf(", %s %.0f..%.0f", Names[I], D.Rotate[I].Min, D.Rotate[I].Max); }
			if (D.Translate[I].Max > D.Translate[I].Min) { std::printf(", %c %.2f..%.2f m", "xyz"[I], D.Translate[I].Min, D.Translate[I].Max); }
		}
	}
	if (N.Kind == ENodeKind::Lod) { std::printf(", visibile da %.0f a %.0f m", N.SwitchOut, N.SwitchIn); }
	if (N.Kind == ENodeKind::Switch) { std::printf(", %d maschere, accesa la %d", N.MaskCount(), N.CurrentMask); }
	if (N.Kind == ENodeKind::Sequence) { std::printf(", %zu fotogrammi in %.2f s", N.Children.size(), N.LoopDuration); }
	if (N.Kind == ENodeKind::ExternalRef) { std::printf(", '%s'", N.ExternalPath.c_str()); }
	if (N.bAnimated) { std::printf(", ANIMATO"); }
	std::printf("\n");
	if (Depth > 40) { return; }
	for (const int32_t Child : N.Children) { PrintNode(Model, Child, Depth + 1); }
}

int main(int ArgCount, char** Arguments)
{
	if (ArgCount < 2)
	{
		std::printf("uso: geomodel_info <modello.flt|.obj|.gltf|.glb> [cartella dei modelli]\n");
		return 2;
	}
	namespace fs = std::filesystem;
	FFileAccess Files;
	Files.Read = [](const std::string& Path, std::vector<uint8_t>& Out)
	{
		std::ifstream In(fs::u8path(Path), std::ios::binary);
		if (!In) { return false; }
		Out.assign(std::istreambuf_iterator<char>(In), std::istreambuf_iterator<char>());
		return true;
	};
	Files.Exists = [](const std::string& Path)
	{
		std::error_code Error;
		return fs::is_regular_file(fs::u8path(Path), Error);
	};

	std::unordered_map<std::string, std::string> Index;
	if (ArgCount >= 3)
	{
		std::error_code Error;
		for (fs::recursive_directory_iterator It(fs::u8path(Arguments[2]), Error), End; !Error && It != End; It.increment(Error))
		{
			if (It->is_regular_file(Error)) { Index.emplace(LowerCase(It->path().filename().u8string()), NormalisePath(It->path().u8string())); }
		}
		std::printf("cartella dei modelli: %zu file\n", Index.size());
	}
	FLoadOptions Options;
	Options.TextureIndex = Index.empty() ? nullptr : &Index;

	FModel Model;
	std::string Error;
	const std::string Path = NormalisePath(Arguments[1]);
	if (!LoadModelFile(Path, Files, Model, Error, Options))
	{
		std::printf("ERRORE: %s\n", Error.c_str());
		return 1;
	}
	std::printf("%s: %s, %zu nodi, %zu triangoli, %zu materiali, %zu texture, %zu punti luce\n", Path.c_str(), Model.Format.c_str(),
		Model.Nodes.size(), Model.TriangleCount(), Model.Materials.size(), Model.Textures.size(), Model.LightPoints.size());
	for (const std::string& Warning : Model.Warnings) { std::printf("  avviso: %s\n", Warning.c_str()); }

	std::printf("\nI nodi:\n");
	PrintNode(Model, 0, 1);

	std::printf("\nLe texture:\n");
	for (const FModelTexture& T : Model.Textures)
	{
		if (!T.Embedded.empty()) { std::printf("  %-50s dentro il file (%s, %zu byte)\n", T.Path.c_str(), T.MimeType.empty() ? "?" : T.MimeType.c_str(), T.Embedded.size()); }
		else { std::printf("  %-50s %s\n", T.Path.c_str(), T.Resolved.empty() ? "NON TROVATA" : T.Resolved.c_str()); }
	}
	std::printf("\nI materiali:\n");
	for (size_t I = 0; I < Model.Materials.size(); ++I)
	{
		const FModelMaterial& M = Model.Materials[I];
		std::printf("  %2zu  '%s', texture %d%s%s", I, M.Name.c_str(), M.Texture, M.bTwoSided ? ", due lati" : "", M.bTranslucent ? ", trasparente" : "");
		if (M.AlphaMode == EAlphaMode::Mask) { std::printf(", ritagliato sotto %.2f", M.AlphaCutoff); }
		if (M.AlphaMode == EAlphaMode::Opaque) { std::printf(", opaco (alfa ignorato)"); }
		if (M.Metallic >= 0.0f) { std::printf(", metallo %.2f", M.Metallic); }
		if (M.Roughness >= 0.0f) { std::printf(", rugosita' %.2f", M.Roughness); }
		if (M.Emissive[0] + M.Emissive[1] + M.Emissive[2] > 0.0f) { std::printf(", luce propria %.2f %.2f %.2f", M.Emissive[0], M.Emissive[1], M.Emissive[2]); }
		if (M.IrMaterial >= 0) { std::printf(", materiale IR %d", M.IrMaterial); }
		if (M.SurfaceCode >= 0) { std::printf(", codice di superficie %d", M.SurfaceCode); }
		std::printf("\n");
	}

	const FModelRig Rig = BuildRig(Model);
	std::printf("\nIl rig (assi dell'attore: X avanti, Y destra, Z alto; metri):\n");
	std::printf("  %zu parti, %zu DOF, %zu animate, %zu switch/sequenze, %zu triangoli\n", Rig.Parts.size(), Rig.DofCount(), Rig.AnimatedCount(),
		Rig.Switches.size(), Rig.TriangleCount());
	std::printf("  misure: avanti %.2f..%.2f, destra %.2f..%.2f, alto %.2f..%.2f (lungo %.2f m)\n", Rig.BoundsMin[0], Rig.BoundsMax[0],
		Rig.BoundsMin[1], Rig.BoundsMax[1], Rig.BoundsMin[2], Rig.BoundsMax[2], Rig.Extent());
	// I triangoli girati: (B - A) x (C - A) contro la normale dei vertici. Se sono tanti, il
	// modello si vedra' "da dentro" (o il file ha le normali sbagliate).
	size_t Against = 0, Counted = 0;
	for (const FRigPart& P : Rig.Parts)
	{
		for (const FRigSection& S : P.Sections)
		{
			for (size_t T = 0; T + 2 < S.Indices.size(); T += 3)
			{
				const FRigVertex& A = S.Vertices[S.Indices[T]];
				const FRigVertex& B = S.Vertices[S.Indices[T + 1]];
				const FRigVertex& C = S.Vertices[S.Indices[T + 2]];
				const double U[3] = { B.P[0] - A.P[0], B.P[1] - A.P[1], B.P[2] - A.P[2] };
				const double V[3] = { C.P[0] - A.P[0], C.P[1] - A.P[1], C.P[2] - A.P[2] };
				const double X[3] = { U[1] * V[2] - U[2] * V[1], U[2] * V[0] - U[0] * V[2], U[0] * V[1] - U[1] * V[0] };
				const double N[3] = { A.N[0] + B.N[0] + C.N[0], A.N[1] + B.N[1] + C.N[1], A.N[2] + B.N[2] + C.N[2] };
				const double Dot = X[0] * N[0] + X[1] * N[1] + X[2] * N[2];
				if (X[0] == 0.0 && X[1] == 0.0 && X[2] == 0.0) { continue; }
				++Counted;
				if (Dot < 0.0) { ++Against; }
			}
		}
	}
	std::printf("  triangoli girati contro le loro normali: %zu su %zu%s\n", Against, Counted,
		Counted > 0 && Against * 10 > Counted ? "  <-- TANTI: il modello si vedra' da dentro, o le normali del file sono sbagliate" : "");
	for (size_t I = 0; I < Rig.Parts.size(); ++I)
	{
		const FRigPart& P = Rig.Parts[I];
		std::printf("  [%zu] %s%s, %zu triangoli in %zu sezioni", I, P.Kind == EPartKind::Dof ? "DOF " : P.Kind == EPartKind::Animated ? "animata " : "",
			P.Name.c_str(), P.TriangleCount(), P.Sections.size());
		if (P.Parent >= 0) { std::printf(", dentro [%d]", P.Parent); }
		if (!P.Switches.empty()) { std::printf(", se lo switch %d mostra il figlio %d", P.Switches.back().Switch, P.Switches.back().Child); }
		if (!P.Lods.empty()) { std::printf(", da %.0f a %.0f m", P.Lods.back().Near, P.Lods.back().Far); }
		std::printf("\n");
	}
	if (!Rig.Animations.empty())
	{
		std::printf("\nLe animazioni (geo.Entities.Animation, o 'anim' nella regola):\n");
		for (size_t I = 0; I < Rig.Animations.size(); ++I)
		{
			const FRigAnimation& A = Rig.Animations[I];
			std::printf("  %zu  '%s': %.2f s, %zu canali\n", I, A.Name.c_str(), A.Duration, A.Channels.size());
		}
	}
	return 0;
}

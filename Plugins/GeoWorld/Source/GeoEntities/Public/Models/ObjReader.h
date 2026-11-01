// =============================================================================
//  ObjReader.h -- Wavefront OBJ e i suoi materiali (MTL). STRATO: C++ PURO.
//
//  Testo, una cosa per riga:
//      v x y z [r g b]     un vertice (qualche esportatore ci mette il colore)
//      vt u v              coordinate di texture (origine in BASSO)
//      vn x y z            una normale
//      f a/b/c d/e/f ...   una faccia: posizione/texture/normale, indici da 1,
//                          negativi = contati dalla fine; un poligono convesso
//      o nome, g nome      un oggetto, un gruppo: diventano nodi
//      usemtl nome         il materiale delle facce che seguono
//      mtllib file.mtl     dove sono i materiali
//
//  Nel MTL: newmtl, Kd (diffuso), Ke (emissivo), Ns (lucentezza), d o Tr
//  (opacita'), map_Kd (la texture; le opzioni -xxx prima del nome si saltano).
//
//  Gli assi non li dice il formato: l'abitudine (Blender, Maya, 3ds Max in
//  esportazione) e' Y in alto e -Z avanti (FAxisConvention::Obj). Un modello
//  che fa diversamente si corregge nella tabella dei modelli (up, forward).
//  Nessuna unita': si assumono metri (scale nella tabella).
// =============================================================================
#pragma once

#include <cstdlib>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "Models/ModelData.h"
#include "Models/ModelFiles.h"

namespace GeoWorld::Models
{
	namespace ObjDetail
	{
		inline std::vector<std::string> Words(const std::string& Line)
		{
			std::vector<std::string> Out;
			std::istringstream Stream(Line);
			std::string Word;
			while (Stream >> Word) { Out.push_back(Word); }
			return Out;
		}

		inline double Number(const std::string& Text) { return std::strtod(Text.c_str(), nullptr); }

		inline std::vector<std::string> Lines(const std::vector<uint8_t>& Bytes)
		{
			std::vector<std::string> Out;
			std::string Line;
			for (uint8_t Byte : Bytes)
			{
				if (Byte == '\n') { Out.push_back(Line); Line.clear(); }
				else if (Byte != '\r') { Line += static_cast<char>(Byte); }
			}
			if (!Line.empty()) { Out.push_back(Line); }
			// Una riga che finisce con \ continua sulla successiva.
			std::vector<std::string> Joined;
			for (const std::string& L : Out)
			{
				if (!Joined.empty() && !Joined.back().empty() && Joined.back().back() == '\\')
				{
					Joined.back().pop_back();
					Joined.back() += " " + L;
				}
				else { Joined.push_back(L); }
			}
			return Joined;
		}

		struct FObjMaterial
		{
			float Kd[3] = { 0.8f, 0.8f, 0.8f };
			float Ke[3] = { 0, 0, 0 };
			float Ns = 0.0f;
			float Opacity = 1.0f;
			std::string Texture;
		};

		inline uint8_t ToByte(double Linear01)
		{
			// I colori del MTL sono "lineari a occhio": li si usa come sRGB, come fanno quasi tutti i visualizzatori.
			return static_cast<uint8_t>(std::clamp(Linear01, 0.0, 1.0) * 255.0 + 0.5);
		}

		inline void ParseMtl(const std::vector<uint8_t>& Bytes, std::unordered_map<std::string, FObjMaterial>& Out)
		{
			FObjMaterial* Current = nullptr;
			for (const std::string& Line : Lines(Bytes))
			{
				const std::vector<std::string> W = Words(Line);
				if (W.empty() || W[0][0] == '#') { continue; }
				if (W[0] == "newmtl" && W.size() >= 2) { Current = &Out[W[1]]; continue; }
				if (!Current) { continue; }
				if (W[0] == "Kd" && W.size() >= 4) { for (int I = 0; I < 3; ++I) { Current->Kd[I] = static_cast<float>(Number(W[1 + I])); } }
				else if (W[0] == "Ke" && W.size() >= 4) { for (int I = 0; I < 3; ++I) { Current->Ke[I] = static_cast<float>(Number(W[1 + I])); } }
				else if (W[0] == "Ns" && W.size() >= 2) { Current->Ns = static_cast<float>(Number(W[1])); }
				else if (W[0] == "d" && W.size() >= 2) { Current->Opacity = static_cast<float>(Number(W.back())); }
				else if (W[0] == "Tr" && W.size() >= 2) { Current->Opacity = 1.0f - static_cast<float>(Number(W.back())); }
				else if (W[0] == "map_Kd" && W.size() >= 2)
				{
					// Le opzioni (-s 1 1 1, -o ..., -bm 1, -clamp on) vengono prima del nome: il nome e' l'ultima parola.
					Current->Texture = W.back();
				}
			}
		}
	}

	/**
	 * Legge un OBJ. Path serve a trovare il MTL e le texture (Files puo' non
	 * saper leggere: allora niente materiali). False solo se il file non c'e'
	 * o non ha geometria.
	 */
	inline bool ReadObj(const std::string& Path, const FFileAccess& Files, FModel& Out, std::string& Error,
		const std::unordered_map<std::string, std::string>* TextureIndex = nullptr)
	{
		using namespace ObjDetail;
		Out = FModel{};
		Out.Format = "OBJ";
		Out.Axes = FAxisConvention::Obj();
		Out.bUvOriginBottom = true;
		std::vector<uint8_t> Bytes;
		if (!Files.Read || !Files.Read(Path, Bytes)) { Error = "non riesco a leggere " + Path; return false; }
		const std::string Directory = DirectoryOf(Path);

		std::vector<std::array<double, 3>> Positions;
		std::vector<std::array<uint8_t, 3>> Colours;
		std::vector<std::array<float, 2>> Uvs;
		std::vector<std::array<float, 3>> Normals;
		std::unordered_map<std::string, FObjMaterial> Materials;
		std::unordered_map<std::string, int32_t> MaterialIds;
		int32_t Node = 0;
		std::string MaterialName;
		int32_t Material = -1;

		const auto MaterialFor = [&](const std::string& Name) -> int32_t
		{
			const auto Known = MaterialIds.find(Name);
			if (Known != MaterialIds.end()) { return Known->second; }
			FModelMaterial M;
			M.Name = Name.empty() ? "default" : Name;
			const auto It = Materials.find(Name);
			if (It != Materials.end())
			{
				const FObjMaterial& Source = It->second;
				if (!Source.Texture.empty())
				{
					M.Texture = Out.FindOrAddTexture(Source.Texture);
					FModelTexture& Texture = Out.Textures[static_cast<size_t>(M.Texture)];
					Texture.Resolved = ResolveReferencedFile(Source.Texture, Directory, Files, TextureIndex);
					if (Texture.Resolved.empty()) { Out.Warn("texture non trovata: " + Source.Texture); }
				}
				M.bTranslucent = Source.Opacity < 0.99f;
				for (int I = 0; I < 3; ++I) { M.Emissive[I] = std::clamp(Source.Ke[I], 0.0f, 1.0f); }
				M.Shininess = std::clamp(Source.Ns / 1000.0f, 0.0f, 1.0f);
			}
			const int32_t Id = Out.FindOrAddMaterial(M);
			MaterialIds[Name] = Id;
			return Id;
		};
		const auto DiffuseOf = [&](const std::string& Name, uint8_t Rgba[4])
		{
			const auto It = Materials.find(Name);
			const FObjMaterial Source = It != Materials.end() ? It->second : FObjMaterial{};
			// Con la texture il diffuso e' quasi sempre 1 o 0,8: si tiene lo stesso (e' quello che mostra il modellatore).
			for (int I = 0; I < 3; ++I) { Rgba[I] = ToByte(Source.Kd[I]); }
			Rgba[3] = ToByte(Source.Opacity);
		};

		size_t Faces = 0;
		for (const std::string& Line : Lines(Bytes))
		{
			const std::vector<std::string> W = Words(Line);
			if (W.empty() || W[0][0] == '#') { continue; }
			const std::string& Key = W[0];
			if (Key == "v" && W.size() >= 4)
			{
				Positions.push_back({ Number(W[1]), Number(W[2]), Number(W[3]) });
				if (W.size() >= 7) { Colours.push_back({ ToByte(Number(W[4])), ToByte(Number(W[5])), ToByte(Number(W[6])) }); }
				else { Colours.push_back({ 255, 255, 255 }); }
			}
			else if (Key == "vt" && W.size() >= 3) { Uvs.push_back({ static_cast<float>(Number(W[1])), static_cast<float>(Number(W[2])) }); }
			else if (Key == "vn" && W.size() >= 4)
			{
				Normals.push_back({ static_cast<float>(Number(W[1])), static_cast<float>(Number(W[2])), static_cast<float>(Number(W[3])) });
			}
			else if ((Key == "o" || Key == "g") && W.size() >= 2) { Node = Out.AddNode(0, ENodeKind::Object, W[1]); }
			else if (Key == "usemtl") { MaterialName = W.size() >= 2 ? W[1] : std::string(); Material = -1; }
			else if (Key == "mtllib")
			{
				for (size_t I = 1; I < W.size(); ++I)
				{
					std::vector<uint8_t> Mtl;
					const std::string MtlPath = ResolveReferencedFile(W[I], Directory, Files);
					if (!MtlPath.empty() && Files.Read(MtlPath, Mtl)) { ParseMtl(Mtl, Materials); }
					else { Out.Warn("materiali non trovati: " + W[I]); }
				}
			}
			else if (Key == "f" && W.size() >= 4)
			{
				if (Material < 0) { Material = MaterialFor(MaterialName); }
				uint8_t Diffuse[4];
				DiffuseOf(MaterialName, Diffuse);
				std::vector<FModelVertex> Corners;
				bool bBad = false;
				for (size_t I = 1; I < W.size(); ++I)
				{
					// a, a/b, a//c, a/b/c
					long Index[3] = { 0, 0, 0 };
					size_t Start = 0;
					for (int Part = 0; Part < 3; ++Part)
					{
						const size_t Slash = W[I].find('/', Start);
						const std::string Field = W[I].substr(Start, Slash == std::string::npos ? std::string::npos : Slash - Start);
						if (!Field.empty()) { Index[Part] = std::strtol(Field.c_str(), nullptr, 10); }
						if (Slash == std::string::npos) { break; }
						Start = Slash + 1;
					}
					const auto Resolve = [](long Raw, size_t Count) -> long
					{
						if (Raw > 0) { return Raw - 1; }
						if (Raw < 0) { return static_cast<long>(Count) + Raw; }
						return -1;
					};
					const long P = Resolve(Index[0], Positions.size());
					const long T = Resolve(Index[1], Uvs.size());
					const long N = Resolve(Index[2], Normals.size());
					if (P < 0 || P >= static_cast<long>(Positions.size())) { bBad = true; break; }
					FModelVertex V;
					for (int C = 0; C < 3; ++C) { V.P[C] = Positions[static_cast<size_t>(P)][static_cast<size_t>(C)]; }
					if (T >= 0 && T < static_cast<long>(Uvs.size())) { V.UV[0] = Uvs[static_cast<size_t>(T)][0]; V.UV[1] = Uvs[static_cast<size_t>(T)][1]; }
					if (N >= 0 && N < static_cast<long>(Normals.size())) { for (int C = 0; C < 3; ++C) { V.N[C] = Normals[static_cast<size_t>(N)][static_cast<size_t>(C)]; } }
					else { V.N[0] = V.N[1] = V.N[2] = 0.0f; }
					for (int C = 0; C < 3; ++C) { V.Rgba[C] = static_cast<uint8_t>(Diffuse[C] * Colours[static_cast<size_t>(P)][static_cast<size_t>(C)] / 255); }
					V.Rgba[3] = Diffuse[3];
					Corners.push_back(V);
				}
				if (bBad) { Out.Warn("faccia con un indice fuori dai vertici: saltata"); continue; }
				Out.AddPolygon(Node, Material, std::move(Corners));
				++Faces;
			}
		}
		if (Faces == 0 || Out.TriangleCount() == 0) { Error = "nessuna faccia in " + Path; return false; }
		return true;
	}
}

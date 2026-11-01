// =============================================================================
//  OpenFlightReader.h -- I modelli OpenFlight (.flt). STRATO: C++ PURO.
//
//  OpenFlight e' il formato dei database di simulazione visiva (Presagis,
//  prima MultiGen). Binario, BIG-ENDIAN, fatto di RECORD:
//
//      uint16 opcode, uint16 lunghezza (record compreso), dati
//
//  La gerarchia non sta nei record ma nella loro SEQUENZA: un Push (10) apre
//  i figli dell'ultimo record "primario" (gruppo, oggetto, faccia, DOF...), un
//  Pop (11) li chiude. I vertici stanno tutti in una palette all'inizio
//  (record 67, poi 68-71); una faccia dice quali usa con una lista di
//  SPOSTAMENTI in byte dall'inizio della palette (record 72).
//
//  Quello che si legge (gli spostamenti dentro i record sono quelli della
//  specifica OpenFlight 16.x, gli stessi del lettore di OpenSceneGraph):
//
//    1 intestazione: versione, UNITA' (metri, km, piedi, pollici, miglia)
//    2 gruppo (anche animato: e' una SEQUENZA di fotogrammi)
//    4 oggetto, 5 faccia, 84-86 mesh (pool di vertici e strisce/ventagli)
//    14 DOF, 73 LOD, 96 switch
//    19/20 sottofacce (decal), 21/22 estensioni (saltate)
//    23 continuazione (un record piu' lungo di 64 KB continua nel successivo)
//    32 palette dei colori, 64 delle texture, 113 dei materiali
//    33 nome lungo, 49 matrice
//    61/62 istanze (un sottoalbero riusato), 63 riferimento esterno (un altro
//    .flt, cercato come le texture)
//    111/130 punti luce: si tengono le posizioni (per le luci, M7)
//
//  Il resto (suoni, testi, strade, curve, volumi di contenimento, multitexture)
//  si salta, e lo si dice negli avvisi.
//
//  Le coordinate restano quelle di OpenFlight (X destra, Y avanti, Z alto,
//  destrorso): il rig le porta all'attore. Le unita' invece diventano subito
//  metri.
// =============================================================================
#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "Models/ModelData.h"
#include "Models/ModelFiles.h"

namespace GeoWorld::Models
{
	namespace Flt
	{
		enum EOpcode : uint16_t
		{
			Header = 1, Group = 2, Object = 4, Face = 5, Push = 10, Pop = 11, Dof = 14, PushSubface = 19, PopSubface = 20,
			PushExtension = 21, PopExtension = 22, Continuation = 23, Comment = 31, ColorPalette = 32, LongId = 33,
			MultiTexture = 52, UvList = 53, Matrix = 49, Replicate = 60, InstanceReference = 61, InstanceDefinition = 62,
			ExternalReference = 63, TexturePalette = 64, VertexPalette = 67, VertexC = 68, VertexCN = 69, VertexCNT = 70,
			VertexCT = 71, VertexList = 72, Lod = 73, MorphVertexList = 89, Mesh = 84, LocalVertexPool = 85,
			MeshPrimitive = 86, Switch = 96, LightPoint = 111, MaterialPalette = 113, IndexedLightPoint = 130,
		};

		// I bit si contano dal piu' alto, come nella specifica.
		inline constexpr uint32_t Bit(int Index) { return 0x80000000u >> Index; }

		struct FRecord
		{
			uint16_t Opcode = 0;
			/** Dove comincia nel file (per gli spostamenti della palette dei vertici). */
			size_t FileOffset = 0;
			/** Il record intero, intestazione compresa: gli spostamenti della specifica valgono cosi' come sono. */
			std::vector<uint8_t> Bytes;

			size_t Size() const { return Bytes.size(); }
			bool Has(size_t At, size_t Count) const { return At + Count <= Bytes.size(); }
			uint8_t U8(size_t At) const { return Has(At, 1) ? Bytes[At] : 0; }
			uint16_t U16(size_t At) const { return Has(At, 2) ? static_cast<uint16_t>(Bytes[At] << 8 | Bytes[At + 1]) : 0; }
			int16_t I16(size_t At) const { return static_cast<int16_t>(U16(At)); }
			uint32_t U32(size_t At) const
			{
				return Has(At, 4) ? (static_cast<uint32_t>(Bytes[At]) << 24 | static_cast<uint32_t>(Bytes[At + 1]) << 16
					| static_cast<uint32_t>(Bytes[At + 2]) << 8 | static_cast<uint32_t>(Bytes[At + 3])) : 0;
			}
			int32_t I32(size_t At) const { return static_cast<int32_t>(U32(At)); }
			float F32(size_t At) const { const uint32_t B = U32(At); float F; std::memcpy(&F, &B, 4); return F; }
			double F64(size_t At) const
			{
				if (!Has(At, 8)) { return 0.0; }
				const uint64_t B = static_cast<uint64_t>(U32(At)) << 32 | U32(At + 4);
				double D; std::memcpy(&D, &B, 8); return D;
			}
			/** Una stringa di lunghezza fissa, finita dal primo zero. */
			std::string Text(size_t At, size_t Count) const
			{
				std::string Out;
				for (size_t I = 0; I < Count && Has(At + I, 1) && Bytes[At + I] != 0; ++I) { Out += static_cast<char>(Bytes[At + I]); }
				return Out;
			}
			/** Il colore impacchettato: A, B, G, R in quest'ordine. */
			void Abgr(size_t At, uint8_t Rgb[3]) const { Rgb[0] = U8(At + 3); Rgb[1] = U8(At + 2); Rgb[2] = U8(At + 1); }
		};

		/** Spezza il file in record, unendo le continuazioni. False se non e' OpenFlight. */
		inline bool SplitRecords(const std::vector<uint8_t>& Bytes, std::vector<FRecord>& Out, std::string& Error)
		{
			Out.clear();
			size_t At = 0;
			while (At + 4 <= Bytes.size())
			{
				const uint16_t Opcode = static_cast<uint16_t>(Bytes[At] << 8 | Bytes[At + 1]);
				const uint16_t Length = static_cast<uint16_t>(Bytes[At + 2] << 8 | Bytes[At + 3]);
				if (Length < 4 || At + Length > Bytes.size())
				{
					if (Out.empty()) { Error = "non e' un file OpenFlight"; return false; }
					break;   // coda rovinata: si tiene quello che c'e'
				}
				if (Opcode == Continuation && !Out.empty())
				{
					Out.back().Bytes.insert(Out.back().Bytes.end(), Bytes.begin() + static_cast<std::ptrdiff_t>(At + 4),
						Bytes.begin() + static_cast<std::ptrdiff_t>(At + Length));
				}
				else
				{
					FRecord Record;
					Record.Opcode = Opcode;
					Record.FileOffset = At;
					Record.Bytes.assign(Bytes.begin() + static_cast<std::ptrdiff_t>(At), Bytes.begin() + static_cast<std::ptrdiff_t>(At + Length));
					Out.push_back(std::move(Record));
				}
				At += Length;
			}
			if (Out.empty() || Out[0].Opcode != Header) { Error = "non e' un file OpenFlight (manca l'intestazione 'db')"; return false; }
			return true;
		}

		/** Metri per unita' (il byte 62 dell'intestazione). */
		inline double UnitsToMetres(uint8_t Units)
		{
			switch (Units)
			{
			case 1: return 1000.0;     // chilometri
			case 4: return 0.3048;     // piedi
			case 5: return 0.0254;     // pollici
			case 8: return 1852.0;     // miglia nautiche
			default: return 1.0;       // metri
			}
		}

		struct FVertex
		{
			double P[3] = { 0, 0, 0 };
			float N[3] = { 0, 0, 0 };
			float UV[2] = { 0, 0 };
			bool bHasNormal = false;
			uint16_t Flags = 0;
			uint8_t Packed[3] = { 255, 255, 255 };
			uint32_t ColorIndex = 0;
		};

		struct FMaterialEntry
		{
			float Diffuse[3] = { 1, 1, 1 };
			float Emissive[3] = { 0, 0, 0 };
			float Shininess = 0.0f;
			float Alpha = 1.0f;
		};

		/** Una faccia (o una mesh, o un punto luce) che aspetta i suoi vertici. */
		struct FPendingFace
		{
			int32_t Node = 0;
			uint8_t DrawType = 0;
			bool bTexWhite = false;
			uint8_t Template = 0;
			int16_t Texture = -1;
			int16_t Material = -1;
			int16_t SurfaceCode = -1;
			int32_t IrMaterial = -1;
			uint16_t Transparency = 0;
			uint32_t Flags = 0;
			uint8_t LightMode = 0;
			uint8_t PackedColor[3] = { 255, 255, 255 };
			uint32_t ColorIndex = 0;
			bool bLightPoint = false;
			bool bMesh = false;
			std::vector<FVertex> Vertices;
			/** Mesh: il pool locale e le primitive (tipo, indici). */
			std::vector<FVertex> Pool;
			std::vector<std::pair<int16_t, std::vector<uint32_t>>> Primitives;
			float ObjectAlpha = 1.0f;
			bool bFlatShaded = false;
		};
	}

	struct FOpenFlightOptions
	{
		/** Quanti livelli di riferimenti esterni seguire (un .flt che ne nomina un altro...). */
		int32_t MaxExternalDepth = 8;
		const std::unordered_map<std::string, std::string>* TextureIndex = nullptr;
	};

	inline bool ReadOpenFlightFile(const std::string& Path, const FFileAccess& Files, FModel& Out, std::string& Error,
		const FOpenFlightOptions& Options = FOpenFlightOptions{}, int32_t Depth = 0);

	/** Il lettore vero e proprio: dai byte di un file che sta in Directory. */
	inline bool ReadOpenFlight(const std::vector<uint8_t>& Bytes, const std::string& Directory, const FFileAccess& Files, FModel& Out,
		std::string& Error, const FOpenFlightOptions& Options = FOpenFlightOptions{}, int32_t Depth = 0)
	{
		using namespace Flt;
		Out = FModel{};
		Out.Axes = FAxisConvention::OpenFlight();
		Out.bUvOriginBottom = true;
		std::vector<FRecord> Records;
		if (!SplitRecords(Bytes, Records, Error)) { return false; }

		const FRecord& HeaderRecord = Records[0];
		const int32_t Version = HeaderRecord.I32(12);
		const double Scale = UnitsToMetres(HeaderRecord.U8(62));
		// 1640 = 16.4, 1561 = 15.61: le versioni "tonde" senza lo zero.
		const int32_t Minor = Version % 100;
		Out.Format = "OpenFlight " + std::to_string(Version / 100) + "." + std::to_string(Minor % 10 == 0 ? Minor / 10 : Minor);
		if (Version < 1400)
		{
			Out.Warn("versione " + std::to_string(Version) + " molto vecchia: alcuni record potrebbero non leggersi (convertilo con Creator o osgconv)");
		}
		Out.Nodes[0].Name = HeaderRecord.Text(4, 8);

		// --- Le palette (possono stare ovunque prima del loro uso, di solito in cima) ---
		std::vector<std::array<uint8_t, 3>> Colours;
		std::map<int32_t, int32_t> TextureByPattern;
		std::map<int32_t, FMaterialEntry> MaterialByIndex;
		std::unordered_map<size_t, FVertex> VertexByOffset;
		size_t PaletteStart = 0, PaletteEnd = 0;
		for (const FRecord& R : Records)
		{
			switch (R.Opcode)
			{
			case ColorPalette:
				for (size_t At = 132; At + 4 <= R.Size(); At += 4)
				{
					uint8_t Rgb[3];
					R.Abgr(At, Rgb);
					Colours.push_back({ Rgb[0], Rgb[1], Rgb[2] });
				}
				break;
			case TexturePalette:
			{
				const std::string File = R.Text(4, 200);
				const int32_t Index = Out.FindOrAddTexture(File);
				FModelTexture& Texture = Out.Textures[static_cast<size_t>(Index)];
				if (Texture.Resolved.empty())
				{
					Texture.Resolved = ResolveReferencedFile(File, Directory, Files, Options.TextureIndex);
					if (Texture.Resolved.empty()) { Out.Warn("texture non trovata: " + File); }
				}
				TextureByPattern[R.I32(204)] = Index;
				break;
			}
			case MaterialPalette:
			{
				FMaterialEntry M;
				for (int I = 0; I < 3; ++I)
				{
					M.Diffuse[I] = R.F32(36 + 4 * I);
					M.Emissive[I] = R.F32(60 + 4 * I);
				}
				M.Shininess = R.F32(72);
				M.Alpha = R.Has(76, 4) ? R.F32(76) : 1.0f;
				MaterialByIndex[R.I32(4)] = M;
				break;
			}
			case VertexPalette:
				PaletteStart = R.FileOffset;
				PaletteEnd = R.FileOffset + R.U32(4);
				break;
			case VertexC: case VertexCN: case VertexCNT: case VertexCT:
			{
				FVertex V;
				V.Flags = R.U16(6);
				for (int I = 0; I < 3; ++I) { V.P[I] = R.F64(8 + 8 * I) * Scale; }
				size_t At = 32;
				if (R.Opcode == VertexCN || R.Opcode == VertexCNT)
				{
					for (int I = 0; I < 3; ++I) { V.N[I] = R.F32(At + 4 * I); }
					V.bHasNormal = true;
					At += 12;
				}
				if (R.Opcode == VertexCNT || R.Opcode == VertexCT)
				{
					V.UV[0] = R.F32(At); V.UV[1] = R.F32(At + 4);
					At += 8;
				}
				R.Abgr(At, V.Packed);
				V.ColorIndex = R.U32(At + 4);
				const size_t Offset = R.FileOffset >= PaletteStart ? R.FileOffset - PaletteStart : 0;
				if (R.FileOffset < PaletteEnd || PaletteEnd == 0) { VertexByOffset[Offset] = V; }
				break;
			}
			default: break;
			}
		}

		const auto PaletteColour = [&](uint32_t Index, uint8_t Rgb[3])
		{
			// Indice = colore * 128 + intensita' (0..127).
			const uint32_t Entry = Index >> 7;
			const double Intensity = static_cast<double>(Index & 0x7Fu) / 127.0;
			if (Entry < Colours.size())
			{
				for (int I = 0; I < 3; ++I) { Rgb[I] = static_cast<uint8_t>(Colours[Entry][static_cast<size_t>(I)] * Intensity + 0.5); }
			}
			else { Rgb[0] = Rgb[1] = Rgb[2] = 255; }
		};

		// --- La gerarchia ---
		enum class ECtx : uint8_t { Node, Face, Skip };
		struct FContext { ECtx Kind = ECtx::Node; int32_t Node = 0; int32_t Face = -1; };
		std::vector<FContext> Stack{ FContext{ ECtx::Node, 0, -1 } };
		std::vector<FPendingFace> Faces;
		std::vector<bool> FaceDone;
		std::map<int32_t, int32_t> InstanceDefinitions;          // numero -> nodo (staccato)
		std::vector<std::pair<int32_t, int32_t>> InstanceUses;   // nodo -> numero
		std::vector<int32_t> ExternalNodes;
		// L'ultimo record primario: un nodo o una faccia, per Push, nome lungo e matrice.
		ECtx LastKind = ECtx::Node;
		int32_t LastNode = 0, LastFace = -1;
		int32_t ExtensionDepth = 0;
		std::set<uint16_t> Unknown;
		bool bWarnedBillboard = false, bWarnedMultiTexture = false;

		const auto ParentNode = [&]() { return Stack.back().Node; };
		std::map<int32_t, float> ObjectAlpha;
		std::map<int32_t, bool> ObjectFlat;

		const auto Emit = [&](int32_t FaceIndex)
		{
			if (FaceIndex < 0 || FaceDone[static_cast<size_t>(FaceIndex)]) { return; }
			FaceDone[static_cast<size_t>(FaceIndex)] = true;
			FPendingFace& F = Faces[static_cast<size_t>(FaceIndex)];
			if (F.Flags & Bit(5)) { return; }                                  // nascosta
			if (F.DrawType == 2 || F.DrawType == 3) { return; }                // fil di ferro
			const bool bLights = F.bLightPoint || F.DrawType == 8 || F.DrawType == 9 || F.DrawType == 10;
			if (F.Template == 2 || F.Template == 4)
			{
				if (!bWarnedBillboard) { Out.Warn("facce billboard (sempre rivolte alla camera): disegnate fisse"); bWarnedBillboard = true; }
			}

			// Il colore della faccia.
			uint8_t FaceRgb[3] = { 255, 255, 255 };
			const bool bTextured = F.Texture >= 0 && TextureByPattern.count(F.Texture) > 0;
			if (F.bLightPoint || (F.Flags & Bit(1))) { /* il colore dei vertici, o solo il materiale */ }
			else if (F.Flags & Bit(3)) { for (int I = 0; I < 3; ++I) { FaceRgb[I] = F.PackedColor[I]; } }
			else { PaletteColour(F.ColorIndex, FaceRgb); }
			if (bTextured && F.bTexWhite) { FaceRgb[0] = FaceRgb[1] = FaceRgb[2] = 255; }

			FMaterialEntry Paint;
			const auto MaterialIt = MaterialByIndex.find(F.Material);
			if (MaterialIt != MaterialByIndex.end()) { Paint = MaterialIt->second; }
			const float Alpha = std::clamp((1.0f - F.Transparency / 65535.0f) * Paint.Alpha * F.ObjectAlpha, 0.0f, 1.0f);

			FModelMaterial M;
			M.Texture = bTextured ? TextureByPattern[F.Texture] : -1;
			M.bTwoSided = F.DrawType == 1;
			M.bTranslucent = Alpha < 0.99f;
			for (int I = 0; I < 3; ++I) { M.Emissive[I] = std::clamp(Paint.Emissive[I], 0.0f, 1.0f); }
			M.Shininess = std::clamp(Paint.Shininess / 128.0f, 0.0f, 1.0f);
			M.IrMaterial = F.IrMaterial > 0 ? F.IrMaterial : -1;
			M.SurfaceCode = F.SurfaceCode > 0 ? F.SurfaceCode : -1;

			const bool bVertexColours = F.bLightPoint || F.LightMode == 1 || F.LightMode == 3;
			const bool bVertexNormals = (F.LightMode == 2 || F.LightMode == 3) && !F.bFlatShaded;
			const auto MakeVertex = [&](const FVertex& Source)
			{
				FModelVertex V;
				for (int I = 0; I < 3; ++I) { V.P[I] = Source.P[I]; }
				if (bVertexNormals && Source.bHasNormal) { for (int I = 0; I < 3; ++I) { V.N[I] = Source.N[I]; } }
				else { V.N[0] = V.N[1] = V.N[2] = 0.0f; }   // la normale della faccia
				V.UV[0] = Source.UV[0]; V.UV[1] = Source.UV[1];
				uint8_t Rgb[3] = { FaceRgb[0], FaceRgb[1], FaceRgb[2] };
				if (bVertexColours && !(Source.Flags & 0x2000) && !(bTextured && F.bTexWhite))
				{
					if (Source.Flags & 0x1000) { for (int I = 0; I < 3; ++I) { Rgb[I] = Source.Packed[I]; } }
					else { PaletteColour(Source.ColorIndex, Rgb); }
				}
				for (int I = 0; I < 3; ++I) { V.Rgba[I] = static_cast<uint8_t>(std::clamp(Rgb[I] * Paint.Diffuse[I] + 0.5f, 0.0f, 255.0f)); }
				V.Rgba[3] = static_cast<uint8_t>(Alpha * 255.0f + 0.5f);
				return V;
			};

			if (bLights)
			{
				for (const FVertex& Source : F.Vertices)
				{
					FModelLightPoint Light;
					Light.Node = F.Node;
					for (int I = 0; I < 3; ++I) { Light.P[I] = Source.P[I]; Light.Normal[I] = Source.bHasNormal ? Source.N[I] : 0.0f; }
					const FModelVertex V = MakeVertex(Source);
					for (int I = 0; I < 4; ++I) { Light.Rgba[I] = V.Rgba[I]; }
					Light.Directionality = F.DrawType == 9 ? 1 : F.DrawType == 10 ? 2 : 0;
					Out.LightPoints.push_back(Light);
				}
				return;
			}

			const int32_t Material = Out.FindOrAddMaterial(M);
			if (!F.bMesh)
			{
				std::vector<FModelVertex> Corners;
				for (const FVertex& Source : F.Vertices) { Corners.push_back(MakeVertex(Source)); }
				Out.AddPolygon(F.Node, Material, std::move(Corners));
				return;
			}
			// Una mesh: strisce, ventagli, strisce di quadrilateri, poligoni indicizzati.
			for (const auto& [Type, Indices] : F.Primitives)
			{
				const auto Corner = [&](uint32_t I) { return I < F.Pool.size() ? MakeVertex(F.Pool[I]) : FModelVertex{}; };
				const auto Triangle = [&](uint32_t A, uint32_t B, uint32_t C)
				{
					if (A >= F.Pool.size() || B >= F.Pool.size() || C >= F.Pool.size()) { return; }
					Out.AddPolygon(F.Node, Material, { Corner(A), Corner(B), Corner(C) });
				};
				const size_t N = Indices.size();
				if (Type == 1)        // striscia di triangoli: un triangolo su due e' girato
				{
					for (size_t I = 0; I + 2 < N; ++I)
					{
						if (I % 2 == 0) { Triangle(Indices[I], Indices[I + 1], Indices[I + 2]); }
						else { Triangle(Indices[I + 1], Indices[I], Indices[I + 2]); }
					}
				}
				else if (Type == 2)   // ventaglio
				{
					for (size_t I = 1; I + 1 < N; ++I) { Triangle(Indices[0], Indices[I], Indices[I + 1]); }
				}
				else if (Type == 3)   // striscia di quadrilateri: 0 1 3 2, 2 3 5 4...
				{
					for (size_t I = 0; I + 3 < N; I += 2)
					{
						Triangle(Indices[I], Indices[I + 1], Indices[I + 3]);
						Triangle(Indices[I], Indices[I + 3], Indices[I + 2]);
					}
				}
				else if (Type == 4)   // poligono
				{
					std::vector<FModelVertex> Corners;
					for (uint32_t I : Indices) { if (I < F.Pool.size()) { Corners.push_back(MakeVertex(F.Pool[I])); } }
					Out.AddPolygon(F.Node, Material, std::move(Corners));
				}
			}
		};

		const auto NewFace = [&](const FRecord& R, bool bMesh, bool bLightPoint)
		{
			FPendingFace F;
			F.Node = ParentNode();
			F.bMesh = bMesh;
			F.bLightPoint = bLightPoint;
			if (!bLightPoint)
			{
				const size_t S = bMesh ? 4 : 0;   // la mesh ha 4 byte in piu' dopo il nome
				F.DrawType = R.U8(18 + S);
				F.bTexWhite = R.U8(19 + S) != 0;
				F.Template = R.U8(25 + S);
				F.Texture = R.I16(28 + S);
				F.Material = R.I16(30 + S);
				F.SurfaceCode = R.I16(32 + S);
				F.IrMaterial = R.I32(36 + S);
				F.Transparency = R.U16(40 + S);
				F.Flags = R.U32(44 + S);
				F.LightMode = R.U8(48 + S);
				R.Abgr(56 + S, F.PackedColor);
				F.ColorIndex = Version < 1510 ? static_cast<uint32_t>(R.U16(20 + S)) : R.U32(68 + S);
			}
			// L'oggetto che la contiene: trasparenza e ombreggiatura piatta.
			for (int32_t N = F.Node; N > 0; N = Out.Nodes[static_cast<size_t>(N)].Parent)
			{
				if (Out.Nodes[static_cast<size_t>(N)].Kind == ENodeKind::Object)
				{
					F.ObjectAlpha = ObjectAlpha.count(N) ? ObjectAlpha[N] : 1.0f;
					F.bFlatShaded = ObjectFlat.count(N) ? ObjectFlat[N] : false;
					break;
				}
			}
			Faces.push_back(std::move(F));
			FaceDone.push_back(false);
			LastKind = ECtx::Face;
			LastFace = static_cast<int32_t>(Faces.size()) - 1;
		};

		const auto NewNode = [&](ENodeKind Kind, const std::string& Name)
		{
			const int32_t Index = Out.AddNode(ParentNode(), Kind, Name);
			LastKind = ECtx::Node;
			LastNode = Index;
			LastFace = -1;
			return Index;
		};

		for (size_t Index = 1; Index < Records.size(); ++Index)
		{
			const FRecord& R = Records[Index];
			if (ExtensionDepth > 0)
			{
				if (R.Opcode == PushExtension) { ++ExtensionDepth; }
				else if (R.Opcode == PopExtension) { --ExtensionDepth; }
				continue;
			}
			switch (R.Opcode)
			{
			case PushExtension: ExtensionDepth = 1; break;
			case Push:
			case PushSubface:
			{
				FContext C;
				if (R.Opcode == PushSubface) { C = FContext{ ECtx::Node, Stack.back().Node, -1 }; }   // le sottofacce stanno nel nodo della faccia
				else if (LastKind == ECtx::Face && LastFace >= 0) { C = FContext{ ECtx::Face, Faces[static_cast<size_t>(LastFace)].Node, LastFace }; }
				else if (LastKind == ECtx::Skip) { C = FContext{ ECtx::Skip, Stack.back().Node, -1 }; }
				else { C = FContext{ ECtx::Node, LastNode, -1 }; }
				Stack.push_back(C);
				LastKind = ECtx::Node;
				LastNode = C.Node;
				break;
			}
			case Pop:
			case PopSubface:
				if (Stack.size() > 1)
				{
					const FContext C = Stack.back();
					Stack.pop_back();
					if (C.Kind == ECtx::Face) { Emit(C.Face); }
					// Dopo un Pop l'ultimo primario e' il contesto in cui si torna: un Push
					// che seguisse subito (i figli dell'intestazione dopo le definizioni
					// di istanza) va li', non dentro il nodo appena chiuso.
					LastKind = ECtx::Node;
					LastNode = Stack.back().Node;
					LastFace = -1;
				}
				break;
			case Group:
			{
				const uint32_t Flags = R.U32(16);
				const bool bAnimated = (Flags & (Bit(1) | Bit(6))) != 0;
				const int32_t Node = NewNode(bAnimated ? ENodeKind::Sequence : ENodeKind::Group, R.Text(4, 8));
				FModelNode& G = Out.Nodes[static_cast<size_t>(Node)];
				G.Flags = Flags;
				G.LoopCount = R.I32(32);
				G.LoopDuration = R.F32(36);
				G.LastFrameDuration = R.F32(40);
				G.bSwing = (Flags & Bit(2)) != 0;
				break;
			}
			case Object:
			{
				const int32_t Node = NewNode(ENodeKind::Object, R.Text(4, 8));
				Out.Nodes[static_cast<size_t>(Node)].Flags = R.U32(12);
				ObjectAlpha[Node] = 1.0f - R.U16(18) / 65535.0f;
				ObjectFlat[Node] = (R.U32(12) & Bit(4)) != 0;
				break;
			}
			case Dof:
			{
				const int32_t Node = NewNode(ENodeKind::Dof, R.Text(4, 8));
				FDof& D = Out.Nodes[static_cast<size_t>(Node)].Dof;
				double Origin[3], OnX[3], InXY[3];
				for (int I = 0; I < 3; ++I)
				{
					Origin[I] = R.F64(16 + 8 * I) * Scale;
					OnX[I] = R.F64(40 + 8 * I) * Scale;
					InXY[I] = R.F64(64 + 8 * I) * Scale;
				}
				double X[3] = { OnX[0] - Origin[0], OnX[1] - Origin[1], OnX[2] - Origin[2] };
				const double V[3] = { InXY[0] - Origin[0], InXY[1] - Origin[1], InXY[2] - Origin[2] };
				double Z[3] = { X[1] * V[2] - X[2] * V[1], X[2] * V[0] - X[0] * V[2], X[0] * V[1] - X[1] * V[0] };
				const double LX = std::sqrt(X[0] * X[0] + X[1] * X[1] + X[2] * X[2]);
				const double LZ = std::sqrt(Z[0] * Z[0] + Z[1] * Z[1] + Z[2] * Z[2]);
				if (LX < 1e-9 || LZ < 1e-9)
				{
					// Il sistema del DOF non e' stato messo: gli assi del modello, nell'origine.
					X[0] = 1; X[1] = 0; X[2] = 0;
					Z[0] = 0; Z[1] = 0; Z[2] = 1;
				}
				else
				{
					for (int I = 0; I < 3; ++I) { X[I] /= LX; Z[I] /= LZ; }
				}
				const double Y[3] = { Z[1] * X[2] - Z[2] * X[1], Z[2] * X[0] - Z[0] * X[2], Z[0] * X[1] - Z[1] * X[0] };
				D.Frame = FAffine::FromColumns(X, Y, Z, Origin);
				// L'ordine dei blocchi nel record: z, y, x, beccheggio, rollio, imbardata, scala z, y, x.
				const auto Range = [&](size_t At, double Factor)
				{
					return FDofAxis{ R.F64(At) * Factor, R.F64(At + 8) * Factor, R.F64(At + 16) * Factor, R.F64(At + 24) * Factor };
				};
				D.Translate[2] = Range(88, Scale);
				D.Translate[1] = Range(120, Scale);
				D.Translate[0] = Range(152, Scale);
				D.Rotate[0] = Range(184, 1.0);
				D.Rotate[1] = Range(216, 1.0);
				D.Rotate[2] = Range(248, 1.0);
				D.Scale[2] = Range(280, 1.0);
				D.Scale[1] = Range(312, 1.0);
				D.Scale[0] = Range(344, 1.0);
				D.Flags = R.U32(376);
				break;
			}
			case Lod:
			{
				const int32_t Node = NewNode(ENodeKind::Lod, R.Text(4, 8));
				FModelNode& L = Out.Nodes[static_cast<size_t>(Node)];
				L.SwitchIn = R.F64(16) * Scale;
				L.SwitchOut = R.F64(24) * Scale;
				for (int I = 0; I < 3; ++I) { L.LodCenter[I] = R.F64(40 + 8 * I) * Scale; }
				break;
			}
			case Switch:
			{
				const int32_t Node = NewNode(ENodeKind::Switch, R.Text(4, 8));
				FModelNode& S = Out.Nodes[static_cast<size_t>(Node)];
				S.CurrentMask = R.I32(16);
				S.WordsPerMask = std::max(0, R.I32(20));
				const int32_t Count = std::max(0, R.I32(24));
				for (int64_t W = 0; W < static_cast<int64_t>(Count) * S.WordsPerMask; ++W)
				{
					S.Masks.push_back(R.U32(28 + static_cast<size_t>(W) * 4));
				}
				break;
			}
			case ExternalReference:
			{
				std::string File = R.Text(4, 200);
				const size_t Bracket = File.find('<');
				if (Bracket != std::string::npos) { File = File.substr(0, Bracket); }
				const int32_t Node = NewNode(ENodeKind::ExternalRef, FileNameOf(File));
				Out.Nodes[static_cast<size_t>(Node)].ExternalPath = File;
				ExternalNodes.push_back(Node);
				break;
			}
			case InstanceDefinition:
			{
				// Un sottoalbero da riusare: un nodo staccato dalla radice.
				const int32_t Number = R.U16(6);
				const int32_t Node = Out.AddNode(-1, ENodeKind::Group, "istanza " + std::to_string(Number));
				InstanceDefinitions[Number] = Node;
				LastKind = ECtx::Node;
				LastNode = Node;
				LastFace = -1;
				break;
			}
			case InstanceReference:
			{
				const int32_t Number = R.U16(6);
				const int32_t Node = NewNode(ENodeKind::Group, "uso dell'istanza " + std::to_string(Number));
				InstanceUses.emplace_back(Node, Number);
				break;
			}
			case Face: NewFace(R, false, false); break;
			case Mesh: NewFace(R, true, false); break;
			case LightPoint: case IndexedLightPoint: NewFace(R, false, true); break;
			case VertexList:
			{
				const int32_t Target = Stack.back().Kind == ECtx::Face ? Stack.back().Face : (LastKind == ECtx::Face ? LastFace : -1);
				if (Target < 0) { break; }
				FPendingFace& F = Faces[static_cast<size_t>(Target)];
				for (size_t At = 4; At + 4 <= R.Size(); At += 4)
				{
					const auto It = VertexByOffset.find(static_cast<size_t>(R.U32(At)));
					if (It != VertexByOffset.end()) { F.Vertices.push_back(It->second); }
				}
				break;
			}
			case LocalVertexPool:
			{
				const int32_t Target = Stack.back().Kind == ECtx::Face ? Stack.back().Face : -1;
				if (Target < 0) { break; }
				FPendingFace& F = Faces[static_cast<size_t>(Target)];
				const uint32_t Count = R.U32(4);
				const uint32_t Mask = R.U32(8);
				size_t At = 12;
				for (uint32_t I = 0; I < Count && At < R.Size(); ++I)
				{
					FVertex V;
					if (Mask & Bit(0)) { for (int C = 0; C < 3; ++C) { V.P[C] = R.F64(At + 8 * static_cast<size_t>(C)) * Scale; } At += 24; }
					if (Mask & Bit(1))
					{
						V.ColorIndex = R.U32(At) & 0x00FFFFFFu;
						At += 4;
					}
					else if (Mask & Bit(2)) { R.Abgr(At, V.Packed); V.Flags |= 0x1000; At += 4; }
					if (Mask & Bit(3)) { for (int C = 0; C < 3; ++C) { V.N[C] = R.F32(At + 4 * static_cast<size_t>(C)); } V.bHasNormal = true; At += 12; }
					if (Mask & Bit(4)) { V.UV[0] = R.F32(At); V.UV[1] = R.F32(At + 4); At += 8; }
					for (int Layer = 5; Layer <= 11; ++Layer) { if (Mask & Bit(Layer)) { At += 8; } }
					F.Pool.push_back(V);
				}
				break;
			}
			case MeshPrimitive:
			{
				const int32_t Target = Stack.back().Kind == ECtx::Face ? Stack.back().Face : -1;
				if (Target < 0) { break; }
				const int16_t Type = R.I16(4);
				const uint16_t IndexSize = R.U16(6);
				const uint32_t Count = R.U32(8);
				std::vector<uint32_t> Indices;
				for (uint32_t I = 0; I < Count; ++I)
				{
					const size_t At = 12 + static_cast<size_t>(I) * IndexSize;
					Indices.push_back(IndexSize == 1 ? R.U8(At) : IndexSize == 2 ? R.U16(At) : R.U32(At));
				}
				Faces[static_cast<size_t>(Target)].Primitives.emplace_back(Type, std::move(Indices));
				break;
			}
			case LongId:
			{
				const std::string Name = R.Text(4, R.Size() - 4);
				if (LastKind == ECtx::Node && LastNode > 0) { Out.Nodes[static_cast<size_t>(LastNode)].Name = Name; }
				break;
			}
			case Matrix:
			{
				if (LastKind != ECtx::Node || LastNode <= 0) { break; }
				FModelNode& N = Out.Nodes[static_cast<size_t>(LastNode)];
				// 16 float, righe = immagini degli assi, l'ultima la traslazione (convenzione a vettore riga).
				float Mx[16];
				for (int I = 0; I < 16; ++I) { Mx[I] = R.F32(4 + 4 * static_cast<size_t>(I)); }
				FAffine A;
				for (int I = 0; I < 3; ++I)
				{
					for (int J = 0; J < 3; ++J) { A.M[I][J] = Mx[J * 4 + I]; }
					A.T[I] = Mx[12 + I] * Scale;
				}
				N.Local = A;
				N.bHasMatrix = true;
				break;
			}
			case MultiTexture: case UvList:
				if (!bWarnedMultiTexture) { Out.Warn("piu' strati di texture: si usa solo il primo"); bWarnedMultiTexture = true; }
				break;
			// Palette e vertici: gia' letti.
			case ColorPalette: case TexturePalette: case MaterialPalette: case VertexPalette:
			case VertexC: case VertexCN: case VertexCNT: case VertexCT: case Comment: case MorphVertexList:
				break;
			default:
				// Un record che non conosciamo. Se ha dei figli (segue un Push),
				// diventa un gruppo: la geometria sotto non si perde.
				if (R.Opcode < 150) { Unknown.insert(R.Opcode); }
				if (Index + 1 < Records.size() && Records[Index + 1].Opcode == Push)
				{
					NewNode(ENodeKind::Group, "record " + std::to_string(R.Opcode));
				}
				break;
			}
		}
		// Le facce rimaste aperte (senza Pop).
		for (int32_t Face = 0; Face < static_cast<int32_t>(Faces.size()); ++Face) { Emit(Face); }
		if (!Unknown.empty())
		{
			std::string List;
			for (uint16_t Opcode : Unknown) { List += (List.empty() ? "" : ", ") + std::to_string(Opcode); }
			Out.Warn("record saltati (opcode " + List + ")");
		}

		// --- Le istanze: una copia del sottoalbero sotto ogni uso ---
		const std::function<void(int32_t, int32_t)> CloneUnder = [&](int32_t Source, int32_t Parent)
		{
			for (const int32_t Child : std::vector<int32_t>(Out.Nodes[static_cast<size_t>(Source)].Children))
			{
				FModelNode Copy = Out.Nodes[static_cast<size_t>(Child)];
				Copy.Parent = Parent;
				Copy.Children.clear();
				const int32_t New = static_cast<int32_t>(Out.Nodes.size());
				Out.Nodes.push_back(std::move(Copy));
				Out.Nodes[static_cast<size_t>(Parent)].Children.push_back(New);
				CloneUnder(Child, New);
			}
		};
		for (const auto& [Node, Number] : InstanceUses)
		{
			const auto It = InstanceDefinitions.find(Number);
			if (It == InstanceDefinitions.end()) { Out.Warn("istanza " + std::to_string(Number) + " usata ma mai definita"); continue; }
			const FModelNode& Definition = Out.Nodes[static_cast<size_t>(It->second)];
			for (const int32_t Mesh : Definition.Meshes) { Out.Nodes[static_cast<size_t>(Node)].Meshes.push_back(Mesh); }
			CloneUnder(It->second, Node);
		}

		// --- I riferimenti esterni: un altro .flt sotto il nodo ---
		for (const int32_t Node : ExternalNodes)
		{
			const std::string Raw = Out.Nodes[static_cast<size_t>(Node)].ExternalPath;
			if (Depth >= Options.MaxExternalDepth) { Out.Warn("riferimenti esterni troppo profondi: " + Raw); continue; }
			const std::string Resolved = ResolveReferencedFile(Raw, Directory, Files, nullptr);
			if (Resolved.empty()) { Out.Warn("riferimento esterno non trovato: " + Raw); continue; }
			FModel Child;
			std::string ChildError;
			if (!ReadOpenFlightFile(Resolved, Files, Child, ChildError, Options, Depth + 1))
			{
				Out.Warn("riferimento esterno " + Raw + ": " + ChildError);
				continue;
			}
			Out.Merge(Node, Child);
		}
		if (Out.TriangleCount() == 0 && Out.LightPoints.empty()) { Out.Warn("nessuna faccia"); }
		return true;
	}

	inline bool ReadOpenFlightFile(const std::string& Path, const FFileAccess& Files, FModel& Out, std::string& Error,
		const FOpenFlightOptions& Options, int32_t Depth)
	{
		std::vector<uint8_t> Bytes;
		if (!Files.Read || !Files.Read(Path, Bytes)) { Error = "non riesco a leggere " + Path; return false; }
		return ReadOpenFlight(Bytes, DirectoryOf(Path), Files, Out, Error, Options, Depth);
	}
}

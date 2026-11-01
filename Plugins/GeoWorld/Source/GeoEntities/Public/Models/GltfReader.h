// =============================================================================
//  GltfReader.h -- glTF 2.0 (.gltf e .glb). STRATO: C++ PURO.
//
//  glTF e' il formato aperto dei modelli del Khronos Group, il "JPEG del 3D"
//  (docs/modelli-design.md, sezione 13). Un documento JSON che descrive:
//
//      buffers       i byte (un file .bin accanto, un "data:" in base64, o il
//                    pezzo binario del .glb)
//      bufferViews   fette di un buffer
//      accessors     come leggere una fetta: quanti elementi, di che tipo
//                    (SCALAR, VEC2, VEC3, VEC4, MAT4), con che numeri (float,
//                    interi, "normalizzati" da 0..255 a 0..1)
//      meshes        primitive: attributi (POSITION, NORMAL, TEXCOORD_n,
//                    COLOR_0, JOINTS_0, WEIGHTS_0) e indici
//      materials     PBR: colore di base (fattore e texture), metallo,
//                    rugosita', emissivo, alfa (OPAQUE, MASK, BLEND), due facce
//      textures,
//      images        PNG/JPEG in un file, in un data:, o dentro il .glb
//      nodes         l'albero: matrice o traslazione/rotazione/scala, mesh,
//                    figli, scheletro (skin)
//      scenes        quali nodi sono le radici
//      animations    curve sui nodi (traslazione, rotazione, scala), con
//                    interpolazione a gradino, lineare o cubica
//
//  Le convenzioni (sezione 4): metri, +Y in alto, +Z avanti, destrorso, UV con
//  l'origine in ALTO (come Unreal: non si girano), triangoli antiorari.
//  I colori dei vertici e i fattori sono LINEARI: si portano nella codifica dei
//  vertici del rig con la radice quadrata (il materiale li rieleva al
//  quadrato).
//
//  Cosa si fa delle cose che non disegniamo ancora:
//    - gli SCHELETRI: la mesh si mette nella posa di riposo (le ossa, nella
//      loro posa del file, la deformano una volta, qui); le animazioni delle
//      ossa non la muovono;
//    - i MORPH TARGET: ignorati (avviso);
//    - le TEXTURE normali, di occlusione, metallo/rugosita': ignorate (i
//      fattori si);
//    - le estensioni: KHR_mesh_quantization, KHR_texture_transform,
//      KHR_materials_emissive_strength, KHR_lights_punctual (diventano punti
//      luce, M7), KHR_materials_unlit si'; le compressioni (Draco, meshopt) e
//      le texture KTX2/WebP no: il messaggio dice come riesportare.
// =============================================================================
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iterator>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "Models/Json.h"
#include "Models/ModelData.h"
#include "Models/ModelFiles.h"

namespace GeoWorld::Models
{
	namespace GltfDetail
	{
		constexpr uint32_t GlbMagic = 0x46546C67u;      // "glTF"
		constexpr uint32_t ChunkJson = 0x4E4F534Au;     // "JSON"
		constexpr uint32_t ChunkBin = 0x004E4942u;      // "BIN\0"

		inline uint32_t ReadU32(const uint8_t* P)
		{
			return static_cast<uint32_t>(P[0]) | static_cast<uint32_t>(P[1]) << 8 | static_cast<uint32_t>(P[2]) << 16 | static_cast<uint32_t>(P[3]) << 24;
		}

		inline int ComponentSize(int64_t ComponentType)
		{
			switch (ComponentType)
			{
			case 5120: case 5121: return 1;
			case 5122: case 5123: return 2;
			case 5125: case 5126: return 4;
			default: return 0;
			}
		}

		inline int TypeComponents(const std::string& Type)
		{
			if (Type == "SCALAR") { return 1; }
			if (Type == "VEC2") { return 2; }
			if (Type == "VEC3") { return 3; }
			if (Type == "VEC4" || Type == "MAT2") { return 4; }
			if (Type == "MAT3") { return 9; }
			if (Type == "MAT4") { return 16; }
			return 0;
		}

		/** Un numero dal buffer (little-endian), normalizzato se l'accessor lo chiede. */
		inline double ReadComponent(const uint8_t* P, int64_t ComponentType, bool bNormalized)
		{
			switch (ComponentType)
			{
			case 5120:
			{
				const double V = static_cast<int8_t>(P[0]);
				return bNormalized ? std::max(V / 127.0, -1.0) : V;
			}
			case 5121: return bNormalized ? P[0] / 255.0 : static_cast<double>(P[0]);
			case 5122:
			{
				const double V = static_cast<int16_t>(static_cast<uint16_t>(P[0] | P[1] << 8));
				return bNormalized ? std::max(V / 32767.0, -1.0) : V;
			}
			case 5123:
			{
				const double V = static_cast<uint16_t>(P[0] | P[1] << 8);
				return bNormalized ? V / 65535.0 : V;
			}
			case 5125: return static_cast<double>(ReadU32(P));
			case 5126:
			{
				const uint32_t Bits = ReadU32(P);
				float F;
				std::memcpy(&F, &Bits, sizeof(F));
				return static_cast<double>(F);
			}
			default: return 0.0;
			}
		}

		inline bool DecodeBase64(const std::string& In, std::vector<uint8_t>& Out)
		{
			Out.clear();
			uint32_t Accumulator = 0;
			int Bits = 0;
			for (const char C : In)
			{
				int Value = -1;
				if (C >= 'A' && C <= 'Z') { Value = C - 'A'; }
				else if (C >= 'a' && C <= 'z') { Value = C - 'a' + 26; }
				else if (C >= '0' && C <= '9') { Value = C - '0' + 52; }
				else if (C == '+' || C == '-') { Value = 62; }
				else if (C == '/' || C == '_') { Value = 63; }
				else if (C == '=') { break; }
				else if (C == ' ' || C == '\n' || C == '\r' || C == '\t') { continue; }
				else { return false; }
				Accumulator = (Accumulator << 6) | static_cast<uint32_t>(Value);
				Bits += 6;
				if (Bits >= 8)
				{
					Bits -= 8;
					Out.push_back(static_cast<uint8_t>((Accumulator >> Bits) & 0xFF));
				}
			}
			return true;
		}

		/** "data:image/png;base64,...." -> i byte e il tipo. */
		inline bool DecodeDataUri(const std::string& Uri, std::vector<uint8_t>& Out, std::string& MimeType)
		{
			if (Uri.compare(0, 5, "data:") != 0) { return false; }
			const size_t Comma = Uri.find(',');
			if (Comma == std::string::npos) { return false; }
			const std::string Header = Uri.substr(5, Comma - 5);
			const size_t Semicolon = Header.find(';');
			MimeType = Header.substr(0, Semicolon);
			if (Header.find(";base64") == std::string::npos) { return false; }
			return DecodeBase64(Uri.substr(Comma + 1), Out);
		}

		/** Gli URI relativi di glTF sono codificati: "mia%20texture.png" e' "mia texture.png". */
		inline int HexDigit(char C)
		{
			if (C >= '0' && C <= '9') { return C - '0'; }
			if (C >= 'a' && C <= 'f') { return C - 'a' + 10; }
			if (C >= 'A' && C <= 'F') { return C - 'A' + 10; }
			return -1;
		}

		inline std::string PercentDecode(const std::string& In)
		{
			std::string Out;
			for (size_t I = 0; I < In.size(); ++I)
			{
				const int High = (In[I] == '%' && I + 2 < In.size()) ? HexDigit(In[I + 1]) : -1;
				const int Low = High >= 0 ? HexDigit(In[I + 2]) : -1;
				if (Low >= 0) { Out += static_cast<char>(High * 16 + Low); I += 2; }
				else { Out += In[I]; }
			}
			return Out;
		}

		struct FDocument
		{
			Json::FValue Root;
			std::vector<std::vector<uint8_t>> Buffers;
		};

		/** La fetta di un bufferView: dove comincia, quanto e' lunga, il passo (0: non detto). */
		inline bool ViewRange(const FDocument& Doc, int64_t ViewIndex, const uint8_t*& Base, size_t& Length, size_t& Stride, std::string& Error)
		{
			const Json::FValue& View = Doc.Root["bufferViews"][static_cast<size_t>(ViewIndex)];
			if (!View.IsObject()) { Error = "bufferView " + std::to_string(ViewIndex) + " non c'e'"; return false; }
			const int64_t Buffer = View["buffer"].IndexOr();
			if (Buffer < 0 || static_cast<size_t>(Buffer) >= Doc.Buffers.size()) { Error = "bufferView " + std::to_string(ViewIndex) + ": buffer sbagliato"; return false; }
			const std::vector<uint8_t>& Bytes = Doc.Buffers[static_cast<size_t>(Buffer)];
			const int64_t Offset = View["byteOffset"].IndexOr(0);
			const int64_t ByteLength = View["byteLength"].IndexOr();
			if (Offset < 0 || ByteLength < 0 || static_cast<uint64_t>(Offset) + static_cast<uint64_t>(ByteLength) > Bytes.size())
			{
				Error = "bufferView " + std::to_string(ViewIndex) + " esce dal buffer (file troncato?)";
				return false;
			}
			Base = Bytes.data() + Offset;
			Length = static_cast<size_t>(ByteLength);
			Stride = static_cast<size_t>(std::max<int64_t>(0, View["byteStride"].IndexOr(0)));
			return true;
		}

		/**
		 * Un accessor intero, come numeri: Count elementi di Components
		 * componenti ciascuno, uno dopo l'altro. Con la parte "sparse" (i pochi
		 * elementi che cambiano rispetto alla base).
		 */
		template <typename T>
		inline bool ReadAccessor(const FDocument& Doc, int64_t Index, std::vector<T>& Out, int& Components, size_t& Count, std::string& Error)
		{
			Out.clear();
			const Json::FValue& A = Doc.Root["accessors"][static_cast<size_t>(Index < 0 ? 0 : Index)];
			if (Index < 0 || !A.IsObject()) { Error = "accessor " + std::to_string(Index) + " non c'e'"; return false; }
			const int64_t ComponentType = A["componentType"].IndexOr();
			const int CompSize = ComponentSize(ComponentType);
			Components = TypeComponents(A["type"].StringOr(""));
			const int64_t CountIn = A["count"].IndexOr();
			const std::string Name = "accessor " + std::to_string(Index);
			if (CompSize == 0 || Components == 0 || CountIn < 0) { Error = Name + ": tipo o numero di elementi sbagliato"; return false; }
			if (CountIn > 100000000) { Error = Name + ": troppi elementi"; return false; }
			// Le matrici piccole con numeri da 1 o 2 byte hanno le colonne allineate a 4: rare, non le leggiamo.
			if ((Components == 4 || Components == 9) && A["type"].StringOr("") != "VEC4" && CompSize < 4) { Error = Name + ": matrice con interi piccoli (non supportata)"; return false; }
			Count = static_cast<size_t>(CountIn);
			const bool bNormalized = A["normalized"].BoolOr(false);
			Out.assign(Count * static_cast<size_t>(Components), T(0));

			const int64_t ViewIndex = A["bufferView"].IndexOr();
			if (ViewIndex >= 0)
			{
				const uint8_t* Base = nullptr;
				size_t Length = 0, Stride = 0;
				if (!ViewRange(Doc, ViewIndex, Base, Length, Stride, Error)) { return false; }
				const size_t Offset = static_cast<size_t>(std::max<int64_t>(0, A["byteOffset"].IndexOr(0)));
				const size_t ElementSize = static_cast<size_t>(Components) * static_cast<size_t>(CompSize);
				if (Stride == 0) { Stride = ElementSize; }
				if (Stride < ElementSize) { Error = Name + ": passo piu' corto di un elemento"; return false; }
				if (Count > 0 && Offset + Stride * (Count - 1) + ElementSize > Length) { Error = Name + ": esce dal suo bufferView"; return false; }
				for (size_t E = 0; E < Count; ++E)
				{
					const uint8_t* P = Base + Offset + Stride * E;
					for (int C = 0; C < Components; ++C)
					{
						Out[E * static_cast<size_t>(Components) + static_cast<size_t>(C)] = static_cast<T>(ReadComponent(P + C * CompSize, ComponentType, bNormalized));
					}
				}
			}

			const Json::FValue& Sparse = A["sparse"];
			if (Sparse.IsObject())
			{
				const int64_t SparseCount = Sparse["count"].IndexOr();
				const Json::FValue& Indices = Sparse["indices"];
				const Json::FValue& Values = Sparse["values"];
				const int64_t IndexType = Indices["componentType"].IndexOr();
				const uint8_t* IndexBase = nullptr;
				const uint8_t* ValueBase = nullptr;
				size_t IndexLength = 0, ValueLength = 0, Unused = 0;
				if (SparseCount < 0 || ComponentSize(IndexType) == 0
					|| !ViewRange(Doc, Indices["bufferView"].IndexOr(), IndexBase, IndexLength, Unused, Error)
					|| !ViewRange(Doc, Values["bufferView"].IndexOr(), ValueBase, ValueLength, Unused, Error))
				{
					if (Error.empty()) { Error = Name + ": parte sparse sbagliata"; }
					return false;
				}
				const size_t IndexOffset = static_cast<size_t>(std::max<int64_t>(0, Indices["byteOffset"].IndexOr(0)));
				const size_t ValueOffset = static_cast<size_t>(std::max<int64_t>(0, Values["byteOffset"].IndexOr(0)));
				const size_t IndexSize = static_cast<size_t>(ComponentSize(IndexType));
				const size_t ElementSize = static_cast<size_t>(Components) * static_cast<size_t>(CompSize);
				const size_t N = static_cast<size_t>(SparseCount);
				if (IndexOffset + N * IndexSize > IndexLength || ValueOffset + N * ElementSize > ValueLength) { Error = Name + ": la parte sparse esce dal buffer"; return false; }
				for (size_t K = 0; K < N; ++K)
				{
					const size_t Target = static_cast<size_t>(ReadComponent(IndexBase + IndexOffset + K * IndexSize, IndexType, false));
					if (Target >= Count) { Error = Name + ": un indice sparse fuori dall'accessor"; return false; }
					for (int C = 0; C < Components; ++C)
					{
						Out[Target * static_cast<size_t>(Components) + static_cast<size_t>(C)] =
							static_cast<T>(ReadComponent(ValueBase + ValueOffset + K * ElementSize + static_cast<size_t>(C) * static_cast<size_t>(CompSize), ComponentType, bNormalized));
					}
				}
			}
			return true;
		}

		/** Una matrice di glTF (16 numeri per COLONNE) nella nostra. */
		inline FAffine MatrixFromColumnMajor(const double* C)
		{
			FAffine Matrix;
			for (int Row = 0; Row < 3; ++Row)
			{
				for (int Column = 0; Column < 3; ++Column) { Matrix.M[Row][Column] = C[Column * 4 + Row]; }
				Matrix.T[Row] = C[12 + Row];
			}
			return Matrix;
		}

		inline bool IsIdentity(const FAffine& A)
		{
			for (int I = 0; I < 3; ++I)
			{
				if (std::fabs(A.T[I]) > 1e-12) { return false; }
				for (int J = 0; J < 3; ++J) { if (std::fabs(A.M[I][J] - (I == J ? 1.0 : 0.0)) > 1e-12) { return false; } }
			}
			return true;
		}

		/** Da lineare (0..1) alla codifica dei colori dei vertici del rig: la radice (il materiale rieleva al quadrato). */
		inline uint8_t LinearToVertexByte(double Linear)
		{
			const double V = std::sqrt(std::clamp(Linear, 0.0, 1.0));
			return static_cast<uint8_t>(std::lround(V * 255.0));
		}

		/** Quello che serve di un materiale per fare i vertici delle sue primitive. */
		struct FMaterialUse
		{
			int32_t ModelMaterial = 0;
			double Factor[4] = { 1, 1, 1, 1 };
			int TexCoord = 0;
			bool bTransform = false;
			double Offset[2] = { 0, 0 }, Rotation = 0.0, Scale[2] = { 1, 1 };
		};

		/** Una primitiva gia' convertita (una volta per mesh, anche se piu' nodi la usano). */
		struct FPrimitive
		{
			int32_t Material = 0;
			std::vector<FModelVertex> Vertices;
			std::vector<uint32_t> Indices;
			std::vector<std::array<uint32_t, 4>> Joints;
			std::vector<std::array<float, 4>> Weights;
		};
	}

	inline bool ReadGltf(const std::string& Path, const FFileAccess& Files, FModel& Out, std::string& Error,
		const std::unordered_map<std::string, std::string>* TextureIndex = nullptr)
	{
		using namespace GltfDetail;
		Out = FModel{};
		Out.Axes = FAxisConvention::Gltf();
		Out.bUvOriginBottom = false;
		const std::string FileName = FileNameOf(Path);
		const std::string Directory = DirectoryOf(Path);
		const std::function<bool(const std::string&, std::vector<uint8_t>&)> ReadBytes = Files.Read;
		std::vector<uint8_t> Bytes;
		if (!ReadBytes || !ReadBytes(Path, Bytes)) { Error = "non riesco a leggere " + Path; return false; }

		// ---- Il contenitore: .glb (intestazione e pezzi) o JSON nudo -------------
		FDocument Doc;
		std::vector<uint8_t> BinChunk;
		bool bHaveBin = false;
		std::string ParseError;
		if (Bytes.size() >= 12 && ReadU32(Bytes.data()) == GlbMagic)
		{
			const uint32_t Version = ReadU32(Bytes.data() + 4);
			if (Version != 2) { Error = FileName + ": .glb versione " + std::to_string(Version) + " (si legge solo glTF 2.0: riesportalo)"; return false; }
			const size_t Total = std::min<size_t>(ReadU32(Bytes.data() + 8), Bytes.size());
			size_t Pos = 12;
			bool bHaveJson = false;
			while (Pos + 8 <= Total)
			{
				const size_t ChunkLength = ReadU32(Bytes.data() + Pos);
				const uint32_t ChunkType = ReadU32(Bytes.data() + Pos + 4);
				Pos += 8;
				if (Pos + ChunkLength > Total) { Error = FileName + ": .glb troncato"; return false; }
				if (ChunkType == ChunkJson && !bHaveJson)
				{
					if (!Json::Parse(reinterpret_cast<const char*>(Bytes.data() + Pos), ChunkLength, Doc.Root, ParseError)) { Error = FileName + ": " + ParseError; return false; }
					bHaveJson = true;
				}
				else if (ChunkType == ChunkBin && !bHaveBin)
				{
					BinChunk.assign(Bytes.begin() + static_cast<std::ptrdiff_t>(Pos), Bytes.begin() + static_cast<std::ptrdiff_t>(Pos + ChunkLength));
					bHaveBin = true;
				}
				Pos += (ChunkLength + 3) & ~static_cast<size_t>(3);
			}
			if (!bHaveJson) { Error = FileName + ": .glb senza il pezzo JSON"; return false; }
		}
		else if (!Json::Parse(reinterpret_cast<const char*>(Bytes.data()), Bytes.size(), Doc.Root, ParseError))
		{
			Error = FileName + ": " + ParseError;
			return false;
		}
		Bytes.clear();
		Bytes.shrink_to_fit();
		const Json::FValue& Root = Doc.Root;
		if (!Root.IsObject()) { Error = FileName + ": non e' un documento glTF"; return false; }

		// ---- Versione ed estensioni ----------------------------------------------
		const std::string Version = Root["asset"]["version"].StringOr("");
		if (Version.empty() || Version[0] != '2')
		{
			Error = FileName + ": glTF " + (Version.empty() ? std::string("senza versione") : Version) + " (si legge solo il 2.0: riesportalo, per esempio con Blender)";
			return false;
		}
		Out.Format = "glTF " + Version;
		const std::string Generator = Root["asset"]["generator"].StringOr("");
		if (!Generator.empty()) { Out.Format += " (" + Generator + ")"; }
		static const char* Supported[] = { "KHR_mesh_quantization", "KHR_texture_transform", "KHR_materials_emissive_strength",
			"KHR_lights_punctual", "KHR_materials_unlit" };
		const Json::FValue& Required = Root["extensionsRequired"];
		for (size_t I = 0; I < Required.Size(); ++I)
		{
			const std::string Name = Required[I].StringOr("");
			if (std::find_if(std::begin(Supported), std::end(Supported), [&](const char* S) { return Name == S; }) != std::end(Supported)) { continue; }
			if (Name == "KHR_draco_mesh_compression" || Name == "EXT_meshopt_compression" || Name == "KHR_meshopt_compression")
			{
				Error = FileName + ": la geometria e' compressa (" + Name + "): riesportalo senza compressione "
					"(Blender: togli 'Compression'; gltf-transform: senza draco/meshopt)";
			}
			else if (Name == "KHR_texture_basisu" || Name == "EXT_texture_webp")
			{
				Error = FileName + ": le texture sono in un formato che non leggiamo (" + Name + "): riesportale in PNG o JPEG";
			}
			else
			{
				Error = FileName + ": richiede l'estensione " + Name + ", che non conosciamo";
			}
			return false;
		}

		// ---- I buffer ----------------------------------------------------------------
		const Json::FValue& BufferList = Root["buffers"];
		for (size_t I = 0; I < BufferList.Size(); ++I)
		{
			const Json::FValue& B = BufferList[I];
			std::vector<uint8_t> Data;
			const std::string Uri = B["uri"].StringOr("");
			std::string Mime;
			if (Uri.empty())
			{
				if (I != 0 || !bHaveBin) { Error = FileName + ": buffer " + std::to_string(I) + " senza dati"; return false; }
				Data = std::move(BinChunk);
			}
			else if (Uri.compare(0, 5, "data:") == 0)
			{
				if (!DecodeDataUri(Uri, Data, Mime)) { Error = FileName + ": buffer " + std::to_string(I) + ": data: sbagliato"; return false; }
			}
			else
			{
				const std::string Where = JoinPath(Directory, NormalisePath(PercentDecode(Uri)));
				if (!ReadBytes(Where, Data)) { Error = FileName + ": manca il file dei dati '" + PercentDecode(Uri) + "'"; return false; }
			}
			const int64_t Declared = B["byteLength"].IndexOr();
			if (Declared >= 0 && Data.size() < static_cast<size_t>(Declared))
			{
				Error = FileName + ": buffer " + std::to_string(I) + " piu' corto del dichiarato (" + std::to_string(Data.size()) + " byte su " + std::to_string(Declared) + ")";
				return false;
			}
			Doc.Buffers.push_back(std::move(Data));
		}

		// ---- Immagini e texture --------------------------------------------------------
		std::map<int64_t, int32_t> TextureOfImage;
		const auto ImageTexture = [&](int64_t ImageIndex) -> int32_t
		{
			const auto Known = TextureOfImage.find(ImageIndex);
			if (Known != TextureOfImage.end()) { return Known->second; }
			const Json::FValue& Image = Root["images"][static_cast<size_t>(ImageIndex)];
			int32_t Result = -1;
			if (Image.IsObject())
			{
				const std::string Uri = Image["uri"].StringOr("");
				const std::string Label = Image["name"].StringOr("immagine " + std::to_string(ImageIndex));
				if (!Uri.empty() && Uri.compare(0, 5, "data:") != 0)
				{
					const std::string Decoded = PercentDecode(Uri);
					Result = Out.FindOrAddTexture(Decoded);
					FModelTexture& T = Out.Textures[static_cast<size_t>(Result)];
					if (T.Resolved.empty()) { T.Resolved = ResolveReferencedFile(Decoded, Directory, Files, TextureIndex); }
				}
				else
				{
					Result = Out.FindOrAddTexture(FileName + "#" + Label);
					FModelTexture& T = Out.Textures[static_cast<size_t>(Result)];
					if (!Uri.empty())
					{
						if (!DecodeDataUri(Uri, T.Embedded, T.MimeType)) { Out.Warn("immagine " + std::to_string(ImageIndex) + ": data: sbagliato"); }
					}
					else
					{
						const uint8_t* Base = nullptr;
						size_t Length = 0, Stride = 0;
						std::string ViewError;
						if (ViewRange(Doc, Image["bufferView"].IndexOr(), Base, Length, Stride, ViewError)) { T.Embedded.assign(Base, Base + Length); }
						else { Out.Warn("immagine " + std::to_string(ImageIndex) + ": " + ViewError); }
						T.MimeType = Image["mimeType"].StringOr("");
					}
				}
			}
			TextureOfImage[ImageIndex] = Result;
			return Result;
		};
		const auto TextureFor = [&](int64_t TextureIndexInFile) -> int32_t
		{
			if (TextureIndexInFile < 0) { return -1; }
			const Json::FValue& T = Root["textures"][static_cast<size_t>(TextureIndexInFile)];
			const int64_t Source = T["source"].IndexOr();
			if (Source < 0)
			{
				if (T["extensions"].IsObject()) { Out.Warn("una texture solo in KTX2/WebP/DDS: non la leggiamo (riesportala in PNG o JPEG)"); }
				return -1;
			}
			return ImageTexture(Source);
		};

		// ---- Materiali -------------------------------------------------------------------
		std::vector<FMaterialUse> MaterialUses;
		FMaterialUse DefaultUse;
		bool bDefaultMade = false;
		const auto DefaultMaterial = [&]() -> const FMaterialUse&
		{
			if (!bDefaultMade)
			{
				// Il materiale di default di glTF: bianco, metallo 1, rugosita' 1 (come i fattori assenti).
				FModelMaterial M;
				M.Name = "default";
				M.Metallic = 1.0f;
				M.Roughness = 1.0f;
				M.AlphaMode = EAlphaMode::Opaque;
				DefaultUse.ModelMaterial = Out.FindOrAddMaterial(M);
				bDefaultMade = true;
			}
			return DefaultUse;
		};
		const Json::FValue& MaterialList = Root["materials"];
		for (size_t I = 0; I < MaterialList.Size(); ++I)
		{
			const Json::FValue& J = MaterialList[I];
			const Json::FValue& Pbr = J["pbrMetallicRoughness"];
			FMaterialUse Use;
			FModelMaterial M;
			M.Name = J["name"].StringOr("materiale " + std::to_string(I));
			for (int C = 0; C < 4; ++C) { Use.Factor[C] = Pbr["baseColorFactor"][static_cast<size_t>(C)].NumberOr(1.0); }
			M.Metallic = static_cast<float>(std::clamp(Pbr["metallicFactor"].NumberOr(1.0), 0.0, 1.0));
			M.Roughness = static_cast<float>(std::clamp(Pbr["roughnessFactor"].NumberOr(1.0), 0.0, 1.0));
			const Json::FValue& Base = Pbr["baseColorTexture"];
			if (Base.IsObject())
			{
				M.Texture = TextureFor(Base["index"].IndexOr());
				Use.TexCoord = static_cast<int>(Base["texCoord"].IndexOr(0));
				const Json::FValue& Transform = Base["extensions"]["KHR_texture_transform"];
				if (Transform.IsObject())
				{
					Use.bTransform = true;
					Use.Offset[0] = Transform["offset"][static_cast<size_t>(0)].NumberOr(0.0);
					Use.Offset[1] = Transform["offset"][static_cast<size_t>(1)].NumberOr(0.0);
					Use.Rotation = Transform["rotation"].NumberOr(0.0);
					Use.Scale[0] = Transform["scale"][static_cast<size_t>(0)].NumberOr(1.0);
					Use.Scale[1] = Transform["scale"][static_cast<size_t>(1)].NumberOr(1.0);
					if (Transform["texCoord"].IsNumber()) { Use.TexCoord = static_cast<int>(Transform["texCoord"].IndexOr(0)); }
				}
			}
			const double Strength = J["extensions"]["KHR_materials_emissive_strength"]["emissiveStrength"].NumberOr(1.0);
			for (int C = 0; C < 3; ++C) { M.Emissive[C] = static_cast<float>(std::max(0.0, J["emissiveFactor"][static_cast<size_t>(C)].NumberOr(0.0) * Strength)); }
			M.bTwoSided = J["doubleSided"].BoolOr(false);
			const std::string Mode = J["alphaMode"].StringOr("OPAQUE");
			M.AlphaMode = Mode == "MASK" ? EAlphaMode::Mask : Mode == "BLEND" ? EAlphaMode::Blend : EAlphaMode::Opaque;
			M.AlphaCutoff = static_cast<float>(J["alphaCutoff"].NumberOr(0.5));
			M.bTranslucent = M.AlphaMode == EAlphaMode::Blend;
			if (J["extensions"].Has("KHR_materials_unlit"))
			{
				// Senza luce: il colore di base diventa luce propria (un display, un cielo dipinto).
				for (int C = 0; C < 3; ++C) { M.Emissive[C] = std::max(M.Emissive[C], static_cast<float>(Use.Factor[C])); }
			}
			Use.ModelMaterial = Out.FindOrAddMaterial(M);
			MaterialUses.push_back(Use);
		}

		// ---- Le primitive di una mesh, convertite una volta ------------------------------
		const Json::FValue& MeshList = Root["meshes"];
		std::vector<std::vector<FPrimitive>> MeshCache(MeshList.Size());
		std::vector<bool> MeshDone(MeshList.Size(), false);
		const auto ConvertMesh = [&](size_t MeshIndex, std::string& MeshError) -> bool
		{
			if (MeshDone[MeshIndex]) { return true; }
			MeshDone[MeshIndex] = true;
			const Json::FValue& Prims = MeshList[MeshIndex]["primitives"];
			const std::string MeshName = MeshList[MeshIndex]["name"].StringOr("mesh " + std::to_string(MeshIndex));
			for (size_t PI = 0; PI < Prims.Size(); ++PI)
			{
				const Json::FValue& P = Prims[PI];
				const int64_t Mode = P["mode"].IndexOr(4);
				if (Mode < 4 || Mode > 6) { Out.Warn(MeshName + ": punti o linee (modo " + std::to_string(Mode) + "), non disegnati"); continue; }
				if (P["targets"].Size() > 0) { Out.Warn(MeshName + ": morph target ignorati"); }
				const Json::FValue& Attributes = P["attributes"];
				const int64_t MaterialIndex = P["material"].IndexOr();
				const FMaterialUse& Use = (MaterialIndex >= 0 && static_cast<size_t>(MaterialIndex) < MaterialUses.size())
					? MaterialUses[static_cast<size_t>(MaterialIndex)] : DefaultMaterial();

				std::vector<float> Positions, Normals, Uvs, Colours, Weights;
				std::vector<double> JointIndices;
				int Components = 0;
				size_t Count = 0, AttributeCount = 0;
				if (!Attributes["POSITION"].IsNumber()) { Out.Warn(MeshName + ": una primitiva senza POSITION"); continue; }
				if (!ReadAccessor(Doc, Attributes["POSITION"].IndexOr(), Positions, Components, Count, MeshError)) { return false; }
				if (Components != 3) { MeshError = MeshName + ": POSITION non e' VEC3"; return false; }
				bool bNormals = false, bUvs = false;
				int ColourComponents = 0;
				if (Attributes["NORMAL"].IsNumber())
				{
					if (!ReadAccessor(Doc, Attributes["NORMAL"].IndexOr(), Normals, Components, AttributeCount, MeshError)) { return false; }
					bNormals = Components == 3 && AttributeCount == Count;
				}
				const std::string UvName = "TEXCOORD_" + std::to_string(Use.TexCoord);
				if (Attributes[UvName].IsNumber())
				{
					if (!ReadAccessor(Doc, Attributes[UvName].IndexOr(), Uvs, Components, AttributeCount, MeshError)) { return false; }
					bUvs = Components == 2 && AttributeCount == Count;
				}
				if (Attributes["COLOR_0"].IsNumber())
				{
					if (!ReadAccessor(Doc, Attributes["COLOR_0"].IndexOr(), Colours, Components, AttributeCount, MeshError)) { return false; }
					ColourComponents = (AttributeCount == Count && (Components == 3 || Components == 4)) ? Components : 0;
				}
				bool bSkin = false;
				if (Attributes["JOINTS_0"].IsNumber() && Attributes["WEIGHTS_0"].IsNumber())
				{
					int JointComponents = 0, WeightComponents = 0;
					size_t JointCount = 0, WeightCount = 0;
					if (!ReadAccessor(Doc, Attributes["JOINTS_0"].IndexOr(), JointIndices, JointComponents, JointCount, MeshError)) { return false; }
					if (!ReadAccessor(Doc, Attributes["WEIGHTS_0"].IndexOr(), Weights, WeightComponents, WeightCount, MeshError)) { return false; }
					bSkin = JointComponents == 4 && WeightComponents == 4 && JointCount == Count && WeightCount == Count;
				}

				// Gli indici, poi i triangoli secondo il modo.
				std::vector<uint32_t> Raw;
				if (P["indices"].IsNumber())
				{
					std::vector<double> IndexValues;
					size_t IndexCount = 0;
					if (!ReadAccessor(Doc, P["indices"].IndexOr(), IndexValues, Components, IndexCount, MeshError)) { return false; }
					Raw.reserve(IndexValues.size());
					for (const double V : IndexValues)
					{
						if (V < 0.0 || V >= static_cast<double>(Count)) { MeshError = MeshName + ": un indice oltre i vertici"; return false; }
						Raw.push_back(static_cast<uint32_t>(V));
					}
				}
				else
				{
					Raw.resize(Count);
					for (size_t I = 0; I < Count; ++I) { Raw[I] = static_cast<uint32_t>(I); }
				}
				std::vector<uint32_t> Triangles;
				if (Mode == 4)
				{
					Triangles.assign(Raw.begin(), Raw.begin() + static_cast<std::ptrdiff_t>(Raw.size() - Raw.size() % 3));
				}
				else if (Mode == 5)
				{
					// La striscia: un triangolo ogni vertice, girando il verso a quelli dispari.
					for (size_t I = 0; I + 2 < Raw.size(); ++I)
					{
						const bool bOdd = I % 2 == 1;
						Triangles.push_back(Raw[I]);
						Triangles.push_back(Raw[bOdd ? I + 2 : I + 1]);
						Triangles.push_back(Raw[bOdd ? I + 1 : I + 2]);
					}
				}
				else
				{
					// Il ventaglio attorno al primo vertice.
					for (size_t I = 1; I + 1 < Raw.size(); ++I)
					{
						Triangles.push_back(Raw[0]);
						Triangles.push_back(Raw[I]);
						Triangles.push_back(Raw[I + 1]);
					}
				}

				// I vertici.
				FPrimitive Prim;
				Prim.Material = Use.ModelMaterial;
				const double Cos = std::cos(Use.Rotation), Sin = std::sin(Use.Rotation);
				const auto MakeVertex = [&](size_t I)
				{
					FModelVertex V;
					for (int C = 0; C < 3; ++C) { V.P[C] = Positions[I * 3 + static_cast<size_t>(C)]; }
					if (bNormals) { for (int C = 0; C < 3; ++C) { V.N[C] = Normals[I * 3 + static_cast<size_t>(C)]; } }
					else { V.N[0] = V.N[1] = V.N[2] = 0.0f; }
					if (bUvs)
					{
						double U = Uvs[I * 2], W = Uvs[I * 2 + 1];
						if (Use.bTransform)
						{
							const double SU = U * Use.Scale[0], SW = W * Use.Scale[1];
							U = Use.Offset[0] + Cos * SU + Sin * SW;
							W = Use.Offset[1] - Sin * SU + Cos * SW;
						}
						V.UV[0] = static_cast<float>(U);
						V.UV[1] = static_cast<float>(W);
					}
					double Colour[4] = { 1, 1, 1, 1 };
					for (int C = 0; C < ColourComponents; ++C) { Colour[C] = Colours[I * static_cast<size_t>(ColourComponents) + static_cast<size_t>(C)]; }
					for (int C = 0; C < 3; ++C) { V.Rgba[C] = LinearToVertexByte(Colour[C] * Use.Factor[C]); }
					V.Rgba[3] = static_cast<uint8_t>(std::lround(std::clamp(Colour[3] * Use.Factor[3], 0.0, 1.0) * 255.0));
					return V;
				};
				const auto PushSkin = [&](size_t I)
				{
					if (!bSkin) { return; }
					std::array<uint32_t, 4> J{};
					std::array<float, 4> W{};
					for (int C = 0; C < 4; ++C)
					{
						J[static_cast<size_t>(C)] = static_cast<uint32_t>(std::max(0.0, JointIndices[I * 4 + static_cast<size_t>(C)]));
						W[static_cast<size_t>(C)] = Weights[I * 4 + static_cast<size_t>(C)];
					}
					Prim.Joints.push_back(J);
					Prim.Weights.push_back(W);
				};
				if (bNormals)
				{
					Prim.Vertices.reserve(Count);
					for (size_t I = 0; I < Count; ++I) { Prim.Vertices.push_back(MakeVertex(I)); PushSkin(I); }
					Prim.Indices = std::move(Triangles);
				}
				else
				{
					// Senza normali glTF vuole le normali "piatte": un vertice per angolo di triangolo.
					for (size_t T = 0; T + 2 < Triangles.size(); T += 3)
					{
						FModelVertex Corner[3] = { MakeVertex(Triangles[T]), MakeVertex(Triangles[T + 1]), MakeVertex(Triangles[T + 2]) };
						const double E1[3] = { Corner[1].P[0] - Corner[0].P[0], Corner[1].P[1] - Corner[0].P[1], Corner[1].P[2] - Corner[0].P[2] };
						const double E2[3] = { Corner[2].P[0] - Corner[0].P[0], Corner[2].P[1] - Corner[0].P[1], Corner[2].P[2] - Corner[0].P[2] };
						double N[3] = { E1[1] * E2[2] - E1[2] * E2[1], E1[2] * E2[0] - E1[0] * E2[2], E1[0] * E2[1] - E1[1] * E2[0] };
						const double L = std::sqrt(N[0] * N[0] + N[1] * N[1] + N[2] * N[2]);
						if (L > 0.0) { for (double& C : N) { C /= L; } }
						for (int K = 0; K < 3; ++K)
						{
							for (int C = 0; C < 3; ++C) { Corner[K].N[C] = static_cast<float>(N[C]); }
							Prim.Indices.push_back(static_cast<uint32_t>(Prim.Vertices.size()));
							Prim.Vertices.push_back(Corner[K]);
							PushSkin(Triangles[T + static_cast<size_t>(K)]);
						}
					}
				}
				MeshCache[MeshIndex].push_back(std::move(Prim));
			}
			return true;
		};

		// ---- I nodi della scena --------------------------------------------------------
		const Json::FValue& NodeList = Root["nodes"];
		std::vector<int32_t> NodeMap(NodeList.Size(), -1);
		std::vector<int64_t> GltfOf;   // per nodo nostro: il nodo di glTF (-1 la radice)
		GltfOf.push_back(-1);
		const std::function<void(int64_t, int32_t, int)> AddNode = [&](int64_t G, int32_t Parent, int Depth)
		{
			if (G < 0 || static_cast<size_t>(G) >= NodeList.Size()) { Out.Warn("un nodo che non esiste: " + std::to_string(G)); return; }
			if (NodeMap[static_cast<size_t>(G)] >= 0) { Out.Warn("il nodo " + std::to_string(G) + " compare due volte: la seconda si ignora"); return; }
			if (Depth > 256) { Out.Warn("gerarchia troppo profonda"); return; }
			const Json::FValue& J = NodeList[static_cast<size_t>(G)];
			const int32_t Index = Out.AddNode(Parent, ENodeKind::Group, J["name"].StringOr("nodo " + std::to_string(G)));
			NodeMap[static_cast<size_t>(G)] = Index;
			GltfOf.push_back(G);
			FModelNode& Node = Out.Nodes[static_cast<size_t>(Index)];
			if (J["matrix"].Size() == 16)
			{
				double C[16];
				for (size_t K = 0; K < 16; ++K) { C[K] = J["matrix"][K].NumberOr(K % 5 == 0 ? 1.0 : 0.0); }
				Node.Local = MatrixFromColumnMajor(C);
				Node.Trs = MatrixToTrs(Node.Local);
			}
			else
			{
				for (size_t K = 0; K < 3; ++K) { Node.Trs.T[K] = J["translation"][K].NumberOr(0.0); Node.Trs.S[K] = J["scale"][K].NumberOr(1.0); }
				for (size_t K = 0; K < 4; ++K) { Node.Trs.R[K] = J["rotation"][K].NumberOr(K == 3 ? 1.0 : 0.0); }
				Node.Local = TrsMatrix(Node.Trs);
			}
			Node.bHasMatrix = !IsIdentity(Node.Local);
			const Json::FValue& Children = J["children"];
			for (size_t K = 0; K < Children.Size(); ++K) { AddNode(Children[K].IndexOr(), Index, Depth + 1); }
		};
		const Json::FValue& Scenes = Root["scenes"];
		if (Scenes.Size() > 0)
		{
			const int64_t Chosen = Root["scene"].IndexOr(0);
			const Json::FValue& Scene = Scenes[static_cast<size_t>(Chosen < static_cast<int64_t>(Scenes.Size()) ? Chosen : 0)];
			for (size_t K = 0; K < Scene["nodes"].Size(); ++K) { AddNode(Scene["nodes"][K].IndexOr(), 0, 1); }
			if (Scenes.Size() > 1) { Out.Warn(std::to_string(Scenes.Size()) + " scene: si disegna la " + std::to_string(Chosen) + " (quella di partenza)"); }
		}
		else
		{
			// Nessuna scena: tutti i nodi che non sono figli di qualcuno.
			std::vector<bool> IsChild(NodeList.Size(), false);
			for (size_t G = 0; G < NodeList.Size(); ++G)
			{
				const Json::FValue& Children = NodeList[G]["children"];
				for (size_t K = 0; K < Children.Size(); ++K)
				{
					const int64_t C = Children[K].IndexOr();
					if (C >= 0 && static_cast<size_t>(C) < IsChild.size()) { IsChild[static_cast<size_t>(C)] = true; }
				}
			}
			for (size_t G = 0; G < NodeList.Size(); ++G) { if (!IsChild[G]) { AddNode(static_cast<int64_t>(G), 0, 1); } }
		}

		// Le trasformazioni di riposo, dalla radice (i padri vengono prima dei figli).
		std::vector<FAffine> Global(Out.Nodes.size());
		for (size_t I = 1; I < Out.Nodes.size(); ++I)
		{
			const FModelNode& N = Out.Nodes[I];
			Global[I] = Global[static_cast<size_t>(N.Parent)] * N.Local;
		}

		// ---- La geometria dei nodi -------------------------------------------------------
		for (size_t I = 1; I < Out.Nodes.size(); ++I)
		{
			const Json::FValue& J = NodeList[static_cast<size_t>(GltfOf[I])];
			const int64_t MeshIndex = J["mesh"].IndexOr();
			if (MeshIndex < 0) { continue; }
			if (static_cast<size_t>(MeshIndex) >= MeshList.Size()) { Out.Warn("una mesh che non esiste: " + std::to_string(MeshIndex)); continue; }
			std::string MeshError;
			if (!ConvertMesh(static_cast<size_t>(MeshIndex), MeshError)) { Error = FileName + ": " + MeshError; return false; }

			// Lo scheletro: le matrici delle ossa nella posa di riposo.
			const int64_t SkinIndex = J["skin"].IndexOr();
			std::vector<FAffine> JointMatrices;
			if (SkinIndex >= 0)
			{
				const Json::FValue& Skin = Root["skins"][static_cast<size_t>(SkinIndex)];
				const Json::FValue& Joints = Skin["joints"];
				std::vector<float> Inverse;
				int Components = 0;
				size_t Count = 0;
				const bool bInverse = Skin["inverseBindMatrices"].IsNumber()
					&& ReadAccessor(Doc, Skin["inverseBindMatrices"].IndexOr(), Inverse, Components, Count, MeshError) && Components == 16 && Count >= Joints.Size();
				for (size_t K = 0; K < Joints.Size(); ++K)
				{
					const int64_t G = Joints[K].IndexOr();
					const int32_t Ours = (G >= 0 && static_cast<size_t>(G) < NodeMap.size()) ? NodeMap[static_cast<size_t>(G)] : -1;
					FAffine Bind;
					if (bInverse)
					{
						double C[16];
						for (size_t E = 0; E < 16; ++E) { C[E] = Inverse[K * 16 + E]; }
						Bind = MatrixFromColumnMajor(C);
					}
					JointMatrices.push_back((Ours >= 0 ? Global[static_cast<size_t>(Ours)] : FAffine::Identity()) * Bind);
				}
				Out.Warn("'" + Out.Nodes[I].Name + "': ha uno scheletro: la mesh e' nella posa di riposo (le ossa non la muovono)");
			}

			for (const FPrimitive& Prim : MeshCache[static_cast<size_t>(MeshIndex)])
			{
				const bool bSkinned = !JointMatrices.empty() && Prim.Joints.size() == Prim.Vertices.size();
				// Una mesh con lo scheletro sta nel sistema della radice (glTF ignora la trasformazione del suo nodo).
				FModelMesh& Mesh = Out.MeshFor(bSkinned ? 0 : static_cast<int32_t>(I), Prim.Material);
				const uint32_t Base = static_cast<uint32_t>(Mesh.Vertices.size());
				for (size_t V = 0; V < Prim.Vertices.size(); ++V)
				{
					FModelVertex Vertex = Prim.Vertices[V];
					if (bSkinned)
					{
						double P[3] = { 0, 0, 0 }, N[3] = { 0, 0, 0 }, Total = 0.0;
						for (size_t K = 0; K < 4; ++K)
						{
							const double W = Prim.Weights[V][K];
							const uint32_t Joint = Prim.Joints[V][K];
							if (W <= 0.0 || Joint >= JointMatrices.size()) { continue; }
							double Pk[3], Nk[3];
							const double Normal[3] = { Vertex.N[0], Vertex.N[1], Vertex.N[2] };
							JointMatrices[Joint].ApplyPoint(Vertex.P, Pk);
							JointMatrices[Joint].ApplyVector(Normal, Nk);
							for (int C = 0; C < 3; ++C) { P[C] += W * Pk[C]; N[C] += W * Nk[C]; }
							Total += W;
						}
						if (Total > 0.0)
						{
							const double L = std::sqrt(N[0] * N[0] + N[1] * N[1] + N[2] * N[2]);
							for (int C = 0; C < 3; ++C) { Vertex.P[C] = P[C] / Total; Vertex.N[C] = L > 0.0 ? static_cast<float>(N[C] / L) : Vertex.N[C]; }
						}
					}
					Mesh.Vertices.push_back(Vertex);
				}
				for (const uint32_t Index : Prim.Indices) { Mesh.Indices.push_back(Base + Index); }
			}
		}

		// ---- Le luci (KHR_lights_punctual): punti luce per M7 -----------------------------
		const Json::FValue& Lights = Root["extensions"]["KHR_lights_punctual"]["lights"];
		for (size_t I = 1; I < Out.Nodes.size(); ++I)
		{
			const int64_t LightIndex = NodeList[static_cast<size_t>(GltfOf[I])]["extensions"]["KHR_lights_punctual"]["light"].IndexOr();
			if (LightIndex < 0 || static_cast<size_t>(LightIndex) >= Lights.Size()) { continue; }
			const Json::FValue& L = Lights[static_cast<size_t>(LightIndex)];
			FModelLightPoint Light;
			Light.Node = static_cast<int32_t>(I);
			for (size_t C = 0; C < 3; ++C) { Light.Rgba[C] = LinearToVertexByte(L["color"][C].NumberOr(1.0)); }
			Light.Directionality = L["type"].StringOr("point") == "point" ? 0 : 1;
			// Le luci di glTF guardano verso -Z del loro nodo.
			Light.Normal[0] = 0.0f; Light.Normal[1] = 0.0f; Light.Normal[2] = -1.0f;
			Out.LightPoints.push_back(Light);
		}

		// ---- Le animazioni --------------------------------------------------------------
		const Json::FValue& AnimationList = Root["animations"];
		for (size_t A = 0; A < AnimationList.Size(); ++A)
		{
			const Json::FValue& J = AnimationList[A];
			FModelAnimation Animation;
			Animation.Name = J["name"].StringOr("animazione " + std::to_string(A));
			const Json::FValue& Samplers = J["samplers"];
			const Json::FValue& Channels = J["channels"];
			for (size_t C = 0; C < Channels.Size(); ++C)
			{
				const Json::FValue& Channel = Channels[C];
				const std::string PathName = Channel["target"]["path"].StringOr("");
				const int64_t G = Channel["target"]["node"].IndexOr();
				if (PathName == "weights") { Out.Warn(Animation.Name + ": animazione dei morph target ignorata"); continue; }
				if (G < 0 || static_cast<size_t>(G) >= NodeMap.size() || NodeMap[static_cast<size_t>(G)] < 0) { continue; }
				FModelAnimChannel Track;
				Track.Node = NodeMap[static_cast<size_t>(G)];
				if (PathName == "translation") { Track.Curve.Path = EAnimPath::Translation; }
				else if (PathName == "rotation") { Track.Curve.Path = EAnimPath::Rotation; }
				else if (PathName == "scale") { Track.Curve.Path = EAnimPath::Scale; }
				else { continue; }
				const Json::FValue& Sampler = Samplers[static_cast<size_t>(std::max<int64_t>(0, Channel["sampler"].IndexOr()))];
				const std::string Interpolation = Sampler["interpolation"].StringOr("LINEAR");
				Track.Curve.Interpolation = Interpolation == "STEP" ? EAnimInterpolation::Step
					: Interpolation == "CUBICSPLINE" ? EAnimInterpolation::CubicSpline : EAnimInterpolation::Linear;
				int Components = 0;
				size_t Count = 0;
				std::string CurveError;
				if (!ReadAccessor(Doc, Sampler["input"].IndexOr(), Track.Curve.Times, Components, Count, CurveError)
					|| !ReadAccessor(Doc, Sampler["output"].IndexOr(), Track.Curve.Values, Components, Count, CurveError)
					|| Components != Track.Curve.Components() || !Track.Curve.IsValid())
				{
					Out.Warn(Animation.Name + ": un canale non si legge" + (CurveError.empty() ? std::string() : " (" + CurveError + ")"));
					continue;
				}
				Out.Nodes[static_cast<size_t>(Track.Node)].bAnimated = true;
				Animation.Channels.push_back(std::move(Track));
			}
			if (!Animation.Channels.empty()) { Out.Animations.push_back(std::move(Animation)); }
		}
		return true;
	}
}

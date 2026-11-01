// =============================================================================
//  ModelData.h -- Un modello 3D com'e' nel file. STRATO: C++ PURO.
//
//  La rappresentazione comune a tutti i formati (docs/modelli-design.md,
//  sezione 2): un albero di nodi, ognuno con una trasformazione e forse della
//  geometria; alcuni nodi si muovono (DOF), altri si accendono e si spengono
//  (switch, LOD, sequenze). I lettori (OpenFlightReader.h, ObjReader.h) la
//  riempiono; il rig (ModelRig.h) la porta nelle convenzioni dell'attore.
//
//  QUI TUTTO E' NELLE CONVENZIONI DEL FORMATO: gli assi (Axes), la mano
//  destra, l'origine delle UV (bUvOriginBottom). Solo le unita' sono gia'
//  metri: i lettori moltiplicano subito (OpenFlight ha piedi e pollici).
//  La conversione verso l'attore avviene in un punto solo, nel rig.
//
//  I triangoli: (B - A) x (C - A) punta FUORI (la faccia davanti), nelle
//  coordinate del formato, che sono destrorse. E' la regola di OpenGL, di
//  OpenFlight, di OBJ e di glTF.
// =============================================================================
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <string>
#include <tuple>
#include <vector>

namespace GeoWorld::Models
{
	// =========================================================================
	//  La trasformazione affine
	// =========================================================================

	/** p' = M p + T. M per righe. */
	struct FAffine
	{
		double M[3][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
		double T[3] = { 0, 0, 0 };

		static FAffine Identity() { return FAffine{}; }

		static FAffine Translation(double X, double Y, double Z)
		{
			FAffine Out;
			Out.T[0] = X; Out.T[1] = Y; Out.T[2] = Z;
			return Out;
		}

		/** Dalle colonne (le immagini degli assi) e dall'origine. */
		static FAffine FromColumns(const double X[3], const double Y[3], const double Z[3], const double Origin[3])
		{
			FAffine Out;
			for (int I = 0; I < 3; ++I)
			{
				Out.M[I][0] = X[I]; Out.M[I][1] = Y[I]; Out.M[I][2] = Z[I];
				Out.T[I] = Origin[I];
			}
			return Out;
		}

		/** Prima O, poi questa (come le matrici: A * B applica B per prima). */
		FAffine operator*(const FAffine& O) const
		{
			FAffine Out;
			for (int I = 0; I < 3; ++I)
			{
				for (int J = 0; J < 3; ++J)
				{
					Out.M[I][J] = M[I][0] * O.M[0][J] + M[I][1] * O.M[1][J] + M[I][2] * O.M[2][J];
				}
				Out.T[I] = M[I][0] * O.T[0] + M[I][1] * O.T[1] + M[I][2] * O.T[2] + T[I];
			}
			return Out;
		}

		void ApplyPoint(const double In[3], double Out[3]) const
		{
			for (int I = 0; I < 3; ++I) { Out[I] = M[I][0] * In[0] + M[I][1] * In[1] + M[I][2] * In[2] + T[I]; }
		}

		void ApplyVector(const double In[3], double Out[3]) const
		{
			for (int I = 0; I < 3; ++I) { Out[I] = M[I][0] * In[0] + M[I][1] * In[1] + M[I][2] * In[2]; }
		}

		double Determinant() const
		{
			return M[0][0] * (M[1][1] * M[2][2] - M[1][2] * M[2][1])
			     - M[0][1] * (M[1][0] * M[2][2] - M[1][2] * M[2][0])
			     + M[0][2] * (M[1][0] * M[2][1] - M[1][1] * M[2][0]);
		}

		/** L'inversa (qualsiasi matrice non singolare; la singolare da' l'identita'). */
		FAffine Inverse() const
		{
			const double D = Determinant();
			FAffine Out;
			if (std::fabs(D) < 1e-300) { return Out; }
			const double I = 1.0 / D;
			Out.M[0][0] = (M[1][1] * M[2][2] - M[1][2] * M[2][1]) * I;
			Out.M[0][1] = (M[0][2] * M[2][1] - M[0][1] * M[2][2]) * I;
			Out.M[0][2] = (M[0][1] * M[1][2] - M[0][2] * M[1][1]) * I;
			Out.M[1][0] = (M[1][2] * M[2][0] - M[1][0] * M[2][2]) * I;
			Out.M[1][1] = (M[0][0] * M[2][2] - M[0][2] * M[2][0]) * I;
			Out.M[1][2] = (M[0][2] * M[1][0] - M[0][0] * M[1][2]) * I;
			Out.M[2][0] = (M[1][0] * M[2][1] - M[1][1] * M[2][0]) * I;
			Out.M[2][1] = (M[0][1] * M[2][0] - M[0][0] * M[2][1]) * I;
			Out.M[2][2] = (M[0][0] * M[1][1] - M[0][1] * M[1][0]) * I;
			for (int R = 0; R < 3; ++R) { Out.T[R] = -(Out.M[R][0] * T[0] + Out.M[R][1] * T[1] + Out.M[R][2] * T[2]); }
			return Out;
		}

		/** Le normali si trasformano con l'inversa trasposta della parte lineare. */
		void ApplyNormal(const double In[3], double Out[3]) const
		{
			const FAffine Inv = Inverse();
			for (int I = 0; I < 3; ++I) { Out[I] = Inv.M[0][I] * In[0] + Inv.M[1][I] * In[1] + Inv.M[2][I] * In[2]; }
			const double L = std::sqrt(Out[0] * Out[0] + Out[1] * Out[1] + Out[2] * Out[2]);
			if (L > 0.0) { for (int K = 0; K < 3; ++K) { Out[K] /= L; } }
		}

		/** Una rotazione attorno a un asse unitario (mano destra), radianti. */
		static FAffine Rotation(int Axis, double Radians)
		{
			FAffine Out;
			const double C = std::cos(Radians), S = std::sin(Radians);
			const int A = (Axis + 1) % 3, B = (Axis + 2) % 3;
			Out.M[A][A] = C; Out.M[A][B] = -S;
			Out.M[B][A] = S; Out.M[B][B] = C;
			return Out;
		}

		static FAffine Scale(double X, double Y, double Z)
		{
			FAffine Out;
			Out.M[0][0] = X; Out.M[1][1] = Y; Out.M[2][2] = Z;
			return Out;
		}
	};

	// =========================================================================
	//  Gli assi di un formato
	// =========================================================================

	/** Un asse con il suo segno: 0 +X, 1 -X, 2 +Y, 3 -Y, 4 +Z, 5 -Z. */
	enum class EAxis : uint8_t { PosX = 0, NegX = 1, PosY = 2, NegY = 3, PosZ = 4, NegZ = 5 };

	inline void AxisVector(EAxis Axis, double Out[3])
	{
		Out[0] = Out[1] = Out[2] = 0.0;
		const int Index = static_cast<int>(Axis) / 2;
		Out[Index] = (static_cast<int>(Axis) % 2 == 0) ? 1.0 : -1.0;
	}

	/** "x", "-z", "+Y"... false se non e' un asse. */
	inline bool ParseAxis(const std::string& Text, EAxis& Out)
	{
		std::string T = Text;
		bool bNegative = false;
		if (!T.empty() && (T[0] == '-' || T[0] == '+')) { bNegative = T[0] == '-'; T = T.substr(1); }
		if (T.size() != 1) { return false; }
		const char C = static_cast<char>(T[0] | 0x20);
		if (C < 'x' || C > 'z') { return false; }
		Out = static_cast<EAxis>((C - 'x') * 2 + (bNegative ? 1 : 0));
		return true;
	}

	inline const char* AxisName(EAxis Axis)
	{
		static const char* Names[6] = { "+x", "-x", "+y", "-y", "+z", "-z" };
		return Names[static_cast<int>(Axis)];
	}

	/**
	 * Dove guardano "avanti" e "alto" nelle coordinate di un formato
	 * (destrorso). La destra e' avanti x alto.
	 */
	struct FAxisConvention
	{
		EAxis Forward = EAxis::PosY;
		EAxis Up = EAxis::PosZ;

		bool IsValid() const { return static_cast<int>(Forward) / 2 != static_cast<int>(Up) / 2; }

		static FAxisConvention OpenFlight() { return FAxisConvention{ EAxis::PosY, EAxis::PosZ }; }
		static FAxisConvention Obj() { return FAxisConvention{ EAxis::NegZ, EAxis::PosY }; }
		static FAxisConvention Gltf() { return FAxisConvention{ EAxis::PosZ, EAxis::PosY }; }
	};

	/**
	 * Dalle coordinate del formato a quelle dell'ATTORE (X avanti, Y destra,
	 * Z alto, sinistrorso): le righe sono avanti, destra, alto. Determinante
	 * -1 per ogni formato destrorso: e' una riflessione, i triangoli si girano.
	 */
	inline FAffine ToActorAxes(const FAxisConvention& Axes)
	{
		double F[3], U[3];
		AxisVector(Axes.Forward, F);
		AxisVector(Axes.Up, U);
		const double R[3] = { F[1] * U[2] - F[2] * U[1], F[2] * U[0] - F[0] * U[2], F[0] * U[1] - F[1] * U[0] };
		FAffine Out;
		for (int J = 0; J < 3; ++J) { Out.M[0][J] = F[J]; Out.M[1][J] = R[J]; Out.M[2][J] = U[J]; }
		return Out;
	}

	// =========================================================================
	//  Geometria e materiali
	// =========================================================================

	struct FModelVertex
	{
		double P[3] = { 0, 0, 0 };
		float N[3] = { 0, 0, 1 };
		float UV[2] = { 0, 0 };
		/** Il colore (sRGB, 0..255): quello della faccia o del vertice, per il diffuso del materiale. */
		uint8_t Rgba[4] = { 255, 255, 255, 255 };
	};

	struct FModelTexture
	{
		/** Come e' scritto nel file. */
		std::string Path;
		/** Dove e' stato trovato (vuoto: non trovato). */
		std::string Resolved;
	};

	struct FModelMaterial
	{
		std::string Name;
		/** In FModel::Textures; -1 nessuna. */
		int32_t Texture = -1;
		bool bTwoSided = false;
		/** Trasparente (alfa < 1 della faccia o del materiale). Quella della texture si scopre leggendola. */
		bool bTranslucent = false;
		/** Luce propria (0..1, lineare): finestre accese, display. */
		float Emissive[3] = { 0.0f, 0.0f, 0.0f };
		/** Quanto e' lucido, 0..1. */
		float Shininess = 0.0f;
		/** I codici di OpenFlight per i sensori: materiale IR e codice di superficie. -1 nessuno. */
		int32_t IrMaterial = -1;
		int32_t SurfaceCode = -1;

		bool operator<(const FModelMaterial& O) const
		{
			return std::tie(Texture, bTwoSided, bTranslucent, Emissive[0], Emissive[1], Emissive[2], Shininess, IrMaterial, SurfaceCode, Name)
				< std::tie(O.Texture, O.bTwoSided, O.bTranslucent, O.Emissive[0], O.Emissive[1], O.Emissive[2], O.Shininess, O.IrMaterial, O.SurfaceCode, O.Name);
		}
	};

	/** Triangoli di un materiale, nelle coordinate del loro nodo. */
	struct FModelMesh
	{
		int32_t Material = 0;
		std::vector<FModelVertex> Vertices;
		std::vector<uint32_t> Indices;

		size_t TriangleCount() const { return Indices.size() / 3; }
	};

	/** Un punto luce (le luci di OpenFlight): per le luci delle entita' (M7). */
	struct FModelLightPoint
	{
		int32_t Node = 0;
		double P[3] = { 0, 0, 0 };
		uint8_t Rgba[4] = { 255, 255, 255, 255 };
		/** 0 omnidirezionale, 1 in una direzione, 2 in due. */
		int32_t Directionality = 0;
		float Normal[3] = { 0, 0, 1 };
	};

	// =========================================================================
	//  I nodi
	// =========================================================================

	enum class ENodeKind : uint8_t { Group, Object, Dof, Lod, Switch, Sequence, ExternalRef };

	inline const char* NodeKindName(ENodeKind Kind)
	{
		switch (Kind)
		{
		case ENodeKind::Object: return "oggetto";
		case ENodeKind::Dof: return "DOF";
		case ENodeKind::Lod: return "LOD";
		case ENodeKind::Switch: return "switch";
		case ENodeKind::Sequence: return "sequenza";
		case ENodeKind::ExternalRef: return "riferimento esterno";
		default: return "gruppo";
		}
	}

	struct FDofAxis
	{
		double Min = 0.0, Max = 0.0, Current = 0.0, Step = 0.0;
	};

	/**
	 * Un grado di liberta' (il record DOF di OpenFlight). Frame porta dal
	 * sistema del DOF a quello del nodo padre (colonne: gli assi del DOF,
	 * T: la sua origine). Nel sistema del DOF il moto e':
	 *
	 *     Moto = Traslazione * Rz(imbardata) * Rx(beccheggio) * Ry(rollio) * Scala
	 *
	 * (l'ordine delle rotazioni degli aerei: prima l'imbardata, poi il
	 * beccheggio, poi il rollio, attorno agli assi che si muovono). Gli angoli
	 * sono in GRADI e MATEMATICI: positivi in senso antiorario guardando
	 * dall'asse in giu' (l'imbardata positiva gira a SINISTRA, al contrario
	 * della bussola). Traslazioni in metri.
	 */
	struct FDof
	{
		FAffine Frame;
		FDofAxis Translate[3];
		/** 0 beccheggio (attorno a x), 1 rollio (attorno a y), 2 imbardata (attorno a z). */
		FDofAxis Rotate[3];
		FDofAxis Scale[3] = { FDofAxis{ 1, 1, 1, 0 }, FDofAxis{ 1, 1, 1, 0 }, FDofAxis{ 1, 1, 1, 0 } };
		uint32_t Flags = 0;
	};

	/** I valori correnti di un DOF: traslazione (m), rotazione (gradi: beccheggio, rollio, imbardata), scala. */
	struct FDofValues
	{
		double Translate[3] = { 0, 0, 0 };
		double Rotate[3] = { 0, 0, 0 };
		double Scale[3] = { 1, 1, 1 };

		static FDofValues Current(const FDof& Dof)
		{
			FDofValues Out;
			for (int I = 0; I < 3; ++I)
			{
				Out.Translate[I] = Dof.Translate[I].Current;
				Out.Rotate[I] = Dof.Rotate[I].Current;
				Out.Scale[I] = Dof.Scale[I].Current == 0.0 ? 1.0 : Dof.Scale[I].Current;
			}
			return Out;
		}
	};

	/** Il moto di un DOF nel suo sistema (vedi FDof). */
	inline FAffine DofMotion(const FDofValues& V)
	{
		constexpr double DegToRad = 3.14159265358979323846 / 180.0;
		return FAffine::Translation(V.Translate[0], V.Translate[1], V.Translate[2])
			* FAffine::Rotation(2, V.Rotate[2] * DegToRad)
			* FAffine::Rotation(0, V.Rotate[0] * DegToRad)
			* FAffine::Rotation(1, V.Rotate[1] * DegToRad)
			* FAffine::Scale(V.Scale[0], V.Scale[1], V.Scale[2]);
	}

	/**
	 * La trasformazione completa del DOF, dal sistema del nodo (quello dei
	 * figli) a quello del padre: Frame * Moto * Frame^-1. A riposo (tutto
	 * zero) e' l'identita': i figli stanno dove li ha messi il modellatore.
	 */
	inline FAffine DofTransform(const FDof& Dof, const FDofValues& V)
	{
		return Dof.Frame * DofMotion(V) * Dof.Frame.Inverse();
	}

	/** I valori tenuti nei limiti (dove il limite c'e': Max > Min). */
	inline FDofValues ClampDof(const FDof& Dof, FDofValues V)
	{
		for (int I = 0; I < 3; ++I)
		{
			if (Dof.Translate[I].Max > Dof.Translate[I].Min) { V.Translate[I] = std::clamp(V.Translate[I], Dof.Translate[I].Min, Dof.Translate[I].Max); }
			if (Dof.Rotate[I].Max > Dof.Rotate[I].Min) { V.Rotate[I] = std::clamp(V.Rotate[I], Dof.Rotate[I].Min, Dof.Rotate[I].Max); }
		}
		return V;
	}

	struct FModelNode
	{
		std::string Name;
		ENodeKind Kind = ENodeKind::Group;
		int32_t Parent = -1;
		std::vector<int32_t> Children;
		/** La matrice del nodo (dal suo sistema a quello del padre). */
		FAffine Local;
		bool bHasMatrix = false;
		/** La geometria del nodo, nel suo sistema (in FModel::Meshes). */
		std::vector<int32_t> Meshes;

		// LOD: visibile fra SwitchOut (vicino) e SwitchIn (lontano), dalla distanza dal centro.
		double SwitchIn = 0.0, SwitchOut = 0.0;
		double LodCenter[3] = { 0, 0, 0 };

		// Switch: quale maschera e' accesa, e le maschere (WordsPerMask parole a 32 bit ciascuna, bit 0 = figlio 0).
		int32_t CurrentMask = 0;
		int32_t WordsPerMask = 0;
		std::vector<uint32_t> Masks;

		// Sequenza (gruppo animato di OpenFlight): i figli sono i fotogrammi.
		float LoopDuration = 0.0f, LastFrameDuration = 0.0f;
		int32_t LoopCount = 0;
		bool bSwing = false;

		FDof Dof;
		/** I bit del record (oggetti: nascosto di giorno, di notte...). */
		uint32_t Flags = 0;
		/** Riferimento esterno: il file. */
		std::string ExternalPath;

		int32_t MaskCount() const { return WordsPerMask > 0 ? static_cast<int32_t>(Masks.size()) / WordsPerMask : 0; }

		bool MaskShows(int32_t Mask, int32_t Child) const
		{
			if (Mask < 0 || Mask >= MaskCount() || Child < 0 || Child / 32 >= WordsPerMask) { return false; }
			return (Masks[static_cast<size_t>(Mask * WordsPerMask + Child / 32)] >> (Child % 32) & 1u) != 0;
		}
	};

	// =========================================================================
	//  Il modello
	// =========================================================================

	struct FModel
	{
		std::vector<FModelNode> Nodes;
		std::vector<FModelMesh> Meshes;
		std::vector<FModelMaterial> Materials;
		std::vector<FModelTexture> Textures;
		std::vector<FModelLightPoint> LightPoints;
		FAxisConvention Axes;
		/** L'origine delle UV e' in basso (OpenGL: OpenFlight, OBJ); in alto per glTF e Unreal. */
		bool bUvOriginBottom = true;
		/** "OpenFlight 16.4", "OBJ". */
		std::string Format;
		std::vector<std::string> Warnings;

		FModel() { Nodes.push_back(FModelNode{}); Nodes[0].Name = "radice"; }

		int32_t AddNode(int32_t Parent, ENodeKind Kind, const std::string& Name)
		{
			FModelNode Node;
			Node.Kind = Kind;
			Node.Name = Name;
			Node.Parent = Parent;
			const int32_t Index = static_cast<int32_t>(Nodes.size());
			Nodes.push_back(std::move(Node));
			if (Parent >= 0) { Nodes[static_cast<size_t>(Parent)].Children.push_back(Index); }
			return Index;
		}

		int32_t FindOrAddMaterial(const FModelMaterial& Material)
		{
			const auto It = MaterialIndex.find(Material);
			if (It != MaterialIndex.end()) { return It->second; }
			const int32_t Index = static_cast<int32_t>(Materials.size());
			Materials.push_back(Material);
			MaterialIndex.emplace(Material, Index);
			return Index;
		}

		int32_t FindOrAddTexture(const std::string& Path)
		{
			for (size_t Index = 0; Index < Textures.size(); ++Index)
			{
				if (Textures[Index].Path == Path) { return static_cast<int32_t>(Index); }
			}
			Textures.push_back(FModelTexture{ Path, std::string() });
			return static_cast<int32_t>(Textures.size() - 1);
		}

		/** I triangoli di quel materiale in quel nodo (una mesh per coppia). */
		FModelMesh& MeshFor(int32_t Node, int32_t Material)
		{
			const auto Key = std::make_pair(Node, Material);
			const auto It = MeshIndex.find(Key);
			if (It != MeshIndex.end()) { return Meshes[static_cast<size_t>(It->second)]; }
			const int32_t Index = static_cast<int32_t>(Meshes.size());
			Meshes.push_back(FModelMesh{});
			Meshes.back().Material = Material;
			MeshIndex.emplace(Key, Index);
			Nodes[static_cast<size_t>(Node)].Meshes.push_back(Index);
			return Meshes.back();
		}

		/**
		 * Un poligono convesso come ventaglio di triangoli. I vertici in ordine
		 * antiorario visti da davanti (la regola dei formati). Le normali che
		 * mancano (N tutto zero) diventano quella della faccia.
		 */
		void AddPolygon(int32_t Node, int32_t Material, std::vector<FModelVertex> Corners)
		{
			if (Corners.size() < 3) { return; }
			double Normal[3] = { 0, 0, 0 };
			// Newell: la normale di un poligono anche non perfettamente piano.
			for (size_t I = 0; I < Corners.size(); ++I)
			{
				const double* A = Corners[I].P;
				const double* B = Corners[(I + 1) % Corners.size()].P;
				Normal[0] += (A[1] - B[1]) * (A[2] + B[2]);
				Normal[1] += (A[2] - B[2]) * (A[0] + B[0]);
				Normal[2] += (A[0] - B[0]) * (A[1] + B[1]);
			}
			const double Length = std::sqrt(Normal[0] * Normal[0] + Normal[1] * Normal[1] + Normal[2] * Normal[2]);
			if (Length < 1e-12) { return; }   // degenere: un segmento o un punto
			for (FModelVertex& V : Corners)
			{
				if (V.N[0] == 0.0f && V.N[1] == 0.0f && V.N[2] == 0.0f)
				{
					for (int I = 0; I < 3; ++I) { V.N[I] = static_cast<float>(Normal[I] / Length); }
				}
			}
			FModelMesh& Mesh = MeshFor(Node, Material);
			const uint32_t Base = static_cast<uint32_t>(Mesh.Vertices.size());
			for (const FModelVertex& V : Corners) { Mesh.Vertices.push_back(V); }
			for (uint32_t I = 1; I + 1 < Corners.size(); ++I)
			{
				Mesh.Indices.push_back(Base);
				Mesh.Indices.push_back(Base + I);
				Mesh.Indices.push_back(Base + I + 1);
			}
		}

		size_t TriangleCount() const
		{
			size_t Count = 0;
			for (const FModelMesh& Mesh : Meshes) { Count += Mesh.TriangleCount(); }
			return Count;
		}

		void Warn(const std::string& Message)
		{
			// Non mille volte la stessa cosa.
			if (Warnings.size() < 64 && std::find(Warnings.begin(), Warnings.end(), Message) == Warnings.end()) { Warnings.push_back(Message); }
		}

		/**
		 * Mette un altro modello (un riferimento esterno) sotto un nodo: i suoi
		 * nodi, la sua geometria, i suoi materiali e le sue texture, rinumerati.
		 * I due modelli devono avere gli stessi assi (due OpenFlight).
		 */
		void Merge(int32_t UnderNode, const FModel& From)
		{
			std::vector<int32_t> TextureMap, MaterialMap, MeshMap, NodeMap(From.Nodes.size(), -1);
			for (const FModelTexture& Texture : From.Textures)
			{
				const int32_t Index = FindOrAddTexture(Texture.Path);
				if (Textures[static_cast<size_t>(Index)].Resolved.empty()) { Textures[static_cast<size_t>(Index)].Resolved = Texture.Resolved; }
				TextureMap.push_back(Index);
			}
			for (FModelMaterial Material : From.Materials)
			{
				if (Material.Texture >= 0) { Material.Texture = TextureMap[static_cast<size_t>(Material.Texture)]; }
				MaterialMap.push_back(FindOrAddMaterial(Material));
			}
			for (FModelMesh Mesh : From.Meshes)
			{
				Mesh.Material = MaterialMap[static_cast<size_t>(Mesh.Material)];
				MeshMap.push_back(static_cast<int32_t>(Meshes.size()));
				Meshes.push_back(std::move(Mesh));
			}
			// La radice dell'altro modello e' il nodo d'aggancio.
			NodeMap[0] = UnderNode;
			for (const int32_t Mesh : From.Nodes[0].Meshes) { Nodes[static_cast<size_t>(UnderNode)].Meshes.push_back(MeshMap[static_cast<size_t>(Mesh)]); }
			for (size_t I = 1; I < From.Nodes.size(); ++I)
			{
				NodeMap[I] = static_cast<int32_t>(Nodes.size());
				Nodes.push_back(From.Nodes[I]);
			}
			for (size_t I = 1; I < From.Nodes.size(); ++I)
			{
				FModelNode& Node = Nodes[static_cast<size_t>(NodeMap[I])];
				Node.Parent = Node.Parent >= 0 ? NodeMap[static_cast<size_t>(Node.Parent)] : -1;
				for (int32_t& Child : Node.Children) { Child = NodeMap[static_cast<size_t>(Child)]; }
				for (int32_t& Mesh : Node.Meshes) { Mesh = MeshMap[static_cast<size_t>(Mesh)]; }
			}
			for (const int32_t Child : From.Nodes[0].Children) { Nodes[static_cast<size_t>(UnderNode)].Children.push_back(NodeMap[static_cast<size_t>(Child)]); }
			for (FModelLightPoint Light : From.LightPoints)
			{
				Light.Node = NodeMap[static_cast<size_t>(Light.Node)];
				LightPoints.push_back(Light);
			}
			for (const std::string& Warning : From.Warnings) { Warn(Nodes[static_cast<size_t>(UnderNode)].Name + ": " + Warning); }
		}

	private:
		std::map<FModelMaterial, int32_t> MaterialIndex;
		std::map<std::pair<int32_t, int32_t>, int32_t> MeshIndex;
	};
}

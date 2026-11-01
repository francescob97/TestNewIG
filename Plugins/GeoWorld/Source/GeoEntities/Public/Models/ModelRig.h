// =============================================================================
//  ModelRig.h -- Dal modello com'e' nel file a quello che si disegna.
//  STRATO: C++ PURO.
//
//  E' il punto UNICO dove le convenzioni del formato diventano quelle
//  dell'attore (docs/modelli-design.md, sezione 4):
//
//    - gli assi: quelli del formato (FModel::Axes, o la tabella dei modelli)
//      diventano X avanti, Y destra, Z alto; la scala, l'imbardata e lo
//      spostamento della tabella si applicano qui;
//    - la mano: da destra a sinistra e' una riflessione, e ogni pezzo con
//      determinante negativo ha i triangoli girati (anche le parti "a
//      specchio" del modello: due riflessioni si annullano);
//    - le UV: origine in alto, come Unreal (V' = 1 - V per i formati OpenGL);
//    - i triangoli: (B - A) x (C - A), fatto con le coordinate dell'attore,
//      punta FUORI, come quelli degli alberi e del terreno.
//
//  E i nodi diventano PARTI (sezione 5). Spariscono i gruppi, gli oggetti, le
//  matrici: la loro trasformazione si cuoce nei vertici. Restano parti solo:
//    - la radice;
//    - ogni DOF (si muove: la sua trasformazione e' Prima * Moto * Dopo);
//    - ogni LOD, ogni figlio di switch o di sequenza (si accende e si spegne:
//      la parte porta le CONDIZIONI per essere visibile).
//  Le parti vuote (senza geometria e senza figli) si tolgono.
// =============================================================================
#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "Models/ModelData.h"

namespace GeoWorld::Models
{
	struct FRigVertex
	{
		float P[3] = { 0, 0, 0 };
		float N[3] = { 0, 0, 1 };
		float UV[2] = { 0, 0 };
		uint8_t Rgba[4] = { 255, 255, 255, 255 };
	};

	struct FRigSection
	{
		int32_t Material = 0;
		std::vector<FRigVertex> Vertices;
		std::vector<uint32_t> Indices;
	};

	/** Visibile se Near <= distanza dal centro < Far (il centro nel sistema della radice, a riposo). */
	struct FLodCondition
	{
		double Center[3] = { 0, 0, 0 };
		double Near = 0.0, Far = 0.0;
	};

	struct FSwitchCondition
	{
		int32_t Switch = 0;
		int32_t Child = 0;
	};

	enum class EPartKind : uint8_t { Static, Dof };

	struct FRigPart
	{
		std::string Name;
		EPartKind Kind = EPartKind::Static;
		int32_t Parent = -1;
		/** Static: dal sistema della parte a quello della madre. */
		FAffine Rest;
		/** Dof: dal sistema della parte a quello della madre = Before * Moto(valori) * After. */
		FAffine Before, After;
		/** I limiti e i valori di partenza, nella semantica del file (gradi matematici, metri). */
		FDof Dof;
		std::vector<FRigSection> Sections;
		std::vector<FLodCondition> Lods;
		std::vector<FSwitchCondition> Switches;
		/** Il contenitore della geometria, nel sistema della parte. */
		float BoundsMin[3] = { 0, 0, 0 }, BoundsMax[3] = { 0, 0, 0 };
		bool bHasBounds = false;

		size_t TriangleCount() const
		{
			size_t Count = 0;
			for (const FRigSection& Section : Sections) { Count += Section.Indices.size() / 3; }
			return Count;
		}
	};

	struct FRigSwitch
	{
		std::string Name;
		bool bSequence = false;
		int32_t DefaultMask = 0;
		int32_t ChildCount = 0;
		int32_t WordsPerMask = 0;
		std::vector<uint32_t> Masks;
		/** Sequenza: quanto dura un giro (s). */
		float LoopDuration = 0.0f;

		int32_t MaskCount() const { return WordsPerMask > 0 ? static_cast<int32_t>(Masks.size()) / WordsPerMask : 0; }
		bool Shows(int32_t Mask, int32_t Child) const
		{
			if (Mask < 0 || Mask >= MaskCount() || Child < 0 || Child / 32 >= WordsPerMask) { return false; }
			return (Masks[static_cast<size_t>(Mask * WordsPerMask + Child / 32)] >> (Child % 32) & 1u) != 0;
		}
	};

	struct FRigLight
	{
		int32_t Part = 0;
		float P[3] = { 0, 0, 0 };
		uint8_t Rgba[4] = { 255, 255, 255, 255 };
		int32_t Directionality = 0;
	};

	/** Le correzioni della tabella dei modelli (EntityModels.h). */
	struct FRigOptions
	{
		/** Gli assi del file, se il formato non li dice giusti. */
		bool bOverrideAxes = false;
		FAxisConvention Axes;
		double Scale = 1.0;
		/** Gradi, positivo a destra (orario visto dall'alto, come la bussola). */
		double YawDeg = 0.0;
		/** Metri, negli assi dell'attore (avanti, destra, alto). */
		double Offset[3] = { 0, 0, 0 };
	};

	struct FModelRig
	{
		std::vector<FRigPart> Parts;
		std::vector<FRigSwitch> Switches;
		std::vector<FModelMaterial> Materials;
		std::vector<FModelTexture> Textures;
		std::vector<FRigLight> Lights;
		std::vector<std::string> Warnings;
		std::string Format;
		/** Il contenitore di tutto, nel sistema della radice, a riposo. */
		double BoundsMin[3] = { 0, 0, 0 }, BoundsMax[3] = { 0, 0, 0 };

		size_t TriangleCount() const
		{
			size_t Count = 0;
			for (const FRigPart& Part : Parts) { Count += Part.TriangleCount(); }
			return Count;
		}

		size_t DofCount() const
		{
			size_t Count = 0;
			for (const FRigPart& Part : Parts) { Count += Part.Kind == EPartKind::Dof ? 1 : 0; }
			return Count;
		}

		int32_t FindPart(const std::string& Name) const
		{
			for (size_t I = 0; I < Parts.size(); ++I) { if (Parts[I].Name == Name) { return static_cast<int32_t>(I); } }
			return -1;
		}

		int32_t FindSwitch(const std::string& Name) const
		{
			for (size_t I = 0; I < Switches.size(); ++I) { if (Switches[I].Name == Name) { return static_cast<int32_t>(I); } }
			return -1;
		}

		/** La lunghezza massima, metri. */
		double Extent() const
		{
			return std::max({ BoundsMax[0] - BoundsMin[0], BoundsMax[1] - BoundsMin[1], BoundsMax[2] - BoundsMin[2], 0.1 });
		}
	};

	/** La trasformazione di una parte nella madre, con questi valori del DOF (ignorati se la parte non e' un DOF). */
	inline FAffine PartTransform(const FRigPart& Part, const FDofValues& Values)
	{
		return Part.Kind == EPartKind::Dof ? Part.Before * DofMotion(Values) * Part.After : Part.Rest;
	}

	/** Lo stato di un modello in un'entita': i valori dei DOF (per parte) e la maschera di ogni switch. */
	struct FRigState
	{
		std::vector<FDofValues> Dofs;
		std::vector<int32_t> Masks;
	};

	inline FRigState DefaultRigState(const FModelRig& Rig)
	{
		FRigState State;
		for (const FRigPart& Part : Rig.Parts) { State.Dofs.push_back(FDofValues::Current(Part.Dof)); }
		for (const FRigSwitch& Switch : Rig.Switches) { State.Masks.push_back(Switch.DefaultMask); }
		return State;
	}

	/** Visibile con questo stato, con la camera qui (nel sistema della radice, metri). */
	inline bool IsPartVisible(const FModelRig& Rig, const FRigPart& Part, const FRigState& State, const double Camera[3])
	{
		for (const FSwitchCondition& Condition : Part.Switches)
		{
			const int32_t Mask = static_cast<size_t>(Condition.Switch) < State.Masks.size() ? State.Masks[static_cast<size_t>(Condition.Switch)] : 0;
			if (!Rig.Switches[static_cast<size_t>(Condition.Switch)].Shows(Mask, Condition.Child)) { return false; }
		}
		for (const FLodCondition& Lod : Part.Lods)
		{
			const double D = std::sqrt((Camera[0] - Lod.Center[0]) * (Camera[0] - Lod.Center[0]) + (Camera[1] - Lod.Center[1]) * (Camera[1] - Lod.Center[1])
				+ (Camera[2] - Lod.Center[2]) * (Camera[2] - Lod.Center[2]));
			if (D < Lod.Near || D >= Lod.Far) { return false; }
		}
		return true;
	}

	namespace RigDetail
	{
		inline void Grow(float Min[3], float Max[3], bool& bHas, const float P[3])
		{
			for (int I = 0; I < 3; ++I)
			{
				Min[I] = bHas ? std::min(Min[I], P[I]) : P[I];
				Max[I] = bHas ? std::max(Max[I], P[I]) : P[I];
			}
			bHas = true;
		}
	}

	/** Dal modello al rig. */
	inline FModelRig BuildRig(const FModel& Model, const FRigOptions& Options = FRigOptions{})
	{
		FModelRig Rig;
		Rig.Materials = Model.Materials;
		Rig.Textures = Model.Textures;
		Rig.Warnings = Model.Warnings;
		Rig.Format = Model.Format;

		// La radice: dal file all'attore. K e' la parte lineare (assi, scala,
		// imbardata): e' il sistema in cui vive la geometria delle parti mobili.
		const FAxisConvention Axes = Options.bOverrideAxes && Options.Axes.IsValid() ? Options.Axes : Model.Axes;
		const double Yaw = Options.YawDeg * 3.14159265358979323846 / 180.0;
		const FAffine K = FAffine::Rotation(2, Yaw) * FAffine::Scale(Options.Scale, Options.Scale, Options.Scale) * ToActorAxes(Axes);
		const FAffine G = FAffine::Translation(Options.Offset[0], Options.Offset[1], Options.Offset[2]) * K;
		const FAffine KInverse = K.Inverse();

		FRigPart Root;
		Root.Name = Model.Nodes.empty() ? std::string("radice") : Model.Nodes[0].Name;
		Root.Rest = FAffine::Identity();
		Rig.Parts.push_back(Root);

		// Acc: dalle coordinate del nodo (file) a quelle della parte corrente (attore).
		// ToRoot: dalle coordinate del nodo alla radice, a riposo (per i centri dei LOD).
		const std::function<void(int32_t, int32_t, const FAffine&, const FAffine&, const std::vector<FLodCondition>&,
			const std::vector<FSwitchCondition>&)> Visit =
			[&](int32_t NodeIndex, int32_t PartIndex, const FAffine& AccIn, const FAffine& ToRootIn,
				const std::vector<FLodCondition>& LodsIn, const std::vector<FSwitchCondition>& SwitchesIn)
		{
			const FModelNode& Node = Model.Nodes[static_cast<size_t>(NodeIndex)];
			FAffine Acc = Node.bHasMatrix ? AccIn * Node.Local : AccIn;
			FAffine ToRoot = Node.bHasMatrix ? ToRootIn * Node.Local : ToRootIn;
			int32_t Part = PartIndex;
			std::vector<FLodCondition> Lods = LodsIn;
			std::vector<FSwitchCondition> Switches = SwitchesIn;

			const auto NewPart = [&](EPartKind Kind, const std::string& Name)
			{
				FRigPart P;
				P.Kind = Kind;
				P.Name = Name;
				P.Parent = Part;
				P.Lods = Lods;
				P.Switches = Switches;
				Rig.Parts.push_back(std::move(P));
				return static_cast<int32_t>(Rig.Parts.size()) - 1;
			};

			if (Node.Kind == ENodeKind::Dof)
			{
				// Il sistema della parte nuova: K (assi dell'attore). Un vertice v del
				// file sta nella parte a K v; nella madre a Acc F Moto F^-1 v.
				const int32_t New = NewPart(EPartKind::Dof, Node.Name);
				FRigPart& P = Rig.Parts[static_cast<size_t>(New)];
				P.Dof = Node.Dof;
				P.Before = Acc * Node.Dof.Frame;
				P.After = Node.Dof.Frame.Inverse() * KInverse;
				Part = New;
				Acc = K;
				ToRoot = ToRoot * DofTransform(Node.Dof, FDofValues::Current(Node.Dof));
			}
			else if (Node.Kind == ENodeKind::Lod)
			{
				// Il centro nel sistema della radice (ToRoot parte da G: e' gia' negli assi dell'attore).
				FLodCondition Condition;
				ToRoot.ApplyPoint(Node.LodCenter, Condition.Center);
				// Le distanze scalano con la scala della tabella.
				const double Factor = Options.Scale;
				Condition.Near = std::min(Node.SwitchOut, Node.SwitchIn) * Factor;
				Condition.Far = std::max(Node.SwitchOut, Node.SwitchIn) * Factor;
				Lods.push_back(Condition);
				Part = NewPart(EPartKind::Static, Node.Name);
			}

			// La geometria del nodo, nella parte corrente.
			FRigPart& Target = Rig.Parts[static_cast<size_t>(Part)];
			const bool bFlip = Acc.Determinant() < 0.0;
			for (const int32_t MeshIndex : Node.Meshes)
			{
				const FModelMesh& Mesh = Model.Meshes[static_cast<size_t>(MeshIndex)];
				FRigSection* Section = nullptr;
				for (FRigSection& Existing : Target.Sections) { if (Existing.Material == Mesh.Material) { Section = &Existing; break; } }
				if (!Section) { Target.Sections.push_back(FRigSection{}); Section = &Target.Sections.back(); Section->Material = Mesh.Material; }
				const uint32_t Base = static_cast<uint32_t>(Section->Vertices.size());
				for (const FModelVertex& V : Mesh.Vertices)
				{
					FRigVertex Out;
					double P[3], N[3];
					Acc.ApplyPoint(V.P, P);
					const double Normal[3] = { V.N[0], V.N[1], V.N[2] };
					Acc.ApplyNormal(Normal, N);
					for (int I = 0; I < 3; ++I) { Out.P[I] = static_cast<float>(P[I]); Out.N[I] = static_cast<float>(N[I]); }
					Out.UV[0] = V.UV[0];
					Out.UV[1] = Model.bUvOriginBottom ? 1.0f - V.UV[1] : V.UV[1];
					for (int I = 0; I < 4; ++I) { Out.Rgba[I] = V.Rgba[I]; }
					RigDetail::Grow(Target.BoundsMin, Target.BoundsMax, Target.bHasBounds, Out.P);
					Section->Vertices.push_back(Out);
				}
				for (size_t I = 0; I + 2 < Mesh.Indices.size(); I += 3)
				{
					Section->Indices.push_back(Base + Mesh.Indices[I]);
					Section->Indices.push_back(Base + Mesh.Indices[bFlip ? I + 2 : I + 1]);
					Section->Indices.push_back(Base + Mesh.Indices[bFlip ? I + 1 : I + 2]);
				}
			}

			// I punti luce del nodo.
			for (const FModelLightPoint& Light : Model.LightPoints)
			{
				if (Light.Node != NodeIndex) { continue; }
				FRigLight Out;
				Out.Part = Part;
				double P[3];
				Acc.ApplyPoint(Light.P, P);
				for (int I = 0; I < 3; ++I) { Out.P[I] = static_cast<float>(P[I]); }
				for (int I = 0; I < 4; ++I) { Out.Rgba[I] = Light.Rgba[I]; }
				Out.Directionality = Light.Directionality;
				Rig.Lights.push_back(Out);
			}

			// I figli; quelli di uno switch o di una sequenza con la loro condizione.
			const bool bSelector = Node.Kind == ENodeKind::Switch || Node.Kind == ENodeKind::Sequence;
			int32_t SwitchIndex = -1;
			if (bSelector)
			{
				FRigSwitch S;
				S.Name = Node.Name;
				S.bSequence = Node.Kind == ENodeKind::Sequence;
				S.ChildCount = static_cast<int32_t>(Node.Children.size());
				if (S.bSequence)
				{
					// Una maschera per fotogramma: si vede il fotogramma 0 finche' M3 non le fa girare.
					S.WordsPerMask = std::max(1, (S.ChildCount + 31) / 32);
					S.Masks.assign(static_cast<size_t>(S.WordsPerMask) * static_cast<size_t>(std::max(1, S.ChildCount)), 0u);
					for (int32_t C = 0; C < S.ChildCount; ++C) { S.Masks[static_cast<size_t>(C * S.WordsPerMask + C / 32)] |= 1u << (C % 32); }
					S.LoopDuration = Node.LoopDuration;
				}
				else
				{
					S.WordsPerMask = Node.WordsPerMask;
					S.Masks = Node.Masks;
					S.DefaultMask = Node.CurrentMask;
				}
				Rig.Switches.push_back(std::move(S));
				SwitchIndex = static_cast<int32_t>(Rig.Switches.size()) - 1;
			}
			for (size_t C = 0; C < Node.Children.size(); ++C)
			{
				const int32_t Child = Node.Children[C];
				if (bSelector)
				{
					std::vector<FSwitchCondition> ChildSwitches = Switches;
					ChildSwitches.push_back(FSwitchCondition{ SwitchIndex, static_cast<int32_t>(C) });
					// Una parte statica (stesso sistema della madre) per ogni figlio.
					FRigPart P;
					P.Kind = EPartKind::Static;
					P.Name = Node.Name + "/" + Model.Nodes[static_cast<size_t>(Child)].Name;
					P.Parent = Part;
					P.Lods = Lods;
					P.Switches = ChildSwitches;
					Rig.Parts.push_back(std::move(P));
					Visit(Child, static_cast<int32_t>(Rig.Parts.size()) - 1, Acc, ToRoot, Lods, ChildSwitches);
				}
				else
				{
					Visit(Child, Part, Acc, ToRoot, Lods, Switches);
				}
			}
		};
		Visit(0, 0, G, G, {}, {});

		// Le parti vuote: si tolgono dal fondo, finche' ce ne sono.
		bool bChanged = true;
		while (bChanged)
		{
			bChanged = false;
			std::vector<int32_t> ChildCount(Rig.Parts.size(), 0);
			for (const FRigPart& P : Rig.Parts) { if (P.Parent >= 0) { ++ChildCount[static_cast<size_t>(P.Parent)]; } }
			for (size_t I = Rig.Parts.size(); I-- > 1;)
			{
				if (ChildCount[I] == 0 && Rig.Parts[I].Sections.empty())
				{
					Rig.Parts.erase(Rig.Parts.begin() + static_cast<std::ptrdiff_t>(I));
					for (FRigPart& P : Rig.Parts) { if (P.Parent > static_cast<int32_t>(I)) { --P.Parent; } }
					for (FRigLight& L : Rig.Lights) { if (L.Part > static_cast<int32_t>(I)) { --L.Part; } }
					bChanged = true;
					break;
				}
			}
		}

		// Il contenitore di tutto, a riposo, nella radice.
		bool bHas = false;
		float Min[3] = { 0, 0, 0 }, Max[3] = { 0, 0, 0 };
		std::vector<FAffine> ToRootOfPart(Rig.Parts.size());
		for (size_t I = 0; I < Rig.Parts.size(); ++I)
		{
			const FRigPart& P = Rig.Parts[I];
			const FAffine Local = PartTransform(P, FDofValues::Current(P.Dof));
			ToRootOfPart[I] = P.Parent >= 0 ? ToRootOfPart[static_cast<size_t>(P.Parent)] * Local : FAffine::Identity();
			if (!P.bHasBounds) { continue; }
			for (int Corner = 0; Corner < 8; ++Corner)
			{
				const double C[3] = { (Corner & 1) ? P.BoundsMax[0] : P.BoundsMin[0], (Corner & 2) ? P.BoundsMax[1] : P.BoundsMin[1],
					(Corner & 4) ? P.BoundsMax[2] : P.BoundsMin[2] };
				double R[3];
				ToRootOfPart[I].ApplyPoint(C, R);
				const float F[3] = { static_cast<float>(R[0]), static_cast<float>(R[1]), static_cast<float>(R[2]) };
				RigDetail::Grow(Min, Max, bHas, F);
			}
		}
		for (int I = 0; I < 3; ++I) { Rig.BoundsMin[I] = Min[I]; Rig.BoundsMax[I] = Max[I]; }
		return Rig;
	}
}

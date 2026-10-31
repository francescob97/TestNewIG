// =============================================================================
//  TreeMesh.h -- Le forme degli alberi, fatte dal codice. STRATO: C++ PURO,
//  header-only.
//
//  Nessun modello da scaricare: ogni specie e' costruita con pochi solidi.
//
//    conifera         un tronco e sei coni sovrapposti, sempre piu' stretti
//    latifoglia       un tronco e una chioma di quattro "bolle" bitorzolute
//    pino domestico   un tronco alto e nudo, e una chioma piatta a ombrello
//    arbusto          tre bolle schiacciate a terra
//
//  Ogni forma ha due DETTAGLI: "vicino" (100-350 triangoli) e "lontano"
//  (20-30: un cono, una bolla), e due VARIANTI (bitorzoli e coni diversi).
//  Un terzo dettaglio, "pieno" (Full, 500-3.500 triangoli), e' per i modelli
//  Nanite che geo.Vegetation.CreateModels salva nell'editor.
//  Sono tutte alte 1 e larghe 1 (x e y fra -0,5 e 0,5, z fra 0 e 1): ogni
//  istanza le scala alla sua altezza e larghezza (TreePlacement.h).
//
//  Assi NEU come il terreno (x nord, y est, z alto), metri. I triangoli girano
//  come quelli del terreno (TileMesh.h): la normale geometrica
//  (B - A) x (C - A) punta FUORI, verso chi guarda. I test lo verificano.
//
//  IL COLORE DEI VERTICI dice al materiale cosa e' cosa:
//    R  255 = chioma, 0 = tronco
//    G  quanto arriva la luce del cielo: piu' scuro sotto e dentro la chioma
//    B  l'altezza nell'albero, 0..255: quanto si piega con il vento
//    A  255
// =============================================================================
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

#include "Vegetation/TreePlacement.h"

namespace GeoWorld::Vegetation
{
	/** Vicino e Lontano: i due LOD delle forme fatte a runtime. Full: i modelli Nanite. */
	enum class ETreeDetail : uint8_t { Near = 0, Far = 1, Full = 2 };

	struct FTreeMesh
	{
		std::vector<float> Positions;   // 3 per vertice
		std::vector<float> Normals;     // 3 per vertice
		std::vector<uint8_t> Colours;   // 4 per vertice (RGBA)
		std::vector<uint32_t> Indices;  // 3 per triangolo

		size_t VertexCount() const { return Positions.size() / 3; }
		size_t TriangleCount() const { return Indices.size() / 3; }
	};

	namespace TreeMeshDetail
	{
		struct FVec
		{
			double X = 0.0, Y = 0.0, Z = 0.0;
			FVec operator+(const FVec& O) const { return { X + O.X, Y + O.Y, Z + O.Z }; }
			FVec operator-(const FVec& O) const { return { X - O.X, Y - O.Y, Z - O.Z }; }
			FVec operator*(double S) const { return { X * S, Y * S, Z * S }; }
			double Dot(const FVec& O) const { return X * O.X + Y * O.Y + Z * O.Z; }
			FVec Cross(const FVec& O) const { return { Y * O.Z - Z * O.Y, Z * O.X - X * O.Z, X * O.Y - Y * O.X }; }
			double Length() const { return std::sqrt(Dot(*this)); }
			FVec Normalized() const
			{
				const double L = Length();
				return L > 1.0e-12 ? FVec{ X / L, Y / L, Z / L } : FVec{ 0.0, 0.0, 1.0 };
			}
		};

		/** Un numero 0..1 da un seme e un indice (per i bitorzoli e i coni). */
		inline double Random(uint32_t Seed, uint32_t Index)
		{
			return PlacementDetail::Unit(PlacementDetail::Mix(static_cast<uint64_t>(Seed) * 0x9E3779B97F4A7C15ull + Index), 0);
		}

		/** L'icosaedro, suddiviso Levels volte, sulla sfera unitaria (0: 12 vertici e 20 triangoli; 1: 42 e 80). */
		inline void UnitSphere(int32_t Levels, std::vector<FVec>& Vertices, std::vector<std::array<uint32_t, 3>>& Faces)
		{
			const double T = (1.0 + std::sqrt(5.0)) * 0.5;
			Vertices = { { -1, T, 0 }, { 1, T, 0 }, { -1, -T, 0 }, { 1, -T, 0 },
			             { 0, -1, T }, { 0, 1, T }, { 0, -1, -T }, { 0, 1, -T },
			             { T, 0, -1 }, { T, 0, 1 }, { -T, 0, -1 }, { -T, 0, 1 } };
			for (FVec& Vertex : Vertices) { Vertex = Vertex.Normalized(); }
			Faces = { { 0, 11, 5 }, { 0, 5, 1 }, { 0, 1, 7 }, { 0, 7, 10 }, { 0, 10, 11 },
			          { 1, 5, 9 }, { 5, 11, 4 }, { 11, 10, 2 }, { 10, 7, 6 }, { 7, 1, 8 },
			          { 3, 9, 4 }, { 3, 4, 2 }, { 3, 2, 6 }, { 3, 6, 8 }, { 3, 8, 9 },
			          { 4, 9, 5 }, { 2, 4, 11 }, { 6, 2, 10 }, { 8, 6, 7 }, { 9, 8, 1 } };
			for (int32_t Level = 0; Level < Levels; ++Level)
			{
				std::vector<std::array<uint32_t, 3>> Split;
				Split.reserve(Faces.size() * 4);
				std::vector<std::array<uint32_t, 3>> Midpoints;   // (a, b, indice), cercati linearmente: sono pochi
				auto Middle = [&Vertices, &Midpoints](uint32_t A, uint32_t B)
				{
					const uint32_t Low = std::min(A, B), High = std::max(A, B);
					for (const std::array<uint32_t, 3>& Known : Midpoints)
					{
						if (Known[0] == Low && Known[1] == High) { return Known[2]; }
					}
					const uint32_t Index = static_cast<uint32_t>(Vertices.size());
					Vertices.push_back(((Vertices[A] + Vertices[B]) * 0.5).Normalized());
					Midpoints.push_back({ Low, High, Index });
					return Index;
				};
				for (const std::array<uint32_t, 3>& Face : Faces)
				{
					const uint32_t AB = Middle(Face[0], Face[1]), BC = Middle(Face[1], Face[2]), CA = Middle(Face[2], Face[0]);
					Split.push_back({ Face[0], AB, CA });
					Split.push_back({ Face[1], BC, AB });
					Split.push_back({ Face[2], CA, BC });
					Split.push_back({ AB, BC, CA });
				}
				Faces.swap(Split);
			}
		}

		class FBuilder
		{
		public:
			explicit FBuilder(FTreeMesh& InOut) : Out(InOut) {}

			uint32_t Vertex(const FVec& Position, const FVec& Normal, uint8_t Foliage, double Light)
			{
				const FVec N = Normal.Normalized();
				const FVec P{ std::min(0.5, std::max(-0.5, Position.X)), std::min(0.5, std::max(-0.5, Position.Y)),
				              std::min(1.0, std::max(0.0, Position.Z)) };
				for (const double Value : { P.X, P.Y, P.Z }) { Out.Positions.push_back(static_cast<float>(Value)); }
				for (const double Value : { N.X, N.Y, N.Z }) { Out.Normals.push_back(static_cast<float>(Value)); }
				Out.Colours.push_back(Foliage);
				Out.Colours.push_back(static_cast<uint8_t>(std::lround(std::min(1.0, std::max(0.0, Light)) * 255.0)));
				Out.Colours.push_back(static_cast<uint8_t>(std::lround(P.Z * 255.0)));
				Out.Colours.push_back(255);
				return static_cast<uint32_t>(Out.Positions.size() / 3 - 1);
			}

			/** Un triangolo girato in modo che la sua normale geometrica stia dalla parte di Outward. */
			void Triangle(uint32_t A, uint32_t B, uint32_t C, const FVec& Outward)
			{
				const FVec Face = (At(B) - At(A)).Cross(At(C) - At(A));
				if (Face.Dot(Outward) < 0.0) { std::swap(B, C); }
				Out.Indices.push_back(A);
				Out.Indices.push_back(B);
				Out.Indices.push_back(C);
			}

			FVec At(uint32_t Index) const
			{
				return { Out.Positions[Index * 3 + 0], Out.Positions[Index * 3 + 1], Out.Positions[Index * 3 + 2] };
			}

		private:
			FTreeMesh& Out;
		};

		/** Un tronco: un prisma di Sides lati dal suolo a Top, senza tappi (stanno nel suolo e nella chioma). */
		inline void Trunk(FBuilder& Mesh, int32_t Sides, double Radius, double Top)
		{
			std::vector<uint32_t> Bottom, Upper;
			for (int32_t Side = 0; Side < Sides; ++Side)
			{
				const double Angle = 2.0 * 3.14159265358979323846 * Side / Sides;
				const FVec Radial{ std::cos(Angle), std::sin(Angle), 0.0 };
				// Un po' piu' stretto in alto, e piu' scuro alla base.
				Bottom.push_back(Mesh.Vertex(Radial * Radius, Radial, 0, 0.55));
				Upper.push_back(Mesh.Vertex(Radial * (Radius * 0.7) + FVec{ 0.0, 0.0, Top }, Radial, 0, 0.8));
			}
			for (int32_t Side = 0; Side < Sides; ++Side)
			{
				const int32_t Next = (Side + 1) % Sides;
				const FVec Middle = (Mesh.At(Bottom[Side]) + Mesh.At(Bottom[Next])) * 0.5;
				const FVec Outward{ Middle.X, Middle.Y, 0.0 };
				Mesh.Triangle(Bottom[Side], Bottom[Next], Upper[Next], Outward);
				Mesh.Triangle(Bottom[Side], Upper[Next], Upper[Side], Outward);
			}
		}

		/**
		 * Una bolla di chioma: una sfera (suddivisa Levels volte) stirata a
		 * ellissoide e bitorzoluta (Bumps: quanto, in frazione del raggio).
		 * La normale e' quella dell'ellissoide liscio: la chioma resta morbida
		 * alla luce anche con i bitorzoli.
		 */
		inline void Blob(FBuilder& Mesh, int32_t Levels, const FVec& Centre, const FVec& Radii, double Bumps,
		                 uint32_t Seed, double CrownBottom, double CrownTop)
		{
			std::vector<FVec> Sphere;
			std::vector<std::array<uint32_t, 3>> Faces;
			UnitSphere(Levels, Sphere, Faces);
			std::vector<uint32_t> Indices;
			Indices.reserve(Sphere.size());
			for (uint32_t Index = 0; Index < Sphere.size(); ++Index)
			{
				const FVec& Unit = Sphere[Index];
				const double Bump = 1.0 + Bumps * (2.0 * Random(Seed, Index) - 1.0);
				const FVec Position = Centre + FVec{ Unit.X * Radii.X, Unit.Y * Radii.Y, Unit.Z * Radii.Z } * Bump;
				const FVec Normal{ Unit.X / Radii.X, Unit.Y / Radii.Y, Unit.Z / Radii.Z };
				// La luce del cielo: poca sotto la chioma e verso il suo interno, piena in cima.
				const double Height = (Position.Z - CrownBottom) / std::max(0.05, CrownTop - CrownBottom);
				const double Light = 0.35 + 0.45 * std::min(1.0, std::max(0.0, Height)) + 0.2 * std::max(0.0, Unit.Z);
				Indices.push_back(Mesh.Vertex(Position, Normal, 255, Light));
			}
			for (const std::array<uint32_t, 3>& Face : Faces)
			{
				const FVec Middle = (Mesh.At(Indices[Face[0]]) + Mesh.At(Indices[Face[1]]) + Mesh.At(Indices[Face[2]])) * (1.0 / 3.0);
				Mesh.Triangle(Indices[Face[0]], Indices[Face[1]], Indices[Face[2]], Middle - Centre);
			}
		}

		/**
		 * Un palco di conifera: un cono dalla base (raggio Radius, quota Base) alla
		 * punta (quota Apex), con il bordo frastagliato, e il fondo chiuso (da
		 * sotto non si vede il vuoto).
		 */
		inline void Cone(FBuilder& Mesh, int32_t Sides, double Radius, double Base, double Apex, uint32_t Seed, double Light)
		{
			const double Slant = std::atan2(Radius, Apex - Base);
			std::vector<uint32_t> Ring, Floor;
			const double Turn = Random(Seed, 999) * 2.0 * 3.14159265358979323846;
			for (int32_t Side = 0; Side < Sides; ++Side)
			{
				const double Angle = Turn + 2.0 * 3.14159265358979323846 * Side / Sides;
				const FVec Radial{ std::cos(Angle), std::sin(Angle), 0.0 };
				const double Reach = Radius * (0.88 + 0.24 * Random(Seed, static_cast<uint32_t>(Side)));
				// I rami pendono un po': il bordo va su e giu'.
				const double Droop = (Side % 2 == 0) ? -0.012 : 0.008;
				const FVec Edge = Radial * Reach + FVec{ 0.0, 0.0, Base + Droop };
				const FVec Normal = Radial * std::cos(Slant) + FVec{ 0.0, 0.0, std::sin(Slant) };
				Ring.push_back(Mesh.Vertex(Edge, Normal, 255, Light));
				Floor.push_back(Mesh.Vertex(Edge, FVec{ 0.0, 0.0, -1.0 }, 255, Light * 0.6));
			}
			const uint32_t Tip = Mesh.Vertex(FVec{ 0.0, 0.0, Apex }, FVec{ 0.0, 0.0, 1.0 }, 255, std::min(1.0, Light + 0.15));
			const uint32_t Middle = Mesh.Vertex(FVec{ 0.0, 0.0, Base + 0.01 }, FVec{ 0.0, 0.0, -1.0 }, 255, Light * 0.5);
			for (int32_t Side = 0; Side < Sides; ++Side)
			{
				const int32_t Next = (Side + 1) % Sides;
				const FVec Edge = (Mesh.At(Ring[Side]) + Mesh.At(Ring[Next])) * 0.5;
				Mesh.Triangle(Tip, Ring[Side], Ring[Next], FVec{ Edge.X, Edge.Y, 0.0 } + FVec{ 0.0, 0.0, Radius * 0.3 });
				Mesh.Triangle(Middle, Floor[Side], Floor[Next], FVec{ 0.0, 0.0, -1.0 });
			}
		}
	}

	/**
	 * La forma di una specie, a un dettaglio, in una variante (0..VariantCount-1).
	 *
	 * Full e' per i modelli NANITE (geo.Vegetation.CreateModels): Nanite
	 * sceglie da solo quanti triangoli disegnare, quindi la forma puo' essere
	 * fitta (1.000-3.500 triangoli) senza costare da lontano. La chioma e'
	 * fatta di piu' ciuffi, piu' lisci; le conifere di piu' palchi.
	 */
	inline void BuildTreeMesh(ESpecies Species, ETreeDetail Detail, uint32_t Variant, FTreeMesh& Out)
	{
		using namespace TreeMeshDetail;
		Out = FTreeMesh();
		FBuilder Mesh(Out);
		const bool bFull = Detail == ETreeDetail::Full;
		const bool bNear = Detail != ETreeDetail::Far;
		const uint32_t Seed = 7919u * (static_cast<uint32_t>(Species) + 1u) + 104729u * Variant + (bNear ? 0u : 15485863u);
		const int32_t Levels = bFull ? 2 : (bNear ? 1 : 0);
		// Piu' la sfera e' suddivisa, piu' i bitorzoli a caso per vertice
		// sembrano spine: con Full si abbassano.
		const double Bumps = bFull ? 0.07 : (bNear ? 0.12 : 0.06);
		const int32_t TrunkSides = bFull ? 10 : 6;
		const double Turn = Random(Seed, 500) * 2.0 * 3.14159265358979323846;
		const double Pi = 3.14159265358979323846;

		switch (Species)
		{
		case ESpecies::Conifer:
		{
			if (bNear)
			{
				const int32_t Tiers = bFull ? 12 : 6;
				const double Step = 0.81 / Tiers;
				Trunk(Mesh, TrunkSides, 0.022, 0.3);
				for (int32_t Tier = 0; Tier < Tiers; ++Tier)
				{
					const double Base = 0.15 + Tier * Step;
					const double Apex = std::min(1.0, Base + (bFull ? 0.22 : 0.30));
					const double Radius = 0.5 * std::pow(1.0 - Tier / (Tiers + 0.5), 1.05);
					Cone(Mesh, bFull ? 20 : 9, Radius / 1.12, Base, Apex, Seed + static_cast<uint32_t>(Tier) * 31u,
						0.5 + 0.48 * Tier / Tiers);
				}
			}
			else
			{
				Cone(Mesh, 6, 0.5 / 1.12, 0.12, 0.78, Seed, 0.6);
				Cone(Mesh, 6, 0.3 / 1.12, 0.5, 1.0, Seed + 1u, 0.85);
			}
			break;
		}
		case ESpecies::UmbrellaPine:
		{
			const FVec Squash{ 0.30, 0.30, 0.11 };
			if (bNear)
			{
				Trunk(Mesh, TrunkSides, 0.03, 0.84);
				Blob(Mesh, Levels, FVec{ 0.0, 0.0, 0.84 }, Squash, Bumps, Seed, 0.72, 0.98);
				const int32_t Around = bFull ? 5 : 2;
				for (int32_t Side = 0; Side < Around; ++Side)
				{
					const double Angle = Turn + Side * 2.0 * Pi / Around;
					const double Reach = bFull ? 0.19 : 0.16;
					const FVec Centre{ Reach * std::cos(Angle), Reach * std::sin(Angle), 0.83 + 0.02 * (Side % 2) };
					Blob(Mesh, Levels, Centre, Squash * (bFull ? 0.8 : 0.95), Bumps, Seed + 1u + static_cast<uint32_t>(Side), 0.72, 0.98);
				}
			}
			else
			{
				Trunk(Mesh, 4, 0.035, 0.84);
				Blob(Mesh, Levels, FVec{ 0.0, 0.0, 0.85 }, FVec{ 0.46, 0.46, 0.12 }, Bumps, Seed, 0.72, 0.98);
			}
			break;
		}
		case ESpecies::Shrub:
		{
			if (bNear)
			{
				Blob(Mesh, Levels, FVec{ 0.0, 0.0, 0.42 }, FVec{ 0.32, 0.32, 0.38 }, Bumps, Seed, 0.0, 0.85);
				const int32_t Around = bFull ? 4 : 2;
				for (int32_t Side = 0; Side < Around; ++Side)
				{
					const double Angle = Turn + Side * (bFull ? 2.0 * Pi / Around : 2.4);
					const FVec Centre{ 0.15 * std::cos(Angle), 0.15 * std::sin(Angle), 0.32 };
					Blob(Mesh, Levels, Centre, FVec{ 0.26, 0.26, 0.28 } * (bFull ? 0.9 : 1.0), Bumps,
						Seed + 1u + static_cast<uint32_t>(Side), 0.0, 0.85);
				}
			}
			else
			{
				Blob(Mesh, Levels, FVec{ 0.0, 0.0, 0.45 }, FVec{ 0.46, 0.46, 0.42 }, Bumps, Seed, 0.0, 0.9);
			}
			break;
		}
		default:   // latifoglie
		{
			if (bNear)
			{
				Trunk(Mesh, TrunkSides, 0.04, 0.5);
				Blob(Mesh, Levels, FVec{ 0.0, 0.0, 0.62 }, FVec{ 0.30, 0.30, 0.30 }, Bumps, Seed, 0.32, 0.96);
				// Vicino: tre ciuffi attorno. Full: due giri, sei in basso e tre
				// in alto, piu' piccoli: la chioma a cavolfiore delle querce.
				const int32_t Lower = bFull ? 6 : 3;
				const int32_t Upper = bFull ? 3 : 0;
				for (int32_t Side = 0; Side < Lower + Upper; ++Side)
				{
					const bool bUpper = Side >= Lower;
					const int32_t Count = bUpper ? Upper : Lower;
					const int32_t Index = bUpper ? Side - Lower : Side;
					const double Angle = Turn + (bUpper ? Pi / Count : 0.0) + Index * 2.0 * Pi / Count
						+ 0.5 * Random(Seed, 600u + static_cast<uint32_t>(Side));
					const double Reach = bUpper ? 0.11 : (bFull ? 0.20 : 0.17);
					const double Height = bUpper ? 0.78 + 0.06 * Random(Seed, 800u + static_cast<uint32_t>(Side))
						: 0.55 + 0.13 * Random(Seed, 700u + static_cast<uint32_t>(Side));
					const FVec Radii = bFull ? (bUpper ? FVec{ 0.16, 0.16, 0.15 } : FVec{ 0.19, 0.19, 0.17 }) : FVec{ 0.22, 0.22, 0.20 };
					const FVec Centre{ Reach * std::cos(Angle), Reach * std::sin(Angle), Height };
					Blob(Mesh, Levels, Centre, Radii, Bumps, Seed + 1u + static_cast<uint32_t>(Side), 0.32, 0.96);
				}
			}
			else
			{
				Blob(Mesh, Levels, FVec{ 0.0, 0.0, 0.6 }, FVec{ 0.45, 0.45, 0.36 }, Bumps, Seed, 0.25, 0.96);
			}
			break;
		}
		}
	}
}

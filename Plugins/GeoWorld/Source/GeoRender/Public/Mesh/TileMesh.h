// =============================================================================
//  TileMesh.h -- Generazione della mesh di una tile. STRATO: C++ PURO.
//
//  Header-only, come tutto lo strato puro: in Unreal ogni modulo e' una DLL e
//  un simbolo definito in un .cpp non sarebbe visibile agli altri.
// =============================================================================
#pragma once

#include <cmath>
#include <cstdint>
#include <vector>

#include "Geo/Ellipsoid.h"
#include "Tiles/TileFormat.h"
#include "Tiles/TilingScheme.h"

namespace GeoWorld::Mesh
{
	using Core::FEcef;
	using Core::FEllipsoid;
	using Core::FGeodetic;
	using Core::FMat3;
	using Tiles::FHeightTile;
	using Tiles::FTileKey;

	/**
	 * =========================================================================
	 *  IN CHE SISTEMA STANNO I VERTICI, E PERCHE' NON E' ENU
	 * =========================================================================
	 *  I vertici sono in un frame LOCALE alla tile, in METRI, con assi
	 *
	 *      X = Nord locale,  Y = Est locale,  Z = Alto locale
	 *
	 *  cioe' NEU e non ENU. La scelta non e' arbitraria ed e' la stessa trappola
	 *  della Fase 1: ENU e' DESTRORSO, Unreal e' SINISTRORSO. Una mesh costruita
	 *  in ENU richiederebbe una trasformazione del componente con determinante
	 *  -1, cioe' una riflessione: FMatrix::ToQuat() su una matrice del genere non
	 *  fallisce, restituisce silenziosamente spazzatura, e il terreno comparirebbe
	 *  specchiato.
	 *
	 *  In NEU invece la trasformazione del componente e' esattamente quella che
	 *  FGeoreferenceSnapshot::GetLocalNeuTransform() gia' produce, ed e' una
	 *  rotazione propria (verificato dal test 9 della Fase 1).
	 *
	 * =========================================================================
	 *  PERCHE' UN FRAME LOCALE ALLA TILE E NON LO SPAZIO MONDO
	 * =========================================================================
	 *  E' la ragione per cui il rebasing della Fase 1 e' quasi gratuito. I
	 *  vertici sono float: in coordinate mondo, a distanza del raggio terrestre,
	 *  un float ha un ULP di 64 cm e la mesh si spappolerebbe. Relativi al
	 *  centro della propria tile restano di pochi chilometri, dove il float e'
	 *  millimetrico.
	 *
	 *  Conseguenza operativa: un rebase NON rigenera nessun vertice. Cambia solo
	 *  la trasformazione del componente.
	 *
	 *  I valori restano in METRI. La conversione in unita' Unreal avviene al
	 *  confine, nel provider, usando GeoWorld::Units::MetersToUu: la regola del
	 *  punto unico di conversione vale anche qui.
	 */
	struct FTileMeshData
	{
		/** Origine del frame locale: centro della tile a quota di riferimento. */
		FGeodetic Origin;

		std::vector<float> Positions;      // 3 per vertice, metri, frame NEU locale
		std::vector<float> Normals;        // 3 per vertice, normalizzate
		std::vector<float> UVs;            // 2 per vertice, [0,1] sulla tile
		std::vector<uint32_t> Indices;     // 3 per triangolo

		uint32_t InteriorVertexCount = 0;
		uint32_t SkirtVertexCount = 0;
		uint32_t TriangleCount = 0;

		float MinHeight = 0.0f;
		float MaxHeight = 0.0f;
		/** Raggio della sfera che contiene la mesh, attorno all'origine locale. */
		double BoundingRadiusMetres = 0.0;

		bool IsValid() const
		{
			return TriangleCount > 0 && Positions.size() == Normals.size()
			    && Positions.size() / 3 == UVs.size() / 2;
		}

		size_t GetByteSize() const
		{
			return Positions.size() * sizeof(float) + Normals.size() * sizeof(float)
			     + UVs.size() * sizeof(float) + Indices.size() * sizeof(uint32_t);
		}
	};

	struct FTileMeshParameters
	{
		/** Genera le gonne verticali ai bordi. */
		bool bGenerateSkirt = true;

		/**
		 * Profondita' della gonna in metri. Se <= 0 viene calcolata da
		 * ComputeSkirtDepth().
		 */
		double SkirtDepthMetres = 0.0;

		/**
		 * Inverte l'ordine dei vertici dei triangoli.
		 *
		 * Serve perche' la convenzione di faccia frontale di Unreal e' una di
		 * quelle cose che si verificano guardando lo schermo, non ragionando.
		 * Se il terreno risulta invisibile dall'alto e visibile da sotto, e'
		 * questo il parametro: geo.Terrain.FlipWinding lo cambia a caldo.
		 */
		bool bFlipWinding = true;
	};

	/**
	 * Profondita' della gonna, in metri.
	 *
	 * DA DOVE VIENE IL NUMERO. La crepa fra due tile di livello diverso nasce
	 * perche' il bordo della tile grossolana e' un segmento fra due suoi post,
	 * mentre la tile fine ha post intermedi che seguono il terreno. Lo scarto
	 * massimo fra i due e' il dislivello del terreno su mezzo intervallo del
	 * livello grossolano, cioe' pendenza x spacing.
	 *
	 * Con una pendenza fino a 4 (76 gradi, piu' di qualunque versante reale) e
	 * l'intervallo del livello grossolano pari al doppio del nostro, si ottiene
	 * 8 x spacing. E' il valore di default: abbondante quanto basta perche' la
	 * gonna non si veda mai spuntare, e non tanto da produrre pareti visibili.
	 */
	inline double ComputeSkirtDepth(uint32_t Level)
	{
		constexpr double MetresPerDegreeLat = 111132.0;
		constexpr double SlopeSafetyFactor = 8.0;
		return Tiles::PostSpacingDeg(Level) * MetresPerDegreeLat * SlopeSafetyFactor;
	}

	namespace Detail
	{
		/** Base NEU (righe Nord, Est, Alto) espressa in ECEF, per un punto. */
		inline FMat3 MakeLocalNeu(const FGeodetic& At)
		{
			return Core::MakeNeuBasis(At.LatRad, At.LonRad);
		}

		inline void SetVector3(std::vector<float>& Target, size_t Index,
		                       double A, double B, double C)
		{
			Target[Index * 3 + 0] = static_cast<float>(A);
			Target[Index * 3 + 1] = static_cast<float>(B);
			Target[Index * 3 + 2] = static_cast<float>(C);
		}
	}

	/**
	 * Costruisce la mesh di una tile.
	 *
	 * La griglia e' 129x129 post; i triangoli sono 128x128x2. La gonna aggiunge
	 * quattro strisce lungo il perimetro.
	 */
	inline void BuildTileMesh(const FHeightTile& Tile, const FTileMeshParameters& Parameters,
	                          FTileMeshData& Out, const FEllipsoid& Ellipsoid = Core::WGS84)
	{
		Out = FTileMeshData();

		if (!Tile.IsValid()) { return; }

		constexpr int32_t Posts = Tiles::TilePosts;
		constexpr int32_t Cells = Tiles::TileCells;

		const Tiles::FTileBounds Bounds =
			Tiles::GetTileBounds(Tile.Key.Level, Tile.Key.X, Tile.Key.Y);

		// Origine del frame locale: centro della tile, a meta' fra le quote
		// estreme. Centrare anche in quota tiene i valori piu' piccoli su una
		// tile molto acclive.
		const double ReferenceHeight = 0.5 * (Tile.MinHeight + Tile.MaxHeight);
		Out.Origin = FGeodetic::FromDegrees(Bounds.CentreLat(), Bounds.CentreLon(), ReferenceHeight);
		Out.MinHeight = Tile.MinHeight;
		Out.MaxHeight = Tile.MaxHeight;

		const FEcef OriginEcef = Core::GeodeticToEcef(Out.Origin, Ellipsoid);
		const FMat3 EcefToLocal = Detail::MakeLocalNeu(Out.Origin);

		const double Spacing = Tiles::PostSpacingDeg(Tile.Key.Level);

		const uint32_t InteriorVertices = static_cast<uint32_t>(Posts) * Posts;
		const uint32_t SkirtVertices = Parameters.bGenerateSkirt
			? static_cast<uint32_t>(4 * Posts) : 0u;
		const uint32_t TotalVertices = InteriorVertices + SkirtVertices;

		Out.Positions.assign(TotalVertices * 3, 0.0f);
		Out.Normals.assign(TotalVertices * 3, 0.0f);
		Out.UVs.assign(TotalVertices * 2, 0.0f);
		Out.InteriorVertexCount = InteriorVertices;
		Out.SkirtVertexCount = SkirtVertices;

		// --- Posizioni dei post, in coordinate locali ------------------------
		//
		// Ogni post passa per geodetiche -> ECEF -> frame locale. E' la parte
		// cara: 16641 conversioni per tile. Sta qui, nello strato puro, proprio
		// perche' possa essere eseguita fuori dal game thread.
		std::vector<double> LocalX(InteriorVertices), LocalY(InteriorVertices), LocalZ(InteriorVertices);
		double MaxRadiusSquared = 0.0;

		for (int32_t J = 0; J < Posts; ++J)
		{
			const double Lat = Bounds.North - static_cast<double>(J) * Spacing;
			for (int32_t I = 0; I < Posts; ++I)
			{
				const double Lon = Bounds.West + static_cast<double>(I) * Spacing;
				const size_t Index = static_cast<size_t>(J) * Posts + I;

				const FEcef Position = Core::GeodeticToEcef(
					FGeodetic::FromRadians(Lat * Core::DegToRad, Lon * Core::DegToRad,
					                       Tile.GetHeight(I, J)), Ellipsoid);

				const FEcef Local = EcefToLocal.Transform(Position - OriginEcef);
				LocalX[Index] = Local.X;   // Nord
				LocalY[Index] = Local.Y;   // Est
				LocalZ[Index] = Local.Z;   // Alto

				MaxRadiusSquared = std::max(MaxRadiusSquared, Local.LengthSquared());

				Detail::SetVector3(Out.Positions, Index, Local.X, Local.Y, Local.Z);

				Out.UVs[Index * 2 + 0] = static_cast<float>(I) / static_cast<float>(Cells);
				Out.UVs[Index * 2 + 1] = static_cast<float>(J) / static_cast<float>(Cells);
			}
		}
		Out.BoundingRadiusMetres = std::sqrt(MaxRadiusSquared);

		// --- Normali ---------------------------------------------------------
		//
		// Differenze centrali sulle POSIZIONI e non sulle quote: il passo fra
		// post in metri non e' costante (varia con la latitudine e con la
		// curvatura), e derivare dalle sole quote darebbe normali sbagliate
		// quanto quella variazione.
		//
		// LIMITE NOTO: sui post di BORDO la differenza e' a un lato solo, perche'
		// i vicini esterni non stanno nella tile. Due tile adiacenti calcolano
		// quindi normali leggermente diverse sullo stesso post condiviso, e la
		// giunzione puo' mostrare una riga di illuminazione. La geometria resta
		// perfettamente continua: e' solo l'illuminazione. Si risolverebbe con
		// un post di bordo in piu' nel formato — per questo l'header delle tile
		// ha un numero di versione.
		for (int32_t J = 0; J < Posts; ++J)
		{
			for (int32_t I = 0; I < Posts; ++I)
			{
				const size_t Index = static_cast<size_t>(J) * Posts + I;

				const int32_t IMinus = (I > 0) ? I - 1 : I;
				const int32_t IPlus = (I < Cells) ? I + 1 : I;
				const int32_t JMinus = (J > 0) ? J - 1 : J;
				const int32_t JPlus = (J < Cells) ? J + 1 : J;

				const size_t East = static_cast<size_t>(J) * Posts + IPlus;
				const size_t West = static_cast<size_t>(J) * Posts + IMinus;
				const size_t South = static_cast<size_t>(JPlus) * Posts + I;
				const size_t North = static_cast<size_t>(JMinus) * Posts + I;

				// Tangenti lungo est e lungo nord.
				const double EastX = LocalX[East] - LocalX[West];
				const double EastY = LocalY[East] - LocalY[West];
				const double EastZ = LocalZ[East] - LocalZ[West];

				const double NorthX = LocalX[North] - LocalX[South];
				const double NorthY = LocalY[North] - LocalY[South];
				const double NorthZ = LocalZ[North] - LocalZ[South];

				// Nord x Est: con gli assi NEU (X=Nord, Y=Est, Z=Alto) questo
				// prodotto punta verso l'alto sul terreno piano.
				double Nx = NorthY * EastZ - NorthZ * EastY;
				double Ny = NorthZ * EastX - NorthX * EastZ;
				double Nz = NorthX * EastY - NorthY * EastX;

				const double Length = std::sqrt(Nx * Nx + Ny * Ny + Nz * Nz);
				if (Length > 1e-12) { Nx /= Length; Ny /= Length; Nz /= Length; }
				else { Nx = 0.0; Ny = 0.0; Nz = 1.0; }

				Detail::SetVector3(Out.Normals, Index, Nx, Ny, Nz);
			}
		}

		// --- Triangoli dell'interno -------------------------------------------
		const uint32_t InteriorTriangles = static_cast<uint32_t>(Cells) * Cells * 2;
		const uint32_t SkirtTriangles = Parameters.bGenerateSkirt
			? static_cast<uint32_t>(4 * Cells * 2) : 0u;
		Out.Indices.reserve((InteriorTriangles + SkirtTriangles) * 3);

		auto AddTriangle = [&](uint32_t A, uint32_t B, uint32_t C)
		{
			if (Parameters.bFlipWinding) { std::swap(B, C); }
			Out.Indices.push_back(A);
			Out.Indices.push_back(B);
			Out.Indices.push_back(C);
		};

		for (int32_t J = 0; J < Cells; ++J)
		{
			for (int32_t I = 0; I < Cells; ++I)
			{
				const uint32_t TopLeft = static_cast<uint32_t>(J * Posts + I);
				const uint32_t TopRight = TopLeft + 1;
				const uint32_t BottomLeft = TopLeft + Posts;
				const uint32_t BottomRight = BottomLeft + 1;

				AddTriangle(TopLeft, TopRight, BottomRight);
				AddTriangle(TopLeft, BottomRight, BottomLeft);
			}
		}

		// --- Gonne -------------------------------------------------------------
		//
		// Una gonna e' una striscia verticale che scende dal bordo della tile.
		// Non nasconde una crepa dipingendoci sopra: la RIEMPIE, perche' il buco
		// fra due tile di livello diverso e' sempre delimitato dai due bordi, e
		// un muro che scende da uno dei due lo tappa da qualunque angolo lo si
		// guardi, tranne che da sotto il terreno.
		//
		// Le gonne servono SOLO fra livelli diversi. Fra tile dello stesso
		// livello le crepe non esistono per costruzione, grazie al post di bordo
		// condiviso: verificato in Fase 2 e di nuovo in Fase 3, bit per bit.
		if (Parameters.bGenerateSkirt)
		{
			const double Depth = (Parameters.SkirtDepthMetres > 0.0)
				? Parameters.SkirtDepthMetres : ComputeSkirtDepth(Tile.Key.Level);

			uint32_t SkirtBase = InteriorVertices;

			// I quattro bordi, ognuno percorso in modo che l'interno resti a
			// sinistra: cosi' l'orientamento dei triangoli della gonna e'
			// coerente con quello della superficie.
			struct FEdge { int32_t StartI, StartJ, StepI, StepJ; };
			const FEdge Edges[4] = {
				{ 0,     0,     1,  0 },   // bordo nord, verso est
				{ Cells, 0,     0,  1 },   // bordo est, verso sud
				{ Cells, Cells, -1, 0 },   // bordo sud, verso ovest
				{ 0,     Cells, 0, -1 },   // bordo ovest, verso nord
			};

			for (const FEdge& Edge : Edges)
			{
				for (int32_t Step = 0; Step <= Cells; ++Step)
				{
					const int32_t I = Edge.StartI + Edge.StepI * Step;
					const int32_t J = Edge.StartJ + Edge.StepJ * Step;
					const size_t Source = static_cast<size_t>(J) * Posts + I;
					const uint32_t Target = SkirtBase + static_cast<uint32_t>(Step);

					// Il vertice della gonna sta sotto quello del bordo, lungo
					// l'alto LOCALE. Nel frame NEU l'alto locale dell'origine e'
					// esattamente +Z, e su una tile la differenza di verticale
					// fra centro e bordo e' trascurabile.
					Detail::SetVector3(Out.Positions, Target,
						LocalX[Source], LocalY[Source], LocalZ[Source] - Depth);

					// Normale orizzontale, uguale a quella del post di bordo ma
					// senza componente verticale: la parete si illumina come una
					// parete e non come un pezzo di terreno.
					const double Nx = Out.Normals[Source * 3 + 0];
					const double Ny = Out.Normals[Source * 3 + 1];
					const double Horizontal = std::sqrt(Nx * Nx + Ny * Ny);
					if (Horizontal > 1e-6)
					{
						Detail::SetVector3(Out.Normals, Target, Nx / Horizontal, Ny / Horizontal, 0.0);
					}
					else
					{
						Detail::SetVector3(Out.Normals, Target, 0.0, 0.0, -1.0);
					}

					Out.UVs[Target * 2 + 0] = Out.UVs[Source * 2 + 0];
					Out.UVs[Target * 2 + 1] = Out.UVs[Source * 2 + 1];
				}

				for (int32_t Step = 0; Step < Cells; ++Step)
				{
					const int32_t I0 = Edge.StartI + Edge.StepI * Step;
					const int32_t J0 = Edge.StartJ + Edge.StepJ * Step;
					const int32_t I1 = Edge.StartI + Edge.StepI * (Step + 1);
					const int32_t J1 = Edge.StartJ + Edge.StepJ * (Step + 1);

					const uint32_t Top0 = static_cast<uint32_t>(J0 * Posts + I0);
					const uint32_t Top1 = static_cast<uint32_t>(J1 * Posts + I1);
					const uint32_t Low0 = SkirtBase + static_cast<uint32_t>(Step);
					const uint32_t Low1 = SkirtBase + static_cast<uint32_t>(Step + 1);

					AddTriangle(Top0, Low0, Low1);
					AddTriangle(Top0, Low1, Top1);
				}

				SkirtBase += static_cast<uint32_t>(Posts);
			}
		}

		Out.TriangleCount = static_cast<uint32_t>(Out.Indices.size() / 3);
	}
}

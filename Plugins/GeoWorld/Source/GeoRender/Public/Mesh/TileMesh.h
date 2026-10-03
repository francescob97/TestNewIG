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

		/**
		 * GEOMORPHING (facoltativo, vuoti se non calcolati). Per ogni vertice:
		 *   MorphDeltas   quanto il vertice sta SOPRA la superficie del padre,
		 *                 in metri, lungo l'alto locale;
		 *   ParentNormals la normale della superficie del padre in quel punto
		 *                 (3 per vertice, frame locale).
		 * Il materiale li usa per far nascere la tile con la forma e la luce
		 * del padre e farla scivolare verso le proprie: vedi ComputeMorphTargets.
		 */
		std::vector<float> MorphDeltas;
		std::vector<float> ParentNormals;

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

		/**
		 * Passo della mesh, in post: 1 = tutti i 129x129 post (33.792
		 * triangoli), 2 = uno ogni due (65x65, 8.448 triangoli), 4 = uno ogni
		 * quattro (33x33, 2.112). Deve dividere 128; altrimenti vale 1.
		 *
		 * PERCHE' ESISTE. Alla prima prova su un portatile da 16 GB il terreno
		 * chiedeva 15-18 milioni di triangoli in memoria. Il dettaglio a schermo
		 * lo decide la soglia d'errore, non quanti triangoli ha una tile: con un
		 * passo 2 e l'errore geometrico raddoppiato (FViewParameters::
		 * GeometricErrorScale) il LOD sceglie tile piu' piccole, ognuna con un
		 * quarto dei triangoli. A parita' di dettaglio sullo schermo servono
		 * meno triangoli, perche' il dettaglio si distribuisce piu' fine: una
		 * tile grande sovra-dettaglia la sua parte lontana.
		 */
		int32_t Step = 1;
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
	 * La griglia e' 129x129 post; i triangoli sono 128x128x2 (con Step 1; con
	 * Step 2 65x65 post e 64x64x2 triangoli). La gonna aggiunge quattro
	 * strisce lungo il perimetro.
	 */
	inline void BuildTileMesh(const FHeightTile& Tile, const FTileMeshParameters& Parameters,
	                          FTileMeshData& Out, const FEllipsoid& Ellipsoid = Core::WGS84)
	{
		Out = FTileMeshData();

		if (!Tile.IsValid()) { return; }

		// La griglia della MESH, che puo' essere piu' rada di quella dei dati:
		// con Step 2 si usa un post ogni due. Da qui in giu' Posts e Cells
		// parlano della mesh; i dati si leggono a (I*PostStep, J*PostStep).
		const int32_t PostStep = (Parameters.Step >= 1 && Tiles::TileCells % Parameters.Step == 0)
			? Parameters.Step : 1;
		const int32_t Cells = Tiles::TileCells / PostStep;
		const int32_t Posts = Cells + 1;

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

		const double Spacing = Tiles::PostSpacingDeg(Tile.Key.Level) * PostStep;

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
					                       Tile.GetHeight(I * PostStep, J * PostStep)), Ellipsoid);

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
			// Con una mesh piu' rada il bordo grossolano si stacca di piu' dal
			// terreno vero: la gonna scende in proporzione al passo.
			const double Depth = (Parameters.SkirtDepthMetres > 0.0)
				? Parameters.SkirtDepthMetres : ComputeSkirtDepth(Tile.Key.Level) * PostStep;

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

	// =========================================================================
	//  La superficie DISEGNATA, e il geomorphing
	// =========================================================================

	/**
	 * La quota del terreno come la disegna la mesh di BuildTileMesh: stessa
	 * griglia (un post ogni Step), stessa diagonale (nord-ovest / sud-est).
	 *
	 * Fuori dalla tile non si blocca al bordo: si prolunga il piano del
	 * triangolo piu' vicino. Serve agli spigoli delle strade che sporgono oltre
	 * il bordo, e ai punti del figlio che cadono proprio sul bordo del padre.
	 */
	class FSurfaceSampler
	{
	public:
		FSurfaceSampler(const FHeightTile& InTile, int32_t Step)
			: Tile(&InTile)
		{
			PostStep = (Step >= 1 && Tiles::TileCells % Step == 0) ? Step : 1;
			Cells = Tiles::TileCells / PostStep;
			Bounds = Tiles::GetTileBounds(InTile.Key.Level, InTile.Key.X, InTile.Key.Y);
			SpanDeg = Tiles::TileSpanDeg(InTile.Key.Level);
		}

		bool IsValid() const { return Tile && Tile->IsValid(); }
		int32_t GetCells() const { return Cells; }
		const Tiles::FTileBounds& GetBounds() const { return Bounds; }
		double GetCellDegrees() const { return SpanDeg / Cells; }

		/** Quota ellissoidica in metri al punto dato (gradi). */
		double HeightAt(double Lon, double Lat) const
		{
			const double X = (Lon - Bounds.West) / SpanDeg * Cells;
			const double Y = (Bounds.North - Lat) / SpanDeg * Cells;
			const int32_t I = std::min(Cells - 1, std::max(0, static_cast<int32_t>(std::floor(X))));
			const int32_t J = std::min(Cells - 1, std::max(0, static_cast<int32_t>(std::floor(Y))));
			const double Fx = X - I;
			const double Fy = Y - J;

			const double H00 = Tile->GetHeight(I * PostStep, J * PostStep);
			const double H10 = Tile->GetHeight((I + 1) * PostStep, J * PostStep);
			const double H01 = Tile->GetHeight(I * PostStep, (J + 1) * PostStep);
			const double H11 = Tile->GetHeight((I + 1) * PostStep, (J + 1) * PostStep);

			// I due triangoli di BuildTileMesh: (NO, NE, SE) sopra la diagonale,
			// (NO, SE, SO) sotto. Dentro ognuno la quota e' un piano.
			if (Fx >= Fy) { return H00 + Fx * (H10 - H00) + Fy * (H11 - H10); }
			return H00 + Fy * (H01 - H00) + Fx * (H11 - H01);
		}

	private:
		const FHeightTile* Tile = nullptr;
		int32_t PostStep = 1;
		int32_t Cells = Tiles::TileCells;
		Tiles::FTileBounds Bounds;
		double SpanDeg = 1.0;
	};

	/**
	 * Calcola i bersagli del geomorphing: per ogni vertice della mesh di
	 * `Tile`, quanto sta sopra la superficie disegnata del padre e la normale
	 * del padre in quel punto.
	 *
	 * PERCHE'. Quando una tile si raffina, al suo posto compaiono le quattro
	 * figlie: in un frame cambiano la forma del terreno (piu' dettaglio), le
	 * normali (quindi quanto e' illuminato ogni pendio) e, di solito, la foto.
	 * L'occhio lo vede come un lampo. Con questi dati il materiale fa nascere
	 * le figlie ESATTAMENTE con la forma e la luce del padre (sposta ogni
	 * vertice di -Delta e usa la normale del padre) e le fa scivolare verso le
	 * proprie in mezzo secondo.
	 *
	 * Il padre si disegna con lo stesso passo del figlio, quindi la sua
	 * superficie si campiona con lo stesso FSurfaceSampler: la forma di
	 * partenza e' quella che c'era a schermo, al centimetro.
	 */
	inline void ComputeMorphTargets(const FHeightTile& Tile, const FHeightTile& Parent, int32_t Step,
	                                FTileMeshData& InOut)
	{
		InOut.MorphDeltas.clear();
		InOut.ParentNormals.clear();
		if (!Tile.IsValid() || !Parent.IsValid() || InOut.InteriorVertexCount == 0) { return; }

		const int32_t PostStep = (Step >= 1 && Tiles::TileCells % Step == 0) ? Step : 1;
		const int32_t Cells = Tiles::TileCells / PostStep;
		const int32_t Posts = Cells + 1;
		if (InOut.InteriorVertexCount != static_cast<uint32_t>(Posts) * Posts) { return; }

		const FSurfaceSampler ParentSurface(Parent, Step);
		const Tiles::FTileBounds Bounds = Tiles::GetTileBounds(Tile.Key.Level, Tile.Key.X, Tile.Key.Y);
		const double Spacing = Tiles::PostSpacingDeg(Tile.Key.Level) * PostStep;

		// Il passo delle differenze finite per la normale del padre: una sua
		// cella. Metri per grado alla latitudine della tile.
		const double Probe = ParentSurface.GetCellDegrees();
		constexpr double MetresPerDegreeLat = 111132.0;
		constexpr double MetresPerDegreeLonAtEquator = 111320.0;
		const double MetresLon = MetresPerDegreeLonAtEquator * std::cos(Bounds.CentreLat() * Core::DegToRad);

		const size_t VertexCount = InOut.Positions.size() / 3;
		InOut.MorphDeltas.assign(VertexCount, 0.0f);
		InOut.ParentNormals.assign(VertexCount * 3, 0.0f);

		for (int32_t J = 0; J < Posts; ++J)
		{
			const double Lat = Bounds.North - static_cast<double>(J) * Spacing;
			for (int32_t I = 0; I < Posts; ++I)
			{
				const double Lon = Bounds.West + static_cast<double>(I) * Spacing;
				const size_t Index = static_cast<size_t>(J) * Posts + I;

				const double Own = Tile.GetHeight(I * PostStep, J * PostStep);
				InOut.MorphDeltas[Index] = static_cast<float>(Own - ParentSurface.HeightAt(Lon, Lat));

				const double DhDEast = (ParentSurface.HeightAt(Lon + Probe, Lat) - ParentSurface.HeightAt(Lon - Probe, Lat))
				                     / (2.0 * Probe * MetresLon);
				const double DhDNorth = (ParentSurface.HeightAt(Lon, Lat + Probe) - ParentSurface.HeightAt(Lon, Lat - Probe))
				                      / (2.0 * Probe * MetresPerDegreeLat);
				// Frame NEU: X nord, Y est, Z alto.
				double Nx = -DhDNorth, Ny = -DhDEast, Nz = 1.0;
				const double Length = std::sqrt(Nx * Nx + Ny * Ny + Nz * Nz);
				Nx /= Length; Ny /= Length; Nz /= Length;
				InOut.ParentNormals[Index * 3 + 0] = static_cast<float>(Nx);
				InOut.ParentNormals[Index * 3 + 1] = static_cast<float>(Ny);
				InOut.ParentNormals[Index * 3 + 2] = static_cast<float>(Nz);
			}
		}

		// Le gonne scendono dai post di bordo: stesso spostamento e stessa
		// normale (orizzontale come la loro). Sono in fondo, nell'ordine dei
		// quattro bordi di BuildTileMesh.
		if (InOut.SkirtVertexCount == static_cast<uint32_t>(4 * Posts))
		{
			struct FEdge { int32_t StartI, StartJ, StepI, StepJ; };
			const FEdge Edges[4] = { { 0, 0, 1, 0 }, { Cells, 0, 0, 1 }, { Cells, Cells, -1, 0 }, { 0, Cells, 0, -1 } };
			size_t Target = InOut.InteriorVertexCount;
			for (const FEdge& Edge : Edges)
			{
				for (int32_t Walk = 0; Walk <= Cells; ++Walk, ++Target)
				{
					const size_t Source = static_cast<size_t>(Edge.StartJ + Edge.StepJ * Walk) * Posts
					                    + static_cast<size_t>(Edge.StartI + Edge.StepI * Walk);
					InOut.MorphDeltas[Target] = InOut.MorphDeltas[Source];
					for (int Axis = 0; Axis < 3; ++Axis)
					{
						InOut.ParentNormals[Target * 3 + Axis] = InOut.Normals[Target * 3 + Axis];
					}
				}
			}
		}
	}
}

// =============================================================================
//  RoadMesh.h -- Le strade in 3D: nastri di geometria posati sul terreno.
//  STRATO: C++ PURO, header-only.
//
//  Da lontano le strade sono dipinte sull'ortofoto (RoadRasterizer.h). Da
//  vicino, sulle tile di terreno piu' fini, diventano GEOMETRIA: un nastro largo
//  quanto la strada, con l'asfalto e la segnaletica, le traversine dei binari,
//  e i ponti sollevati sopra la valle invece che dipinti sul fondo.
//
//  IL PROBLEMA VERO: STARE SUL TERRENO, NE' SOPRA NE' SOTTO
//  Il terreno che si vede non e' il DEM: e' una mesh di triangoli, con un post
//  ogni due (passo 2) e ogni cella divisa lungo la diagonale nord-ovest /
//  sud-est. Una strada che prendesse le quote dal DEM "vero" starebbe in certi
//  punti mezzo metro sotto la superficie disegnata (e sparirebbe a tratti) e
//  in altri sopra (e galleggerebbe). Qui si fa il contrario:
//
//    1. la quota si prende dalla MESH, interpolando nello stesso triangolo che
//       il terreno disegna (FSurfaceSampler);
//    2. la strada si spezza dove attraversa uno spigolo della mesh: le linee
//       della griglia e le diagonali. Fra due tagli il terreno e' un piano, e
//       un segmento dritto ci sta sopra esattamente;
//    3. la si solleva di 20 cm, piu' qualche centimetro per classe, cosi' due
//       strade che si incrociano non si contendono lo stesso piano (z-fighting).
//
//  La mesh si costruisce PER TILE DI TERRENO, nel frame locale della tile
//  (stessa origine di BuildTileMesh): il componente della strada ha la stessa
//  trasformazione del terreno e segue il rebase senza conti in piu'.
// =============================================================================
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "Geo/Ellipsoid.h"
#include "Mesh/TileMesh.h"
#include "Roads/RoadRasterizer.h"
#include "Tiles/TileFormat.h"
#include "Tiles/TilingScheme.h"
#include "Tiles/VectorTileFormat.h"

namespace GeoWorld::Roads
{
	using Core::FEcef;
	using Core::FGeodetic;
	using Core::FMat3;
	using Mesh::FTileMeshData;
	using Tiles::FHeightTile;

	// =========================================================================
	//  L'atlante delle superfici
	// =========================================================================

	/**
	 * Una texture sola per tutte le strade, divisa in strisce verticali: una per
	 * superficie. La U della mesh sceglie la striscia (e la attraversa da un
	 * bordo all'altro della strada), la V scorre lungo la strada e si ripete
	 * ogni RoadPatternMetres.
	 *
	 * PERCHE' UN ATLANTE e non una texture per tipo: una tile di citta' ha
	 * strade di sei tipi, e sei materiali vorrebbero dire sei sezioni di mesh e
	 * sei chiamate di disegno per tile. Con l'atlante e' una.
	 */
	enum class ERoadSurface : int32_t
	{
		MajorAsphalt = 0,   // asfalto con strisce bianche ai bordi e mezzeria tratteggiata
		MinorAsphalt = 1,   // asfalto senza segnaletica
		Dirt = 2,           // sterrato
		Rail = 3,           // massicciata, traversine, due rotaie
		Concrete = 4,       // piste, fianchi dei ponti
		Paving = 5,         // zone pedonali
		Gravel = 6,         // sentieri
	};

	inline constexpr int32_t RoadAtlasStrips = 8;
	inline constexpr int32_t RoadAtlasStripPixels = 32;
	inline constexpr int32_t RoadAtlasWidth = RoadAtlasStrips * RoadAtlasStripPixels;   // 256
	inline constexpr int32_t RoadAtlasHeight = 256;

	/**
	 * Metri di strada coperti da un'altezza dell'atlante. 12 m e' il ciclo
	 * della mezzeria extraurbana italiana: 4,5 m di striscia e 7,5 di vuoto.
	 * Con 20 traversine ogni 12 m il passo e' 60 cm, quello vero.
	 */
	inline constexpr double RoadPatternMetres = 12.0;

	namespace Detail
	{
		/** Rumore deterministico per pixel, -1..1: l'asfalto non e' una tinta unita. */
		inline float AtlasNoise(int32_t X, int32_t Y)
		{
			uint32_t Hash = static_cast<uint32_t>(X) * 73856093u ^ static_cast<uint32_t>(Y) * 19349663u;
			Hash = (Hash ^ (Hash >> 13)) * 1274126177u;
			Hash ^= Hash >> 16;
			return static_cast<float>(Hash & 0xFFFF) / 32767.5f - 1.0f;
		}

		inline void PutAtlasPixel(std::vector<uint8_t>& Pixels, int32_t X, int32_t Y,
		                          int R, int G, int B, float Noise)
		{
			auto Clamp = [](float Value) { return static_cast<uint8_t>(std::min(255.0f, std::max(0.0f, Value))); };
			uint8_t* Out = Pixels.data() + (static_cast<size_t>(Y) * RoadAtlasWidth + X) * 4;
			Out[0] = Clamp(static_cast<float>(B) + Noise);
			Out[1] = Clamp(static_cast<float>(G) + Noise);
			Out[2] = Clamp(static_cast<float>(R) + Noise);
			Out[3] = 255;
		}
	}

	/**
	 * Disegna l'atlante: RoadAtlasWidth x RoadAtlasHeight pixel BGRA sRGB, riga
	 * 0 = inizio del ciclo lungo la strada. Si genera a runtime (niente asset
	 * binari da mantenere) e si prova nei test, colore per colore.
	 */
	inline void BuildRoadAtlas(std::vector<uint8_t>& OutBGRA)
	{
		OutBGRA.assign(static_cast<size_t>(RoadAtlasWidth) * RoadAtlasHeight * 4, 255);
		const int32_t W = RoadAtlasStripPixels;

		for (int32_t Y = 0; Y < RoadAtlasHeight; ++Y)
		{
			for (int32_t Column = 0; Column < W; ++Column)
			{
				const float Fine = Detail::AtlasNoise(Column, Y) * 6.0f;
				const float Coarse = Detail::AtlasNoise(Column / 3, Y / 3) * 10.0f;

				// 0. Asfalto con segnaletica: bordi a 1 pixel dal margine (su una
				// carreggiata da 7 m un pixel e' ~22 cm, come la striscia vera),
				// mezzeria tratteggiata per 4,5 m su 12.
				{
					const int32_t X = 0 * W + Column;
					const bool bEdge = (Column == 2 || Column == W - 3);
					const bool bCentre = (Column == W / 2 - 1 || Column == W / 2)
					                  && Y < (RoadAtlasHeight * 45) / 120;
					if (bEdge || bCentre) { Detail::PutAtlasPixel(OutBGRA, X, Y, 222, 222, 216, Fine * 0.5f); }
					else { Detail::PutAtlasPixel(OutBGRA, X, Y, 74, 74, 76, Fine + Coarse * 0.4f); }
				}
				// 1. Asfalto semplice, piu' chiaro e piu' consumato.
				Detail::PutAtlasPixel(OutBGRA, 1 * W + Column, Y, 88, 87, 86, Fine + Coarse * 0.6f);
				// 2. Sterrato: due solchi piu' scuri dove passano le ruote.
				{
					const bool bRut = (Column >= 7 && Column <= 9) || (Column >= W - 10 && Column <= W - 8);
					const int Base = bRut ? 112 : 136;
					Detail::PutAtlasPixel(OutBGRA, 2 * W + Column, Y, Base, Base - 16, Base - 42, Fine * 1.5f + Coarse);
				}
				// 3. Ferrovia: massicciata, traversine ogni 60 cm, due rotaie
				// a 1,435 m (scartamento) attorno al centro di un nastro da 4,5 m.
				{
					const int32_t X = 3 * W + Column;
					const bool bRail = (Column == 11 || Column == 20);
					const bool bSleeper = (Column >= 7 && Column <= 24) && ((Y % (RoadAtlasHeight / 20)) < 5);
					if (bRail) { Detail::PutAtlasPixel(OutBGRA, X, Y, 150, 148, 145, Fine * 0.3f); }
					else if (bSleeper) { Detail::PutAtlasPixel(OutBGRA, X, Y, 96, 80, 62, Fine); }
					else { Detail::PutAtlasPixel(OutBGRA, X, Y, 118, 110, 100, Fine * 2.0f + Coarse); }
				}
				// 4. Cemento: piste e fianchi dei ponti.
				Detail::PutAtlasPixel(OutBGRA, 4 * W + Column, Y, 150, 150, 146, Fine + Coarse * 0.3f);
				// 5. Lastricato pedonale: un reticolo di giunti.
				{
					const bool bJoint = (Column % 8 == 0) || (Y % 8 == 0);
					const int Base = bJoint ? 120 : 160;
					Detail::PutAtlasPixel(OutBGRA, 5 * W + Column, Y, Base, Base - 6, Base - 14, Fine);
				}
				// 6. Ghiaia chiara dei sentieri.
				Detail::PutAtlasPixel(OutBGRA, 6 * W + Column, Y, 168, 158, 140, Fine * 2.0f);
				// 7. Riserva: grigio neutro.
				Detail::PutAtlasPixel(OutBGRA, 7 * W + Column, Y, 128, 128, 128, 0.0f);
			}
		}
	}

	/** U di inizio e fine della striscia, con un pixel e mezzo di margine contro i bordi. */
	inline void SurfaceUvRange(ERoadSurface Surface, float& OutU0, float& OutU1)
	{
		const float Strip = 1.0f / static_cast<float>(RoadAtlasStrips);
		const float Margin = 1.5f / static_cast<float>(RoadAtlasWidth);
		const float Start = static_cast<float>(static_cast<int32_t>(Surface)) * Strip;
		OutU0 = Start + Margin;
		OutU1 = Start + Strip - Margin;
	}

	// =========================================================================
	//  Quali classi diventano 3D, e come
	// =========================================================================

	struct FRoad3DClass
	{
		bool bEnabled = false;
		ERoadSurface Surface = ERoadSurface::MinorAsphalt;
		/** Piu' alto = sta sopra agli incroci (qualche centimetro piu' in alto). */
		int32_t Tier = 0;
	};

	struct FRoad3DStyle
	{
		FRoad3DClass Classes[Tiles::RoadClassCount];

		FRoad3DClass For(const FVectorFeature& Feature) const
		{
			const int32_t Index = static_cast<int32_t>(Feature.Class);
			if (Index <= 0 || Index >= Tiles::RoadClassCount) { return FRoad3DClass{}; }
			FRoad3DClass Result = Classes[Index];
			// Lo sterrato vale per le strade, non per binari e piste.
			if (Feature.IsUnpaved() && Index < static_cast<int32_t>(ERoadClass::Rail)
			    && Result.Surface != ERoadSurface::Gravel)
			{
				Result.Surface = ERoadSurface::Dirt;
			}
			return Result;
		}
	};

	inline FRoad3DStyle MakeRoad3DStyle()
	{
		FRoad3DStyle Style;
		auto Set = [&Style](ERoadClass Class, ERoadSurface Surface, int32_t Tier)
		{
			FRoad3DClass& Entry = Style.Classes[static_cast<int32_t>(Class)];
			Entry.bEnabled = true;
			Entry.Surface = Surface;
			Entry.Tier = Tier;
		};
		Set(ERoadClass::Motorway,     ERoadSurface::MajorAsphalt, 9);
		Set(ERoadClass::Trunk,        ERoadSurface::MajorAsphalt, 8);
		Set(ERoadClass::Primary,      ERoadSurface::MajorAsphalt, 7);
		Set(ERoadClass::Secondary,    ERoadSurface::MajorAsphalt, 6);
		Set(ERoadClass::Tertiary,     ERoadSurface::MajorAsphalt, 5);
		Set(ERoadClass::Unclassified, ERoadSurface::MinorAsphalt, 4);
		Set(ERoadClass::Residential,  ERoadSurface::MinorAsphalt, 4);
		Set(ERoadClass::LivingStreet, ERoadSurface::Paving,       3);
		Set(ERoadClass::Service,      ERoadSurface::MinorAsphalt, 3);
		Set(ERoadClass::Pedestrian,   ERoadSurface::Paving,       2);
		Set(ERoadClass::Track,        ERoadSurface::Dirt,         1);
		Set(ERoadClass::Path,         ERoadSurface::Gravel,       0);
		Set(ERoadClass::Rail,         ERoadSurface::Rail,         6);
		Set(ERoadClass::LightRail,    ERoadSurface::Rail,         5);
		Set(ERoadClass::RailService,  ERoadSurface::Rail,         4);
		Set(ERoadClass::Runway,       ERoadSurface::Concrete,     2);
		Set(ERoadClass::Taxiway,      ERoadSurface::Concrete,     2);
		return Style;
	}

	// =========================================================================
	//  La quota della superficie DISEGNATA
	// =========================================================================

	/**
	 * La quota del terreno come la disegna la mesh di BuildTileMesh: stessa
	 * griglia (un post ogni Step), stessa diagonale (nord-ovest / sud-est).
	 *
	 * Fuori dalla tile non si blocca al bordo: si prolunga il piano del
	 * triangolo piu' vicino. Serve agli spigoli dei nastri che sporgono di
	 * qualche metro oltre il bordo, dove la strada esce dalla tile.
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

	// =========================================================================
	//  La costruzione
	// =========================================================================

	struct FRoadMeshParameters
	{
		/** Passo della mesh del terreno: DEVE essere quello usato per la tile. */
		int32_t Step = 2;
		/** Stessa convenzione del terreno (FTileMeshParameters::bFlipWinding). */
		bool bFlipWinding = true;
		/** Sollevamento sul terreno, in metri. */
		double Lift = 0.20;
		/** In piu' per ogni gradino di classe: agli incroci la piu' importante sta sopra. */
		double LiftPerTier = 0.02;
		/** Spessore dell'impalcato dei ponti, in metri. */
		double BridgeDepth = 1.2;
		/** Moltiplica la larghezza vera. */
		double WidthScale = 1.0;
	};

	struct FRoadMeshStats
	{
		int32_t Features = 0;
		int32_t Bridges = 0;
		int32_t CrossSections = 0;
		int32_t Triangles = 0;
	};

	namespace Detail
	{
		struct FVec2 { double X = 0.0, Y = 0.0; };

		inline FVec2 Add(FVec2 A, FVec2 B) { return { A.X + B.X, A.Y + B.Y }; }
		inline FVec2 Sub(FVec2 A, FVec2 B) { return { A.X - B.X, A.Y - B.Y }; }
		inline FVec2 Scale(FVec2 A, double S) { return { A.X * S, A.Y * S }; }
		inline double Dot(FVec2 A, FVec2 B) { return A.X * B.X + A.Y * B.Y; }
		inline double Length(FVec2 A) { return std::sqrt(Dot(A, A)); }

		/** Ritaglio di Liang-Barsky di un segmento sul quadrato [0,1]x[0,1]. */
		inline bool ClipToUnitSquare(FVec2 A, FVec2 B, double& OutT0, double& OutT1)
		{
			OutT0 = 0.0;
			OutT1 = 1.0;
			const double Dx = B.X - A.X;
			const double Dy = B.Y - A.Y;
			const double P[4] = { -Dx, Dx, -Dy, Dy };
			const double Q[4] = { A.X, 1.0 - A.X, A.Y, 1.0 - A.Y };
			for (int Side = 0; Side < 4; ++Side)
			{
				if (P[Side] == 0.0)
				{
					if (Q[Side] < 0.0) { return false; }
					continue;
				}
				const double R = Q[Side] / P[Side];
				if (P[Side] < 0.0) { OutT0 = std::max(OutT0, R); }
				else { OutT1 = std::min(OutT1, R); }
				if (OutT0 > OutT1) { return false; }
			}
			return OutT1 - OutT0 > 1e-12;
		}

		/**
		 * Dove un segmento (in coordinate di cella) taglia gli spigoli della
		 * mesh: le verticali x = k, le orizzontali y = k e le diagonali
		 * x - y = k. Aggiunge i parametri t in (0, 1).
		 */
		inline void AddEdgeCrossings(double X0, double Y0, double X1, double Y1, std::vector<double>& OutT)
		{
			auto Family = [&OutT](double A, double B)
			{
				if (std::fabs(B - A) < 1e-12) { return; }
				const double Low = std::min(A, B);
				const double High = std::max(A, B);
				for (double K = std::floor(Low) + 1.0; K < High; K += 1.0)
				{
					const double T = (K - A) / (B - A);
					if (T > 0.0 && T < 1.0) { OutT.push_back(T); }
				}
			};
			Family(X0, X1);
			Family(Y0, Y1);
			Family(X0 - Y0, X1 - Y1);
		}
	}

	/**
	 * Costruisce le strade 3D di UNA tile di terreno.
	 *
	 * `Surface` e' la superficie della tile (stesso passo della sua mesh).
	 * `Wide`, facoltativa, e' la superficie di un antenato che copre tutta la
	 * tile vettoriale: serve alle estremita' dei ponti, che possono cadere fuori
	 * dalla tile di terreno. Senza, si usa `Surface` prolungata.
	 *
	 * Il risultato e' nel frame locale della tile, con la STESSA origine di
	 * BuildTileMesh (centro della tile, a meta' fra le quote estreme).
	 */
	inline void BuildRoadMesh(const FHeightTile& TerrainTile, const FSurfaceSampler& Surface,
	                          const FSurfaceSampler* Wide, const FVectorTile& Vectors,
	                          const FVectorWindow& Window, const FRoad3DStyle& Style,
	                          const FRoadMeshParameters& Parameters, FTileMeshData& Out,
	                          FRoadMeshStats* OutStats = nullptr,
	                          const Core::FEllipsoid& Ellipsoid = Core::WGS84)
	{
		using namespace Detail;
		Out = FTileMeshData();
		FRoadMeshStats Stats;
		if (!TerrainTile.IsValid() || Window.Span <= 0.0)
		{
			if (OutStats) { *OutStats = Stats; }
			return;
		}

		const Tiles::FTileBounds Bounds = Tiles::GetTileBounds(TerrainTile.Key.Level, TerrainTile.Key.X, TerrainTile.Key.Y);
		const double SpanDeg = Tiles::TileSpanDeg(TerrainTile.Key.Level);

		// La stessa origine di BuildTileMesh: il componente della strada usa la
		// trasformazione della tile di terreno.
		Out.Origin = FGeodetic::FromDegrees(Bounds.CentreLat(), Bounds.CentreLon(),
		                                    0.5 * (TerrainTile.MinHeight + TerrainTile.MaxHeight));
		Out.MinHeight = TerrainTile.MinHeight;
		Out.MaxHeight = TerrainTile.MaxHeight;
		const FEcef OriginEcef = Core::GeodeticToEcef(Out.Origin, Ellipsoid);
		const FMat3 EcefToLocal = Mesh::Detail::MakeLocalNeu(Out.Origin);

		// Metri per unita' di frazione della tile, in est (u) e in sud (v).
		constexpr double MetresPerDegreeLat = 111132.0;
		constexpr double MetresPerDegreeLonAtEquator = 111320.0;
		const double MetresU = SpanDeg * MetresPerDegreeLonAtEquator * std::cos(Bounds.CentreLat() * Core::DegToRad);
		const double MetresV = SpanDeg * MetresPerDegreeLat;
		const double Cells = static_cast<double>(Surface.GetCells());

		auto ToLonLat = [&](FVec2 Metric, double& OutLon, double& OutLat)
		{
			OutLon = Bounds.West + (Metric.X / MetresU) * SpanDeg;
			OutLat = Bounds.North - (Metric.Y / MetresV) * SpanDeg;
		};

		// Vertici e indici: ogni quadrilatero ha i suoi quattro vertici, con la
		// sua normale. Costa il doppio dei vertici di una striscia condivisa, ma
		// l'orientamento e le normali di ponti e fianchi sono giusti per
		// costruzione, e su una tile di citta' si parla di decine di migliaia.
		auto AddVertex = [&](const FEcef& Local, const FEcef& Normal, float U, float V)
		{
			Out.Positions.push_back(static_cast<float>(Local.X));
			Out.Positions.push_back(static_cast<float>(Local.Y));
			Out.Positions.push_back(static_cast<float>(Local.Z));
			Out.Normals.push_back(static_cast<float>(Normal.X));
			Out.Normals.push_back(static_cast<float>(Normal.Y));
			Out.Normals.push_back(static_cast<float>(Normal.Z));
			Out.UVs.push_back(U);
			Out.UVs.push_back(V);
			return static_cast<uint32_t>(Out.Positions.size() / 3 - 1);
		};

		// Un quadrilatero A-B-C-D (in giro), con la normale che deve avere.
		// L'ordine dei triangoli si sceglie perche' la normale geometrica
		// coincida con quella voluta, poi si applica la stessa inversione del
		// terreno: cosi' la faccia visibile e' la stessa convenzione.
		auto Cross = [](const FEcef& A, const FEcef& B)
		{
			return FEcef{ A.Y * B.Z - A.Z * B.Y, A.Z * B.X - A.X * B.Z, A.X * B.Y - A.Y * B.X };
		};
		auto DotE = [](const FEcef& A, const FEcef& B) { return A.X * B.X + A.Y * B.Y + A.Z * B.Z; };

		// L'orientamento si decide TRIANGOLO PER TRIANGOLO. Nelle curve strette
		// il lato interno di due sezioni consecutive puo' incrociarsi, e il
		// quadrilatero diventa un "papillon": i suoi due triangoli hanno versi
		// opposti, e deciderli insieme ne lasciava uno rovesciato (invisibile
		// dall'alto). Un test lo ha trovato: uno su 1.740.
		auto AddQuad = [&](const FEcef (&Corners)[4], FEcef Wanted, const float (&U)[4], const float (&V)[4])
		{
			const FEcef Geometric = Cross(Corners[1] - Corners[0], Corners[2] - Corners[0])
			                      + Cross(Corners[2] - Corners[0], Corners[3] - Corners[0]);
			const double GeometricLength = std::sqrt(Geometric.LengthSquared());
			if (GeometricLength < 1e-9) { return; }
			const double WantedLength = std::sqrt(Wanted.LengthSquared());
			if (WantedLength < 1e-9)
			{
				// Faccia di sopra: la normale e' quella del quadrilatero, verso l'alto.
				Wanted = FEcef{ Geometric.X / GeometricLength, Geometric.Y / GeometricLength, Geometric.Z / GeometricLength };
				if (Wanted.Z < 0.0) { Wanted = FEcef{ -Wanted.X, -Wanted.Y, -Wanted.Z }; }
			}
			else
			{
				Wanted = FEcef{ Wanted.X / WantedLength, Wanted.Y / WantedLength, Wanted.Z / WantedLength };
			}

			uint32_t Index[4];
			for (int Corner = 0; Corner < 4; ++Corner) { Index[Corner] = AddVertex(Corners[Corner], Wanted, U[Corner], V[Corner]); }

			auto Triangle = [&](int A, int B, int C)
			{
				const FEcef Normal = Cross(Corners[B] - Corners[A], Corners[C] - Corners[A]);
				if (Normal.LengthSquared() < 1e-18) { return; }   // degenere: non si disegna
				// Allineato alla normale voluta: A, B, C; altrimenti al
				// contrario. Poi la stessa inversione del terreno.
				bool bSwap = DotE(Normal, Wanted) < 0.0;
				if (Parameters.bFlipWinding) { bSwap = !bSwap; }
				Out.Indices.push_back(Index[A]);
				Out.Indices.push_back(bSwap ? Index[C] : Index[B]);
				Out.Indices.push_back(bSwap ? Index[B] : Index[C]);
				++Stats.Triangles;
			};
			Triangle(0, 1, 2);
			Triangle(0, 2, 3);
		};

		auto LocalAt = [&](FVec2 Metric, double Height)
		{
			double Lon = 0.0, Lat = 0.0;
			ToLonLat(Metric, Lon, Lat);
			const FEcef Ecef = Core::GeodeticToEcef(
				FGeodetic::FromRadians(Lat * Core::DegToRad, Lon * Core::DegToRad, Height), Ellipsoid);
			return EcefToLocal.Transform(Ecef - OriginEcef);
		};

		auto TerrainAt = [&](FVec2 Metric)
		{
			double Lon = 0.0, Lat = 0.0;
			ToLonLat(Metric, Lon, Lat);
			return Surface.HeightAt(Lon, Lat);
		};

		auto WideAt = [&](FVec2 Metric)
		{
			double Lon = 0.0, Lat = 0.0;
			ToLonLat(Metric, Lon, Lat);
			return (Wide && Wide->IsValid()) ? Wide->HeightAt(Lon, Lat) : Surface.HeightAt(Lon, Lat);
		};

		// Un pezzo di strada fra due sezioni consecutive.
		struct FSection
		{
			FVec2 Left, Right, Centre;
			double Along = 0.0;          // metri dall'inizio della linea
			double LeftHeight = 0.0, RightHeight = 0.0;
		};

		std::vector<FVec2> Points;
		std::vector<double> Cumulative;
		std::vector<double> Splits;
		std::vector<FSection> Sections;

		for (size_t FeatureIndex = 0; FeatureIndex < Vectors.Features.size(); ++FeatureIndex)
		{
			const FVectorFeature& Feature = Vectors.Features[FeatureIndex];
			const FRoad3DClass Class = Style.For(Feature);
			if (!Class.bEnabled || Feature.PointCount < 2) { continue; }

			// Scarto veloce: la linea passa lontano da questa tile?
			const double Reach = Feature.GetWidthMetres() * Parameters.WidthScale;
			const double MarginU = Reach / MetresU * Window.Span;
			const double MarginV = Reach / MetresV * Window.Span;
			if (Feature.MaxX < Window.U0 - MarginU || Feature.MinX > Window.U0 + Window.Span + MarginU ||
			    Feature.MaxY < Window.V0 - MarginV || Feature.MinY > Window.V0 + Window.Span + MarginV)
			{
				continue;
			}

			// La linea in metri, nel sistema della tile di terreno (origine
			// nell'angolo nord-ovest, x verso est, y verso sud).
			Points.resize(Feature.PointCount);
			Cumulative.resize(Feature.PointCount);
			for (uint32_t Index = 0; Index < Feature.PointCount; ++Index)
			{
				const Tiles::FVectorPoint& Point = Vectors.Points[Feature.FirstPoint + Index];
				const double U = (static_cast<double>(Point.X) - Window.U0) / Window.Span;
				const double V = (static_cast<double>(Point.Y) - Window.V0) / Window.Span;
				Points[Index] = FVec2{ U * MetresU, V * MetresV };
				Cumulative[Index] = (Index == 0) ? 0.0 : Cumulative[Index - 1] + Length(Sub(Points[Index], Points[Index - 1]));
			}
			const double TotalLength = Cumulative.back();
			if (TotalLength <= 0.0) { continue; }

			const double Half = 0.5 * Feature.GetWidthMetres() * Parameters.WidthScale;
			const double Lift = Parameters.Lift + Parameters.LiftPerTier * Class.Tier
			                  + 0.003 * static_cast<double>(FeatureIndex % 5);
			const bool bBridge = Feature.IsBridge();

			// Il ponte: un impalcato dritto fra le due spalle, non un nastro
			// posato sul fondo della valle. Le quote delle spalle si prendono
			// dove il ponte comincia e finisce (anche fuori dalla tile: per
			// questo c'e' Wide).
			const double BridgeStart = bBridge ? WideAt(Points.front()) : 0.0;
			const double BridgeEnd = bBridge ? WideAt(Points.back()) : 0.0;

			float U0 = 0.0f, U1 = 1.0f;
			SurfaceUvRange(Class.Surface, U0, U1);
			float ConcreteU0 = 0.0f, ConcreteU1 = 1.0f;
			SurfaceUvRange(ERoadSurface::Concrete, ConcreteU0, ConcreteU1);

			bool bDrawnSomething = false;

			// Un "tratto" e' una sequenza di segmenti consecutivi dentro la tile.
			// Si spezza dove la linea esce e rientra.
			Sections.clear();
			auto FlushRun = [&]()
			{
				for (size_t Index = 0; Index + 1 < Sections.size(); ++Index)
				{
					const FSection& A = Sections[Index];
					const FSection& B = Sections[Index + 1];
					const FEcef Corners[4] = {
						LocalAt(A.Left, A.LeftHeight), LocalAt(A.Right, A.RightHeight),
						LocalAt(B.Right, B.RightHeight), LocalAt(B.Left, B.LeftHeight) };
					const float Us[4] = { U0, U1, U1, U0 };
					const float VA = static_cast<float>(A.Along / RoadPatternMetres);
					const float VB = static_cast<float>(B.Along / RoadPatternMetres);
					const float Vs[4] = { VA, VA, VB, VB };
					AddQuad(Corners, FEcef{ 0.0, 0.0, 0.0 }, Us, Vs);

					if (bBridge)
					{
						// Fianchi e fondo dell'impalcato: senza, da sotto e di
						// lato il ponte sarebbe un foglio di carta.
						const double Depth = Parameters.BridgeDepth;
						const FEcef BottomCorners[4] = {
							LocalAt(A.Left, A.LeftHeight - Depth), LocalAt(A.Right, A.RightHeight - Depth),
							LocalAt(B.Right, B.RightHeight - Depth), LocalAt(B.Left, B.LeftHeight - Depth) };
						const float SideV[4] = { VA, VB, VB, VA };
						const float SideU[4] = { ConcreteU0, ConcreteU0, ConcreteU1, ConcreteU1 };

						// Le normali "volute" dei fianchi: verso l'esterno.
						const FEcef Across = Corners[1] - Corners[0];
						const FEcef Outward{ -Across.X, -Across.Y, 0.0 };
						const FEcef LeftSide[4] = { Corners[0], Corners[3], BottomCorners[3], BottomCorners[0] };
						AddQuad(LeftSide, Outward, SideU, SideV);
						const FEcef RightSide[4] = { Corners[1], Corners[2], BottomCorners[2], BottomCorners[1] };
						AddQuad(RightSide, FEcef{ Across.X, Across.Y, 0.0 }, SideU, SideV);
						const float BottomU[4] = { ConcreteU0, ConcreteU1, ConcreteU1, ConcreteU0 };
						AddQuad(BottomCorners, FEcef{ 0.0, 0.0, -1.0 }, BottomU, Vs);
					}
					bDrawnSomething = true;
				}
				Stats.CrossSections += static_cast<int32_t>(Sections.size());
				Sections.clear();
			};

			auto AddSection = [&](FVec2 Centre, FVec2 Normal, double Along)
			{
				FSection Section;
				Section.Centre = Centre;
				Section.Left = Add(Centre, Scale(Normal, Half));
				Section.Right = Sub(Centre, Scale(Normal, Half));
				Section.Along = Along;

				const double TerrainLeft = TerrainAt(Section.Left);
				const double TerrainRight = TerrainAt(Section.Right);
				if (bBridge)
				{
					// Impalcato in piano trasversale, lineare fra le spalle, mai
					// sotto il terreno.
					const double Fraction = std::min(1.0, std::max(0.0, Along / TotalLength));
					const double Deck = BridgeStart + (BridgeEnd - BridgeStart) * Fraction;
					const double DeckHeight = std::max({ Deck, TerrainAt(Centre), TerrainLeft, TerrainRight });
					Section.LeftHeight = DeckHeight + Lift;
					Section.RightHeight = DeckHeight + Lift;
				}
				else
				{
					Section.LeftHeight = TerrainLeft + Lift;
					Section.RightHeight = TerrainRight + Lift;
				}
				Sections.push_back(Section);
			};

			// Le direzioni dei segmenti, per le sezioni d'angolo (mitra).
			auto SegmentNormal = [&](uint32_t Index)
			{
				const FVec2 Direction = Sub(Points[Index + 1], Points[Index]);
				const double Len = Length(Direction);
				if (Len <= 1e-9) { return FVec2{ 0.0, 1.0 }; }
				return FVec2{ -Direction.Y / Len, Direction.X / Len };
			};

			bool bInRun = false;
			for (uint32_t Index = 0; Index + 1 < Feature.PointCount; ++Index)
			{
				const FVec2 A = Points[Index];
				const FVec2 B = Points[Index + 1];
				const FVec2 UnitA{ A.X / MetresU, A.Y / MetresV };
				const FVec2 UnitB{ B.X / MetresU, B.Y / MetresV };
				double T0 = 0.0, T1 = 1.0;
				if (!ClipToUnitSquare(UnitA, UnitB, T0, T1) || Length(Sub(B, A)) <= 1e-6)
				{
					if (bInRun) { FlushRun(); bInRun = false; }
					continue;
				}

				const FVec2 Normal = SegmentNormal(Index);
				const FVec2 Direction = Sub(B, A);
				const double SegmentLength = Length(Direction);

				// La sezione d'inizio: ingresso nella tile, o angolo con il
				// segmento precedente (mitra, allungata fino al doppio).
				if (!bInRun || T0 > 0.0)
				{
					if (bInRun) { FlushRun(); }
					AddSection(Add(A, Scale(Direction, T0)), Normal, Cumulative[Index] + T0 * SegmentLength);
					bInRun = true;
				}

				// I tagli: dove l'asse e i due bordi attraversano gli spigoli della mesh.
				Splits.clear();
				for (const double Offset : { 0.0, Half, -Half })
				{
					const FVec2 P = Add(A, Scale(Normal, Offset));
					const FVec2 Q = Add(B, Scale(Normal, Offset));
					AddEdgeCrossings(P.X / MetresU * Cells, P.Y / MetresV * Cells,
					                 Q.X / MetresU * Cells, Q.Y / MetresV * Cells, Splits);
				}
				std::sort(Splits.begin(), Splits.end());
				double Last = T0;
				for (const double T : Splits)
				{
					if (T <= T0 + 1e-6 || T >= T1 - 1e-6 || T - Last < 1e-6) { continue; }
					AddSection(Add(A, Scale(Direction, T)), Normal, Cumulative[Index] + T * SegmentLength);
					Last = T;
				}

				// La sezione di fine: uscita dalla tile, fine della linea, o
				// angolo con il segmento successivo.
				const bool bExits = T1 < 1.0;
				const bool bLast = (Index + 2 == Feature.PointCount);
				FVec2 EndNormal = Normal;
				if (!bExits && !bLast)
				{
					const FVec2 Next = SegmentNormal(Index + 1);
					FVec2 Mitre = Add(Normal, Next);
					const double MitreLength = Length(Mitre);
					if (MitreLength > 1e-6)
					{
						Mitre = Scale(Mitre, 1.0 / MitreLength);
						const double Cosine = std::max(0.5, Dot(Mitre, Normal));
						EndNormal = Scale(Mitre, 1.0 / Cosine);
					}
				}
				AddSection(Add(A, Scale(Direction, T1)), EndNormal, Cumulative[Index] + T1 * SegmentLength);
				if (bExits) { FlushRun(); bInRun = false; }
			}
			if (bInRun) { FlushRun(); }

			if (bDrawnSomething)
			{
				++Stats.Features;
				if (bBridge) { ++Stats.Bridges; }
			}
		}

		Out.InteriorVertexCount = static_cast<uint32_t>(Out.Positions.size() / 3);
		Out.TriangleCount = static_cast<uint32_t>(Out.Indices.size() / 3);
		if (OutStats) { *OutStats = Stats; }
	}
}

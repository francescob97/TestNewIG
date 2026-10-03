// =============================================================================
//  RoadMesh.h -- Le strade in 3D: geometria vera posata sul terreno.
//  STRATO: C++ PURO, header-only.
//
//  Da lontano le strade sono dipinte sull'ortofoto (RoadRasterizer.h). Da
//  vicino, sulle tile di terreno piu' fini, diventano GEOMETRIA.
//
//  LA SECONDA VERSIONE. La prima posava un nastro largo quanto la strada, 20 cm
//  sopra il terreno e inclinato come lui. Alla prova: "manca la
//  tridimensionalita'". Aveva ragione: visto dall'alto un nastro sottile e'
//  identico a una strada dipinta, e da vicino e' un foglio di carta che
//  galleggia. Una strada vera ha uno SPESSORE e dei BORDI:
//
//    - la carreggiata e' IN PIANO di traverso, non inclinata come il pendio;
//    - ai lati scende al terreno con una SCARPATA (in rilevato) o ci sale (in
//      trincea), che e' quello che si vede di una strada di montagna;
//    - in citta' ha il CORDOLO e il MARCIAPIEDE, 15 cm piu' alto;
//    - le autostrade hanno il GUARDRAIL, i ponti i PARAPETTI e le PILE;
//    - i binari stanno sulla MASSICCIATA, un trapezio di pietrisco.
//
//                 marciapiede                          scarpata
//               ______________                  ______
//     cordolo  |              |\  carreggiata  /      \__  guardrail (autostrade)
//  ____________|              | \____________ /          \______  terreno
//
//  IL PROBLEMA VERO: STARE SUL TERRENO, NE' SOPRA NE' SOTTO
//  Il terreno che si vede non e' il DEM: e' una mesh di triangoli, con un post
//  ogni due (passo 2) e ogni cella divisa lungo la diagonale nord-ovest /
//  sud-est. Quindi:
//
//    1. le quote si prendono dalla MESH, nello stesso triangolo che il terreno
//       disegna (FSurfaceSampler, in Mesh/TileMesh.h);
//    2. la strada si spezza dove l'asse o un bordo attraversano uno spigolo
//       della mesh: fra due tagli il terreno e' un piano;
//    3. la carreggiata sta al MASSIMO del terreno sotto di lei, piu' un
//       sollevamento: non puo' esserci terreno che la buca. Dove il pendio la
//       lascia sospesa, la scarpata riempie il vuoto fino al suolo.
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
	 * bordo all'altro della superficie), la V scorre lungo la strada e si ripete
	 * ogni RoadPatternMetres.
	 *
	 * PERCHE' UN ATLANTE e non una texture per tipo: una tile di citta' ha
	 * strade di sei tipi, e sei materiali vorrebbero dire sei sezioni di mesh e
	 * sei chiamate di disegno per tile. Con l'atlante e' una.
	 *
	 * IL CANALE ALFA E' LA RUVIDITA' (0 = specchio, 255 = opaca): l'asfalto e'
	 * opaco, le strisce un po' meno, le rotaie e il guardrail sono metallo che
	 * luccica. M_GeoRoad la legge cosi'. E' la differenza fra "un colore" e "un
	 * materiale" quando il sole e' basso.
	 */
	enum class ERoadSurface : int32_t
	{
		MajorAsphalt = 0,   // asfalto con strisce bianche ai bordi e mezzeria tratteggiata
		MinorAsphalt = 1,   // asfalto senza segnaletica
		Dirt = 2,           // sterrato
		Rail = 3,           // traversine e due rotaie, sulla massicciata
		Concrete = 4,       // piste, ponti, parapetti, pile
		Paving = 5,         // zone pedonali e marciapiedi
		Gravel = 6,         // sentieri
		Verge = 7,          // scarpate: terra ed erba secca
		Steel = 8,          // guardrail
		Kerb = 9,           // cordolo in pietra
		Ballast = 10,       // fianchi della massicciata
	};

	inline constexpr int32_t RoadAtlasStrips = 16;
	inline constexpr int32_t RoadAtlasStripPixels = 64;
	/**
	 * BANDA DI GUARDIA. Ogni striscia ha 8 pixel per lato che ripetono il suo
	 * bordo, e la mesh legge solo i 48 centrali.
	 *
	 * PERCHE'. Da lontano la scheda video legge i mip piccoli dell'atlante, e il
	 * filtro bilineare prende anche il pixel ACCANTO a quello chiesto: senza
	 * margine, il bordo di una strada d'asfalto pescava il colore della
	 * striscia vicina (i binari, lo sterrato). Era uno dei motivi dello
	 * "sfrigolio": una frangia di colore sbagliato che compare e scompare a
	 * seconda di quale mip si sta leggendo. Le mipmap fatte a blocchi 2x2 su
	 * strisce larghe una potenza di due non mescolano mai due strisce; la
	 * banda di guardia copre il filtro fino al mip 3.
	 */
	inline constexpr int32_t RoadAtlasGuardPixels = 8;
	inline constexpr int32_t RoadAtlasContentPixels = RoadAtlasStripPixels - 2 * RoadAtlasGuardPixels;   // 48
	inline constexpr int32_t RoadAtlasWidth = RoadAtlasStrips * RoadAtlasStripPixels;   // 1024
	inline constexpr int32_t RoadAtlasHeight = 512;

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

		/**
		 * Quanto del pixel [X, X+1) copre la banda [Start, End): 0..1.
		 *
		 * E' l'anti-aliasing delle strisce dipinte. La prima versione le
		 * disegnava "pixel dentro o fuori", larghe un pixel: a ogni passo del
		 * filtro della scheda video una striscia larga un texel cade a cavallo
		 * di due pixel dello schermo o dentro uno solo, e lampeggia. Con la
		 * copertura il bordo e' sfumato gia' nella texture.
		 */
		inline float Coverage(float X, float Start, float End)
		{
			return std::max(0.0f, std::min(X + 1.0f, End) - std::max(X, Start));
		}

		/** Una banda che si ripete con periodo Period (lungo la V). */
		inline float PeriodicCoverage(float Y, float Period, float Start, float End)
		{
			const float Phase = std::fmod(Y, Period);
			return Coverage(Phase, Start, End) + Coverage(Phase - Period, Start, End);
		}

		struct FAtlasColour
		{
			float R = 128.0f, G = 128.0f, B = 128.0f;
			float Roughness = 0.8f;
		};

		inline FAtlasColour Mix(const FAtlasColour& A, const FAtlasColour& B, float Weight)
		{
			const float W = std::min(1.0f, std::max(0.0f, Weight));
			return FAtlasColour{ A.R + (B.R - A.R) * W, A.G + (B.G - A.G) * W, A.B + (B.B - A.B) * W,
			                     A.Roughness + (B.Roughness - A.Roughness) * W };
		}

		inline FAtlasColour Shade(FAtlasColour Colour, float Noise)
		{
			Colour.R += Noise;
			Colour.G += Noise;
			Colour.B += Noise;
			return Colour;
		}

		/**
		 * Il colore della superficie `Strip` nel punto (Column, Row): Column e'
		 * la posizione dentro la parte utile della striscia, 0..48, da un
		 * bordo all'altro della superficie.
		 */
		inline FAtlasColour AtlasSample(int32_t Strip, int32_t Column, int32_t Row)
		{
			const float X = static_cast<float>(Column);
			const float Y = static_cast<float>(Row);
			const float W = static_cast<float>(RoadAtlasContentPixels);
			const float H = static_cast<float>(RoadAtlasHeight);
			const float PerMetre = H / static_cast<float>(RoadPatternMetres);
			const float Fine = AtlasNoise(Column + Strip * 97, Row) * 6.0f;
			const float Coarse = AtlasNoise((Column + Strip * 97) / 3, Row / 3) * 10.0f;

			switch (static_cast<ERoadSurface>(Strip))
			{
			case ERoadSurface::MajorAsphalt:
			{
				// Bordi bianchi a 0,3 m dal margine, larghi ~1,3 texel (su una
				// carreggiata da 7 m: ~19 cm, come le strisce vere); mezzeria
				// tratteggiata 4,5 m su 12, con le estremita' sfumate.
				const FAtlasColour Asphalt = Shade(FAtlasColour{ 72, 72, 74, 0.82f }, Fine + Coarse * 0.4f);
				const FAtlasColour Paint{ 224, 224, 218, 0.55f };
				const float Edge = Coverage(X, 2.0f, 3.3f) + Coverage(X, W - 3.3f, W - 2.0f);
				const float Dash = Coverage(X, W * 0.5f - 0.65f, W * 0.5f + 0.65f)
				                 * PeriodicCoverage(Y, H, 0.0f, H * 4.5f / 12.0f);
				return Shade(Mix(Asphalt, Paint, Edge + Dash), 0.0f);
			}
			case ERoadSurface::MinorAsphalt:
				return Shade(FAtlasColour{ 86, 85, 84, 0.85f }, Fine + Coarse * 0.6f);
			case ERoadSurface::Dirt:
			{
				// Due solchi piu' scuri dove passano le ruote.
				const float Rut = Coverage(X, 9.0f, 13.0f) + Coverage(X, W - 13.0f, W - 9.0f);
				const FAtlasColour Ground{ 136, 120, 94, 0.95f };
				const FAtlasColour Track{ 112, 96, 70, 0.95f };
				return Shade(Mix(Ground, Track, Rut * 0.8f), Fine * 1.5f + Coarse);
			}
			case ERoadSurface::Rail:
			{
				// Massicciata, traversine ogni 60 cm (25 cm di legno o cemento),
				// due rotaie a 1,435 m (scartamento) attorno al centro di un
				// nastro da 4,5 m.
				const FAtlasColour Stones = Shade(FAtlasColour{ 118, 110, 100, 0.95f }, Fine * 2.0f + Coarse);
				const FAtlasColour Sleeper{ 104, 92, 78, 0.85f };
				const FAtlasColour Steel{ 168, 166, 162, 0.30f };
				const float Gauge = 1.435f / 4.5f * W;
				const float Centre = W * 0.5f;
				const float SleeperCover = Coverage(X, Centre - 0.36f * W, Centre + 0.36f * W)
				                         * PeriodicCoverage(Y, 0.6f * PerMetre, 0.0f, 0.25f * PerMetre);
				const float RailCover = Coverage(X, Centre - Gauge * 0.5f - 0.6f, Centre - Gauge * 0.5f + 0.6f)
				                      + Coverage(X, Centre + Gauge * 0.5f - 0.6f, Centre + Gauge * 0.5f + 0.6f);
				return Mix(Mix(Stones, Sleeper, SleeperCover), Steel, RailCover);
			}
			case ERoadSurface::Concrete:
			{
				// Lastre da 5 m con un giunto scuro.
				const float Joint = PeriodicCoverage(Y, 5.0f * PerMetre, 0.0f, 1.0f);
				return Shade(Mix(FAtlasColour{ 152, 152, 148, 0.75f }, FAtlasColour{ 110, 110, 108, 0.8f }, Joint * 0.7f),
				             Fine + Coarse * 0.3f);
			}
			case ERoadSurface::Paving:
			{
				// Autobloccanti: un reticolo di giunti ogni ~30 cm.
				const float Joint = std::min(1.0f, PeriodicCoverage(X, 6.0f, 0.0f, 0.8f)
				                                 + PeriodicCoverage(Y, 0.3f * PerMetre, 0.0f, 1.0f));
				return Shade(Mix(FAtlasColour{ 158, 152, 144, 0.75f }, FAtlasColour{ 112, 108, 102, 0.8f }, Joint * 0.6f),
				             Fine);
			}
			case ERoadSurface::Gravel:
				return Shade(FAtlasColour{ 168, 158, 140, 0.95f }, Fine * 2.0f);
			case ERoadSurface::Verge:
				// Terra ed erba secca: un colore che sta bene accanto a quasi
				// tutte le ortofoto, che sotto la scarpata continuano.
				return Shade(FAtlasColour{ 104, 104, 76, 0.95f }, Fine * 2.0f + Coarse * 1.5f);
			case ERoadSurface::Steel:
			{
				// La lama del guardrail: due onde chiare, una scura in mezzo;
				// un paletto scuro ogni 2 m.
				const float Wave = 0.5f + 0.5f * std::cos(X / W * 2.0f * 3.14159265f * 2.0f);
				const FAtlasColour Bright{ 178, 180, 182, 0.35f };
				const FAtlasColour Dark{ 112, 114, 116, 0.45f };
				const float Post = PeriodicCoverage(Y, 2.0f * PerMetre, 0.0f, 0.12f * PerMetre);
				return Mix(Mix(Dark, Bright, Wave), FAtlasColour{ 70, 72, 74, 0.5f }, Post);
			}
			case ERoadSurface::Kerb:
				return Shade(FAtlasColour{ 168, 168, 162, 0.7f }, Fine);
			case ERoadSurface::Ballast:
				return Shade(FAtlasColour{ 124, 116, 106, 0.95f }, Fine * 2.5f + Coarse);
			default:
				return FAtlasColour{ 128, 128, 128, 0.8f };
			}
		}

		inline void PutAtlasPixel(std::vector<uint8_t>& Pixels, int32_t X, int32_t Y, const FAtlasColour& Colour)
		{
			auto Clamp = [](float Value) { return static_cast<uint8_t>(std::min(255.0f, std::max(0.0f, Value + 0.5f))); };
			uint8_t* Out = Pixels.data() + (static_cast<size_t>(Y) * RoadAtlasWidth + X) * 4;
			Out[0] = Clamp(Colour.B);
			Out[1] = Clamp(Colour.G);
			Out[2] = Clamp(Colour.R);
			Out[3] = Clamp(Colour.Roughness * 255.0f);
		}
	}

	/**
	 * Disegna l'atlante: RoadAtlasWidth x RoadAtlasHeight pixel BGRA (colore
	 * sRGB, alfa = ruvidita' lineare), riga 0 = inizio del ciclo lungo la
	 * strada. Si genera a runtime (niente asset binari da mantenere) e si prova
	 * nei test, colore per colore.
	 */
	inline void BuildRoadAtlas(std::vector<uint8_t>& OutBGRA)
	{
		OutBGRA.assign(static_cast<size_t>(RoadAtlasWidth) * RoadAtlasHeight * 4, 255);
		for (int32_t Strip = 0; Strip < RoadAtlasStrips; ++Strip)
		{
			for (int32_t Y = 0; Y < RoadAtlasHeight; ++Y)
			{
				for (int32_t Column = 0; Column < RoadAtlasStripPixels; ++Column)
				{
					// La banda di guardia ripete la prima e l'ultima colonna utile.
					const int32_t Content = std::min(RoadAtlasContentPixels - 1,
						std::max(0, Column - RoadAtlasGuardPixels));
					Detail::PutAtlasPixel(OutBGRA, Strip * RoadAtlasStripPixels + Column, Y,
						Detail::AtlasSample(Strip, Content, Y));
				}
			}
		}
	}

	/** U di inizio e fine della parte utile della striscia (senza la banda di guardia). */
	inline void SurfaceUvRange(ERoadSurface Surface, float& OutU0, float& OutU1)
	{
		const float Pixel = 1.0f / static_cast<float>(RoadAtlasWidth);
		const float Start = static_cast<float>(static_cast<int32_t>(Surface) * RoadAtlasStripPixels + RoadAtlasGuardPixels);
		OutU0 = (Start + 0.5f) * Pixel;
		OutU1 = (Start + static_cast<float>(RoadAtlasContentPixels) - 0.5f) * Pixel;
	}

	// =========================================================================
	//  Quali classi diventano 3D, e come
	// =========================================================================

	/** Com'e' fatta la sezione trasversale. */
	enum class ERoadProfile : int32_t
	{
		Road,     // carreggiata, scarpate o marciapiedi
		Rail,     // massicciata
		Runway,   // piste: piatte, con una scarpata bassa
	};

	struct FRoad3DClass
	{
		bool bEnabled = false;
		ERoadSurface Surface = ERoadSurface::MinorAsphalt;
		ERoadProfile Profile = ERoadProfile::Road;
		/** Piu' alto = sta sopra agli incroci (qualche centimetro piu' in alto). */
		int32_t Tier = 0;
		/** Guardrail ai due bordi (autostrade e superstrade). */
		bool bGuardrail = false;
		/** Marciapiedi sui due lati quando OSM non dice niente. */
		bool bSidewalksByDefault = false;
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
				Result.bSidewalksByDefault = false;
			}
			return Result;
		}
	};

	inline FRoad3DStyle MakeRoad3DStyle()
	{
		FRoad3DStyle Style;
		auto Set = [&Style](ERoadClass Class, ERoadSurface Surface, int32_t Tier,
		                    ERoadProfile Profile = ERoadProfile::Road)
		{
			FRoad3DClass& Entry = Style.Classes[static_cast<int32_t>(Class)];
			Entry.bEnabled = true;
			Entry.Surface = Surface;
			Entry.Tier = Tier;
			Entry.Profile = Profile;
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
		Set(ERoadClass::Rail,         ERoadSurface::Rail,         6, ERoadProfile::Rail);
		Set(ERoadClass::LightRail,    ERoadSurface::Rail,         5, ERoadProfile::Rail);
		Set(ERoadClass::RailService,  ERoadSurface::Rail,         4, ERoadProfile::Rail);
		Set(ERoadClass::Runway,       ERoadSurface::Concrete,     2, ERoadProfile::Runway);
		Set(ERoadClass::Taxiway,      ERoadSurface::Concrete,     2, ERoadProfile::Runway);

		// I SENTIERI NO. Larghi un metro e mezzo, a un chilometro sono meno di
		// un pixel: un nastro 3D cosi' sottile appare e scompare fra un frame e
		// l'altro (lo "sfrigolio"), e dipinti sulla foto si vedono gia' bene.
		Style.Classes[static_cast<int32_t>(ERoadClass::Path)] = FRoad3DClass{};

		Style.Classes[static_cast<int32_t>(ERoadClass::Motorway)].bGuardrail = true;
		Style.Classes[static_cast<int32_t>(ERoadClass::Trunk)].bGuardrail = true;
		// In citta' le residenziali hanno quasi sempre il marciapiede, e in
		// OSM quasi mai il tag. Le altre lo hanno solo se il tag lo dice.
		Style.Classes[static_cast<int32_t>(ERoadClass::Residential)].bSidewalksByDefault = true;
		return Style;
	}

	// =========================================================================
	//  La quota della superficie DISEGNATA
	// =========================================================================

	/** La superficie disegnata: vive in Mesh/TileMesh.h, la usa anche il geomorphing. */
	using Mesh::FSurfaceSampler;

	// =========================================================================
	//  La costruzione
	// =========================================================================

	struct FRoadMeshParameters
	{
		/** Passo della mesh del terreno: DEVE essere quello usato per la tile. */
		int32_t Step = 2;
		/** Stessa convenzione del terreno (FTileMeshParameters::bFlipWinding). */
		bool bFlipWinding = true;
		/** Sollevamento della carreggiata sul punto PIU' ALTO del terreno sotto di lei, in metri. */
		double Lift = 0.15;
		/** In piu' per ogni gradino di classe: agli incroci la piu' importante sta sopra. */
		double LiftPerTier = 0.02;
		/** Spessore dell'impalcato dei ponti, in metri. */
		double BridgeDepth = 1.2;
		/** Moltiplica la larghezza vera. */
		double WidthScale = 1.0;

		/** Cordolo e marciapiede. */
		double KerbHeight = 0.15;
		double SidewalkWidth = 1.8;
		/** Scarpata: metri in orizzontale per metro di dislivello (2:3, quella delle strade vere). */
		double VergeSlope = 1.5;
		double VergeMinWidth = 0.6;
		double VergeMaxWidth = 8.0;
		/** Di quanto il piede della scarpata entra sotto il terreno: niente fessure. */
		double FootSink = 0.15;
		/** La massicciata dei binari: altezza sul terreno. */
		double BallastHeight = 0.35;
		/** Guardrail: la lama sta fra queste due quote sopra la carreggiata. */
		double GuardrailBottom = 0.40;
		double GuardrailTop = 0.75;
		/** Parapetti dei ponti. */
		double ParapetHeight = 1.0;
		double ParapetWidth = 0.3;
		/** Una pila ogni tanti metri, se l'impalcato sta abbastanza in alto. */
		double PierSpacing = 35.0;
		double PierMinClearance = 2.5;
	};

	struct FRoadMeshStats
	{
		int32_t Features = 0;
		int32_t Bridges = 0;
		int32_t CrossSections = 0;
		int32_t Triangles = 0;
		int32_t SidewalkSections = 0;
		/** Tratti di marciapiede o scarpata tolti perche' cadevano su un'altra strada (incroci). */
		int32_t JunctionCuts = 0;
		int32_t Piers = 0;
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

		/** Distanza di P dal segmento AB. */
		inline double DistanceToSegment(FVec2 P, FVec2 A, FVec2 B)
		{
			const FVec2 AB = Sub(B, A);
			const double Len2 = Dot(AB, AB);
			const double T = (Len2 > 0.0) ? std::min(1.0, std::max(0.0, Dot(Sub(P, A), AB) / Len2)) : 0.0;
			return Length(Sub(P, Add(A, Scale(AB, T))));
		}

		/**
		 * Dove stanno le carreggiate delle ALTRE strade: serve agli incroci.
		 *
		 * Un marciapiede che proseguisse dritto attraverso l'incrocio sarebbe un
		 * gradino di 15 cm in mezzo alla strada che incrocia; una scarpata,
		 * una lama di terra che spunta dall'asfalto. Prima di disegnarli si
		 * guarda se il punto cade sulla carreggiata di un'altra strada, e in
		 * quel tratto si saltano. Una griglia uniforme sulla tile basta: poche
		 * migliaia di segmenti, e la domanda e' sempre "chi c'e' qui vicino".
		 */
		class FCarriagewayIndex
		{
		public:
			void Reset(double InWidth, double InHeight, double Margin)
			{
				Segments.clear();
				OriginX = -Margin;
				OriginY = -Margin;
				CellSize = std::max(InWidth, InHeight) / 32.0 + 1e-9;
				Columns = static_cast<int32_t>(std::ceil((InWidth + 2.0 * Margin) / CellSize)) + 1;
				Rows = static_cast<int32_t>(std::ceil((InHeight + 2.0 * Margin) / CellSize)) + 1;
				Cells.assign(static_cast<size_t>(Columns) * Rows, std::vector<uint32_t>());
			}

			void Add(FVec2 A, FVec2 B, double Half, int32_t Feature, bool bBridge)
			{
				const uint32_t Index = static_cast<uint32_t>(Segments.size());
				Segments.push_back(FSegment{ A, B, Half, Feature, bBridge });
				const int32_t X0 = CellX(std::min(A.X, B.X) - Half), X1 = CellX(std::max(A.X, B.X) + Half);
				const int32_t Y0 = CellY(std::min(A.Y, B.Y) - Half), Y1 = CellY(std::max(A.Y, B.Y) + Half);
				for (int32_t Y = Y0; Y <= Y1; ++Y)
				{
					for (int32_t X = X0; X <= X1; ++X) { Cells[static_cast<size_t>(Y) * Columns + X].push_back(Index); }
				}
			}

			/** P cade sulla carreggiata di una strada diversa da Feature (e allo stesso livello)? */
			bool IsOnOtherCarriageway(FVec2 P, int32_t Feature, bool bBridge) const
			{
				if (Cells.empty()) { return false; }
				for (const uint32_t Index : Cells[static_cast<size_t>(CellY(P.Y)) * Columns + CellX(P.X)])
				{
					const FSegment& Segment = Segments[Index];
					// Un ponte passa SOPRA: non taglia il marciapiede di chi sta sotto.
					if (Segment.Feature == Feature || Segment.bBridge != bBridge) { continue; }
					if (DistanceToSegment(P, Segment.A, Segment.B) < Segment.Half) { return true; }
				}
				return false;
			}

			/**
			 * Dove il segmento AB incrocia (o tocca, come in un incrocio a T) la
			 * carreggiata di un'altra strada: per ognuna il parametro t lungo AB
			 * e la mezza larghezza dell'altra misurata LUNGO AB (si allunga
			 * quanto piu' l'incrocio e' obliquo).
			 *
			 * Serve a mettere sezioni fitte proprio li': le sezioni normali
			 * cadono dove la strada attraversa gli spigoli della mesh, ogni 5-10
			 * m, e una via larga 5 m potrebbe stare tutta fra due di loro. Il
			 * marciapiede la scavalcherebbe senza che nessuna sezione se ne
			 * accorga.
			 */
			void CollectCrossings(FVec2 A, FVec2 B, int32_t Feature, bool bBridge,
			                      std::vector<std::pair<double, double>>& Out) const
			{
				Out.clear();
				if (Cells.empty()) { return; }
				const FVec2 AB = Sub(B, A);
				const double Len = Length(AB);
				if (Len <= 1e-9) { return; }

				++Stamp;
				if (Visited.size() != Segments.size()) { Visited.assign(Segments.size(), 0); }

				const int32_t X0 = CellX(std::min(A.X, B.X)), X1 = CellX(std::max(A.X, B.X));
				const int32_t Y0 = CellY(std::min(A.Y, B.Y)), Y1 = CellY(std::max(A.Y, B.Y));
				for (int32_t Y = Y0; Y <= Y1; ++Y)
				{
					for (int32_t X = X0; X <= X1; ++X)
					{
						for (const uint32_t Index : Cells[static_cast<size_t>(Y) * Columns + X])
						{
							if (Visited[Index] == Stamp) { continue; }
							Visited[Index] = Stamp;
							const FSegment& Segment = Segments[Index];
							if (Segment.Feature == Feature || Segment.bBridge != bBridge) { continue; }

							const FVec2 PQ = Sub(Segment.B, Segment.A);
							const double PQLen = Length(PQ);
							const double Denominator = AB.X * PQ.Y - AB.Y * PQ.X;
							if (PQLen <= 1e-9 || std::fabs(Denominator) < 1e-9 * Len * PQLen) { continue; }   // parallele

							const FVec2 AP = Sub(Segment.A, A);
							const double T = (AP.X * PQ.Y - AP.Y * PQ.X) / Denominator;
							const double U = (AP.X * AB.Y - AP.Y * AB.X) / Denominator;
							// Un metro di tolleranza alle estremita': gli incroci a T.
							if (T < -1.0 / Len || T > 1.0 + 1.0 / Len || U < -1.0 / PQLen || U > 1.0 + 1.0 / PQLen) { continue; }

							const double Sine = std::fabs(Denominator) / (Len * PQLen);
							// Sotto i 30 gradi si tiene fermo il valore: un incrocio cosi'
							// radente e' quasi un affiancamento, e l'allungamento
							// riempirebbe di sezioni un tratto lungo.
							Out.emplace_back(T, Segment.Half / std::max(0.5, Sine));
						}
					}
				}
			}

		private:
			struct FSegment
			{
				FVec2 A, B;
				double Half = 0.0;
				int32_t Feature = -1;
				bool bBridge = false;
			};

			int32_t CellX(double X) const
			{
				return std::min(Columns - 1, std::max(0, static_cast<int32_t>(std::floor((X - OriginX) / CellSize))));
			}
			int32_t CellY(double Y) const
			{
				return std::min(Rows - 1, std::max(0, static_cast<int32_t>(std::floor((Y - OriginY) / CellSize))));
			}

			std::vector<FSegment> Segments;
			std::vector<std::vector<uint32_t>> Cells;
			mutable std::vector<uint32_t> Visited;
			mutable uint32_t Stamp = 0;
			double OriginX = 0.0, OriginY = 0.0, CellSize = 1.0;
			int32_t Columns = 0, Rows = 0;
		};
	}

	/**
	 * Costruisce le strade 3D di UNA tile di terreno.
	 *
	 * `Surface` e' la superficie della tile (stesso passo della sua mesh).
	 * `Wide`, facoltativa, e' la superficie di un antenato che copre tutta la
	 * tile vettoriale: serve alle estremita' dei ponti, che possono cadere fuori
	 * dalla tile di terreno. Senza, si usa `Surface` prolungata.
	 * `Parent`, facoltativa, e' la superficie del PADRE: con lei si scrivono i
	 * dati del geomorphing (MorphDeltas), cosi' le strade scivolano insieme al
	 * terreno quando la tile si affina (vedi ComputeMorphTargets).
	 *
	 * Il risultato e' nel frame locale della tile, con la STESSA origine di
	 * BuildTileMesh (centro della tile, a meta' fra le quote estreme).
	 */
	inline void BuildRoadMesh(const FHeightTile& TerrainTile, const FSurfaceSampler& Surface,
	                          const FSurfaceSampler* Wide, const FVectorTile& Vectors,
	                          const FVectorWindow& Window, const FRoad3DStyle& Style,
	                          const FRoadMeshParameters& Parameters, FTileMeshData& Out,
	                          FRoadMeshStats* OutStats = nullptr,
	                          const FSurfaceSampler* Parent = nullptr,
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
		const bool bMorph = Parent && Parent->IsValid();

		// La stessa origine di BuildTileMesh: il componente della strada usa la
		// trasformazione della tile di terreno.
		Out.Origin = FGeodetic::FromDegrees(Bounds.CentreLat(), Bounds.CentreLon(),
		                                    0.5 * (TerrainTile.MinHeight + TerrainTile.MaxHeight));
		Out.MinHeight = TerrainTile.MinHeight;
		Out.MaxHeight = TerrainTile.MaxHeight;
		const FEcef OriginEcef = Core::GeodeticToEcef(Out.Origin, Ellipsoid);
		const FMat3 EcefToLocal = Mesh::Detail::MakeLocalNeu(Out.Origin);

		// Metri per unita' di frazione della tile, in est (x) e in sud (y).
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

		// Di quanto la superficie di questa tile sta sopra quella del padre:
		// il materiale riporta la strada li' quando la tile nasce (Morph = 1).
		auto MorphDeltaAt = [&](FVec2 Metric)
		{
			if (!bMorph) { return 0.0; }
			double Lon = 0.0, Lat = 0.0;
			ToLonLat(Metric, Lon, Lat);
			return Surface.HeightAt(Lon, Lat) - Parent->HeightAt(Lon, Lat);
		};

		auto LocalAt = [&](FVec2 Metric, double Height)
		{
			double Lon = 0.0, Lat = 0.0;
			ToLonLat(Metric, Lon, Lat);
			const FEcef Ecef = Core::GeodeticToEcef(
				FGeodetic::FromRadians(Lat * Core::DegToRad, Lon * Core::DegToRad, Height), Ellipsoid);
			return EcefToLocal.Transform(Ecef - OriginEcef);
		};

		// Una direzione orizzontale del sistema metrico (x est, y sud) nel frame
		// locale NEU (X nord, Y est, Z alto).
		auto LocalDirection = [](FVec2 Metric) { return FEcef{ -Metric.Y, Metric.X, 0.0 }; };

		// ------------------------------------------------------------------
		//  Vertici, quadrilateri
		// ------------------------------------------------------------------
		auto Cross = [](const FEcef& A, const FEcef& B)
		{
			return FEcef{ A.Y * B.Z - A.Z * B.Y, A.Z * B.X - A.X * B.Z, A.X * B.Y - A.Y * B.X };
		};
		auto DotE = [](const FEcef& A, const FEcef& B) { return A.X * B.X + A.Y * B.Y + A.Z * B.Z; };

		/** Un angolo di quadrilatero: dove sta (sistema metrico) e a che quota. */
		struct FCorner
		{
			FVec2 Metric;
			double Height = 0.0;
			/** I ponti e le pile non seguono il terreno: non scivolano col morphing. */
			bool bFixed = false;
		};

		// Ogni quadrilatero ha i suoi quattro vertici, con la sua normale. Costa
		// il doppio dei vertici di una striscia condivisa, ma ogni faccia ha la
		// normale giusta per costruzione (spigoli vivi fra cordolo e
		// marciapiede, fra parapetto e impalcato), che e' quello che fa leggere
		// la forma.
		//
		// L'orientamento si decide TRIANGOLO PER TRIANGOLO: nelle curve strette
		// il lato interno di due sezioni consecutive puo' incrociarsi, e il
		// quadrilatero diventa un "papillon" con i due triangoli di verso
		// opposto. Deciderli insieme ne lasciava uno rovesciato (invisibile).
		// `Wanted` nullo = faccia "di sopra": la normale e' quella geometrica,
		// girata verso l'alto.
		auto AddQuad = [&](const FCorner (&Corners)[4], FEcef Wanted, const float (&U)[4], const float (&V)[4])
		{
			FEcef Local[4];
			for (int Corner = 0; Corner < 4; ++Corner) { Local[Corner] = LocalAt(Corners[Corner].Metric, Corners[Corner].Height); }

			const FEcef Geometric = Cross(Local[1] - Local[0], Local[2] - Local[0])
			                      + Cross(Local[2] - Local[0], Local[3] - Local[0]);
			const double GeometricLength = std::sqrt(Geometric.LengthSquared());
			if (GeometricLength < 1e-9) { return; }
			const double WantedLength = std::sqrt(Wanted.LengthSquared());
			if (WantedLength < 1e-9)
			{
				Wanted = FEcef{ Geometric.X / GeometricLength, Geometric.Y / GeometricLength, Geometric.Z / GeometricLength };
				if (Wanted.Z < 0.0) { Wanted = FEcef{ -Wanted.X, -Wanted.Y, -Wanted.Z }; }
			}
			else
			{
				Wanted = FEcef{ Wanted.X / WantedLength, Wanted.Y / WantedLength, Wanted.Z / WantedLength };
			}

			uint32_t Index[4];
			for (int Corner = 0; Corner < 4; ++Corner)
			{
				Out.Positions.push_back(static_cast<float>(Local[Corner].X));
				Out.Positions.push_back(static_cast<float>(Local[Corner].Y));
				Out.Positions.push_back(static_cast<float>(Local[Corner].Z));
				Out.Normals.push_back(static_cast<float>(Wanted.X));
				Out.Normals.push_back(static_cast<float>(Wanted.Y));
				Out.Normals.push_back(static_cast<float>(Wanted.Z));
				Out.UVs.push_back(U[Corner]);
				Out.UVs.push_back(V[Corner]);
				if (bMorph)
				{
					Out.MorphDeltas.push_back(Corners[Corner].bFixed
						? 0.0f : static_cast<float>(MorphDeltaAt(Corners[Corner].Metric)));
				}
				Index[Corner] = static_cast<uint32_t>(Out.Positions.size() / 3 - 1);
			}

			auto Triangle = [&](int A, int B, int C)
			{
				const FEcef Normal = Cross(Local[B] - Local[A], Local[C] - Local[A]);
				if (Normal.LengthSquared() < 1e-18) { return; }   // degenere: non si disegna
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

		// ------------------------------------------------------------------
		//  Le linee in metri, e l'indice delle carreggiate per gli incroci
		// ------------------------------------------------------------------
		const double TileMargin = 40.0;
		auto NearTile = [&](const FVectorFeature& Feature, double Reach)
		{
			const double MarginU = (Reach + TileMargin) / MetresU * Window.Span;
			const double MarginV = (Reach + TileMargin) / MetresV * Window.Span;
			return !(Feature.MaxX < Window.U0 - MarginU || Feature.MinX > Window.U0 + Window.Span + MarginU ||
			         Feature.MaxY < Window.V0 - MarginV || Feature.MinY > Window.V0 + Window.Span + MarginV);
		};
		auto PointMetric = [&](const Tiles::FVectorPoint& Point)
		{
			const double U = (static_cast<double>(Point.X) - Window.U0) / Window.Span;
			const double V = (static_cast<double>(Point.Y) - Window.V0) / Window.Span;
			return FVec2{ U * MetresU, V * MetresV };
		};

		FCarriagewayIndex Carriageways;
		Carriageways.Reset(MetresU, MetresV, TileMargin);
		for (size_t FeatureIndex = 0; FeatureIndex < Vectors.Features.size(); ++FeatureIndex)
		{
			const FVectorFeature& Feature = Vectors.Features[FeatureIndex];
			if (!Style.For(Feature).bEnabled || Feature.PointCount < 2) { continue; }
			const double Half = 0.5 * Feature.GetWidthMetres() * Parameters.WidthScale;
			if (!NearTile(Feature, Half)) { continue; }
			for (uint32_t Index = 0; Index + 1 < Feature.PointCount; ++Index)
			{
				Carriageways.Add(PointMetric(Vectors.Points[Feature.FirstPoint + Index]),
				                 PointMetric(Vectors.Points[Feature.FirstPoint + Index + 1]),
				                 Half, static_cast<int32_t>(FeatureIndex), Feature.IsBridge());
			}
		}

		// ------------------------------------------------------------------
		//  Le sezioni trasversali
		// ------------------------------------------------------------------

		/** Un lato della sezione: i punti del profilo dal bordo della carreggiata verso fuori. */
		struct FSide
		{
			FCorner Points[4];
			int32_t Count = 0;
			/** Falso dove il lato cade sulla carreggiata di un'altra strada (incroci). */
			bool bOpen = true;
		};

		struct FSection
		{
			FVec2 Centre;
			/** Verso DESTRA della linea (nel suo verso), allungata negli angoli (mitra). */
			FVec2 Normal;
			double Along = 0.0;          // metri dall'inizio della linea
			double Deck = 0.0;           // quota della carreggiata, in piano di traverso
			FSide Sides[2];              // 0 = destra, 1 = sinistra
		};

		std::vector<FVec2> Points;
		std::vector<double> Cumulative;
		std::vector<double> Splits;
		std::vector<std::pair<double, double>> Crossings;
		std::vector<FSection> Sections;

		for (size_t FeatureIndex = 0; FeatureIndex < Vectors.Features.size(); ++FeatureIndex)
		{
			const FVectorFeature& Feature = Vectors.Features[FeatureIndex];
			const FRoad3DClass Class = Style.For(Feature);
			if (!Class.bEnabled || Feature.PointCount < 2) { continue; }

			const double Half = 0.5 * Feature.GetWidthMetres() * Parameters.WidthScale;
			if (!NearTile(Feature, Half + Parameters.SidewalkWidth + Parameters.VergeMaxWidth)) { continue; }

			// La linea in metri, nel sistema della tile di terreno (origine
			// nell'angolo nord-ovest, x verso est, y verso sud).
			Points.resize(Feature.PointCount);
			Cumulative.resize(Feature.PointCount);
			for (uint32_t Index = 0; Index < Feature.PointCount; ++Index)
			{
				Points[Index] = PointMetric(Vectors.Points[Feature.FirstPoint + Index]);
				Cumulative[Index] = (Index == 0) ? 0.0 : Cumulative[Index - 1] + Length(Sub(Points[Index], Points[Index - 1]));
			}
			const double TotalLength = Cumulative.back();
			if (TotalLength <= 0.0) { continue; }

			const bool bBridge = Feature.IsBridge();
			const int32_t Self = static_cast<int32_t>(FeatureIndex);

			// Il sollevamento: per classe (agli incroci la piu' importante sta
			// sopra), piu' qualche millimetro per linea. Due strade della stessa
			// classe che si incrociano non devono stare alla STESSA quota: il
			// renderer non saprebbe quale disegnare, e sceglierebbe pixel per
			// pixel in modo diverso a ogni frame (z-fighting: lo sfrigolio).
			const double Lift = Parameters.Lift + Parameters.LiftPerTier * Class.Tier
			                  + 0.002 * static_cast<double>(FeatureIndex % 25)
			                  + (Class.Profile == ERoadProfile::Rail ? Parameters.BallastHeight : 0.0);

			// I marciapiedi. OSM dice "sinistra" e "destra" nel verso della
			// linea; qui il lato 0 e' la destra, il lato 1 la sinistra.
			bool bSidewalk[2] = { false, false };
			if (!bBridge && Class.Profile == ERoadProfile::Road && Class.Surface != ERoadSurface::Dirt)
			{
				if (Feature.IsSidewalkKnown())
				{
					bSidewalk[0] = Feature.HasSidewalkRight();
					bSidewalk[1] = Feature.HasSidewalkLeft();
				}
				else
				{
					bSidewalk[0] = bSidewalk[1] = Class.bSidewalksByDefault;
				}
			}
			const bool bGuardrail = Class.bGuardrail && !bBridge;
			const ERoadSurface SideSurface = (Class.Profile == ERoadProfile::Rail) ? ERoadSurface::Ballast : ERoadSurface::Verge;

			// Il ponte: un impalcato dritto fra le due spalle, non un nastro
			// posato sul fondo della valle. Le quote delle spalle si prendono
			// dove il ponte comincia e finisce (anche fuori dalla tile: per
			// questo c'e' Wide).
			const double BridgeStart = bBridge ? WideAt(Points.front()) : 0.0;
			const double BridgeEnd = bBridge ? WideAt(Points.back()) : 0.0;

			// La quota della carreggiata in una sezione: il PUNTO PIU' ALTO del
			// terreno sotto di lei (e sotto i marciapiedi, che stanno un
			// cordolo piu' su), piu' il sollevamento.
			auto DeckAt = [&](FVec2 Centre, FVec2 Normal, double Along)
			{
				double Highest = TerrainAt(Centre);
				for (const double Offset : { -Half, -0.5 * Half, 0.5 * Half, Half })
				{
					Highest = std::max(Highest, TerrainAt(Add(Centre, Scale(Normal, Offset))));
				}
				for (int Side = 0; Side < 2; ++Side)
				{
					if (!bSidewalk[Side]) { continue; }
					const double Sign = (Side == 0) ? 1.0 : -1.0;
					for (const double Offset : { Half + 0.5 * Parameters.SidewalkWidth, Half + Parameters.SidewalkWidth })
					{
						Highest = std::max(Highest,
							TerrainAt(Add(Centre, Scale(Normal, Sign * Offset))) - Parameters.KerbHeight);
					}
				}
				if (bBridge)
				{
					// Impalcato lineare fra le spalle, mai sotto il terreno.
					const double Fraction = std::min(1.0, std::max(0.0, Along / TotalLength));
					Highest = std::max(Highest, BridgeStart + (BridgeEnd - BridgeStart) * Fraction);
				}
				return Highest + Lift;
			};

			float U0 = 0.0f, U1 = 1.0f;
			SurfaceUvRange(Class.Surface, U0, U1);

			bool bDrawnSomething = false;

			auto AddSection = [&](FVec2 Centre, FVec2 Normal, double Along)
			{
				FSection Section;
				Section.Centre = Centre;
				Section.Normal = Normal;
				Section.Along = Along;
				Section.Deck = DeckAt(Centre, Normal, Along);

				for (int Side = 0; Side < 2; ++Side)
				{
					const double Sign = (Side == 0) ? 1.0 : -1.0;
					auto At = [&](double Offset) { return Add(Centre, Scale(Normal, Sign * Offset)); };
					FSide& Profile = Section.Sides[Side];
					const double Deck = Section.Deck;

					if (bBridge)
					{
						// Parapetto: faccia interna, cima, faccia esterna fino al
						// fondo dell'impalcato.
						const double Outer = Half + Parameters.ParapetWidth;
						Profile.Points[0] = FCorner{ At(Half), Deck, true };
						Profile.Points[1] = FCorner{ At(Half), Deck + Parameters.ParapetHeight, true };
						Profile.Points[2] = FCorner{ At(Outer), Deck + Parameters.ParapetHeight, true };
						Profile.Points[3] = FCorner{ At(Outer), Deck - Parameters.BridgeDepth, true };
						Profile.Count = 4;
						continue;
					}

					if (bSidewalk[Side])
					{
						// Cordolo, marciapiede, e un piccolo scalino fino al suolo.
						const double Outer = Half + Parameters.SidewalkWidth;
						const double Foot = Outer + 0.3;
						Profile.Points[0] = FCorner{ At(Half), Deck };
						Profile.Points[1] = FCorner{ At(Half), Deck + Parameters.KerbHeight };
						Profile.Points[2] = FCorner{ At(Outer), Deck + Parameters.KerbHeight };
						Profile.Points[3] = FCorner{ At(Foot), TerrainAt(At(Foot)) - Parameters.FootSink };
						Profile.Count = 4;
						Profile.bOpen = !Carriageways.IsOnOtherCarriageway(At(Half + 0.5 * Parameters.SidewalkWidth), Self, false);
						continue;
					}

					// Scarpata: dal bordo della carreggiata al terreno, con la
					// pendenza delle strade vere. In rilevato scende, in trincea sale.
					const double Probe = Half + Parameters.VergeMinWidth;
					const double Drop = std::fabs(Deck - TerrainAt(At(Probe)));
					const double Reach = std::min(Parameters.VergeMaxWidth,
						std::max(Parameters.VergeMinWidth, Parameters.VergeSlope * Drop));
					const FVec2 Foot = At(Half + Reach);
					Profile.Points[0] = FCorner{ At(Half), Deck };
					Profile.Points[1] = FCorner{ Foot, TerrainAt(Foot) - Parameters.FootSink };
					Profile.Count = 2;
					Profile.bOpen = !Carriageways.IsOnOtherCarriageway(At(Half + 0.5 * Reach), Self, false);
				}
				Sections.push_back(Section);
			};

			// Un "tratto" e' una sequenza di sezioni consecutive dentro la tile.
			auto FlushRun = [&]()
			{
				for (size_t Index = 0; Index + 1 < Sections.size(); ++Index)
				{
					const FSection& A = Sections[Index];
					const FSection& B = Sections[Index + 1];
					const float VA = static_cast<float>(A.Along / RoadPatternMetres);
					const float VB = static_cast<float>(B.Along / RoadPatternMetres);
					const float Vs[4] = { VA, VA, VB, VB };

					// La carreggiata, in piano di traverso.
					{
						const FCorner Corners[4] = {
							FCorner{ Add(A.Centre, Scale(A.Normal, Half)), A.Deck, bBridge },
							FCorner{ Sub(A.Centre, Scale(A.Normal, Half)), A.Deck, bBridge },
							FCorner{ Sub(B.Centre, Scale(B.Normal, Half)), B.Deck, bBridge },
							FCorner{ Add(B.Centre, Scale(B.Normal, Half)), B.Deck, bBridge } };
						const float Us[4] = { U0, U1, U1, U0 };
						AddQuad(Corners, FEcef{ 0.0, 0.0, 0.0 }, Us, Vs);
					}

					// I due lati: un quadrilatero per ogni tratto del profilo.
					for (int Side = 0; Side < 2; ++Side)
					{
						const FSide& SA = A.Sides[Side];
						const FSide& SB = B.Sides[Side];
						if (SA.Count != SB.Count || SA.Count < 2) { continue; }
						if (!SA.bOpen || !SB.bOpen) { ++Stats.JunctionCuts; continue; }

						const double Sign = (Side == 0) ? 1.0 : -1.0;
						const FEcef Outward = LocalDirection(Scale(Add(A.Normal, B.Normal), Sign));
						const FEcef Inward{ -Outward.X, -Outward.Y, 0.0 };

						for (int Band = 0; Band + 1 < SA.Count; ++Band)
						{
							// Che superficie e che verso ha ogni tratto.
							ERoadSurface BandSurface = SideSurface;
							FEcef Wanted{ 0.0, 0.0, 0.0 };
							if (bBridge)
							{
								BandSurface = ERoadSurface::Concrete;
								Wanted = (Band == 0) ? Inward : (Band == 2) ? Outward : FEcef{ 0.0, 0.0, 0.0 };
							}
							else if (SA.Count == 4)
							{
								BandSurface = (Band == 1) ? ERoadSurface::Paving : ERoadSurface::Kerb;
								if (Band == 0) { Wanted = Inward; }
							}

							float SU0 = 0.0f, SU1 = 1.0f;
							SurfaceUvRange(BandSurface, SU0, SU1);
							const FCorner Corners[4] = { SA.Points[Band], SA.Points[Band + 1], SB.Points[Band + 1], SB.Points[Band] };
							const float Us[4] = { SU0, SU1, SU1, SU0 };
							AddQuad(Corners, Wanted, Us, Vs);
						}
						if (SA.Count == 4 && !bBridge) { ++Stats.SidewalkSections; }

						// Il guardrail: una lama al bordo della carreggiata,
						// visibile dai due lati.
						if (bGuardrail)
						{
							float GU0 = 0.0f, GU1 = 1.0f;
							SurfaceUvRange(ERoadSurface::Steel, GU0, GU1);
							const FVec2 EdgeA = SA.Points[0].Metric;
							const FVec2 EdgeB = SB.Points[0].Metric;
							const FCorner Rail[4] = {
								FCorner{ EdgeA, A.Deck + Parameters.GuardrailBottom }, FCorner{ EdgeA, A.Deck + Parameters.GuardrailTop },
								FCorner{ EdgeB, B.Deck + Parameters.GuardrailTop }, FCorner{ EdgeB, B.Deck + Parameters.GuardrailBottom } };
							const float Us[4] = { GU0, GU1, GU1, GU0 };
							AddQuad(Rail, Inward, Us, Vs);
							AddQuad(Rail, Outward, Us, Vs);
						}
					}

					// Il fondo dell'impalcato: senza, da sotto il ponte sarebbe un foglio.
					if (bBridge)
					{
						float CU0 = 0.0f, CU1 = 1.0f;
						SurfaceUvRange(ERoadSurface::Concrete, CU0, CU1);
						const FCorner Bottom[4] = { A.Sides[0].Points[3], A.Sides[1].Points[3], B.Sides[1].Points[3], B.Sides[0].Points[3] };
						const float Us[4] = { CU0, CU1, CU1, CU0 };
						AddQuad(Bottom, FEcef{ 0.0, 0.0, -1.0 }, Us, Vs);
					}
					bDrawnSomething = true;
				}
				Stats.CrossSections += static_cast<int32_t>(Sections.size());
				Sections.clear();
			};

			// Le direzioni dei segmenti, per le sezioni d'angolo (mitra).
			auto SegmentNormal = [&](uint32_t Index)
			{
				const FVec2 Direction = Sub(Points[Index + 1], Points[Index]);
				const double Len = Length(Direction);
				if (Len <= 1e-9) { return FVec2{ 0.0, 1.0 }; }
				return FVec2{ -Direction.Y / Len, Direction.X / Len };
			};

			Sections.clear();
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

				// E sezioni fitte attorno agli incroci, dove marciapiedi e
				// scarpate devono interrompersi (vedi CollectCrossings): ogni
				// metro e mezzo, che e' anche la fessura massima che resta.
				Carriageways.CollectCrossings(A, B, Self, bBridge, Crossings);
				for (const std::pair<double, double>& Crossing : Crossings)
				{
					const double Reach = Crossing.second + Parameters.SidewalkWidth + 0.5;
					for (double Offset = -Reach; Offset <= Reach; Offset += 1.5)
					{
						const double T = Crossing.first + Offset / SegmentLength;
						if (T > T0 && T < T1) { Splits.push_back(T); }
					}
				}

				std::sort(Splits.begin(), Splits.end());
				double Last = T0;
				for (const double T : Splits)
				{
					// Due tagli a meno di 2 cm l'uno dall'altro sono lo stesso taglio.
					if (T <= T0 + 1e-6 || T >= T1 - 1e-6 || (T - Last) * SegmentLength < 0.02) { continue; }
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

			// Le pile del ponte: una ogni PierSpacing metri, dove l'impalcato
			// sta abbastanza in alto. La costruisce solo la tile che ne contiene
			// il centro, cosi' una pila sul confine non compare due volte.
			if (bBridge && TotalLength > Parameters.PierSpacing)
			{
				float CU0 = 0.0f, CU1 = 1.0f;
				SurfaceUvRange(ERoadSurface::Concrete, CU0, CU1);
				uint32_t Segment = 0;
				for (double Along = Parameters.PierSpacing; Along < TotalLength - 0.5 * Parameters.PierSpacing;
				     Along += Parameters.PierSpacing)
				{
					while (Segment + 2 < Feature.PointCount && Cumulative[Segment + 1] < Along) { ++Segment; }
					const double SegmentLength = Cumulative[Segment + 1] - Cumulative[Segment];
					if (SegmentLength <= 1e-6) { continue; }
					const FVec2 Direction = Scale(Sub(Points[Segment + 1], Points[Segment]), 1.0 / SegmentLength);
					const FVec2 Centre = Add(Points[Segment], Scale(Direction, Along - Cumulative[Segment]));
					if (Centre.X < 0.0 || Centre.X >= MetresU || Centre.Y < 0.0 || Centre.Y >= MetresV) { continue; }

					const FVec2 Normal{ -Direction.Y, Direction.X };
					const double Top = DeckAt(Centre, Normal, Along) - Parameters.BridgeDepth;
					const double Ground = TerrainAt(Centre);
					if (Top - Ground < Parameters.PierMinClearance) { continue; }

					// Una scatola: 1,6 m lungo il ponte, 60% della larghezza di traverso.
					const FVec2 L = Scale(Direction, 0.8);
					const FVec2 W = Scale(Normal, std::min(3.0, 0.6 * Half));
					const FVec2 Box[4] = { Add(Add(Centre, L), W), Sub(Add(Centre, L), W),
					                       Sub(Sub(Centre, L), W), Add(Sub(Centre, L), W) };
					const float PierV[4] = { 0.0f, 0.0f, static_cast<float>((Top - Ground) / RoadPatternMetres),
					                         static_cast<float>((Top - Ground) / RoadPatternMetres) };
					for (int Face = 0; Face < 4; ++Face)
					{
						const FVec2 P = Box[Face];
						const FVec2 Q = Box[(Face + 1) % 4];
						const FCorner Corners[4] = { FCorner{ P, Ground - 0.5 }, FCorner{ Q, Ground - 0.5 },
						                             FCorner{ Q, Top, true }, FCorner{ P, Top, true } };
						const float Us[4] = { CU0, CU1, CU1, CU0 };
						const FVec2 Middle = Scale(Add(P, Q), 0.5);
						AddQuad(Corners, LocalDirection(Sub(Middle, Centre)), Us, PierV);
					}
					++Stats.Piers;
				}
			}

			if (bDrawnSomething)
			{
				++Stats.Features;
				if (bBridge) { ++Stats.Bridges; }
			}
		}

		// Le strade non cambiano luce col morphing: la "normale del padre" e'
		// la loro. Scivolano solo in quota, con il terreno sotto.
		if (bMorph) { Out.ParentNormals = Out.Normals; }

		Out.InteriorVertexCount = static_cast<uint32_t>(Out.Positions.size() / 3);
		Out.TriangleCount = static_cast<uint32_t>(Out.Indices.size() / 3);
		if (OutStats) { *OutStats = Stats; }
	}
}

// =============================================================================
//  StreetLights.h -- I lampioni delle strade. STRATO: C++ PURO, header-only.
//
//  Di notte una citta' vista dall'alto e' fatta di file di punti arancioni e
//  bianchi lungo le strade; da vicino, di pozze di luce sull'asfalto. Qui c'e'
//  tutto quello che serve a metterli, senza Unreal (provato in
//  georoads_main.cpp, sezione "lampioni"):
//
//   1. QUALI STRADE. Il tag `lit` di OSM, se c'e' (la pipeline lo mette nel
//      record della linea: RoadLit*). Quasi sempre manca, e allora decide la
//      classe insieme a QUANTO E' ABITATO attorno: la lunghezza delle strade
//      residenziali, pedonali e "living street" entro ~250 m, in km per km²
//      (FUrbanDensity). In un paese sono 5-20, in campagna meno di 1. Le
//      residenziali hanno i lampioni quasi sempre (anche le frazioni), le
//      principali solo dentro l'abitato, le autostrade mai (solo le rampe in
//      citta'), sentieri e sterrati solo se il tag lo dice.
//   2. DOVE. Lungo la strada, a passo fisso (circa 3,5 volte l'altezza del
//      palo: 26 m in una via di quartiere, 34 su una provinciale in paese).
//      Le way di OSM sono spesso a pezzi (un incrocio, un cambio di nome):
//      prima si RICUCIONO le linee che si toccano solo fra loro due (FChain),
//      poi si cammina. Il primo lampione sta a mezzo passo dall'inizio, cosi'
//      fra due tratti (o due tile) c'e' sempre fra mezzo e un passo e mezzo.
//      Ogni tile mette solo i lampioni dei tratti DENTRO di se' (le linee
//      escono di un buffer): nessun lampione doppio sul confine.
//      Un lato solo sulle strade strette, alternati su quelle medie, a coppie
//      su quelle larghe; il palo sul marciapiede (o sul ciglio), lo sbraccio
//      verso la strada. Mai in mezzo a un'altra strada (agli incroci).
//   3. COME. Tre tipi: palo alto con lo sbraccio (le principali), palo medio
//      (i quartieri), lanterna (le zone pedonali). Sodio arancione o LED: un
//      COMUNE (una cella di ~2 km) e' passato ai LED o no, tre su quattro si';
//      sulle principali LED neutro, nei quartieri caldo.
//   4. QUANDO. Una fotocellula per CIRCUITO (una cella di ~300 m: una via si
//      accende tutta insieme), con la sua soglia fra 15 e 60 lux: si
//      accendono nei minuti dopo il tramonto, prima se e' nuvolo. La mattina
//      si spengono a una luce un po' piu' alta (l'isteresi delle fotocellule).
//      Nel cuore della notte molti comuni riducono il flusso (NightDimming).
//   5. QUANTA LUCE. Non quella vera: come la luna (Sky/DayLight.h) anche i
//      lampioni sono in proporzione al sole del livello. Sotto un palo ~1/50
//      del sole del livello: venti volte la luna piena.
//
//  Le forme dei pali (BuildLampMesh) e il quadrato dei punti luce lontani
//  (BuildSpriteQuad) sono mesh fatte da codice, nel formato degli alberi
//  (Vegetation/TreeMesh.h): Unreal le traduce con la stessa funzione.
// =============================================================================
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

#include "Mesh/GroundCells.h"
#include "Sky/DayLight.h"
#include "Tiles/TileKey.h"
#include "Tiles/TilingScheme.h"
#include "Tiles/VectorTileFormat.h"
#include "Vegetation/TreeMesh.h"

namespace GeoWorld::StreetLights
{
	using Tiles::ERoadClass;
	using Tiles::FVectorFeature;
	using Tiles::FVectorTile;

	// =========================================================================
	//  Tipi, colori, disposizioni
	// =========================================================================

	enum class ELampKind : uint8_t
	{
		/** Palo alto con lo sbraccio sopra la carreggiata: le strade principali. */
		Arm = 0,
		/** Palo medio con la testa corta: le vie di quartiere. */
		Post = 1,
		/** Lanterna bassa: zone pedonali, vicoli, sentieri illuminati. */
		Lantern = 2,
	};
	inline constexpr int32_t LampKindCount = 3;

	enum class ELampColour : uint8_t
	{
		/** Sodio ad alta pressione: l'arancione delle citta' di una volta. */
		Sodium = 0,
		/** LED caldo, ~3000 K: i quartieri e i centri storici. */
		WarmLed = 1,
		/** LED neutro, ~4000 K: le strade principali. */
		NeutralLed = 2,
	};
	inline constexpr int32_t LampColourCount = 3;

	enum class ELampLayout : uint8_t
	{
		/** Tutti dallo stesso lato (la destra nel verso della linea). */
		OneSide = 0,
		/** Alternati: uno a destra, il successivo a sinistra. */
		Staggered = 1,
		/** A coppie, uno di fronte all'altro. */
		Opposite = 2,
	};

	/** La forma di un tipo di palo, in metri, alla sua altezza di riferimento. */
	struct FLampShape
	{
		/** Dove sta la luce: altezza dal suolo... */
		double LightHeightM = 8.0;
		/** ...e quanto sporge dal palo verso la strada. */
		double ReachM = 0.0;
	};

	inline FLampShape LampShape(ELampKind Kind)
	{
		switch (Kind)
		{
		case ELampKind::Arm:  return FLampShape{ 9.9, 1.8 };
		case ELampKind::Post: return FLampShape{ 7.92, 0.7 };
		default:              return FLampShape{ 4.25, 0.0 };
		}
	}

	inline const char* LampKindName(ELampKind Kind)
	{
		switch (Kind)
		{
		case ELampKind::Arm:  return "sbraccio";
		case ELampKind::Post: return "palo";
		default:              return "lanterna";
		}
	}

	/**
	 * Il colore di una lampada, in luce LINEARE, con la luminanza (Rec. 709)
	 * uguale a 1: il colore cambia la tinta, non quanta luce c'e'.
	 */
	inline void LampColourRgb(ELampColour Colour, float Out[3])
	{
		double R = 1.0, G = 0.71, B = 0.42;              // 3000 K
		if (Colour == ELampColour::Sodium) { G = 0.52; B = 0.13; }
		else if (Colour == ELampColour::NeutralLed) { G = 0.82; B = 0.64; }
		const double Luminance = 0.2126 * R + 0.7152 * G + 0.0722 * B;
		Out[0] = static_cast<float>(R / Luminance);
		Out[1] = static_cast<float>(G / Luminance);
		Out[2] = static_cast<float>(B / Luminance);
	}

	// =========================================================================
	//  Le impostazioni
	// =========================================================================

	struct FStreetLightSettings
	{
		/** Strade "abitate" attorno (km per km²) da cui una strada principale ha i lampioni. */
		double UrbanDensity = 3.0;
		/** ...e da cui li ha una residenziale o una pedonale (basta una frazione). */
		double HamletDensity = 1.0;
		/** Il raggio su cui si misura, m. */
		double DensityRadiusM = 250.0;
		/** Moltiplica il passo fra un lampione e l'altro. */
		double SpacingScale = 1.0;
		/** Quanti comuni sono passati ai LED (il resto ha ancora il sodio). */
		double LedShare = 0.75;
		uint32_t Seed = 0x1A3Bu;
		/** Un tetto per tile: una tile rovinata non deve riempire la memoria. */
		size_t MaxLampsPerTile = 60000;
	};

	// =========================================================================
	//  Quanto e' abitato attorno: la densita' della rete di strade di casa
	// =========================================================================

	namespace StreetDetail
	{
		inline constexpr double MetresPerDegreeLat = 111132.0;
		inline constexpr double MetresPerDegreeLonAtEquator = 111320.0;
		inline constexpr double DegToRad = 3.14159265358979323846 / 180.0;

		inline uint64_t Mix(uint64_t Value)
		{
			Value ^= Value >> 33;
			Value *= 0xff51afd7ed558ccdull;
			Value ^= Value >> 33;
			Value *= 0xc4ceb9fe1a85ec53ull;
			Value ^= Value >> 33;
			return Value;
		}

		/** Un numero 0..1 dal punto (in una griglia di Step gradi) e dal seme. */
		inline double CellUnit(double LatDeg, double LonDeg, double StepDeg, uint32_t Seed, uint32_t Salt)
		{
			const int64_t Row = static_cast<int64_t>(std::floor(LatDeg / StepDeg));
			const int64_t Column = static_cast<int64_t>(std::floor(LonDeg / StepDeg));
			const uint64_t Hash = Mix(static_cast<uint64_t>(Row) * 0x9E3779B97F4A7C15ull
			                        ^ Mix(static_cast<uint64_t>(Column) + 0x632BE59BD9B4E019ull)
			                        ^ (static_cast<uint64_t>(Seed) << 32) ^ Salt);
			return static_cast<double>(Hash >> 11) / 9007199254740992.0;
		}

		/** Le classi che dicono "qui abita qualcuno". */
		inline bool IsHabitation(ERoadClass Class)
		{
			return Class == ERoadClass::Residential || Class == ERoadClass::LivingStreet || Class == ERoadClass::Pedestrian;
		}

		struct FPoint2
		{
			double X = 0.0, Y = 0.0;
		};

		inline FPoint2 Sub(FPoint2 A, FPoint2 B) { return FPoint2{ A.X - B.X, A.Y - B.Y }; }
		inline double Length(FPoint2 A) { return std::sqrt(A.X * A.X + A.Y * A.Y); }

		/** La distanza (al quadrato) dal punto P al segmento AB. */
		inline double SegmentDistanceSquared(FPoint2 P, FPoint2 A, FPoint2 B)
		{
			const FPoint2 AB = Sub(B, A), AP = Sub(P, A);
			const double Span = AB.X * AB.X + AB.Y * AB.Y;
			const double T = Span > 0.0 ? std::clamp((AP.X * AB.X + AP.Y * AB.Y) / Span, 0.0, 1.0) : 0.0;
			const double Dx = AP.X - T * AB.X, Dy = AP.Y - T * AB.Y;
			return Dx * Dx + Dy * Dy;
		}
	}

	/**
	 * Il passaggio fra le unita' locali di una tile vettoriale e i metri: x
	 * verso est, y verso SUD (come le unita' della tile), origine nell'angolo
	 * nord-ovest. Le tile sono quadrate in gradi, non in metri.
	 */
	struct FTileMetric
	{
		Tiles::FTileBounds Bounds;
		double Extent = Tiles::VectorTileExtent;
		/** Metri per unita' locale, verso est e verso sud. */
		double MetresPerUnitX = 1.0, MetresPerUnitY = 1.0;

		static FTileMetric For(const Tiles::FTileKey& Key, int32_t TileExtent)
		{
			FTileMetric Metric;
			Metric.Bounds = Tiles::GetTileBounds(Key.Level, Key.X, Key.Y);
			Metric.Extent = static_cast<double>(std::max(1, TileExtent));
			const double LonScale = StreetDetail::MetresPerDegreeLonAtEquator
			                      * std::max(0.01, std::cos(Metric.Bounds.CentreLat() * StreetDetail::DegToRad));
			Metric.MetresPerUnitX = Metric.Bounds.SpanLon() * LonScale / Metric.Extent;
			Metric.MetresPerUnitY = Metric.Bounds.SpanLat() * StreetDetail::MetresPerDegreeLat / Metric.Extent;
			return Metric;
		}

		StreetDetail::FPoint2 ToMetres(double U, double V) const { return StreetDetail::FPoint2{ U * MetresPerUnitX, V * MetresPerUnitY }; }

		void ToDegrees(StreetDetail::FPoint2 Metres, double& OutLatDeg, double& OutLonDeg) const
		{
			OutLonDeg = Bounds.West + Metres.X / MetresPerUnitX / Extent * Bounds.SpanLon();
			OutLatDeg = Bounds.North - Metres.Y / MetresPerUnitY / Extent * Bounds.SpanLat();
		}

		double WidthM() const { return Extent * MetresPerUnitX; }
		double HeightM() const { return Extent * MetresPerUnitY; }
	};

	/**
	 * La densita' della rete di strade "di casa" (residenziali, pedonali,
	 * living street) attorno a ogni punto di una tile, in km per km²: la
	 * lunghezza di quelle strade dentro un quadrato di lato 2 x Radius, divisa
	 * per la sua area. Una griglia di 64 x 64 celle sulla tile con il suo buffer,
	 * sfocata con una media mobile (separabile).
	 */
	class FUrbanDensity
	{
	public:
		static constexpr int32_t Cells = 64;

		void Build(const FVectorTile& Tile, const FTileMetric& Metric, double RadiusM)
		{
			using namespace StreetDetail;
			Origin = -static_cast<double>(std::max(0, Tile.Buffer));
			CellUnits = (Metric.Extent - 2.0 * Origin) / Cells;
			const double CellX = CellUnits * Metric.MetresPerUnitX, CellY = CellUnits * Metric.MetresPerUnitY;
			std::vector<double> Metres(static_cast<size_t>(Cells) * Cells, 0.0);

			for (const FVectorFeature& Feature : Tile.Features)
			{
				if (!IsHabitation(Feature.Class) || Feature.PointCount < 2) { continue; }
				for (uint32_t Index = 1; Index < Feature.PointCount; ++Index)
				{
					const Tiles::FVectorPoint& A = Tile.Points[Feature.FirstPoint + Index - 1];
					const Tiles::FVectorPoint& B = Tile.Points[Feature.FirstPoint + Index];
					const double SegmentM = Length2(Metric, A, B);
					if (SegmentM <= 0.0) { continue; }
					// Si spezza in pezzi piu' corti di mezza cella: ogni pezzo va nella sua.
					const int32_t Pieces = std::max(1, static_cast<int32_t>(std::ceil(SegmentM / (0.5 * std::min(CellX, CellY)))));
					for (int32_t Piece = 0; Piece < Pieces; ++Piece)
					{
						const double T = (Piece + 0.5) / Pieces;
						const double U = A.X + (B.X - A.X) * T, V = A.Y + (B.Y - A.Y) * T;
						const int32_t Column = CellOf(U), Row = CellOf(V);
						Metres[static_cast<size_t>(Row) * Cells + Column] += SegmentM / Pieces;
					}
				}
			}

			// La media mobile: prima lungo le righe, poi lungo le colonne.
			const int32_t RadiusX = std::max(0, static_cast<int32_t>(std::lround(RadiusM / CellX)));
			const int32_t RadiusY = std::max(0, static_cast<int32_t>(std::lround(RadiusM / CellY)));
			std::vector<double> Rows(Metres.size(), 0.0);
			for (int32_t Row = 0; Row < Cells; ++Row)
			{
				for (int32_t Column = 0; Column < Cells; ++Column)
				{
					double Sum = 0.0;
					for (int32_t Other = std::max(0, Column - RadiusX); Other <= std::min(Cells - 1, Column + RadiusX); ++Other)
					{
						Sum += Metres[static_cast<size_t>(Row) * Cells + Other];
					}
					Rows[static_cast<size_t>(Row) * Cells + Column] = Sum;
				}
			}
			const double AreaM2 = (2.0 * RadiusX + 1.0) * CellX * (2.0 * RadiusY + 1.0) * CellY;
			Grid.assign(Metres.size(), 0.0f);
			for (int32_t Row = 0; Row < Cells; ++Row)
			{
				for (int32_t Column = 0; Column < Cells; ++Column)
				{
					double Sum = 0.0;
					for (int32_t Other = std::max(0, Row - RadiusY); Other <= std::min(Cells - 1, Row + RadiusY); ++Other)
					{
						Sum += Rows[static_cast<size_t>(Other) * Cells + Column];
					}
					// m / m² -> km / km²: per mille.
					Grid[static_cast<size_t>(Row) * Cells + Column] = static_cast<float>(Sum / AreaM2 * 1000.0);
				}
			}
		}

		/** La densita' nel punto (unita' locali della tile), km per km². */
		double At(double U, double V) const
		{
			if (Grid.empty()) { return 0.0; }
			return Grid[static_cast<size_t>(CellOf(V)) * Cells + CellOf(U)];
		}

	private:
		int32_t CellOf(double Units) const
		{
			return std::clamp(static_cast<int32_t>(std::floor((Units - Origin) / CellUnits)), 0, Cells - 1);
		}

		static double Length2(const FTileMetric& Metric, const Tiles::FVectorPoint& A, const Tiles::FVectorPoint& B)
		{
			return StreetDetail::Length(StreetDetail::Sub(Metric.ToMetres(B.X, B.Y), Metric.ToMetres(A.X, A.Y)));
		}

		double Origin = 0.0;
		double CellUnits = 1.0;
		std::vector<float> Grid;
	};

	// =========================================================================
	//  Quale strada ha i lampioni, e come
	// =========================================================================

	struct FLampStyle
	{
		bool bLit = false;
		ELampKind Kind = ELampKind::Post;
		ELampLayout Layout = ELampLayout::OneSide;
		/** L'altezza della luce, m. */
		double HeightM = 7.5;
		/** Il passo fra un lampione e il successivo (sullo stesso lato o alternati), m. */
		double SpacingM = 26.0;
		/** Dal centro della strada al palo, m. */
		double OffsetM = 3.5;
		/** Quanta luce rispetto a una strada principale (1). */
		double Output = 1.0;
		/**
		 * La densita' della rete di casa (km per km²) che serve, punto per
		 * punto: una provinciale lunga chilometri ha i lampioni dentro il
		 * paese e non in campagna. 0 = ovunque (il tag lit=yes).
		 */
		double MinDensity = 0.0;
	};

	/**
	 * Lo stile dei lampioni di una linea: se li PUO' avere (bLit) e come.
	 * Dove li ha davvero lo decide MinDensity, lampione per lampione.
	 *
	 * Sui PONTI no: la luce starebbe alla quota del fondovalle, sotto
	 * l'impalcato (il terreno non sa che c'e' un ponte). Lo stesso per le
	 * linee sopraelevate (layer > 0).
	 */
	inline FLampStyle LampStyleFor(const FVectorFeature& Feature, const FStreetLightSettings& Settings)
	{
		FLampStyle Style;
		const int32_t Index = static_cast<int32_t>(Feature.Class);
		if (Index <= 0 || Index >= static_cast<int32_t>(ERoadClass::Rail)) { return Style; }
		if (Feature.Lit == Tiles::RoadLitNo || Feature.IsBridge() || Feature.Layer > 0) { return Style; }

		const bool bTagged = Feature.Lit == Tiles::RoadLitYes;
		const bool bLink = (Feature.Flags & Tiles::RoadFlagLink) != 0;
		const double Width = std::max(1.0, static_cast<double>(Feature.GetWidthMetres()));
		// Senza il tag: i lampioni dove la rete di casa e' abbastanza fitta.
		double Needed = -1.0;     // -1: mai
		bool bMajor = false;

		switch (Feature.Class)
		{
		case ERoadClass::Motorway:
		case ERoadClass::Trunk:
			Style = FLampStyle{ false, ELampKind::Arm, ELampLayout::OneSide, 12.0, 42.0, 0.0, 1.2, 0.0 };
			Needed = bLink ? Settings.UrbanDensity : -1.0;
			bMajor = true;
			break;
		case ERoadClass::Primary:
		case ERoadClass::Secondary:
			Style = FLampStyle{ false, ELampKind::Arm, ELampLayout::OneSide, 10.0, 34.0, 0.0, 1.0, 0.0 };
			Needed = Settings.UrbanDensity;
			break;
		case ERoadClass::Tertiary:
		case ERoadClass::Unclassified:
			Style = FLampStyle{ false, ELampKind::Arm, ELampLayout::OneSide, 9.0, 30.0, 0.0, 0.9, 0.0 };
			Needed = Settings.UrbanDensity;
			break;
		case ERoadClass::Residential:
			Style = FLampStyle{ false, ELampKind::Post, ELampLayout::OneSide, 7.5, 26.0, 0.0, 0.7, 0.0 };
			Needed = Settings.HamletDensity;
			break;
		case ERoadClass::Service:
			Style = FLampStyle{ false, ELampKind::Post, ELampLayout::OneSide, 7.0, 28.0, 0.0, 0.6, 0.0 };
			Needed = Settings.UrbanDensity;
			break;
		case ERoadClass::LivingStreet:
		case ERoadClass::Pedestrian:
			Style = FLampStyle{ false, ELampKind::Lantern, ELampLayout::OneSide, 4.25, 18.0, 0.0, 0.5, 0.0 };
			Needed = Settings.HamletDensity;
			break;
		case ERoadClass::Track:
			Style = FLampStyle{ false, ELampKind::Post, ELampLayout::OneSide, 6.0, 30.0, 0.0, 0.5, 0.0 };
			break;
		case ERoadClass::Path:
			Style = FLampStyle{ false, ELampKind::Lantern, ELampLayout::OneSide, 4.0, 22.0, 0.0, 0.4, 0.0 };
			break;
		default:
			return FLampStyle{};
		}
		// Lo sterrato senza il tag: campagna, niente lampioni.
		if (Feature.IsUnpaved() && !bTagged) { Needed = -1.0; }
		Style.bLit = bTagged || Needed >= 0.0;
		Style.MinDensity = bTagged ? 0.0 : std::max(0.0, Needed);

		// La disposizione: un lato solo sulle strette (e sulle autostrade, che
		// in OSM sono una linea per carreggiata: il lato esterno di ognuna),
		// alternati sulle medie, a coppie sulle larghe.
		if (Style.Kind != ELampKind::Lantern && !bLink && !bMajor)
		{
			if (Width > 15.0) { Style.Layout = ELampLayout::Opposite; }
			else if (Width > 9.0) { Style.Layout = ELampLayout::Staggered; }
		}

		// Il palo: sul marciapiede (mezzo metro dal cordolo), sul ciglio senza;
		// la lanterna sul bordo della zona pedonale, o accanto al sentiero.
		const double Half = 0.5 * Width;
		const bool bSidewalk = Feature.IsSidewalkKnown()
			? (Feature.HasSidewalkLeft() || Feature.HasSidewalkRight())
			: Feature.Class == ERoadClass::Residential;
		if (Style.Kind == ELampKind::Lantern)
		{
			Style.OffsetM = (Feature.Class == ERoadClass::Path) ? Half + 0.5 : std::max(0.8, Half - 0.3);
		}
		else if (bMajor) { Style.OffsetM = Half + 1.5; }
		else { Style.OffsetM = Half + (bSidewalk ? 0.5 : 0.8); }

		Style.SpacingM = std::max(8.0, Style.SpacingM * Settings.SpacingScale);
		return Style;
	}

	// =========================================================================
	//  Le linee ricucite
	// =========================================================================

	/**
	 * Una catena di linee della tile, ricucite dove due (e solo due) si
	 * toccano con lo stesso stile: una via spezzata in OSM torna intera. I
	 * punti sono in unita' locali, nell'ordine in cui si cammina.
	 */
	struct FChain
	{
		std::vector<StreetDetail::FPoint2> Points;
		/** Le linee della catena (indici in FVectorTile::Features). */
		std::vector<uint32_t> Features;
	};

	namespace StreetDetail
	{
		inline uint32_t PackPoint(const Tiles::FVectorPoint& Point)
		{
			return (static_cast<uint32_t>(static_cast<uint16_t>(Point.X)) << 16) | static_cast<uint16_t>(Point.Y);
		}

		/**
		 * Lo stile ridotto a un numero: due linee si ricuciono solo se e'
		 * uguale (stessa classe, stessa disposizione, stessa larghezza al
		 * metro, stesso tag). 0 = senza lampioni.
		 */
		inline uint64_t StyleKey(const FVectorFeature& Feature, const FLampStyle& Style)
		{
			if (!Style.bLit) { return 0; }
			return 1ull | (static_cast<uint64_t>(Feature.Class) << 8) | (static_cast<uint64_t>(Style.Layout) << 16)
			     | (static_cast<uint64_t>(std::lround(Feature.GetWidthMetres())) << 24)
			     | (static_cast<uint64_t>(Feature.Lit) << 40);
		}
	}

	/**
	 * Le catene delle linee con Keys[i] != 0. Prima quelle che cominciano a un
	 * capo libero, nel verso della prima linea (le autostrade e i sensi unici
	 * restano nel loro verso: il lato destro e' quello esterno); poi quelle
	 * chiuse ad anello (rotonde).
	 */
	inline void BuildChains(const FVectorTile& Tile, const std::vector<uint64_t>& Keys, std::vector<FChain>& Out)
	{
		using namespace StreetDetail;
		Out.clear();
		const size_t Count = Tile.Features.size();
		struct FEnd { uint32_t Feature; bool bStart; };
		std::unordered_map<uint32_t, std::vector<FEnd>> Nodes;
		auto StartOf = [&Tile](const FVectorFeature& F) { return PackPoint(Tile.Points[F.FirstPoint]); };
		auto EndOf = [&Tile](const FVectorFeature& F) { return PackPoint(Tile.Points[F.FirstPoint + F.PointCount - 1]); };
		for (uint32_t Index = 0; Index < Count; ++Index)
		{
			const FVectorFeature& Feature = Tile.Features[Index];
			if (Keys[Index] == 0 || Feature.PointCount < 2) { continue; }
			Nodes[StartOf(Feature)].push_back(FEnd{ Index, true });
			Nodes[EndOf(Feature)].push_back(FEnd{ Index, false });
		}
		// Un nodo cuce se ci arrivano esattamente due capi, di due linee
		// diverse con lo stesso stile.
		auto Link = [&](uint32_t Node, uint32_t From, FEnd& OutNext)
		{
			const auto Found = Nodes.find(Node);
			if (Found == Nodes.end() || Found->second.size() != 2) { return false; }
			const FEnd& A = Found->second[0];
			const FEnd& B = Found->second[1];
			if (A.Feature == B.Feature || Keys[A.Feature] != Keys[B.Feature]) { return false; }
			OutNext = (A.Feature == From) ? B : A;
			return true;
		};
		auto IsLinked = [&](uint32_t Node, uint32_t From) { FEnd Ignored{}; return Link(Node, From, Ignored); };

		std::vector<bool> Visited(Count, false);
		auto Walk = [&](uint32_t First, bool bForward)
		{
			FChain Chain;
			uint32_t Current = First;
			bool bCurrentForward = bForward;
			while (true)
			{
				Visited[Current] = true;
				Chain.Features.push_back(Current);
				const FVectorFeature& Feature = Tile.Features[Current];
				for (uint32_t Step = 0; Step < Feature.PointCount; ++Step)
				{
					const uint32_t P = bCurrentForward ? Step : Feature.PointCount - 1 - Step;
					if (Step == 0 && !Chain.Points.empty()) { continue; }    // il punto in comune
					const Tiles::FVectorPoint& Point = Tile.Points[Feature.FirstPoint + P];
					Chain.Points.push_back(FPoint2{ static_cast<double>(Point.X), static_cast<double>(Point.Y) });
				}
				const uint32_t Exit = bCurrentForward ? EndOf(Feature) : StartOf(Feature);
				FEnd Next{};
				if (!Link(Exit, Current, Next) || Visited[Next.Feature]) { break; }
				Current = Next.Feature;
				bCurrentForward = Next.bStart;
			}
			if (Chain.Points.size() >= 2) { Out.push_back(std::move(Chain)); }
		};

		for (uint32_t Index = 0; Index < Count; ++Index)
		{
			const FVectorFeature& Feature = Tile.Features[Index];
			if (Visited[Index] || Keys[Index] == 0 || Feature.PointCount < 2) { continue; }
			if (!IsLinked(StartOf(Feature), Index)) { Walk(Index, true); }
		}
		for (uint32_t Index = 0; Index < Count; ++Index)
		{
			const FVectorFeature& Feature = Tile.Features[Index];
			if (Visited[Index] || Keys[Index] == 0 || Feature.PointCount < 2) { continue; }
			if (!IsLinked(EndOf(Feature), Index)) { Walk(Index, false); }
		}
		for (uint32_t Index = 0; Index < Count; ++Index)
		{
			if (!Visited[Index] && Keys[Index] != 0 && Tile.Features[Index].PointCount >= 2) { Walk(Index, true); }
		}
	}

	// =========================================================================
	//  Gli ostacoli: le altre strade
	// =========================================================================

	/**
	 * Le strade (e i binari, e le piste) di una tile in una griglia di celle
	 * da 40 m, per sapere in fretta se un palo cadrebbe su una carreggiata.
	 * I sentieri no: un marciapiede mappato a parte e' proprio dove va il palo.
	 */
	class FObstacleGrid
	{
	public:
		void Build(const FVectorTile& Tile, const FTileMetric& Metric)
		{
			using namespace StreetDetail;
			const double Buffer = static_cast<double>(std::max(0, Tile.Buffer));
			OriginX = -Buffer * Metric.MetresPerUnitX;
			OriginY = -Buffer * Metric.MetresPerUnitY;
			Columns = std::max(1, static_cast<int32_t>(std::ceil((Metric.WidthM() - 2.0 * OriginX) / CellM)));
			Rows = std::max(1, static_cast<int32_t>(std::ceil((Metric.HeightM() - 2.0 * OriginY) / CellM)));
			Buckets.assign(static_cast<size_t>(Columns) * Rows, {});
			Segments.clear();
			for (uint32_t Index = 0; Index < Tile.Features.size(); ++Index)
			{
				const FVectorFeature& Feature = Tile.Features[Index];
				if (Feature.Class == ERoadClass::Path || Feature.Class == ERoadClass::Unknown) { continue; }
				const double Half = 0.5 * Feature.GetWidthMetres();
				for (uint32_t Point = 1; Point < Feature.PointCount; ++Point)
				{
					const Tiles::FVectorPoint& A = Tile.Points[Feature.FirstPoint + Point - 1];
					const Tiles::FVectorPoint& B = Tile.Points[Feature.FirstPoint + Point];
					FSegment Segment{ Metric.ToMetres(A.X, A.Y), Metric.ToMetres(B.X, B.Y), Half, Index };
					const uint32_t Id = static_cast<uint32_t>(Segments.size());
					Segments.push_back(Segment);
					const double Reach = Half + Margin;
					const int32_t C0 = ColumnOf(std::min(Segment.A.X, Segment.B.X) - Reach);
					const int32_t C1 = ColumnOf(std::max(Segment.A.X, Segment.B.X) + Reach);
					const int32_t R0 = RowOf(std::min(Segment.A.Y, Segment.B.Y) - Reach);
					const int32_t R1 = RowOf(std::max(Segment.A.Y, Segment.B.Y) + Reach);
					for (int32_t Row = R0; Row <= R1; ++Row)
					{
						for (int32_t Column = C0; Column <= C1; ++Column)
						{
							Buckets[static_cast<size_t>(Row) * Columns + Column].push_back(Id);
						}
					}
				}
			}
		}

		/**
		 * Il punto (metri della tile) e' su una carreggiata che non e' fra le
		 * Own (le linee della catena del lampione)? "Su" vuol dire entro la
		 * meta' della sua larghezza piu' Margin.
		 */
		bool Blocked(StreetDetail::FPoint2 Point, const std::vector<uint32_t>& Own) const
		{
			if (Buckets.empty()) { return false; }
			const std::vector<uint32_t>& Bucket = Buckets[static_cast<size_t>(RowOf(Point.Y)) * Columns + ColumnOf(Point.X)];
			for (const uint32_t Id : Bucket)
			{
				const FSegment& Segment = Segments[Id];
				if (std::find(Own.begin(), Own.end(), Segment.Feature) != Own.end()) { continue; }
				const double Reach = Segment.Half + Margin;
				if (StreetDetail::SegmentDistanceSquared(Point, Segment.A, Segment.B) < Reach * Reach) { return true; }
			}
			return false;
		}

		static constexpr double CellM = 40.0;
		static constexpr double Margin = 0.6;

	private:
		struct FSegment
		{
			StreetDetail::FPoint2 A, B;
			double Half = 0.0;
			uint32_t Feature = 0;
		};
		int32_t ColumnOf(double X) const { return std::clamp(static_cast<int32_t>(std::floor((X - OriginX) / CellM)), 0, Columns - 1); }
		int32_t RowOf(double Y) const { return std::clamp(static_cast<int32_t>(std::floor((Y - OriginY) / CellM)), 0, Rows - 1); }

		double OriginX = 0.0, OriginY = 0.0;
		int32_t Columns = 0, Rows = 0;
		std::vector<FSegment> Segments;
		std::vector<std::vector<uint32_t>> Buckets;
	};

	// =========================================================================
	//  I lampioni di una tile
	// =========================================================================

	/** Un lampione: dove sta il palo, dove guarda, com'e'. */
	struct FStreetLamp
	{
		/** La base del palo. */
		double LatDeg = 0.0, LonDeg = 0.0;
		/** Il punto della strada da cui e' nato: decide la cella (vedi BuildCellLamps). */
		double AnchorLatDeg = 0.0, AnchorLonDeg = 0.0;
		/** Verso dove sporge lo sbraccio (la strada): 0 nord, pi/2 est. */
		float YawRad = 0.0f;
		/** L'altezza della luce, m. */
		float HeightM = 7.5f;
		/** Quanta luce rispetto a una strada principale. */
		float Output = 1.0f;
		/** La soglia della fotocellula del suo circuito, lux. */
		float SwitchLux = 30.0f;
		float Random = 0.0f;
		ELampKind Kind = ELampKind::Post;
		ELampColour Colour = ELampColour::WarmLed;
	};

	struct FPlacementStats
	{
		int32_t LitFeatures = 0;
		int32_t Chains = 0;
		int32_t Lamps = 0;
		/** Lampioni non messi perche' sarebbero caduti su un'altra strada. */
		int32_t Blocked = 0;
		/** Posti lungo una strada che li puo' avere, ma in un tratto non abbastanza abitato. */
		int32_t Dark = 0;
		int32_t ByKind[LampKindCount] = { 0, 0, 0 };
	};

	namespace StreetDetail
	{
		/** Liang-Barsky: il tratto [T0, T1] del segmento AB dentro il rettangolo. Falso se non c'e'. */
		inline bool ClipSegment(FPoint2 A, FPoint2 B, double MaxX, double MaxY, double& OutT0, double& OutT1)
		{
			double T0 = 0.0, T1 = 1.0;
			const double Dx = B.X - A.X, Dy = B.Y - A.Y;
			const double P[4] = { -Dx, Dx, -Dy, Dy };
			const double Q[4] = { A.X, MaxX - A.X, A.Y, MaxY - A.Y };
			for (int32_t Side = 0; Side < 4; ++Side)
			{
				if (P[Side] == 0.0)
				{
					if (Q[Side] < 0.0) { return false; }
					continue;
				}
				const double R = Q[Side] / P[Side];
				if (P[Side] < 0.0) { T0 = std::max(T0, R); }
				else { T1 = std::min(T1, R); }
				if (T0 > T1) { return false; }
			}
			OutT0 = T0;
			OutT1 = T1;
			return true;
		}

		/**
		 * Il palo cade sulla SUA strada, piu' in la'? Succede agli angoli: due
		 * vie ricucite in una catena girano di 90 gradi, e il palo dell'ultimo
		 * lampione prima dell'angolo starebbe sulla carreggiata dell'altro
		 * braccio. Si guarda la catena tranne il pezzo attorno al lampione
		 * (lungo quanto lo scostamento piu' un metro, per parte): li' il palo
		 * e' per costruzione a Offset dal centro. Su una curva dolce la
		 * distanza resta Offset; sotto, e' un angolo.
		 */
		inline bool OwnBlocked(const std::vector<FPoint2>& Points, const std::vector<double>& Cumulative, double S, FPoint2 Pole,
		                       double OffsetM)
		{
			const double Window = OffsetM + 1.0;
			const double Reach = OffsetM - 0.05;
			for (size_t Index = 1; Index < Points.size(); ++Index)
			{
				const double C0 = Cumulative[Index - 1], C1 = Cumulative[Index];
				if (C1 <= C0) { continue; }
				// I due pezzi del segmento fuori dalla finestra [S - Window, S + Window].
				const double Parts[2][2] = { { C0, std::min(C1, S - Window) }, { std::max(C0, S + Window), C1 } };
				for (const auto& Part : Parts)
				{
					if (Part[1] <= Part[0]) { continue; }
					const double T0 = (Part[0] - C0) / (C1 - C0), T1 = (Part[1] - C0) / (C1 - C0);
					const FPoint2 A{ Points[Index - 1].X + (Points[Index].X - Points[Index - 1].X) * T0,
					                 Points[Index - 1].Y + (Points[Index].Y - Points[Index - 1].Y) * T0 };
					const FPoint2 B{ Points[Index - 1].X + (Points[Index].X - Points[Index - 1].X) * T1,
					                 Points[Index - 1].Y + (Points[Index].Y - Points[Index - 1].Y) * T1 };
					if (SegmentDistanceSquared(Pole, A, B) < Reach * Reach) { return true; }
				}
			}
			return false;
		}

		/** Il colore di un lampione: il comune e' passato ai LED? Le principali LED neutro, il resto caldo. */
		inline ELampColour ChooseColour(double LatDeg, double LonDeg, const FLampStyle& Style, const FStreetLightSettings& Settings)
		{
			if (CellUnit(LatDeg, LonDeg, 0.02, Settings.Seed, 0x11u) >= Settings.LedShare) { return ELampColour::Sodium; }
			return Style.Kind == ELampKind::Arm ? ELampColour::NeutralLed : ELampColour::WarmLed;
		}

		/** La soglia della fotocellula del circuito (una cella di ~300 m): 15..60 lux, distribuita in logaritmo. */
		inline double CircuitSwitchLux(double LatDeg, double LonDeg, uint32_t Seed)
		{
			const double U = CellUnit(LatDeg, LonDeg, 0.003, Seed, 0x22u);
			return 15.0 * std::exp(U * std::log(4.0));
		}
	}

	/**
	 * Tutti i lampioni di una tile vettoriale: quali strade, le catene, il
	 * passo, i lati, gli ostacoli. Solo i tratti DENTRO la tile (le linee
	 * escono di un buffer): la tile accanto mette i suoi.
	 */
	inline void PlaceTileLamps(const FVectorTile& Tile, const FStreetLightSettings& Settings, std::vector<FStreetLamp>& Out,
	                           FPlacementStats* OutStats = nullptr)
	{
		using namespace StreetDetail;
		Out.clear();
		FPlacementStats Stats;
		const FTileMetric Metric = FTileMetric::For(Tile.Key, Tile.Extent);

		FUrbanDensity Density;
		Density.Build(Tile, Metric, Settings.DensityRadiusM);

		// Lo stile di ogni linea (dove li ha davvero, lo dice la densita' punto per punto).
		std::vector<FLampStyle> Styles(Tile.Features.size());
		std::vector<uint64_t> Keys(Tile.Features.size(), 0);
		for (size_t Index = 0; Index < Tile.Features.size(); ++Index)
		{
			const FVectorFeature& Feature = Tile.Features[Index];
			if (Feature.PointCount < 2) { continue; }
			Styles[Index] = LampStyleFor(Feature, Settings);
			Keys[Index] = StyleKey(Feature, Styles[Index]);
			Stats.LitFeatures += Keys[Index] != 0 ? 1 : 0;
		}

		std::vector<FChain> Chains;
		BuildChains(Tile, Keys, Chains);
		Stats.Chains = static_cast<int32_t>(Chains.size());
		if (Chains.empty())
		{
			if (OutStats) { *OutStats = Stats; }
			return;
		}

		FObstacleGrid Obstacles;
		Obstacles.Build(Tile, Metric);
		const double MaxX = Metric.WidthM(), MaxY = Metric.HeightM();

		std::vector<FPoint2> Points;
		std::vector<double> Cumulative;
		std::vector<std::pair<double, double>> Inside;
		for (const FChain& Chain : Chains)
		{
			const FLampStyle& Style = Styles[Chain.Features.front()];
			Points.resize(Chain.Points.size());
			Cumulative.resize(Chain.Points.size());
			for (size_t Index = 0; Index < Chain.Points.size(); ++Index)
			{
				Points[Index] = Metric.ToMetres(Chain.Points[Index].X, Chain.Points[Index].Y);
				Cumulative[Index] = (Index == 0) ? 0.0 : Cumulative[Index - 1] + Length(Sub(Points[Index], Points[Index - 1]));
			}

			// I tratti dentro la tile, in metri lungo la catena (quelli che si
			// toccano diventano uno).
			Inside.clear();
			for (size_t Index = 1; Index < Points.size(); ++Index)
			{
				double T0 = 0.0, T1 = 0.0;
				const double SegmentM = Cumulative[Index] - Cumulative[Index - 1];
				if (SegmentM <= 0.0 || !ClipSegment(Points[Index - 1], Points[Index], MaxX, MaxY, T0, T1)) { continue; }
				const double A = Cumulative[Index - 1] + T0 * SegmentM, B = Cumulative[Index - 1] + T1 * SegmentM;
				if (!Inside.empty() && A - Inside.back().second < 1.0e-6) { Inside.back().second = B; }
				else { Inside.emplace_back(A, B); }
			}

			size_t Segment = 1;
			for (const std::pair<double, double>& Run : Inside)
			{
				int32_t Ordinal = 0;
				for (double S = Run.first + 0.5 * Style.SpacingM; S <= Run.second; S += Style.SpacingM, ++Ordinal)
				{
					// Il punto della strada e la sua direzione.
					while (Segment + 1 < Points.size() && Cumulative[Segment] < S) { ++Segment; }
					const double SegmentM = std::max(1.0e-9, Cumulative[Segment] - Cumulative[Segment - 1]);
					const double T = std::clamp((S - Cumulative[Segment - 1]) / SegmentM, 0.0, 1.0);
					const FPoint2 A = Points[Segment - 1], B = Points[Segment];
					const FPoint2 Anchor{ A.X + (B.X - A.X) * T, A.Y + (B.Y - A.Y) * T };
					const FPoint2 Direction{ (B.X - A.X) / SegmentM, (B.Y - A.Y) / SegmentM };
					// Abbastanza abitato qui? (Il passo resta: un tratto buio e' un buco nella fila.)
					const double AnchorU = Anchor.X / Metric.MetresPerUnitX, AnchorV = Anchor.Y / Metric.MetresPerUnitY;
					if (Style.MinDensity > 0.0 && Density.At(AnchorU, AnchorV) < Style.MinDensity) { ++Stats.Dark; continue; }
					// La destra nel verso della linea (x est, y sud).
					const FPoint2 Right{ -Direction.Y, Direction.X };

					int32_t Sides[2] = { 1, 0 };
					int32_t SideCount = 1;
					if (Style.Layout == ELampLayout::Staggered) { Sides[0] = (Ordinal % 2 == 0) ? 1 : -1; }
					else if (Style.Layout == ELampLayout::Opposite) { Sides[0] = 1; Sides[1] = -1; SideCount = 2; }

					for (int32_t Which = 0; Which < SideCount; ++Which)
					{
						const double Side = static_cast<double>(Sides[Which]);
						const FPoint2 Pole{ Anchor.X + Right.X * Side * Style.OffsetM, Anchor.Y + Right.Y * Side * Style.OffsetM };
						if (Obstacles.Blocked(Pole, Chain.Features) || OwnBlocked(Points, Cumulative, S, Pole, Style.OffsetM))
						{
							++Stats.Blocked;
							continue;
						}
						if (Out.size() >= Settings.MaxLampsPerTile) { break; }

						FStreetLamp Lamp;
						Metric.ToDegrees(Pole, Lamp.LatDeg, Lamp.LonDeg);
						Metric.ToDegrees(Anchor, Lamp.AnchorLatDeg, Lamp.AnchorLonDeg);
						// Lo sbraccio guarda la strada: -Side * Right, da (est, sud) a (nord, est).
						const double TowardEast = -Side * Right.X, TowardSouth = -Side * Right.Y;
						Lamp.YawRad = static_cast<float>(std::atan2(TowardEast, -TowardSouth));
						Lamp.HeightM = static_cast<float>(Style.HeightM);
						Lamp.Output = static_cast<float>(Style.Output);
						Lamp.Kind = Style.Kind;
						Lamp.Colour = ChooseColour(Lamp.LatDeg, Lamp.LonDeg, Style, Settings);
						Lamp.SwitchLux = static_cast<float>(CircuitSwitchLux(Lamp.LatDeg, Lamp.LonDeg, Settings.Seed));
						Lamp.Random = static_cast<float>(CellUnit(Lamp.LatDeg, Lamp.LonDeg, 1.0e-5, Settings.Seed, 0x33u));
						Out.push_back(Lamp);
						++Stats.ByKind[static_cast<int32_t>(Style.Kind)];
					}
				}
			}
		}
		Stats.Lamps = static_cast<int32_t>(Out.size());
		if (OutStats) { *OutStats = Stats; }
	}

	// =========================================================================
	//  I lampioni di una cella, sul terreno disegnato
	// =========================================================================

	/** Un lampione pronto per i componenti: nel frame della cella (nord, est, alto, m). */
	struct FLampInstance
	{
		/** La base del palo, sul terreno disegnato. */
		float Base[3] = { 0.0f, 0.0f, 0.0f };
		/** Dove sta la luce. */
		float Light[3] = { 0.0f, 0.0f, 0.0f };
		float YawRad = 0.0f;
		/** La scala della forma (Kind) per arrivare all'altezza voluta. */
		float Scale = 1.0f;
		/** Il colore lineare per la luce relativa (Output). */
		float Colour[3] = { 1.0f, 1.0f, 1.0f };
		float SwitchLux = 30.0f;
		float Random = 0.0f;
		float HeightM = 7.5f;
		float Output = 1.0f;
		/** Lo stesso lampione ha lo stesso numero anche quando la cella si rifa' (le luci vere lo seguono). */
		uint64_t Id = 0;
		uint8_t Kind = 0;
	};

	/** I dati per istanza dei materiali (Per Instance Custom Data): il colore e la soglia. */
	inline constexpr int32_t InstanceDataFloats = 5;

	inline void InstanceData(const FLampInstance& Lamp, float Out[InstanceDataFloats])
	{
		Out[0] = Lamp.Colour[0];
		Out[1] = Lamp.Colour[1];
		Out[2] = Lamp.Colour[2];
		Out[3] = Lamp.SwitchLux;
		Out[4] = Lamp.Random;
	}

	/**
	 * Il livello delle celle: ~1,2 km x 0,9 km a 45 gradi (il 14), o quello
	 * delle tile vettoriali piu' fini se il dataset scende piu' in giu'.
	 */
	inline uint32_t CellLevelFor(int32_t MaxVectorLevel) { return static_cast<uint32_t>(std::max(14, MaxVectorLevel)); }

	/**
	 * I lampioni della tile che stanno nella cella (dal loro punto di strada:
	 * una cella di un'altra tile non li vede, e nessuno e' contato due volte),
	 * sul terreno disegnato, nel frame della cella. Quelli senza terreno sotto
	 * si saltano: la cella si rifa' quando il terreno arriva.
	 */
	inline void BuildCellLamps(const Tiles::FTileBounds& Cell, const std::vector<FStreetLamp>& Lamps,
	                           const Cells::FGroundSurfaces& Ground, std::vector<FLampInstance>& Out)
	{
		Out.clear();
		const Cells::FCellFrame Frame = Cells::FCellFrame::At(Cell.CentreLat(), Cell.CentreLon());
		for (const FStreetLamp& Lamp : Lamps)
		{
			if (Lamp.AnchorLatDeg < Cell.South || Lamp.AnchorLatDeg >= Cell.North
				|| Lamp.AnchorLonDeg < Cell.West || Lamp.AnchorLonDeg >= Cell.East) { continue; }
			double GroundM = 0.0, SlopeDeg = 0.0;
			if (!Ground.GroundAt(Lamp.LatDeg, Lamp.LonDeg, GroundM, SlopeDeg)) { continue; }

			FLampInstance Instance;
			double Base[3];
			Frame.Local(Lamp.LatDeg, Lamp.LonDeg, GroundM, Base);
			const FLampShape Shape = LampShape(Lamp.Kind);
			const double Scale = Lamp.HeightM / Shape.LightHeightM;
			const double Reach = Shape.ReachM * Scale;
			for (int32_t Axis = 0; Axis < 3; ++Axis) { Instance.Base[Axis] = static_cast<float>(Base[Axis]); }
			Instance.Light[0] = static_cast<float>(Base[0] + std::cos(Lamp.YawRad) * Reach);
			Instance.Light[1] = static_cast<float>(Base[1] + std::sin(Lamp.YawRad) * Reach);
			Instance.Light[2] = static_cast<float>(Base[2] + Lamp.HeightM);
			Instance.YawRad = Lamp.YawRad;
			Instance.Scale = static_cast<float>(Scale);
			float Rgb[3];
			LampColourRgb(Lamp.Colour, Rgb);
			for (int32_t Channel = 0; Channel < 3; ++Channel) { Instance.Colour[Channel] = Rgb[Channel] * Lamp.Output; }
			Instance.SwitchLux = Lamp.SwitchLux;
			Instance.Random = Lamp.Random;
			Instance.HeightM = Lamp.HeightM;
			Instance.Output = Lamp.Output;
			Instance.Kind = static_cast<uint8_t>(Lamp.Kind);
			Instance.Id = StreetDetail::Mix(static_cast<uint64_t>(std::llround(Lamp.LatDeg * 1.0e6)) * 0x9E3779B97F4A7C15ull
			                              ^ static_cast<uint64_t>(std::llround(Lamp.LonDeg * 1.0e6)));
			Out.push_back(Instance);
		}
	}

	// =========================================================================
	//  Quando si accendono, e quanta luce fanno
	// =========================================================================

	/** La luce che vede una fotocellula, lux "veri": il sole e il crepuscolo, meno con le nuvole. */
	inline double AmbientLux(double SunAltitudeDeg, double CloudCover)
	{
		const double Clear = 1.0 - 0.75 * std::clamp(CloudCover, 0.0, 1.0);
		return 110000.0 * Sky::SunlightShare(SunAltitudeDeg) * Clear;
	}

	/** La mattina una fotocellula spegne a una luce piu' alta di quella a cui ha acceso. */
	inline constexpr double MorningSwitchFactor = 1.6;

	/**
	 * Quanto e' acceso un lampione con soglia SwitchLux: 1 sotto la soglia, 0
	 * sopra il 15% in piu', in mezzo una rampa (la lampada che si scalda).
	 * E' la stessa formula del materiale (StreetLightShader.inl).
	 */
	inline double LampOnLevel(double Ambient, double SwitchLux, bool bMorning)
	{
		const double Threshold = std::max(SwitchLux * (bMorning ? MorningSwitchFactor : 1.0), 0.001);
		return std::clamp((Threshold * 1.15 - Ambient) / (Threshold * 0.15), 0.0, 1.0);
	}

	/** L'altezza del sole, gradi (senza la luna: costa la meta' di ObserveSky). */
	inline double SunAltitudeDeg(double JdUt, double LatDeg, double LonDeg)
	{
		const Sky::FEquatorial Sun = Sky::SunPosition(Sky::TerrestrialTime(JdUt));
		return Sky::EquatorialToHorizontal(Sun.RightAscensionDeg, Sun.DeclinationDeg, Sky::LocalSiderealTime(JdUt, LonDeg), LatDeg).AltitudeDeg;
	}

	/** E' mattina (il sole sale)? Decide l'isteresi delle fotocellule. */
	inline bool IsMorning(double JdUt, double LatDeg, double LonDeg)
	{
		return SunAltitudeDeg(JdUt + 5.0 / 1440.0, LatDeg, LonDeg) > SunAltitudeDeg(JdUt, LatDeg, LonDeg);
	}

	/**
	 * La riduzione del flusso nel cuore della notte: 1 la sera, 1 - Depth fra
	 * le 00:30 e le 04:30 (ora locale), con un'ora di rampa ai due capi.
	 */
	inline double NightDimming(double LocalHour, double Depth)
	{
		double Hour = std::fmod(LocalHour, 24.0);
		if (Hour < 0.0) { Hour += 24.0; }
		// Le ore dalle 18: la notte diventa un intervallo solo (23,5 -> 5,5, 29,5 -> 35,5).
		const double FromEvening = Hour < 12.0 ? Hour + 24.0 : Hour;
		const double Down = std::clamp(FromEvening - 23.5, 0.0, 1.0);
		const double Up = std::clamp(29.5 - FromEvening, 0.0, 1.0);
		return 1.0 - std::clamp(Depth, 0.0, 1.0) * std::min(Down, Up);
	}

	/**
	 * La luce al piede di un lampione di riferimento (Output 1), nelle unita'
	 * della scena: 1/50 del sole del livello, venti volte la luna piena
	 * (Sky/DayLight.h). Brightness la regola (geo.Lights.Brightness).
	 */
	inline double FootLux(double DayLux, double Brightness) { return 0.02 * DayLux * std::max(0.0, Brightness); }

	/** Una luce vera (uno spot verso il basso) per un lampione. */
	struct FSpotLight
	{
		/** Candele: al piede, a HeightM sotto, fanno Candela / H² lux. */
		double Candela = 0.0;
		double RadiusM = 30.0;
		double InnerDeg = 45.0;
		double OuterDeg = 72.0;
	};

	inline FSpotLight SpotFor(double HeightM, double Output, double Foot, double OnLevel, double Dimming)
	{
		FSpotLight Spot;
		const double Height = std::max(1.0, HeightM);
		Spot.Candela = Foot * Output * Height * Height * std::clamp(OnLevel, 0.0, 1.0) * std::clamp(Dimming, 0.0, 1.0);
		Spot.RadiusM = 4.5 * Height;
		return Spot;
	}

	// =========================================================================
	//  Chi ha una luce vera: i piu' vicini
	// =========================================================================

	struct FLightCandidate
	{
		uint64_t Id = 0;
		double DistanceM = 0.0;
	};

	struct FLightChoice
	{
		uint64_t Id = 0;
		/** Quanto deve essere accesa: 1 per i piu' vicini, verso 0 in fondo alla lista. */
		double Weight = 1.0;
		/** L'indice del candidato. */
		size_t Candidate = 0;
	};

	/**
	 * Le Count luci vere: i lampioni piu' vicini, dal piu' vicino. Il peso e'
	 * pieno per i primi tre quarti e scende a zero verso il fondo: un
	 * lampione entra ed esce dalla lista dove pesa quasi niente, e la sua luce
	 * non "scatta" mentre la camera si muove (la sfumatura nel tempo la fa chi
	 * disegna).
	 */
	inline void SelectLights(const std::vector<FLightCandidate>& Candidates, int32_t Count, std::vector<FLightChoice>& Out)
	{
		Out.clear();
		if (Count <= 0 || Candidates.empty()) { return; }
		std::vector<size_t> Order(Candidates.size());
		for (size_t Index = 0; Index < Order.size(); ++Index) { Order[Index] = Index; }
		const size_t Kept = std::min(Order.size(), static_cast<size_t>(Count));
		std::partial_sort(Order.begin(), Order.begin() + static_cast<std::ptrdiff_t>(Kept), Order.end(), [&Candidates](size_t A, size_t B)
		{
			if (Candidates[A].DistanceM != Candidates[B].DistanceM) { return Candidates[A].DistanceM < Candidates[B].DistanceM; }
			return Candidates[A].Id < Candidates[B].Id;
		});
		for (size_t Rank = 0; Rank < Kept; ++Rank)
		{
			const double Weight = std::clamp((static_cast<double>(Count) - static_cast<double>(Rank)) / (0.25 * Count), 0.0, 1.0);
			Out.push_back(FLightChoice{ Candidates[Order[Rank]].Id, Weight, Order[Rank] });
		}
	}

	// =========================================================================
	//  Le forme: i pali e il quadrato dei punti luce
	// =========================================================================

	namespace StreetDetail
	{
		using FMeshVec = Vegetation::TreeMeshDetail::FVec;

		inline void PushVertex(Vegetation::FTreeMesh& Mesh, const FMeshVec& Position, const FMeshVec& Normal, const uint8_t Colour[4])
		{
			Mesh.Positions.insert(Mesh.Positions.end(), { static_cast<float>(Position.X), static_cast<float>(Position.Y), static_cast<float>(Position.Z) });
			Mesh.Normals.insert(Mesh.Normals.end(), { static_cast<float>(Normal.X), static_cast<float>(Normal.Y), static_cast<float>(Normal.Z) });
			Mesh.Colours.insert(Mesh.Colours.end(), { Colour[0], Colour[1], Colour[2], Colour[3] });
		}

		/** Un quadrilatero piano (A, B, C, D in senso antiorario visto da fuori), con la sua normale. */
		inline void AddQuad(Vegetation::FTreeMesh& Mesh, const FMeshVec& A, const FMeshVec& B, const FMeshVec& C, const FMeshVec& D,
		                    const uint8_t Colour[4])
		{
			const FMeshVec Normal = (B - A).Cross(D - A).Normalized();
			const uint32_t First = static_cast<uint32_t>(Mesh.VertexCount());
			PushVertex(Mesh, A, Normal, Colour);
			PushVertex(Mesh, B, Normal, Colour);
			PushVertex(Mesh, C, Normal, Colour);
			PushVertex(Mesh, D, Normal, Colour);
			Mesh.Indices.insert(Mesh.Indices.end(), { First, First + 1, First + 2, First, First + 2, First + 3 });
		}

		/** Un tubo da Bottom a Top (raggi diversi: si assottiglia), Sides lati, con il tappo in cima. */
		inline void AddTube(Vegetation::FTreeMesh& Mesh, const FMeshVec& Bottom, const FMeshVec& Top, double BottomRadius, double TopRadius,
		                    int32_t Sides, const uint8_t Colour[4])
		{
			const FMeshVec Axis = (Top - Bottom).Normalized();
			const FMeshVec Helper = std::fabs(Axis.Z) < 0.9 ? FMeshVec{ 0.0, 0.0, 1.0 } : FMeshVec{ 1.0, 0.0, 0.0 };
			const FMeshVec U = Helper.Cross(Axis).Normalized();
			const FMeshVec V = Axis.Cross(U);
			constexpr double TwoPi = 6.28318530717958647692;
			for (int32_t Side = 0; Side < Sides; ++Side)
			{
				const double A0 = TwoPi * Side / Sides, A1 = TwoPi * (Side + 1) / Sides;
				const FMeshVec D0 = U * std::cos(A0) + V * std::sin(A0);
				const FMeshVec D1 = U * std::cos(A1) + V * std::sin(A1);
				AddQuad(Mesh, Bottom + D0 * BottomRadius, Bottom + D1 * BottomRadius, Top + D1 * TopRadius, Top + D0 * TopRadius, Colour);
			}
			// Il tappo: un ventaglio di triangoli verso il centro.
			const uint32_t Centre = static_cast<uint32_t>(Mesh.VertexCount());
			PushVertex(Mesh, Top, Axis, Colour);
			for (int32_t Side = 0; Side < Sides; ++Side)
			{
				const double Angle = TwoPi * Side / Sides;
				PushVertex(Mesh, Top + (U * std::cos(Angle) + V * std::sin(Angle)) * TopRadius, Axis, Colour);
			}
			for (int32_t Side = 0; Side < Sides; ++Side)
			{
				const uint32_t A = Centre + 1 + static_cast<uint32_t>(Side);
				const uint32_t B = Centre + 1 + static_cast<uint32_t>((Side + 1) % Sides);
				Mesh.Indices.insert(Mesh.Indices.end(), { Centre, A, B });
			}
		}

		/** Una scatola allineata agli assi; Faces sono i colori delle facce [-X, +X, -Y, +Y, -Z, +Z]. */
		inline void AddBox(Vegetation::FTreeMesh& Mesh, const FMeshVec& Centre, const FMeshVec& Half, const uint8_t* Faces[6])
		{
			const double X0 = Centre.X - Half.X, X1 = Centre.X + Half.X;
			const double Y0 = Centre.Y - Half.Y, Y1 = Centre.Y + Half.Y;
			const double Z0 = Centre.Z - Half.Z, Z1 = Centre.Z + Half.Z;
			AddQuad(Mesh, { X0, Y1, Z0 }, { X0, Y0, Z0 }, { X0, Y0, Z1 }, { X0, Y1, Z1 }, Faces[0]);
			AddQuad(Mesh, { X1, Y0, Z0 }, { X1, Y1, Z0 }, { X1, Y1, Z1 }, { X1, Y0, Z1 }, Faces[1]);
			AddQuad(Mesh, { X0, Y0, Z0 }, { X1, Y0, Z0 }, { X1, Y0, Z1 }, { X0, Y0, Z1 }, Faces[2]);
			AddQuad(Mesh, { X1, Y1, Z0 }, { X0, Y1, Z0 }, { X0, Y1, Z1 }, { X1, Y1, Z1 }, Faces[3]);
			AddQuad(Mesh, { X0, Y0, Z0 }, { X0, Y1, Z0 }, { X1, Y1, Z0 }, { X1, Y0, Z0 }, Faces[4]);
			AddQuad(Mesh, { X0, Y0, Z1 }, { X1, Y0, Z1 }, { X1, Y1, Z1 }, { X0, Y1, Z1 }, Faces[5]);
		}
	}

	/**
	 * La forma di un tipo di palo, in metri, alla sua altezza di riferimento
	 * (LampShape): la base nell'origine, lo sbraccio verso +X, Z in alto. Il
	 * palo entra 0,6 m sotto il suolo (il terreno disegnato non e' mai
	 * esattamente piano). Il colore dei vertici: RGB il metallo (sRGB), A 255
	 * il vetro della lampada (il materiale lo accende).
	 * Simple: il LOD da lontano, con meno lati.
	 */
	inline void BuildLampMesh(ELampKind Kind, bool bSimple, Vegetation::FTreeMesh& Out)
	{
		using namespace StreetDetail;
		Out = Vegetation::FTreeMesh();
		const int32_t Sides = bSimple ? 4 : 8;
		const uint8_t Metal[4] = { 92, 96, 100, 0 };
		const uint8_t Dark[4] = { 46, 52, 48, 0 };
		const uint8_t Glass[4] = { 245, 240, 225, 255 };
		const FLampShape Shape = LampShape(Kind);
		if (Kind == ELampKind::Lantern)
		{
			const double Top = Shape.LightHeightM - 0.25;
			AddTube(Out, { 0.0, 0.0, -0.6 }, { 0.0, 0.0, Top }, 0.08, 0.06, Sides, Dark);
			const uint8_t* Faces[6] = { Glass, Glass, Glass, Glass, Dark, Dark };
			AddBox(Out, { 0.0, 0.0, Shape.LightHeightM }, { 0.17, 0.17, 0.25 }, Faces);
			AddTube(Out, { 0.0, 0.0, Shape.LightHeightM + 0.25 }, { 0.0, 0.0, Shape.LightHeightM + 0.5 }, 0.24, 0.03, Sides, Dark);
			return;
		}
		const double Head = Shape.LightHeightM + 0.08;
		const double PoleTop = Head - (Kind == ELampKind::Arm ? 0.25 : 0.1);
		AddTube(Out, { 0.0, 0.0, -0.6 }, { 0.0, 0.0, PoleTop }, Kind == ELampKind::Arm ? 0.12 : 0.09,
			Kind == ELampKind::Arm ? 0.06 : 0.05, Sides, Metal);
		// Lo sbraccio: dal palo alla testa, un po' in salita.
		const double HeadHalf = Kind == ELampKind::Arm ? 0.35 : 0.25;
		AddTube(Out, { 0.0, 0.0, PoleTop - 0.05 }, { Shape.ReachM - HeadHalf, 0.0, Head }, 0.045, 0.035, bSimple ? 3 : 6, Metal);
		const uint8_t* Faces[6] = { Metal, Metal, Metal, Metal, Glass, Metal };
		AddBox(Out, { Shape.ReachM, 0.0, Head }, { HeadHalf, Kind == ELampKind::Arm ? 0.16 : 0.12, 0.06 }, Faces);
	}

	/**
	 * Il quadrato dei punti luce: quattro vertici quasi nello stesso punto (5
	 * cm), l'angolo nel colore (R 0/255 a sinistra/destra, G in basso/in
	 * alto). Il materiale lo apre verso la camera (StreetLightShader.inl).
	 */
	inline void BuildSpriteQuad(Vegetation::FTreeMesh& Out)
	{
		using namespace StreetDetail;
		Out = Vegetation::FTreeMesh();
		const double H = 0.025;
		const uint8_t Corners[4][4] = { { 0, 0, 0, 0 }, { 255, 0, 0, 0 }, { 255, 255, 0, 0 }, { 0, 255, 0, 0 } };
		const FMeshVec Positions[4] = { { 0.0, -H, -H }, { 0.0, H, -H }, { 0.0, H, H }, { 0.0, -H, H } };
		for (int32_t Corner = 0; Corner < 4; ++Corner) { PushVertex(Out, Positions[Corner], { 1.0, 0.0, 0.0 }, Corners[Corner]); }
		Out.Indices = { 0, 1, 2, 0, 2, 3 };
	}
}

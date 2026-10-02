// =============================================================================
//  RoadRasterizer.h -- Disegna le linee di una tile vettoriale in un'immagine.
//  STRATO: C++ PURO, header-only.
//
//  E' il cuore della Fase 8: prende le strade, le ferrovie e le piste di una
//  tile vettoriale e produce, per UNA tile di terreno, un'immagine trasparente
//  da sovrapporre all'ortofoto nel materiale.
//
//  PERCHE' SU CPU, IN UN WORKER, E NON SULLA SCHEDA VIDEO
//  Sulla GPU si farebbe prima, ma sarebbe codice che qui non si puo' provare:
//  questo invece gira nei test standalone, dove si misura la larghezza di una
//  strada pixel per pixel. Il costo e' di qualche millisecondo per tile, su un
//  thread di lavoro: il game thread riceve pixel gia' pronti, come per le foto.
//
//  TRE SCELTE CHE CONTANO
//
//  1. Si lavora in METRI, non in pixel. Un pixel di una tile non e' quadrato
//     sul terreno: a 45 gradi di latitudine un grado di longitudine e' lungo il
//     71% di uno di latitudine. Una strada larga 7 m deve esserlo in tutte le
//     direzioni, quindi distanze e larghezze si misurano in metri.
//
//  2. Il bordo e' ANTIALIAS, con un filtro a scatola largo un pixel: la
//     copertura di un pixel e' la frazione della sua larghezza che cade dentro
//     la strada. Una strada piu' sottile di un pixel diventa una linea tenue
//     invece che seghettata o intermittente, ed e' quello che fa una foto.
//
//  3. L'immagine e' in ALFA PREMOLTIPLICATO: il colore e' gia' moltiplicato per
//     la copertura. Sembra un dettaglio, ma le mipmap e il filtro bilineare
//     fanno medie fra pixel, e mediare un pixel di strada con uno trasparente
//     (colore nero, alfa zero) in alfa normale scurisce i bordi a ogni livello
//     di mip. In premoltiplicato la media e' giusta per costruzione.
// =============================================================================
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "Tiles/ImageTileFormat.h"     // Detail::SrgbToLinear / LinearToSrgb
#include "Tiles/TileKey.h"
#include "Tiles/TilingScheme.h"
#include "Tiles/VectorTileFormat.h"

namespace GeoWorld::Roads
{
	using Tiles::ERoadClass;
	using Tiles::FTileKey;
	using Tiles::FVectorFeature;
	using Tiles::FVectorTile;

	// =========================================================================
	//  Stili
	// =========================================================================

	/** Come si disegna una classe di linea. Colore in luce LINEARE (0..1). */
	struct FRoadStyleEntry
	{
		float R = 0.0f, G = 0.0f, B = 0.0f;
		/** 0 = la classe non si disegna. */
		float Opacity = 0.0f;
		/** Moltiplica la larghezza vera: 1 = fedele. */
		float WidthScale = 1.0f;
		/** Larghezza minima in pixel: 0 = fisica (sotto il pixel si attenua). */
		float MinWidthPixels = 0.0f;
	};

	enum class ERoadStyle : uint8_t
	{
		/** Colori di asfalto, massicciata e cemento: si fondono con la foto. */
		Realistic,
		/** Colori da carta stradale e larghezza minima di un pixel: serve a
		 *  vedere da lontano se le strade stanno dove devono stare. */
		Map,
	};

	struct FRoadStyle
	{
		FRoadStyleEntry Classes[Tiles::RoadClassCount];
		/** Sostituisce il colore delle classi stradali sterrate. */
		FRoadStyleEntry Unpaved;

		/** Per valore: sono sei float, e cosi' lo stile si puo' condividere fra
		 *  thread senza pensarci (non c'e' niente di mutabile dentro). */
		FRoadStyleEntry For(const FVectorFeature& Feature) const
		{
			const int32_t Index = static_cast<int32_t>(Feature.Class);
			if (Index <= 0 || Index >= Tiles::RoadClassCount) { return FRoadStyleEntry{}; }
			const FRoadStyleEntry& Entry = Classes[Index];
			// Lo sterrato cambia colore solo alle strade: una ferrovia non e'
			// "sterrata" anche se qualcuno le ha messo surface=gravel.
			const bool bRoad = Index < static_cast<int32_t>(ERoadClass::Rail);
			if (Feature.IsUnpaved() && bRoad && Entry.Opacity > 0.0f && Unpaved.Opacity > 0.0f)
			{
				FRoadStyleEntry Result = Unpaved;
				Result.WidthScale = Entry.WidthScale;
				Result.MinWidthPixels = Entry.MinWidthPixels;
				Result.Opacity = std::min(Entry.Opacity, Unpaved.Opacity);
				return Result;
			}
			return Entry;
		}
	};

	namespace Detail
	{
		inline FRoadStyleEntry Srgb(int R, int G, int B, float Opacity,
		                            float WidthScale = 1.0f, float MinWidthPixels = 0.0f)
		{
			FRoadStyleEntry Entry;
			Entry.R = Tiles::Detail::SrgbToLinear(static_cast<uint8_t>(R));
			Entry.G = Tiles::Detail::SrgbToLinear(static_cast<uint8_t>(G));
			Entry.B = Tiles::Detail::SrgbToLinear(static_cast<uint8_t>(B));
			Entry.Opacity = Opacity;
			Entry.WidthScale = WidthScale;
			Entry.MinWidthPixels = MinWidthPixels;
			return Entry;
		}

		inline void SetClass(FRoadStyle& Style, ERoadClass Class, const FRoadStyleEntry& Entry)
		{
			Style.Classes[static_cast<int32_t>(Class)] = Entry;
		}
	}

	/**
	 * Gli stili predefiniti.
	 *
	 * I colori realistici sono quelli che hanno le cose viste dall'alto in una
	 * foto aerea d'estate, non quelli "veri" del materiale: l'asfalto
	 * invecchiato e' un grigio medio, non nero. L'opacita' non e' piena: un
	 * po' della foto sotto resta visibile, e la strada non sembra un adesivo.
	 */
	inline FRoadStyle MakeRoadStyle(ERoadStyle Which)
	{
		using Detail::SetClass;
		using Detail::Srgb;
		FRoadStyle Style;

		if (Which == ERoadStyle::Map)
		{
			const float W = 1.0f;     // larghezza fedele...
			const float M = 1.5f;     // ...ma mai sotto il pixel e mezzo
			SetClass(Style, ERoadClass::Motorway,     Srgb(233, 115,  45, 1.0f, W, M));
			SetClass(Style, ERoadClass::Trunk,        Srgb(240, 150,  70, 1.0f, W, M));
			SetClass(Style, ERoadClass::Primary,      Srgb(250, 190,  90, 1.0f, W, M));
			SetClass(Style, ERoadClass::Secondary,    Srgb(250, 220, 120, 1.0f, W, M));
			SetClass(Style, ERoadClass::Tertiary,     Srgb(255, 245, 180, 1.0f, W, M));
			SetClass(Style, ERoadClass::Unclassified, Srgb(255, 255, 255, 1.0f, W, 1.0f));
			SetClass(Style, ERoadClass::Residential,  Srgb(255, 255, 255, 1.0f, W, 1.0f));
			SetClass(Style, ERoadClass::LivingStreet, Srgb(235, 235, 235, 1.0f, W, 1.0f));
			SetClass(Style, ERoadClass::Service,      Srgb(225, 225, 225, 1.0f, W, 1.0f));
			SetClass(Style, ERoadClass::Pedestrian,   Srgb(220, 220, 235, 1.0f, W, 1.0f));
			SetClass(Style, ERoadClass::Track,        Srgb(160, 110,  60, 1.0f, W, 1.0f));
			SetClass(Style, ERoadClass::Path,         Srgb(230,  80,  80, 1.0f, W, 1.0f));
			SetClass(Style, ERoadClass::Rail,         Srgb( 30,  30,  30, 1.0f, W, M));
			SetClass(Style, ERoadClass::LightRail,    Srgb( 90,  90, 110, 1.0f, W, 1.0f));
			SetClass(Style, ERoadClass::RailService,  Srgb( 70,  70,  70, 1.0f, W, 1.0f));
			SetClass(Style, ERoadClass::Runway,       Srgb(110, 110, 140, 1.0f, W, M));
			SetClass(Style, ERoadClass::Taxiway,      Srgb(150, 150, 180, 1.0f, W, 1.0f));
			Style.Unpaved = Srgb(160, 110, 60, 1.0f);
			return Style;
		}

		SetClass(Style, ERoadClass::Motorway,     Srgb( 88,  88,  90, 0.92f));
		SetClass(Style, ERoadClass::Trunk,        Srgb( 90,  90,  92, 0.92f));
		SetClass(Style, ERoadClass::Primary,      Srgb( 96,  96,  98, 0.90f));
		SetClass(Style, ERoadClass::Secondary,    Srgb( 98,  98, 100, 0.88f));
		SetClass(Style, ERoadClass::Tertiary,     Srgb(100, 100, 102, 0.86f));
		SetClass(Style, ERoadClass::Unclassified, Srgb(104, 104, 104, 0.84f));
		SetClass(Style, ERoadClass::Residential,  Srgb(104, 104, 106, 0.84f));
		SetClass(Style, ERoadClass::LivingStreet, Srgb(118, 116, 114, 0.80f));
		SetClass(Style, ERoadClass::Service,      Srgb(110, 110, 110, 0.78f));
		SetClass(Style, ERoadClass::Pedestrian,   Srgb(150, 145, 138, 0.75f));
		SetClass(Style, ERoadClass::Track,        Srgb(140, 125, 100, 0.70f));
		SetClass(Style, ERoadClass::Path,         Srgb(160, 155, 148, 0.55f));
		SetClass(Style, ERoadClass::Rail,         Srgb(108,  98,  88, 0.90f));
		SetClass(Style, ERoadClass::LightRail,    Srgb(100,  95,  90, 0.80f));
		SetClass(Style, ERoadClass::RailService,  Srgb(108,  98,  88, 0.80f));
		SetClass(Style, ERoadClass::Runway,       Srgb(122, 122, 124, 0.95f));
		SetClass(Style, ERoadClass::Taxiway,      Srgb(112, 112, 114, 0.95f));
		Style.Unpaved = Srgb(142, 126, 100, 0.70f);
		return Style;
	}

	// =========================================================================
	//  Quale tile vettoriale, e quale pezzo, per una tile di terreno
	// =========================================================================

	/**
	 * Il livello vettoriale da usare per una tile di terreno: il piu' profondo
	 * disponibile che non superi quello del terreno. -1 se non ce n'e'.
	 *
	 * NON il piu' vicino in assoluto: una tile vettoriale piu' profonda del
	 * terreno ne coprirebbe solo un pezzo, e ne servirebbero quattro (o
	 * sedici) per una tile sola. Una meno profonda invece la contiene tutta:
	 * se ne disegna solo il pezzo che serve.
	 */
	inline int32_t ChooseVectorLevel(uint32_t TerrainLevel, const std::vector<uint32_t>& SortedLevels)
	{
		int32_t Best = -1;
		for (const uint32_t Level : SortedLevels)
		{
			if (Level <= TerrainLevel) { Best = static_cast<int32_t>(Level); }
		}
		return Best;
	}

	/** Il pezzo di tile vettoriale che copre una tile di terreno, in unita' locali. */
	struct FVectorWindow
	{
		FTileKey VectorKey;
		double U0 = 0.0;
		double V0 = 0.0;
		double Span = 0.0;
	};

	/**
	 * Calcola la finestra. Stessa aritmetica intera del ritaglio delle ortofoto
	 * (ImageryMapping.h): con Extent potenza di due le finestre cadono su
	 * numeri esatti, e due tile di terreno vicine si toccano senza fessure.
	 */
	inline FVectorWindow MakeVectorWindow(const FTileKey& TerrainKey, uint32_t VectorLevel, int32_t Extent)
	{
		FVectorWindow Window;
		if (VectorLevel >= TerrainKey.Level)
		{
			Window.VectorKey = TerrainKey;
			Window.Span = static_cast<double>(Extent);
			return Window;
		}
		const uint32_t Shift = TerrainKey.Level - VectorLevel;
		const uint32_t Divisions = 1u << Shift;
		Window.VectorKey = FTileKey{ VectorLevel, TerrainKey.X >> Shift, TerrainKey.Y >> Shift };
		Window.Span = static_cast<double>(Extent) / static_cast<double>(Divisions);
		Window.U0 = static_cast<double>(TerrainKey.X & (Divisions - 1)) * Window.Span;
		Window.V0 = static_cast<double>(TerrainKey.Y & (Divisions - 1)) * Window.Span;
		return Window;
	}

	/** Tutto cio' che serve al disegno, oltre alla tile e allo stile. */
	struct FRoadRasterRequest
	{
		/** Lato dell'immagine in pixel. */
		int32_t Size = 256;
		FVectorWindow Window;
		/** Metri per unita' locale, in x (est) e y (sud). */
		double MetresPerUnitX = 1.0;
		double MetresPerUnitY = 1.0;
	};

	/**
	 * Prepara la richiesta per una tile di terreno.
	 *
	 * I metri per unita' si calcolano alla latitudine del CENTRO della tile di
	 * terreno, non di quella vettoriale: e' piu' locale, e su 2 km la
	 * differenza di scala in longitudine e' sotto il millesimo.
	 */
	inline FRoadRasterRequest MakeRasterRequest(const FTileKey& TerrainKey, uint32_t VectorLevel,
	                                            int32_t Extent, int32_t Size)
	{
		constexpr double MetresPerDegreeLat = 111132.0;
		constexpr double MetresPerDegreeLonAtEquator = 111320.0;
		constexpr double DegreesToRadians = 3.14159265358979323846 / 180.0;

		FRoadRasterRequest Request;
		Request.Size = Size;
		Request.Window = MakeVectorWindow(TerrainKey, VectorLevel, Extent);

		const double SpanDeg = Tiles::TileSpanDeg(Request.Window.VectorKey.Level);
		const double CentreLat = Tiles::GetTileBounds(TerrainKey.Level, TerrainKey.X, TerrainKey.Y).CentreLat();
		Request.MetresPerUnitX = SpanDeg * MetresPerDegreeLonAtEquator
		                       * std::cos(CentreLat * DegreesToRadians) / static_cast<double>(Extent);
		Request.MetresPerUnitY = SpanDeg * MetresPerDegreeLat / static_cast<double>(Extent);
		return Request;
	}

	// =========================================================================
	//  Il disegno
	// =========================================================================

	struct FRoadRasterStats
	{
		int32_t FeaturesDrawn = 0;
		int32_t FeaturesSkipped = 0;
		int64_t SegmentsDrawn = 0;
		int64_t PixelsEvaluated = 0;
		/** Pixel con alfa diverso da zero nel risultato. */
		int64_t PixelsCovered = 0;
	};

	namespace Detail
	{
		/**
		 * Frazione di un pixel largo `Filter` (in metri) che cade dentro una
		 * striscia di mezza larghezza `Half`, quando il centro del pixel sta a
		 * distanza `Distance` dall'asse.
		 *
		 * E' l'antialias "a scatola". La forma piu' ovvia,
		 * clamp(Half - Distance + Filter/2), sbaglia proprio sulle linee
		 * sottili: con Half che tende a zero da' comunque mezzo pixel pieno
		 * sull'asse, cioe' disegna strade che non esistono. Questa invece da'
		 * 2*Half/Filter: una strada larga un quarto di pixel copre un quarto
		 * di pixel.
		 */
		inline float BoxCoverage(float Distance, float Half, float Filter)
		{
			const float Low = std::max(-Half, Distance - Filter * 0.5f);
			const float High = std::min(Half, Distance + Filter * 0.5f);
			return std::max(0.0f, High - Low) / Filter;
		}

		/** Distanza fra il punto P e il segmento AB (tutto in metri). */
		inline float DistanceToSegment(float Px, float Py, float Ax, float Ay, float Bx, float By)
		{
			const float Dx = Bx - Ax;
			const float Dy = By - Ay;
			const float LengthSq = Dx * Dx + Dy * Dy;
			float T = 0.0f;
			if (LengthSq > 0.0f)
			{
				T = ((Px - Ax) * Dx + (Py - Ay) * Dy) / LengthSq;
				T = std::min(1.0f, std::max(0.0f, T));
			}
			const float Cx = Ax + T * Dx - Px;
			const float Cy = Ay + T * Dy - Py;
			return std::sqrt(Cx * Cx + Cy * Cy);
		}

		/** Luce lineare -> sRGB a 8 bit, con una tabella da 4096 voci. */
		inline uint8_t LinearToSrgbFast(float Linear)
		{
			static const std::vector<uint8_t> Table = []()
			{
				std::vector<uint8_t> Values(4096);
				for (int Index = 0; Index < 4096; ++Index)
				{
					Values[static_cast<size_t>(Index)] =
						Tiles::Detail::LinearToSrgb((static_cast<float>(Index) + 0.5f) / 4096.0f);
				}
				return Values;
			}();
			const int Index = static_cast<int>(std::min(1.0f, std::max(0.0f, Linear)) * 4095.0f + 0.5f);
			return Table[static_cast<size_t>(Index)];
		}
	}

	/**
	 * Disegna la tile vettoriale nella finestra richiesta.
	 *
	 * `OutBGRA` riceve Size x Size pixel BGRA a 8 bit, riga 0 a NORD, colore
	 * premoltiplicato e codificato sRGB (la texture va creata con SRGB = true).
	 * Ritorna true se almeno un pixel non e' trasparente: se e' false la tile
	 * di terreno non ha bisogno di nessuna immagine.
	 *
	 * Le linee si disegnano nell'ordine del file, che la pipeline ha gia'
	 * ordinato: prima chi sta sotto.
	 */
	inline bool RasterizeRoads(const FVectorTile& Tile, const FRoadRasterRequest& Request,
	                           const FRoadStyle& Style, std::vector<uint8_t>& OutBGRA,
	                           FRoadRasterStats* OutStats = nullptr)
	{
		FRoadRasterStats Stats;
		const int32_t Size = Request.Size;
		OutBGRA.assign(static_cast<size_t>(Size) * Size * 4, 0);
		if (Size <= 0 || Request.Window.Span <= 0.0)
		{
			if (OutStats) { *OutStats = Stats; }
			return false;
		}

		// Dimensioni di un pixel sul terreno, in metri.
		const double UnitsPerPixel = Request.Window.Span / static_cast<double>(Size);
		const float PixelX = static_cast<float>(UnitsPerPixel * Request.MetresPerUnitX);
		const float PixelY = static_cast<float>(UnitsPerPixel * Request.MetresPerUnitY);
		if (!(PixelX > 0.0f) || !(PixelY > 0.0f))
		{
			if (OutStats) { *OutStats = Stats; }
			return false;
		}

		const size_t PixelCount = static_cast<size_t>(Size) * Size;
		std::vector<float> Accum(PixelCount * 4, 0.0f);   // RGBA lineare premoltiplicato
		std::vector<float> Cover(PixelCount, 0.0f);       // copertura della linea corrente

		// Punti della linea corrente, in metri, con l'origine nell'angolo
		// nord-ovest della finestra. Si riusa fra una linea e l'altra.
		std::vector<float> MetricX;
		std::vector<float> MetricY;

		const double U0 = Request.Window.U0;
		const double V0 = Request.Window.V0;
		const double U1 = U0 + Request.Window.Span;
		const double V1 = V0 + Request.Window.Span;

		for (const FVectorFeature& Feature : Tile.Features)
		{
			const FRoadStyleEntry Entry = Style.For(Feature);
			if (Entry.Opacity <= 0.0f || Feature.PointCount < 2)
			{
				++Stats.FeaturesSkipped;
				continue;
			}

			const float TrueHalf = 0.5f * Feature.GetWidthMetres() * Entry.WidthScale;
			// Il caso peggiore, per lo scarto veloce qui sotto: l'impronta di un
			// pixel non supera mai PixelX + PixelY in nessuna direzione.
			const float MaxFilter = PixelX + PixelY;
			const float Reach = std::max(TrueHalf, 0.5f * Entry.MinWidthPixels * MaxFilter) + MaxFilter;

			// Scarto veloce: la linea sta tutta lontana dalla finestra?
			const double MarginU = Reach / Request.MetresPerUnitX;
			const double MarginV = Reach / Request.MetresPerUnitY;
			if (Feature.MaxX < U0 - MarginU || Feature.MinX > U1 + MarginU ||
			    Feature.MaxY < V0 - MarginV || Feature.MinY > V1 + MarginV)
			{
				++Stats.FeaturesSkipped;
				continue;
			}

			MetricX.resize(Feature.PointCount);
			MetricY.resize(Feature.PointCount);
			for (uint32_t Index = 0; Index < Feature.PointCount; ++Index)
			{
				const Tiles::FVectorPoint& Point = Tile.Points[Feature.FirstPoint + Index];
				MetricX[Index] = static_cast<float>((Point.X - U0) * Request.MetresPerUnitX);
				MetricY[Index] = static_cast<float>((Point.Y - V0) * Request.MetresPerUnitY);
			}

			// Rettangolo dei pixel toccati da questa linea: si azzera alla fine.
			int32_t DirtyX0 = Size, DirtyY0 = Size, DirtyX1 = -1, DirtyY1 = -1;

			for (uint32_t Index = 0; Index + 1 < Feature.PointCount; ++Index)
			{
				const float Ax = MetricX[Index], Ay = MetricY[Index];
				const float Bx = MetricX[Index + 1], By = MetricY[Index + 1];

				// Un segmento lungo si spezza in pezzi da 16 pixel. La distanza
				// da un segmento e' il minimo delle distanze dai suoi pezzi, quindi
				// il risultato non cambia; cambia il lavoro: un segmento in
				// diagonale su tutta la tile toccherebbe tutti i pixel del suo
				// rettangolo, cioe' l'intera immagine.
				const float LengthPixels = std::sqrt(((Bx - Ax) / PixelX) * ((Bx - Ax) / PixelX)
				                                   + ((By - Ay) / PixelY) * ((By - Ay) / PixelY));
				const int32_t Pieces = std::max(1, static_cast<int32_t>(std::ceil(LengthPixels / 16.0f)));

				// L'IMPRONTA DEL PIXEL nella direzione perpendicolare alla strada.
				// I pixel non sono quadrati (3,4 x 4,8 m a Torino): una strada
				// orizzontale li attraversa nel senso dei 4,8 m, una verticale
				// in quello dei 3,4. Il filtro dell'antialias deve essere largo
				// esattamente quanto il passo dei pixel lungo la sezione: solo
				// cosi' la somma delle coperture lungo una sezione da' la
				// larghezza vera, qualunque sia la posizione della strada
				// rispetto alla griglia. Con un filtro "medio" una strada da
				// 10 m ne misurava 9,5, e una sottile spariva a tratti.
				const float SegmentLength = std::sqrt((Bx - Ax) * (Bx - Ax) + (By - Ay) * (By - Ay));
				const float NormalX = (SegmentLength > 0.0f) ? std::fabs(By - Ay) / SegmentLength : 0.7071f;
				const float NormalY = (SegmentLength > 0.0f) ? std::fabs(Bx - Ax) / SegmentLength : 0.7071f;
				const float Filter = NormalX * PixelX + NormalY * PixelY;
				const float Half = std::max(TrueHalf, 0.5f * Entry.MinWidthPixels * Filter);
				const float SegmentReach = Half + Filter;

				for (int32_t Piece = 0; Piece < Pieces; ++Piece)
				{
					const float T0 = static_cast<float>(Piece) / static_cast<float>(Pieces);
					const float T1 = static_cast<float>(Piece + 1) / static_cast<float>(Pieces);
					const float Px0 = Ax + (Bx - Ax) * T0, Py0 = Ay + (By - Ay) * T0;
					const float Px1 = Ax + (Bx - Ax) * T1, Py1 = Ay + (By - Ay) * T1;

					const int32_t X0 = std::max(0, static_cast<int32_t>(std::floor((std::min(Px0, Px1) - SegmentReach) / PixelX)));
					const int32_t X1 = std::min(Size - 1, static_cast<int32_t>(std::floor((std::max(Px0, Px1) + SegmentReach) / PixelX)));
					const int32_t Y0 = std::max(0, static_cast<int32_t>(std::floor((std::min(Py0, Py1) - SegmentReach) / PixelY)));
					const int32_t Y1 = std::min(Size - 1, static_cast<int32_t>(std::floor((std::max(Py0, Py1) + SegmentReach) / PixelY)));
					if (X0 > X1 || Y0 > Y1) { continue; }

					++Stats.SegmentsDrawn;
					DirtyX0 = std::min(DirtyX0, X0); DirtyX1 = std::max(DirtyX1, X1);
					DirtyY0 = std::min(DirtyY0, Y0); DirtyY1 = std::max(DirtyY1, Y1);

					for (int32_t Row = Y0; Row <= Y1; ++Row)
					{
						const float CentreY = (static_cast<float>(Row) + 0.5f) * PixelY;
						float* CoverRow = Cover.data() + static_cast<size_t>(Row) * Size;
						for (int32_t Column = X0; Column <= X1; ++Column)
						{
							const float CentreX = (static_cast<float>(Column) + 0.5f) * PixelX;
							const float Distance = Detail::DistanceToSegment(CentreX, CentreY, Px0, Py0, Px1, Py1);
							if (Distance >= SegmentReach) { continue; }
							// Il MASSIMO, non la somma: dove due segmenti della
							// stessa strada si incontrano, il pixel e' coperto
							// una volta sola, non due.
							CoverRow[Column] = std::max(CoverRow[Column],
								Detail::BoxCoverage(Distance, Half, Filter));
						}
						Stats.PixelsEvaluated += (X1 - X0 + 1);
					}
				}
			}

			if (DirtyX1 < DirtyX0)
			{
				++Stats.FeaturesSkipped;
				continue;
			}
			++Stats.FeaturesDrawn;

			// Composizione "sopra" in premoltiplicato: nuovo = sorgente +
			// vecchio * (1 - alfa sorgente). E si azzera la copertura per la
			// linea successiva, ma solo dove la si e' scritta.
			for (int32_t Row = DirtyY0; Row <= DirtyY1; ++Row)
			{
				for (int32_t Column = DirtyX0; Column <= DirtyX1; ++Column)
				{
					const size_t Pixel = static_cast<size_t>(Row) * Size + Column;
					const float Coverage = Cover[Pixel];
					if (Coverage <= 0.0f) { continue; }
					Cover[Pixel] = 0.0f;

					const float Alpha = Coverage * Entry.Opacity;
					const float Keep = 1.0f - Alpha;
					float* Out = Accum.data() + Pixel * 4;
					Out[0] = Entry.R * Alpha + Out[0] * Keep;
					Out[1] = Entry.G * Alpha + Out[1] * Keep;
					Out[2] = Entry.B * Alpha + Out[2] * Keep;
					Out[3] = Alpha + Out[3] * Keep;
				}
			}
		}

		bool bAnything = false;
		for (size_t Pixel = 0; Pixel < PixelCount; ++Pixel)
		{
			const float* In = Accum.data() + Pixel * 4;
			const uint8_t Alpha = static_cast<uint8_t>(std::lround(std::min(1.0f, In[3]) * 255.0f));
			if (Alpha == 0) { continue; }
			bAnything = true;
			++Stats.PixelsCovered;
			uint8_t* Out = OutBGRA.data() + Pixel * 4;
			Out[0] = Detail::LinearToSrgbFast(In[2]);   // B
			Out[1] = Detail::LinearToSrgbFast(In[1]);   // G
			Out[2] = Detail::LinearToSrgbFast(In[0]);   // R
			Out[3] = Alpha;
		}

		if (OutStats) { *OutStats = Stats; }
		return bAnything;
	}
}

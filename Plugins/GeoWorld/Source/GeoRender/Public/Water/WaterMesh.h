// =============================================================================
//  WaterMesh.h -- Lo scavo del terreno, la superficie dell'acqua, il raster.
//  STRATO: C++ PURO, header-only.
//
//  Tre lavori, tutti per UNA tile di terreno e sui thread di lavoro:
//
//   1. SCAVARE il terreno sotto l'acqua (CarveHeights): ogni post scende fino
//      al fondale della tile d'acqua. Senza, la superficie dell'acqua
//      coinciderebbe con il terreno (il DEM sopra un lago e' piano alla quota
//      del lago; il mare e' il geoide) e non ci sarebbe niente sotto da
//      vedere in trasparenza.
//   2. La MESH della superficie (BuildWaterMesh): una griglia alla quota
//      dell'acqua, solo nelle celle vicine all'acqua, nello STESSO frame
//      locale della mesh del terreno (stessa origine): il componente
//      dell'acqua usa la trasformazione della tile.
//   3. I PIXEL del raster per la scheda video (BuildWaterRasterPixels):
//      distanza dalla riva, onde, corrente.
//
//  LA VISTA. La tile d'acqua giusta e' quella dello stesso livello del
//  terreno; se non c'e' (il dataset d'acqua si ferma prima, o non e' ancora
//  caricata) si usa quella di un antenato, ritagliata sul pezzo che copre la
//  tile: FWaterView fa il conto una volta, come FDrapeTransform per le foto.
// =============================================================================
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <unordered_map>
#include <vector>

#include "Geo/Ellipsoid.h"
#include "Mesh/TileMesh.h"
#include "Tiles/TileFormat.h"
#include "Tiles/TilingScheme.h"
#include "Tiles/WaterTileFormat.h"

namespace GeoWorld::Water
{
	using Core::FEcef;
	using Core::FGeodetic;
	using Core::FMat3;
	using Mesh::FTileMeshData;
	using Tiles::FHeightTile;
	using Tiles::FTileKey;
	using Tiles::FWaterTile;

	/** Una tile d'acqua vista da una tile di terreno: la stessa, o un antenato ritagliato. */
	struct FWaterView
	{
		const FWaterTile* Tile = nullptr;
		FTileKey Terrain;
		/** La tile di terreno occupa [Offset, Offset + Scale] della tile d'acqua, in U e in V. */
		double OffsetU = 0.0, OffsetV = 0.0, Scale = 1.0;

		bool IsValid() const { return Tile != nullptr && Tile->IsValid(); }

		/** Vista di `Water` dalla tile di terreno `TerrainKey`. Non valida se `Water` non e' un antenato. */
		static FWaterView Make(const FWaterTile& Water, const FTileKey& TerrainKey)
		{
			FWaterView View;
			View.Terrain = TerrainKey;
			if (Water.Key.Level > TerrainKey.Level) { return View; }
			const uint32_t Depth = TerrainKey.Level - Water.Key.Level;
			if ((TerrainKey.X >> Depth) != Water.Key.X || (TerrainKey.Y >> Depth) != Water.Key.Y) { return View; }
			View.Tile = &Water;
			View.Scale = 1.0 / static_cast<double>(1u << Depth);
			View.OffsetU = static_cast<double>(TerrainKey.X - (Water.Key.X << Depth)) * View.Scale;
			View.OffsetV = static_cast<double>(TerrainKey.Y - (Water.Key.Y << Depth)) * View.Scale;
			return View;
		}

		/**
		 * Un campo dei post (superficie o fondale) al post (I, J) del TERRENO,
		 * 0..128. Bilineare sui post validi; NaN se i validi pesano meno di
		 * meta' (bordo dell'acqua: meglio niente che un valore inventato).
		 */
		float PostField(const std::vector<float>& Field, double I, double J) const
		{
			constexpr int32_t Cells = Tiles::WaterPosts - 1;
			const double Gx = (OffsetU + Scale * I / Cells) * Cells;
			const double Gy = (OffsetV + Scale * J / Cells) * Cells;
			const int32_t X0 = std::min(Cells - 1, std::max(0, static_cast<int32_t>(std::floor(Gx))));
			const int32_t Y0 = std::min(Cells - 1, std::max(0, static_cast<int32_t>(std::floor(Gy))));
			const double Ax = std::min(1.0, std::max(0.0, Gx - X0));
			const double Ay = std::min(1.0, std::max(0.0, Gy - Y0));
			double Sum = 0.0, Weight = 0.0;
			const double Weights[4] = { (1 - Ax) * (1 - Ay), Ax * (1 - Ay), (1 - Ax) * Ay, Ax * Ay };
			const int32_t Xs[4] = { X0, X0 + 1, X0, X0 + 1 };
			const int32_t Ys[4] = { Y0, Y0, Y0 + 1, Y0 + 1 };
			for (int Corner = 0; Corner < 4; ++Corner)
			{
				const float Value = Field[static_cast<size_t>(Ys[Corner]) * Tiles::WaterPosts + Xs[Corner]];
				if (std::isfinite(Value) && Weights[Corner] > 0.0)
				{
					Sum += Value * Weights[Corner];
					Weight += Weights[Corner];
				}
			}
			return (Weight >= 0.5) ? static_cast<float>(Sum / Weight) : std::numeric_limits<float>::quiet_NaN();
		}

		float SurfaceAtPost(double I, double J) const { return PostField(Tile->Surface, I, J); }
		float BedAtPost(double I, double J) const { return PostField(Tile->Bed, I, J); }

		/** Un canale del raster in (U, V) della tile di TERRENO (0..1), bilineare. */
		template <typename TValue>
		double RasterField(const std::vector<TValue>& Field, double U, double V) const
		{
			constexpr int32_t Size = Tiles::WaterRaster;
			const double Fx = (OffsetU + Scale * U) * Size - 0.5;
			const double Fy = (OffsetV + Scale * V) * Size - 0.5;
			const int32_t X0 = std::min(Size - 2, std::max(0, static_cast<int32_t>(std::floor(Fx))));
			const int32_t Y0 = std::min(Size - 2, std::max(0, static_cast<int32_t>(std::floor(Fy))));
			const double Ax = std::min(1.0, std::max(0.0, Fx - X0));
			const double Ay = std::min(1.0, std::max(0.0, Fy - Y0));
			auto At = [&Field](int32_t X, int32_t Y) { return static_cast<double>(Field[static_cast<size_t>(Y) * Size + X]); };
			return At(X0, Y0) * (1 - Ax) * (1 - Ay) + At(X0 + 1, Y0) * Ax * (1 - Ay)
			     + At(X0, Y0 + 1) * (1 - Ax) * Ay + At(X0 + 1, Y0 + 1) * Ax * Ay;
		}

		/**
		 * Distanza dalla riva in pixel della tile di TERRENO (positiva in
		 * acqua). Un pixel dell'antenato vale 1/Scale pixel della tile: la
		 * distanza si riscala, cosi' il materiale fa sempre lo stesso conto.
		 */
		float SdfPixelsAt(double U, double V) const
		{
			const double Encoded = RasterField(Tile->Sdf, U, V);
			return static_cast<float>((Encoded - Tiles::WaterSdfZero) / Tiles::WaterSdfScale / Scale);
		}
	};

	/**
	 * Scava il terreno: ogni post scende al fondale, dove il fondale c'e' ed e'
	 * piu' basso. Aggiorna le quote estreme (servono all'origine della mesh).
	 * Ritorna quanti post sono stati abbassati.
	 */
	inline int32_t CarveHeights(FHeightTile& Tile, const FWaterView& View)
	{
		if (!Tile.IsValid() || !View.IsValid()) { return 0; }
		int32_t Carved = 0;
		for (int32_t J = 0; J < Tiles::TilePosts; ++J)
		{
			for (int32_t I = 0; I < Tiles::TilePosts; ++I)
			{
				const float Bed = View.BedAtPost(I, J);
				float& Height = Tile.Heights[static_cast<size_t>(J) * Tiles::TilePosts + I];
				if (std::isfinite(Bed) && Bed < Height)
				{
					Height = Bed;
					++Carved;
				}
			}
		}
		if (Carved > 0)
		{
			const auto Extremes = std::minmax_element(Tile.Heights.begin(), Tile.Heights.end());
			Tile.MinHeight = *Extremes.first;
			Tile.MaxHeight = *Extremes.second;
		}
		return Carved;
	}

	struct FWaterMeshParameters
	{
		/** Passo della mesh del terreno (1, 2, 4...): la superficie usa lo stesso. */
		int32_t Step = 2;
		bool bFlipWinding = true;
		/** Gonna sui bordi della tile, contro le fessure fra livelli diversi. */
		double SkirtDepthMetres = 2.0;
		/** Una cella si disegna se l'acqua e' a meno di cosi' (pixel della tile). */
		float NearShorePixels = 3.0f;
		/**
		 * L'acqua "appoggiata": dove il file non porta la superficie (un'acqua
		 * troppo stretta per scavarla a questo livello, vedi sotto) la
		 * superficie sta sul terreno, cosi' tanti metri sopra.
		 */
		double DrapeLiftMetres = 0.3;
	};

	/**
	 * La superficie dell'acqua di una tile di terreno.
	 *
	 * `CarvedTerrain` deve essere la tile di terreno GIA' SCAVATA, la stessa da
	 * cui si costruisce la mesh del terreno: da lei viene l'origine del frame
	 * locale (centro della tile, a meta' fra le quote estreme), e con la stessa
	 * origine il componente dell'acqua puo' usare la trasformazione della tile.
	 *
	 * Normali: l'alto dell'ellissoide nel punto. Le onde (Gerstner) e il
	 * dettaglio le aggiunge il materiale.
	 *
	 * DUE MODI, VERTICE PER VERTICE
	 *  - Il file porta la SUPERFICIE: il vertice sta li', in piano. E' l'acqua
	 *    abbastanza larga da essere scavata a questo livello (la pipeline la
	 *    allarga di qualche post oltre la riva, cosi' le celle a cavallo della
	 *    riva la hanno in tutti gli angoli).
	 *  - Non la porta: e' un'acqua piu' stretta di qualche cella della mesh (un
	 *    torrente, o il Po visto da 20 km). Scavarla farebbe un pettine di buchi:
	 *    il vertice si APPOGGIA sul terreno scavato, DrapeLiftMetres sopra. Le
	 *    due mesh hanno gli stessi vertici e la stessa diagonale, quindi l'acqua
	 *    sta sopra il terreno di DrapeLiftMetres in ogni punto della cella, e la
	 *    forma la disegna la distanza dalla riva nel materiale.
	 * Una cella si disegna se l'acqua e' vicina (NearShorePixels).
	 */
	inline void BuildWaterMesh(const FHeightTile& CarvedTerrain, const FWaterView& View,
	                           const FWaterMeshParameters& Parameters, FTileMeshData& Out,
	                           const Core::FEllipsoid& Ellipsoid = Core::WGS84)
	{
		Out = FTileMeshData();
		if (!CarvedTerrain.IsValid() || !View.IsValid()) { return; }

		const int32_t Step = (Parameters.Step >= 1 && Tiles::TileCells % Parameters.Step == 0) ? Parameters.Step : 1;
		const int32_t Cells = Tiles::TileCells / Step;
		const int32_t Posts = Cells + 1;
		const Tiles::FTileBounds Bounds = Tiles::GetTileBounds(CarvedTerrain.Key.Level, CarvedTerrain.Key.X, CarvedTerrain.Key.Y);
		const double Spacing = Tiles::PostSpacingDeg(CarvedTerrain.Key.Level) * Step;

		Out.Origin = FGeodetic::FromDegrees(Bounds.CentreLat(), Bounds.CentreLon(),
		                                    0.5 * (CarvedTerrain.MinHeight + CarvedTerrain.MaxHeight));
		Out.MinHeight = CarvedTerrain.MinHeight;
		Out.MaxHeight = CarvedTerrain.MaxHeight;
		const FEcef OriginEcef = Core::GeodeticToEcef(Out.Origin, Ellipsoid);
		const FMat3 EcefToLocal = Mesh::Detail::MakeLocalNeu(Out.Origin);

		// Le quote dei post della mesh: la superficie, o il terreno + il
		// sollevamento dove la superficie non c'e' (vedi sopra).
		std::vector<float> Surface(static_cast<size_t>(Posts) * Posts);
		for (int32_t J = 0; J < Posts; ++J)
		{
			for (int32_t I = 0; I < Posts; ++I)
			{
				const float Level = View.SurfaceAtPost(I * Step, J * Step);
				Surface[static_cast<size_t>(J) * Posts + I] = std::isfinite(Level) ? Level
					: CarvedTerrain.GetHeight(I * Step, J * Step) + static_cast<float>(Parameters.DrapeLiftMetres);
			}
		}

		// Le celle da disegnare: quelle con l'acqua vicina (la distanza dalla
		// riva negli angoli e al centro). La riva vera la taglia il materiale:
		// la cella puo' sporgere sulla terra, il pixel no.
		const double CellUv = 1.0 / Cells;
		std::vector<uint8_t> Drawn(static_cast<size_t>(Cells) * Cells, 0);
		for (int32_t J = 0; J < Cells; ++J)
		{
			for (int32_t I = 0; I < Cells; ++I)
			{
				float Nearest = -1.0e9f;
				for (const double Du : { 0.0, 0.5, 1.0 })
				{
					for (const double Dv : { 0.0, 0.5, 1.0 })
					{
						Nearest = std::max(Nearest, View.SdfPixelsAt((I + Du) * CellUv, (J + Dv) * CellUv));
					}
				}
				if (Nearest > -Parameters.NearShorePixels) { Drawn[static_cast<size_t>(J) * Cells + I] = 1; }
			}
		}

		std::unordered_map<uint64_t, uint32_t> VertexOf;
		auto LocalAt = [&](double Lon, double Lat, double Height)
		{
			return EcefToLocal.Transform(Core::GeodeticToEcef(
				FGeodetic::FromRadians(Lat * Core::DegToRad, Lon * Core::DegToRad, Height), Ellipsoid) - OriginEcef);
		};
		auto AddVertex = [&](int32_t I, int32_t J, double Drop) -> uint32_t
		{
			const uint64_t Key = (static_cast<uint64_t>(Drop > 0.0 ? 1 : 0) << 40) | (static_cast<uint64_t>(J) << 20) | static_cast<uint64_t>(I);
			const auto Found = VertexOf.find(Key);
			if (Found != VertexOf.end()) { return Found->second; }

			const double Lon = Bounds.West + I * Spacing;
			const double Lat = Bounds.North - J * Spacing;
			const double Height = Surface[static_cast<size_t>(J) * Posts + I] - Drop;
			const FEcef Local = LocalAt(Lon, Lat, Height);
			// L'alto del punto (normale all'ellissoide) nel frame della tile.
			const double LatRad = Lat * Core::DegToRad, LonRad = Lon * Core::DegToRad;
			const FEcef Up = EcefToLocal.Transform(FEcef{ std::cos(LatRad) * std::cos(LonRad),
			                                               std::cos(LatRad) * std::sin(LonRad), std::sin(LatRad) });
			const uint32_t Index = static_cast<uint32_t>(Out.Positions.size() / 3);
			Out.Positions.insert(Out.Positions.end(), { static_cast<float>(Local.X), static_cast<float>(Local.Y), static_cast<float>(Local.Z) });
			Out.Normals.insert(Out.Normals.end(), { static_cast<float>(Up.X), static_cast<float>(Up.Y), static_cast<float>(Up.Z) });
			Out.UVs.insert(Out.UVs.end(), { static_cast<float>(I) / Cells, static_cast<float>(J) / Cells });
			Out.BoundingRadiusMetres = std::max(Out.BoundingRadiusMetres, std::sqrt(Local.LengthSquared()));
			VertexOf.emplace(Key, Index);
			return Index;
		};
		auto AddTriangle = [&](uint32_t A, uint32_t B, uint32_t C)
		{
			if (Parameters.bFlipWinding) { std::swap(B, C); }
			Out.Indices.insert(Out.Indices.end(), { A, B, C });
		};

		// Stessa triangolazione del terreno (diagonale nord-ovest / sud-est).
		for (int32_t J = 0; J < Cells; ++J)
		{
			for (int32_t I = 0; I < Cells; ++I)
			{
				if (!Drawn[static_cast<size_t>(J) * Cells + I]) { continue; }
				const uint32_t TopLeft = AddVertex(I, J, 0.0), TopRight = AddVertex(I + 1, J, 0.0);
				const uint32_t BottomLeft = AddVertex(I, J + 1, 0.0), BottomRight = AddVertex(I + 1, J + 1, 0.0);
				AddTriangle(TopLeft, TopRight, BottomRight);
				AddTriangle(TopLeft, BottomRight, BottomLeft);
			}
		}

		// Le gonne sui bordi della TILE (non sulla riva: li' taglia il
		// materiale). Ogni bordo percorso con l'interno a sinistra, come nel
		// terreno, perche' l'orientamento sia lo stesso.
		if (Parameters.SkirtDepthMetres > 0.0)
		{
			struct FEdge { int32_t StartI, StartJ, StepI, StepJ, CellI, CellJ; };
			const FEdge Edges[4] = {
				{ 0, 0, 1, 0, 0, 0 },                   // nord, verso est
				{ Cells, 0, 0, 1, -1, 0 },              // est, verso sud
				{ Cells, Cells, -1, 0, -1, -1 },        // sud, verso ovest
				{ 0, Cells, 0, -1, 0, -1 },             // ovest, verso nord
			};
			for (const FEdge& Edge : Edges)
			{
				for (int32_t Walk = 0; Walk < Cells; ++Walk)
				{
					const int32_t I0 = Edge.StartI + Edge.StepI * Walk, J0 = Edge.StartJ + Edge.StepJ * Walk;
					const int32_t I1 = I0 + Edge.StepI, J1 = J0 + Edge.StepJ;
					// La cella che sta dentro, accanto a questo tratto di bordo.
					const int32_t CellI = std::min(I0, I1) + (Edge.StepI == 0 ? Edge.CellI : 0);
					const int32_t CellJ = std::min(J0, J1) + (Edge.StepJ == 0 ? Edge.CellJ : 0);
					if (CellI < 0 || CellJ < 0 || CellI >= Cells || CellJ >= Cells) { continue; }
					if (!Drawn[static_cast<size_t>(CellJ) * Cells + CellI]) { continue; }
					const uint32_t Top0 = AddVertex(I0, J0, 0.0), Top1 = AddVertex(I1, J1, 0.0);
					const uint32_t Low0 = AddVertex(I0, J0, Parameters.SkirtDepthMetres);
					const uint32_t Low1 = AddVertex(I1, J1, Parameters.SkirtDepthMetres);
					AddTriangle(Top0, Low0, Low1);
					AddTriangle(Top0, Low1, Top1);
				}
			}
		}

		Out.InteriorVertexCount = static_cast<uint32_t>(Out.Positions.size() / 3);
		Out.TriangleCount = static_cast<uint32_t>(Out.Indices.size() / 3);
	}

	/**
	 * I pixel del raster per la scheda video: 256 x 256 BGRA, NON sRGB (sono
	 * dati, non colori). R = distanza dalla riva (come nel file, ma in pixel
	 * della tile di terreno), G = onde, B = corrente verso est + 128, A =
	 * corrente verso sud + 128. Con una vista su un antenato si ricampiona la
	 * finestra.
	 */
	inline void BuildWaterRasterPixels(const FWaterView& View, std::vector<uint8_t>& OutBGRA)
	{
		constexpr int32_t Size = Tiles::WaterRaster;
		OutBGRA.assign(static_cast<size_t>(Size) * Size * 4, 0);
		if (!View.IsValid()) { return; }
		const FWaterTile& Tile = *View.Tile;
		const bool bFlow = Tile.HasFlow();
		const bool bSame = View.Scale >= 1.0;
		auto Byte = [](double Value) { return static_cast<uint8_t>(std::min(255.0, std::max(0.0, std::round(Value)))); };

		for (int32_t Row = 0; Row < Size; ++Row)
		{
			for (int32_t Column = 0; Column < Size; ++Column)
			{
				uint8_t* Pixel = OutBGRA.data() + (static_cast<size_t>(Row) * Size + Column) * 4;
				const size_t Source = static_cast<size_t>(Row) * Size + Column;
				if (bSame)
				{
					Pixel[2] = Tile.Sdf[Source];
					Pixel[1] = Tile.Wave[Source];
					Pixel[0] = static_cast<uint8_t>(bFlow ? Tile.FlowU[Source] + 128 : 128);
					Pixel[3] = static_cast<uint8_t>(bFlow ? Tile.FlowV[Source] + 128 : 128);
					continue;
				}
				const double U = (Column + 0.5) / Size, V = (Row + 0.5) / Size;
				Pixel[2] = Byte(Tiles::WaterSdfZero + Tiles::WaterSdfScale * View.SdfPixelsAt(U, V));
				Pixel[1] = Byte(View.RasterField(Tile.Wave, U, V));
				Pixel[0] = bFlow ? Byte(View.RasterField(Tile.FlowU, U, V) + 128.0) : 128;
				Pixel[3] = bFlow ? Byte(View.RasterField(Tile.FlowV, U, V) + 128.0) : 128;
			}
		}
	}
}

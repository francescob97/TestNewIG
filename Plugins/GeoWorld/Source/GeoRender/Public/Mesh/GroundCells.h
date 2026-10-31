// =============================================================================
//  GroundCells.h -- Celle attorno alla camera, a bande, sul terreno DISEGNATO.
//  STRATO: C++ PURO, header-only.
//
//  Quello che serve a chiunque metta oggetti sul terreno a celle attorno
//  alla camera: gli alberi (Vegetation/VegetationCells.h) e gli edifici
//  (Buildings/BuildingCells.h). Provato dai test di tutti e due.
//
//   - QUALI CELLE: quelle con un punto entro il raggio, dalla piu' vicina,
//     contando anche l'altezza della camera sopra il suolo (CellsAround);
//   - LE BANDE: vicino, medio, lontano, fuori, con l'ISTERESI (BandFor): per
//     cambiare banda si supera il confine di qualche decina di metri, o una
//     cella sul confine cambierebbe a ogni passo della camera;
//   - IL SUOLO: la quota e la pendenza del terreno DISEGNATO, dalle tile di
//     terreno visibili (FGroundSurfaces), e la FIRMA di quelle tile
//     (SurfaceSignature): quando il terreno sotto si affina la firma cambia,
//     e la cella si rifa' sul terreno nuovo.
// =============================================================================
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

#include "Geo/Ellipsoid.h"
#include "Mesh/TileMesh.h"
#include "Tiles/TileKey.h"
#include "Tiles/TilingScheme.h"

namespace GeoWorld::Cells
{
	// =========================================================================
	//  Dal punto geografico al frame locale della cella
	// =========================================================================

	/**
	 * Il frame di una cella: origine al suo centro sull'ellissoide, assi NEU
	 * (nord, est, alto: gli stessi delle tile del terreno, vedi TileMesh.h).
	 * Le istanze degli alberi e i vertici degli edifici stanno qui, in metri; la trasformazione del
	 * componente porta il frame nel mondo (e il rebase cambia solo quella).
	 */
	struct FCellFrame
	{
		Core::FGeodetic Origin;
		Core::FEcef OriginEcef;
		Core::FMat3 EcefToLocal;

		static FCellFrame At(double LatDeg, double LonDeg)
		{
			FCellFrame Frame;
			Frame.Origin = Core::FGeodetic::FromDegrees(LatDeg, LonDeg, 0.0);
			Frame.OriginEcef = Core::GeodeticToEcef(Frame.Origin, Core::WGS84);
			Frame.EcefToLocal = Core::MakeNeuBasis(Frame.Origin.LatRad, Frame.Origin.LonRad);
			return Frame;
		}

		/** Il punto (gradi, quota ellissoidica) in metri nel frame: nord, est, alto. */
		void Local(double LatDeg, double LonDeg, double HeightM, double Out[3]) const
		{
			const Core::FEcef Point = Core::GeodeticToEcef(Core::FGeodetic::FromDegrees(LatDeg, LonDeg, HeightM), Core::WGS84);
			const Core::FEcef Local = EcefToLocal.Transform(Point - OriginEcef);
			Out[0] = Local.X;
			Out[1] = Local.Y;
			Out[2] = Local.Z;
		}
	};

	/** Le bande, dalla camera: 0 vicino, 1 medio, 2 lontano, 3 fuori. */
	inline constexpr int32_t BandOutside = 3;

	struct FBandSettings
	{
		// I default sono quelli degli alberi; gli edifici hanno i loro.
		/** Fin qui la banda vicina (tutto, con l'ombra). */
		double NearM = 700.0;
		/** Fin qui la banda media. */
		double MidM = 1500.0;
		/** Fin qui la banda lontana; oltre niente: c'e' la foto. */
		double FarM = 2500.0;
		/** Per cambiare banda si supera il confine di tanto. */
		double HysteresisM = 75.0;

		/** Cambia il raggio tenendo le proporzioni fra le bande (geo.Vegetation.Radius). */
		void SetFarM(double InFarM)
		{
			const double Far = std::max(200.0, InFarM);
			NearM = Far * 0.28;
			MidM = Far * 0.60;
			FarM = Far;
		}
	};

	namespace CellDetail
	{
		inline constexpr double MetresPerDegreeLat = 111132.0;
		inline constexpr double MetresPerDegreeLonAtEquator = 111320.0;
		inline constexpr double DegToRad = 3.14159265358979323846 / 180.0;

		inline double MetresPerDegreeLon(double LatDeg)
		{
			return MetresPerDegreeLonAtEquator * std::max(0.01, std::cos(LatDeg * DegToRad));
		}
	}

	/**
	 * La distanza dalla camera al punto della cella piu' vicino, m: in
	 * orizzontale (zero se la camera e' sopra la cella) e in verticale
	 * l'altezza della camera sopra il suolo.
	 */
	inline double DistanceToCellM(double CameraLatDeg, double CameraLonDeg, double AboveGroundM,
	                              const Tiles::FTileBounds& Cell)
	{
		using namespace CellDetail;
		const double Lat = std::clamp(CameraLatDeg, Cell.South, Cell.North);
		const double Lon = std::clamp(CameraLonDeg, Cell.West, Cell.East);
		const double North = (Lat - CameraLatDeg) * MetresPerDegreeLat;
		const double East = (Lon - CameraLonDeg) * MetresPerDegreeLon(CameraLatDeg);
		const double Up = std::max(0.0, AboveGroundM);
		return std::sqrt(North * North + East * East + Up * Up);
	}

	/**
	 * La banda di una cella a questa distanza. Previous e' quella che ha
	 * adesso (0..3), o -1 se e' nuova: una cella nuova prende la sua banda
	 * senza isteresi.
	 */
	inline int32_t BandFor(double DistanceM, const FBandSettings& Settings, int32_t Previous)
	{
		const double Limits[3] = { Settings.NearM, Settings.MidM, Settings.FarM };
		int32_t Band = BandOutside;
		for (int32_t Index = 0; Index < 3; ++Index)
		{
			if (DistanceM <= Limits[Index]) { Band = Index; break; }
		}
		if (Previous < 0 || Previous > BandOutside || Band == Previous) { return Band; }
		// Allontanandosi si lascia la banda solo HysteresisM oltre il suo confine;
		// avvicinandosi si entra solo HysteresisM dentro il confine di quella nuova.
		if (Band > Previous && DistanceM <= Limits[Previous] + Settings.HysteresisM) { return Previous; }
		if (Band < Previous && DistanceM > Limits[Previous - 1] - Settings.HysteresisM) { return Previous; }
		return Band;
	}

	struct FCellRef
	{
		Tiles::FTileKey Key;
		double DistanceM = 0.0;
	};

	/**
	 * Le celle (del livello Level) con un punto entro RadiusM dalla camera,
	 * dalla piu' vicina. Nessuna se la camera sta piu' in alto del raggio.
	 */
	inline void CellsAround(double CameraLatDeg, double CameraLonDeg, double AboveGroundM, double RadiusM,
	                        uint32_t Level, std::vector<FCellRef>& Out)
	{
		using namespace CellDetail;
		Out.clear();
		if (RadiusM <= 0.0 || AboveGroundM >= RadiusM) { return; }
		const double DeltaLat = RadiusM / MetresPerDegreeLat;
		const double DeltaLon = RadiusM / MetresPerDegreeLon(CameraLatDeg);
		uint32_t X0 = 0, Y0 = 0, X1 = 0, Y1 = 0;
		Tiles::TileForLonLat(Level, CameraLonDeg - DeltaLon, std::min(90.0, CameraLatDeg + DeltaLat), X0, Y0);
		Tiles::TileForLonLat(Level, CameraLonDeg + DeltaLon, std::max(-90.0, CameraLatDeg - DeltaLat), X1, Y1);
		for (uint32_t Y = Y0; Y <= Y1; ++Y)
		{
			for (uint32_t X = X0; X <= X1; ++X)
			{
				const Tiles::FTileBounds Bounds = Tiles::GetTileBounds(Level, X, Y);
				const double Distance = DistanceToCellM(CameraLatDeg, CameraLonDeg, AboveGroundM, Bounds);
				if (Distance <= RadiusM) { Out.push_back(FCellRef{ Tiles::FTileKey{ Level, X, Y }, Distance }); }
			}
		}
		std::sort(Out.begin(), Out.end(), [](const FCellRef& A, const FCellRef& B) { return A.DistanceM < B.DistanceM; });
	}

	/**
	 * La firma del terreno sotto una cella: le tile visibili che la toccano e
	 * il passo della mesh. 0 = nessun terreno (la cella aspetta).
	 */
	inline uint64_t SurfaceSignature(std::vector<Tiles::FTileKey> Keys, int32_t Step)
	{
		if (Keys.empty()) { return 0; }
		std::sort(Keys.begin(), Keys.end(),
			[](const Tiles::FTileKey& A, const Tiles::FTileKey& B) { return A.Pack() < B.Pack(); });
		uint64_t Hash = 0x9E3779B97F4A7C15ull ^ static_cast<uint64_t>(Step);
		for (const Tiles::FTileKey& Key : Keys)
		{
			Hash ^= Key.Pack() + 0x9E3779B97F4A7C15ull + (Hash << 6) + (Hash >> 2);
		}
		return Hash == 0 ? 1 : Hash;
	}

	/**
	 * I due rettangoli hanno un pezzo di area in comune? Un bordo condiviso
	 * non basta: la tile di terreno accanto non porta alberi alla cella, e
	 * metterla nella firma rifarebbe la cella quando cambia la vicina.
	 */
	inline bool BoundsOverlap(const Tiles::FTileBounds& A, const Tiles::FTileBounds& B)
	{
		return A.West < B.East && B.West < A.East && A.South < B.North && B.South < A.North;
	}

	/**
	 * Il terreno DISEGNATO sotto una cella: le tile visibili che la toccano,
	 * ognuna con il suo FSurfaceSampler (stessa griglia e stessa diagonale della
	 * mesh). Tiene vive le quote (shared_ptr): il worker le legge mentre il
	 * game thread va avanti.
	 */
	class FGroundSurfaces
	{
	public:
		void Add(std::shared_ptr<const Tiles::FHeightTile> Tile, int32_t Step)
		{
			if (!Tile || !Tile->IsValid()) { return; }
			const Tiles::FHeightTile& Heights = *Tile;
			Pieces.push_back(FPiece{ std::move(Tile), Mesh::FSurfaceSampler(Heights, Step),
				Tiles::GetTileBounds(Heights.Key.Level, Heights.Key.X, Heights.Key.Y), Heights.Key.Level });
		}

		bool IsEmpty() const { return Pieces.empty(); }
		size_t Count() const { return Pieces.size(); }

		/**
		 * Quota ellissoidica (m) e pendenza (gradi) del terreno disegnato nel
		 * punto. Falso se nessuna tile lo copre. Se due tile lo coprono (un
		 * bordo), vale la piu' fine.
		 */
		bool GroundAt(double LatDeg, double LonDeg, double& OutHeightM, double& OutSlopeDeg) const
		{
			using namespace CellDetail;
			const FPiece* Best = nullptr;
			for (const FPiece& Piece : Pieces)
			{
				if (LonDeg < Piece.Bounds.West || LonDeg > Piece.Bounds.East
					|| LatDeg < Piece.Bounds.South || LatDeg > Piece.Bounds.North) { continue; }
				if (!Best || Piece.Level > Best->Level) { Best = &Piece; }
			}
			if (!Best) { return false; }
			const Mesh::FSurfaceSampler& Surface = Best->Sampler;
			OutHeightM = Surface.HeightAt(LonDeg, LatDeg);

			// La pendenza: differenze di qua e di la' del punto, ProbeM metri.
			// Sul terreno disegnato (triangoli piani) e' la pendenza del
			// triangolo, o la media dei due vicini su uno spigolo.
			constexpr double ProbeM = 2.0;
			const double DeltaLat = ProbeM / MetresPerDegreeLat;
			const double DeltaLon = ProbeM / MetresPerDegreeLon(LatDeg);
			const double North = (Surface.HeightAt(LonDeg, LatDeg + DeltaLat) - Surface.HeightAt(LonDeg, LatDeg - DeltaLat)) / (2.0 * ProbeM);
			const double East = (Surface.HeightAt(LonDeg + DeltaLon, LatDeg) - Surface.HeightAt(LonDeg - DeltaLon, LatDeg)) / (2.0 * ProbeM);
			OutSlopeDeg = std::atan(std::sqrt(North * North + East * East)) / DegToRad;
			return true;
		}

	private:
		struct FPiece
		{
			std::shared_ptr<const Tiles::FHeightTile> Tile;
			Mesh::FSurfaceSampler Sampler;
			Tiles::FTileBounds Bounds;
			uint32_t Level = 0;
		};
		std::vector<FPiece> Pieces;
	};

}

// =============================================================================
//  VegetationCells.h -- Le celle degli alberi attorno alla camera. STRATO: C++
//  PURO, header-only.
//
//  Gli alberi non si mettono tutti insieme: si mettono a CELLE, le tile del
//  livello 15 (~610 x 430 m a 45°), solo quelle entro ~2,5 km dalla camera.
//  Una cella si costruisce su un worker (quale albero, dove, a che quota) e
//  diventa sul game thread qualche componente di istanze (uno per forma).
//
//  Qui c'e' tutto quello che non ha bisogno di Unreal, provato in
//  geovegetation_main.cpp (sezione 7):
//
//   - QUALI CELLE: quelle che hanno un punto entro il raggio, dalla piu'
//     vicina (CellsAround). Contando anche l'altezza sopra il suolo: da 5 km
//     di quota non se ne costruisce nessuna.
//   - LE BANDE: vicino (tutti gli alberi, con l'ombra), medio (la meta'),
//     lontano (un quarto). Per cambiare banda si deve superare il confine di
//     qualche decina di metri (l'ISTERESI): senza, una cella proprio sul
//     confine cambierebbe a ogni passo della camera, e gli alberi
//     comparirebbero e sparirebbero a scatti.
//   - IL SUOLO: gli alberi stanno sul terreno DISEGNATO (FSurfaceSampler),
//     cioe' sulle tile visibili adesso. La FIRMA di una cella dice su quali
//     tile e' stata costruita: quando il terreno sotto si affina la firma
//     cambia, e la cella si rifa' sul terreno nuovo. Senza, gli alberi
//     resterebbero sospesi (o sepolti) sulla forma vecchia.
//   - LE ISTANZE: l'albero di TreePlacement.h nel frame della cella, in
//     metri, con quello che serve al materiale (il verde, l'altezza, il caso).
// =============================================================================
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

#include "Mesh/TileMesh.h"
#include "Tiles/TileKey.h"
#include "Tiles/TilingScheme.h"
#include "Tiles/VegetationTileFormat.h"
#include "Vegetation/TreePlacement.h"

namespace GeoWorld::Vegetation
{
	/** Il livello delle celle: ~610 m nord-sud, ~430 m est-ovest a 45°. */
	inline constexpr uint32_t CellLevel = 15;

	/** Le bande, dalla camera: 0 vicino, 1 medio, 2 lontano, 3 fuori. */
	inline constexpr int32_t BandOutside = 3;

	struct FBandSettings
	{
		/** Fin qui tutti gli alberi, e fanno ombra. */
		double NearM = 700.0;
		/** Fin qui la meta' (BandFraction). */
		double MidM = 1500.0;
		/** Fin qui un quarto; oltre niente: c'e' la foto. */
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

	/**
	 * Un albero pronto per il componente di istanze: nel frame della cella
	 * (nord, est, alto, m) con quello che serve al materiale.
	 */
	struct FTreeInstance
	{
		float Local[3] = { 0.0f, 0.0f, 0.0f };
		float YawRad = 0.0f, WidthM = 0.0f, HeightM = 0.0f;
		float Colour[3] = { 0.0f, 0.0f, 0.0f };
		float Random = 0.0f;
		float Thin = 0.0f;
		uint8_t Species = 0;
		uint8_t Variant = 0;
	};

	/** I dati per istanza del materiale (Per Instance Custom Data): il verde, l'altezza, il caso. */
	inline constexpr int32_t InstanceDataFloats = 5;

	inline void InstanceData(const FTreeInstance& Tree, float Out[InstanceDataFloats])
	{
		Out[0] = Tree.Colour[0];
		Out[1] = Tree.Colour[1];
		Out[2] = Tree.Colour[2];
		Out[3] = Tree.HeightM;
		Out[4] = Tree.Random;
	}

	inline void MakeInstances(const std::vector<FTree>& Trees, const FCellFrame& Frame, std::vector<FTreeInstance>& Out)
	{
		Out.clear();
		Out.reserve(Trees.size());
		for (const FTree& Tree : Trees)
		{
			double Local[3];
			Frame.Local(Tree.LatDeg, Tree.LonDeg, Tree.GroundM, Local);
			FTreeInstance Instance;
			for (int32_t Axis = 0; Axis < 3; ++Axis) { Instance.Local[Axis] = static_cast<float>(Local[Axis]); }
			Instance.YawRad = Tree.YawRad;
			Instance.WidthM = Tree.WidthM;
			Instance.HeightM = Tree.HeightM;
			for (int32_t Channel = 0; Channel < 3; ++Channel) { Instance.Colour[Channel] = Tree.Colour[Channel]; }
			Instance.Random = Tree.Random;
			Instance.Thin = Tree.Thin;
			Instance.Species = static_cast<uint8_t>(Tree.Species);
			Instance.Variant = Tree.Variant;
			Out.push_back(Instance);
		}
	}

	/**
	 * Tutto il lavoro di una cella, quello del worker: chi cresce dove
	 * (PlaceTrees, sulla copertura del suolo e sul terreno disegnato) e le
	 * istanze nel frame della cella. Il frame e' quello di FCellFrame::At sul
	 * centro della cella: il componente ha la sua trasformazione.
	 */
	inline void BuildCellInstances(const Tiles::FTileBounds& Cell, const FPlacementSettings& Settings,
	                               const Tiles::FVegetationTile& Cover, const FGroundSurfaces& Ground, double GeoidM,
	                               std::vector<FTreeInstance>& Out)
	{
		std::vector<FTree> Trees;
		PlaceTrees(Cell.South, Cell.West, Cell.North, Cell.East, Settings,
			[&Cover](double LatDeg, double LonDeg) { return Cover.ClassAt(LatDeg, LonDeg); },
			[&Ground](double LatDeg, double LonDeg, double& OutHeightM, double& OutSlopeDeg)
			{
				return Ground.GroundAt(LatDeg, LonDeg, OutHeightM, OutSlopeDeg);
			},
			GeoidM, Trees);
		MakeInstances(Trees, FCellFrame::At(Cell.CentreLat(), Cell.CentreLon()), Out);
	}

	/** La forma (specie, variante) di un albero: l'indice del suo componente nella cella. */
	inline int32_t ShapeIndex(uint8_t Species, uint8_t Variant)
	{
		return static_cast<int32_t>(Species) * VariantCount + std::min<int32_t>(Variant, VariantCount - 1);
	}
	inline constexpr int32_t ShapeCount = SpeciesCount * VariantCount;
}

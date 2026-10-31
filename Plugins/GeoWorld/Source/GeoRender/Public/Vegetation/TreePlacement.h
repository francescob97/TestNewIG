// =============================================================================
//  TreePlacement.h -- Dove crescono gli alberi, e quali. STRATO: C++ PURO,
//  header-only.
//
//  IL PUNTO DI PARTENZA: una GRIGLIA GEOGRAFICA FISSA, uguale per tutto il
//  pianeta. Righe ogni 8 m verso nord; in ogni riga colonne ogni 8 m verso est
//  (il passo in longitudine lo decide il coseno della riga). Ogni punto della
//  griglia ha i suoi numeri a caso, fatti con un hash della riga e della
//  colonna: se li' cresce qualcosa, cosa, quanto e' alto, come e' girato, di
//  che verde. Per questo:
//
//   - lo stesso albero sta SEMPRE nello stesso posto: tornando indietro, o
//     ricostruendo una cella perche' il terreno sotto si e' affinato, si
//     ritrova identico;
//   - due celle vicine non hanno cuciture: un punto della griglia sta in una
//     cella sola (bordi mezzi aperti) e la griglia non dipende dalle celle.
//
//  COSA CRESCE: la copertura del suolo nel punto (ESA WorldCover, 10 m:
//  VegetationClasses.h) dice la probabilita' di un albero e di un arbusto. Il
//  bosco ha un albero sull'85% dei punti (uno ogni ~75 m^2, le chiome si
//  toccano), il prato qualche arbusto isolato, i campi e la citta' qualche
//  albero sparso. Moltiplicato per la densita' scelta (geo.Vegetation.Density).
//
//  QUALE ALBERO: lo dice la quota sul mare, e un po' la latitudine.
//   - conifere (abeti, larici): dal 10% in pianura al 95% sopra ~1.500 m sulle
//     Alpi; scendendo verso sud la fascia sale (nell'Appennino meridionale le
//     faggete arrivano a 1.800 m);
//   - pini domestici: nelle pianure e colline del centro-sud (sotto il 44,5°
//     parallelo, sotto i ~300 m), uno su sei;
//   - latifoglie (querce, faggi, castagni): il resto;
//   - sopra il LIMITE DEL BOSCO (2.100 m) solo arbusti, sempre piu' bassi;
//     sopra i 2.700 m niente. Vicino al limite gli alberi si fanno piccoli.
//   - niente sulle pareti: oltre 40° di pendenza la roccia (gli arbusti
//     resistono fino a 50°).
//
//  Chi chiama da' due funzioni: la classe nel punto (dalla tile di
//  vegetazione) e la quota del suolo con la pendenza (dalla superficie del
//  terreno DISEGNATA: gli alberi stanno sul terreno che si vede).
// =============================================================================
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "Geo/Ellipsoid.h"
#include "Mesh/GroundCells.h"
#include "Tiles/VegetationClasses.h"

namespace GeoWorld::Vegetation
{
	using Tiles::ELandCover;

	enum class ESpecies : uint8_t
	{
		Broadleaf = 0,      // querce, faggi, castagni: chioma tonda
		Conifer = 1,        // abeti, larici: un cono
		UmbrellaPine = 2,   // il pino domestico: tronco nudo e chioma piatta
		Shrub = 3,          // arbusti, macchia
	};
	inline constexpr int32_t SpeciesCount = 4;
	/** Quante forme diverse per specie (le mesh): l'albero ne prende una a caso. */
	inline constexpr int32_t VariantCount = 2;

	inline const char* SpeciesName(ESpecies Species)
	{
		switch (Species)
		{
		case ESpecies::Broadleaf: return "latifoglie";
		case ESpecies::Conifer: return "conifere";
		case ESpecies::UmbrellaPine: return "pini domestici";
		default: return "arbusti";
		}
	}

	struct FPlacementSettings
	{
		/** Il passo della griglia, m: un albero possibile ogni tanto. */
		double SpacingM = 8.0;
		/** Moltiplica le probabilita' di ogni classe (geo.Vegetation.Density). */
		double Density = 1.0;
		/** Sopra (m s.l.m.) niente alberi, solo arbusti; sopra la seconda niente. */
		double TreelineM = 2100.0;
		double ShrublineM = 2700.0;
		/** Oltre questa pendenza niente alberi; gli arbusti resistono 10° in piu'. */
		double MaxSlopeDeg = 40.0;
		uint32_t Seed = 1;
	};

	/** Che cosa cresce su una classe: la probabilita' per punto della griglia di un albero e di un arbusto. */
	struct FCoverPlants
	{
		double Trees = 0.0;
		double Shrubs = 0.0;
	};

	inline FCoverPlants PlantsFor(uint8_t Class)
	{
		switch (static_cast<ELandCover>(Class))
		{
		case ELandCover::Trees: return { 0.85, 0.08 };
		case ELandCover::Shrubs: return { 0.05, 0.55 };
		case ELandCover::Grass: return { 0.012, 0.04 };
		case ELandCover::Crops: return { 0.01, 0.005 };
		case ELandCover::Built: return { 0.03, 0.01 };
		case ELandCover::Wetland: return { 0.02, 0.12 };
		case ELandCover::Mangrove: return { 0.85, 0.0 };
		default: return {};
		}
	}

	/** Un albero (o un arbusto): dove, quanto e' grande, come. */
	struct FTree
	{
		uint64_t Id = 0;
		ESpecies Species = ESpecies::Broadleaf;
		uint8_t Variant = 0;
		double LatDeg = 0.0, LonDeg = 0.0;
		/** La quota ellissoidica del suolo sotto, m. */
		double GroundM = 0.0;
		float HeightM = 0.0f, WidthM = 0.0f, YawRad = 0.0f;
		/** Il verde della chioma (lineare, 0..1). */
		float Colour[3] = { 0.0f, 0.0f, 0.0f };
		/** Un numero a caso 0..1: la fase del vento nel materiale. */
		float Random = 0.0f;
		/**
		 * Un altro numero a caso 0..1, per DIRADARE da lontano: una cella lontana
		 * tiene solo gli alberi con Thin sotto la sua frazione. Sempre gli stessi:
		 * avvicinandosi se ne aggiungono, nessuno cambia posto.
		 */
		float Thin = 0.0f;
	};

	namespace PlacementDetail
	{
		inline constexpr double MetresPerDegreeLat = 111132.0;
		inline constexpr double MetresPerDegreeLon = 111320.0;
		inline constexpr double DegToRad = 3.14159265358979323846 / 180.0;

		inline uint64_t Mix(uint64_t Value)
		{
			Value += 0x9E3779B97F4A7C15ull;
			Value = (Value ^ (Value >> 30)) * 0xBF58476D1CE4E5B9ull;
			Value = (Value ^ (Value >> 27)) * 0x94D049BB133111EBull;
			return Value ^ (Value >> 31);
		}

		inline uint64_t PointHash(int64_t Row, int64_t Column, uint32_t Seed)
		{
			return Mix(Mix(static_cast<uint64_t>(Row) * 0x632BE59BD9B4E019ull + Seed) ^ static_cast<uint64_t>(Column));
		}

		/** Un numero 0..1 dall'hash del punto: Salt sceglie quale. */
		inline double Unit(uint64_t Hash, uint32_t Salt)
		{
			return static_cast<double>(Mix(Hash + 0xA24BAED4963EE407ull * (Salt + 1)) >> 11) * (1.0 / 9007199254740992.0);
		}

		inline double Smoothstep(double Edge0, double Edge1, double X)
		{
			const double T = std::min(1.0, std::max(0.0, (X - Edge0) / (Edge1 - Edge0)));
			return T * T * (3.0 - 2.0 * T);
		}

		inline double RowStepDeg(double SpacingM) { return SpacingM / MetresPerDegreeLat; }

		/** Il passo in longitudine della riga: con il coseno della SUA latitudine, uguale per tutte le celle. */
		inline double ColumnStepDeg(int64_t Row, double SpacingM)
		{
			const double Lat = (static_cast<double>(Row) + 0.5) * RowStepDeg(SpacingM);
			return SpacingM / (MetresPerDegreeLon * std::max(0.01, std::cos(Lat * DegToRad)));
		}
	}

	/** Quante conifere fra gli alberi, alla quota (s.l.m.) e latitudine. */
	inline double ConiferShare(double AltitudeM, double LatDeg)
	{
		// Scendendo verso sud la fascia delle conifere sale: ~180 m per grado sotto il 46°.
		const double Shift = std::max(0.0, 46.0 - LatDeg) * 180.0;
		return 0.10 + 0.85 * PlacementDetail::Smoothstep(700.0 + Shift, 1500.0 + Shift, AltitudeM);
	}

	/** Quanti pini domestici fra gli alberi: pianure e colline del centro-sud. */
	inline double UmbrellaPineShare(double AltitudeM, double LatDeg)
	{
		if (LatDeg > 44.5) { return 0.0; }
		return 0.17 * (1.0 - PlacementDetail::Smoothstep(150.0, 450.0, AltitudeM));
	}

	/**
	 * Ogni punto della griglia nel rettangolo [South, North) x [West, East),
	 * in gradi: Visit(Row, Column, LatDeg, LonDeg, Hash). Mezzi aperti: un punto
	 * sul confine fra due celle sta in una sola.
	 */
	template <typename FVisit>
	inline void ForEachGridPoint(double South, double West, double North, double East,
	                             const FPlacementSettings& Settings, FVisit&& Visit)
	{
		using namespace PlacementDetail;
		const double Spacing = std::max(1.0, Settings.SpacingM);
		const double RowStep = RowStepDeg(Spacing);
		const int64_t FirstRow = static_cast<int64_t>(std::floor(South / RowStep)) - 1;
		const int64_t LastRow = static_cast<int64_t>(std::floor(North / RowStep)) + 1;
		for (int64_t Row = FirstRow; Row <= LastRow; ++Row)
		{
			const double ColumnStep = ColumnStepDeg(Row, Spacing);
			const int64_t FirstColumn = static_cast<int64_t>(std::floor(West / ColumnStep)) - 1;
			const int64_t LastColumn = static_cast<int64_t>(std::floor(East / ColumnStep)) + 1;
			for (int64_t Column = FirstColumn; Column <= LastColumn; ++Column)
			{
				const uint64_t Hash = PointHash(Row, Column, Settings.Seed);
				// Il punto si sposta a caso dentro la sua casella (fra il 10 e il 90%):
				// niente file regolari, e mai due alberi sullo stesso tronco.
				const double LatDeg = (static_cast<double>(Row) + 0.1 + 0.8 * Unit(Hash, 0)) * RowStep;
				const double LonDeg = (static_cast<double>(Column) + 0.1 + 0.8 * Unit(Hash, 1)) * ColumnStep;
				if (LatDeg < South || LatDeg >= North || LonDeg < West || LonDeg >= East) { continue; }
				Visit(Row, Column, LatDeg, LonDeg, Hash);
			}
		}
	}

	/**
	 * Gli alberi del rettangolo.
	 *
	 * ClassAt(LatDeg, LonDeg) -> uint8_t: la copertura del suolo nel punto.
	 * GroundAt(LatDeg, LonDeg, double& HeightM, double& SlopeDeg) -> bool: la
	 *   quota ellissoidica del suolo e la pendenza; false se il suolo non si
	 *   conosce (niente albero).
	 * GeoidM: il geoide, per la quota sul mare (le regole delle specie).
	 */
	template <typename FClassAt, typename FGroundAt>
	inline void PlaceTrees(double South, double West, double North, double East, const FPlacementSettings& Settings,
	                       FClassAt&& ClassAt, FGroundAt&& GroundAt, double GeoidM, std::vector<FTree>& Out)
	{
		using namespace PlacementDetail;
		Out.clear();
		const double Density = std::max(0.0, Settings.Density);
		ForEachGridPoint(South, West, North, East, Settings,
			[&](int64_t Row, int64_t Column, double LatDeg, double LonDeg, uint64_t Hash)
			{
				const FCoverPlants Plants = PlantsFor(ClassAt(LatDeg, LonDeg));
				const double Pick = Unit(Hash, 2);
				const double TreeLimit = std::min(1.0, Plants.Trees * Density);
				const double ShrubLimit = std::min(1.0, TreeLimit + Plants.Shrubs * Density);
				if (Pick >= ShrubLimit) { return; }
				bool bTree = Pick < TreeLimit;

				double Ground = 0.0, Slope = 0.0;
				if (!GroundAt(LatDeg, LonDeg, Ground, Slope)) { return; }
				const double Altitude = Ground - GeoidM;

				// Il limite del bosco: sopra solo arbusti, sopra ancora niente.
				if (bTree && Altitude > Settings.TreelineM) { bTree = false; }
				if (!bTree && Altitude > Settings.ShrublineM) { return; }
				if (Slope > Settings.MaxSlopeDeg + (bTree ? 0.0 : 10.0)) { return; }

				FTree Tree;
				Tree.Id = Hash;
				Tree.LatDeg = LatDeg;
				Tree.LonDeg = LonDeg;
				Tree.GroundM = Ground;
				Tree.Variant = static_cast<uint8_t>(Unit(Hash, 3) < 0.5 ? 0 : 1);
				Tree.YawRad = static_cast<float>(Unit(Hash, 4) * 2.0 * 3.14159265358979323846);
				Tree.Random = static_cast<float>(Unit(Hash, 5));
				Tree.Thin = static_cast<float>(Unit(Hash, 6));

				// Quale albero, dalla quota e dalla latitudine.
				if (!bTree) { Tree.Species = ESpecies::Shrub; }
				else
				{
					const double Kind = Unit(Hash, 7);
					const double Conifer = ConiferShare(Altitude, LatDeg);
					const double Umbrella = UmbrellaPineShare(Altitude, LatDeg) * (1.0 - Conifer);
					Tree.Species = Kind < Conifer ? ESpecies::Conifer
						: Kind < Conifer + Umbrella ? ESpecies::UmbrellaPine : ESpecies::Broadleaf;
				}

				// Quanto e' grande: altezza e larghezza della chioma, per specie.
				struct FSize { double MinHeight, MaxHeight, MinRatio, MaxRatio; };
				static const FSize Sizes[SpeciesCount] = {
					{ 9.0, 21.0, 0.60, 0.85 },     // latifoglie
					{ 14.0, 30.0, 0.30, 0.42 },    // conifere
					{ 10.0, 17.0, 0.95, 1.20 },    // pini domestici
					{ 1.0, 3.2, 1.00, 1.60 },      // arbusti
				};
				const FSize& Size = Sizes[static_cast<int32_t>(Tree.Species)];
				// Vicino al limite del bosco tutto si fa piu' piccolo (fino alla meta').
				const double Line = bTree ? Settings.TreelineM : Settings.ShrublineM;
				const double Stunt = 1.0 - 0.5 * Smoothstep(Line - 600.0, Line, Altitude);
				const double Height = (Size.MinHeight + (Size.MaxHeight - Size.MinHeight) * std::pow(Unit(Hash, 8), 0.8)) * Stunt;
				const double Ratio = Size.MinRatio + (Size.MaxRatio - Size.MinRatio) * Unit(Hash, 9);
				Tree.HeightM = static_cast<float>(Height);
				Tree.WidthM = static_cast<float>(Height * Ratio);

				// Il verde: uno per specie, piu' chiaro o piu' scuro, piu' giallo o piu' blu.
				static const float Greens[SpeciesCount][3] = {
					{ 0.075f, 0.125f, 0.035f },    // latifoglie: verde pieno
					{ 0.035f, 0.072f, 0.040f },    // conifere: verde scuro, bluastro
					{ 0.060f, 0.100f, 0.040f },    // pini domestici
					{ 0.090f, 0.115f, 0.050f },    // arbusti: verde oliva
				};
				const float* Green = Greens[static_cast<int32_t>(Tree.Species)];
				const float Bright = static_cast<float>(0.75 + 0.5 * Unit(Hash, 10));
				const float Warm = static_cast<float>(Unit(Hash, 11) - 0.5) * 0.25f;
				Tree.Colour[0] = Green[0] * Bright * (1.0f + Warm);
				Tree.Colour[1] = Green[1] * Bright;
				Tree.Colour[2] = Green[2] * Bright * (1.0f - Warm);

				(void)Row;
				(void)Column;
				Out.push_back(Tree);
			});
	}

	// Il frame locale della cella (nord, est, alto) sta in Mesh/GroundCells.h:
	// lo usano anche gli edifici.
	using Cells::FCellFrame;

	/**
	 * Quanti alberi tiene una cella a questa distanza dalla camera (0..1): tutti
	 * entro NearM, la meta' fino a MidM, un quarto oltre. Da lontano l'albero
	 * e' qualche pixel, e la foto sotto fa gia' il bosco.
	 */
	inline float BandFraction(int32_t Band)
	{
		return Band <= 0 ? 1.0f : (Band == 1 ? 0.5f : 0.25f);
	}
}

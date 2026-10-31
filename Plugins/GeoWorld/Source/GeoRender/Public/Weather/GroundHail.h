// =============================================================================
//  GroundHail.h -- La grandine posata al suolo. STRATO: C++ PURO, header-only.
//
//  La grandine NON e' uno stato per il mondo come il bagnato: cade sotto i
//  nuclei dei temporali, che sono larghi qualche chilometro e viaggiano con
//  il vento in quota. Al suolo lascia una STRISCIA bianca (la "hail swath"
//  dei meteorologi) lunga quanto il nucleo ha viaggiato, che poi fonde. Un
//  mondo tutto bianco perche' un nucleo e' passato sopra la camera sarebbe
//  sbagliato, e da un aereo si vedrebbe subito.
//
//  Quindi una GRIGLIA al suolo: 256 x 256 celle su 120 km attorno alla
//  camera (470 m l'una), FERMA rispetto al terreno. A ogni passo ogni cella
//  aggiunge la grandine che le cade sopra adesso (la mappa dei rovesci,
//  FPrecipitationField::HailRateMmH) e toglie quella che fonde. Quando la
//  camera si allontana la griglia si ricentra spostandosi di celle intere:
//  la grandine resta esattamente dov'era.
//
//  I NUMERI.
//   - Lo spessore: un mm d'acqua di grandine fa 1,85 mm di strato (ghiaccio,
//     densita' 0,9, in chicchi che lasciano vuoto il 40%).
//   - Il suolo e' bianco con 4 mm di strato (chicchi da un centimetro, uno
//     ogni due o tre centimetri): con la grandine di default (30 mm/h nei
//     nuclei, il 60% grandine) succede in una decina di minuti.
//   - Fonde di 0,6 mm all'ora per ogni grado sopra zero: a 20 °C uno strato
//     di 8 mm resta una quarantina di minuti. Sotto zero non fonde.
//
//  La temperatura e' quella al suolo sotto la camera (la griglia non conosce
//  le quote): in montagna la grandine fonde come in valle. E' un limite.
// =============================================================================
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "Weather/Precipitation.h"

namespace GeoWorld::Weather
{
	/** Lo strato di grandine con cui il suolo e' tutto bianco, mm. */
	inline constexpr double HailFullCoverMm = 4.0;
	/** Mm di strato per mm d'acqua: ghiaccio (0,9) in chicchi che lasciano vuoto il 40%. */
	inline constexpr double HailDepthPerWaterMm = 1.0 / (0.9 * 0.6);
	/** Quanto fonde all'ora per ogni grado sopra zero, mm di strato. */
	inline constexpr double HailMeltMmPerHourPerDegree = 0.6;

	class FHailGround
	{
	public:
		static constexpr int32_t DefaultSize = 256;
		static constexpr double DefaultExtentM = 120000.0;

		FHailGround() { Reset(DefaultSize, DefaultExtentM); }

		/** Una griglia nuova, vuota, non ancora centrata. */
		void Reset(int32_t InSize, double InExtentM)
		{
			Size = std::max<int32_t>(2, InSize & ~1);   // pari: il centro sta fra due celle
			ExtentM = std::max(1000.0, InExtentM);
			Depth.assign(static_cast<size_t>(Size) * Size, 0.0f);
			bCentred = false;
			bAny = false;
		}

		bool IsCentred() const { return bCentred; }
		/** Nessuna grandine da nessuna parte: chi la disegna puo' non fare niente. */
		bool IsEmpty() const { return !bAny; }
		int32_t GetSize() const { return Size; }
		double GetExtentM() const { return ExtentM; }
		double GetCellM() const { return ExtentM / Size; }

		/** Il centro della griglia (fra le quattro celle centrali). */
		double GetCentreLatDeg() const { return ReferenceLatDeg + CentreRow * GetCellM() / 111132.0; }
		double GetCentreLonDeg() const { return ReferenceLonDeg + CentreColumn * GetCellM() / (111320.0 * ReferenceCos); }

		/**
		 * Quanto la griglia e' "stirata" verso est rispetto ai metri veri al
		 * suo centro. L'est della griglia e' misurato con il coseno della
		 * latitudine del PRIMO centro (solo cosi' ricentrare di celle intere
		 * lascia la grandine al suo posto); al centro di adesso un metro vero
		 * verso est vale questo fattore in metri della griglia. Lo shader ci
		 * moltiplica l'est. A 300 km dal primo centro e' gia' qualche per cento.
		 */
		double GetEastScale() const
		{
			const double Here = std::cos(GetCentreLatDeg() * WindDegToRad);
			return Here > 1.0e-6 ? ReferenceCos / Here : 1.0;
		}

		/**
		 * Centra la griglia sul punto, spostandola di CELLE INTERE: cio' che
		 * resta dentro non si muove di un millimetro, cio' che esce si perde,
		 * cio' che entra e' vuoto. La prima volta fissa il riferimento.
		 */
		void Recentre(double LatDeg, double LonDeg)
		{
			if (!bCentred)
			{
				ReferenceLatDeg = LatDeg;
				ReferenceLonDeg = LonDeg;
				ReferenceCos = std::max(1.0e-6, std::cos(LatDeg * WindDegToRad));
				CentreRow = CentreColumn = 0;
				bCentred = true;
				return;
			}
			const int64_t Row = static_cast<int64_t>(std::llround((LatDeg - ReferenceLatDeg) * 111132.0 / GetCellM()));
			const int64_t Column = static_cast<int64_t>(std::llround((LonDeg - ReferenceLonDeg) * 111320.0 * ReferenceCos / GetCellM()));
			const int64_t ShiftRows = Row - CentreRow, ShiftColumns = Column - CentreColumn;
			if (ShiftRows == 0 && ShiftColumns == 0) { return; }

			std::vector<float> Moved(Depth.size(), 0.0f);
			bool bStill = false;
			for (int32_t NewRow = 0; NewRow < Size; ++NewRow)
			{
				const int64_t OldRow = NewRow + ShiftRows;
				if (OldRow < 0 || OldRow >= Size) { continue; }
				for (int32_t NewColumn = 0; NewColumn < Size; ++NewColumn)
				{
					const int64_t OldColumn = NewColumn + ShiftColumns;
					if (OldColumn < 0 || OldColumn >= Size) { continue; }
					const float Value = Depth[static_cast<size_t>(OldRow) * Size + static_cast<size_t>(OldColumn)];
					Moved[static_cast<size_t>(NewRow) * Size + NewColumn] = Value;
					bStill = bStill || Value > 0.0f;
				}
			}
			Depth.swap(Moved);
			bAny = bStill;
			CentreRow = Row;
			CentreColumn = Column;
		}

		/** Dove sta il centro della cella (Row verso nord, Column verso est). */
		void CellLatLon(int32_t Row, int32_t Column, double& OutLatDeg, double& OutLonDeg) const
		{
			const double North = (CentreRow + Row - Size / 2 + 0.5) * GetCellM();
			const double East = (CentreColumn + Column - Size / 2 + 0.5) * GetCellM();
			OutLatDeg = ReferenceLatDeg + North / 111132.0;
			OutLonDeg = ReferenceLonDeg + East / (111320.0 * ReferenceCos);
		}

		/**
		 * Un passo: ogni cella prende la grandine che le cade addosso adesso e
		 * perde quella che fonde. TemperatureC: l'aria al suolo (sotto la camera).
		 */
		void Advance(const FPrecipitationField& Field, double DeltaSeconds, double TemperatureC)
		{
			if (!bCentred || !(DeltaSeconds > 0.0)) { return; }
			const double Hours = DeltaSeconds / 3600.0;
			const float Melt = static_cast<float>(std::max(0.0, TemperatureC) * HailMeltMmPerHourPerDegree * Hours);
			const bool bFalling = Field.GetSettings().Kind == EPrecipitationKind::Hail
				&& Field.GetSettings().RateMmH > 0.0 && Field.GetSettings().Coverage > 0.0;
			if (!bFalling && (!bAny || Melt <= 0.0f)) { return; }

			bool bStill = false;
			for (int32_t Row = 0; Row < Size; ++Row)
			{
				for (int32_t Column = 0; Column < Size; ++Column)
				{
					float& Cell = Depth[static_cast<size_t>(Row) * Size + Column];
					if (bFalling)
					{
						double LatDeg = 0.0, LonDeg = 0.0;
						CellLatLon(Row, Column, LatDeg, LonDeg);
						Cell += static_cast<float>(Field.HailRateMmH(LatDeg, LonDeg) * HailDepthPerWaterMm * Hours);
					}
					Cell = std::max(0.0f, Cell - Melt);
					bStill = bStill || Cell > 0.0f;
				}
			}
			bAny = bStill;
		}

		/** Grandine a mano: un disco di raggio RadiusM con lo strato DepthMm (per provare). */
		void AddDisc(double LatDeg, double LonDeg, double RadiusM, double DepthMm)
		{
			if (!bCentred) { Recentre(LatDeg, LonDeg); }
			const double MetresEast = 111320.0 * std::cos(LatDeg * WindDegToRad);
			for (int32_t Row = 0; Row < Size; ++Row)
			{
				for (int32_t Column = 0; Column < Size; ++Column)
				{
					double CellLat = 0.0, CellLon = 0.0;
					CellLatLon(Row, Column, CellLat, CellLon);
					const double North = (CellLat - LatDeg) * 111132.0, East = (CellLon - LonDeg) * MetresEast;
					if (North * North + East * East > RadiusM * RadiusM) { continue; }
					float& Cell = Depth[static_cast<size_t>(Row) * Size + Column];
					Cell = std::max(Cell, static_cast<float>(DepthMm));
					bAny = bAny || Cell > 0.0f;
				}
			}
		}

		void Clear()
		{
			std::fill(Depth.begin(), Depth.end(), 0.0f);
			bAny = false;
		}

		/** Lo strato nella cella che contiene il punto, mm (0 fuori dalla griglia). */
		double DepthAt(double LatDeg, double LonDeg) const
		{
			if (!bCentred) { return 0.0; }
			const double North = (LatDeg - ReferenceLatDeg) * 111132.0 / GetCellM() - CentreRow + Size / 2;
			const double East = (LonDeg - ReferenceLonDeg) * 111320.0 * ReferenceCos / GetCellM() - CentreColumn + Size / 2;
			const int64_t Row = static_cast<int64_t>(std::floor(North)), Column = static_cast<int64_t>(std::floor(East));
			if (Row < 0 || Row >= Size || Column < 0 || Column >= Size) { return 0.0; }
			return Depth[static_cast<size_t>(Row) * Size + static_cast<size_t>(Column)];
		}

		/** Quanto e' bianco il suolo li', 0..1. */
		double CoverAt(double LatDeg, double LonDeg) const
		{
			return PrecipitationDetail::Clamp(DepthAt(LatDeg, LonDeg) / HailFullCoverMm, 0.0, 1.0);
		}

		/** La texture per il materiale: BGRA, righe verso nord, colonne verso est, R = quanto e' bianco. */
		void WriteBgra(std::vector<uint8_t>& Out) const
		{
			Out.assign(static_cast<size_t>(Size) * Size * 4, 0);
			for (size_t Index = 0; Index < Depth.size(); ++Index)
			{
				const double Cover = PrecipitationDetail::Clamp(Depth[Index] / HailFullCoverMm, 0.0, 1.0);
				Out[Index * 4 + 2] = static_cast<uint8_t>(std::lround(Cover * 255.0));
				Out[Index * 4 + 3] = 255;
			}
		}

		/** Lo strato piu' spesso, mm (per le stampe e i test). */
		double MaxDepth() const
		{
			float Largest = 0.0f;
			for (const float Value : Depth) { Largest = std::max(Largest, Value); }
			return Largest;
		}

	private:
		int32_t Size = DefaultSize;
		double ExtentM = DefaultExtentM;
		/** Il riferimento fisso (il primo centro) e dove sta adesso il centro, in celle da li'. */
		double ReferenceLatDeg = 0.0, ReferenceLonDeg = 0.0, ReferenceCos = 1.0;
		int64_t CentreRow = 0, CentreColumn = 0;
		bool bCentred = false;
		bool bAny = false;
		/** Lo strato, mm, riga per riga da sud a nord. */
		std::vector<float> Depth;
	};
}

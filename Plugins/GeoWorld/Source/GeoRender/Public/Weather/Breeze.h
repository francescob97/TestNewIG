// =============================================================================
//  Breeze.h -- Le brezze: i venti che fa il sole. STRATO: C++ PURO,
//  header-only.
//
//  Il sole scalda la terra piu' dell'acqua, e i pendii al sole piu' delle
//  valli in ombra; di notte la terra si raffredda prima. L'aria calda sale,
//  quella fredda scende, e attorno nasce un vento locale, che si somma a
//  quello del tempo che fa (WindField.h):
//
//   1. LA BREZZA DI MARE (di giorno) e DI TERRA (di notte). Sulla costa, dal
//      mare verso terra nel pomeriggio (3-6 m/s, spessa ~700 m, con il
//      ritorno in quota), da terra verso il mare di notte (1-2,5 m/s,
//      sottile). Anche sui laghi grandi (il Garda ha la sua "Ora").
//   2. LA BREZZA DI PENDIO. Di giorno l'aria risale i versanti al sole
//      (anabatica: 1-3 m/s, qualche decina di metri); di notte scende lungo
//      tutti i pendii (catabatica: sottile, pochi metri, ma costante).
//   3. LA BREZZA DI VALLE. Di giorno risale la valle verso i monti, di notte
//      scende verso la pianura: la pendenza su qualche chilometro.
//
//  IL RITARDO. Le brezze non seguono il sole istante per istante: la terra
//  ci mette ore a scaldarsi rispetto al mare. Lo STATO TERMICO
//  (FBreezeClimate) insegue la spinta del sole con un tempo proprio: 2,5 ore
//  per il mare (la brezza parte a meta' mattina e ha il massimo verso le 15),
//  40 minuti per i pendii, un'ora e mezza per le valli.
//
//  IL TEMPO CHE FA LE SPEGNE: con piu' di 9 m/s di vento, o con il cielo
//  coperto (il sole non scalda) e la pioggia, le brezze non ci sono.
//
//  DOVE E' L'ACQUA: una mappa di ~100 km attorno alla camera (FWaterMap),
//  lisciata su 10 km: il suo gradiente punta verso l'acqua e la sua
//  grandezza dice quanto si e' sulla costa (1 sulla riva di un mare, quasi
//  zero a 30 km, poco attorno a un laghetto).
// =============================================================================
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "Sky/Astronomy.h"
#include "Weather/WindField.h"

namespace GeoWorld::Weather
{
	struct FBreezeSettings
	{
		bool bEnabled = true;
		/** Moltiplica tutte le brezze (0 = nessuna, 1 = come in natura, fino a 3). */
		double Strength = 1.0;
	};

	inline constexpr double SeaBreezeMaxMs = 6.0;
	inline constexpr double LandBreezeMaxMs = 2.5;
	inline constexpr double SlopeWindMaxMs = 2.5;
	inline constexpr double ValleyWindMaxMs = 3.0;
	/** Con questo vento di riferimento (a 10 m) le brezze sono sparite. */
	inline constexpr double BreezeKillWindMs = 9.0;
	/** La scala della costa: la mappa dell'acqua e' lisciata con una gaussiana di questo sigma, m. */
	inline constexpr double CoastScaleM = 10000.0;
	/** Lo stato del mare che fa la brezza di terra piena (di notte, cielo sereno). */
	inline constexpr double LandBreezeFullState = 0.35;

	namespace BreezeDetail
	{
		inline double Smooth(double Edge0, double Edge1, double X)
		{
			const double T = std::max(0.0, std::min(1.0, (X - Edge0) / (Edge1 - Edge0)));
			return T * T * (3.0 - 2.0 * T);
		}

		/** Il sole (altezza e azimut, gradi) da un punto, a un istante UT. */
		inline void SunAt(double JdUt, double LatDeg, double LonDeg, double& OutAltitudeDeg, double& OutAzimuthDeg)
		{
			const Sky::FEquatorial Sun = Sky::SunPosition(Sky::TerrestrialTime(JdUt));
			const Sky::FHorizontal Here = Sky::EquatorialToHorizontal(Sun.RightAscensionDeg, Sun.DeclinationDeg,
				Sky::LocalSiderealTime(JdUt, LonDeg), LatDeg);
			OutAltitudeDeg = Here.AltitudeDeg;
			OutAzimuthDeg = Here.AzimuthDeg;
		}
	}

	// =========================================================================
	//  Lo stato termico: quanto la terra e' piu' calda (o piu' fredda)
	// =========================================================================

	class FBreezeClimate
	{
	public:
		/** La spinta del sole adesso, senza ritardo: dove lo stato vuole andare. */
		struct FForcing
		{
			/** Terra contro acqua: > 0 la terra e' piu' calda (brezza di mare), < 0 di notte. */
			double Coast = 0.0;
			/** Pendii contro aria libera: > 0 al sole (anabatica), < 0 di notte (catabatica). */
			double Slope = 0.0;
		};

		static FForcing ForcingAt(double SunAltitudeDeg, double CloudCover, bool bRaining)
		{
			using namespace BreezeDetail;
			const double Cover = std::max(0.0, std::min(1.0, CloudCover));
			const double Sun = std::max(0.0, std::sin(SunAltitudeDeg * Sky::DegToRad));
			const double Clear = (1.0 - 0.75 * Cover) * (bRaining ? 0.2 : 1.0);
			// Di notte la terra perde calore verso il cielo sereno (le nuvole lo
			// trattengono). Comincia dopo il tramonto.
			const double Night = (1.0 - Smooth(-6.0, 0.0, SunAltitudeDeg)) * (1.0 - 0.8 * Cover) * (bRaining ? 0.3 : 1.0);
			FForcing Out;
			// La terra deve prima recuperare il fresco della notte: col sole
			// basso (sotto ~12 gradi) non scalda ancora piu' del mare.
			Out.Coast = std::max(-1.0, std::min(1.0, 1.7 * std::max(0.0, Sun - 0.2) * Clear - LandBreezeFullState * Night));
			Out.Slope = std::max(-1.0, std::min(1.0, 1.3 * Sun * Clear - 0.6 * Night));
			return Out;
		}

		/** Ore che servono allo stato per fare 2/3 della strada verso la spinta. */
		static constexpr double CoastHours = 2.5;
		static constexpr double SlopeHours = 0.67;
		static constexpr double ValleyHours = 1.5;

		/**
		 * Lo stato all'istante JdUt, nel punto dato. Si integra dall'ultima
		 * volta a passi di 10 minuti; se il tempo e' saltato (piu' di tre ore,
		 * o all'indietro di piu' di un'ora: geo.Sky.Time) si riparte da 36 ore
		 * prima, cosi' alle 15 la brezza e' quella delle 15 e non quella di
		 * un'ora che e' appena cominciata.
		 */
		void Advance(double JdUt, double LatDeg, double LonDeg, double CloudCover, bool bRaining)
		{
			const double Gap = (JdUt - LastJd) * 24.0;
			if (!bStarted || Gap > 3.0 || Gap < -1.0)
			{
				CoastState = SlopeState = ValleyState = 0.0;
				LastJd = JdUt - 1.5;
				bStarted = true;
			}
			const double Total = (JdUt - LastJd) * 86400.0;
			const double Span = std::fabs(Total);
			const int32_t Steps = std::max(1, static_cast<int32_t>(std::ceil(Span / 600.0)));
			const double Step = Span / Steps;
			for (int32_t Index = 0; Index < Steps; ++Index)
			{
				// La spinta a meta' del passo (in avanti nel tempo: la fisica non va all'indietro).
				const double Middle = LastJd + (Total >= 0.0 ? (Index + 0.5) : -(Index + 0.5)) * Step / 86400.0;
				double Altitude = 0.0, Azimuth = 0.0;
				BreezeDetail::SunAt(Middle, LatDeg, LonDeg, Altitude, Azimuth);
				const FForcing Push = ForcingAt(Altitude, CloudCover, bRaining);
				CoastState += (Push.Coast - CoastState) * (1.0 - std::exp(-Step / (CoastHours * 3600.0)));
				SlopeState += (Push.Slope - SlopeState) * (1.0 - std::exp(-Step / (SlopeHours * 3600.0)));
				ValleyState += (Push.Slope - ValleyState) * (1.0 - std::exp(-Step / (ValleyHours * 3600.0)));
			}
			LastJd = JdUt;
			BreezeDetail::SunAt(JdUt, LatDeg, LonDeg, SunAltitude, SunAzimuth);
		}

		bool IsValid() const { return bStarted; }
		double Coast() const { return CoastState; }
		double Slope() const { return SlopeState; }
		double Valley() const { return ValleyState; }
		double SunAltitudeDeg() const { return SunAltitude; }
		double SunAzimuthDeg() const { return SunAzimuth; }

	private:
		bool bStarted = false;
		double LastJd = 0.0;
		double CoastState = 0.0, SlopeState = 0.0, ValleyState = 0.0;
		double SunAltitude = -90.0, SunAzimuth = 0.0;
	};

	// =========================================================================
	//  La mappa dell'acqua
	// =========================================================================

	/**
	 * Una griglia di 48 x 48 celle da 2 km attorno a un centro: per ogni cella
	 * se il suo centro e' acqua, poi lisciata con una gaussiana di 10 km. Il
	 * lisciato e' la "frazione d'acqua" dei dintorni; il suo gradiente punta
	 * verso l'acqua ed e' massimo sulla riva.
	 */
	class FWaterMap
	{
	public:
		static constexpr int32_t Size = 48;
		static constexpr double CellM = 2000.0;

		template <typename TIsWater>
		void Build(double CentreLatDeg, double CentreLonDeg, const TIsWater& IsWater)
		{
			CentreLat = CentreLatDeg;
			CentreLon = CentreLonDeg;
			MetresEast = 111320.0 * std::cos(CentreLat * Sky::DegToRad);
			std::vector<double> Raw(static_cast<size_t>(Size) * Size, 0.0);
			bAnyWater = false;
			for (int32_t Row = 0; Row < Size; ++Row)
			{
				for (int32_t Column = 0; Column < Size; ++Column)
				{
					double Lat = 0.0, Lon = 0.0;
					CellCentre(Row, Column, Lat, Lon);
					const bool bWater = IsWater(Lon, Lat);
					Raw[static_cast<size_t>(Row) * Size + Column] = bWater ? 1.0 : 0.0;
					bAnyWater = bAnyWater || bWater;
				}
			}
			// La gaussiana, separabile: prima le righe, poi le colonne; fuori
			// dalla mappa vale il bordo (la costa continua com'e').
			const double Sigma = CoastScaleM / CellM;
			const int32_t Radius = static_cast<int32_t>(std::ceil(3.0 * Sigma));
			std::vector<double> Kernel(static_cast<size_t>(2 * Radius + 1));
			double Sum = 0.0;
			for (int32_t Offset = -Radius; Offset <= Radius; ++Offset)
			{
				const double Weight = std::exp(-0.5 * Offset * Offset / (Sigma * Sigma));
				Kernel[static_cast<size_t>(Offset + Radius)] = Weight;
				Sum += Weight;
			}
			for (double& Weight : Kernel) { Weight /= Sum; }
			auto Clamp = [](int32_t Value) { return std::max(0, std::min(Size - 1, Value)); };
			std::vector<double> Rows(Raw.size(), 0.0);
			for (int32_t Row = 0; Row < Size; ++Row)
			{
				for (int32_t Column = 0; Column < Size; ++Column)
				{
					double Value = 0.0;
					for (int32_t Offset = -Radius; Offset <= Radius; ++Offset)
					{
						Value += Kernel[static_cast<size_t>(Offset + Radius)] * Raw[static_cast<size_t>(Row) * Size + Clamp(Column + Offset)];
					}
					Rows[static_cast<size_t>(Row) * Size + Column] = Value;
				}
			}
			Smoothed.assign(Raw.size(), 0.0);
			for (int32_t Row = 0; Row < Size; ++Row)
			{
				for (int32_t Column = 0; Column < Size; ++Column)
				{
					double Value = 0.0;
					for (int32_t Offset = -Radius; Offset <= Radius; ++Offset)
					{
						Value += Kernel[static_cast<size_t>(Offset + Radius)] * Rows[static_cast<size_t>(Clamp(Row + Offset)) * Size + Column];
					}
					Smoothed[static_cast<size_t>(Row) * Size + Column] = Value;
				}
			}
			bBuilt = true;
		}

		bool IsBuilt() const { return bBuilt; }
		bool HasWater() const { return bAnyWater; }
		double GetCentreLat() const { return CentreLat; }
		double GetCentreLon() const { return CentreLon; }

		/**
		 * La frazione d'acqua lisciata in un punto e il suo gradiente (per
		 * metro, verso nord e verso est). Falso fuori dalla mappa.
		 */
		bool Sample(double LatDeg, double LonDeg, double& OutFraction, double& OutGradNorth, double& OutGradEast) const
		{
			OutFraction = 0.0;
			OutGradNorth = OutGradEast = 0.0;
			if (!bBuilt) { return false; }
			// In celle, dal centro della prima: riga 0 a nord, colonna 0 a ovest.
			const double Column = (LonDeg - CentreLon) * MetresEast / CellM + 0.5 * (Size - 1);
			const double Row = (CentreLat - LatDeg) * 111132.0 / CellM + 0.5 * (Size - 1);
			if (Column < 0.0 || Row < 0.0 || Column > Size - 1.0 || Row > Size - 1.0) { return false; }
			OutFraction = Bilinear(Row, Column);
			// Differenze centrate di mezza cella: il gradiente del lisciato.
			const double East = (Bilinear(Row, std::min(Size - 1.0, Column + 0.5)) - Bilinear(Row, std::max(0.0, Column - 0.5)));
			const double North = (Bilinear(std::max(0.0, Row - 0.5), Column) - Bilinear(std::min(Size - 1.0, Row + 0.5), Column));
			OutGradEast = East / CellM;
			OutGradNorth = North / CellM;
			return true;
		}

	private:
		void CellCentre(int32_t Row, int32_t Column, double& OutLat, double& OutLon) const
		{
			OutLat = CentreLat - (Row - 0.5 * (Size - 1)) * CellM / 111132.0;
			OutLon = CentreLon + (Column - 0.5 * (Size - 1)) * CellM / MetresEast;
		}

		double Bilinear(double Row, double Column) const
		{
			const int32_t R0 = std::max(0, std::min(Size - 2, static_cast<int32_t>(std::floor(Row))));
			const int32_t C0 = std::max(0, std::min(Size - 2, static_cast<int32_t>(std::floor(Column))));
			const double Fr = Row - R0, Fc = Column - C0;
			auto At = [this](int32_t R, int32_t C) { return Smoothed[static_cast<size_t>(R) * Size + C]; };
			const double Top = At(R0, C0) + (At(R0, C0 + 1) - At(R0, C0)) * Fc;
			const double Bottom = At(R0 + 1, C0) + (At(R0 + 1, C0 + 1) - At(R0 + 1, C0)) * Fc;
			return Top + (Bottom - Top) * Fr;
		}

		bool bBuilt = false;
		bool bAnyWater = false;
		double CentreLat = 0.0, CentreLon = 0.0, MetresEast = 111320.0;
		std::vector<double> Smoothed;
	};

	// =========================================================================
	//  Le brezze in un punto
	// =========================================================================

	/** Il suolo attorno a un punto, per le brezze (lo prepara chi conosce terreno e acqua). */
	struct FBreezeTerrain
	{
		bool bOverWater = false;
		/** La mappa dell'acqua: frazione lisciata e gradiente (per metro). */
		bool bHaveWater = false;
		double WaterFraction = 0.0, WaterGradNorth = 0.0, WaterGradEast = 0.0;
		/** Pendenza del suolo (salita per metro) su +-150 m: i pendii. */
		double SlopeNorth = 0.0, SlopeEast = 0.0;
		/** La stessa su +-2,5 km: le valli. */
		double ValleyNorth = 0.0, ValleyEast = 0.0;
	};

	/** Quanto si e' sulla costa, 0..1: 1 sulla riva di un mare, ~0,13 a 20 km, quasi 0 a 30 km. */
	inline double CoastalShare(const FBreezeTerrain& Terrain)
	{
		if (!Terrain.bHaveWater) { return 0.0; }
		const double Gradient = std::sqrt(Terrain.WaterGradNorth * Terrain.WaterGradNorth + Terrain.WaterGradEast * Terrain.WaterGradEast);
		// Su una costa diritta il gradiente del lisciato vale al massimo 1 / (sigma radice di 2 pi greco).
		return std::min(1.0, Gradient * CoastScaleM * std::sqrt(2.0 * WindPi));
	}

	/** Quanto il vento del tempo che fa lascia vivere le brezze, 0..1. */
	inline double BreezeSurvival(double ReferenceSpeedMs)
	{
		return std::max(0.0, std::min(1.0, 1.0 - ReferenceSpeedMs / BreezeKillWindMs));
	}

	/**
	 * Le tre brezze in un punto: [0] mare/terra, [1] pendio, [2] valle.
	 * ReferenceSpeedMs: il vento del tempo che fa a 10 m (le spegne).
	 */
	inline void ComputeBreezes(const FBreezeClimate& Climate, const FBreezeTerrain& Terrain, double ReferenceSpeedMs,
	                           const FBreezeSettings& Settings, FBreezeLayer OutLayers[BreezeLayerCount])
	{
		for (int32_t Index = 0; Index < BreezeLayerCount; ++Index) { OutLayers[Index] = FBreezeLayer(); }
		if (!Settings.bEnabled || !Climate.IsValid()) { return; }
		const double Scale = std::max(0.0, std::min(3.0, Settings.Strength)) * BreezeSurvival(ReferenceSpeedMs);
		if (Scale <= 0.0) { return; }

		// --- 1. Mare e terra ----------------------------------------------------
		const double Coastal = CoastalShare(Terrain);
		const double Gradient = std::sqrt(Terrain.WaterGradNorth * Terrain.WaterGradNorth + Terrain.WaterGradEast * Terrain.WaterGradEast);
		if (Coastal > 0.01 && Gradient > 0.0)
		{
			// Verso terra: contro il gradiente dell'acqua.
			const double LandNorth = -Terrain.WaterGradNorth / Gradient, LandEast = -Terrain.WaterGradEast / Gradient;
			FBreezeLayer& Layer = OutLayers[0];
			const double State = Climate.Coast();
			if (State > 0.0)
			{
				const double Speed = SeaBreezeMaxMs * State * Coastal * Scale;
				Layer.North = LandNorth * Speed;
				Layer.East = LandEast * Speed;
				Layer.PeakM = 150.0;
				Layer.DepthM = 700.0;
				Layer.Return = 0.3;
			}
			else
			{
				const double Speed = LandBreezeMaxMs * std::min(1.0, -State / LandBreezeFullState) * Coastal * Scale;
				Layer.North = -LandNorth * Speed;
				Layer.East = -LandEast * Speed;
				Layer.PeakM = 30.0;
				Layer.DepthM = 250.0;
			}
		}
		if (Terrain.bOverWater) { return; }   // sull'acqua niente pendii ne' valli

		// --- 2. I pendii --------------------------------------------------------
		const double Steepness = std::sqrt(Terrain.SlopeNorth * Terrain.SlopeNorth + Terrain.SlopeEast * Terrain.SlopeEast);
		if (Steepness > 1.0e-3)
		{
			const double UpNorth = Terrain.SlopeNorth / Steepness, UpEast = Terrain.SlopeEast / Steepness;
			const double SlopeShare = std::min(1.0, Steepness / 0.15);
			FBreezeLayer& Layer = OutLayers[1];
			const double State = Climate.Slope();
			if (State > 0.0)
			{
				// Al sole: un versante che guarda il sole si scalda di piu' del
				// piano, uno in ombra meno. La normale del pendio contro il sole.
				const double SunAltitude = Climate.SunAltitudeDeg() * Sky::DegToRad, SunAzimuth = Climate.SunAzimuthDeg() * Sky::DegToRad;
				const double Sun[3] = { std::cos(SunAltitude) * std::cos(SunAzimuth), std::cos(SunAltitude) * std::sin(SunAzimuth), std::sin(SunAltitude) };
				const double Length = std::sqrt(1.0 + Steepness * Steepness);
				const double Normal[3] = { -Terrain.SlopeNorth / Length, -Terrain.SlopeEast / Length, 1.0 / Length };
				const double OnSlope = Normal[0] * Sun[0] + Normal[1] * Sun[1] + Normal[2] * Sun[2];
				const double Aspect = std::max(0.0, std::min(1.5, OnSlope / std::max(0.2, Sun[2])));
				const double Speed = SlopeWindMaxMs * State * SlopeShare * Aspect * Scale;
				Layer.North = UpNorth * Speed;
				Layer.East = UpEast * Speed;
				Layer.PeakM = 25.0;
				Layer.DepthM = 120.0;
				Layer.SlopeUp = Steepness;
			}
			else
			{
				const double Speed = SlopeWindMaxMs * (-State) * SlopeShare * Scale;
				Layer.North = -UpNorth * Speed;
				Layer.East = -UpEast * Speed;
				Layer.PeakM = 4.0;
				Layer.DepthM = 40.0;
				Layer.SlopeUp = -Steepness;
			}
		}

		// --- 3. Le valli --------------------------------------------------------
		const double Rise = std::sqrt(Terrain.ValleyNorth * Terrain.ValleyNorth + Terrain.ValleyEast * Terrain.ValleyEast);
		if (Rise > 1.0e-4)
		{
			const double UpNorth = Terrain.ValleyNorth / Rise, UpEast = Terrain.ValleyEast / Rise;
			const double State = Climate.Valley();
			const double Speed = ValleyWindMaxMs * std::fabs(State) * std::min(1.0, Rise / 0.03) * Scale;
			const double Sign = State > 0.0 ? 1.0 : -1.0;
			FBreezeLayer& Layer = OutLayers[2];
			Layer.North = Sign * UpNorth * Speed;
			Layer.East = Sign * UpEast * Speed;
			Layer.PeakM = State > 0.0 ? 100.0 : 50.0;
			Layer.DepthM = State > 0.0 ? 500.0 : 250.0;
			Layer.Return = State > 0.0 ? 0.2 : 0.0;
		}
	}
}

// =============================================================================
//  Precipitation.h -- Pioggia, neve, grandine. STRATO: C++ PURO, header-only.
//
//  Quattro pezzi, tutti senza Unreal e provati da geoweather_tests:
//
//   1. IL CAMPO (FPrecipitationField): dove cade e quanto. Un tempo
//      impostato (intensita' in mm/h d'acqua, copertura, dimensione dei
//      rovesci, temperatura) diventa una mappa: con copertura 1 piove
//      dappertutto (pioggia "stratiforme"), con copertura 0,3 piove a
//      rovesci che coprono il 30% del suolo, trasportati dal vento in quota
//      e che nascono e muoiono in mezz'ora. La grandine cade nei NUCLEI dei
//      rovesci, dove l'intensita' e' massima.
//   2. COSA CADE, per quota: la temperatura scende di 6,5 °C al km; sopra lo
//      zero termico nevica, sotto piove, in mezzo (qualche centinaio di metri)
//      cade mista. In montagna neve, in valle pioggia, con lo stesso tempo.
//   3. IL SUOLO (FGroundWeather): si bagna sotto la pioggia e si asciuga
//      dopo; la neve si posa sopra la quota neve e la copertura scende mentre
//      nevica, poi sale mentre fonde.
//   4. LE PARTICELLE (FPrecipitationParticles): velocita' di caduta (gocce,
//      fiocchi, chicchi), densita' e dimensioni da disegnare, e lo spostamento
//      di ogni tipo, INTEGRATO frame per frame (come le onde e le raffiche:
//      niente salti quando cambia il vento).
//
//  Piu' la VISIBILITA' (quanto la precipitazione la riduce) e la mesh delle
//  particelle (BuildPrecipitationMesh): quadrati degeneri che il materiale
//  apre e muove sulla scheda video.
//
//  UNITA'. Metri, secondi, °C, mm/h di acqua equivalente (la neve cade piu'
//  "spessa": 1 mm/h d'acqua sono ~1 cm/h di neve). Vettori: (nord, est, su).
// =============================================================================
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

#include "Weather/WindField.h"

namespace GeoWorld::Weather
{
	/** Cosa cade: deciso dalla temperatura (Auto) o imposto. */
	enum class EPrecipitationKind : uint8_t
	{
		Auto,     // pioggia sotto lo zero termico, neve sopra
		Rain,     // sempre pioggia
		Snow,     // sempre neve
		Hail,     // pioggia (o neve in quota) e GRANDINE nei nuclei dei rovesci
	};

	struct FPrecipitationSettings
	{
		EPrecipitationKind Kind = EPrecipitationKind::Auto;
		/** Intensita' nel cuore della precipitazione, mm/h d'acqua. 0 = sereno. */
		double RateMmH = 0.0;
		/** Quanto suolo e' sotto la precipitazione: 1 dappertutto, 0,3 rovesci sparsi. */
		double Coverage = 1.0;
		/** Dimensione tipica dei rovesci, km. */
		double CellSizeKm = 8.0;
		/** Temperatura al livello del mare, °C, e quanto cala salendo, °C/km. */
		double SeaLevelTemperatureC = 15.0;
		double LapseRateCPerKm = 6.5;
		uint32_t Seed = 7;
	};

	/** La precipitazione in un punto. Le tre frazioni sommano a 1 se cade qualcosa. */
	struct FPrecipitationSample
	{
		double RateMmH = 0.0;
		double Rain = 0.0, Snow = 0.0, Hail = 0.0;
		double TemperatureC = 15.0;
		/** Quanto la precipitazione lascia vedere, m (infinito senza precipitazione). */
		double VisibilityM = std::numeric_limits<double>::infinity();

		bool IsFalling() const { return RateMmH > 0.01; }
	};

	namespace PrecipitationDetail
	{
		using WindDetail::Clamp;

		/**
		 * La mappa dei rovesci: le stesse quattro ottave della turbolenza del
		 * vento (stessa statistica, deviazione ~1), ma il TEMPO non accelera
		 * con l'ottava: tutto il disegno cambia insieme, in decine di minuti.
		 * Con il tempo moltiplicato come lo spazio (la turbolenza), i
		 * dettagli di un rovescio cambierebbero otto volte piu' in fretta del
		 * rovescio stesso, e non lo si vedrebbe viaggiare.
		 */
		inline double CellNoise(double X, double Y, double T, uint32_t Seed)
		{
			double Sum = 0.0, Amplitude = 1.0, Frequency = 1.0;
			for (int32_t Octave = 0; Octave < WindDetail::TurbulenceOctaves; ++Octave)
			{
				Sum += Amplitude * WindDetail::GradientNoise(X * Frequency + 17.31 * Octave, Y * Frequency - 9.17 * Octave,
				                                             T + 3.71 * Octave, Seed + 101u * static_cast<uint32_t>(Octave));
				Amplitude *= 0.7937005259840998;    // 2^(-1/3)
				Frequency *= 2.0;
			}
			return Sum / WindDetail::TurbulenceStd;
		}

		inline double Smoothstep(double Edge0, double Edge1, double X)
		{
			const double T = Clamp((X - Edge0) / (Edge1 - Edge0), 0.0, 1.0);
			return T * T * (3.0 - 2.0 * T);
		}

		/**
		 * L'inversa della normale standard (Acklam, errore relativo < 1,2e-9):
		 * la soglia del rumore che lascia sopra di se' la frazione voluta.
		 */
		inline double InverseNormal(double P)
		{
			P = Clamp(P, 1.0e-12, 1.0 - 1.0e-12);
			static const double A[6] = { -3.969683028665376e+01, 2.209460984245205e+02, -2.759285104469687e+02,
			                             1.383577518672690e+02, -3.066479806614716e+01, 2.506628277459239e+00 };
			static const double B[5] = { -5.447609879822406e+01, 1.615858368580409e+02, -1.556989798598866e+02,
			                             6.680131188771972e+01, -1.328068155288572e+01 };
			static const double C[6] = { -7.784894002430293e-03, -3.223964580411365e-01, -2.400758277161838e+00,
			                             -2.549732539343734e+00, 4.374664141464968e+00, 2.938163982698783e+00 };
			static const double D[4] = { 7.784695709041462e-03, 3.224671290700398e-01, 2.445134137142996e+00,
			                             3.754408661907416e+00 };
			const double Low = 0.02425;
			if (P < Low)
			{
				const double Q = std::sqrt(-2.0 * std::log(P));
				return (((((C[0] * Q + C[1]) * Q + C[2]) * Q + C[3]) * Q + C[4]) * Q + C[5])
				     / ((((D[0] * Q + D[1]) * Q + D[2]) * Q + D[3]) * Q + 1.0);
			}
			if (P > 1.0 - Low)
			{
				const double Q = std::sqrt(-2.0 * std::log(1.0 - P));
				return -(((((C[0] * Q + C[1]) * Q + C[2]) * Q + C[3]) * Q + C[4]) * Q + C[5])
				     / ((((D[0] * Q + D[1]) * Q + D[2]) * Q + D[3]) * Q + 1.0);
			}
			const double Q = P - 0.5, R = Q * Q;
			return (((((A[0] * R + A[1]) * R + A[2]) * R + A[3]) * R + A[4]) * R + A[5]) * Q
			     / (((((B[0] * R + B[1]) * R + B[2]) * R + B[3]) * R + B[4]) * R + 1.0);
		}
	}

	// =========================================================================
	//  Visibilita' e caduta: le formule, separate per i test
	// =========================================================================

	/**
	 * Coefficiente di estinzione (1/km) di ogni precipitazione, dalle sue mm/h.
	 * La visibilita' (contrasto 2%, Koschmieder) e' 3,912 / estinzione:
	 *   pioggia  0,29 R^0,67   ->  1 mm/h: 13,5 km;  10 mm/h: 2,9 km;  50 mm/h: 1 km
	 *   neve     3,9  S^0,75   ->  1 mm/h: 1 km;      4 mm/h: 0,35 km
	 *   grandine come la pioggia (pesa molto, nasconde poco).
	 * La neve "nasconde" dieci volte la pioggia a parita' d'acqua: fiocchi
	 * grandi, lenti, tanti.
	 */
	inline double RainExtinctionPerKm(double RateMmH) { return RateMmH > 0.0 ? 0.29 * std::pow(RateMmH, 0.67) : 0.0; }
	inline double SnowExtinctionPerKm(double RateMmH) { return RateMmH > 0.0 ? 3.9 * std::pow(RateMmH, 0.75) : 0.0; }

	/** Visibilita' in metri da una miscela (intensita' totale e frazioni). */
	inline double PrecipitationVisibilityM(double RateMmH, double Rain, double Snow, double Hail)
	{
		const double Extinction = RainExtinctionPerKm(RateMmH * (Rain + Hail)) + SnowExtinctionPerKm(RateMmH * Snow);
		return (Extinction > 0.0) ? 3912.0 / Extinction : std::numeric_limits<double>::infinity();
	}

	/**
	 * Velocita' di caduta delle gocce: diametro mediano di Marshall-Palmer
	 * (0,89 R^0,21 mm) e la velocita' terminale di Atlas (9,65 - 10,3 e^(-0,6 D)).
	 * 1 mm/h: ~4 m/s; 50 mm/h: ~7 m/s.
	 */
	inline double RainFallSpeed(double RateMmH)
	{
		const double Diameter = 0.89 * std::pow(std::max(RateMmH, 0.1), 0.21);
		return std::max(2.0, 9.65 - 10.3 * std::exp(-0.6 * Diameter));
	}
	/** I fiocchi: ~1 m/s, poco piu' con la neve bagnata (vicino allo zero). */
	inline double SnowFallSpeed(double TemperatureC) { return 0.9 + 0.6 * PrecipitationDetail::Smoothstep(-3.0, 1.0, TemperatureC); }
	/** Diametro dei chicchi (m): 5 mm con poca grandine, fino a 2 cm. */
	inline double HailDiameter(double HailRateMmH) { return 0.005 + 0.015 * PrecipitationDetail::Clamp(HailRateMmH / 30.0, 0.0, 1.0); }
	/** Velocita' terminale dei chicchi: 14,5 m/s per un centimetro, come radice del diametro. */
	inline double HailFallSpeed(double DiameterM) { return 14.5 * std::sqrt(std::max(DiameterM, 0.001) / 0.01); }

	// =========================================================================
	//  1-2. Il campo
	// =========================================================================

	class FPrecipitationField
	{
	public:
		void SetSettings(const FPrecipitationSettings& InSettings)
		{
			using PrecipitationDetail::Clamp;
			Settings = InSettings;
			Settings.RateMmH = Clamp(Settings.RateMmH, 0.0, 300.0);
			Settings.Coverage = Clamp(Settings.Coverage, 0.0, 1.0);
			Settings.CellSizeKm = Clamp(Settings.CellSizeKm, 0.5, 200.0);
			Settings.LapseRateCPerKm = Clamp(Settings.LapseRateCPerKm, 0.0, 12.0);
			Settings.SeaLevelTemperatureC = Clamp(Settings.SeaLevelTemperatureC, -60.0, 60.0);
		}
		const FPrecipitationSettings& GetSettings() const { return Settings; }

		/**
		 * Avanza il tempo e TRASPORTA i rovesci con il vento in quota (quello
		 * che li porta: ~3000 m), integrato come le raffiche.
		 */
		void Advance(double DeltaSeconds, double SteeringNorth, double SteeringEast)
		{
			if (!(DeltaSeconds > 0.0)) { return; }
			DriftNorth += SteeringNorth * DeltaSeconds;
			DriftEast += SteeringEast * DeltaSeconds;
			Seconds += DeltaSeconds;
		}

		double TemperatureAt(double AltitudeM) const
		{
			return Settings.SeaLevelTemperatureC - Settings.LapseRateCPerKm * AltitudeM / 1000.0;
		}

		/** Lo zero termico, m s.l.m. (0 se gela gia' al livello del mare). */
		double FreezingLevelM() const
		{
			if (Settings.SeaLevelTemperatureC <= 0.0) { return 0.0; }
			if (Settings.LapseRateCPerKm <= 0.0) { return 1.0e6; }
			return 1000.0 * Settings.SeaLevelTemperatureC / Settings.LapseRateCPerKm;
		}

		/**
		 * La quota neve: dove cade meta' neve e meta' pioggia, 300 m sotto lo
		 * zero termico (i fiocchi ci mettono qualche centinaio di metri a
		 * fondere). Con la neve imposta, ovunque; con la pioggia imposta, mai.
		 */
		double SnowLineM() const
		{
			if (Settings.Kind == EPrecipitationKind::Snow) { return -1.0e6; }
			if (Settings.Kind == EPrecipitationKind::Rain) { return 1.0e6; }
			return FreezingLevelM() - 300.0;
		}

		/** Frazione di neve a una quota: 0 sotto quota neve - 150 m, 1 sopra + 150 m. */
		double SnowFractionAt(double AltitudeM) const
		{
			const double Line = SnowLineM();
			return PrecipitationDetail::Clamp((AltitudeM - (Line - 150.0)) / 300.0, 0.0, 1.0);
		}

		/**
		 * La cima delle nuvole che piovono, m s.l.m.: 6 km per una pioggia
		 * estesa (i nembostrati), 10 km per i rovesci e la grandine (i
		 * cumulonembi). Sopra non cade niente; nell'ultimo chilometro sfuma.
		 */
		double CloudTopM() const
		{
			const bool bShowers = Settings.Coverage < 0.999 || Settings.Kind == EPrecipitationKind::Hail;
			return bShowers ? 10000.0 : 6000.0;
		}

		/**
		 * Quanto del massimo cade qui, 0..1, senza la quota: la mappa dei
		 * rovesci. Rumore di gradiente in quattro ottave, trasportato e che
		 * cambia in ~40 minuti; la soglia che lascia sopra di se' la frazione
		 * "copertura" del suolo; da li' sale a 1 in 1,2 deviazioni (i nuclei).
		 */
		double Intensity(double LatDeg, double LonDeg) const
		{
			using namespace PrecipitationDetail;
			if (Settings.RateMmH <= 0.0 || Settings.Coverage <= 0.0) { return 0.0; }
			const double Scale = Settings.CellSizeKm * 1000.0;
			const double X = (LatDeg * 111132.0 - DriftNorth) / Scale;
			const double Y = (LonDeg * 111320.0 * std::cos(LatDeg * WindDegToRad) - DriftEast) / Scale;
			// Un rovescio vive ~40 minuti: in quel tempo il disegno cambia di un'unita' del rumore.
			const double Noise = CellNoise(X, Y, Seconds / 2400.0, Settings.Seed + 5000u);
			if (Settings.Coverage >= 0.999)
			{
				// Dappertutto, con un'ondulazione del 15% (non e' mai uniforme).
				return Clamp(0.85 + 0.15 * Noise, 0.5, 1.0);
			}
			const double Threshold = InverseNormal(1.0 - Settings.Coverage);
			const double Core = Clamp((Noise - Threshold) / 1.2, 0.0, 1.0);
			return Core > 0.0 ? Clamp(0.15 + 0.85 * Core, 0.0, 1.0) : 0.0;
		}

		/** La precipitazione in un punto, a una quota s.l.m. */
		FPrecipitationSample Sample(double LatDeg, double LonDeg, double AltitudeM) const
		{
			using namespace PrecipitationDetail;
			FPrecipitationSample Out;
			Out.TemperatureC = TemperatureAt(AltitudeM);
			const double Level = Intensity(LatDeg, LonDeg);
			const double Ceiling = Clamp((CloudTopM() - AltitudeM) / 1000.0, 0.0, 1.0);
			if (Level <= 0.0 || Ceiling <= 0.0) { return Out; }
			Out.RateMmH = Settings.RateMmH * Level * Ceiling;
			// La grandine: nei nuclei (dove si sale verso il massimo), fino al 60%.
			Out.Hail = (Settings.Kind == EPrecipitationKind::Hail) ? 0.6 * Smoothstep(0.55, 0.9, Level) : 0.0;
			const double Snow = SnowFractionAt(AltitudeM);
			Out.Snow = (1.0 - Out.Hail) * Snow;
			Out.Rain = (1.0 - Out.Hail) * (1.0 - Snow);
			Out.VisibilityM = PrecipitationVisibilityM(Out.RateMmH, Out.Rain, Out.Snow, Out.Hail);
			return Out;
		}

		double GetTime() const { return Seconds; }

	private:
		FPrecipitationSettings Settings;
		double Seconds = 0.0;
		double DriftNorth = 0.0, DriftEast = 0.0;
	};

	// =========================================================================
	//  3. Il suolo
	// =========================================================================

	/**
	 * Il suolo bagnato e la neve posata. Uno stato per il mondo, non per
	 * punto: lo guida la precipitazione SOTTO LA CAMERA (che e' quella che si
	 * vede). La neve dipende dalla quota: sopra la COPERTURA il suolo e'
	 * bianco.
	 *
	 *   - si bagna in ~4 minuti di pioggia (prima, con la pioggia forte), si
	 *     asciuga in ~40 (molto piu' lentamente sotto zero);
	 *   - mentre nevica, la copertura scende verso la quota neve, tanto piu' in
	 *     fretta quanto piu' nevica (con 1 mm/h la meta' della strada in ~10
	 *     minuti);
	 *   - senza neve fonde: la copertura sale di 20 m all'ora per ogni grado
	 *     sopra zero alla sua quota.
	 */
	class FGroundWeather
	{
	public:
		/** "Nessuna neve": la copertura sta piu' in alto di qualunque montagna. */
		static constexpr double NoSnowM = 1.0e5;

		void SetWetness(double Value) { Wetness = PrecipitationDetail::Clamp(Value, 0.0, 1.0); }
		void SetSnowCover(double AltitudeM) { SnowCoverM = AltitudeM; }
		void ClearSnowCover() { SnowCoverM = NoSnowM; }
		double GetWetness() const { return Wetness; }
		double GetSnowCoverM() const { return SnowCoverM; }
		bool HasSnow() const { return SnowCoverM < NoSnowM * 0.5; }

		/**
		 * @param Here      la precipitazione al suolo sotto la camera
		 * @param Field     per la quota neve e la temperatura alla copertura
		 */
		void Advance(double DeltaSeconds, const FPrecipitationSample& Here, const FPrecipitationField& Field)
		{
			if (!(DeltaSeconds > 0.0)) { return; }
			const double Rain = Here.RateMmH * (Here.Rain + Here.Hail);
			if (Rain > 0.1)
			{
				const double Rate = PrecipitationDetail::Clamp(Rain / 2.0, 0.25, 2.0) / 240.0;
				Wetness = std::min(1.0, Wetness + DeltaSeconds * Rate);
			}
			else
			{
				const double Dry = (Here.TemperatureC > 0.0) ? 1.0 / 2400.0 : 0.2 / 2400.0;
				Wetness = std::max(0.0, Wetness - DeltaSeconds * Dry);
			}

			// La neve: cade dove la quota neve sta sotto la copertura. Si
			// guarda l'intensita' qui (la camera vede la nevicata che ha attorno).
			const double Snowing = Here.RateMmH;
			const double Line = std::max(Field.SnowLineM(), -500.0);
			if (Snowing > 0.05 && Line < SnowCoverM)
			{
				const double Start = std::min(SnowCoverM, Line + 3000.0);    // da nessuna neve: si comincia dall'alto
				const double Rate = PrecipitationDetail::Clamp(Snowing, 0.05, 10.0) / 900.0;
				SnowCoverM = Line + (Start - Line) * std::exp(-DeltaSeconds * Rate);
			}
			else if (HasSnow())
			{
				const double Warmth = std::max(0.0, Field.TemperatureAt(SnowCoverM));
				SnowCoverM += DeltaSeconds * Warmth * 20.0 / 3600.0;
				if (Warmth > 0.0 && SnowCoverM > 9000.0) { SnowCoverM = NoSnowM; }
			}
		}

	private:
		double Wetness = 0.0;
		double SnowCoverM = NoSnowM;
	};

	// =========================================================================
	//  4. Le particelle
	// =========================================================================

	enum class EParticleType : int32_t { Rain = 0, Snow = 1, Hail = 2 };
	inline constexpr int32_t ParticleTypeCount = 3;

	/** Un tipo di particella, pronto per il materiale. */
	struct FParticleLayer
	{
		/** Velocita' (nord, est, su) in m/s: vento piu' caduta. */
		double Velocity[3] = { 0.0, 0.0, 0.0 };
		/** Spostamento accumulato meno la posizione della camera, ridotto alla scatola (m). */
		double Phase[3] = { 0.0, 0.0, 0.0 };
		/** Quante particelle di questo tipo si vedono, 0..1. */
		double Density = 0.0;
		/** Dimensione: diametro di fiocchi e chicchi, scia delle gocce (m). */
		double Size = 0.0;
	};

	/**
	 * Le particelle attorno alla camera: una scatola (default 50 x 50 x 30 m)
	 * che si ripete nello spazio. Ogni particella ha un punto fisso nella
	 * scatola e, sulla scheda video, sta a (punto + spostamento - camera)
	 * ridotto alla scatola: cosi' le particelle stanno ferme nel MONDO mentre
	 * la camera si muove (parallasse giusta), e cadono con il loro spostamento.
	 */
	class FPrecipitationParticles
	{
	public:
		double Box[3] = { 50.0, 50.0, 30.0 };

		/**
		 * @param Here         la precipitazione alla quota della camera
		 * @param WindNorth... il vento alla camera (m/s), con le raffiche
		 * @param Camera       la posizione della camera nel mondo (m, nord-est-su)
		 */
		void Advance(double DeltaSeconds, const FPrecipitationSample& Here, double WindNorth, double WindEast,
		             double WindUp, const double Camera[3])
		{
			using PrecipitationDetail::Clamp;
			const double Rain = Here.RateMmH * Here.Rain, Snow = Here.RateMmH * Here.Snow, Hail = Here.RateMmH * Here.Hail;
			const double HailSize = HailDiameter(Hail);
			const double Fall[ParticleTypeCount] = { RainFallSpeed(Rain), SnowFallSpeed(Here.TemperatureC), HailFallSpeed(HailSize) };
			// I fiocchi seguono il vento del tutto, le gocce quasi, i chicchi meno.
			const double Follow[ParticleTypeCount] = { 0.95, 1.0, 0.7 };
			const double Densities[ParticleTypeCount] = {
				Clamp(std::sqrt(Rain / 20.0), 0.0, 1.0),
				Clamp(std::pow(Snow / 2.0, 0.6), 0.0, 1.0),
				Clamp(std::sqrt(Hail / 10.0), 0.0, 1.0) };
			for (int32_t Type = 0; Type < ParticleTypeCount; ++Type)
			{
				FParticleLayer& Layer = Layers[Type];
				Layer.Velocity[0] = Follow[Type] * WindNorth;
				Layer.Velocity[1] = Follow[Type] * WindEast;
				Layer.Velocity[2] = Follow[Type] * WindUp - Fall[Type];
				Layer.Density = Densities[Type];
				for (int32_t Axis = 0; Axis < 3; ++Axis)
				{
					if (DeltaSeconds > 0.0) { Shift[Type][Axis] = Wrap(Shift[Type][Axis] + Layer.Velocity[Axis] * DeltaSeconds, Box[Axis]); }
					Layer.Phase[Axis] = Wrap(Shift[Type][Axis] - Camera[Axis], Box[Axis]);
				}
			}
			// La scia di una goccia: quanto cade in 1/30 di secondo (l'occhio e
			// le camere la vedono cosi'), fra 5 cm e 40 cm.
			Layers[0].Size = Clamp(std::sqrt(Layers[0].Velocity[0] * Layers[0].Velocity[0] + Layers[0].Velocity[1] * Layers[0].Velocity[1]
			                                  + Layers[0].Velocity[2] * Layers[0].Velocity[2]) / 30.0, 0.05, 0.4);
			Layers[1].Size = 0.012;
			Layers[2].Size = HailSize;
		}

		/** Il mondo si e' spostato sotto la camera (rebase): le particelle restano dove sono. */
		void ApplyOriginShift(const double Delta[3])
		{
			for (int32_t Type = 0; Type < ParticleTypeCount; ++Type)
			{
				for (int32_t Axis = 0; Axis < 3; ++Axis) { Shift[Type][Axis] = Wrap(Shift[Type][Axis] - Delta[Axis], Box[Axis]); }
			}
		}

		const FParticleLayer& GetLayer(EParticleType Type) const { return Layers[static_cast<int32_t>(Type)]; }

		/** La frazione di ogni tipo fra le particelle: chi cade, in proporzione a quanto si vede. */
		void GetMix(double& OutRain, double& OutSnow, double& OutHail) const
		{
			const double Total = Layers[0].Density + Layers[1].Density + Layers[2].Density;
			if (Total <= 0.0) { OutRain = 1.0; OutSnow = OutHail = 0.0; return; }
			OutRain = Layers[0].Density / Total;
			OutSnow = Layers[1].Density / Total;
			OutHail = Layers[2].Density / Total;
		}

		static double Wrap(double Value, double Size)
		{
			return Value - Size * std::floor(Value / Size + 0.5);
		}

	private:
		FParticleLayer Layers[ParticleTypeCount];
		double Shift[ParticleTypeCount][3] = {};
	};

	// =========================================================================
	//  La mesh delle particelle
	// =========================================================================

	/**
	 * Count quadrati, ognuno di 4 vertici NELLO STESSO PUNTO (il suo posto
	 * nella scatola, metri, centrata nell'origine) e due triangoli: degeneri
	 * finche' il materiale non li apre (World Position Offset). UV0 = angolo
	 * (0/1, 0/1), UV1 = due numeri a caso per particella (tipo, dimensione,
	 * fase, se si vede).
	 */
	struct FPrecipitationMesh
	{
		std::vector<float> Positions;   // 3 per vertice
		std::vector<float> Corners;     // 2 per vertice
		std::vector<float> Seeds;       // 2 per vertice
		std::vector<uint32_t> Indices;  // 6 per particella
	};

	inline void BuildPrecipitationMesh(int32_t Count, const double Box[3], uint32_t Seed, FPrecipitationMesh& Out)
	{
		Out = FPrecipitationMesh();
		Count = std::max(0, Count);
		Out.Positions.reserve(static_cast<size_t>(Count) * 12);
		Out.Corners.reserve(static_cast<size_t>(Count) * 8);
		Out.Seeds.reserve(static_cast<size_t>(Count) * 8);
		Out.Indices.reserve(static_cast<size_t>(Count) * 6);
		auto Unit = [Seed](int32_t Index, int64_t Channel)
		{
			return static_cast<double>(WindDetail::Hash(Index, Channel, 77, Seed) & 0xFFFFFFu) / 16777216.0;
		};
		static const float CornerUv[4][2] = { { 0.0f, 0.0f }, { 1.0f, 0.0f }, { 1.0f, 1.0f }, { 0.0f, 1.0f } };
		for (int32_t Index = 0; Index < Count; ++Index)
		{
			const float Centre[3] = {
				static_cast<float>((Unit(Index, 0) - 0.5) * Box[0]),
				static_cast<float>((Unit(Index, 1) - 0.5) * Box[1]),
				static_cast<float>((Unit(Index, 2) - 0.5) * Box[2]) };
			const float SeedA = static_cast<float>(Unit(Index, 3));
			const float SeedB = static_cast<float>(Unit(Index, 4));
			const uint32_t First = static_cast<uint32_t>(Index) * 4u;
			for (int32_t Corner = 0; Corner < 4; ++Corner)
			{
				Out.Positions.insert(Out.Positions.end(), { Centre[0], Centre[1], Centre[2] });
				Out.Corners.insert(Out.Corners.end(), { CornerUv[Corner][0], CornerUv[Corner][1] });
				Out.Seeds.insert(Out.Seeds.end(), { SeedA, SeedB });
			}
			Out.Indices.insert(Out.Indices.end(), { First, First + 1u, First + 2u, First, First + 2u, First + 3u });
		}
	}
}

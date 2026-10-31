// =============================================================================
//  WindField.h -- Il vento in ogni punto: la simulazione. STRATO: C++ PURO.
//
//  Fino alla Fase 9 il vento era un numero solo (velocita', direzione,
//  raffiche come due sinusoidi). Qui diventa un CAMPO: in ogni punto e a ogni
//  quota, a ogni istante, un vettore (nord, est, su) in m/s. Lo stesso campo
//  per tutti: le onde, le stampe a schermo, le frecce del debug, e domani le
//  nuvole, gli alberi, gli aerei della Fase 7.
//
//  COSA LO FA, dal piu' grande al piu' piccolo (docs/vento-design.md):
//
//   1. IL TEMPO CHE FA. Il vento di riferimento: quello di una stazione meteo,
//      a 10 m su un prato (rugosita' 3 cm), come in un METAR. Con la
//      "variabilita'" la sua velocita' e direzione derivano piano (decine di
//      minuti), uguali dappertutto.
//   2. LO STRATO LIMITE. Sopra ~600 m il vento non sente il suolo (vento di
//      gradiente); sotto, l'attrito lo frena con un profilo logaritmico che
//      dipende dalla RUGOSITA': sul mare (0,2 mm) arriva al suolo piu' forte,
//      in citta' (1 m) piu' debole. E gira con la quota: al suolo e' ruotato
//      verso la bassa pressione (nell'emisfero nord: a sinistra) di 10° sul
//      mare e ~30° sulla terra.
//   3. I RILIEVI. Sulle creste accelera, nelle valli rallenta, sottovento a un
//      pendio ripido c'e' il ridosso (meno vento, piu' turbolenza); dove il
//      vento sale su un pendio, sale anche lui (vento verticale). Effetti che
//      si spengono salendo.
//   3b. LE BREZZE (Weather/Breeze.h): i venti locali che fa il sole (di mare e
//      di terra, di pendio, di valle), sommati al vento medio, ognuna col
//      suo profilo in quota.
//   4. LA TURBOLENZA. Raffiche COERENTI nello spazio: un campo di rumore con
//      la lunghezza di scala dell'atmosfera vera (decine o centinaia di metri),
//      TRASPORTATO dal vento medio ("turbolenza congelata", Taylor): la stessa
//      raffica passa sopra il lago e poi sulla riva, e si vede correre.
//
//  Tutto e' deterministico (un seme): la raffica che vede l'acqua e' quella
//  che un aereo, nello stesso punto e allo stesso istante, sentirebbe.
//
//  UNITA'. Metri, secondi, gradi. Direzione: quella DA CUI soffia (bollettini).
//  Vettori: (nord, est, su), in m/s.
// =============================================================================
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "Weather/Wind.h"

namespace GeoWorld::Weather
{
	inline constexpr double WindPi = 3.14159265358979323846;
	inline constexpr double WindDegToRad = WindPi / 180.0;
	/** Costante di von Karman, quella del profilo logaritmico. */
	inline constexpr double VonKarman = 0.4;

	// --- Rugosita' del suolo (z0, metri) ----------------------------------------
	inline constexpr double RoughnessWater = 0.0002;
	/** Il prato della stazione meteo: il vento di riferimento e' misurato qui. */
	inline constexpr double RoughnessReference = 0.03;
	/** Terra "media" senza dati di copertura: campi, siepi, case sparse. */
	inline constexpr double RoughnessLand = 0.1;
	inline constexpr double RoughnessUrban = 1.0;

	/** Quota del vento di riferimento sul suolo, metri. */
	inline constexpr double ReferenceHeightM = 10.0;
	/** Cima dello strato limite: sopra, il vento di gradiente non sente il suolo. */
	inline constexpr double BoundaryLayerTopM = 600.0;

	/** Vento in quota: strati come nei bollettini dei venti in quota (e nel CIGI). */
	struct FWindLayer
	{
		double AltitudeM = 1500.0;     // sul livello del mare
		double SpeedMs = 10.0;
		double FromDegrees = 270.0;
	};

	struct FWindSettings
	{
		/** Il vento di riferimento: 10 m su un prato, come una stazione meteo. */
		FWind Reference{ 5.0, 240.0 };
		/** 0 = aria liscia, 1 = la turbolenza dell'atmosfera vera, fino a 2. */
		double Turbulence = 1.0;
		/** 0 = il tempo non cambia, 1 = deriva di +-20% e +-15° (tipici) in decine di minuti. */
		double Variability = 0.0;
		/** Facoltativi, ordinati per quota: sopra lo strato limite il vento li segue. */
		std::vector<FWindLayer> Aloft;
		/** Rugosita' della terra dove non si sa di meglio. */
		double LandRoughnessM = RoughnessLand;
		uint32_t Seed = 1;
	};

	/**
	 * Una BREZZA: un vento locale fatto dal sole (Weather/Breeze.h), con il
	 * suo profilo in quota. Il vettore e' quello del massimo; sotto cresce dal
	 * suolo come un profilo logaritmico, sopra cala fino a DepthM; oltre, se
	 * Return > 0, torna indietro (il flusso di ritorno della brezza di mare).
	 */
	struct FBreezeLayer
	{
		double North = 0.0, East = 0.0;
		/** Quota del massimo sul suolo, m. */
		double PeakM = 100.0;
		/** Spessore: a questa quota la brezza e' finita, m. */
		double DepthM = 500.0;
		/** Il ritorno in quota, frazione del massimo (fra DepthM e 2,2 DepthM). */
		double Return = 0.0;
		/** Pendenza del suolo lungo la brezza (> 0 se sale): vicino al suolo l'aria segue il pendio. */
		double SlopeUp = 0.0;
	};

	/** Mare/terra, pendio, valle. */
	inline constexpr int32_t BreezeLayerCount = 3;

	/** La frazione del massimo della brezza alla quota z sul suolo (negativa nel ritorno). */
	inline double BreezeProfile(double HeightM, const FBreezeLayer& Layer)
	{
		const double Z = HeightM, Depth = std::max(1.0, Layer.DepthM);
		if (Z <= 0.0) { return 0.0; }
		if (Z <= Depth)
		{
			const double Near = std::min(1.0, std::log(1.0 + Z / 0.1) / std::log(1.0 + std::max(Layer.PeakM, 0.2) / 0.1));
			const double Ratio = Z / Depth;
			return Near * (1.0 - Ratio * Ratio);
		}
		if (Z < 2.2 * Depth && Layer.Return > 0.0)
		{
			return -Layer.Return * std::sin(WindPi * (Z - Depth) / (1.2 * Depth));
		}
		return 0.0;
	}

	/**
	 * Quello che il suolo fa al vento in un punto: rugosita' e rilievi. Lo
	 * prepara chi conosce il terreno (ComputeOrography + chi sa dov'e' l'acqua).
	 */
	struct FSurfaceInfluence
	{
		double RoughnessM = RoughnessLand;
		/** Fattore di velocita' al suolo dei rilievi: 1,3 = cresta, 0,8 = valle. */
		double SpeedUp = 1.0;
		/** 0..1: ridosso sottovento a un pendio ripido. */
		double Shelter = 0.0;
		/** Pendenza del suolo nella direzione del vento (> 0 in salita): w = u · pendenza. */
		double SlopeAlongWind = 0.0;
		/** Quota (sul suolo) a cui gli effetti dei rilievi sono scesi a 1/e. */
		double DecayHeightM = 400.0;
		/** Le brezze qui (Weather/Breeze.h ComputeBreezes); zero = nessuna. */
		FBreezeLayer Breezes[BreezeLayerCount];
	};

	/** La somma delle brezze alla quota z sul suolo (nord, est), m/s. */
	inline void BreezeAt(const FSurfaceInfluence& Surface, double HeightM, double& OutNorth, double& OutEast)
	{
		OutNorth = 0.0;
		OutEast = 0.0;
		for (const FBreezeLayer& Layer : Surface.Breezes)
		{
			const double Share = BreezeProfile(HeightM, Layer);
			OutNorth += Layer.North * Share;
			OutEast += Layer.East * Share;
		}
	}

	/** Il vento in un punto. */
	struct FWindSample
	{
		/** Totale: medio + turbolenza, m/s. */
		double North = 0.0, East = 0.0, Up = 0.0;
		/** Solo il medio (niente raffiche). */
		double MeanNorth = 0.0, MeanEast = 0.0, MeanUp = 0.0;
		/** Deviazione standard della componente lungo il vento, m/s. */
		double SigmaU = 0.0;
		/** Lunghezza di scala della turbolenza, m. */
		double TurbulenceLengthM = 0.0;

		double MeanSpeed() const { return std::sqrt(MeanNorth * MeanNorth + MeanEast * MeanEast); }
		double Speed() const { return std::sqrt(North * North + East * East); }
		/** Direzione da cui soffia il vento medio, 0..360. */
		double MeanFromDegrees() const
		{
			double Degrees = std::atan2(-MeanEast, -MeanNorth) / WindDegToRad;
			if (Degrees < 0.0) { Degrees += 360.0; }
			return Degrees;
		}
		/** La raffica attesa (di picco, ~3 s): media piu' tre deviazioni. */
		double GustSpeed() const { return MeanSpeed() + 3.0 * SigmaU; }
	};

	namespace WindDetail
	{
		inline double Clamp(double Value, double Low, double High) { return std::min(High, std::max(Low, Value)); }

		inline double WrapDegrees(double Degrees)
		{
			Degrees = std::fmod(Degrees, 360.0);
			return (Degrees < 0.0) ? Degrees + 360.0 : Degrees;
		}

		/** (nord, est) del vento che soffia DA FromDegrees a Speed. */
		inline void ToVector(double Speed, double FromDegrees, double& OutNorth, double& OutEast)
		{
			const double Toward = (FromDegrees + 180.0) * WindDegToRad;
			OutNorth = Speed * std::cos(Toward);
			OutEast = Speed * std::sin(Toward);
		}

		inline uint32_t Hash(int64_t X, int64_t Y, int64_t Z, uint32_t Seed)
		{
			uint64_t H = static_cast<uint64_t>(X) * 0x9E3779B97F4A7C15ULL
			           ^ static_cast<uint64_t>(Y) * 0xC2B2AE3D27D4EB4FULL
			           ^ static_cast<uint64_t>(Z) * 0x165667B19E3779F9ULL
			           ^ (static_cast<uint64_t>(Seed) << 32);
			H ^= H >> 33; H *= 0xFF51AFD7ED558CCDULL;
			H ^= H >> 33; H *= 0xC4CEB9FE1A85EC53ULL;
			H ^= H >> 33;
			return static_cast<uint32_t>(H);
		}

		inline double Fade(double T) { return T * T * T * (T * (T * 6.0 - 15.0) + 10.0); }

		/** Il gradiente di un nodo: uno dei 12 spigoli del cubo (Perlin "improved"). */
		inline double Gradient(uint32_t H, double X, double Y, double Z)
		{
			switch (H % 12u)
			{
			case 0: return X + Y;   case 1: return -X + Y;  case 2: return X - Y;   case 3: return -X - Y;
			case 4: return X + Z;   case 5: return -X + Z;  case 6: return X - Z;   case 7: return -X - Z;
			case 8: return Y + Z;   case 9: return -Y + Z;  case 10: return Y - Z;  default: return -Y - Z;
			}
		}

		/** Rumore di gradiente 3D, liscio (derivate continue), media zero. */
		inline double GradientNoise(double X, double Y, double Z, uint32_t Seed)
		{
			const double Fx = std::floor(X), Fy = std::floor(Y), Fz = std::floor(Z);
			const int64_t Ix = static_cast<int64_t>(Fx), Iy = static_cast<int64_t>(Fy), Iz = static_cast<int64_t>(Fz);
			const double Dx = X - Fx, Dy = Y - Fy, Dz = Z - Fz;
			const double U = Fade(Dx), V = Fade(Dy), W = Fade(Dz);
			auto Corner = [&](int64_t Ox, int64_t Oy, int64_t Oz)
			{
				return Gradient(Hash(Ix + Ox, Iy + Oy, Iz + Oz, Seed),
					Dx - static_cast<double>(Ox), Dy - static_cast<double>(Oy), Dz - static_cast<double>(Oz));
			};
			const double X00 = Corner(0, 0, 0) + U * (Corner(1, 0, 0) - Corner(0, 0, 0));
			const double X10 = Corner(0, 1, 0) + U * (Corner(1, 1, 0) - Corner(0, 1, 0));
			const double X01 = Corner(0, 0, 1) + U * (Corner(1, 0, 1) - Corner(0, 0, 1));
			const double X11 = Corner(0, 1, 1) + U * (Corner(1, 1, 1) - Corner(0, 1, 1));
			const double Y0 = X00 + V * (X10 - X00);
			const double Y1 = X01 + V * (X11 - X01);
			return Y0 + W * (Y1 - Y0);
		}

		/**
		 * Quattro ottave, ognuna meta' lunghezza e ampiezza 2^(-1/3) della
		 * precedente: la cascata di Kolmogorov (spettro -5/3) tagliata a 1/8
		 * della scala. Divisa per la sua deviazione standard misurata (0,407 su
		 * 400.000 punti): esce a deviazione ~1, e il test lo ricontrolla.
		 */
		inline constexpr int32_t TurbulenceOctaves = 4;
		inline constexpr double TurbulenceStd = 0.4074;

		inline double Turbulence(double X, double Y, double Z, uint32_t Seed)
		{
			double Sum = 0.0, Amplitude = 1.0, Frequency = 1.0;
			for (int32_t Octave = 0; Octave < TurbulenceOctaves; ++Octave)
			{
				Sum += Amplitude * GradientNoise(X * Frequency + 17.31 * Octave, Y * Frequency - 9.17 * Octave,
				                                 Z * Frequency + 3.71 * Octave, Seed + 101u * static_cast<uint32_t>(Octave));
				Amplitude *= 0.7937005259840998;    // 2^(-1/3)
				Frequency *= 2.0;
			}
			return Sum / TurbulenceStd;
		}

		/** Rumore lento in una dimensione (il tempo): deviazione ~0,5 (misurata: 0,321 prima di scalare), liscio. */
		inline double SlowNoise(double T, uint32_t Seed)
		{
			return 0.5 / 0.3209 * (GradientNoise(T, 0.37, 0.71, Seed) + 0.5 * GradientNoise(2.0 * T + 5.3, 0.13, 0.29, Seed + 7u));
		}
	}

	/**
	 * Rotazione al suolo rispetto al vento di gradiente, in gradi: 10° sul
	 * mare, ~27° sul prato, ~35° in citta' (valori di letteratura: lo strato
	 * di Ekman). Positiva = al suolo il vento "viene" da meno gradi.
	 */
	inline double SurfaceTurningDegrees(double RoughnessM)
	{
		return WindDetail::Clamp(10.0 + 8.0 * std::log10(std::max(RoughnessM, 1.0e-5) / RoughnessWater), 5.0, 40.0);
	}

	/**
	 * Rotazione a quota z sul suolo: tutta (SurfaceTurningDegrees) fino a 10 m,
	 * poi cala con il logaritmo della quota fino a zero in cima allo strato limite.
	 */
	inline double TurningAt(double HeightM, double RoughnessM)
	{
		const double Z = std::min(BoundaryLayerTopM, std::max(ReferenceHeightM, HeightM));
		return SurfaceTurningDegrees(RoughnessM)
		     * (1.0 - std::log(Z / ReferenceHeightM) / std::log(BoundaryLayerTopM / ReferenceHeightM));
	}

	/** Frazione del vento di gradiente a quota z sul suolo, profilo logaritmico. */
	inline double LogProfile(double HeightM, double RoughnessM)
	{
		const double Z = WindDetail::Clamp(HeightM, 0.0, BoundaryLayerTopM);
		return std::log((Z + RoughnessM) / RoughnessM) / std::log((BoundaryLayerTopM + RoughnessM) / RoughnessM);
	}

	/**
	 * Intensita' di turbolenza (sigma_u / U) a quota z: 1 / ln(z / z0), come
	 * nell'Eurocodice del vento (EN 1991-1-4), sopra una quota minima e
	 * saturata in citta'. Sopra lo strato limite resta un fondo del 5%.
	 */
	inline double TurbulenceIntensity(double HeightM, double RoughnessM)
	{
		if (HeightM >= BoundaryLayerTopM) { return 0.05; }
		const double Z = std::max(HeightM, std::max(2.0, 20.0 * RoughnessM));
		return WindDetail::Clamp(1.0 / std::log(Z / RoughnessM), 0.05, 0.35);
	}

	/** Lunghezza di scala della turbolenza (EN 1991-1-4, appendice B), metri. */
	inline double TurbulenceLength(double HeightM, double RoughnessM)
	{
		const double Z = WindDetail::Clamp(HeightM, std::max(2.0, 20.0 * RoughnessM), BoundaryLayerTopM);
		const double Alpha = 0.67 + 0.05 * std::log(RoughnessM);
		return std::max(30.0, 300.0 * std::pow(Z / 200.0, Alpha));
	}

	/**
	 * Il campo. Ha uno STATO: il tempo e quanto il vento ha trasportato la
	 * turbolenza. Il trasporto si INTEGRA (Advance), non si calcola come
	 * velocita' x tempo: con la variabilita' la velocita' cambia, e dopo
	 * un'ora di gioco un metro al secondo in piu' sposterebbe tutte le raffiche
	 * di 3,6 km in un colpo (lo stesso errore che le onde evitano con le fasi).
	 */
	class FWindField
	{
	public:
		void SetSettings(const FWindSettings& InSettings)
		{
			Settings = InSettings;
			Settings.Reference.SpeedMs = WindDetail::Clamp(Settings.Reference.SpeedMs, 0.0, 80.0);
			Settings.Reference.FromDegrees = WindDetail::WrapDegrees(Settings.Reference.FromDegrees);
			Settings.Turbulence = WindDetail::Clamp(Settings.Turbulence, 0.0, 2.0);
			Settings.Variability = WindDetail::Clamp(Settings.Variability, 0.0, 1.0);
			Settings.LandRoughnessM = WindDetail::Clamp(Settings.LandRoughnessM, RoughnessWater, 3.0);
			std::sort(Settings.Aloft.begin(), Settings.Aloft.end(),
				[](const FWindLayer& A, const FWindLayer& B) { return A.AltitudeM < B.AltitudeM; });
		}
		const FWindSettings& GetSettings() const { return Settings; }

		/**
		 * Avanza il tempo e trasporta la turbolenza con il vento di adesso.
		 * LatDeg serve solo al verso della rotazione (emisfero).
		 */
		void Advance(double DeltaSeconds, double LatDeg = 45.0)
		{
			if (!(DeltaSeconds > 0.0)) { return; }
			// Il trasporto: il vento di gradiente a ~100 m (0,7), lo stesso per
			// tutte le quote. Integrato a passi brevi anche se il frame e' lungo.
			const int32_t Steps = std::max(1, static_cast<int32_t>(std::ceil(DeltaSeconds / 0.5)));
			const double Step = DeltaSeconds / Steps;
			for (int32_t Index = 0; Index < Steps; ++Index)
			{
				const FWind Gradient = GradientWind(LatDeg);
				double North = 0.0, East = 0.0;
				WindDetail::ToVector(0.7 * Gradient.SpeedMs, Gradient.FromDegrees, North, East);
				DriftNorth += North * Step;
				DriftEast += East * Step;
				Seconds += Step;
			}
		}
		double GetTime() const { return Seconds; }

		/**
		 * Il vento di riferimento ADESSO: quello impostato, con la deriva lenta
		 * della variabilita' (tipicamente +-20% di velocita' e +-15° in decine
		 * di minuti; in un giorno si arriva al doppio).
		 * Uguale dappertutto: e' il tempo che fa, non il posto.
		 */
		FWind ReferenceWind() const
		{
			FWind Wind = Settings.Reference;
			if (Settings.Variability > 0.0)
			{
				const double Speed = WindDetail::SlowNoise(Seconds / 1500.0, Settings.Seed + 11u);
				const double Turn = WindDetail::SlowNoise(Seconds / 2100.0, Settings.Seed + 23u);
				Wind.SpeedMs = std::max(0.0, Wind.SpeedMs * (1.0 + 0.4 * Settings.Variability * Speed));
				Wind.FromDegrees = WindDetail::WrapDegrees(Wind.FromDegrees + 30.0 * Settings.Variability * Turn);
			}
			return Wind;
		}

		/**
		 * Il vento di gradiente (in cima allo strato limite) che da' il vento di
		 * riferimento: dal prato della stazione si risale il profilo e la
		 * rotazione. Nell'emisfero sud la rotazione va al contrario.
		 */
		FWind GradientWind(double LatDeg) const
		{
			const FWind Reference = ReferenceWind();
			FWind Gradient;
			Gradient.SpeedMs = Reference.SpeedMs / LogProfile(ReferenceHeightM, RoughnessReference);
			const double Sign = (LatDeg >= 0.0) ? 1.0 : -1.0;
			Gradient.FromDegrees = WindDetail::WrapDegrees(Reference.FromDegrees
			                                              + Sign * TurningAt(ReferenceHeightM, RoughnessReference));
			return Gradient;
		}

		/** Il vento medio a 10 m sopra una superficie piana di rugosita' data (per le onde). */
		FWind SurfaceWind(double LatDeg, double RoughnessM) const
		{
			const FWind Gradient = GradientWind(LatDeg);
			const double Sign = (LatDeg >= 0.0) ? 1.0 : -1.0;
			FWind Wind;
			Wind.SpeedMs = Gradient.SpeedMs * LogProfile(ReferenceHeightM, RoughnessM);
			Wind.FromDegrees = WindDetail::WrapDegrees(Gradient.FromDegrees - Sign * TurningAt(ReferenceHeightM, RoughnessM));
			return Wind;
		}

		/**
		 * Il vento in un punto, adesso.
		 *
		 * @param HeightAboveGroundM  quota sul suolo (o sull'acqua) del punto
		 * @param AltitudeM           quota sul livello del mare (per i venti in quota)
		 * @param Surface             rugosita' e rilievi sotto il punto
		 */
		FWindSample Sample(double LatDeg, double LonDeg, double HeightAboveGroundM, double AltitudeM,
		                   const FSurfaceInfluence& Surface) const
		{
			using namespace WindDetail;
			FWindSample Out;
			const double Z = std::max(0.0, HeightAboveGroundM);
			const double Roughness = Clamp(Surface.RoughnessM, RoughnessWater, 3.0);
			const double Sign = (LatDeg >= 0.0) ? 1.0 : -1.0;

			// --- 1-2. Il vento medio: gradiente, profilo, rotazione -------------
			const FWind Gradient = GradientWind(LatDeg);
			double MeanNorth = 0.0, MeanEast = 0.0;
			ToVector(Gradient.SpeedMs * LogProfile(Z, Roughness), Gradient.FromDegrees - Sign * TurningAt(Z, Roughness),
			         MeanNorth, MeanEast);

			// Sopra lo strato limite: verso i venti in quota, se ci sono. Si
			// parte dal vento di gradiente in cima allo strato (continuo con il
			// profilo) e si interpola fra gli strati; sopra l'ultimo, l'ultimo.
			const double Top = AltitudeM - Z + BoundaryLayerTopM;
			if (AltitudeM > Top && !Settings.Aloft.empty())
			{
				double LowAltitude = Top, LowNorth = 0.0, LowEast = 0.0;
				ToVector(Gradient.SpeedMs, Gradient.FromDegrees, LowNorth, LowEast);
				MeanNorth = LowNorth;
				MeanEast = LowEast;
				for (const FWindLayer& Layer : Settings.Aloft)
				{
					if (Layer.AltitudeM <= Top) { continue; }     // dentro lo strato limite: comanda il suolo
					double LayerNorth = 0.0, LayerEast = 0.0;
					ToVector(Layer.SpeedMs, Layer.FromDegrees, LayerNorth, LayerEast);
					if (AltitudeM <= Layer.AltitudeM)
					{
						const double T = (AltitudeM - LowAltitude) / std::max(1.0, Layer.AltitudeM - LowAltitude);
						MeanNorth = LowNorth + T * (LayerNorth - LowNorth);
						MeanEast = LowEast + T * (LayerEast - LowEast);
						break;
					}
					LowAltitude = Layer.AltitudeM;
					LowNorth = MeanNorth = LayerNorth;
					LowEast = MeanEast = LayerEast;
				}
			}

			// --- 3. I rilievi: si spengono salendo -------------------------------
			const double Decay = std::exp(-Z / std::max(1.0, Surface.DecayHeightM));
			const double Shelter = Clamp(Surface.Shelter, 0.0, 1.0);
			const double Ground = Clamp(Surface.SpeedUp, 0.3, 2.0) * (1.0 - 0.6 * Shelter);
			const double Factor = 1.0 + (Ground - 1.0) * Decay;
			MeanNorth *= Factor;
			MeanEast *= Factor;
			const double Synoptic = std::sqrt(MeanNorth * MeanNorth + MeanEast * MeanEast);
			Out.MeanUp = Synoptic * Clamp(Surface.SlopeAlongWind, -0.6, 0.6) * Decay;

			// --- 3b. Le brezze: venti locali del sole, ognuna col suo profilo --------
			for (const FBreezeLayer& Layer : Surface.Breezes)
			{
				const double Share = BreezeProfile(Z, Layer);
				if (Share == 0.0) { continue; }
				MeanNorth += Layer.North * Share;
				MeanEast += Layer.East * Share;
				// Vicino al suolo la brezza di pendio segue il pendio: sale o scende con lui.
				Out.MeanUp += std::sqrt(Layer.North * Layer.North + Layer.East * Layer.East) * std::max(Share, 0.0)
				            * Clamp(Layer.SlopeUp, -0.6, 0.6);
			}
			const double Horizontal = std::sqrt(MeanNorth * MeanNorth + MeanEast * MeanEast);
			Out.MeanNorth = MeanNorth;
			Out.MeanEast = MeanEast;
			Out.North = Out.MeanNorth;
			Out.East = Out.MeanEast;
			Out.Up = Out.MeanUp;

			// --- 4. La turbolenza, congelata e trasportata ------------------------
			const double Intensity = TurbulenceIntensity(Z, Roughness) * (1.0 + 1.5 * Shelter);
			Out.SigmaU = Settings.Turbulence * Intensity * Horizontal;
			Out.TurbulenceLengthM = TurbulenceLength(Z, Roughness);
			if (Out.SigmaU > 1.0e-6)
			{
				const double L = Out.TurbulenceLengthM;
				// Metri sul piano locale, meno quanto il vento ha trasportato il campo.
				const double X = (LatDeg * 111132.0 - DriftNorth) / L;
				const double Y = (LonDeg * 111320.0 * std::cos(LatDeg * WindDegToRad) - DriftEast) / L;
				// E la raffica cambia mentre viaggia: in qualche L / sigma. La quota
				// sposta il campo: strati diversi, raffiche diverse ma vicine.
				const double T = Seconds * std::max(Out.SigmaU, 0.2) / (3.0 * L) + Z / L;
				const double Along = Turbulence(X, Y, T, Settings.Seed);
				const double Across = 0.75 * Turbulence(X + 31.7, Y - 47.3, T, Settings.Seed + 1000u);
				const double Vertical = 0.5 * Turbulence(X - 73.1, Y + 12.9, T, Settings.Seed + 2000u);
				const double UnitNorth = (Horizontal > 1.0e-9) ? MeanNorth / Horizontal : 1.0;
				const double UnitEast = (Horizontal > 1.0e-9) ? MeanEast / Horizontal : 0.0;
				Out.North += Out.SigmaU * (Along * UnitNorth - Across * UnitEast);
				Out.East += Out.SigmaU * (Along * UnitEast + Across * UnitNorth);
				// Vicino al suolo l'aria non entra nel terreno: la verticale si smorza.
				Out.Up += Out.SigmaU * Vertical * Clamp(Z / 20.0, 0.0, 1.0);
			}
			return Out;
		}

	private:
		FWindSettings Settings;
		double Seconds = 0.0;
		double DriftNorth = 0.0, DriftEast = 0.0;
	};

	/**
	 * I rilievi attorno a un punto, dal terreno: `HeightAt(Lon, Lat, OutHeight)`
	 * ritorna falso dove non sa (allora vale la quota del punto). Il vento
	 * soffia VERSO (TowardNorth, TowardEast), vettore unitario.
	 *
	 *  - ACCELERAZIONE: quanto il punto sta sopra (o sotto) la media dei due
	 *    anelli a 500 m e 2 km, diviso 1 km: una "pendenza" alla Jackson-Hunt
	 *    (+200 m su una collina larga un paio di km: +32% di vento in cresta);
	 *  - RIDOSSO: il pendio piu' ripido sopravvento entro 1,5 km; oltre 14°
	 *    il punto e' riparato (fino al -60% di vento, +150% di turbolenza);
	 *  - PENDENZA lungo il vento, su +-150 m: il vento verticale.
	 * 25 quote per punto.
	 */
	template <typename THeightAt>
	FSurfaceInfluence ComputeOrography(const THeightAt& HeightAt, double LatDeg, double LonDeg,
	                                   double TowardNorth, double TowardEast, double RoughnessM)
	{
		using namespace WindDetail;
		FSurfaceInfluence Out;
		Out.RoughnessM = RoughnessM;
		const double MetresNorth = 111132.0, MetresEast = 111320.0 * std::cos(LatDeg * WindDegToRad);
		double Centre = 0.0;
		if (!HeightAt(LonDeg, LatDeg, Centre)) { return Out; }
		auto At = [&](double North, double East)
		{
			double Height = Centre;
			if (!HeightAt(LonDeg + East / MetresEast, LatDeg + North / MetresNorth, Height)) { Height = Centre; }
			return Height;
		};

		double RingSum = 0.0;
		for (int32_t Index = 0; Index < 8; ++Index)
		{
			const double Angle = Index * WindPi / 4.0;
			RingSum += At(500.0 * std::cos(Angle), 500.0 * std::sin(Angle));
			RingSum += At(2000.0 * std::cos(Angle), 2000.0 * std::sin(Angle));
		}
		const double Relative = Centre - RingSum / 16.0;
		Out.SpeedUp = Clamp(1.0 + 1.6 * Clamp(Relative / 1000.0, -0.4, 0.4), 0.5, 1.6);

		double Steepest = 0.0;
		const double Distances[6] = { 100.0, 200.0, 400.0, 700.0, 1000.0, 1500.0 };
		for (const double Distance : Distances)
		{
			const double Upwind = At(-TowardNorth * Distance, -TowardEast * Distance);
			Steepest = std::max(Steepest, std::atan2(Upwind - Centre, Distance));
		}
		Out.Shelter = Clamp((Steepest - 0.25) / 0.35, 0.0, 1.0);

		const double Ahead = At(TowardNorth * 150.0, TowardEast * 150.0);
		const double Behind = At(-TowardNorth * 150.0, -TowardEast * 150.0);
		Out.SlopeAlongWind = Clamp((Ahead - Behind) / 300.0, -0.6, 0.6);
		return Out;
	}
}

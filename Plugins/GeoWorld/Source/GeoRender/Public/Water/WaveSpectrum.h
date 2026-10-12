// =============================================================================
//  WaveSpectrum.h -- Dal vento alle onde. STRATO: C++ PURO, header-only.
//
//  Il materiale dell'acqua somma GerstnerWaveCount onde di Gerstner: ognuna
//  ha una direzione, una lunghezza, un'ampiezza e una "ripidita'". Qui si
//  decide QUALI onde, a partire dal vento, con la fisica del mare e non a
//  occhio:
//
//    - un mare "completamente sviluppato" (Pierson-Moskowitz, 1964) con vento
//      U a 10 m di quota ha altezza significativa Hs = 0,21 U² / g e una
//      lunghezza d'onda di picco Lp = 2 pi U² / (0,877² g): 8 m/s -> 1,4 m e
//      53 m; 15 m/s -> 4,8 m e 187 m;
//    - le sei onde stanno attorno al picco (da 0,5 a 2 Lp), si spartiscono
//      l'energia di Hs secondo lo spettro, vanno nella direzione VERSO cui
//      soffia il vento con una dispersione di +-40 gradi (le piu' lunghe piu'
//      allineate);
//    - la velocita' viene dalla dispersione delle acque profonde, w = sqrt(g k):
//      le onde lunghe corrono, quelle corte no.
//
//  NIENTE SALTI, IN TRE PUNTI
//    1. La fase di ogni onda si INTEGRA (fase -= w dt) invece di calcolarla come
//       w t, in double, e il materiale la riceve gia' ridotta a [0, 2 pi).
//    2. Un'onda che c'e' non cambia mai lunghezza ne' direzione (farebbe
//       "scorrere" il mare lontano): quando il vento cambia, le onde vecchie
//       sfumano a zero e quelle nuove crescono da zero. Vedi FWaveField.
//    3. Il REBASE sposta il mondo sotto le onde (le onde dipendono dalla
//       posizione nel mondo): ApplyOriginShift aggiunge a ogni fase k (D . delta)
//       e il mare resta dov'era.
//
//  UNITA' E ASSI. Metri e secondi. Le direzioni sono negli assi del mondo di
//  Unreal vicino all'origine: X = nord, Y = est (lo yaw e' l'azimut).
// =============================================================================
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

#include "Weather/Wind.h"

namespace GeoWorld::Water
{
	inline constexpr int32_t GerstnerWaveCount = 6;
	inline constexpr double Gravity = 9.80665;
	inline constexpr double TwoPi = 6.283185307179586;

	/** Il vento: sta nel meteo (Weather/Wind.h), le onde lo leggono. */
	using Weather::FWind;

	/** Altezza significativa di un mare completamente sviluppato, in metri. */
	inline double SignificantWaveHeight(double WindMs) { return 0.21 * WindMs * WindMs / Gravity; }

	/** Lunghezza d'onda di picco dello spettro, in metri. */
	inline double PeakWavelength(double WindMs)
	{
		return TwoPi * WindMs * WindMs / (0.877 * 0.877 * Gravity);
	}

	/** Quanta schiuma fa il vento sulle creste: niente sotto i 5 m/s, piena sopra i 12. */
	inline double WhitecapAmount(double WindMs)
	{
		const double T = std::min(1.0, std::max(0.0, (WindMs - 5.0) / 7.0));
		return T * T * (3.0 - 2.0 * T);
	}

	struct FGerstnerWave
	{
		double DirNorth = 1.0, DirEast = 0.0;   // unitario, verso cui l'onda corre
		double Wavelength = 1.0;                // m
		double Amplitude = 0.0;                 // m
		double Steepness = 0.0;                 // Q di Gerstner, 0..1
		double Number() const { return TwoPi / Wavelength; }                 // k, 1/m
		double AngularFrequency() const { return std::sqrt(Gravity * Number()); }   // w, 1/s
		double Speed() const { return AngularFrequency() / Number(); }       // m/s
	};

	using FWaveSet = std::array<FGerstnerWave, GerstnerWaveCount>;

	namespace Detail
	{
		/** Densita' spettrale di Pierson-Moskowitz in w (forma, non scala). */
		inline double PiersonMoskowitz(double Omega, double WindMs)
		{
			if (Omega <= 0.0 || WindMs <= 0.0) { return 0.0; }
			const double Peak = 0.877 * Gravity / WindMs;
			return std::exp(-1.25 * std::pow(Peak / Omega, 4.0)) / std::pow(Omega, 5.0);
		}
	}

	/**
	 * Le onde per un vento. `MaxSteepness` limita la somma delle ripidita' (la
	 * somma di Q k A sotto 1: oltre, le creste di Gerstner si ripiegano su se
	 * stesse in cappi).
	 */
	inline FWaveSet BuildWaveSet(const FWind& Wind, double MaxSteepness = 0.85)
	{
		// Lunghezze attorno al picco e scarti di direzione: deterministici, cosi'
		// lo stesso vento da' sempre lo stesso mare (e nessuno sfarfallio).
		static constexpr double LengthFactors[GerstnerWaveCount] = { 2.0, 1.55, 1.2, 0.95, 0.72, 0.52 };
		static constexpr double AngleOffsets[GerstnerWaveCount] = { 0.0, 14.0, -18.0, 27.0, -33.0, 40.0 };

		const double Speed = std::max(0.0, Wind.SpeedMs);
		const double Peak = std::max(1.0, PeakWavelength(std::max(Speed, 0.5)));
		const double Hs = SignificantWaveHeight(Speed);

		FWaveSet Waves;
		double Weights[GerstnerWaveCount];
		double WeightSum = 0.0;
		for (int32_t Index = 0; Index < GerstnerWaveCount; ++Index)
		{
			FGerstnerWave& Wave = Waves[static_cast<size_t>(Index)];
			Wave.Wavelength = Peak * LengthFactors[Index];
			const double Radians = (Wind.FromDegrees + 180.0 + AngleOffsets[Index]) * TwoPi / 360.0;
			Wave.DirNorth = std::cos(Radians);
			Wave.DirEast = std::sin(Radians);

			// Energia della banda: densita' dello spettro per la larghezza della
			// banda in w (le onde sono campioni di uno spettro continuo).
			const double Omega = Wave.AngularFrequency();
			const double Width = 0.25 * Omega;
			Weights[Index] = Detail::PiersonMoskowitz(Omega, std::max(Speed, 0.5)) * Width;
			WeightSum += Weights[Index];
		}

		// Hs = 4 sqrt(m0), e per una somma di sinusoidi m0 = somma A² / 2.
		const double M0 = (Hs / 4.0) * (Hs / 4.0);
		double KaSum = 0.0;
		for (int32_t Index = 0; Index < GerstnerWaveCount; ++Index)
		{
			FGerstnerWave& Wave = Waves[static_cast<size_t>(Index)];
			const double Share = (WeightSum > 0.0) ? Weights[Index] / WeightSum : 1.0 / GerstnerWaveCount;
			Wave.Amplitude = std::sqrt(2.0 * M0 * Share);
			KaSum += Wave.Number() * Wave.Amplitude;
		}

		// Ripidita': piu' vento, creste piu' aguzze; la somma di Q k A resta
		// sotto MaxSteepness (GPU Gems 1, cap. 1).
		const double Sharpness = std::min(1.0, 0.35 + Speed / 25.0);
		for (FGerstnerWave& Wave : Waves)
		{
			const double Ka = Wave.Number() * Wave.Amplitude;
			Wave.Steepness = (KaSum > 0.0 && Ka > 0.0)
				? std::min(1.0, Sharpness * MaxSteepness / KaSum) : 0.0;
		}
		return Waves;
	}

	/**
	 * Il mare che cambia, uno per mondo, aggiornato a ogni frame.
	 *
	 * IL PROBLEMA DA EVITARE: IL MARE CHE "SCORRE". Ogni onda e' sin(k D.X + fase).
	 * Se k o D cambiano anche di poco, in un punto lontano X la fase cambia di
	 * dk |X|: a 5 km dall'origine un 1% di k sono decine di radianti, cioe' le
	 * creste che corrono a velocita' assurde mentre il vento cambia. La prima
	 * versione ricalcolava le onde a ogni frame dal vento CON LE RAFFICHE: il
	 * mare lontano sarebbe scivolato avanti e indietro ogni dieci secondi.
	 *
	 * LA REGOLA: un'onda che c'e' non cambia MAI lunghezza ne' direzione.
	 *   - Se l'onda voluta dal vento e' quasi la stessa (lunghezza entro il 3%,
	 *     direzione entro 2 gradi) cambiano solo ampiezza e ripidita', con
	 *     dolcezza: quelle non fanno scorrere niente.
	 *   - Altrimenti l'onda SFUMA a zero, e solo a ampiezza zero viene
	 *     sostituita da quella nuova, che cresce dal nulla. Al massimo due onde
	 *     per volta, cosi' il mare non si spiana mai: un cambio di vento grosso
	 *     si completa in una decina di secondi.
	 *
	 * Il mare segue il vento MEDIO: le raffiche muovono l'increspatura (le
	 * normali di dettaglio) e la schiuma, non le onde lunghe. E' anche la
	 * fisica: un'onda di 50 m ci mette ore a formarsi, non segue un soffio.
	 */
	class FWaveField
	{
	public:
		/** Quanto dura la dissolvenza di un'onda (verso zero, o da zero), in secondi. */
		static constexpr double FadeSeconds = 2.0;
		/** Quante onde possono essere in sostituzione insieme. */
		static constexpr int32_t MaxSwapping = 2;

		/** Il vento voluto (quello medio, senza raffiche). */
		void SetTargetWind(const FWind& Wind) { Target = Wind; }
		/** Il vento a cui il mare sta arrivando (si avvicina a quello voluto in TimeConstant). */
		const FWind& GetWind() const { return Current; }
		/** Le onde come le disegna il materiale: ampiezze gia' pesate dalle dissolvenze. */
		const FWaveSet& GetWaves() const { return Effective; }
		double GetPhase(int32_t Index) const { return Phases[static_cast<size_t>(Index)]; }
		/** Quante onde stanno sfumando per essere sostituite (0 = mare stabile). */
		int32_t GetSwappingCount() const
		{
			int32_t Count = 0;
			for (const bool bSwap : Swapping) { Count += bSwap ? 1 : 0; }
			return Count;
		}

		/** Salta subito al vento voluto (all'avvio, o dopo un teletrasporto). */
		void SnapToTarget()
		{
			Current = Target;
			Slots = BuildWaveSet(Target);
			Weights.fill(1.0);
			Swapping.fill(false);
			UpdateEffective();
		}

		void Advance(double DeltaSeconds, double TimeConstant = 6.0)
		{
			if (DeltaSeconds <= 0.0) { return; }

			// Il vento "del mare" si avvicina a quello voluto come VETTORE:
			// girare da 350 a 10 gradi passa per il nord, non per tutta la rosa
			// dei venti. Serve all'overlay; le onde guardano gia' la meta.
			double TargetNorth = 0.0, TargetEast = 0.0, CurrentNorth = 0.0, CurrentEast = 0.0;
			Target.Toward(TargetNorth, TargetEast);
			Current.Toward(CurrentNorth, CurrentEast);
			const double Blend = 1.0 - std::exp(-DeltaSeconds / std::max(1e-3, TimeConstant));
			const double North = CurrentNorth * Current.SpeedMs + (TargetNorth * Target.SpeedMs - CurrentNorth * Current.SpeedMs) * Blend;
			const double East = CurrentEast * Current.SpeedMs + (TargetEast * Target.SpeedMs - CurrentEast * Current.SpeedMs) * Blend;
			const double Speed = std::sqrt(North * North + East * East);
			if (Speed > 1e-6)
			{
				// Verso cui soffia -> da cui soffia.
				Current.FromDegrees = std::fmod(std::atan2(East, North) * 360.0 / TwoPi + 180.0 + 360.0, 360.0);
			}
			Current.SpeedMs = Speed;

			// Le onde: ognuna verso la sua gemella del mare voluto.
			const FWaveSet Wanted = BuildWaveSet(Target);
			const double Fade = DeltaSeconds / FadeSeconds;
			for (int32_t Index = 0; Index < GerstnerWaveCount; ++Index)
			{
				const size_t I = static_cast<size_t>(Index);
				FGerstnerWave& Slot = Slots[I];
				const FGerstnerWave& Goal = Wanted[I];
				if (Swapping[I])
				{
					if (Weights[I] <= 0.0)
					{
						// Era gia' a zero nel frame scorso: ora, e solo ora,
						// l'onda cambia (zero prima, zero dopo). La fase resta
						// quella che era: con ampiezza zero non si vede.
						Slot = Goal;
						Swapping[I] = false;
					}
					else
					{
						Weights[I] = std::max(0.0, Weights[I] - Fade);
					}
					continue;
				}
				const bool bSameWave = std::fabs(Slot.Wavelength / Goal.Wavelength - 1.0) < 0.03
					&& Slot.DirNorth * Goal.DirNorth + Slot.DirEast * Goal.DirEast > 0.99939;   // cos 2 gradi
				if (!bSameWave && GetSwappingCount() < MaxSwapping)
				{
					Swapping[I] = true;
					continue;
				}
				Weights[I] = std::min(1.0, Weights[I] + Fade);
				if (bSameWave)
				{
					Slot.Amplitude += (Goal.Amplitude - Slot.Amplitude) * Blend;
					Slot.Steepness += (Goal.Steepness - Slot.Steepness) * Blend;
				}
			}

			for (int32_t Index = 0; Index < GerstnerWaveCount; ++Index)
			{
				double& Phase = Phases[static_cast<size_t>(Index)];
				Phase = std::fmod(Phase - Slots[static_cast<size_t>(Index)].AngularFrequency() * DeltaSeconds, TwoPi);
				if (Phase < 0.0) { Phase += TwoPi; }
			}
			UpdateEffective();
		}

		/**
		 * Il rebase: il mondo si e' spostato di (DeltaNorth, DeltaEast) metri (la
		 * vecchia posizione della nuova origine). Le onde restano ferme rispetto
		 * alla Terra.
		 */
		void ApplyOriginShift(double DeltaNorth, double DeltaEast)
		{
			for (int32_t Index = 0; Index < GerstnerWaveCount; ++Index)
			{
				const FGerstnerWave& Wave = Slots[static_cast<size_t>(Index)];
				double& Phase = Phases[static_cast<size_t>(Index)];
				Phase = std::fmod(Phase + Wave.Number() * (Wave.DirNorth * DeltaNorth + Wave.DirEast * DeltaEast), TwoPi);
				if (Phase < 0.0) { Phase += TwoPi; }
			}
		}

		/**
		 * Lo spostamento di Gerstner di un punto (nord, est, alto), in metri: lo
		 * stesso conto del materiale, qui per i test (e per chi vorra' far
		 * galleggiare una barca).
		 */
		void Displacement(double North, double East, double& OutNorth, double& OutEast, double& OutUp) const
		{
			OutNorth = OutEast = OutUp = 0.0;
			for (int32_t Index = 0; Index < GerstnerWaveCount; ++Index)
			{
				const FGerstnerWave& Wave = Effective[static_cast<size_t>(Index)];
				const double Theta = Wave.Number() * (Wave.DirNorth * North + Wave.DirEast * East)
				                   + Phases[static_cast<size_t>(Index)];
				const double Horizontal = Wave.Steepness * Wave.Amplitude * std::cos(Theta);
				OutNorth += Wave.DirNorth * Horizontal;
				OutEast += Wave.DirEast * Horizontal;
				OutUp += Wave.Amplitude * std::sin(Theta);
			}
		}

	private:
		/**
		 * Le onde pesate. Durante un cambio convivono onde del vento vecchio e
		 * del nuovo, ognuna con la sua ripidita': la somma di Q k A potrebbe
		 * superare il limite, e allora si scalano tutte le Q insieme.
		 */
		void UpdateEffective(double MaxSteepness = 0.85)
		{
			double KaQ = 0.0;
			for (int32_t Index = 0; Index < GerstnerWaveCount; ++Index)
			{
				const size_t I = static_cast<size_t>(Index);
				Effective[I] = Slots[I];
				Effective[I].Amplitude = Slots[I].Amplitude * Weights[I];
				KaQ += Effective[I].Steepness * Effective[I].Number() * Effective[I].Amplitude;
			}
			if (KaQ > MaxSteepness)
			{
				for (FGerstnerWave& Wave : Effective) { Wave.Steepness *= MaxSteepness / KaQ; }
			}
		}

		FWind Target;
		FWind Current;
		/** Le onde "vere": lunghezza e direzione fisse finche' non vengono sostituite. */
		FWaveSet Slots = BuildWaveSet(FWind{});
		FWaveSet Effective = BuildWaveSet(FWind{});
		std::array<double, GerstnerWaveCount> Weights = { 1.0, 1.0, 1.0, 1.0, 1.0, 1.0 };
		std::array<bool, GerstnerWaveCount> Swapping{};
		std::array<double, GerstnerWaveCount> Phases{};
	};
}

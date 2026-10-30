// =============================================================================
//  Lightning.h -- Fulmini e tuoni. STRATO: C++ PURO, header-only.
//
//  Quattro pezzi, tutti senza Unreal e provati da geoweather_tests:
//
//   1. DOVE E QUANDO (FLightningScheduler): i fulmini nascono nei NUCLEI dei
//      temporali, gli stessi della precipitazione (Precipitation.h), con una
//      densita' di fulmini per km^2 al minuto. Un processo di Poisson nel
//      cerchio attorno alla camera, "diradato" dove non c'e' un nucleo:
//      dove piove piano non fulmina.
//   2. IL LAMPO (FlashBrightness): un fulmine non e' un colpo solo. Il
//      primo "colpo di ritorno" e' il piu' forte, poi ne seguono altri sullo
//      stesso canale ogni qualche decina di millisecondi: e' per questo che
//      il lampo SFARFALLA. Fra un colpo e l'altro resta una luce debole.
//   3. LA FORMA (BuildLightningChannel): un canale tortuoso dalla nube al
//      suolo, con dei rami che si fermano a mezz'aria. Spostamento del punto
//      medio, ricorsivo: lo stesso trucco con cui si disegnano le coste
//      frattali.
//   4. IL TUONO (SynthesizeThunder): ogni pezzo del canale fa il suo botto
//      nello stesso istante, ma i pezzi stanno a distanze diverse da chi
//      ascolta, e il suono ci mette 3 secondi a fare un chilometro. Il botto
//      arriva quindi "spalmato" nel tempo: e' il ROMBO. E l'aria si mangia
//      gli acuti con la distanza: da vicino uno schiocco, da lontano un
//      brontolio basso.
//
//  UNITA'. Metri, secondi, gradi; il canale e' in metri locali (nord, est,
//  su) con il punto colpito nell'origine.
// =============================================================================
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "Weather/Precipitation.h"

namespace GeoWorld::Weather
{
	/** Quanti fulmini, e di che tipo. */
	struct FLightningSettings
	{
		/** 0 = niente fulmini; 1 = un temporale normale; fino a 5 (una supercella). */
		double Activity = 0.0;
		/** Quanti arrivano al suolo: circa uno su tre, gli altri restano nella nube. */
		double CloudToGroundFraction = 0.3;
		/** Fino a che distanza dalla camera nascono (m): oltre non si vedono ne' si sentono. */
		double RadiusM = 30000.0;
		uint32_t Seed = 11;
	};

	enum class ELightningKind : uint8_t
	{
		CloudToGround,   // nube-suolo: il canale che si vede
		IntraCloud,      // dentro la nube: si vede solo il cielo che si illumina
	};

	/** Al massimo tanti colpi di ritorno in un fulmine. */
	inline constexpr int32_t MaxStrokes = 8;

	/** Un fulmine. */
	struct FLightningStrike
	{
		ELightningKind Kind = ELightningKind::CloudToGround;
		double LatDeg = 0.0, LonDeg = 0.0;
		/** Quando comincia, sull'orologio dello scheduler (s). */
		double TimeSeconds = 0.0;
		/** I colpi: quanti, quando (dall'inizio, s) e quanto forti (il primo 1). */
		int32_t Strokes = 1;
		double StrokeTimes[MaxStrokes] = {};
		double StrokePeaks[MaxStrokes] = {};
		/** Il seme della forma del canale e del tuono. */
		uint32_t Seed = 0;
	};

	/** La densita' di fulmini nei nuclei con attivita' 1, per km^2 al minuto (un temporale: ~5 al minuto). */
	inline constexpr double CoreFlashDensityPerKm2PerMinute = 0.25;
	/** Fin dove si sente il tuono, m. */
	inline constexpr double ThunderAudibleM = 25000.0;

	/** La velocita' del suono nell'aria: 340 m/s a 15 °C, cioe' "tre secondi per chilometro". */
	inline double SpeedOfSound(double TemperatureC) { return 331.3 + 0.606 * TemperatureC; }

	namespace LightningDetail
	{
		using PrecipitationDetail::Clamp;
		using PrecipitationDetail::Smoothstep;

		/** SplitMix64: piccolo, veloce, ripetibile su ogni piattaforma (std::mt19937 + le distribuzioni non lo sono). */
		class FRandom
		{
		public:
			explicit FRandom(uint64_t InSeed) : State(InSeed) {}
			uint64_t Next()
			{
				uint64_t Z = (State += 0x9E3779B97F4A7C15ull);
				Z = (Z ^ (Z >> 30)) * 0xBF58476D1CE4E5B9ull;
				Z = (Z ^ (Z >> 27)) * 0x94D049BB133111EBull;
				return Z ^ (Z >> 31);
			}
			/** In [0, 1). */
			double Uniform() { return static_cast<double>(Next() >> 11) * (1.0 / 9007199254740992.0); }
			/** In [Low, High). */
			double Range(double Low, double High) { return Low + (High - Low) * Uniform(); }

		private:
			uint64_t State;
		};

		/** Quanti eventi in un intervallo con Lambda attesi (Knuth: va bene fino a qualche centinaio). */
		inline int32_t Poisson(FRandom& Random, double Lambda)
		{
			if (!(Lambda > 0.0)) { return 0; }
			const double Limit = std::exp(-std::min(Lambda, 600.0));
			int32_t Count = 0;
			double Product = Random.Uniform();
			while (Product > Limit && Count < 10000)
			{
				++Count;
				Product *= Random.Uniform();
			}
			return Count;
		}

		/**
		 * Quanto un punto e' "nucleo di temporale", 0..1. Nei rovesci (copertura
		 * sotto 1) i nuclei sono la parte alta della mappa. In una pioggia
		 * estesa (un fronte) ci sono solo pochi temporali "immersi" nei punti
		 * piu' intensi, e deboli: un decimo. (La mappa di un fronte arriva al
		 * massimo in un punto su sei: senza quel decimo, un fronte fulminava
		 * piu' dei temporali.)
		 */
		inline double ConvectiveWeight(const FPrecipitationField& Field, double LatDeg, double LonDeg)
		{
			const double Level = Field.Intensity(LatDeg, LonDeg);
			if (Level <= 0.0) { return 0.0; }
			return Field.GetSettings().Coverage >= 0.999 ? 0.1 * Smoothstep(0.97, 1.0, Level) : Smoothstep(0.55, 0.9, Level);
		}
	}

	// =========================================================================
	//  1. Dove e quando
	// =========================================================================

	class FLightningScheduler
	{
	public:
		void SetSettings(const FLightningSettings& InSettings)
		{
			Settings = InSettings;
			Settings.Activity = LightningDetail::Clamp(Settings.Activity, 0.0, 5.0);
			Settings.CloudToGroundFraction = LightningDetail::Clamp(Settings.CloudToGroundFraction, 0.0, 1.0);
			Settings.RadiusM = LightningDetail::Clamp(Settings.RadiusM, 1000.0, 100000.0);
		}
		const FLightningSettings& GetSettings() const { return Settings; }
		double GetTime() const { return Seconds; }

		/** Fulmina? Serve l'attivita' e che piova da qualche parte. */
		bool IsActive(const FPrecipitationField& Field) const
		{
			return Settings.Activity > 0.0 && Field.GetSettings().RateMmH > 0.0 && Field.GetSettings().Coverage > 0.0;
		}

		/**
		 * Avanza di DeltaSeconds e mette in OutNew i fulmini nati, nel cerchio
		 * attorno al centro (la camera).
		 *
		 * PERCHE' "DIRADATO". Un processo di Poisson con una densita' che cambia
		 * da punto a punto si ottiene cosi': si tirano punti a caso come se
		 * tutto il cerchio fosse un nucleo (la densita' massima), e ognuno si
		 * tiene con la probabilita' "quanto e' nucleo" del suo posto. Il
		 * risultato e' esattamente Poisson con la densita' vera, senza dover
		 * integrare la mappa.
		 */
		void Advance(double DeltaSeconds, double CentreLatDeg, double CentreLonDeg, const FPrecipitationField& Field,
		             std::vector<FLightningStrike>& OutNew)
		{
			if (!(DeltaSeconds > 0.0)) { return; }
			const double Start = Seconds;
			Seconds += DeltaSeconds;
			if (!IsActive(Field)) { return; }

			LightningDetail::FRandom Random(Mix(Counter++));
			const double AreaKm2 = 3.141592653589793 * Settings.RadiusM * Settings.RadiusM / 1.0e6;
			const double Expected = Settings.Activity * CoreFlashDensityPerKm2PerMinute / 60.0 * AreaKm2 * DeltaSeconds;
			const int32_t Candidates = LightningDetail::Poisson(Random, Expected);
			const double MetresNorth = 111132.0;
			const double MetresEast = 111320.0 * std::cos(CentreLatDeg * WindDegToRad);
			for (int32_t Index = 0; Index < Candidates; ++Index)
			{
				// Uniforme nel cerchio: il raggio va come la radice.
				const double Radius = Settings.RadiusM * std::sqrt(Random.Uniform());
				const double Angle = 2.0 * 3.141592653589793 * Random.Uniform();
				const double LatDeg = CentreLatDeg + Radius * std::cos(Angle) / MetresNorth;
				const double LonDeg = CentreLonDeg + Radius * std::sin(Angle) / MetresEast;
				if (Random.Uniform() >= LightningDetail::ConvectiveWeight(Field, LatDeg, LonDeg)) { continue; }

				const ELightningKind Kind = Random.Uniform() < Settings.CloudToGroundFraction
					? ELightningKind::CloudToGround : ELightningKind::IntraCloud;
				OutNew.push_back(Make(Kind, LatDeg, LonDeg, Start + DeltaSeconds * Random.Uniform(), Random));
			}
		}

		/** Un fulmine adesso, in un punto: per i comandi e i test. */
		FLightningStrike Strike(ELightningKind Kind, double LatDeg, double LonDeg)
		{
			LightningDetail::FRandom Random(Mix(Counter++) ^ 0xA5A5A5A5ull);
			return Make(Kind, LatDeg, LonDeg, Seconds, Random);
		}

	private:
		uint64_t Mix(uint64_t Value) const
		{
			return (static_cast<uint64_t>(Settings.Seed) << 32) ^ (Value * 0x9E3779B97F4A7C15ull);
		}

		/**
		 * I colpi di ritorno: un nube-suolo ne ha in media 3 (fino a 8), a
		 * 30-90 ms l'uno dall'altro, i successivi un po' piu' deboli del primo.
		 * Un lampo nella nube sfarfalla anche lui, con meno colpi.
		 */
		static FLightningStrike Make(ELightningKind Kind, double LatDeg, double LonDeg, double Time,
		                             LightningDetail::FRandom& Random)
		{
			FLightningStrike Made;
			Made.Kind = Kind;
			Made.LatDeg = LatDeg;
			Made.LonDeg = LonDeg;
			Made.TimeSeconds = Time;
			const double Mean = (Kind == ELightningKind::CloudToGround) ? 2.5 : 1.5;
			Made.Strokes = std::min(MaxStrokes, 1 + static_cast<int32_t>(-std::log(1.0 - Random.Uniform()) * Mean));
			Made.StrokeTimes[0] = 0.0;
			Made.StrokePeaks[0] = 1.0;
			for (int32_t Stroke = 1; Stroke < Made.Strokes; ++Stroke)
			{
				Made.StrokeTimes[Stroke] = Made.StrokeTimes[Stroke - 1] + Random.Range(0.03, 0.09);
				Made.StrokePeaks[Stroke] = Random.Range(0.45, 0.85);
			}
			Made.Seed = static_cast<uint32_t>(Random.Next() >> 32);
			return Made;
		}

		FLightningSettings Settings;
		double Seconds = 0.0;
		uint64_t Counter = 0;
	};

	// =========================================================================
	//  2. Il lampo
	// =========================================================================

	/** Quanto dura la luce di un fulmine, dall'inizio (s). */
	inline double FlashDuration(const FLightningStrike& Strike)
	{
		return Strike.StrokeTimes[std::max(0, Strike.Strokes - 1)] + 0.25;
	}

	/**
	 * La luce del fulmine dopo SecondsSinceStart: ogni colpo un picco che si
	 * spegne in ~25 ms (un frame o due: l'occhio lo vede come uno scatto),
	 * fra un colpo e l'altro la "corrente continua", debole. Il primo colpo
	 * vale 1.
	 */
	inline double FlashBrightness(const FLightningStrike& Strike, double SecondsSinceStart)
	{
		if (SecondsSinceStart < 0.0 || SecondsSinceStart > FlashDuration(Strike)) { return 0.0; }
		double Light = 0.0;
		for (int32_t Stroke = 0; Stroke < Strike.Strokes; ++Stroke)
		{
			const double Since = SecondsSinceStart - Strike.StrokeTimes[Stroke];
			if (Since >= 0.0) { Light += Strike.StrokePeaks[Stroke] * std::exp(-Since / 0.025); }
		}
		const double Last = Strike.StrokeTimes[std::max(0, Strike.Strokes - 1)];
		const double Glow = SecondsSinceStart <= Last ? 0.12 : 0.12 * std::exp(-(SecondsSinceStart - Last) / 0.05);
		return Light + Glow;
	}

	// =========================================================================
	//  3. La forma
	// =========================================================================

	/** Un pezzo di canale: da A a B (m, nord-est-su), e quanto e' luminoso (1 = il canale principale). */
	struct FBoltSegment
	{
		double A[3] = { 0.0, 0.0, 0.0 };
		double B[3] = { 0.0, 0.0, 0.0 };
		double Weight = 1.0;
	};

	namespace LightningDetail
	{
		/**
		 * Spostamento del punto medio: il segmento si spezza a meta' e il punto
		 * medio si sposta di lato (un quarto della lunghezza al massimo), poi si
		 * ripete sulle due meta'. Ogni livello aggiunge zig-zag piu' piccoli.
		 */
		inline void Subdivide(const double A[3], const double B[3], int32_t Depth, double Weight, FRandom& Random,
		                      std::vector<FBoltSegment>& Out)
		{
			if (Depth <= 0)
			{
				FBoltSegment Segment;
				for (int32_t Axis = 0; Axis < 3; ++Axis) { Segment.A[Axis] = A[Axis]; Segment.B[Axis] = B[Axis]; }
				Segment.Weight = Weight;
				Out.push_back(Segment);
				return;
			}
			const double Length = std::sqrt((B[0] - A[0]) * (B[0] - A[0]) + (B[1] - A[1]) * (B[1] - A[1]) + (B[2] - A[2]) * (B[2] - A[2]));
			double Middle[3] = {
				0.5 * (A[0] + B[0]) + Random.Range(-0.25, 0.25) * Length,
				0.5 * (A[1] + B[1]) + Random.Range(-0.25, 0.25) * Length,
				0.5 * (A[2] + B[2]) + Random.Range(-0.08, 0.08) * Length };
			Middle[2] = std::max(Middle[2], std::min(A[2], B[2]));    // mai sotto il pezzo piu' basso: niente canali sottoterra
			Subdivide(A, Middle, Depth - 1, Weight, Random, Out);
			Subdivide(Middle, B, Depth - 1, Weight, Random, Out);
		}
	}

	/** Quanti pezzi ha il canale principale (2^7). */
	inline constexpr int32_t MainChannelSegments = 128;

	/**
	 * Il canale di un nube-suolo alto HeightM: dalla nube (in cima, un po'
	 * spostata di lato) al punto colpito (l'origine). Prima i 128 pezzi del
	 * canale principale, dall'alto in basso; poi i rami, piu' deboli, che si
	 * fermano a mezz'aria. Nell'ultimo 15% in alto la luce sfuma: li' il
	 * canale entra nella nube.
	 */
	inline void BuildLightningChannel(double HeightM, uint32_t Seed, std::vector<FBoltSegment>& Out)
	{
		using namespace LightningDetail;
		Out.clear();
		FRandom Random(0x5EEDull ^ (static_cast<uint64_t>(Seed) << 16));
		const double Top[3] = { Random.Range(-0.2, 0.2) * HeightM, Random.Range(-0.2, 0.2) * HeightM, HeightM };
		const double Ground[3] = { 0.0, 0.0, 0.0 };
		Subdivide(Top, Ground, 7, 1.0, Random, Out);
		// Il punto colpito esattamente a terra (l'ultimo pezzo puo' essersi alzato di poco).
		Out.back().B[0] = Out.back().B[1] = Out.back().B[2] = 0.0;

		// I rami: partono dal canale, scendono di lato e si fermano a mezz'aria.
		const size_t Main = Out.size();
		int32_t Branches = 0;
		for (size_t Index = 4; Index + 8 < Main && Branches < 9; Index += 4)
		{
			if (Random.Uniform() > 0.35) { continue; }
			const FBoltSegment From = Out[Index];
			const double StartHeight = From.A[2];
			if (StartHeight < 0.25 * HeightM) { continue; }
			const double Length = Random.Range(0.15, 0.45) * StartHeight;
			const double Heading = Random.Range(0.0, 2.0 * 3.141592653589793);
			const double End[3] = { From.A[0] + 0.6 * Length * std::cos(Heading),
			                        From.A[1] + 0.6 * Length * std::sin(Heading),
			                        From.A[2] - 0.8 * Length };
			Subdivide(From.A, End, 5, 0.35, Random, Out);
			++Branches;
		}

		// La cima entra nella nube: sfuma nell'ultimo 15%.
		for (FBoltSegment& Segment : Out)
		{
			const double Middle = 0.5 * (Segment.A[2] + Segment.B[2]);
			Segment.Weight *= 1.0 - Smoothstep(0.85 * HeightM, HeightM, Middle);
		}
	}

	// =========================================================================
	//  4. Il tuono
	// =========================================================================

	struct FThunder
	{
		/** Mono, in [-1, 1], dal primo suono che arriva. */
		std::vector<float> Samples;
		int32_t SampleRate = 22050;
		/** Quanto dopo il lampo arriva il primo suono (s): la distanza diviso la velocita' del suono. */
		double DelaySeconds = 0.0;
		/** Quanto forte, 0..1: cala con la distanza (6 dB ogni volta che raddoppia). */
		double Loudness = 0.0;
		/** La distanza del pezzo di canale piu' vicino (m). */
		double NearestM = 0.0;
	};

	/**
	 * Il tuono di un canale (BuildLightningChannel) per chi ascolta in
	 * Listener (m, nello stesso sistema del canale). False se e' troppo
	 * lontano per sentirlo.
	 *
	 *  - ogni pezzo di canale e' un botto: rumore filtrato, che arriva dopo
	 *    distanza / velocita' del suono, con un'ampiezza che cala con la
	 *    distanza;
	 *  - l'aria assorbe gli acuti: il filtro si chiude con la distanza
	 *    (4,7 kHz a 100 m, 2,3 kHz a 1 km, 120 Hz a 10 km);
	 *  - sotto, un brontolio basso che si spegne in qualche secondo: le eco
	 *    del suolo e delle nubi.
	 */
	inline bool SynthesizeThunder(const std::vector<FBoltSegment>& Channel, const double Listener[3], double TemperatureC,
	                              uint32_t Seed, int32_t SampleRate, FThunder& Out)
	{
		using namespace LightningDetail;
		Out = FThunder{};
		Out.SampleRate = SampleRate;
		if (Channel.empty() || SampleRate < 4000) { return false; }

		const double Sound = SpeedOfSound(TemperatureC);
		std::vector<double> Distance(Channel.size()), Length(Channel.size());
		double Nearest = 1.0e30, Farthest = 0.0;
		for (size_t Index = 0; Index < Channel.size(); ++Index)
		{
			const FBoltSegment& Segment = Channel[Index];
			double Middle[3], Span = 0.0, Away = 0.0;
			for (int32_t Axis = 0; Axis < 3; ++Axis)
			{
				Middle[Axis] = 0.5 * (Segment.A[Axis] + Segment.B[Axis]);
				Span += (Segment.B[Axis] - Segment.A[Axis]) * (Segment.B[Axis] - Segment.A[Axis]);
				Away += (Middle[Axis] - Listener[Axis]) * (Middle[Axis] - Listener[Axis]);
			}
			Distance[Index] = std::sqrt(Away);
			Length[Index] = std::sqrt(Span);
			Nearest = std::min(Nearest, Distance[Index]);
			Farthest = std::max(Farthest, Distance[Index]);
		}
		if (Nearest > ThunderAudibleM) { return false; }

		Out.NearestM = Nearest;
		Out.DelaySeconds = Nearest / Sound;
		Out.Loudness = Clamp(1000.0 / std::max(Nearest, 1.0), 0.06, 1.0);
		const double Tail = 1.5 + 2.5 * std::min(1.0, Nearest / 10000.0);
		const double Total = (Farthest - Nearest) / Sound + Tail + 0.3;
		std::vector<double> Mixed(static_cast<size_t>(Total * SampleRate) + 1, 0.0);
		FRandom Random(0x7400ull ^ (static_cast<uint64_t>(Seed) << 20));

		// I botti dei pezzi di canale.
		for (size_t Index = 0; Index < Channel.size(); ++Index)
		{
			const double Away = std::max(Distance[Index], 30.0);
			const double Amplitude = Channel[Index].Weight * Length[Index] / Away;
			if (Amplitude <= 0.0) { continue; }
			const double Cutoff = 120.0 + 5000.0 * std::exp(-Away / 1200.0);
			const double Alpha = 1.0 - std::exp(-2.0 * 3.141592653589793 * Cutoff / SampleRate);
			const double Duration = 0.025 + 0.06 * std::min(1.0, Away / 5000.0);
			const size_t First = static_cast<size_t>((Distance[Index] - Nearest) / Sound * SampleRate);
			const size_t Count = static_cast<size_t>(Duration * SampleRate);
			double Filtered = 0.0;
			for (size_t Sample = 0; Sample < Count && First + Sample < Mixed.size(); ++Sample)
			{
				const double Time = static_cast<double>(Sample) / SampleRate;
				const double Envelope = std::min(1.0, Time / 0.002) * std::exp(-Time / (0.3 * Duration));
				Filtered += Alpha * (Random.Range(-1.0, 1.0) - Filtered);
				Mixed[First + Sample] += Amplitude * Envelope * Filtered;
			}
		}

		// Il brontolio: rumore molto basso (due poli a ~90 Hz) che si spegne piano.
		double Peak = 0.0;
		for (double Value : Mixed) { Peak = std::max(Peak, std::abs(Value)); }
		if (Peak <= 0.0) { return false; }
		const double RumbleAlpha = 1.0 - std::exp(-2.0 * 3.141592653589793 * 90.0 / SampleRate);
		const double RumbleDecay = 0.8 + 1.2 * std::min(1.0, Nearest / 10000.0);
		double Low1 = 0.0, Low2 = 0.0;
		for (size_t Sample = 0; Sample < Mixed.size(); ++Sample)
		{
			const double Time = static_cast<double>(Sample) / SampleRate;
			Low1 += RumbleAlpha * (Random.Range(-1.0, 1.0) - Low1);
			Low2 += RumbleAlpha * (Low1 - Low2);
			Mixed[Sample] += 6.0 * Peak * Low2 * std::min(1.0, Time / 0.05) * std::exp(-Time / RumbleDecay);
		}

		// In [-1, 1], con un margine.
		Peak = 0.0;
		for (double Value : Mixed) { Peak = std::max(Peak, std::abs(Value)); }
		Out.Samples.resize(Mixed.size());
		for (size_t Sample = 0; Sample < Mixed.size(); ++Sample)
		{
			Out.Samples[Sample] = static_cast<float>(0.95 * Mixed[Sample] / Peak);
		}
		return true;
	}
}

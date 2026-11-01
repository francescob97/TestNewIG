// =============================================================================
//  DeadReckoning.h -- Dove sta un'entita' FRA due aggiornamenti, e come ci si
//  rientra quando arriva il successivo. STRATO: C++ PURO.
//
//  Gli aggiornamenti arrivano a 1-30 Hz, il motore disegna a 60-120: fra uno e
//  l'altro qualcuno deve inventare (docs/fase7-design.md, sezione 6).
//
//  UNA estrapolazione, parametrica: l'algoritmo di DIS (EDeadReckoning) e' un
//  suggerimento che sceglie i termini:
//
//                    orientamento     posizione              velocita' negli assi
//     1 fermo        fermo            ferma                  -
//     2 FPW  5 FVW   fermo            + v t  (+ a t²/2)      del mondo (ECEF)
//     3 RPW  4 RVW   ruota con w      + v t  (+ a t²/2)      del mondo
//     6 FPB  9 FVB   fermo            come 2 e 5             del corpo
//     7 RPB  8 RVB   ruota con w      + la velocita' che     del corpo: gira con
//                                      gira con il corpo      l'entita'
//
//  La differenza fra W e B si vede solo quando l'entita' ruota: un aereo in
//  virata con la velocita' "del mondo" prosegue dritto per la tangente, con
//  quella "del corpo" continua a girare. 0 (altro): tutti i termini, nel mondo.
//
//  Poi il RIENTRO (FEntityTrack): l'aggiornamento nuovo quasi mai coincide con
//  quello che avevamo inventato. Saltarci sopra e' uno scatto; ignorarlo e'
//  deriva. Si tiene l'errore (dove l'entita' e' DISEGNATA meno dove DOVREBBE
//  essere) e lo si porta a zero in BlendSeconds, con una curva morbida.
//
//  E il TEMPO: l'orologio di DIS che si riazzera ogni ora (FHourUnwrapper) e
//  quello della simulazione che non e' il nostro (FClockSync).
// =============================================================================
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "Entities/EntityState.h"

namespace GeoWorld::Entities
{
	// =========================================================================
	//  La rotazione
	// =========================================================================

	inline FEcef Cross(const FEcef& A, const FEcef& B)
	{
		return FEcef{ A.Y * B.Z - A.Z * B.Y, A.Z * B.X - A.X * B.Z, A.X * B.Y - A.Y * B.X };
	}

	/** La rotazione di un vettore di rotazione (asse * angolo, radianti): exp(v/2). */
	inline FQuatD QuatFromRotationVector(const FEcef& V)
	{
		const double Angle = V.Length();
		if (Angle < 1e-12)
		{
			// Al primo ordine: (1, v/2), poi normalizzato.
			return FQuatD{ 1.0, 0.5 * V.X, 0.5 * V.Y, 0.5 * V.Z }.Normalized();
		}
		return FQuatD::FromAxisAngle(V, Angle);
	}

	/**
	 * L'orientamento dopo Seconds con la velocita' angolare NEGLI ASSI DEL
	 * CORPO: q(t) = q0 * exp(w t / 2). La rotazione del corpo si compone a
	 * DESTRA (prima il corpo gira su se stesso, poi lo si porta nel mondo).
	 * Esatta per w costante.
	 */
	inline FQuatD RotateByBodyRates(const FQuatD& Orientation, const FEcef& BodyRates, double Seconds)
	{
		return (Orientation * QuatFromRotationVector(BodyRates * Seconds)).Normalized();
	}

	inline bool RotatesOrientation(EDeadReckoning Algorithm)
	{
		switch (Algorithm)
		{
		case EDeadReckoning::RPW: case EDeadReckoning::RVW:
		case EDeadReckoning::RPB: case EDeadReckoning::RVB:
		case EDeadReckoning::Other:
			return true;
		default:
			return false;
		}
	}

	inline bool UsesAcceleration(EDeadReckoning Algorithm)
	{
		switch (Algorithm)
		{
		case EDeadReckoning::RVW: case EDeadReckoning::FVW:
		case EDeadReckoning::RVB: case EDeadReckoning::FVB:
		case EDeadReckoning::Other:
			return true;
		default:
			return false;
		}
	}

	inline bool UsesBodyAxes(EDeadReckoning Algorithm)
	{
		return Algorithm == EDeadReckoning::FPB || Algorithm == EDeadReckoning::RPB
			|| Algorithm == EDeadReckoning::RVB || Algorithm == EDeadReckoning::FVB;
	}

	// =========================================================================
	//  L'estrapolazione
	// =========================================================================

	/**
	 * Lo stato Seconds dopo From.Time, secondo l'algoritmo. Seconds puo' essere
	 * negativo (un attimo prima dell'aggiornamento). L'accelerazione angolare
	 * non e' usata: DIS non la trasmette, e su un secondo pesa poco.
	 *
	 * Negli assi del corpo, quando l'entita' ruota, la velocita' gira con lei:
	 * la posizione e' l'integrale di R(s) (vb + ab s) ds, che si fa a passi di
	 * al piu' 0,05 radianti con Simpson (l'errore va con la quarta potenza
	 * del passo: su una virata intera, millimetri).
	 */
	inline FEntityState Extrapolate(const FEntityState& From, EDeadReckoning Algorithm, double Seconds)
	{
		FEntityState Out = From;
		Out.Time = From.Time + Seconds;
		if (Algorithm == EDeadReckoning::Static || Seconds == 0.0)
		{
			if (Algorithm == EDeadReckoning::Static) { Out.Velocity = FEcef{}; Out.Acceleration = FEcef{}; }
			return Out;
		}

		const bool bRotates = RotatesOrientation(Algorithm);
		const bool bAccel = UsesAcceleration(Algorithm);
		const FEcef Accel = bAccel ? From.Acceleration : FEcef{};
		if (bRotates)
		{
			Out.Orientation = RotateByBodyRates(From.Orientation, From.AngularVelocity, Seconds);
		}

		if (!UsesBodyAxes(Algorithm) || !bRotates || From.AngularVelocity.Length() < 1e-9)
		{
			// Nel mondo (o un corpo che non gira: e' la stessa cosa).
			Out.Position = From.Position + From.Velocity * Seconds + Accel * (0.5 * Seconds * Seconds);
			Out.Velocity = From.Velocity + Accel * Seconds;
			return Out;
		}

		// Negli assi del corpo: velocita' e accelerazione riportate nel corpo
		// all'istante dell'aggiornamento, poi girano con lui. Attenzione
		// all'accelerazione: quella canonica e' nel MONDO e contiene gia' la
		// centripeta della virata (w x v), che la rotazione del corpo produce
		// da se'. Nel corpo resta solo la variazione della velocita' vista dal
		// corpo: ab = R^-1 a - w x vb (il teorema del trasporto). In una
		// virata stabile e' zero, e la traiettoria e' un cerchio esatto.
		// (L'adattatore DIS fara' il contrario: a = R (ab + w x vb).)
		const FQuatD Inverse = From.Orientation.Conjugate();
		const FEcef BodyVelocity = Inverse.Rotate(From.Velocity);
		const FEcef BodyAccel = bAccel ? Inverse.Rotate(Accel) - Cross(From.AngularVelocity, BodyVelocity) : FEcef{};
		const auto WorldVelocityAt = [&](double S)
		{
			const FQuatD Q = RotateByBodyRates(From.Orientation, From.AngularVelocity, S);
			return Q.Rotate(BodyVelocity + BodyAccel * S);
		};

		const double Turn = From.AngularVelocity.Length() * std::fabs(Seconds);
		const int32_t Steps = std::clamp(static_cast<int32_t>(std::ceil(Turn / 0.05)), 1, 4096);
		const double H = Seconds / Steps;
		FEcef Sum{};
		for (int32_t Step = 0; Step < Steps; ++Step)
		{
			const double S0 = Step * H;
			Sum = Sum + (WorldVelocityAt(S0) + WorldVelocityAt(S0 + 0.5 * H) * 4.0 + WorldVelocityAt(S0 + H)) * (H / 6.0);
		}
		Out.Position = From.Position + Sum;
		Out.Velocity = WorldVelocityAt(Seconds);
		const FEcef BodyVelocityNow = BodyVelocity + BodyAccel * Seconds;
		Out.Acceleration = Out.Orientation.Rotate(BodyAccel + Cross(From.AngularVelocity, BodyVelocityNow));
		return Out;
	}

	// =========================================================================
	//  Il rientro
	// =========================================================================

	/** La curva del rientro: parte e arriva piatta (3u² - 2u³), niente strappi. */
	inline double SmoothStep(double U)
	{
		const double T = std::clamp(U, 0.0, 1.0);
		return T * T * (3.0 - 2.0 * T);
	}

	struct FTrackSettings
	{
		/** Quanto dura il rientro, secondi (geo.Entities.Blend). 0: si salta subito. */
		double BlendSeconds = 0.4;
		/** Oltre questo errore si salta (un teletrasporto, un riposizionamento): metri. */
		double SnapMetres = 100.0;
		/** Oltre questo errore di assetto si salta: radianti (~60 gradi). */
		double SnapRadians = 1.05;
		/** Oltre questo tempo dall'ultimo aggiornamento non si inventa piu': ci si ferma. */
		double MaxExtrapolationSeconds = 5.0;
	};

	/** Dove disegnare un'entita' adesso. */
	struct FDrawnPose
	{
		FEcef Position;
		FQuatD Orientation;
		FEcef Velocity;
		/** Dove dice l'ultimo aggiornamento (senza il rientro): la differenza e' l'errore che si sta smaltendo. */
		FEcef TruthPosition;
		/** Quanto manca alla fine del rientro (0..1, 0: niente rientro in corso). */
		double BlendRemaining = 0.0;
		/** True quando si e' smesso di inventare (aggiornamenti fermi da troppo). */
		bool bStale = false;
	};

	/**
	 * Una traccia: l'ultimo stato ricevuto, il suo algoritmo, e l'errore da
	 * smaltire. L'errore e' nel MONDO (ECEF): un vettore di posizione e una
	 * rotazione che, applicata a sinistra, porta l'assetto vero in quello
	 * disegnato. Smaltirlo e' accorciare il vettore e la rotazione insieme.
	 */
	class FEntityTrack
	{
	public:
		bool HasState() const { return bHasState; }
		const FEntityState& Last() const { return LastState; }
		EDeadReckoning Algorithm() const { return LastAlgorithm; }
		/** L'ultimo salto (la distanza fra disegnato e vero quando e' arrivato l'aggiornamento), metri. */
		double LastCorrectionMetres() const { return LastCorrection; }
		uint32_t Snaps() const { return SnapCount; }

		/** Un aggiornamento nuovo. Now: il tempo di simulazione a cui lo si disegna. */
		void Update(const FEntityState& State, EDeadReckoning Algo, double Now, const FTrackSettings& Settings)
		{
			if (!bHasState)
			{
				LastState = State;
				LastAlgorithm = Algo;
				bHasState = true;
				ErrorPosition = FEcef{};
				ErrorRotation = FQuatD{};
				bBlending = false;
				return;
			}
			// Dove era disegnata adesso, e dove dice il nuovo stato.
			const FDrawnPose Drawn = Evaluate(Now, Settings);
			const FEntityState Truth = Extrapolate(State, Algo, ClampAhead(Now - State.Time, Settings));
			const FEcef Gap = Drawn.Position - Truth.Position;
			const FQuatD Turn = (Drawn.Orientation * Truth.Orientation.Conjugate()).Normalized();
			LastCorrection = Gap.Length();

			LastState = State;
			LastAlgorithm = Algo;
			BlendStart = Now;
			if (Settings.BlendSeconds <= 0.0 || LastCorrection > Settings.SnapMetres || Turn.AngleRad() > Settings.SnapRadians)
			{
				if (LastCorrection > Settings.SnapMetres || Turn.AngleRad() > Settings.SnapRadians) { ++SnapCount; }
				ErrorPosition = FEcef{};
				ErrorRotation = FQuatD{};
				bBlending = false;
				return;
			}
			ErrorPosition = Gap;
			ErrorRotation = Turn;
			bBlending = true;
		}

		/** Dove disegnarla al tempo di simulazione Now. */
		FDrawnPose Evaluate(double Now, const FTrackSettings& Settings) const
		{
			FDrawnPose Out;
			if (!bHasState) { return Out; }
			const double Ahead = Now - LastState.Time;
			const FEntityState Truth = Extrapolate(LastState, LastAlgorithm, ClampAhead(Ahead, Settings));
			Out.bStale = Ahead > Settings.MaxExtrapolationSeconds;

			const double Remaining = bBlending && Settings.BlendSeconds > 0.0
				? 1.0 - SmoothStep((Now - BlendStart) / Settings.BlendSeconds)
				: 0.0;
			Out.BlendRemaining = Remaining;
			Out.TruthPosition = Truth.Position;
			Out.Position = Truth.Position + ErrorPosition * Remaining;
			Out.Orientation = Remaining > 0.0
				? (FQuatD::Slerp(FQuatD{}, ErrorRotation, Remaining) * Truth.Orientation).Normalized()
				: Truth.Orientation;
			Out.Velocity = Out.bStale ? FEcef{} : Truth.Velocity;
			return Out;
		}

	private:
		static double ClampAhead(double Ahead, const FTrackSettings& Settings)
		{
			// Indietro di poco si', di molto no (un pacchetto dal passato).
			return std::clamp(Ahead, -1.0, Settings.MaxExtrapolationSeconds);
		}

		FEntityState LastState;
		EDeadReckoning LastAlgorithm = EDeadReckoning::Other;
		bool bHasState = false;
		FEcef ErrorPosition;
		FQuatD ErrorRotation;
		bool bBlending = false;
		double BlendStart = 0.0;
		double LastCorrection = 0.0;
		uint32_t SnapCount = 0;
	};

	// =========================================================================
	//  Il tempo
	// =========================================================================

	/** Un'unita' del timestamp di DIS: un'ora divisa in 2^31 parti (~1,68 microsecondi). */
	inline constexpr double DisTimeUnitSeconds = 3600.0 / 2147483648.0;

	/** I secondi DENTRO l'ora di un timestamp DIS (i 31 bit alti). */
	inline double DisTimestampSeconds(uint32_t Raw) { return static_cast<double>(Raw >> 1) * DisTimeUnitSeconds; }

	/** Il bit basso: 1 assoluto (l'ora UTC vera), 0 relativo (un orologio del simulatore). */
	inline bool DisTimestampIsAbsolute(uint32_t Raw) { return (Raw & 1u) != 0; }

	/**
	 * Il timestamp di DIS riparte da zero ogni ora. Questo lo rende monotòno:
	 * conta le ore. Un valore che torna indietro di piu' di mezz'ora e' l'ora
	 * nuova; uno che va avanti di piu' di mezz'ora e' un pacchetto in ritardo
	 * dell'ora vecchia (lo si data nell'ora vecchia, senza toccare il conto).
	 * Il punto in cui si gestisce il giro e' QUESTO, e uno solo (sezione 5).
	 */
	class FHourUnwrapper
	{
	public:
		double Unwrap(double SecondsInHour)
		{
			if (!bStarted)
			{
				bStarted = true;
				Last = SecondsInHour;
				return SecondsInHour;
			}
			if (SecondsInHour < Last - 1800.0)
			{
				++Hours;
				Last = SecondsInHour;
				return static_cast<double>(Hours) * 3600.0 + SecondsInHour;
			}
			if (SecondsInHour > Last + 1800.0)
			{
				// In ritardo dall'ora prima: non cambia il conto ne' l'ultimo valore.
				return static_cast<double>(Hours - 1) * 3600.0 + SecondsInHour;
			}
			Last = std::max(Last, SecondsInHour);
			return static_cast<double>(Hours) * 3600.0 + SecondsInHour;
		}

		double UnwrapRaw(uint32_t Raw) { return Unwrap(DisTimestampSeconds(Raw)); }
		int64_t HoursCounted() const { return Hours; }

	private:
		bool bStarted = false;
		double Last = 0.0;
		int64_t Hours = 0;
	};

	/**
	 * Il tempo della simulazione non e' il nostro. Ogni aggiornamento dice
	 * "vero al tempo T" e arriva al nostro tempo L: L - T e' lo scarto fra i
	 * due orologi PIU' il ritardo del viaggio, che e' sempre positivo. Il
	 * minimo degli scarti e' la stima migliore dello scarto vero (il pacchetto
	 * piu' veloce); lo si lascia risalire piano, per seguire un orologio che
	 * deriva, e lo si riparte da capo se cambia tutto (la simulazione
	 * riavviata, messa in pausa).
	 */
	class FClockSync
	{
	public:
		/** Un campione: il tempo di simulazione del messaggio e il nostro quando e' arrivato. */
		void Observe(double SimSeconds, double LocalSeconds)
		{
			const double Sample = LocalSeconds - SimSeconds;
			if (!bStarted || std::fabs(Sample - Offset) > ResetSeconds)
			{
				if (bStarted) { ++Resets; }
				bStarted = true;
				Offset = Sample;
				return;
			}
			if (Sample < Offset) { Offset = Sample; }
			else { Offset += (Sample - Offset) * Relax; }
		}

		bool IsSynced() const { return bStarted; }
		double SimNow(double LocalSeconds) const { return LocalSeconds - Offset; }
		double OffsetSeconds() const { return Offset; }
		uint32_t ResetCount() const { return Resets; }

		/** Quanto lo scarto risale verso un campione piu' lento, per campione (0..1). */
		double Relax = 0.01;
		/** Oltre questo salto si riparte: secondi. */
		double ResetSeconds = 2.0;

	private:
		bool bStarted = false;
		double Offset = 0.0;
		uint32_t Resets = 0;
	};
}

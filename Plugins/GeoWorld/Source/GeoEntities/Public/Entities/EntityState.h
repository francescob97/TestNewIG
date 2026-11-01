// =============================================================================
//  EntityState.h -- Lo stato di un'entita', nella sua forma canonica.
//  STRATO: C++ PURO, header-only (Fase 7).
//
//  Aerei, veicoli, navi: ogni protocollo (memoria condivisa, CIGI, DIS, HLA)
//  li descrive a modo suo, ma il contenuto fisico e' lo stesso:
//
//      chi sono + che cosa sono + dove + come orientati + come si muovono + quando
//
//  Dentro il motore esiste SOLO questa forma (docs/fase7-design.md, sezione 3):
//
//   - la POSIZIONE in ECEF, metri: l'unica autorita' dalla Fase 1. Il rebase
//     non tocca le entita', come non tocca le tile;
//   - l'ORIENTAMENTO come quaternione dal frame del CORPO all'ECEF. Il corpo
//     e' quello aeronautico: X avanti, Y a destra, Z in BASSO (FRD). Niente
//     angoli di Eulero dentro: ogni protocollo li definisce a modo suo;
//   - VELOCITA' e ACCELERAZIONE lineari in ECEF (m/s, m/s²);
//   - VELOCITA' e ACCELERAZIONE angolari negli assi del CORPO (rad/s): un
//     aereo gira attorno ai propri assi, non a quelli della Terra;
//   - il TEMPO a cui tutto questo e' vero, secondi di simulazione.
// =============================================================================
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>

#include "Geo/GeoTypes.h"

namespace GeoWorld::Entities
{
	using Core::FEcef;
	using Core::FMat3;

	// =========================================================================
	//  Il quaternione
	// =========================================================================

	/**
	 * Un quaternione di rotazione (Hamilton, w + xi + yj + zk). Ruota i vettori
	 * in modo ATTIVO: Rotate(v) = q v q*. Il prodotto A * B applica prima B,
	 * poi A (come le matrici).
	 */
	struct FQuatD
	{
		double W = 1.0, X = 0.0, Y = 0.0, Z = 0.0;

		static FQuatD Identity() { return FQuatD{}; }

		static FQuatD FromAxisAngle(const FEcef& Axis, double AngleRad)
		{
			const FEcef Unit = Axis.Normalized();
			const double S = std::sin(0.5 * AngleRad);
			return FQuatD{ std::cos(0.5 * AngleRad), Unit.X * S, Unit.Y * S, Unit.Z * S };
		}

		FQuatD operator*(const FQuatD& O) const
		{
			return FQuatD{ W * O.W - X * O.X - Y * O.Y - Z * O.Z,
			               W * O.X + X * O.W + Y * O.Z - Z * O.Y,
			               W * O.Y - X * O.Z + Y * O.W + Z * O.X,
			               W * O.Z + X * O.Y - Y * O.X + Z * O.W };
		}

		FQuatD Conjugate() const { return FQuatD{ W, -X, -Y, -Z }; }

		double Norm() const { return std::sqrt(W * W + X * X + Y * Y + Z * Z); }

		FQuatD Normalized() const
		{
			const double N = Norm();
			return N > 0.0 ? FQuatD{ W / N, X / N, Y / N, Z / N } : FQuatD{};
		}

		FEcef Rotate(const FEcef& V) const
		{
			// v' = v + 2w (q x v) + 2 q x (q x v), con q la parte vettoriale.
			const FEcef Q{ X, Y, Z };
			const FEcef T{ 2.0 * (Q.Y * V.Z - Q.Z * V.Y), 2.0 * (Q.Z * V.X - Q.X * V.Z), 2.0 * (Q.X * V.Y - Q.Y * V.X) };
			return FEcef{ V.X + W * T.X + (Q.Y * T.Z - Q.Z * T.Y),
			              V.Y + W * T.Y + (Q.Z * T.X - Q.X * T.Z),
			              V.Z + W * T.Z + (Q.X * T.Y - Q.Y * T.X) };
		}

		/** La matrice della stessa rotazione (colonne = immagini degli assi). */
		FMat3 ToMatrix() const
		{
			FMat3 R;
			R.M[0][0] = 1.0 - 2.0 * (Y * Y + Z * Z); R.M[0][1] = 2.0 * (X * Y - W * Z);       R.M[0][2] = 2.0 * (X * Z + W * Y);
			R.M[1][0] = 2.0 * (X * Y + W * Z);       R.M[1][1] = 1.0 - 2.0 * (X * X + Z * Z); R.M[1][2] = 2.0 * (Y * Z - W * X);
			R.M[2][0] = 2.0 * (X * Z - W * Y);       R.M[2][1] = 2.0 * (Y * Z + W * X);       R.M[2][2] = 1.0 - 2.0 * (X * X + Y * Y);
			return R;
		}

		/** Da una matrice di rotazione VERA (determinante +1). Il metodo di Shepperd: stabile. */
		static FQuatD FromMatrix(const FMat3& R)
		{
			const double Trace = R.M[0][0] + R.M[1][1] + R.M[2][2];
			FQuatD Q;
			if (Trace > 0.0)
			{
				const double S = 2.0 * std::sqrt(1.0 + Trace);
				Q = FQuatD{ 0.25 * S, (R.M[2][1] - R.M[1][2]) / S, (R.M[0][2] - R.M[2][0]) / S, (R.M[1][0] - R.M[0][1]) / S };
			}
			else if (R.M[0][0] > R.M[1][1] && R.M[0][0] > R.M[2][2])
			{
				const double S = 2.0 * std::sqrt(1.0 + R.M[0][0] - R.M[1][1] - R.M[2][2]);
				Q = FQuatD{ (R.M[2][1] - R.M[1][2]) / S, 0.25 * S, (R.M[0][1] + R.M[1][0]) / S, (R.M[0][2] + R.M[2][0]) / S };
			}
			else if (R.M[1][1] > R.M[2][2])
			{
				const double S = 2.0 * std::sqrt(1.0 + R.M[1][1] - R.M[0][0] - R.M[2][2]);
				Q = FQuatD{ (R.M[0][2] - R.M[2][0]) / S, (R.M[0][1] + R.M[1][0]) / S, 0.25 * S, (R.M[1][2] + R.M[2][1]) / S };
			}
			else
			{
				const double S = 2.0 * std::sqrt(1.0 + R.M[2][2] - R.M[0][0] - R.M[1][1]);
				Q = FQuatD{ (R.M[1][0] - R.M[0][1]) / S, (R.M[0][2] + R.M[2][0]) / S, (R.M[1][2] + R.M[2][1]) / S, 0.25 * S };
			}
			return Q.Normalized();
		}

		/** L'angolo della rotazione, radianti (0..pi). */
		double AngleRad() const
		{
			const double C = std::fabs(W) / std::max(Norm(), 1e-300);
			return 2.0 * std::acos(std::min(1.0, C));
		}

		/** L'interpolazione sferica: t = 0 da' A, t = 1 da' B, per la via piu' corta. */
		static FQuatD Slerp(const FQuatD& A, const FQuatD& B, double T)
		{
			double Dot = A.W * B.W + A.X * B.X + A.Y * B.Y + A.Z * B.Z;
			FQuatD End = B;
			if (Dot < 0.0) { Dot = -Dot; End = FQuatD{ -B.W, -B.X, -B.Y, -B.Z }; }
			if (Dot > 0.9995)
			{
				return FQuatD{ A.W + (End.W - A.W) * T, A.X + (End.X - A.X) * T, A.Y + (End.Y - A.Y) * T, A.Z + (End.Z - A.Z) * T }.Normalized();
			}
			const double Theta = std::acos(Dot);
			const double Sa = std::sin((1.0 - T) * Theta) / std::sin(Theta), Sb = std::sin(T * Theta) / std::sin(Theta);
			return FQuatD{ A.W * Sa + End.W * Sb, A.X * Sa + End.X * Sb, A.Y * Sa + End.Y * Sb, A.Z * Sa + End.Z * Sb }.Normalized();
		}
	};

	// =========================================================================
	//  L'identita' e il tipo
	// =========================================================================

	/**
	 * Chi e' un'entita': la SORGENTE (la memoria condivisa, un host CIGI, una
	 * rete DIS...) piu' l'identificatore nativo di quella sorgente. Due
	 * sorgenti possono usare lo stesso numero: il solo numero non basta.
	 */
	struct FEntityKey
	{
		uint32_t Source = 0;
		uint64_t Native = 0;

		bool operator==(const FEntityKey& O) const { return Source == O.Source && Native == O.Native; }
		bool operator!=(const FEntityKey& O) const { return !(*this == O); }
	};

	struct FEntityKeyHash
	{
		size_t operator()(const FEntityKey& Key) const
		{
			uint64_t H = Key.Native * 0x9E3779B97F4A7C15ull ^ (static_cast<uint64_t>(Key.Source) * 0xC2B2AE3D27D4EB4Full);
			H ^= H >> 31;
			return static_cast<size_t>(H);
		}
	};

	/**
	 * Per le TMap e i TSet di Unreal, che cercano GetTypeHash nel namespace
	 * del tipo. Qui e non nello strato Unreal: una TMap con questa chiave
	 * come membro di una classe la vuole gia' nell'header della classe.
	 */
	inline uint32_t GetTypeHash(const FEntityKey& Key)
	{
		const uint64_t H = FEntityKeyHash{}(Key);
		return static_cast<uint32_t>(H ^ (H >> 32));
	}

	/**
	 * Che cosa e' un'entita': l'enumerazione di DIS e dell'RPR-FOM, sette
	 * campi (genere, dominio, paese, categoria, sottocategoria, specifico,
	 * extra). Descrive COSA e', non quale modello disegnare: la traduzione la
	 * fa una tabella (EntityModels.h).
	 */
	struct FEntityKind
	{
		uint8_t Kind = 0;           // 1 piattaforma, 2 munizione, 3 forma di vita, ...
		uint8_t Domain = 0;         // 1 terra, 2 aria, 3 superficie (mare), 4 sottomarino, 5 spazio
		uint16_t Country = 0;       // 105 Italia, 225 Stati Uniti, ...
		uint8_t Category = 0;
		uint8_t Subcategory = 0;
		uint8_t Specific = 0;
		uint8_t Extra = 0;

		int32_t Field(int32_t Index) const
		{
			switch (Index)
			{
			case 0: return Kind;
			case 1: return Domain;
			case 2: return Country;
			case 3: return Category;
			case 4: return Subcategory;
			case 5: return Specific;
			default: return Extra;
			}
		}

		/** "1.2.105.1.0.0.0": il modo in cui la scrivono tutti. */
		std::string Text() const
		{
			char Buffer[48];
			std::snprintf(Buffer, sizeof(Buffer), "%u.%u.%u.%u.%u.%u.%u", Kind, Domain, Country, Category, Subcategory, Specific, Extra);
			return Buffer;
		}

		bool operator==(const FEntityKind& O) const
		{
			return Kind == O.Kind && Domain == O.Domain && Country == O.Country && Category == O.Category
				&& Subcategory == O.Subcategory && Specific == O.Specific && Extra == O.Extra;
		}
	};

	// =========================================================================
	//  Lo stato
	// =========================================================================

	struct FEntityState
	{
		/** ECEF, metri. */
		FEcef Position;
		/** Dal corpo (X avanti, Y destra, Z giu') all'ECEF. */
		FQuatD Orientation;
		/** ECEF, m/s e m/s². */
		FEcef Velocity;
		FEcef Acceleration;
		/** Negli assi del corpo, rad/s (p rollio, q beccheggio, r imbardata) e rad/s². */
		FEcef AngularVelocity;
		FEcef AngularAcceleration;
		/** Quando tutto questo e' vero: secondi di simulazione. */
		double Time = 0.0;
	};

	/**
	 * Come un protocollo dice di estrapolare (gli algoritmi di dead reckoning
	 * di DIS, IEEE 1278.1, con i loro numeri). F/R: orientamento fermo o che
	 * ruota; P/V: solo la velocita', o anche l'accelerazione; W/B: velocita' e
	 * accelerazione negli assi del mondo o del corpo.
	 */
	enum class EDeadReckoning : uint8_t
	{
		Other = 0, Static = 1, FPW = 2, RPW = 3, RVW = 4, FVW = 5, FPB = 6, RPB = 7, RVB = 8, FVB = 9,
	};

	inline const char* DeadReckoningName(EDeadReckoning Algorithm)
	{
		switch (Algorithm)
		{
		case EDeadReckoning::Static: return "fermo";
		case EDeadReckoning::FPW: return "FPW";
		case EDeadReckoning::RPW: return "RPW";
		case EDeadReckoning::RVW: return "RVW";
		case EDeadReckoning::FVW: return "FVW";
		case EDeadReckoning::FPB: return "FPB";
		case EDeadReckoning::RPB: return "RPB";
		case EDeadReckoning::RVB: return "RVB";
		case EDeadReckoning::FVB: return "FVB";
		default: return "altro";
		}
	}
}

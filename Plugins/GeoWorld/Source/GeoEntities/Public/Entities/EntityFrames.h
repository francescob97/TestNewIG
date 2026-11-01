// =============================================================================
//  EntityFrames.h -- Le conversioni di frame delle entita'. STRATO: C++ PURO.
//
//  Le tre trappole di docs/fase7-design.md (sezione 4), tutte qui, in un
//  posto solo. Gli adattatori dei protocolli le usano; il resto del motore
//  vede solo lo stato canonico (EntityState.h).
//
//  TRAPPOLA 1 -- NED NON E' NEU. La simulazione usa North, East, DOWN: una
//  velocita' verticale di +5 in NED SCENDE. Il terreno di GeoWorld usa
//  North, East, Up (MakeNeuBasis). NED e' destrorso, NEU sinistrorso.
//
//  TRAPPOLA 2 -- GLI ANGOLI DI DIS E QUELLI DI CIGI NON SONO GLI STESSI. Tutti
//  e due sono la sequenza 3-2-1 (imbardata, beccheggio, rollio), ma DIS la
//  misura rispetto agli assi ECEF, CIGI rispetto al NED del punto dove
//  l'entita' si trova. Un aereo che punta a nord a Roma e uno a Tokyo hanno la
//  stessa imbardata CIGI (0) e psi DIS completamente diversi.
//
//  TRAPPOLA 3 -- GLI ASSI DEL CORPO. In aeronautica X avanti, Y a destra, Z in
//  BASSO; un attore di Unreal X avanti, Y a destra, Z in ALTO. La terza
//  componente cambia segno: senza, un aereo che vira a destra vira a sinistra.
//
//  La rotazione di Eulero 3-2-1: R = Rz(imbardata) Ry(beccheggio) Rx(rollio),
//  dal corpo al riferimento. Imbardata positiva verso est (a destra),
//  beccheggio positivo a muso in su, rollio positivo con l'ala destra in giu'.
// =============================================================================
#pragma once

#include <algorithm>
#include <cmath>

#include "Entities/EntityState.h"
#include "Geo/Ellipsoid.h"

namespace GeoWorld::Entities
{
	using Core::FGeodetic;

	/** Imbardata, beccheggio, rollio, radianti (la sequenza 3-2-1). */
	struct FEuler
	{
		double Yaw = 0.0, Pitch = 0.0, Roll = 0.0;
	};

	// =========================================================================
	//  Trappola 1: NED
	// =========================================================================

	/** Da ECEF a NED nel punto: le righe sono North, East, Down. Destrorsa (det +1). */
	inline FMat3 MakeNedBasis(double LatRad, double LonRad)
	{
		const FMat3 Neu = Core::MakeNeuBasis(LatRad, LonRad);
		FMat3 Ned = Neu;
		for (int Column = 0; Column < 3; ++Column) { Ned.M[2][Column] = -Neu.M[2][Column]; }
		return Ned;
	}

	/** Un vettore NED (una velocita', un'accelerazione) in ECEF. */
	inline FEcef NedToEcef(double LatRad, double LonRad, const FEcef& Ned)
	{
		return MakeNedBasis(LatRad, LonRad).Transposed().Transform(Ned);
	}

	/** Un vettore ECEF nel NED del punto. */
	inline FEcef EcefToNed(double LatRad, double LonRad, const FEcef& Ecef)
	{
		return MakeNedBasis(LatRad, LonRad).Transform(Ecef);
	}

	// =========================================================================
	//  Eulero 3-2-1
	// =========================================================================

	/** La matrice dal corpo al riferimento: Rz(imbardata) Ry(beccheggio) Rx(rollio). */
	inline FMat3 MatrixFromEuler(const FEuler& Angles)
	{
		const double Cy = std::cos(Angles.Yaw), Sy = std::sin(Angles.Yaw);
		const double Cp = std::cos(Angles.Pitch), Sp = std::sin(Angles.Pitch);
		const double Cr = std::cos(Angles.Roll), Sr = std::sin(Angles.Roll);
		FMat3 R;
		R.M[0][0] = Cy * Cp; R.M[0][1] = Cy * Sp * Sr - Sy * Cr; R.M[0][2] = Cy * Sp * Cr + Sy * Sr;
		R.M[1][0] = Sy * Cp; R.M[1][1] = Sy * Sp * Sr + Cy * Cr; R.M[1][2] = Sy * Sp * Cr - Cy * Sr;
		R.M[2][0] = -Sp;     R.M[2][1] = Cp * Sr;                R.M[2][2] = Cp * Cr;
		return R;
	}

	/**
	 * Gli angoli di una matrice dal corpo al riferimento. A beccheggio +-90
	 * gradi (il muso dritto in su o in giu') imbardata e rollio si confondono
	 * (il gimbal lock): si tiene il rollio a zero e tutto va nell'imbardata.
	 */
	inline FEuler EulerFromMatrix(const FMat3& R)
	{
		FEuler Out;
		const double SinPitch = std::max(-1.0, std::min(1.0, -R.M[2][0]));
		Out.Pitch = std::asin(SinPitch);
		if (std::fabs(SinPitch) > 0.999999)
		{
			Out.Roll = 0.0;
			Out.Yaw = std::atan2(-R.M[0][1], R.M[1][1]);
		}
		else
		{
			Out.Yaw = std::atan2(R.M[1][0], R.M[0][0]);
			Out.Roll = std::atan2(R.M[2][1], R.M[2][2]);
		}
		return Out;
	}

	// =========================================================================
	//  Trappola 2: gli angoli di CIGI (locali) e di DIS (ECEF)
	// =========================================================================

	/** L'orientamento canonico da imbardata, beccheggio, rollio rispetto al NED del punto (CIGI, la memoria condivisa). */
	inline FQuatD OrientationFromLocalEuler(double LatRad, double LonRad, const FEuler& Angles)
	{
		return FQuatD::FromMatrix(MakeNedBasis(LatRad, LonRad).Transposed() * MatrixFromEuler(Angles));
	}

	inline FEuler LocalEulerFromOrientation(double LatRad, double LonRad, const FQuatD& Orientation)
	{
		return EulerFromMatrix(MakeNedBasis(LatRad, LonRad) * Orientation.ToMatrix());
	}

	/** L'orientamento canonico dagli angoli psi, theta, phi di DIS (rispetto agli assi ECEF). */
	inline FQuatD OrientationFromDisEuler(const FEuler& Angles)
	{
		return FQuatD::FromMatrix(MatrixFromEuler(Angles));
	}

	inline FEuler DisEulerFromOrientation(const FQuatD& Orientation)
	{
		return EulerFromMatrix(Orientation.ToMatrix());
	}

	/** Gli angoli di DIS di un'entita' a (lat, lon) con questi angoli locali: la composizione con la base del punto. */
	inline FEuler DisFromLocalEuler(double LatRad, double LonRad, const FEuler& Local)
	{
		return DisEulerFromOrientation(OrientationFromLocalEuler(LatRad, LonRad, Local));
	}

	inline FEuler LocalFromDisEuler(double LatRad, double LonRad, const FEuler& Dis)
	{
		return LocalEulerFromOrientation(LatRad, LonRad, OrientationFromDisEuler(Dis));
	}

	// =========================================================================
	//  Trappola 3: dagli assi del corpo a quelli di un attore di Unreal
	// =========================================================================

	/**
	 * Gli assi di un attore di Unreal (X avanti, Y destra, Z ALTO) nel frame
	 * NEU del punto dove sta l'entita' (le righe: avanti, destra, alto).
	 * In coordinate NEU, trattate come quelle di Unreal, e' una rotazione VERA
	 * (determinante +1): lo strato Unreal la fa diventare un FQuat e la compone
	 * con GetLocalNeuRotation del punto.
	 */
	inline FMat3 ActorAxesNeu(double LatRad, double LonRad, const FQuatD& Orientation)
	{
		const FMat3 Neu = Core::MakeNeuBasis(LatRad, LonRad);
		const FEcef Forward = Neu.Transform(Orientation.Rotate(FEcef{ 1.0, 0.0, 0.0 }));
		const FEcef Right = Neu.Transform(Orientation.Rotate(FEcef{ 0.0, 1.0, 0.0 }));
		// Z del corpo e' in basso: l'alto dell'attore e' il suo opposto.
		const FEcef Up = Neu.Transform(Orientation.Rotate(FEcef{ 0.0, 0.0, -1.0 }));
		return FMat3::FromRows(Forward, Right, Up);
	}

	/**
	 * Gli stessi assi direttamente nello spazio di UNREAL: EcefToUnreal e' la
	 * base della georeferenziazione (FGeoreference::GetEcefToUeBasis, le righe
	 * Nord, Est, Alto dell'origine, det -1). Le righe del risultato sono gli
	 * assi X, Y, Z dell'attore in coordinate Unreal: e' il layout di
	 * FMatrix(FPlane X, FPlane Y, FPlane Z, W), e ToQuat() e' lecito perche'
	 * il determinante e' +1 (la riflessione e' una sola: quella dell'ECEF).
	 */
	inline FMat3 ActorAxesInUnreal(const FMat3& EcefToUnreal, const FQuatD& Orientation)
	{
		return FMat3::FromRows(EcefToUnreal.Transform(Orientation.Rotate(FEcef{ 1.0, 0.0, 0.0 })),
			EcefToUnreal.Transform(Orientation.Rotate(FEcef{ 0.0, 1.0, 0.0 })),
			EcefToUnreal.Transform(Orientation.Rotate(FEcef{ 0.0, 0.0, -1.0 })));
	}

	/**
	 * Una velocita' angolare dagli assi del corpo (p, q, r: avanti, destra,
	 * GIU') a quelli di un attore (avanti, destra, ALTO): -p, -q, +r.
	 *
	 * Sembra strano che cambino X e Y, che negli assi sono uguali, e non Z:
	 * e' la riflessione. Una velocita' angolare e' un asse PIU' un verso di
	 * rotazione, e il verso lo decide la regola della mano destra, che in un
	 * frame sinistrorso (quello dell'attore) gira al contrario. Quindi: tutte
	 * e tre cambiano segno per la mano, e la Z lo cambia ancora per l'asse
	 * capovolto. Il risultato e' quello per cui, scritta con le componenti
	 * dell'attore, v' = w x v torna (il test lo verifica su un aereo che vira
	 * e rolla): un rollio a destra di DIS e' un rollio NEGATIVO per l'attore
	 * (docs/fase7-design.md, trappola 3).
	 */
	inline FEcef BodyRatesToActor(const FEcef& BodyRates)
	{
		return FEcef{ -BodyRates.X, -BodyRates.Y, BodyRates.Z };
	}

	// =========================================================================
	//  La quota
	// =========================================================================

	/**
	 * Da una quota sul livello del mare a una sull'ellissoide (quella di
	 * GeoWorld): si aggiunge l'ondulazione del geoide del punto, lungo la
	 * verticale. In Italia ~45 m: abbastanza per mettere un carro armato sotto
	 * terra (docs/fase7-design.md, sezione 10).
	 */
	inline FEcef RaiseAlongNormal(const FEcef& Position, double Metres)
	{
		const FGeodetic Here = Core::EcefToGeodetic(Position, Core::WGS84);
		const FEcef Up = Core::GeodeticSurfaceNormal(Here);
		return Position + Up * Metres;
	}
}

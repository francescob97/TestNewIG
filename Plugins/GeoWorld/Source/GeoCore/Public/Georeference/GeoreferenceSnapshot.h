// =============================================================================
//  GeoreferenceSnapshot.h -- Il ponte fra lo strato puro e Unreal Engine.
//
//  STRATO: UNREAL. Da qui in poi FVector, FQuat e compagnia sono ammessi.
//
//  FGeoreferenceSnapshot e' una FOTOGRAFIA immutabile della georeferenziazione:
//  origine corrente, basi precalcolate e un numero di generazione.
//
//  PERCHE' UNA FOTOGRAFIA E NON UN PUNTATORE AL SUBSYSTEM:
//  dalla Fase 3 il loader dei tile e la selezione LOD gireranno su thread pool.
//  Regola fissata adesso, non dopo: UN WORKER NON CHIAMA MAI IL SUBSYSTEM.
//  Riceve una copia di questa struct all'avvio del task (e' una manciata di
//  double, la copia costa nulla) e la usa per tutta la sua durata. Se nel
//  frattempo avviene un rebase, il Generation non corrispondera' piu' e il game
//  thread ricalcolera' la sola trasformazione finale al momento dell'attach.
//  Risultato: nessun lock nel percorso caldo, nessuno stato condiviso mutabile.
//
//  NOTA UE: questa NON e' una USTRUCT. Le USTRUCT servono per la riflessione
//  (Blueprint, serializzazione, replica) e impongono che ogni membro sia
//  riflettibile: FMat3 con il suo double[3][3] non lo e'. Siccome questa struct
//  vive solo in C++, la riflessione sarebbe costo puro senza beneficio. Per
//  l'esposizione a Blueprint c'e' FGeoCoordinate in GeoWorldTypes.h.
// =============================================================================
#pragma once

#include "CoreMinimal.h"
#include "Geo/Georeference.h"

struct GEOCORE_API FGeoreferenceSnapshot
{
	using FGeodetic   = GeoWorld::Core::FGeodetic;
	using FEcef       = GeoWorld::Core::FEcef;
	using FUnrealPos  = GeoWorld::Core::FUnrealPos;
	using FMat3       = GeoWorld::Core::FMat3;

	/** La trasformazione vera e propria (C++ puro, testata senza Unreal). */
	GeoWorld::Core::FGeoreference Georeference;

	/**
	 * Incrementa a ogni rebase. Serve ai worker thread per accorgersi che il
	 * risultato che stanno per consegnare e' stato calcolato con un'origine
	 * ormai vecchia.
	 */
	uint32 Generation = 0;

	// --- Conversioni: l'unico punto dove FUnrealPos diventa FVector -----------
	//
	// NOTA UE FONDAMENTALE: in UE5 FVector e' FVector3d, cioe' DOUBLE, per via
	// delle Large World Coordinates. In UE4 era float e un pianeta era
	// letteralmente irrappresentabile (il mondo era limitato a ~21 km).
	// Qui quindi non si perde precisione nella conversione: sono tre double
	// copiati in tre double.

	FVector EcefToUnreal(const FEcef& Position) const
	{
		const FUnrealPos P = Georeference.EcefToUnreal(Position);
		return FVector(P.X, P.Y, P.Z);
	}

	FEcef UnrealToEcef(const FVector& Position) const
	{
		return Georeference.UnrealToEcef(FUnrealPos{ Position.X, Position.Y, Position.Z });
	}

	FVector GeodeticToUnreal(const FGeodetic& Geodetic) const
	{
		const FUnrealPos P = Georeference.GeodeticToUnreal(Geodetic);
		return FVector(P.X, P.Y, P.Z);
	}

	FGeodetic UnrealToGeodetic(const FVector& Position) const
	{
		return Georeference.UnrealToGeodetic(FUnrealPos{ Position.X, Position.Y, Position.Z });
	}

	/** Ruota una direzione da spazio Unreal a ECEF (nessuna traslazione). */
	FEcef UnrealDirectionToEcef(const FVector& Direction) const
	{
		return Georeference.UnrealDirectionToEcef(
			FUnrealPos{ Direction.X, Direction.Y, Direction.Z });
	}

	/** Distanza in metri dall'origine corrente. */
	double DistanceFromOriginMeters(const FVector& Position) const
	{
		return Georeference.DistanceFromOriginMeters(
			FUnrealPos{ Position.X, Position.Y, Position.Z });
	}

	// ---------------------------------------------------------------------
	//  Orientamento "in piedi sul terreno" in un punto geodetico qualunque.
	//
	//  Restituisce la rotazione che porta gli assi dell'attore a coincidere con
	//  Nord / Est / Alto LOCALI di quel punto. A 500 km di distanza la verticale
	//  locale e' ruotata di ~4.5 gradi rispetto a quella dell'origine: usare la
	//  base dell'origine ovunque produce oggetti visibilmente storti.
	//
	//  NOTA UE sulla costruzione della matrice: il costruttore
	//  FMatrix(FPlane X, FPlane Y, FPlane Z, FPlane W) interpreta le prime tre
	//  righe come gli ASSI X, Y, Z dell'oggetto espressi in spazio mondo, e la
	//  quarta come l'origine. E' esattamente il layout che ci serve.
	//
	//  ToQuat() e' valida solo per ROTAZIONI PROPRIE (det +1). La nostra lo e':
	//  la base NEU locale in ECEF ha det -1, la matrice ECEF->Unreal ha det -1,
	//  e il loro prodotto ha det +1. La riflessione destrorso->sinistrorso
	//  avviene una volta sola, al confine della georeferenziazione; dentro lo
	//  spazio di Unreal tutto e' rotazione pura. (Verificato dal test 9 del
	//  harness standalone: se questa proprieta' si rompesse, ToQuat() non
	//  darebbe errore, restituirebbe silenziosamente spazzatura.)
	// ---------------------------------------------------------------------
	FQuat GetLocalNeuRotation(const FGeodetic& AtGeodetic) const
	{
		const FMat3 Basis = Georeference.GetLocalNeuBasisInUnreal(AtGeodetic);

		const FEcef NorthAxis = Basis.GetRow(0);   // -> asse X dell'attore
		const FEcef EastAxis  = Basis.GetRow(1);   // -> asse Y dell'attore
		const FEcef UpAxis    = Basis.GetRow(2);   // -> asse Z dell'attore

		const FMatrix RotationMatrix(
			FPlane(NorthAxis.X, NorthAxis.Y, NorthAxis.Z, 0.0),
			FPlane(EastAxis.X,  EastAxis.Y,  EastAxis.Z,  0.0),
			FPlane(UpAxis.X,    UpAxis.Y,    UpAxis.Z,    0.0),
			FPlane(0.0, 0.0, 0.0, 1.0));

		return RotationMatrix.ToQuat();
	}

	/** Posizione + orientamento locale, pronta per SetWorldTransform. */
	FTransform GetLocalNeuTransform(const FGeodetic& AtGeodetic) const
	{
		return FTransform(GetLocalNeuRotation(AtGeodetic), GeodeticToUnreal(AtGeodetic));
	}
};

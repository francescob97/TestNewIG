// =============================================================================
//  GeoEntitySource.h -- Una SORGENTE di entita'. STRATO: UNREAL.
//
//  Un adattatore che riceve: la memoria condivisa, poi CIGI, DIS, HLA. Ognuna
//  ha il suo thread (nessun socket ne' attesa sul game thread, sezione 9 di
//  docs/fase7-design.md), traduce nella forma canonica SUL SUO THREAD e
//  consegna gli FEntityUpdate in una coda MPSC: molti produttori (le
//  sorgenti), un consumatore (il game thread, UGeoEntitySubsystem). La coda e'
//  l'unico punto di contatto.
//
//  La direzione opposta (pubblicare le entita' nostre, DIS e HLA) sara' una
//  interfaccia SEPARATA, non metodi in piu' qui: ritmi, errori e motivi di
//  fallimento diversi (sezione 8).
// =============================================================================
#pragma once

#include "CoreMinimal.h"
#include "Containers/Queue.h"

#include "Entities/EntityRegistry.h"

/** La coda fra i thread delle sorgenti e il game thread. */
using FGeoEntityQueue = TQueue<GeoWorld::Entities::FEntityUpdate, EQueueMode::Mpsc>;

enum class EGeoSourceState : uint8
{
	Stopped,
	/** Accesa, ma dall'altra parte non c'e' (ancora) nessuno. */
	Waiting,
	Receiving,
	/** Dall'altra parte c'e' qualcosa che non si capisce (Message dice cosa). */
	Error,
};

struct FGeoEntitySourceStatus
{
	EGeoSourceState State = EGeoSourceState::Stopped;
	/** La frase per geo.Entities.List: dove aspetta, che errore. */
	FString Message;
	/** Fotografie o pacchetti ricevuti, entita' consegnate. */
	uint64 Messages = 0;
	uint64 Updates = 0;
	/** Letture da rifare (la memoria a meta' scrittura): molte vogliono dire un problema. */
	uint64 Retries = 0;
	/** FPlatformTime::Seconds() dell'ultimo messaggio. */
	double LastMessageSeconds = 0.0;
	/** Le entita' nell'ultimo messaggio (per una fotografia: tutte quelle vive). */
	int32 LastCount = 0;
};

/**
 * Una sorgente. Open fa partire il suo thread, Close lo ferma e aspetta che
 * sia fermo (dopo, la sorgente non tocca piu' la coda).
 */
class GEOENTITIES_API IGeoEntitySource
{
public:
	virtual ~IGeoEntitySource() = default;

	/** Il numero nella chiave delle entita' (FEntityKey::Source): distingue le sorgenti. */
	virtual uint32 GetSourceId() const = 0;
	/** "memoria condivisa 'GeoWorldEntities'", "CIGI :8004"... */
	virtual FString GetName() const = 0;
	/** Da dove riceve: il nome della memoria, la porta. Due sorgenti uguali non si aprono due volte. */
	virtual FString GetAddress() const = 0;

	virtual bool Open(const TSharedRef<FGeoEntityQueue, ESPMode::ThreadSafe>& Queue, FString& OutError) = 0;
	virtual void Close() = 0;

	/** Si puo' chiamare da qualsiasi thread. */
	virtual FGeoEntitySourceStatus GetStatus() const = 0;
};

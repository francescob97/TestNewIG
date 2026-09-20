// =============================================================================
//  GeoLoaderPool.h -- Il pool di thread per le letture da disco. STRATO: UNREAL.
//
//  PERCHE' ESISTE (dalla Fase 6)
//  Fino alla Fase 5 questa logica stava dentro UGeoTileStreamingSubsystem.
//  Con le ortofoto serve un secondo streaming, e c'erano tre strade:
//  duplicare il codice, mettere tutto in una classe base astratta, o estrarre
//  la parte che e' davvero comune.
//
//  La parte davvero comune e' POCA e ben definita: il ciclo di vita del pool e
//  la traduzione della priorita'. Tutto il resto -- come si legge un file, come
//  si interpreta, dove finisce il risultato -- dipende dal payload e NON va
//  condiviso: una classe base che prova a condividerlo finisce per avere un
//  metodo virtuale per ogni differenza, che e' duplicazione con piu' passaggi.
//
//  Quindi: il pool si condivide, il lavoro no.
// =============================================================================
#pragma once

#include "CoreMinimal.h"
#include "Misc/QueuedThreadPool.h"

/**
 * Pool di thread dedicato alle letture BLOCCANTI.
 *
 * PERCHE' UN POOL DEDICATO E NON IL TASK GRAPH DEL MOTORE. Il task graph e'
 * dimensionato sui core e i suoi thread sono pensati per lavoro che non si
 * blocca mai. Una lettura da disco si blocca per definizione: occupare con essa
 * un thread del task graph significa togliere un core a tutto il resto del
 * motore per la durata dell'I/O. Un pool separato, con pochi thread e a
 * priorita' bassa, puo' invece stare fermo in attesa senza fare danni.
 */
class GEOTILES_API FGeoLoaderPool
{
public:
	FGeoLoaderPool() = default;

	// Non copiabile: possiede un pool di thread.
	FGeoLoaderPool(const FGeoLoaderPool&) = delete;
	FGeoLoaderPool& operator=(const FGeoLoaderPool&) = delete;

	~FGeoLoaderPool() { Shutdown(); }

	void Startup(int32 ThreadCount, const TCHAR* Name)
	{
		Shutdown();

		Threads = FMath::Max(1, ThreadCount);
		PoolName = Name;

		Pool = FQueuedThreadPool::Allocate();
		// TPri_BelowNormal: il caricamento non deve mai contendere la CPU con il
		// game thread. Meglio una tile che arriva un frame dopo che un frame che
		// salta.
		Pool->Create(Threads, 128 * 1024, TPri_BelowNormal, Name);
	}

	void Shutdown()
	{
		if (!Pool) { return; }

		// Destroy() chiama Abandon() su tutto cio' che e' ancora in coda e
		// aspetta i lavori gia' partiti. Senza, si distruggerebbe il proprietario
		// mentre dei thread ne stanno ancora usando la coda.
		Pool->Destroy();
		delete Pool;
		Pool = nullptr;
	}

	/**
	 * Abbandona i lavori ancora in coda, tenendo il pool vivo.
	 *
	 * I lavori GIA' PARTITI non si interrompono: durano pochi millisecondi e
	 * fermarli a meta' costerebbe piu' di quanto si risparmi.
	 */
	void CancelQueued()
	{
		if (!Pool) { return; }
		Pool->Destroy();
		delete Pool;
		Pool = FQueuedThreadPool::Allocate();
		Pool->Create(Threads, 128 * 1024, TPri_BelowNormal, *PoolName);
	}

	bool IsRunning() const { return Pool != nullptr; }
	int32 GetThreadCount() const { return Threads; }

	/**
	 * Traduce la nostra priorita' (0 = piu' urgente) in quella del motore.
	 *
	 * NOTA UE, ed e' una trappola vera: EQueuedWorkPriority va da Blocking(0) a
	 * Lowest(5), e il valore 6 e' Count, cioe' il CONTEGGIO dei valori, non una
	 * priorita'. Passarlo sarebbe un valore fuori dominio, e il compilatore non
	 * dice niente perche' e' pur sempre un valore dell'enum.
	 *
	 * Blocking si evita del tutto: blocca il pool finche' il lavoro non finisce,
	 * che e' l'ultima cosa da fare con una lettura da disco. Si mappa quindi su
	 * High(2)..Lowest(5).
	 */
	static EQueuedWorkPriority MapPriority(int32 Priority)
	{
		const int32 Value = FMath::Clamp(5 - FMath::Clamp(Priority, 0, 3),
			static_cast<int32>(EQueuedWorkPriority::High),
			static_cast<int32>(EQueuedWorkPriority::Lowest));
		return static_cast<EQueuedWorkPriority>(Value);
	}

	/**
	 * Accoda un lavoro.
	 *
	 * NOTA UE: AddQueuedWork prende la PROPRIETA' dell'oggetto, che si distrugge
	 * da solo in DoThreadedWork o in Abandon. Non va cancellato dal chiamante,
	 * e dimenticare Abandon() nella propria IQueuedWork e' la causa classica
	 * delle perdite di memoria alla chiusura.
	 */
	bool AddWork(IQueuedWork* Work, int32 Priority)
	{
		if (!Pool || !Work) { return false; }
		Pool->AddQueuedWork(Work, MapPriority(Priority));
		return true;
	}

private:
	FQueuedThreadPool* Pool = nullptr;
	int32 Threads = 4;
	FString PoolName = TEXT("GeoLoaderPool");
};

// =============================================================================
//  GeoSharedMemorySource.h -- La sorgente piu' semplice: la memoria condivisa.
//  STRATO: UNREAL.
//
//  Un thread suo guarda la SEQUENZA della memoria (Entities/SharedMemoryFormat.h)
//  ~500 volte al secondo; quando cambia, copia una fotografia coerente (il
//  seqlock: riprova se la trova a meta' scrittura), la traduce nella forma
//  canonica e la mette nella coda. Chi c'era nella fotografia prima e non c'e'
//  piu' se n'e' andato: per lui va in coda un "rimuovi".
//
//  Se la sequenza resta ferma per due secondi si richiude e si riapre la
//  memoria: su Linux un generatore ripartito ne crea una NUOVA con lo stesso
//  nome, e quella vecchia (che teniamo aperta noi) non cambierebbe piu'.
// =============================================================================
#pragma once

#include "CoreMinimal.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "Sources/GeoEntitySource.h"

#include <atomic>

class FGeoSharedMemorySource final : public IGeoEntitySource, public FRunnable
{
public:
	FGeoSharedMemorySource(uint32 InSourceId, const FString& InMemoryName);
	virtual ~FGeoSharedMemorySource() override;

	// IGeoEntitySource
	virtual uint32 GetSourceId() const override { return SourceId; }
	virtual FString GetName() const override;
	virtual FString GetAddress() const override { return MemoryName; }
	virtual bool Open(const TSharedRef<FGeoEntityQueue, ESPMode::ThreadSafe>& InQueue, FString& OutError) override;
	virtual void Close() override;
	virtual FGeoEntitySourceStatus GetStatus() const override;

	// FRunnable
	virtual uint32 Run() override;
	virtual void Stop() override { bStopping.store(true); }

private:
	void SetState(EGeoSourceState State, const FString& Message);

	const uint32 SourceId;
	const FString MemoryName;
	TSharedPtr<FGeoEntityQueue, ESPMode::ThreadSafe> Queue;
	FRunnableThread* Thread = nullptr;
	std::atomic<bool> bStopping{ false };

	mutable FCriticalSection StatusLock;
	FGeoEntitySourceStatus Status;
};

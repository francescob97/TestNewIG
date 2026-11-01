#include "Sources/GeoSharedMemorySource.h"

#include "Entities/SharedMemoryFormat.h"
#include "Sources/GeoSharedMemoryRegion.h"

#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/ScopeLock.h"

#include <string>
#include <vector>

using namespace GeoWorld::Entities;

namespace
{
	/** Ogni quanto si guarda la sequenza, s: 2 ms, ~500 volte al secondo. */
	constexpr float SharedMemoryPollS = 0.002f;
	/** Ogni quanto si riprova ad aprire una memoria che non c'e', s. */
	constexpr double SharedMemoryRetryOpenS = 0.5;
	/** Sequenza ferma da tanto: si riapre (il generatore potrebbe averne una nuova). */
	constexpr double SharedMemoryStaleS = 2.0;
	/** Quante volte si riprova una copia trovata a meta' scrittura, prima di aspettare il giro dopo. */
	constexpr int32 SharedMemoryCopyTries = 16;
}

FGeoSharedMemorySource::FGeoSharedMemorySource(uint32 InSourceId, const FString& InMemoryName)
	: SourceId(InSourceId)
	, MemoryName(InMemoryName)
{
}

FGeoSharedMemorySource::~FGeoSharedMemorySource()
{
	Close();
}

FString FGeoSharedMemorySource::GetName() const
{
	return FString::Printf(TEXT("memoria condivisa '%s'"), *MemoryName);
}

bool FGeoSharedMemorySource::Open(const TSharedRef<FGeoEntityQueue, ESPMode::ThreadSafe>& InQueue, FString& OutError)
{
	if (Thread) { return true; }
	Queue = InQueue;
	bStopping.store(false);
	SetState(EGeoSourceState::Waiting, FString::Printf(TEXT("aspetto la memoria '%s'"), *MemoryName));
	Thread = FRunnableThread::Create(this, *FString::Printf(TEXT("GeoEntities %s"), *MemoryName), 0, TPri_BelowNormal);
	if (!Thread)
	{
		OutError = TEXT("non riesco a far partire il thread di lettura");
		SetState(EGeoSourceState::Error, OutError);
		return false;
	}
	return true;
}

void FGeoSharedMemorySource::Close()
{
	if (Thread)
	{
		// Kill(true): chiama Stop() e aspetta che Run() sia uscito.
		Thread->Kill(true);
		delete Thread;
		Thread = nullptr;
	}
	Queue.Reset();
	SetState(EGeoSourceState::Stopped, TEXT("ferma"));
}

FGeoEntitySourceStatus FGeoSharedMemorySource::GetStatus() const
{
	FScopeLock Lock(&StatusLock);
	return Status;
}

void FGeoSharedMemorySource::SetState(EGeoSourceState State, const FString& Message)
{
	FScopeLock Lock(&StatusLock);
	Status.State = State;
	Status.Message = Message;
}

uint32 FGeoSharedMemorySource::Run()
{
	FGeoSharedMemoryRegion Region;
	uint64 LastSequence = ~0ull;
	double LastChange = 0.0;
	double LastOpenAttempt = -1.0e9;
	TSet<uint64> Previous;
	std::vector<uint8_t> Copy;
	std::vector<SharedMemory::FRecord> Records;

	while (!bStopping.load())
	{
		const double Now = FPlatformTime::Seconds();
		if (!Region.IsOpen())
		{
			if (Now - LastOpenAttempt < SharedMemoryRetryOpenS)
			{
				FPlatformProcess::Sleep(0.05f);
				continue;
			}
			LastOpenAttempt = Now;
			FString Error;
			if (!Region.Open(MemoryName, Error))
			{
				SetState(EGeoSourceState::Waiting, Error);
				continue;
			}
			LastSequence = ~0ull;
			LastChange = Now;
		}

		// La sequenza costa otto byte: la si guarda prima di copiare tutto.
		std::atomic_thread_fence(std::memory_order_acquire);
		const uint64 Sequence = Region.GetSize() >= SharedMemory::HeaderBytes
			? SharedMemory::Detail::ReadLe(Region.GetData() + SharedMemory::SequenceOffset, 8) : 0;
		if (Sequence == LastSequence)
		{
			if (Now - LastChange > SharedMemoryStaleS)
			{
				Region.Close();
				LastOpenAttempt = Now;
			}
			FPlatformProcess::Sleep(SharedMemoryPollS);
			continue;
		}

		std::string Error;
		SharedMemory::ECopyResult Result = SharedMemory::ECopyResult::Busy;
		int32 Retries = 0;
		for (int32 Try = 0; Try < SharedMemoryCopyTries; ++Try)
		{
			Result = SharedMemory::CopyConsistent(Region.GetData(), Region.GetSize(), Copy, Error);
			if (Result == SharedMemory::ECopyResult::Ok || Result == SharedMemory::ECopyResult::Invalid) { break; }
			++Retries;
			FPlatformProcess::YieldThread();
		}
		if (Retries > 0)
		{
			FScopeLock Lock(&StatusLock);
			Status.Retries += static_cast<uint64>(Retries);
		}
		if (Result == SharedMemory::ECopyResult::Invalid)
		{
			SetState(EGeoSourceState::Error, FString::Printf(TEXT("'%s': %s"), *MemoryName, UTF8_TO_TCHAR(Error.c_str())));
			Region.Close();
			LastOpenAttempt = Now;
			continue;
		}
		if (Result != SharedMemory::ECopyResult::Ok) { continue; }

		SharedMemory::FHeader Header;
		if (!SharedMemory::ReadSnapshot(Copy.data(), Copy.size(), Header, Records, Error))
		{
			SetState(EGeoSourceState::Error, FString::Printf(TEXT("'%s': %s"), *MemoryName, UTF8_TO_TCHAR(Error.c_str())));
			continue;
		}
		LastSequence = Header.Sequence;
		LastChange = Now;

		// La traduzione si fa QUI, sul thread della sorgente (sezione 9).
		TSet<uint64> Seen;
		Seen.Reserve(static_cast<int32>(Records.size()));
		for (const SharedMemory::FRecord& Record : Records)
		{
			Queue->Enqueue(SharedMemory::ToUpdate(Record, Header, SourceId, Now));
			Seen.Add(Record.Id);
		}
		for (const uint64 Id : Previous)
		{
			if (Seen.Contains(Id)) { continue; }
			FEntityUpdate Gone;
			Gone.Key = FEntityKey{ SourceId, Id };
			Gone.bRemove = true;
			Gone.ReceivedSeconds = Now;
			Queue->Enqueue(MoveTemp(Gone));
		}
		Previous = MoveTemp(Seen);

		{
			FScopeLock Lock(&StatusLock);
			Status.State = EGeoSourceState::Receiving;
			Status.Message = FString::Printf(TEXT("'%s': %d entita', tempo %.1f s%s"), *MemoryName,
				static_cast<int32>(Records.size()), Header.SimTime,
				Header.AltitudeAboveSeaLevel() ? TEXT(", quote sul livello del mare") : TEXT(", quote sull'ellissoide"));
			++Status.Messages;
			Status.Updates += Records.size();
			Status.LastMessageSeconds = Now;
			Status.LastCount = static_cast<int32>(Records.size());
		}
	}
	Region.Close();
	return 0;
}

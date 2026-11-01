// =============================================================================
//  GeoSharedMemoryRegion.h -- Aprire in lettura una memoria condivisa con un
//  nome. STRATO: UNREAL (piattaforma).
//
//  PERCHE' NON FPlatformMemory::MapNamedSharedMemoryRegion. Su Windows quella
//  di Unreal mette davanti al nome "Global\", sia per creare sia per aprire:
//  e' lo spazio dei nomi di tutto il sistema, dove creare vuole i diritti di
//  amministratore. Python (multiprocessing.shared_memory) crea nello spazio
//  della sessione, senza prefisso: Unreal cercherebbe "Global\GeoWorldEntities"
//  e non la troverebbe mai. Qui il nome si passa com'e':
//
//    Windows       OpenFileMappingW("GeoWorldEntities") + MapViewOfFile
//    Linux, macOS  shm_open("/GeoWorldEntities") + mmap (Python aggiunge la /)
//
//  Solo lettura: noi non scriviamo mai nella memoria di un altro.
// =============================================================================
#pragma once

#include "CoreMinimal.h"

class FGeoSharedMemoryRegion
{
public:
	FGeoSharedMemoryRegion() = default;
	~FGeoSharedMemoryRegion() { Close(); }
	FGeoSharedMemoryRegion(const FGeoSharedMemoryRegion&) = delete;
	FGeoSharedMemoryRegion& operator=(const FGeoSharedMemoryRegion&) = delete;

	/** False (e il perche' in OutError) se non c'e' o non si apre. */
	bool Open(const FString& Name, FString& OutError);
	void Close();

	bool IsOpen() const { return Data != nullptr; }
	const uint8* GetData() const { return Data; }
	SIZE_T GetSize() const { return Size; }

private:
	const uint8* Data = nullptr;
	SIZE_T Size = 0;
	/** Windows: l'handle del file mapping (gli altri sistemi lo chiudono subito dopo mmap). */
	void* Handle = nullptr;
};

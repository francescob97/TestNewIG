#include "Sources/GeoSharedMemoryRegion.h"

#if PLATFORM_WINDOWS
	#include "Windows/WindowsHWrapper.h"
#elif PLATFORM_LINUX || PLATFORM_MAC
	#include <cerrno>
	#include <cstring>
	#include <fcntl.h>
	#include <sys/mman.h>
	#include <sys/stat.h>
	#include <unistd.h>
#endif

bool FGeoSharedMemoryRegion::Open(const FString& Name, FString& OutError)
{
	Close();
#if PLATFORM_WINDOWS
	HANDLE Mapping = ::OpenFileMappingW(FILE_MAP_READ, 0, *Name);
	if (!Mapping)
	{
		const uint32 Code = ::GetLastError();
		OutError = Code == ERROR_FILE_NOT_FOUND
			? FString::Printf(TEXT("la memoria '%s' non c'e' (il generatore non e' acceso?)"), *Name)
			: FString::Printf(TEXT("la memoria '%s' non si apre (errore %u)"), *Name, Code);
		return false;
	}
	void* View = ::MapViewOfFile(Mapping, FILE_MAP_READ, 0, 0, 0);
	if (!View)
	{
		OutError = FString::Printf(TEXT("la memoria '%s' non si mappa (errore %u)"), *Name, ::GetLastError());
		::CloseHandle(Mapping);
		return false;
	}
	// La dimensione: quella della regione mappata (arrotondata alla pagina).
	MEMORY_BASIC_INFORMATION Information;
	if (::VirtualQuery(View, &Information, sizeof(Information)) == 0)
	{
		OutError = FString::Printf(TEXT("la memoria '%s' non dice quanto e' grande"), *Name);
		::UnmapViewOfFile(View);
		::CloseHandle(Mapping);
		return false;
	}
	Handle = Mapping;
	Data = static_cast<const uint8*>(View);
	Size = static_cast<SIZE_T>(Information.RegionSize);
	return true;
#elif PLATFORM_LINUX || PLATFORM_MAC
	const FString PosixName = Name.StartsWith(TEXT("/")) ? Name : TEXT("/") + Name;
	const int Descriptor = ::shm_open(TCHAR_TO_UTF8(*PosixName), O_RDONLY, 0);
	if (Descriptor < 0)
	{
		const int Code = errno;
		OutError = Code == ENOENT
			? FString::Printf(TEXT("la memoria '%s' non c'e' (il generatore non e' acceso?)"), *Name)
			: FString::Printf(TEXT("la memoria '%s' non si apre: %s"), *Name, UTF8_TO_TCHAR(std::strerror(Code)));
		return false;
	}
	struct stat Information;
	if (::fstat(Descriptor, &Information) != 0 || Information.st_size <= 0)
	{
		OutError = FString::Printf(TEXT("la memoria '%s' e' vuota"), *Name);
		::close(Descriptor);
		return false;
	}
	void* View = ::mmap(nullptr, static_cast<size_t>(Information.st_size), PROT_READ, MAP_SHARED, Descriptor, 0);
	::close(Descriptor);
	if (View == MAP_FAILED)
	{
		OutError = FString::Printf(TEXT("la memoria '%s' non si mappa: %s"), *Name, UTF8_TO_TCHAR(std::strerror(errno)));
		return false;
	}
	Data = static_cast<const uint8*>(View);
	Size = static_cast<SIZE_T>(Information.st_size);
	return true;
#else
	OutError = FString::Printf(TEXT("la memoria condivisa '%s' non e' supportata su questa piattaforma"), *Name);
	return false;
#endif
}

void FGeoSharedMemoryRegion::Close()
{
	if (!Data) { return; }
#if PLATFORM_WINDOWS
	::UnmapViewOfFile(Data);
	if (Handle) { ::CloseHandle(static_cast<HANDLE>(Handle)); }
#elif PLATFORM_LINUX || PLATFORM_MAC
	::munmap(const_cast<uint8*>(Data), Size);
#endif
	Data = nullptr;
	Size = 0;
	Handle = nullptr;
}

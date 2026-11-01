// =============================================================================
//  SharedMemoryFormat.h -- Il formato della memoria condivisa delle entita'.
//  STRATO: C++ PURO.
//
//  Il trasporto piu' semplice che c'e' (docs/fase7-design.md, sezione 12):
//  nessuna rete, nessun parsing di protocollo. Un altro programma (il
//  generatore finto di Pipeline/geoworld/entitygen.py, un simulatore vero)
//  tiene aperta una zona di memoria con un NOME, e ci scrive dentro lo stato
//  di tutte le sue entita'; noi la leggiamo.
//
//  Il nome: "GeoWorldEntities". E' quello che Python passa a
//  multiprocessing.shared_memory: su Windows il nome di un file mapping
//  (senza "Global\", che vorrebbe i diritti di amministratore), su Linux e
//  macOS "/GeoWorldEntities" di shm_open.
//
//  E' una FOTOGRAFIA, non un flusso di messaggi: ci sono tutte le entita'
//  vive, adesso. Chi non c'e' piu' nella fotografia se n'e' andato.
//
//  IL FORMATO (little-endian, versione 1):
//
//    intestazione, 64 byte
//       0  char[4]  "GENT"
//       4  uint16   versione (1)
//       6  uint16   byte dell'intestazione (64)
//       8  uint16   byte di un'entita' (128)
//      10  uint16   bandiere: bit 0 = le quote sono sul LIVELLO DEL MARE
//                   (altrimenti sull'ellissoide WGS84)
//      12  uint32   capacita' (quante entita' ci stanno)
//      16  uint32   quante ce ne sono adesso
//      20  uint32   riservato
//      24  uint64   SEQUENZA (vedi sotto)
//      32  float64  il tempo di simulazione della fotografia, secondi
//      40  ...      riservato fino a 64
//
//    un'entita', 128 byte, subito dopo, una dietro l'altra
//       0  uint64   identificatore (unico per questa sorgente)
//       8  uint8    genere, dominio; uint16 paese; uint8 categoria,
//                   sottocategoria, specifico, extra (il tipo di DIS)
//      16  uint8    bandiere: bit 0 = appoggiala al terreno
//      17  uint8    algoritmo di estrapolazione (1..9 come DIS; 0 tutto)
//      18  uint8    fazione (0 altro, 1 amico, 2 avversario, 3 neutrale)
//      19  uint8    riservato
//      20  int32    indice di modello (come CIGI; -1 nessuno)
//      24  float64  latitudine, longitudine (gradi), quota (metri)
//      48  float32  imbardata, beccheggio, rollio (gradi) rispetto al NED
//                   DEL PUNTO, come CIGI (non rispetto all'ECEF come DIS)
//      60  float32  velocita' Nord, Est, GIU' (m/s): NED, +giu' scende
//      72  float32  accelerazione Nord, Est, Giu' (m/s²)
//      84  float32  velocita' angolare p, q, r negli assi del corpo (rad/s)
//      96  char[32] il nome da scrivere sopra (UTF-8, finisce con 0)
//
//  LA SEQUENZA (un "seqlock"): chi scrive la porta a un numero DISPARI prima
//  di cominciare e al pari successivo quando ha finito. Chi legge la legge
//  prima e dopo aver copiato: se era dispari, o se e' cambiata, ha copiato a
//  meta' una scrittura, e riprova. Nessun lock fra due processi, nessuna
//  attesa per chi scrive.
// =============================================================================
#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "Entities/EntityFrames.h"
#include "Entities/EntityRegistry.h"

namespace GeoWorld::Entities::SharedMemory
{
	inline constexpr const char* Name = "GeoWorldEntities";
	inline constexpr uint16_t Version = 1;
	inline constexpr size_t HeaderBytes = 64;
	inline constexpr size_t RecordBytes = 128;
	inline constexpr size_t LabelBytes = 32;
	inline constexpr uint32_t DefaultCapacity = 1024;
	inline constexpr uint16_t FlagAltitudeMsl = 1;
	inline constexpr uint8_t RecordFlagClamp = 1;
	inline constexpr size_t SequenceOffset = 24;

	inline constexpr size_t BytesFor(uint32_t Capacity) { return HeaderBytes + RecordBytes * Capacity; }

	struct FHeader
	{
		uint16_t Version = SharedMemory::Version;
		uint16_t Flags = 0;
		uint32_t Capacity = 0;
		uint32_t Count = 0;
		uint64_t Sequence = 0;
		double SimTime = 0.0;

		bool AltitudeAboveSeaLevel() const { return (Flags & FlagAltitudeMsl) != 0; }
	};

	struct FRecord
	{
		uint64_t Id = 0;
		FEntityKind Kind;
		uint8_t Flags = 0;
		uint8_t DeadReckoning = 0;
		uint8_t Force = 0;
		int32_t ModelId = -1;
		double LatDeg = 0.0, LonDeg = 0.0, AltM = 0.0;
		float YawDeg = 0.0f, PitchDeg = 0.0f, RollDeg = 0.0f;
		float VelNorth = 0.0f, VelEast = 0.0f, VelDown = 0.0f;
		float AccNorth = 0.0f, AccEast = 0.0f, AccDown = 0.0f;
		float RateP = 0.0f, RateQ = 0.0f, RateR = 0.0f;
		std::string Label;
	};

	// =========================================================================
	//  Byte little-endian (esplicito: non si conta sull'ordine della macchina)
	// =========================================================================

	namespace Detail
	{
		inline uint64_t ReadLe(const uint8_t* At, int32_t Bytes)
		{
			uint64_t Out = 0;
			for (int32_t Index = Bytes - 1; Index >= 0; --Index) { Out = (Out << 8) | At[Index]; }
			return Out;
		}

		inline void WriteLe(uint8_t* At, uint64_t Value, int32_t Bytes)
		{
			for (int32_t Index = 0; Index < Bytes; ++Index) { At[Index] = static_cast<uint8_t>(Value >> (8 * Index)); }
		}

		inline double ReadF64(const uint8_t* At)
		{
			const uint64_t Bits = ReadLe(At, 8);
			double Out;
			std::memcpy(&Out, &Bits, 8);
			return Out;
		}

		inline float ReadF32(const uint8_t* At)
		{
			const uint32_t Bits = static_cast<uint32_t>(ReadLe(At, 4));
			float Out;
			std::memcpy(&Out, &Bits, 4);
			return Out;
		}

		inline void WriteF64(uint8_t* At, double Value)
		{
			uint64_t Bits;
			std::memcpy(&Bits, &Value, 8);
			WriteLe(At, Bits, 8);
		}

		inline void WriteF32(uint8_t* At, float Value)
		{
			uint32_t Bits;
			std::memcpy(&Bits, &Value, 4);
			WriteLe(At, Bits, 4);
		}
	}

	// =========================================================================
	//  Leggere
	// =========================================================================

	/** L'intestazione; false (e il perche') se non e' la nostra. */
	inline bool ReadHeader(const uint8_t* Bytes, size_t Size, FHeader& Out, std::string& Error)
	{
		if (Size < HeaderBytes) { Error = "la memoria e' piu' piccola di un'intestazione"; return false; }
		if (std::memcmp(Bytes, "GENT", 4) != 0) { Error = "non e' memoria di GeoWorld (manca 'GENT')"; return false; }
		Out.Version = static_cast<uint16_t>(Detail::ReadLe(Bytes + 4, 2));
		const size_t Header = static_cast<size_t>(Detail::ReadLe(Bytes + 6, 2));
		const size_t Record = static_cast<size_t>(Detail::ReadLe(Bytes + 8, 2));
		if (Out.Version != Version) { Error = "versione " + std::to_string(Out.Version) + ", questa build legge la " + std::to_string(Version); return false; }
		if (Header != HeaderBytes || Record != RecordBytes) { Error = "misure dell'intestazione o delle entita' sbagliate"; return false; }
		Out.Flags = static_cast<uint16_t>(Detail::ReadLe(Bytes + 10, 2));
		Out.Capacity = static_cast<uint32_t>(Detail::ReadLe(Bytes + 12, 4));
		Out.Count = static_cast<uint32_t>(Detail::ReadLe(Bytes + 16, 4));
		Out.Sequence = Detail::ReadLe(Bytes + SequenceOffset, 8);
		Out.SimTime = Detail::ReadF64(Bytes + 32);
		if (Out.Count > Out.Capacity) { Error = "piu' entita' della capacita'"; return false; }
		if (BytesFor(Out.Capacity) > Size) { Error = "la capacita' dichiarata non sta nella memoria"; return false; }
		return true;
	}

	inline FRecord ReadRecord(const uint8_t* At)
	{
		FRecord Out;
		Out.Id = Detail::ReadLe(At, 8);
		Out.Kind.Kind = At[8];
		Out.Kind.Domain = At[9];
		Out.Kind.Country = static_cast<uint16_t>(Detail::ReadLe(At + 10, 2));
		Out.Kind.Category = At[12];
		Out.Kind.Subcategory = At[13];
		Out.Kind.Specific = At[14];
		Out.Kind.Extra = At[15];
		Out.Flags = At[16];
		Out.DeadReckoning = At[17];
		Out.Force = At[18];
		Out.ModelId = static_cast<int32_t>(static_cast<uint32_t>(Detail::ReadLe(At + 20, 4)));
		Out.LatDeg = Detail::ReadF64(At + 24);
		Out.LonDeg = Detail::ReadF64(At + 32);
		Out.AltM = Detail::ReadF64(At + 40);
		float* const Floats[12] = { &Out.YawDeg, &Out.PitchDeg, &Out.RollDeg, &Out.VelNorth, &Out.VelEast, &Out.VelDown,
			&Out.AccNorth, &Out.AccEast, &Out.AccDown, &Out.RateP, &Out.RateQ, &Out.RateR };
		for (int32_t Index = 0; Index < 12; ++Index) { *Floats[Index] = Detail::ReadF32(At + 48 + 4 * Index); }
		const char* Text = reinterpret_cast<const char*>(At + 96);
		size_t Length = 0;
		while (Length < LabelBytes && Text[Length] != '\0') { ++Length; }
		Out.Label.assign(Text, Length);
		return Out;
	}

	/** Una fotografia intera (gia' copiata, coerente): l'intestazione e le entita'. */
	inline bool ReadSnapshot(const uint8_t* Bytes, size_t Size, FHeader& Header, std::vector<FRecord>& Records, std::string& Error)
	{
		Records.clear();
		if (!ReadHeader(Bytes, Size, Header, Error)) { return false; }
		if (HeaderBytes + RecordBytes * static_cast<size_t>(Header.Count) > Size) { Error = "la copia e' piu' corta delle entita' dichiarate"; return false; }
		Records.reserve(Header.Count);
		for (uint32_t Index = 0; Index < Header.Count; ++Index)
		{
			Records.push_back(ReadRecord(Bytes + HeaderBytes + RecordBytes * Index));
		}
		return true;
	}

	enum class ECopyResult : uint8_t { Ok, Busy, Torn, Invalid };

	/**
	 * Copia in Out una fotografia COERENTE della memoria mappata, o dice
	 * perche' no: Busy (chi scrive e' a meta', la sequenza e' dispari), Torn
	 * (e' cambiata durante la copia), Invalid (non e' il nostro formato). Chi
	 * chiama riprova qualche volta: una scrittura di mille entita' dura
	 * microsecondi.
	 *
	 * Le barriere (acquire) impediscono al compilatore e al processore di
	 * leggere la sequenza dopo i dati o i dati prima della sequenza.
	 */
	inline ECopyResult CopyConsistent(const uint8_t* Mapped, size_t MappedSize, std::vector<uint8_t>& Out, std::string& Error)
	{
		if (MappedSize < HeaderBytes) { Error = "memoria troppo piccola"; return ECopyResult::Invalid; }
		std::atomic_thread_fence(std::memory_order_acquire);
		const uint64_t Before = Detail::ReadLe(Mapped + SequenceOffset, 8);
		if ((Before & 1u) != 0) { return ECopyResult::Busy; }
		std::atomic_thread_fence(std::memory_order_acquire);

		FHeader Header;
		if (!ReadHeader(Mapped, MappedSize, Header, Error))
		{
			// Un'intestazione strana a meta' di una scrittura non e' ancora un errore.
			std::atomic_thread_fence(std::memory_order_acquire);
			return Detail::ReadLe(Mapped + SequenceOffset, 8) != Before ? ECopyResult::Torn : ECopyResult::Invalid;
		}
		const size_t Bytes = HeaderBytes + RecordBytes * static_cast<size_t>(Header.Count);
		Out.resize(Bytes);
		std::memcpy(Out.data(), Mapped, Bytes);

		std::atomic_thread_fence(std::memory_order_acquire);
		const uint64_t After = Detail::ReadLe(Mapped + SequenceOffset, 8);
		return After == Before ? ECopyResult::Ok : ECopyResult::Torn;
	}

	// =========================================================================
	//  Scrivere (per i test e per chi volesse scrivere da C++)
	// =========================================================================

	inline void WriteHeader(uint8_t* Bytes, const FHeader& Header)
	{
		std::memset(Bytes, 0, HeaderBytes);
		std::memcpy(Bytes, "GENT", 4);
		Detail::WriteLe(Bytes + 4, Header.Version, 2);
		Detail::WriteLe(Bytes + 6, HeaderBytes, 2);
		Detail::WriteLe(Bytes + 8, RecordBytes, 2);
		Detail::WriteLe(Bytes + 10, Header.Flags, 2);
		Detail::WriteLe(Bytes + 12, Header.Capacity, 4);
		Detail::WriteLe(Bytes + 16, Header.Count, 4);
		Detail::WriteLe(Bytes + SequenceOffset, Header.Sequence, 8);
		Detail::WriteF64(Bytes + 32, Header.SimTime);
	}

	inline void WriteRecord(uint8_t* At, const FRecord& Record)
	{
		std::memset(At, 0, RecordBytes);
		Detail::WriteLe(At, Record.Id, 8);
		At[8] = Record.Kind.Kind;
		At[9] = Record.Kind.Domain;
		Detail::WriteLe(At + 10, Record.Kind.Country, 2);
		At[12] = Record.Kind.Category;
		At[13] = Record.Kind.Subcategory;
		At[14] = Record.Kind.Specific;
		At[15] = Record.Kind.Extra;
		At[16] = Record.Flags;
		At[17] = Record.DeadReckoning;
		At[18] = Record.Force;
		Detail::WriteLe(At + 20, static_cast<uint32_t>(Record.ModelId), 4);
		Detail::WriteF64(At + 24, Record.LatDeg);
		Detail::WriteF64(At + 32, Record.LonDeg);
		Detail::WriteF64(At + 40, Record.AltM);
		const float Floats[12] = { Record.YawDeg, Record.PitchDeg, Record.RollDeg, Record.VelNorth, Record.VelEast, Record.VelDown,
			Record.AccNorth, Record.AccEast, Record.AccDown, Record.RateP, Record.RateQ, Record.RateR };
		for (int32_t Index = 0; Index < 12; ++Index) { Detail::WriteF32(At + 48 + 4 * Index, Floats[Index]); }
		std::memcpy(At + 96, Record.Label.data(), std::min(Record.Label.size(), LabelBytes - 1));
	}

	// =========================================================================
	//  Tradurre: dalla fotografia alla forma canonica (l'adattatore)
	// =========================================================================

	/**
	 * Un'entita' della memoria condivisa nella forma canonica. Qui passano
	 * due delle tre trappole: gli angoli sono LOCALI (NED del punto, come
	 * CIGI) e le velocita' sono NED (+giu' scende).
	 */
	inline FEntityUpdate ToUpdate(const FRecord& Record, const FHeader& Header, uint32_t Source, double ReceivedSeconds)
	{
		const double Lat = Record.LatDeg * Core::DegToRad, Lon = Record.LonDeg * Core::DegToRad;
		FEntityUpdate Out;
		Out.Key = FEntityKey{ Source, Record.Id };
		Out.Kind = Record.Kind;
		Out.Label = Record.Label;
		Out.ModelId = Record.ModelId;
		Out.Force = Record.Force <= 3 ? static_cast<EForce>(Record.Force) : EForce::Other;
		Out.Algorithm = Record.DeadReckoning <= 9 ? static_cast<EDeadReckoning>(Record.DeadReckoning) : EDeadReckoning::Other;
		Out.bAboveSeaLevel = Header.AltitudeAboveSeaLevel();
		Out.bClampToGround = (Record.Flags & RecordFlagClamp) != 0;
		Out.ReceivedSeconds = ReceivedSeconds;

		FEntityState& State = Out.State;
		State.Position = Core::GeodeticToEcef(Core::FGeodetic::FromRadians(Lat, Lon, Record.AltM), Core::WGS84);
		State.Orientation = OrientationFromLocalEuler(Lat, Lon,
			FEuler{ Record.YawDeg * Core::DegToRad, Record.PitchDeg * Core::DegToRad, Record.RollDeg * Core::DegToRad });
		State.Velocity = NedToEcef(Lat, Lon, FEcef{ Record.VelNorth, Record.VelEast, Record.VelDown });
		State.Acceleration = NedToEcef(Lat, Lon, FEcef{ Record.AccNorth, Record.AccEast, Record.AccDown });
		State.AngularVelocity = FEcef{ Record.RateP, Record.RateQ, Record.RateR };
		State.Time = Header.SimTime;
		return Out;
	}
}

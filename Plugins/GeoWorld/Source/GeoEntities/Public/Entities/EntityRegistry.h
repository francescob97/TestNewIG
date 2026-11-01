// =============================================================================
//  EntityRegistry.h -- Tutte le entita' conosciute. STRATO: C++ PURO.
//
//  Il registro vive sul game thread e basta (docs/fase7-design.md, sezione
//  9): gli adattatori, sui loro thread, traducono e consegnano degli
//  FEntityUpdate in una coda; il subsystem li vuota qui dentro una volta per
//  frame. Nessun lock nel percorso caldo.
//
//  Il registro NON sono gli attori (sezione 11): ci stanno tutte le entita',
//  anche quelle lontane mille chilometri; solo le vicine ricevono un attore,
//  e lo decide lo strato Unreal.
//
//  Il tempo: ogni sorgente ha il suo orologio (FClockSync). Un aggiornamento
//  porta il tempo di simulazione a cui e' vero e il tempo NOSTRO a cui e'
//  arrivato; il registro tiene la corrispondenza, e chi disegna chiede "dove
//  sta adesso" in tempo nostro.
// =============================================================================
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "Entities/DeadReckoning.h"
#include "Entities/EntityFrames.h"

namespace GeoWorld::Entities
{
	/** La fazione, quella di DIS: decide il colore di chi non ha un modello. */
	enum class EForce : uint8_t { Other = 0, Friendly = 1, Opposing = 2, Neutral = 3 };

	inline const char* ForceName(EForce Force)
	{
		switch (Force)
		{
		case EForce::Friendly: return "amico";
		case EForce::Opposing: return "avversario";
		case EForce::Neutral: return "neutrale";
		default: return "altro";
		}
	}

	/** Un messaggio di un adattatore: gia' tradotto nella forma canonica. */
	struct FEntityUpdate
	{
		FEntityKey Key;
		FEntityKind Kind;
		/** Il nome da scrivere sopra (il nominativo, la sigla). */
		std::string Label;
		/** L'indice di modello di CIGI (il database dell'IG); -1 nessuno. */
		int32_t ModelId = -1;
		EForce Force = EForce::Other;
		FEntityState State;
		EDeadReckoning Algorithm = EDeadReckoning::Other;
		/**
		 * La quota della posizione e' sul LIVELLO DEL MARE (il geoide), non
		 * sull'ellissoide: va alzata dell'ondulazione del punto (sezione 10).
		 * Lo dichiara la sorgente: non si indovina.
		 */
		bool bAboveSeaLevel = false;
		/** Va appoggiata al terreno: la quota la decide chi disegna (CIGI, i veicoli). */
		bool bClampToGround = false;
		/** L'entita' se ne va. */
		bool bRemove = false;
		/** Quando e' arrivato, nel tempo NOSTRO (secondi monotòni). */
		double ReceivedSeconds = 0.0;
	};

	/** Un'entita' nel registro. */
	struct FEntityRecord
	{
		FEntityKey Key;
		FEntityKind Kind;
		std::string Label;
		int32_t ModelId = -1;
		EForce Force = EForce::Other;
		bool bClampToGround = false;
		FEntityTrack Track;
		/** L'ultimo messaggio, nel tempo nostro: per chi tace troppo a lungo. */
		double LastHeardSeconds = 0.0;
		uint64_t Updates = 0;
		/** Messaggi scartati perche' piu' vecchi dell'ultimo (arrivati in disordine). */
		uint64_t Late = 0;
	};

	enum class EApplyResult : uint8_t { Created, Updated, Removed, Ignored };

	class FEntityRegistry
	{
	public:
		FTrackSettings Settings;

		/**
		 * L'ondulazione del geoide (metri) in un punto: serve alle sorgenti con
		 * le quote sul livello del mare. Vuota: nessuna correzione (e il
		 * subsystem lo dice a schermo).
		 */
		std::function<double(const Core::FGeodetic&)> GeoidUndulation;

		EApplyResult Apply(const FEntityUpdate& Update)
		{
			if (Update.bRemove)
			{
				return Records.erase(Update.Key) > 0 ? EApplyResult::Removed : EApplyResult::Ignored;
			}

			FClockSync& Clock = Clocks[Update.Key.Source];
			Clock.Observe(Update.State.Time, Update.ReceivedSeconds);
			const double Now = Clock.SimNow(Update.ReceivedSeconds);

			FEntityState State = Update.State;
			if (Update.bAboveSeaLevel && GeoidUndulation)
			{
				State.Position = RaiseAlongNormal(State.Position, GeoidUndulation(Core::EcefToGeodetic(State.Position, Core::WGS84)));
			}

			auto Found = Records.find(Update.Key);
			const bool bNew = Found == Records.end();
			FEntityRecord& Record = bNew ? Records[Update.Key] : Found->second;
			if (!bNew && Record.Track.HasState())
			{
				const double Previous = Record.Track.Last().Time;
				if (State.Time < Previous - 1.0)
				{
					// Molto piu' indietro: la simulazione e' ripartita. Si ricomincia.
					Record.Track = FEntityTrack{};
				}
				else if (State.Time < Previous)
				{
					// Un pacchetto arrivato in disordine: quello che abbiamo e' piu' nuovo.
					++Record.Late;
					Record.LastHeardSeconds = Update.ReceivedSeconds;
					return EApplyResult::Ignored;
				}
			}

			Record.Key = Update.Key;
			Record.Kind = Update.Kind;
			if (!Update.Label.empty()) { Record.Label = Update.Label; }
			Record.ModelId = Update.ModelId;
			Record.Force = Update.Force;
			Record.bClampToGround = Update.bClampToGround;
			Record.LastHeardSeconds = Update.ReceivedSeconds;
			++Record.Updates;
			Record.Track.Update(State, Update.Algorithm, Now, Settings);
			return bNew ? EApplyResult::Created : EApplyResult::Updated;
		}

		/** Chi tace da piu' di TimeoutSeconds se ne va (il battito di DIS). Restituisce chi. */
		std::vector<FEntityKey> Expire(double NowSeconds, double TimeoutSeconds)
		{
			std::vector<FEntityKey> Gone;
			for (auto It = Records.begin(); It != Records.end();)
			{
				if (NowSeconds - It->second.LastHeardSeconds > TimeoutSeconds)
				{
					Gone.push_back(It->first);
					It = Records.erase(It);
				}
				else
				{
					++It;
				}
			}
			return Gone;
		}

		/** Tutte quelle di una sorgente (che si e' fermata). */
		std::vector<FEntityKey> RemoveSource(uint32_t Source)
		{
			std::vector<FEntityKey> Gone;
			for (auto It = Records.begin(); It != Records.end();)
			{
				if (It->first.Source == Source) { Gone.push_back(It->first); It = Records.erase(It); }
				else { ++It; }
			}
			Clocks.erase(Source);
			return Gone;
		}

		const FEntityRecord* Find(const FEntityKey& Key) const
		{
			const auto It = Records.find(Key);
			return It == Records.end() ? nullptr : &It->second;
		}

		/** Dove disegnarla adesso (NowSeconds: il tempo nostro). */
		FDrawnPose PoseAt(const FEntityRecord& Record, double NowSeconds) const
		{
			const auto Clock = Clocks.find(Record.Key.Source);
			const double SimNow = Clock == Clocks.end() ? Record.Track.Last().Time : Clock->second.SimNow(NowSeconds);
			return Record.Track.Evaluate(SimNow, Settings);
		}

		const FClockSync* ClockOf(uint32_t Source) const
		{
			const auto It = Clocks.find(Source);
			return It == Clocks.end() ? nullptr : &It->second;
		}

		template <typename FunctionType>
		void ForEach(FunctionType&& Function) const
		{
			for (const auto& Pair : Records) { Function(Pair.second); }
		}

		size_t Num() const { return Records.size(); }
		void Clear() { Records.clear(); Clocks.clear(); }

	private:
		std::unordered_map<FEntityKey, FEntityRecord, FEntityKeyHash> Records;
		std::unordered_map<uint32_t, FClockSync> Clocks;
	};
}

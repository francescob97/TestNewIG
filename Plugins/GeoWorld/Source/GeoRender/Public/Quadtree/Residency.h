// =============================================================================
//  Residency.h -- Cosa tenere pronto, in base a DOVE si e' e DOVE si va.
//  STRATO: C++ PURO.
// =============================================================================
//
//  IL PROBLEMA
//  -----------
//  La Fase 4 sceglieva le tile guardando il frustum: caricava cio' che si
//  vedeva in quel momento. Funziona finche' la camera sta ferma; appena gira,
//  tutto cio' che entra nel campo va letto dal disco, trasformato in mesh e
//  vestito con la sua ortofoto MENTRE lo si sta gia' guardando. Da qui il
//  bordo nero ruotando e gli scatti.
//
//  L'IDEA
//  ------
//  Separare due domande che la Fase 4 confondeva:
//
//     "cosa DISEGNO adesso?"        -> dipende solo dalla POSIZIONE
//                                      (a scartare cio' che sta dietro ci
//                                       pensa il renderer di Unreal)
//     "cosa devo avere PRONTO?"     -> dipende dalla posizione e dalla
//                                      VELOCITA': dove saro' fra qualche
//                                      secondo
//
//  Questo file risponde alla seconda. Produce un PIANO DI RESIDENZA: la lista
//  delle tile che conviene avere in memoria, in ordine di urgenza, divisa in
//  fasce:
//
//     fascia 0            l'insieme ideale alla posizione attuale
//     fasce 1..N          l'insieme ideale nelle posizioni PREVISTE
//                         (fra 1/N, 2/N, ... N/N dell'orizzonte di previsione)
//     fascia N+1          l'anello di SICUREZZA: l'insieme ideale con una
//                         soglia d'errore piu' severa, cioe' "come se fossi
//                         piu' vicino a tutto". Copre i cambi di direzione che
//                         nessuna previsione lineare puo' indovinare.
//
//  Chi usa il piano decide quanto spingersi: le quote (66 KB a tile) si
//  caricano per tutte le fasce; le mesh (qualche MB a tile) solo fino alle
//  fasce previste; l'anello di sicurezza resta in RAM come quote.
//
//  "IDEALE" vuol dire: la selezione che si farebbe se tutto fosse gia' in
//  memoria. La selezione per il disegno invece si ferma dove i dati finiscono
//  (regola anti-buchi della Fase 4); per decidere cosa CARICARE serve sapere
//  dove si vorrebbe arrivare, non dove si e' arrivati.
// =============================================================================
#pragma once

#include "Quadtree/QuadtreeTypes.h"
#include "Quadtree/TileSelector.h"

#include <algorithm>
#include <cmath>
#include <unordered_set>
#include <vector>

namespace GeoWorld::Quadtree
{
	// ======================================================================
	//  1. L'insieme ideale
	// ======================================================================

	/**
	 * Avvolge una disponibilita' vera e risponde "si'" a IsTileLoaded.
	 *
	 * E' tutto cio' che serve per trasformare il selettore della Fase 4 in un
	 * calcolatore dell'insieme ideale: stesso attraversamento, stesse regole di
	 * esistenza, di orizzonte e di errore, ma senza fermarsi davanti ai dati
	 * mancanti. Riusarlo invece di scrivere un secondo attraversamento vuol
	 * dire che le due selezioni non possono divergere per un errore di copia.
	 */
	class FAssumeEverythingLoaded : public ITileAvailability
	{
	public:
		explicit FAssumeEverythingLoaded(const ITileAvailability& InInner) : Inner(InInner) {}

		virtual bool TileExists(const FTileKey& Key) const override { return Inner.TileExists(Key); }

		virtual bool GetHeightRange(const FTileKey& Key, double& OutMin, double& OutMax) const override
		{
			return Inner.GetHeightRange(Key, OutMin, OutMax);
		}

		virtual bool IsTileLoaded(const FTileKey&) const override { return true; }

	private:
		const ITileAvailability& Inner;
	};

	/** Le tile che si disegnerebbero da View se tutto fosse in memoria. */
	inline void SelectIdealTiles(const FViewParameters& View, const ITileAvailability& Availability,
	                             uint32_t MinLevel, uint32_t MaxLevel,
	                             FSelectionResult& OutResult,
	                             const Core::FEllipsoid& Ellipsoid = Core::WGS84)
	{
		const FAssumeEverythingLoaded Everything(Availability);
		SelectTiles(View, Everything, MinLevel, MaxLevel, OutResult, Ellipsoid);
	}

	// ======================================================================
	//  2. Dove saro' fra poco
	// ======================================================================

	/**
	 * Stima la velocita' della camera dalle sue posizioni e la proietta avanti.
	 *
	 * PERCHE' STIMARLA E NON CHIEDERLA. La camera puo' essere mossa da un
	 * pawn, dall'editor, da un comando geo.Goto o, piu' avanti, dall'host via
	 * CIGI/DIS. Nessuno di questi ha l'obbligo di dichiarare una velocita';
	 * tutti pero' producono posizioni. Derivarla dalle posizioni funziona con
	 * tutti senza toccarli. Quando la Fase 7 porra' in ingresso velocita' vere,
	 * basteranno a sostituire questa stima: l'interfaccia verso chi la usa e'
	 * solo PredictPosition().
	 *
	 * PERCHE' FILTRATA. La velocita' istantanea fra due frame e' rumorosa: un
	 * frame lento, un piccolo scatto del mouse. Un filtro esponenziale con
	 * costante di mezzo secondo la rende stabile senza farla arrivare in
	 * ritardo di secondi.
	 *
	 * PERCHE' RICONOSCERE IL TELETRASPORTO. Un geo.Goto da Roma a Milano sposta
	 * la camera di 480 km in un frame: come velocita' sarebbero trentamila
	 * chilometri al secondo, e la previsione metterebbe la camera in Siberia.
	 * Un salto va riconosciuto e trattato per cio' che e': non un moto, ma un
	 * ricominciare da capo.
	 */
	class FMotionPredictor
	{
	public:
		struct FSettings
		{
			/** Costante di tempo del filtro sulla velocita', in secondi. */
			double SmoothingSeconds = 0.5;

			/**
			 * Un salto oltre questa frazione della quota, in un solo
			 * aggiornamento, e' un teletrasporto.
			 *
			 * PERCHE' RELATIVO ALLA QUOTA. L'insieme di tile scala con la
			 * quota: da 2 km il dettaglio fine copre qualche chilometro, da
			 * 600 km copre mezza Italia. Una soglia fissa in metri sarebbe
			 * sbagliata in un verso o nell'altro: 5 km per frame sono un salto
			 * a bassa quota e un normale volo orbitale ad alta.
			 */
			double TeleportHeightFraction = 0.5;

			/** Sotto questa distanza non e' mai un teletrasporto (quote bassissime). */
			double TeleportMinDistanceMetres = 500.0;
		};

		FMotionPredictor() = default;
		explicit FMotionPredictor(const FSettings& InSettings) : Settings(InSettings) {}

		/**
		 * Nuova posizione. Ritorna true se e' stato un TELETRASPORTO: chi
		 * chiama dovrebbe allora buttare le richieste vecchie e ripartire.
		 */
		bool Update(const FEcef& Position, double TimeSeconds,
		            const Core::FEllipsoid& Ellipsoid = Core::WGS84)
		{
			if (!bHasSample)
			{
				Store(Position, TimeSeconds);
				return false;
			}

			const double DeltaTime = TimeSeconds - LastTime;
			const FEcef Displacement = Position - LastPosition;
			const double Distance = Displacement.Length();

			// Il salto si giudica sulla distanza, non sulla velocita': un geo.Goto
			// fatto a editor fermo arriva con un DeltaTime qualunque.
			const double Height = std::max(0.0, Core::EcefToGeodetic(Position, Ellipsoid).HeightM);
			const double Threshold = std::max(Settings.TeleportMinDistanceMetres,
			                                  Settings.TeleportHeightFraction * Height);
			if (Distance > Threshold)
			{
				Velocity = FEcef{ 0.0, 0.0, 0.0 };
				Store(Position, TimeSeconds);
				++TeleportCount;
				return true;
			}

			// Stesso istante (due chiamate nello stesso frame): niente da stimare.
			if (DeltaTime <= 1e-6) { return false; }

			// Filtro esponenziale scritto in funzione di DeltaTime: con frame
			// irregolari il peso del campione nuovo si adatta da solo, invece
			// di dipendere dal frame rate come farebbe un alpha fisso.
			const FEcef Instant = Displacement * (1.0 / DeltaTime);
			const double Alpha = 1.0 - std::exp(-DeltaTime / std::max(1e-3, Settings.SmoothingSeconds));
			Velocity = Velocity + (Instant - Velocity) * Alpha;

			Store(Position, TimeSeconds);
			return false;
		}

		void Reset()
		{
			bHasSample = false;
			Velocity = FEcef{ 0.0, 0.0, 0.0 };
		}

		bool HasSample() const { return bHasSample; }
		const FEcef& GetVelocity() const { return Velocity; }
		double GetSpeed() const { return Velocity.Length(); }
		const FEcef& GetPosition() const { return LastPosition; }
		int32_t GetTeleportCount() const { return TeleportCount; }

		/**
		 * Dove sara' la camera fra SecondsAhead secondi, a velocita' costante.
		 *
		 * Una retta in ECEF, non un arco: in 12 secondi a 250 m/s la corda si
		 * stacca dall'arco di 3 km^2 / (2 R) = 0.7 m. Per decidere quali tile
		 * caricare e' irrilevante.
		 */
		FEcef PredictPosition(double SecondsAhead) const
		{
			return LastPosition + Velocity * SecondsAhead;
		}

	private:
		void Store(const FEcef& Position, double TimeSeconds)
		{
			LastPosition = Position;
			LastTime = TimeSeconds;
			bHasSample = true;
		}

		FSettings Settings;
		FEcef LastPosition{ 0.0, 0.0, 0.0 };
		FEcef Velocity{ 0.0, 0.0, 0.0 };
		double LastTime = 0.0;
		bool bHasSample = false;
		int32_t TeleportCount = 0;
	};

	// ======================================================================
	//  3. Il piano
	// ======================================================================

	struct FResidencySettings
	{
		/**
		 * Quanti secondi avanti guardare. 12 s a 250 m/s sono 3 km: il tempo
		 * di caricare e costruire con larghezza, senza riempire la memoria di
		 * posti in cui forse non si andra'.
		 */
		double LookaheadSeconds = 12.0;

		/** Quante posizioni previste campionare lungo l'orizzonte. */
		int32_t PredictionSamples = 3;

		/** Sotto questa velocita' (m/s) la previsione non aggiunge niente. */
		double MinSpeedForPrediction = 1.0;

		/**
		 * Soglia dell'anello di sicurezza, come frazione della soglia normale.
		 * 0.5 = "come se fossi al doppio del dettaglio", cioe' circa la meta'
		 * della distanza da ogni cosa: copre un cambio di direzione brusco in
		 * qualunque verso. 0 lo spegne.
		 */
		double SafetyErrorFactor = 0.5;
	};

	/** Una tile del piano. */
	struct FResidencyEntry
	{
		FTileKey Key;

		/** 0 = adesso, 1..N = previste, N+1 = sicurezza. */
		int32_t Tier = 0;

		/** Distanza dalla posizione (attuale o prevista) che l'ha voluta. */
		double DistanceMetres = 0.0;
	};

	struct FResidencyPlan
	{
		/** Senza duplicati, in ordine di fascia e poi di distanza. */
		std::vector<FResidencyEntry> Entries;

		int32_t PredictionTiers = 0;     // quante fasce previste ci sono
		int32_t SafetyTier = -1;         // indice della fascia di sicurezza, -1 se spenta
		int32_t CountNow = 0;
		int32_t CountPredicted = 0;      // solo quelle NUOVE rispetto alle fasce prima
		int32_t CountSafety = 0;
		int32_t NodesVisited = 0;

		void Reset()
		{
			Entries.clear();
			PredictionTiers = 0;
			SafetyTier = -1;
			CountNow = CountPredicted = CountSafety = NodesVisited = 0;
		}
	};

	/**
	 * Costruisce il piano di residenza.
	 *
	 * View viene copiata e il frustum spento: il piano, per definizione, non
	 * dipende da dove si guarda. Da View si usano la posizione, la soglia e i
	 * parametri dello schermo (che servono all'errore su schermo).
	 */
	inline void BuildResidencyPlan(const FViewParameters& View,
	                               const FMotionPredictor& Motion,
	                               const FResidencySettings& Settings,
	                               const ITileAvailability& Availability,
	                               uint32_t MinLevel, uint32_t MaxLevel,
	                               FResidencyPlan& OutPlan,
	                               const Core::FEllipsoid& Ellipsoid = Core::WGS84)
	{
		OutPlan.Reset();

		FViewParameters Base = View;
		Base.bFrustumCulling = false;

		std::unordered_set<FTileKey> Seen;
		FSelectionResult Selection;

		// Aggiunge la selezione ideale da Probe come fascia Tier, saltando cio'
		// che una fascia precedente (piu' urgente) ha gia' messo nel piano.
		auto AddTier = [&](const FViewParameters& Probe, int32_t Tier) -> int32_t
		{
			SelectIdealTiles(Probe, Availability, MinLevel, MaxLevel, Selection, Ellipsoid);
			OutPlan.NodesVisited += Selection.NodesVisited;

			// Le piu' vicine per prime: a parita' di fascia sono quelle che
			// si vedranno piu' grandi.
			std::stable_sort(Selection.ToRender.begin(), Selection.ToRender.end(),
				[](const FSelectedTile& A, const FSelectedTile& B)
				{
					return A.DistanceMetres < B.DistanceMetres;
				});

			int32_t Added = 0;
			for (const FSelectedTile& Tile : Selection.ToRender)
			{
				if (!Seen.insert(Tile.Key).second) { continue; }
				OutPlan.Entries.push_back(FResidencyEntry{ Tile.Key, Tier, Tile.DistanceMetres });
				++Added;
			}
			return Added;
		};

		// --- Fascia 0: dove sono adesso --------------------------------------
		OutPlan.CountNow = AddTier(Base, 0);
		int32_t NextTier = 1;

		// --- Fasce 1..N: dove saro' ------------------------------------------
		const int32_t Samples = std::max(0, Settings.PredictionSamples);
		if (Samples > 0 && Settings.LookaheadSeconds > 0.0 &&
		    Motion.GetSpeed() >= Settings.MinSpeedForPrediction)
		{
			for (int32_t Index = 1; Index <= Samples; ++Index)
			{
				FViewParameters Probe = Base;
				Probe.CameraEcef = Motion.PredictPosition(
					Settings.LookaheadSeconds * static_cast<double>(Index) / Samples);
				OutPlan.CountPredicted += AddTier(Probe, NextTier++);
			}
			OutPlan.PredictionTiers = Samples;
		}

		// --- Fascia di sicurezza: tutto attorno, un livello piu' fine --------
		if (Settings.SafetyErrorFactor > 0.0)
		{
			FViewParameters Probe = Base;
			Probe.MaxScreenSpaceError = Base.MaxScreenSpaceError * Settings.SafetyErrorFactor;
			OutPlan.SafetyTier = NextTier;
			OutPlan.CountSafety = AddTier(Probe, NextTier++);
		}
	}
}

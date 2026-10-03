// =============================================================================
//  GeoTerrainSubsystem.h -- Dalla selezione LOD alla geometria. STRATO: UNREAL.
// =============================================================================
#pragma once

#include "CoreMinimal.h"
#include "Containers/Queue.h"
#include "Subsystems/WorldSubsystem.h"
#include "Tasks/Task.h"

#include "Mesh/TileMesh.h"
#include "Terrain/GeoTerrainMeshProvider.h"
#include "Tiles/TileKey.h"

#include "GeoTerrainSubsystem.generated.h"

class UGeoQuadtreeSubsystem;
class UGeoTileStreamingSubsystem;

USTRUCT()
struct FGeoTerrainStats
{
	GENERATED_BODY()

	UPROPERTY() int32 TileConGeometria = 0;
	UPROPERTY() int32 TileInAttesa = 0;
	UPROPERTY() int32 CostruiteQuestoFrame = 0;
	UPROPERTY() int32 RimosseQuestoFrame = 0;
	/** Triangoli che il RENDERER ha davvero (letti dal provider). */
	UPROPERTY() int32 TriangoliTotali = 0;
	/** Triangoli che il subsystem crede di aver costruito. Se i due numeri non
	 *  coincidono, la geometria si e' persa fra la mesh e il componente. */
	UPROPERTY() int32 TriangoliCostruiti = 0;
	UPROPERTY() float MemoriaGeometriaMB = 0.0f;
	UPROPERTY() float TempoCostruzioneMediaMs = 0.0f;
	UPROPERTY() int32 Rebase = 0;

	// --- Residenza delle mesh ----------------------------------------------
	/** Mesh costruite e a schermo / costruite ma nascoste (pronte o in memoria). */
	UPROPERTY() int32 MeshVisibili = 0;
	UPROPERTY() int32 MeshNascoste = 0;
	/** Mesh costruite in anticipo, nascoste, questo frame. */
	UPROPERTY() int32 PrecostruiteQuestoFrame = 0;
	/** Mesh nascoste buttate questo frame per rientrare nel budget. */
	UPROPERTY() int32 SfrattateQuestoFrame = 0;
	/** Quante tile della fascia "adesso" del piano hanno la mesh, e quante sono. */
	UPROPERTY() int32 PronteAdesso = 0;
	UPROPERTY() int32 PianoAdesso = 0;
	/** Idem per le fasce previste. */
	UPROPERTY() int32 ProntePreviste = 0;
	UPROPERTY() int32 PianoPreviste = 0;
	UPROPERTY() bool InRiscaldamento = false;

	// --- Costruzione su thread -------------------------------------------
	/** Mesh in costruzione sui thread di lavoro (o pronte, in attesa di consegna). */
	UPROPERTY() int32 InCostruzione = 0;
	/** Millisecondi spesi SUL GAME THREAD per consegnare le mesh, questo frame. */
	UPROPERTY() float ConsegnaMsQuestoFrame = 0.0f;
	UPROPERTY() bool OmbreAccese = false;

	/** Tile comparse al posto di un antenato questo frame, e transizioni in corso. */
	UPROPERTY() int32 RaffinateQuestoFrame = 0;
	UPROPERTY() int32 InTransizione = 0;
};

/**
 * Trasforma la decisione del quadtree in geometria vera.
 *
 * =============================================================================
 *  IL BUDGET PER FRAME
 * =============================================================================
 *  Costruire la mesh di una tile significa 16.641 conversioni geodetiche piu'
 *  33.792 triangoli: qualche millisecondo. Farne dieci in un frame lo fa
 *  saltare. Percio' se ne costruisce un numero limitato per frame, e il resto
 *  aspetta: il terreno si riempie in qualche decimo di secondo invece di
 *  comparire tutto insieme facendo scattare l'immagine.
 *
 *  Nel frattempo non si vedono buchi, perche' il quadtree continua a
 *  selezionare il padre finche' i figli non sono pronti (regola della Fase 4).
 *
 *  AGGIORNAMENTO: la costruzione NON sta piu' sul game thread. Nella Fase 5
 *  era un limite voluto ("aggiungere asincronia dopo, quando servira'"); e'
 *  servito alla prima prova su un portatile, dove 4 mesh per frame a qualche
 *  millisecondo l'una facevano girare il gioco a scatti appena ci si muoveva.
 *  Ora geodesia e FDynamicMesh3 si costruiscono sui thread di lavoro
 *  (UE::Tasks) e il budget per frame vale per la sola CONSEGNA al componente.
 */
UCLASS()
class GEORENDER_API UGeoTerrainSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual bool IsTickableInEditor() const override { return true; }

	void SetEnabled(bool bInEnabled);
	bool IsEnabled() const { return bTerrainEnabled; }

	void SetMaxTilesPerFrame(int32 Count) { MaxTilesPerFrame = FMath::Clamp(Count, 1, 64); }
	int32 GetMaxTilesPerFrame() const { return MaxTilesPerFrame; }

	/**
	 * Quante mesh tenere costruite in tutto, visibili piu' nascoste.
	 *
	 * Le visibili non si toccano mai; oltre il budget si buttano per prime le
	 * nascoste che il piano non vuole piu', dalla meno recente. Una mesh
	 * costa qualche MB fra RAM e scheda video: 2.000 sono dell'ordine dei
	 * 5-10 GB. Sulla macchina da 64 GB si puo' alzare.
	 */
	void SetMeshBudget(int32 Count) { MeshBudget = FMath::Clamp(Count, 64, 100000); }
	int32 GetMeshBudget() const { return MeshBudget; }

	/**
	 * Mesh per frame durante il RISCALDAMENTO: dopo un teletrasporto, o
	 * quando manca buona parte di cio' che serve adesso. 0 lo spegne.
	 *
	 * E' la scelta "meglio un'attesa lunga una volta sola": per qualche
	 * decimo di secondo il frame rate scende, e in cambio il terreno arriva
	 * tutto insieme invece di riempirsi a pezzi per secondi.
	 */
	/**
	 * Mesh in costruzione contemporaneamente sui thread di lavoro. Oltre, si
	 * aspetta che se ne liberi uno. Il doppio in riscaldamento.
	 */
	void SetBuildsInFlight(int32 Count) { BuildsInFlight = FMath::Clamp(Count, 1, 64); }
	int32 GetBuildsInFlight() const { return BuildsInFlight; }

	/** Ombre proiettate dal terreno. Default spente: vedi IGeoTerrainMeshProvider::SetCastShadows. */
	void SetCastShadows(bool bInCastShadows);
	bool IsCastingShadows() const { return Provider.IsValid() ? Provider->IsCastingShadows() : bCastShadowsWanted; }

	void SetWarmupTilesPerFrame(int32 Count) { WarmupTilesPerFrame = FMath::Clamp(Count, 0, 256); }
	int32 GetWarmupTilesPerFrame() const { return WarmupTilesPerFrame; }
	bool IsWarmingUp() const { return bWarmingUp; }

	/**
	 * Durata delle transizioni quando una tile si affina (geomorphing e
	 * dissolvenza della foto), in secondi. 0 = di colpo, come prima.
	 */
	void SetTransitionSeconds(float Seconds)
	{
		TransitionSeconds = FMath::Clamp(Seconds, 0.0f, 5.0f);
		if (Provider.IsValid()) { Provider->SetTransitionSeconds(TransitionSeconds); }
	}
	float GetTransitionSeconds() const { return TransitionSeconds; }

	void SetSkirtEnabled(bool bInSkirt);
	bool IsSkirtEnabled() const { return MeshParameters.bGenerateSkirt; }

	/**
	 * Passo della mesh: 1, 2, 4 (un post ogni N). Default 2. Cambiarlo
	 * ricostruisce tutto e dice al quadtree di quanto cresce l'errore
	 * geometrico. Vedi FTileMeshParameters::Step.
	 */
	void SetMeshStep(int32 InStep);
	int32 GetMeshStep() const { return MeshParameters.Step; }

	/** Inverte l'orientamento dei triangoli. Se il terreno e' invisibile
	 *  dall'alto e visibile da sotto, e' questo. */
	void SetFlipWinding(bool bInFlip);
	bool IsFlipWinding() const { return MeshParameters.bFlipWinding; }

	void SetWireframe(bool bInWireframe);
	bool IsWireframe() const { return Provider.IsValid() && Provider->IsWireframe(); }

	void SetDebugOverlayEnabled(bool bInShow) { bShowDebugOverlay = bInShow; }
	bool IsDebugOverlayEnabled() const { return bShowDebugOverlay; }

	/** Butta tutta la geometria e la ricostruisce. Utile dopo un cambio di parametri. */
	void RebuildAll();

	FGeoTerrainStats GetStats() const { return Stats; }
	FString GetProviderName() const { return Provider.IsValid() ? Provider->GetName() : TEXT("nessuno"); }

	/**
	 * Disegna i BOUNDS dei componenti come scatole di debug.
	 *
	 * PERCHE' SERVE UN SECONDO CANALE DI DISEGNO. Le linee di debug non passano
	 * per il materiale, non hanno faccia frontale e non vengono nebbiate come la
	 * geometria opaca. Se le scatole si vedono e il terreno no, la geometria e'
	 * al posto giusto e il problema e' nel come viene ombreggiata; se non si
	 * vede nemmeno una scatola, non stai guardando dove credi.
	 */
	void SetDrawBounds(bool bInDraw) { bDrawBounds = bInDraw; }
	bool IsDrawBounds() const { return bDrawBounds; }

	/**
	 * Le tile che hanno geometria adesso, VISIBILI PER PRIME. Serve al
	 * drappeggio della Fase 6: vestendo nell'ordine, le texture di cio' che
	 * si vede vengono create prima di quelle delle mesh nascoste.
	 * Ritorna quante delle prime sono visibili.
	 */
	int32 GetBuiltTileKeys(TArray<GeoWorld::Tiles::FTileKey>& Out) const
	{
		Out.Reset();
		Out.Reserve(BuiltTiles.Num());
		for (const TPair<uint64, FBuiltTile>& Pair : BuiltTiles)
		{
			if (Pair.Value.bVisible) { Out.Add(Pair.Value.Key); }
		}
		const int32 VisibleCount = Out.Num();
		for (const TPair<uint64, FBuiltTile>& Pair : BuiltTiles)
		{
			if (!Pair.Value.bVisible) { Out.Add(Pair.Value.Key); }
		}
		return VisibleCount;
	}

	/**
	 * Chi decide se una tile costruita e' anche VESTITA abbastanza da mostrarla.
	 *
	 * PERCHE' ESISTE. Il provider crea le tile con il materiale grigio di base,
	 * e la foto arriva quando il subsystem delle ortofoto passa a vestirle: un
	 * frame dopo, o piu' di uno se il budget di texture e' finito. Per quei
	 * frame la tile compariva GRIGIO CHIARO in mezzo alle foto: i "flash"
	 * della terza prova su Torino, a decine quando ci si gira. Le ortofoto
	 * registrano qui la loro risposta, e il terreno la aggiunge a "ha la
	 * mesh" nella readiness che da' al quadtree: una tile compare solo vestita,
	 * e fino ad allora resta a schermo il padre.
	 *
	 * DALLA FASE 8 I VESTITORI SONO PIU' D'UNO: le ortofoto e le strade. Ognuno
	 * registra il proprio predicato con un nome, e una tile e' vestita quando
	 * TUTTI dicono di si'. Con un predicato solo, il secondo a registrarsi
	 * avrebbe cancellato il primo, e le tile sarebbero tornate a comparire
	 * senza foto (o senza strade, che spuntano un attimo dopo).
	 */
	void SetDressPredicate(FName Owner, TFunction<bool(const GeoWorld::Tiles::FTileKey&)> InPredicate)
	{
		DressPredicates.Add(Owner, MoveTemp(InPredicate));
	}
	void ClearDressPredicate(FName Owner) { DressPredicates.Remove(Owner); }

	/** Tutti i vestitori registrati dicono che la tile e' pronta? */
	bool IsTileDressed(const GeoWorld::Tiles::FTileKey& Key) const
	{
		for (const TPair<FName, TFunction<bool(const GeoWorld::Tiles::FTileKey&)>>& Pair : DressPredicates)
		{
			if (Pair.Value && !Pair.Value(Key)) { return false; }
		}
		return true;
	}

	/** Il provider, per chi deve vestire le tile. Puo' essere nullo. */
	IGeoTerrainMeshProvider* GetProvider() const { return Provider.Get(); }

	/** Diagnosi delle prime MaxEntries tile, lette dal provider (geo.Terrain.Diag). */
	void GetTileDiagnostics(TArray<FGeoTerrainTileDiagnostic>& Out, int32 MaxEntries) const
	{
		Out.Reset();
		if (Provider.IsValid()) { Provider->GetDiagnostics(Out, MaxEntries); }
	}

private:
	void SynchroniseWithSelection();
	bool DispatchBuild(const GeoWorld::Tiles::FTileKey& Key, bool bUrgent);
	void CommitCompletedBuilds(const FGeoreferenceSnapshot& Snapshot, const TSet<uint64>& Visible, int32 Budget);
	void WaitForAllBuilds();
	void EvictHiddenMeshes(const TSet<uint64>& Visible);
	void RefreshPlanSet();
	void DrawDebugOverlay();
	void DrawTileBounds();
	void OnGeoreferenceRebased(const FGeoreferenceSnapshot& Snapshot);

	TUniquePtr<IGeoTerrainMeshProvider> Provider;

	UPROPERTY(Transient) TObjectPtr<UGeoQuadtreeSubsystem> Quadtree;
	UPROPERTY(Transient) TObjectPtr<UGeoTileStreamingSubsystem> Streaming;

	/**
	 * Tile che hanno geometria costruita.
	 *
	 * Si conserva la CHIAVE COMPLETA e non solo il conteggio: la chiave
	 * impacchettata usata per la mappa non e' invertibile, e senza quella vera
	 * non si potrebbe dire al provider quale componente distruggere ne' al
	 * loader quale tile spinnare.
	 */
	struct FBuiltTile
	{
		GeoWorld::Tiles::FTileKey Key;
		int32 TriangleCount = 0;

		/** A schermo adesso? Le nascoste sono pronte in anticipo o tenute dopo. */
		bool bVisible = false;

		/** Ultima volta che era visibile o voluta dal piano: decide chi sfrattare. */
		double LastWantedSeconds = 0.0;
	};
	TMap<uint64, FBuiltTile> BuiltTiles;

	/** Mostra/nasconde nel provider e tiene allineati flag e pin delle quote. */
	void SetBuiltVisible(FBuiltTile& Built, bool bVisible);

	/**
	 * Le tile del piano di cui vale la pena costruire la MESH: fasce "adesso"
	 * e "previste". La fascia di sicurezza no: resta come quote in RAM, perche'
	 * costruirla costerebbe migliaia di mesh per posti dove probabilmente non
	 * si andra'. Ricalcolato solo quando il quadtree rifa' il piano.
	 */
	/** I vestitori, per nome ("Ortofoto", "Strade"): vedi SetDressPredicate. */
	TMap<FName, TFunction<bool(const GeoWorld::Tiles::FTileKey&)>> DressPredicates;

	TSet<uint64> PlanMeshKeys;
	int32 PlanGenerationSeen = -1;
	int32 TeleportsSeen = 0;

	// --- Costruzione su thread ----------------------------------------------
	//
	//  game thread                         thread di lavoro (UE::Tasks)
	//  -----------                         ----------------------------
	//  DispatchBuild(tile) ------------->  BuildTileMesh (geodesia)
	//                                      PrepareTileMesh (FDynamicMesh3)
	//  CommitCompletedBuilds  <---coda---  risultato
	//    SetMesh + trasformazione
	//
	//  Il game thread fa solo le due estremita'. Tutto il resto, cioe' quasi
	//  tutto il costo, sta sui thread di lavoro.

	/** Risultato di una costruzione, consegnato al game thread. */
	struct FMeshBuildResult
	{
		GeoWorld::Tiles::FTileKey Key;
		int32 Generation = 0;
		FGeoPreparedTileMeshPtr Prepared;
		double Seconds = 0.0;
	};

	/**
	 * La coda sta in un oggetto CONDIVISO, non dentro il subsystem: ogni lavoro
	 * in volo ne tiene un riferimento. Se il subsystem venisse distrutto con un
	 * lavoro ancora in corso, il lavoro scriverebbe comunque in una coda viva,
	 * invece che nella memoria di un oggetto morto.
	 */
	struct FMeshBuildQueue
	{
		TQueue<FMeshBuildResult, EQueueMode::Mpsc> Completed;
	};
	TSharedPtr<FMeshBuildQueue, ESPMode::ThreadSafe> BuildQueue;

	struct FInFlightBuild
	{
		UE::Tasks::FTask Task;
		/** Chiesta dal disegno (true) o costruzione in anticipo (false). Solo statistica. */
		bool bUrgent = false;
	};
	TMap<uint64, FInFlightBuild> InFlightBuilds;

	/** Lavori di una generazione scaduta: non servono piu', ma vanno attesi alla chiusura. */
	TArray<UE::Tasks::FTask> OrphanBuilds;

	/**
	 * Cresce a ogni RebuildAll (gonne, orientamento...): un risultato con una
	 * generazione vecchia e' stato costruito con i parametri di prima e si butta.
	 */
	int32 BuildGeneration = 0;

	/** Il passo si porta a 2 in Initialize: vedi FTileMeshParameters::Step e prova-torino.md. */
	GeoWorld::Mesh::FTileMeshParameters MeshParameters;
	FGeoTerrainStats Stats;
	bool bDrawBounds = false;
	FDelegateHandle RebaseHandle;

	int32 MaxTilesPerFrame = 4;
	/** Si ridimensiona in Initialize in base alla RAM della macchina. */
	int32 MeshBudget = 2000;
	int32 WarmupTilesPerFrame = 16;
	int32 BuildsInFlight = 6;
	bool bCastShadowsWanted = false;
	float TransitionSeconds = 0.6f;
	bool bWarmingUp = false;
	bool bTerrainEnabled = false;
	bool bShowDebugOverlay = false;

	int32 BuildCount = 0;
	double TotalBuildSeconds = 0.0;
};

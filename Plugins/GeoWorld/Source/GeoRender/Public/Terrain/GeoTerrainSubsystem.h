// =============================================================================
//  GeoTerrainSubsystem.h -- Dalla selezione LOD alla geometria. STRATO: UNREAL.
// =============================================================================
#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"

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
 *  La costruzione sta comunque sul game thread. E' il limite principale di
 *  questa fase, ed e' un limite VOLUTO per ora: FTileMeshData e BuildTileMesh
 *  sono C++ puro senza alcuno stato condiviso, quindi spostarli sul thread pool
 *  della Fase 3 e' un lavoro localizzato. Farlo adesso avrebbe aggiunto
 *  asincronia a una fase che ha gia' abbastanza modi di essere sbagliata.
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
	void SetWarmupTilesPerFrame(int32 Count) { WarmupTilesPerFrame = FMath::Clamp(Count, 0, 256); }
	int32 GetWarmupTilesPerFrame() const { return WarmupTilesPerFrame; }
	bool IsWarmingUp() const { return bWarmingUp; }

	void SetSkirtEnabled(bool bInSkirt);
	bool IsSkirtEnabled() const { return MeshParameters.bGenerateSkirt; }

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
	bool BuildTile(const GeoWorld::Tiles::FTileKey& Key, const FGeoreferenceSnapshot& Snapshot, bool bVisible);
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
	TSet<uint64> PlanMeshKeys;
	int32 PlanGenerationSeen = -1;
	int32 TeleportsSeen = 0;

	GeoWorld::Mesh::FTileMeshParameters MeshParameters;
	FGeoTerrainStats Stats;
	bool bDrawBounds = false;
	FDelegateHandle RebaseHandle;

	int32 MaxTilesPerFrame = 4;
	int32 MeshBudget = 2000;
	int32 WarmupTilesPerFrame = 24;
	bool bWarmingUp = false;
	bool bTerrainEnabled = false;
	bool bShowDebugOverlay = false;

	int32 BuildCount = 0;
	double TotalBuildSeconds = 0.0;
};

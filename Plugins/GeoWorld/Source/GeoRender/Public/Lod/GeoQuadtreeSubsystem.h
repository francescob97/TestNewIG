// =============================================================================
//  GeoQuadtreeSubsystem.h -- Selezione LOD per frame. STRATO: UNREAL.
// =============================================================================
#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"

#include "Quadtree/QuadtreeTypes.h"
#include "Quadtree/Residency.h"
#include "Quadtree/TileSelector.h"

#include "GeoQuadtreeSubsystem.generated.h"

class UGeoTileStreamingSubsystem;

USTRUCT()
struct FGeoQuadtreeStats
{
	GENERATED_BODY()

	UPROPERTY() int32 NodiVisitati = 0;
	UPROPERTY() int32 TileDisegnate = 0;
	UPROPERTY() int32 TileRichieste = 0;
	UPROPERTY() int32 ScartateFrustum = 0;
	UPROPERTY() int32 ScartateOrizzonte = 0;
	UPROPERTY() int32 ScartateAssenti = 0;
	UPROPERTY() int32 NodiRaffinati = 0;
	UPROPERTY() int32 LivelloMinimo = 0;
	UPROPERTY() int32 LivelloMassimo = 0;
	UPROPERTY() float ErrorePeggiorePx = 0.0f;
	UPROPERTY() float SogliaErrorePx = 4.0f;
	UPROPERTY() float TempoSelezioneMs = 0.0f;

	// --- Residenza (vedi Quadtree/Residency.h) ---------------------------
	UPROPERTY() bool VistaIndipendente = true;
	UPROPERTY() float VelocitaMs = 0.0f;
	UPROPERTY() int32 Teletrasporti = 0;
	UPROPERTY() int32 PianoAdesso = 0;
	UPROPERTY() int32 PianoPreviste = 0;
	UPROPERTY() int32 PianoSicurezza = 0;
	/** Quante tile di ciascuna fascia hanno gia' le quote in RAM. */
	UPROPERTY() int32 InRamAdesso = 0;
	UPROPERTY() int32 InRamPreviste = 0;
	UPROPERTY() int32 InRamSicurezza = 0;
	UPROPERTY() int32 RichiestePrecarico = 0;
	UPROPERTY() float TempoPianoMs = 0.0f;
};

/**
 * =============================================================================
 *  COSA FA
 * =============================================================================
 *  A ogni frame guarda dove sta la camera, attraversa il quadtree e decide
 *  quali tile andrebbero disegnate. Quelle che mancano le chiede al loader
 *  della Fase 3; quelle che ci sono finiscono in una lista che la Fase 5
 *  trasformera' in mesh.
 *
 *  Finche' la Fase 5 non esiste, la lista si vede solo col disegno di debug:
 *  e' lo stesso motivo per cui la Fase 3 aveva bisogno dei suoi strumenti.
 *
 * =============================================================================
 *  PERCHE' LA SELEZIONE STA IN C++ PURO E QUI C'E' SOLO LA COLLA
 * =============================================================================
 *  Tutta la matematica — volumi, frustum, orizzonte, errore su schermo,
 *  attraversamento — vive in GeoRender/Public/Quadtree senza una riga di
 *  Unreal, ed e' verificata da 37 test eseguibili in un secondo. Qui si fa solo
 *  il lavoro che richiede il motore: leggere la camera, convertire in ECEF,
 *  girare il risultato al loader, disegnare.
 *
 *  E' la stessa divisione delle fasi precedenti, e ha gia' ripagato: il bug
 *  dell'horizon culling che scartava la tile contenente la camera e' stato
 *  trovato da un test standalone, non aprendo l'editor.
 */
UCLASS()
class GEORENDER_API UGeoQuadtreeSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual bool IsTickableInEditor() const override { return true; }

	/** Attiva o disattiva la selezione per frame. */
	void SetEnabled(bool bInEnabled) { bEnabled = bInEnabled; }
	bool IsEnabled() const { return bEnabled; }

	/** Errore su schermo tollerato, in pixel. E' la manopola principale del LOD. */
	void SetMaxScreenSpaceError(double Pixels);
	double GetMaxScreenSpaceError() const { return MaxScreenSpaceError; }

	void SetFrustumMargin(double InMargin) { FrustumMargin = FMath::Clamp(InMargin, 1.0, 3.0); }

	/**
	 * Quanto meno dettaglio fuori dalla vista: 1 = tutto uguale in ogni
	 * direzione (serve memoria: ~3 volte le tile), 4 = dietro tile ~4 volte
	 * piu' grossolane per lato. Vedi FViewParameters::OutOfViewErrorFactor.
	 */
	void SetOutOfViewErrorFactor(double Factor) { OutOfViewErrorFactor = FMath::Clamp(Factor, 1.0, 64.0); bPlanDirty = true; }
	double GetOutOfViewErrorFactor() const { return OutOfViewErrorFactor; }

	/** Il terreno dice qui il passo della propria mesh (FTileMeshParameters::Step). */
	void SetGeometricErrorScale(double Scale) { GeometricErrorScale = FMath::Max(1.0, Scale); bPlanDirty = true; }
	double GetGeometricErrorScale() const { return GeometricErrorScale; }
	double GetFrustumMargin() const { return FrustumMargin; }

	/** Blocca la selezione sulla vista corrente: utile per ispezionarla da fuori. */
	void SetFrozen(bool bInFrozen) { bFrozen = bInFrozen; }
	bool IsFrozen() const { return bFrozen; }

	void SetDebugOverlayEnabled(bool bInEnabled) { bShowDebugOverlay = bInEnabled; }
	bool IsDebugOverlayEnabled() const { return bShowDebugOverlay; }
	void SetDebugDrawEnabled(bool bInEnabled) { bDrawSelection = bInEnabled; }
	bool IsDebugDrawEnabled() const { return bDrawSelection; }

	FGeoQuadtreeStats GetStats() const { return Stats; }

	/** Le tile scelte per il disegno in questo frame. La Fase 5 leggera' questa. */
	const TArray<GeoWorld::Quadtree::FSelectedTile>& GetSelectedTiles() const { return SelectedTiles; }

	/** Esegue una selezione adesso, fuori dal tick. Ritorna false se manca la vista. */
	bool RunSelection();

	// --- Residenza ----------------------------------------------------------
	//
	// Due modi di lavorare, per poterli confrontare:
	//   vista-indipendente (default): si disegna tutto cio' che serve attorno
	//     alla camera, in ogni direzione; il frustum lo applica Unreal.
	//   classico (Fase 4): si seleziona solo cio' che sta nel frustum allargato.

	void SetViewIndependent(bool bInViewIndependent) { bViewIndependent = bInViewIndependent; }
	bool IsViewIndependent() const { return bViewIndependent; }

	/** Precaricamento in base al piano di residenza. */
	void SetPrefetchEnabled(bool bInEnabled) { bPrefetchEnabled = bInEnabled; bPlanDirty = true; }
	bool IsPrefetchEnabled() const { return bPrefetchEnabled; }

	void SetLookaheadSeconds(double Seconds)
	{
		ResidencySettings.LookaheadSeconds = FMath::Clamp(Seconds, 0.0, 120.0);
		bPlanDirty = true;
	}
	double GetLookaheadSeconds() const { return ResidencySettings.LookaheadSeconds; }

	void SetSafetyFactor(double Factor)
	{
		// 0 spegne; sopra 1 non avrebbe senso (sarebbe MENO dettaglio della
		// fascia attuale, cioe' tutte tile gia' coperte).
		ResidencySettings.SafetyErrorFactor = FMath::Clamp(Factor, 0.0, 1.0);
		bPlanDirty = true;
	}
	double GetSafetyFactor() const { return ResidencySettings.SafetyErrorFactor; }

	/** Il piano corrente: lo legge il terreno per costruire in anticipo. */
	const GeoWorld::Quadtree::FResidencyPlan& GetResidencyPlan() const { return Plan; }

	/** Cresce a ogni ricalcolo del piano: chi ne tiene una copia sa quando rifarla. */
	int32 GetPlanGeneration() const { return PlanGeneration; }

	/** Tile che il disegno vorrebbe ADESSO e non ha: le piu' urgenti di tutte. */
	const TArray<GeoWorld::Quadtree::FTileRequest>& GetRenderRequests() const { return RenderRequests; }

	/** Quanti teletrasporti ha visto: il terreno lo usa per entrare in riscaldamento. */
	int32 GetTeleportCount() const { return Motion.GetTeleportCount(); }

	/**
	 * Chi decide se una tile e' "pronta per il disegno".
	 *
	 * Per la Fase 4 da sola basta che le quote siano in RAM. Con il terreno
	 * acceso invece serve che la MESH esista: altrimenti il selettore
	 * scenderebbe sui figli appena caricati, il terreno toglierebbe il padre
	 * e per qualche frame non ci sarebbe niente al suo posto. Il terreno
	 * registra qui la propria risposta, e la regola anti-buchi della Fase 4
	 * torna a valere per quello che si vede davvero.
	 */
	void SetRenderReadiness(TFunction<bool(const GeoWorld::Tiles::FTileKey&)> InReadiness)
	{
		RenderReadiness = MoveTemp(InReadiness);
	}
	void ClearRenderReadiness() { RenderReadiness = nullptr; }

private:
	bool BuildViewParameters(GeoWorld::Quadtree::FViewParameters& OutView);
	void UpdateResidency(const GeoWorld::Quadtree::FViewParameters& View);
	void DrawDebugOverlay();
	void DrawSelection() const;

	UPROPERTY(Transient)
	TObjectPtr<UGeoTileStreamingSubsystem> Streaming;

	TArray<GeoWorld::Quadtree::FSelectedTile> SelectedTiles;
	GeoWorld::Quadtree::FSelectionResult Result;
	FGeoQuadtreeStats Stats;

	/**
	 * 8 pixel, non piu' 4. Con 4, alla prima prova su un portatile, il terreno
	 * chiedeva 15-18 milioni di triangoli in memoria e quasi 6 a schermo.
	 * Dimezzare la soglia quadruplica le tile: e' la manopola piu' potente
	 * che c'e', e si cambia a caldo con geo.Lod.Error o geo.Quality.
	 */
	double MaxScreenSpaceError = 8.0;

	/** Fuori dalla vista si tollera questo multiplo dell'errore. Vedi FViewParameters. */
	double OutOfViewErrorFactor = 4.0;

	/** Lo imposta il terreno: il passo della sua mesh. Vedi FViewParameters. */
	double GeometricErrorScale = 1.0;

	/** Allargamento del frustum per la sola selezione: vedi FViewParameters. */
	double FrustumMargin = 1.2;
	bool bEnabled = false;
	bool bFrozen = false;
	bool bShowDebugOverlay = false;
	bool bDrawSelection = false;

	// --- Residenza ---------------------------------------------------------
	bool bViewIndependent = true;
	bool bPrefetchEnabled = true;
	bool bPlanDirty = true;
	double LastPlanTime = 0.0;
	int32 PlanGeneration = 0;

	/** Ogni quanto ricalcolare il piano. Il disegno si rifa' a ogni frame. */
	static constexpr double PlanIntervalSeconds = 0.25;

	/**
	 * Tetto alle letture di precarico in volo. Senza, un piano da migliaia di
	 * tile riempirebbe la coda del pool, e una tile diventata urgente dovrebbe
	 * aspettare dietro a tutte: la coda ha priorita', ma una richiesta gia'
	 * accodata non si riordina.
	 */
	static constexpr int32 MaxPrefetchInFlight = 128;

	GeoWorld::Quadtree::FMotionPredictor Motion;
	GeoWorld::Quadtree::FResidencySettings ResidencySettings;
	GeoWorld::Quadtree::FResidencyPlan Plan;
	TArray<GeoWorld::Quadtree::FTileRequest> RenderRequests;
	TFunction<bool(const GeoWorld::Tiles::FTileKey&)> RenderReadiness;

	/** Ultima vista usata: serve a continuare a disegnare quando si e' congelata. */
	GeoWorld::Quadtree::FViewParameters FrozenView;
	bool bHasFrozenView = false;
};

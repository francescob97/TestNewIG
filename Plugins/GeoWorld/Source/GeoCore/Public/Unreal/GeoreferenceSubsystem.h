// =============================================================================
//  GeoreferenceSubsystem.h -- Proprietario dell'origine del mondo e del rebasing.
//  STRATO: UNREAL.
// =============================================================================
#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "Tickable.h"
#include "Misc/ScopeRWLock.h"

#include "Unreal/GeoreferenceSnapshot.h"
#include "Unreal/GeoWorldTypes.h"

#include "GeoreferenceSubsystem.generated.h"

class UGeoTransformComponent;

/** Notifica di rebase avvenuto. Delegate C++ (non dynamic): piu' veloce e non
 *  richiede che il parametro sia una USTRUCT riflessa. */
DECLARE_MULTICAST_DELEGATE_OneParam(FOnGeoreferenceRebased, const FGeoreferenceSnapshot& /*NewSnapshot*/);

/**
 * =============================================================================
 *  PERCHE' UN UWorldSubsystem
 * =============================================================================
 *  Un "subsystem" in UE5 e' un singleton con ciclo di vita gestito dal motore:
 *  niente attore da piazzare nel livello, niente pattern singleton fatto a mano,
 *  niente rischio di dimenticarselo in una mappa. Si ottiene ovunque con
 *      GetWorld()->GetSubsystem<UGeoreferenceSubsystem>()
 *
 *  Esistono quattro famiglie, e la scelta non e' indifferente:
 *    UEngineSubsystem        -> uno per processo. Troppo ampio: PIE e editor
 *                               condividerebbero la stessa origine.
 *    UGameInstanceSubsystem  -> uno per sessione di gioco. Non esiste
 *                               nell'editor fuori dal Play.
 *    ULocalPlayerSubsystem   -> uno per giocatore locale. Sbagliato: l'origine
 *                               e' una proprieta' del mondo, non di chi guarda.
 *    UWorldSubsystem         -> uno per UWorld. <-- il nostro caso.
 *
 *  E' quello giusto perche' l'editor, il PIE e ogni preview hanno UWorld
 *  distinti, e ognuno deve avere la propria origine indipendente. Se cosi' non
 *  fosse, aprire il PIE mentre si guarda il viewport dell'editor farebbe
 *  saltare l'origine sotto i piedi all'altro.
 *
 * =============================================================================
 *  PERCHE' ANCHE FTickableGameObject
 * =============================================================================
 *  I subsystem NON hanno un tick nativo. Ereditando anche da FTickableGameObject
 *  si ottiene un Tick(float) per frame senza dover creare un attore fittizio
 *  solo per avere un tick. E' il modo idiomatico in UE5.
 *
 *  Sul TIMING: i FTickableGameObject vengono aggiornati da UWorld::Tick. Noi
 *  leggiamo la posizione della camera cosi' com'e' in quel momento, che in
 *  pratica e' quella calcolata nel frame precedente. E' deliberato e innocuo:
 *  con una soglia di 10 km, un frame di latenza corrisponde a un errore di
 *  posizione che nessuna camera riesce a produrre; in cambio, tutto il resto
 *  del frame vede un'origine gia' stabile e coerente.
 */
UCLASS()
class GEOCORE_API UGeoreferenceSubsystem : public UWorldSubsystem, public FTickableGameObject
{
	GENERATED_BODY()

public:
	using FGeodetic = GeoWorld::Core::FGeodetic;
	using FEcef     = GeoWorld::Core::FEcef;

	// --- UWorldSubsystem ---------------------------------------------------

	/**
	 * NOTA UE: senza questo filtro il subsystem verrebbe creato anche per i
	 * mondi "di servizio" che Unreal crea in continuazione (preview delle
	 * miniature degli asset, anteprime dei materiali, mondi di importazione).
	 * Sono decine, e ognuno pagherebbe un tick inutile.
	 */
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	// --- FTickableGameObject ----------------------------------------------

	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual bool IsTickable() const override;
	/** Deve ticchettare anche fuori dal Play, altrimenti nell'editor il terreno
	 *  non si aggiorna finche' non premi Play: trappola classica. */
	virtual bool IsTickableInEditor() const override { return true; }

	// --- API pubblica ------------------------------------------------------

	/**
	 * Copia dello stato corrente. THREAD-SAFE: e' cosi' che i worker thread
	 * ottengono la georeferenziazione, senza mai toccare questo UObject
	 * (gli UObject non sono thread-safe e il GC puo' muoverli).
	 */
	FGeoreferenceSnapshot GetSnapshot() const;

	/** Origine corrente, in gradi. */
	FGeoCoordinate GetOriginCoordinate() const;

	/** Cambia origine adesso, riposizionando tutto ciò che e' georeferenziato. */
	void SetOrigin(const FGeodetic& NewOrigin);

	/** Numero di rebase avvenuti da quando il mondo e' stato creato. */
	uint32 GetRebaseCount() const { return RebaseCount; }

	/** Soglia di rebasing corrente, in metri. */
	double GetRebaseThresholdMeters() const { return RebaseThresholdMeters; }
	void   SetRebaseThresholdMeters(double NewThresholdMeters);

	bool IsAutoRebaseEnabled() const { return bAutoRebase; }
	void SetAutoRebaseEnabled(bool bEnabled) { bAutoRebase = bEnabled; }

	/** Invocato DOPO ogni rebase, a origine gia' aggiornata. */
	FOnGeoreferenceRebased OnGeoreferenceRebased;

	// --- Registro dei componenti georeferenziati ---------------------------
	//
	// I componenti si iscrivono in OnRegister e si cancellano in OnUnregister.
	// Usiamo TWeakObjectPtr e non puntatori nudi perche' il garbage collector
	// di Unreal puo' distruggere un oggetto in qualunque momento: una weak
	// pointer diventa nullptr invece che pendente, e il ciclo di notifica
	// compatta l'array scartando le voci morte.
	//
	// NOTA DI SCALA: questo registro e' pensato per POCHE decine di oggetti
	// (marker, attori posizionati a mano). Il terreno della Fase 4 NON si
	// registrera' tile per tile: passera' il quadtree con una sola voce, perche'
	// iterare cinquemila componenti a ogni rebase e' spreco inutile.

	void RegisterGeoComponent(UGeoTransformComponent* Component);
	void UnregisterGeoComponent(UGeoTransformComponent* Component);
	int32 GetRegisteredComponentCount() const { return RegisteredComponents.Num(); }

	// --- Debug -------------------------------------------------------------

	void SetDebugOverlayEnabled(bool bEnabled) { bShowDebugOverlay = bEnabled; }
	bool IsDebugOverlayEnabled() const { return bShowDebugOverlay; }

	/**
	 * Posizione della camera attiva in spazio mondo.
	 * Ritorna false se non c'e' nessuna camera (es. mondo appena creato).
	 * Incapsula la differenza fra gioco (PlayerCameraManager) ed editor
	 * (viewport, che NON e' un attore).
	 */
	bool GetActiveViewLocation(FVector& OutLocation) const;

	/** Teletrasporta la camera attiva su un punto geografico ("geo.Goto"). */
	bool TeleportViewTo(const FGeodetic& Destination);

private:
	void ApplyRebase(const FGeodetic& NewOrigin);
	void RefreshRegisteredComponents();
	void DrawDebugOverlay() const;

	/**
	 * Snapshot corrente.
	 * Scritture e letture dal GAME THREAD possono accedervi direttamente: sono
	 * tutte sullo stesso thread e non si possono sovrapporre. Il lock qui sotto
	 * esiste per i WORKER THREAD, che devono passare da GetSnapshot().
	 */
	FGeoreferenceSnapshot Snapshot;

	/**
	 * NOTA UE: FRWLock e' un lock lettori/scrittore. Le letture (GetSnapshot,
	 * chiamata dai worker) non si bloccano fra loro; solo il rebase, che e' raro
	 * e avviene sul game thread, prende il lock in scrittura.
	 * "mutable" perche' GetSnapshot() e' const ma deve comunque prendere il lock.
	 */
	mutable FRWLock SnapshotLock;

	UPROPERTY(Transient)
	TArray<TWeakObjectPtr<UGeoTransformComponent>> RegisteredComponents;

	double RebaseThresholdMeters   = 10000.0;
	int32  MinFramesBetweenRebases = 10;
	int32  FramesSinceLastRebase   = 0;
	uint32 RebaseCount             = 0;
	bool   bAutoRebase             = true;
	bool   bMoveViewTargetOnRebase = true;
	bool   bShowDebugOverlay       = false;

	/** Statistiche per l'HUD. */
	double LastRebaseDistanceMeters = 0.0;
};

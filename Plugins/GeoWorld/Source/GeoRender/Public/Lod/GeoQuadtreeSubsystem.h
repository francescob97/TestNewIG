// =============================================================================
//  GeoQuadtreeSubsystem.h -- Selezione LOD per frame. STRATO: UNREAL.
// =============================================================================
#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"

#include "Quadtree/QuadtreeTypes.h"
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

private:
	bool BuildViewParameters(GeoWorld::Quadtree::FViewParameters& OutView);
	void DrawDebugOverlay();
	void DrawSelection() const;

	UPROPERTY(Transient)
	TObjectPtr<UGeoTileStreamingSubsystem> Streaming;

	TArray<GeoWorld::Quadtree::FSelectedTile> SelectedTiles;
	GeoWorld::Quadtree::FSelectionResult Result;
	FGeoQuadtreeStats Stats;

	double MaxScreenSpaceError = 4.0;

	/** Allargamento del frustum per la sola selezione: vedi FViewParameters. */
	double FrustumMargin = 1.2;
	bool bEnabled = false;
	bool bFrozen = false;
	bool bShowDebugOverlay = false;
	bool bDrawSelection = false;

	/** Ultima vista usata: serve a continuare a disegnare quando si e' congelata. */
	GeoWorld::Quadtree::FViewParameters FrozenView;
	bool bHasFrozenView = false;
};

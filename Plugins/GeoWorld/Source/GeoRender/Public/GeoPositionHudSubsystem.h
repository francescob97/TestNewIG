// =============================================================================
//  GeoPositionHudSubsystem.h -- Dove sono: latitudine, longitudine, quota.
//  STRATO: UNREAL.
//
//  Un riquadro sulla destra dello schermo, sempre acceso (geo.Hud 0 lo spegne):
//
//      45,07031° N    7,68620° E
//      45°04'13,1" N  7°41'10,3" E
//      Quota       1.234 m  ellissoide
//      s.l.m.      1.186 m  (se c'e' il geoide: dataset dell'acqua)
//      Dal suolo     842 m  (se le quote sotto sono in memoria)
//      Rotta        273°
//
//  PERCHE' NON AddOnScreenDebugMessage come gli overlay. Quei messaggi stanno
//  tutti a SINISTRA, in colonna, e si mescolano con gli overlay dei subsystem.
//  Qui si disegna direttamente sul Canvas del viewport, attraverso
//  UDebugDrawService: il motore chiama una funzione nostra a ogni frame, per
//  ogni viewport, con il Canvas gia' pronto. Non serve un HUD di gioco, quindi
//  funziona anche nel viewport dell'editor, senza Play.
//
//  QUALE POSIZIONE. Quella della VISTA che si sta disegnando (FSceneView), non
//  "la camera del giocatore": nell'editor con il Play aperto ci sono due
//  viewport, e ognuno deve mostrare la propria.
// =============================================================================
#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"

#include "GeoPositionHudSubsystem.generated.h"

class APlayerController;
class UCanvas;

UCLASS()
class GEORENDER_API UGeoPositionHudSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	void SetEnabled(bool bInEnabled) { bEnabled = bInEnabled; }
	bool IsEnabled() const { return bEnabled; }

private:
	/** Chiamata dal motore per ogni viewport, a ogni frame. */
	void Draw(UCanvas* Canvas, APlayerController* Controller);

	/** Quota del terreno DISEGNATO sotto il punto, se le quote sono in memoria. */
	bool GroundHeightAt(double LonDeg, double LatDeg, double& OutHeight) const;

	FDelegateHandle GameDrawHandle;
	FDelegateHandle EditorDrawHandle;

	/** Lo stesso viewport puo' avere sia il canale "Game" sia "Editor": si disegna una volta. */
	uint64 LastDrawnFrame = 0;
	const void* LastDrawnView = nullptr;

	bool bEnabled = true;
};

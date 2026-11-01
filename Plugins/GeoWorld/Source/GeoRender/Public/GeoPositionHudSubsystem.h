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
//      Vento      7,2 m/s (14 kt) da 245°   alla quota della vista (Weather)
//      Raffiche   11,3 m/s, adesso 8,1, verticale +0,4 m/s
//      A 10 m     4,1 m/s (8 kt) da 232°    sotto di te, come una stazione meteo
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

/**
 * Le righe del riquadro che arrivano da moduli che GeoRender non conosce
 * (GeoEntities dipende da GeoRender, non il contrario): si aggiungono qui.
 * Ogni funzione riceve il mondo e il punto della vista, e aggiunge le sue
 * coppie (etichetta, valore).
 */
using FGeoHudLines = TArray<TPair<FString, FString>>;
// NOTA: il tipo con la virgola (TPair<FString, FString>) passa da un alias:
// dentro la macro la virgola spezzerebbe gli argomenti.
DECLARE_MULTICAST_DELEGATE_ThreeParams(FGeoHudExtraLines, const UWorld* /*World*/, const FVector& /*ViewLocation*/,
	FGeoHudLines& /*Lines*/);

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

	/**
	 * Quota (ellissoidica) del terreno DISEGNATO sotto il punto, se le quote
	 * sono in memoria; al largo, il mare (il geoide). Pubblica: la usano anche
	 * le entita' (GeoEntities) per appoggiare i veicoli al suolo che si vede.
	 */
	static bool GroundHeightAt(const UWorld* World, double LonDeg, double LatDeg, double& OutHeight);

	/** Le righe degli altri moduli (vedi FGeoHudExtraLines). */
	static FGeoHudExtraLines& ExtraLines();

private:
	/** Chiamata dal motore per ogni viewport, a ogni frame. */
	void Draw(UCanvas* Canvas, APlayerController* Controller);

	FDelegateHandle GameDrawHandle;
	FDelegateHandle EditorDrawHandle;

	/** Lo stesso viewport puo' avere sia il canale "Game" sia "Editor": si disegna una volta. */
	uint64 LastDrawnFrame = 0;
	const void* LastDrawnView = nullptr;

	bool bEnabled = true;
};

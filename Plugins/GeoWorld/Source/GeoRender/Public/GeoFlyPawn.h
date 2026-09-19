// =============================================================================
//  GeoFlyPawn.h -- Camera di volo per navigare il pianeta. STRATO: UNREAL.
// =============================================================================
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/DefaultPawn.h"

#include "GeoFlyPawn.generated.h"

/**
 * =============================================================================
 *  PERCHE' NON BASTA IL PAWN DI DEFAULT
 * =============================================================================
 *  ADefaultPawn vola a 1200 unita' al secondo. Con la convenzione del progetto
 *  (1 uu = 1 cm) sono 12 metri al secondo: per attraversare i 20 km che separano
 *  due tile di livello 9 servono quasi tre minuti. Su scala planetaria un pawn
 *  a velocita' fissa e' inutilizzabile in entrambe le direzioni: o e' lento da
 *  continente, o e' ingovernabile da valle.
 *
 * =============================================================================
 *  LA REGOLA: LA VELOCITA' SEGUE LA QUOTA
 * =============================================================================
 *  E' la stessa scelta di Google Earth e Cesium, e non e' un trucco estetico:
 *  quello che conta per chi guarda non e' la velocita' in metri al secondo, ma
 *  quanto in fretta cambia l'inquadratura. A 100 m di quota inquadri un isolato,
 *  a 100 km una regione, e per percorrerli "alla stessa andatura percepita" la
 *  velocita' deve essere proporzionale alla quota.
 *
 *  Il fattore e' mezzo: in un secondo percorri mezza volta la tua quota. A 200 m
 *  sono 360 km/h, a 15 km sono 27.000 km/h. Sotto e sopra si satura, perche'
 *  vicino al suolo la proporzionalita' porterebbe a zero e in orbita a numeri
 *  senza senso.
 *
 *  NOTA IMPORTANTE: la quota e' quella sull'ELLISSOIDE, non sul terreno. Sopra
 *  una vetta di 4000 m la camera va piu' veloce di quanto la distanza dal suolo
 *  giustificherebbe. Correggerlo richiederebbe campionare la quota del terreno
 *  sotto la camera a ogni frame, cioe' far dipendere il movimento dalla cache
 *  delle tile: si puo' fare, ma non prima che dia fastidio davvero.
 */
UCLASS(ClassGroup = (GeoWorld))
class GEORENDER_API AGeoFlyPawn : public ADefaultPawn
{
	GENERATED_BODY()

public:
	AGeoFlyPawn();

	virtual void Tick(float DeltaSeconds) override;

	/** Moltiplicatore manuale sopra la velocita' calcolata dalla quota. */
	void SetSpeedMultiplier(float InMultiplier);
	float GetSpeedMultiplier() const { return SpeedMultiplier; }

	/** Velocita' massima corrente, in km/h: e' cio' che si mostra a schermo. */
	float GetCurrentSpeedKmh() const { return CurrentSpeedKmh; }

	/** Quota usata per calcolarla, in metri sull'ellissoide. */
	double GetCurrentHeightM() const { return CurrentHeightM; }

private:
	float SpeedMultiplier = 1.0f;
	float CurrentSpeedKmh = 0.0f;
	double CurrentHeightM = 0.0;
};

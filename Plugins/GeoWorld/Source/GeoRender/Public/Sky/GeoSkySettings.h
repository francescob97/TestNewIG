// =============================================================================
//  GeoSkySettings.h -- Il cielo nelle impostazioni del progetto. STRATO: UNREAL.
//
//  Project Settings -> Plugins -> "GeoWorld - Cielo": a che ora si comincia,
//  quanto corre il tempo, il fuso, quanto e' chiara la notte. I comandi
//  geo.Sky.* cambiano gli stessi valori mentre il mondo gira (senza salvarli).
//
//  Config = Game, DefaultConfig: Config/DefaultGame.ini, nel progetto.
// =============================================================================
#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"

#include "GeoSkySettings.generated.h"

UCLASS(Config = Game, DefaultConfig, meta = (DisplayName = "GeoWorld - Cielo"))
class GEORENDER_API UGeoSkySettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	virtual FName GetCategoryName() const override { return FName(TEXT("Plugins")); }

	/** Il cielo muove il sole del livello (spento: il livello resta com'e'). */
	UPROPERTY(Config, EditAnywhere, Category = "Cielo")
	bool bEnabled = true;

	/** Si comincia dall'ora vera (adesso), invece che da StartHour di oggi. */
	UPROPERTY(Config, EditAnywhere, Category = "Tempo")
	bool bStartAtRealTime = false;

	/** L'ora locale da cui si comincia, oggi (10,5 = le 10:30). */
	UPROPERTY(Config, EditAnywhere, Category = "Tempo", meta = (ClampMin = "0", ClampMax = "24"))
	float StartHour = 10.5f;

	/** Quanto corre il tempo: 0 fermo, 1 come l'orologio, 60 un minuto al secondo. */
	UPROPERTY(Config, EditAnywhere, Category = "Tempo")
	float Speed = 0.0f;

	/** L'ora dell'Europa centrale con l'ora legale (Italia). Spento: FixedUtcOffsetHours. */
	UPROPERTY(Config, EditAnywhere, Category = "Tempo")
	bool bCentralEuropeTime = true;

	/** Lo scostamento da UT, ore, se non e' l'ora dell'Europa centrale. */
	UPROPERTY(Config, EditAnywhere, Category = "Tempo")
	float FixedUtcOffsetHours = 0.0f;

	/**
	 * La luce del sole, lux. 0: quella che ha la luce del livello quando il
	 * cielo la prende (il valore su cui il livello e' tarato).
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Luce")
	float SunIlluminanceLux = 0.0f;

	/** La luna piena rispetto al sole (dal vero 1/400.000: troppo poco per l'esposizione di un gioco). */
	UPROPERTY(Config, EditAnywhere, Category = "Luce", meta = (ClampMin = "0.00001", ClampMax = "0.1"))
	float MoonLightRatio = 0.001f;

	/** Di notte l'esposizione scende di tanti stop: la notte resta notte invece di una giornata grigia. */
	UPROPERTY(Config, EditAnywhere, Category = "Luce", meta = (ClampMin = "-8", ClampMax = "0"))
	float NightExposureBias = -2.0f;

	/** Stelle, Via Lattea e la luna con le fasi (M_GeoNightSky). */
	UPROPERTY(Config, EditAnywhere, Category = "Notte")
	bool bNightSky = true;

	/** Le finestre degli edifici accese di notte. */
	UPROPERTY(Config, EditAnywhere, Category = "Notte")
	bool bCityLights = true;

	/**
	 * Senza le Virtual Shadow Maps, fin dove arrivano le ombre del sole (m):
	 * il default di Unreal per una luce mobile e' 200 m, troppo poco per una
	 * citta' vista dall'alto. Con le Virtual Shadow Maps non serve.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Luce", meta = (ClampMin = "100", ClampMax = "50000"))
	float ShadowDistanceM = 3000.0f;
};

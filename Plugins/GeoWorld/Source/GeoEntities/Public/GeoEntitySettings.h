// =============================================================================
//  GeoEntitySettings.h -- Project Settings -> Plugins -> GeoWorld - Entita'.
//  STRATO: UNREAL.
//
//  Le stesse cose dei comandi geo.Entities.*, scritte una volta per tutte
//  (Config/DefaultGame.ini). I comandi le cambiano a caldo, per la sessione.
// =============================================================================
#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "Engine/EngineTypes.h"

#include "GeoEntitySettings.generated.h"

UCLASS(Config = Game, DefaultConfig, meta = (DisplayName = "GeoWorld - Entita'"))
class GEOENTITIES_API UGeoEntitySettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	virtual FName GetCategoryName() const override { return FName(TEXT("Plugins")); }

	/** All'avvio del Play si mette subito in ascolto della memoria condivisa (come geo.Entities.Listen). */
	UPROPERTY(Config, EditAnywhere, Category = "Sorgenti")
	bool bListenOnStart = false;

	/** Il nome della memoria condivisa: quello di `geoworld demo-entities --name`. */
	UPROPERTY(Config, EditAnywhere, Category = "Sorgenti")
	FString SharedMemoryName = TEXT("GeoWorldEntities");

	/**
	 * La cartella dei modelli (fuori dal progetto va benissimo): i file delle
	 * regole della tabella (F16/f16.flt) partono da qui, e le texture che un
	 * modello nomina con il percorso di un'altra macchina si cercano qui per
	 * nome. Come geo.Entities.ModelFolder.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Modelli")
	FDirectoryPath ModelFolder;

	/**
	 * La tabella dei modelli (Entities/EntityModels.h): una regola per riga,
	 * "1.2.*.21  heli 12 11 3.5". Vuota: le forme di serie, una per dominio.
	 * Un percorso relativo e' dalla cartella del progetto.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Modelli", meta = (FilePathFilter = "txt"))
	FFilePath ModelTable;

	/** Fin dove un'entita' ha un attore (m). Oltre sta nel registro, ma non si disegna. */
	UPROPERTY(Config, EditAnywhere, Category = "Disegno", meta = (ClampMin = "100", ClampMax = "500000"))
	float ActorRadiusM = 30000.0f;

	/** Al massimo tanti attori, i piu' vicini (gli attori costano, i dati no: sezione 11). */
	UPROPERTY(Config, EditAnywhere, Category = "Disegno", meta = (ClampMin = "1", ClampMax = "10000"))
	int32 MaxActors = 400;

	/** Il nome sopra ogni entita'. */
	UPROPERTY(Config, EditAnywhere, Category = "Disegno")
	bool bLabels = true;

	/** Quanto dura il rientro su un aggiornamento nuovo (s). 0: si salta subito. */
	UPROPERTY(Config, EditAnywhere, Category = "Estrapolazione", meta = (ClampMin = "0", ClampMax = "5"))
	float BlendSeconds = 0.4f;

	/** Chi tace da tanto se ne va (s): il battito di DIS. */
	UPROPERTY(Config, EditAnywhere, Category = "Estrapolazione", meta = (ClampMin = "0.5", ClampMax = "600"))
	float TimeoutSeconds = 5.0f;
};

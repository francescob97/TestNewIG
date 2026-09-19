// =============================================================================
//  GeoWorldSettings.h -- Impostazioni di progetto del plugin.
//  STRATO: UNREAL.
// =============================================================================
#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "Georeference/GeoWorldTypes.h"

#include "GeoWorldSettings.generated.h"

/**
 * NOTA UE -- perche' UDeveloperSettings:
 * derivando da UDeveloperSettings, questa classe compare AUTOMATICAMENTE in
 * Project Settings (sotto la categoria scelta da GetCategoryName) senza
 * scrivere una riga di Slate/UI, e le proprieta' marcate "config" vengono
 * serializzate da sole nel .ini indicato dal meta-specificatore della UCLASS.
 *
 *   config = Game      -> scrive in Config/DefaultGame.ini
 *   defaultconfig      -> salva nel .ini del PROGETTO (condiviso, va in git)
 *                         invece che in quello dell'utente locale. E' cio' che
 *                         vogliamo per impostazioni che definiscono il mondo.
 *
 * Si legge da qualunque punto del codice con
 *     GetDefault<UGeoWorldSettings>()
 * che restituisce il CDO (Class Default Object), cioe' l'istanza-prototipo che
 * Unreal crea per ogni classe. Non serve (e non si deve) istanziarla.
 */
UCLASS(config = Game, defaultconfig, meta = (DisplayName = "GeoWorld"))
class GEOCORE_API UGeoWorldSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UGeoWorldSettings();

	/** Fa comparire la voce sotto "Plugins" invece che fra le sezioni di motore. */
	virtual FName GetCategoryName() const override { return FName(TEXT("Plugins")); }

	// --- Origine ----------------------------------------------------------

	/** Origine geodetica all'avvio, prima che il rebasing entri in gioco. */
	UPROPERTY(config, EditAnywhere, Category = "Georeference")
	FGeoCoordinate InitialOrigin = FGeoCoordinate(41.890210, 12.492231, 40.0);   // Colosseo

	// --- Rebasing ---------------------------------------------------------

	/**
	 * Quando la camera supera questa distanza dall'origine corrente, l'origine
	 * viene ricalcolata sulla posizione della camera.
	 *
	 * Come si sceglie il valore: entro 10 km un float in centimetri ha un ULP
	 * di 0.625 mm (misurato dal test 8 dell'harness standalone), piu' che
	 * sufficiente per i vertici delle mesh e per la fisica. A distanza del
	 * geocentro lo stesso float avrebbe un ULP di 64 cm.
	 * Alzarlo riduce la frequenza dei rebase, abbassarlo aumenta la precisione
	 * dei sistemi che lavorano in singola precisione.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Georeference|Rebasing",
		meta = (ClampMin = "0.1", UIMin = "1.0", UIMax = "100.0", Units = "Kilometers"))
	double RebaseThresholdKm = 10.0;

	/**
	 * Numero minimo di frame fra due rebase consecutivi.
	 *
	 * E' l'isteresi che impedisce il "thrashing": senza, una camera che oscilla
	 * esattamente attorno alla soglia farebbe scattare un rebase a ogni frame.
	 * Nella pratica la nuova origine coincide con la posizione della camera,
	 * quindi la distanza riparte da zero e il problema e' gia' quasi risolto;
	 * questo e' il paracadute.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Georeference|Rebasing",
		meta = (ClampMin = "0", UIMax = "120"))
	int32 MinFramesBetweenRebases = 10;

	/** Rebasing automatico attivo. Disattivarlo e' utile per isolare bug. */
	UPROPERTY(config, EditAnywhere, Category = "Georeference|Rebasing")
	bool bAutoRebase = true;

	/**
	 * Al rebase, sposta anche la camera/pawn in modo che resti nello stesso
	 * punto GEOGRAFICO.
	 *
	 * Deve restare attivo in condizioni normali: e' cio' che rende il rebase
	 * invisibile. Disattivarlo serve solo per vedere "a occhio nudo" che il
	 * rebase e' avvenuto (la camera salta), utile in fase di debug.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Georeference|Rebasing")
	bool bMoveViewTargetOnRebase = true;

	// --- Debug ------------------------------------------------------------

	/** HUD di debug attivo all'avvio (equivale a "geo.Debug 1"). */
	UPROPERTY(config, EditAnywhere, Category = "Debug")
	bool bShowDebugOverlay = false;
};

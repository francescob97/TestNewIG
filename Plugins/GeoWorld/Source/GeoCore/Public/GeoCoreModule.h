#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

/**
 * NOTA UE: ogni modulo ha una classe che implementa IModuleInterface, con
 * StartupModule/ShutdownModule chiamate quando la DLL viene caricata e
 * scaricata. Se non serve fare nulla si usa FDefaultModuleImpl; qui ci serve
 * per registrare le console command e loggare la disponibilita' del modulo.
 */
class FGeoCoreModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
};

/**
 * Categoria di log dedicata: nell'Output Log si filtra per "LogGeoWorld" invece
 * di pescare i nostri messaggi in mezzo a LogTemp.
 *
 * NOTA UE: il prefisso GEOCORE_API e' indispensabile. Ogni modulo e' una DLL
 * separata, e senza il macro di export la categoria non e' visibile agli altri
 * moduli: il codice compila e poi il LINKER fallisce con un errore di simbolo
 * non risolto, che e' un messaggio molto meno chiaro della causa. E' lo stesso
 * schema che usa il motore in CoreGlobals.h (CORE_API DECLARE_LOG_CATEGORY_EXTERN...).
 */
GEOCORE_API DECLARE_LOG_CATEGORY_EXTERN(LogGeoWorld, Log, All);

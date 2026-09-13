#include "Unreal/GeoWorldSettings.h"

UGeoWorldSettings::UGeoWorldSettings()
{
	// NOTA UE: i valori veri stanno negli inizializzatori di default in
	// GeoWorldSettings.h. Questo costruttore serve perche' Unreal costruisce il
	// CDO (Class Default Object) all'avvio, e perche' UDeveloperSettings legge
	// il .ini DOPO la costruzione: qualunque valore assegnato qui verrebbe
	// comunque sovrascritto da Config/DefaultGame.ini, se la chiave esiste.
	// E' l'ordine giusto: il .ini vince sempre sul codice.
	CategoryName = FName(TEXT("Plugins"));
	SectionName  = FName(TEXT("GeoWorld"));
}

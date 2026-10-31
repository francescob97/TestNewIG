// =============================================================================
//  GeoBuildingSettings.h -- La libreria dei MONUMENTI, nelle impostazioni del
//  progetto. STRATO: UNREAL.
//
//  Project Settings -> Plugins -> "GeoWorld - Edifici": una lista. Ogni voce
//  dice quale edificio di OpenStreetMap sostituire con un modello (di solito
//  Nanite, fatto o comprato a parte):
//
//    Id       "Q12345" (Wikidata: il tag wikidata=* dell'edificio su
//             OpenStreetMap, o la pagina del monumento su wikidata.org),
//             oppure "way/123456" o "relation/987" (l'id OSM: si legge su
//             openstreetmap.org cliccando l'edificio);
//    Model    la static mesh;
//    ...      rotazione, quota, e se adattarla alla sagoma.
//
//  Al posto della sagoma estrusa compare il modello, a terra sul baricentro
//  della sagoma, girato come il suo lato lungo (piu' YawDegrees). Il modello
//  si usa alla sua grandezza vera (gli asset sono in centimetri), a meno di
//  bFitToFootprint.
//
//  Config = Game, DefaultConfig: Config/DefaultGame.ini, nel progetto.
// =============================================================================
#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "UObject/SoftObjectPtr.h"

#include "GeoBuildingSettings.generated.h"

class UStaticMesh;

USTRUCT()
struct GEORENDER_API FGeoLandmark
{
	GENERATED_BODY()

	/** "Q12345" (Wikidata), "way/123456" o "relation/987" (OpenStreetMap). */
	UPROPERTY(Config, EditAnywhere, Category = "Monumento")
	FString Id;

	UPROPERTY(Config, EditAnywhere, Category = "Monumento")
	TSoftObjectPtr<UStaticMesh> Model;

	/** Rotazione in piu' rispetto al lato lungo della sagoma, gradi (verso est). */
	UPROPERTY(Config, EditAnywhere, Category = "Monumento")
	float YawDegrees = 0.0f;

	/** Quota in piu', m: per un modello con la base sotto o sopra il suo zero. */
	UPROPERTY(Config, EditAnywhere, Category = "Monumento")
	float HeightOffsetM = 0.0f;

	/** Adatta il modello alla sagoma (lato lungo e lato corto) invece di usarlo alla sua grandezza. */
	UPROPERTY(Config, EditAnywhere, Category = "Monumento")
	bool bFitToFootprint = false;
};

UCLASS(Config = Game, DefaultConfig, meta = (DisplayName = "GeoWorld - Edifici"))
class GEORENDER_API UGeoBuildingSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	virtual FName GetCategoryName() const override { return FName(TEXT("Plugins")); }

	UPROPERTY(Config, EditAnywhere, Category = "Monumenti")
	TArray<FGeoLandmark> Landmarks;

	/**
	 * L'id di una voce come lo scrivono le tile: Wikidata (OutWikidata) o OSM
	 * con il tipo nei 2 bit alti (OutOsmId). Falso se non si capisce.
	 */
	static bool ParseId(const FString& Id, uint64& OutOsmId, uint32& OutWikidata);
};

// =============================================================================
//  GeoVegetationSettings.h -- La libreria dei modelli degli alberi, nelle
//  impostazioni del progetto. STRATO: UNREAL.
//
//  Project Settings -> Plugins -> "GeoWorld - Vegetazione": otto caselle, una
//  per forma (quattro specie, due varianti). Ognuna punta a una static mesh.
//  Di default puntano ai modelli Nanite che geo.Vegetation.CreateModels salva
//  in /GeoWorld/Vegetation. Si puo' metterci un modello proprio (un abete di
//  Quixel, una quercia comprata): conta solo che stia in piedi (la base in
//  basso, l'alto su Z); la grandezza la adatta il runtime, albero per albero,
//  dai bounds del modello.
//
//  Una casella vuota, o un modello che non c'e', vuol dire "la forma fatta dal
//  codice" (TreeMesh.h, due LOD, non Nanite): gli alberi ci sono comunque.
//
//  NOTA UE: UDeveloperSettings e' la classe delle impostazioni che compaiono
//  da sole in Project Settings. "Config = Game, DefaultConfig" le salva in
//  Config/DefaultGame.ini, cioe' nel progetto (e nel repository), non nelle
//  preferenze di chi lo apre.
// =============================================================================
#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "UObject/SoftObjectPtr.h"

#include "GeoVegetationSettings.generated.h"

class UStaticMesh;

UCLASS(Config = Game, DefaultConfig, meta = (DisplayName = "GeoWorld - Vegetazione"))
class GEORENDER_API UGeoVegetationSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UGeoVegetationSettings();

	virtual FName GetCategoryName() const override { return FName(TEXT("Plugins")); }

	/** Il modello di una forma (specie 0..3 come ESpecies, variante 0..1). */
	TSoftObjectPtr<UStaticMesh> GetModel(int32 Species, int32 Variant) const;

	UPROPERTY(Config, EditAnywhere, Category = "Modelli", meta = (DisplayName = "Latifoglie, forma 1"))
	TSoftObjectPtr<UStaticMesh> Broadleaf0;
	UPROPERTY(Config, EditAnywhere, Category = "Modelli", meta = (DisplayName = "Latifoglie, forma 2"))
	TSoftObjectPtr<UStaticMesh> Broadleaf1;
	UPROPERTY(Config, EditAnywhere, Category = "Modelli", meta = (DisplayName = "Conifere, forma 1"))
	TSoftObjectPtr<UStaticMesh> Conifer0;
	UPROPERTY(Config, EditAnywhere, Category = "Modelli", meta = (DisplayName = "Conifere, forma 2"))
	TSoftObjectPtr<UStaticMesh> Conifer1;
	UPROPERTY(Config, EditAnywhere, Category = "Modelli", meta = (DisplayName = "Pini domestici, forma 1"))
	TSoftObjectPtr<UStaticMesh> UmbrellaPine0;
	UPROPERTY(Config, EditAnywhere, Category = "Modelli", meta = (DisplayName = "Pini domestici, forma 2"))
	TSoftObjectPtr<UStaticMesh> UmbrellaPine1;
	UPROPERTY(Config, EditAnywhere, Category = "Modelli", meta = (DisplayName = "Arbusti, forma 1"))
	TSoftObjectPtr<UStaticMesh> Shrub0;
	UPROPERTY(Config, EditAnywhere, Category = "Modelli", meta = (DisplayName = "Arbusti, forma 2"))
	TSoftObjectPtr<UStaticMesh> Shrub1;

	/**
	 * Con un modello proprio, il materiale e' il suo (con i suoi colori, e
	 * senza il vento e la neve di M_GeoVegetation, se non li ha). Acceso, il
	 * runtime mette M_GeoVegetation anche sui modelli propri: serve solo se
	 * il modello ha il colore dei vertici come TreeMesh.h.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Modelli", meta = (DisplayName = "M_GeoVegetation anche sui modelli propri"))
	bool bForceVegetationMaterial = false;
};

// =============================================================================
//  GeoTransformComponent.h -- Ancora un attore a una coordinata geografica.
//  STRATO: UNREAL.
// =============================================================================
#pragma once

#include "CoreMinimal.h"
#include "Components/SceneComponent.h"
#include "Unreal/GeoWorldTypes.h"

#include "GeoTransformComponent.generated.h"

class UGeoreferenceSubsystem;

/**
 * Tiene l'attore proprietario nel punto geografico indicato, qualunque sia
 * l'origine corrente del mondo.
 *
 * =============================================================================
 *  PERCHE' UN USceneComponent E NON UN ATTORE
 * =============================================================================
 *  - si attacca a QUALUNQUE attore esistente, senza imporre una classe base;
 *  - eredita gratis il gizmo di trasformazione dell'editor;
 *  - soprattutto: ha la coppia GARANTITA OnRegister / OnUnregister, che e' il
 *    posto corretto dove iscriversi e cancellarsi dal subsystem.
 *
 *  NOTA UE IMPORTANTE: in Unreal NON si usano costruttore e distruttore C++ per
 *  questo genere di cose. Gli oggetti vengono costruiti anche come CDO (il
 *  prototipo della classe), duplicati all'avvio del PIE, ricaricati dall'hot
 *  reload e distrutti dal garbage collector in momenti che non controlli.
 *  OnRegister/OnUnregister invece corrispondono esattamente a "il componente e'
 *  entrato / uscito dal mondo", che e' cio' che ci interessa.
 */
UCLASS(ClassGroup = (GeoWorld), meta = (BlueprintSpawnableComponent),
	HideCategories = (Transform))
class GEOCORE_API UGeoTransformComponent : public USceneComponent
{
	GENERATED_BODY()

public:
	UGeoTransformComponent();

	/** Posizione geografica dell'attore. E' l'UNICA autorita': la posizione in
	 *  unita' Unreal e' sempre un valore derivato da questa. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GeoWorld")
	FGeoCoordinate Coordinate = FGeoCoordinate(41.890210, 12.492231, 40.0);

	/**
	 * Orienta l'attore secondo la verticale locale: Z verso l'alto locale,
	 * X verso il Nord locale, Y verso l'Est locale.
	 *
	 * Se disattivato, l'attore mantiene la rotazione che ha, il che a 500 km
	 * dall'origine significa apparire inclinato di ~4.5 gradi rispetto al
	 * terreno sotto di lui.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GeoWorld")
	bool bAlignToLocalUp = true;

	/** Ricalcola la trasformazione mondo a partire dalla coordinata geografica. */
	UFUNCTION(BlueprintCallable, Category = "GeoWorld")
	void RefreshFromGeoreference();

	/** Sposta l'attore su una nuova coordinata geografica. */
	UFUNCTION(BlueprintCallable, Category = "GeoWorld")
	void SetGeoCoordinate(const FGeoCoordinate& NewCoordinate);

	// --- USceneComponent ---------------------------------------------------
	virtual void OnRegister() override;
	virtual void OnUnregister() override;

#if WITH_EDITOR
	/** Aggiorna la posizione mentre digiti le coordinate nel pannello Details. */
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

private:
	UGeoreferenceSubsystem* GetGeoreferenceSubsystem() const;
};

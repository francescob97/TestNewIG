// =============================================================================
//  GeoMarkerActor.h -- Cubo di verifica piazzato su una coordinata geografica.
//  Serve alla verifica visiva della Fase 1: e' l'occhio che controlla i numeri.
// =============================================================================
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

#include "GeoMarkerActor.generated.h"

class UGeoTransformComponent;
class UStaticMeshComponent;

UCLASS(ClassGroup = (GeoWorld))
class GEORENDER_API AGeoMarkerActor : public AActor
{
	GENERATED_BODY()

public:
	AGeoMarkerActor();

	/** Etichetta mostrata sopra il cubo quando l'overlay di debug e' attivo. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GeoWorld")
	FString Label = TEXT("Marker");

	/** Lato del cubo in metri. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GeoWorld")
	double SizeMeters = 10.0;

	virtual void Tick(float DeltaSeconds) override;
	virtual void OnConstruction(const FTransform& Transform) override;

	UGeoTransformComponent* GetGeoTransform() const { return GeoTransform; }

private:
	/**
	 * NOTA UE: TObjectPtr<> invece del puntatore nudo.
	 * In UE5 e' la forma raccomandata per i riferimenti fra UObject: nelle build
	 * dell'editor traccia gli accessi per il lazy loading degli asset, mentre in
	 * quelle di gioco si compila esattamente come un puntatore nudo (costo zero).
	 * La UPROPERTY e' comunque obbligatoria: senza, il garbage collector non
	 * vede il riferimento e puo' distruggere il componente sotto i nostri piedi.
	 */
	UPROPERTY(VisibleAnywhere, Category = "GeoWorld")
	TObjectPtr<UGeoTransformComponent> GeoTransform;

	UPROPERTY(VisibleAnywhere, Category = "GeoWorld")
	TObjectPtr<UStaticMeshComponent> MeshComponent;
};

#include "Unreal/GeoTransformComponent.h"

#include "Unreal/GeoreferenceSubsystem.h"
#include "Unreal/GeoreferenceSnapshot.h"

#include "Engine/World.h"
#include "GameFramework/Actor.h"

UGeoTransformComponent::UGeoTransformComponent()
{
	// Non serve nessun tick: ci muoviamo solo quando l'origine cambia, e in
	// quel caso e' il subsystem a chiamarci. Un tick per frame che nel 99.99%
	// dei frame non fa nulla e' costo puro, e con migliaia di componenti si
	// vede nel profiler.
	PrimaryComponentTick.bCanEverTick = false;

	// ------------------------------------------------------------------
	//  TRAPPOLA UE DA CONOSCERE: la Mobility.
	//
	//  Un componente Static NON puo' essere spostato a runtime: Unreal assume
	//  che sia immobile per il precomputed lighting e per ottimizzazioni della
	//  scena, e spostarlo produce warning o comportamenti indefiniti.
	//  Siccome ogni rebase sposta questi componenti, devono essere Movable.
	// ------------------------------------------------------------------
	Mobility = EComponentMobility::Movable;
}

UGeoreferenceSubsystem* UGeoTransformComponent::GetGeoreferenceSubsystem() const
{
	const UWorld* World = GetWorld();
	return World ? World->GetSubsystem<UGeoreferenceSubsystem>() : nullptr;
}

void UGeoTransformComponent::OnRegister()
{
	Super::OnRegister();

	if (UGeoreferenceSubsystem* Georeference = GetGeoreferenceSubsystem())
	{
		// RegisterGeoComponent chiama gia' RefreshFromGeoreference: il
		// componente si mette nel posto giusto nell'istante in cui entra nel mondo.
		Georeference->RegisterGeoComponent(this);
	}
}

void UGeoTransformComponent::OnUnregister()
{
	if (UGeoreferenceSubsystem* Georeference = GetGeoreferenceSubsystem())
	{
		Georeference->UnregisterGeoComponent(this);
	}

	Super::OnUnregister();
}

void UGeoTransformComponent::RefreshFromGeoreference()
{
	const UGeoreferenceSubsystem* Georeference = GetGeoreferenceSubsystem();
	if (!Georeference)
	{
		return;
	}

	const FGeoreferenceSnapshot Snapshot = Georeference->GetSnapshot();
	const GeoWorld::Core::FGeodetic Geodetic = Coordinate.ToGeodetic();

	const FVector NewLocation = Snapshot.GeodeticToUnreal(Geodetic);

	if (bAlignToLocalUp)
	{
		// SetWorldLocationAndRotation e non SetRelative*: la coordinata
		// geografica e' assoluta per definizione, non ha senso interpretarla
		// rispetto a un eventuale genitore.
		SetWorldLocationAndRotation(NewLocation, Snapshot.GetLocalNeuRotation(Geodetic),
			/*bSweep=*/false, /*OutSweepHitResult=*/nullptr, ETeleportType::TeleportPhysics);
	}
	else
	{
		SetWorldLocation(NewLocation, /*bSweep=*/false, nullptr, ETeleportType::TeleportPhysics);
	}
}

void UGeoTransformComponent::SetGeoCoordinate(const FGeoCoordinate& NewCoordinate)
{
	Coordinate = NewCoordinate;
	RefreshFromGeoreference();
}

#if WITH_EDITOR
void UGeoTransformComponent::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	// NOTA UE: PostEditChangeProperty scatta quando una proprieta' viene
	// modificata dal pannello Details. Senza questo override, scrivere una
	// latitudine nell'editor non muoverebbe nulla finche' non si ricarica il
	// livello - e sembrerebbe che il plugin sia rotto.
	RefreshFromGeoreference();
}
#endif

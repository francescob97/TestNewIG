#include "GeoFlyPawn.h"

#include "GeoCoreModule.h"
#include "Geo/GeoUnits.h"
#include "Georeference/GeoreferenceSubsystem.h"

#include "Components/SphereComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/FloatingPawnMovement.h"

namespace
{
	// Mezza quota al secondo: vedi la nota nell'header.
	constexpr double SpeedPerHeight = 0.5;

	// Sotto i 100 m la proporzionalita' tenderebbe a zero; sopra i 3000 km/s
	// non serve a niente. Entrambi in metri al secondo.
	constexpr double MinSpeedMs = 30.0;
	constexpr double MaxSpeedMs = 300000.0;
}

AGeoFlyPawn::AGeoFlyPawn()
{
	PrimaryActorTick.bCanEverTick = true;

	// NOTA UE: ADefaultPawn registra da solo i tasti WASD, il mouse e Q/E
	// (bAddDefaultMovementBindings, acceso di default). E' il motivo per cui
	// questa classe non ha bisogno di una sola riga di input: derivare da
	// ADefaultPawn invece che da APawn fa risparmiare tutta la configurazione
	// di Enhanced Input, che per una camera di debug sarebbe sproporzionata.

	// Niente collisione: il terreno della Fase 5 non ne ha, ma il pawn si
	// incastrerebbe comunque contro qualunque altra cosa nel livello, e una
	// camera di ispezione che si blocca e' solo un fastidio.
	if (USphereComponent* Collision = GetCollisionComponent())
	{
		Collision->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	}
}

void AGeoFlyPawn::SetSpeedMultiplier(float InMultiplier)
{
	SpeedMultiplier = FMath::Clamp(InMultiplier, 0.01f, 100.0f);
}

void AGeoFlyPawn::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	UFloatingPawnMovement* Movement = Cast<UFloatingPawnMovement>(GetMovementComponent());
	if (!Movement) { return; }

	UWorld* World = GetWorld();
	UGeoreferenceSubsystem* Georeference =
		World ? World->GetSubsystem<UGeoreferenceSubsystem>() : nullptr;
	if (!Georeference) { return; }

	// La quota va chiesta alla georeferenziazione, non letta da Location.Z.
	// Z e' l'altezza sopra l'ORIGINE corrente, che il rebasing sposta di
	// continuo: userebbe un numero che cambia significato ogni pochi chilometri.
	const FGeoreferenceSnapshot Snapshot = Georeference->GetSnapshot();
	CurrentHeightM = Snapshot.UnrealToGeodetic(GetActorLocation()).HeightM;

	const double SpeedMs = FMath::Clamp(
		FMath::Abs(CurrentHeightM) * SpeedPerHeight, MinSpeedMs, MaxSpeedMs)
		* SpeedMultiplier;

	// L'unica conversione metri -> unita' di questo file, e passa da GeoUnits.
	const double SpeedUu = SpeedMs * GeoWorld::Units::MetersToUu;

	Movement->MaxSpeed = static_cast<float>(SpeedUu);

	// Accelerazione e decelerazione proporzionali alla velocita' massima: con
	// valori fissi, a velocita' planetarie ci vorrebbero minuti per partire e
	// altrettanti per fermarsi. Il fattore 4 da' circa un quarto di secondo per
	// raggiungere la velocita' di crociera, a qualunque scala.
	Movement->Acceleration = static_cast<float>(SpeedUu * 4.0);
	Movement->Deceleration = static_cast<float>(SpeedUu * 4.0);

	CurrentSpeedKmh = static_cast<float>(SpeedMs * 3.6);

	// Un pawn di debug che non dice a che velocita' sta andando costringe a
	// indovinare perche' "si muove troppo". La chiave fissa evita che la riga
	// si accumuli frame dopo frame.
	if (GEngine)
	{
		GEngine->AddOnScreenDebugMessage(0x6E91, 0.0f, FColor::Silver,
			FString::Printf(TEXT("Volo: %.0f km/h   quota %.0f m   moltiplicatore x%.2f"),
				CurrentSpeedKmh, CurrentHeightM, SpeedMultiplier));
	}
}

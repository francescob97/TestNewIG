#include "GeoMarkerActor.h"

#include "Georeference/GeoTransformComponent.h"
#include "Georeference/GeoreferenceSubsystem.h"

#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Materials/MaterialInterface.h"
#include "UObject/ConstructorHelpers.h"
#include "DrawDebugHelpers.h"
#include "Geo/GeoUnits.h"

AGeoMarkerActor::AGeoMarkerActor()
{
	// Il tick serve solo a disegnare l'etichetta di debug: e' pochissimo lavoro
	// e riguarda una manciata di attori. Dalla Fase 4 i tile NON avranno tick.
	PrimaryActorTick.bCanEverTick = true;

	// --- La radice e' il componente georeferenziato ---
	// Cosi' l'intero attore (e tutto cio' che gli viene attaccato) segue
	// automaticamente la coordinata geografica a ogni rebase.
	GeoTransform = CreateDefaultSubobject<UGeoTransformComponent>(TEXT("GeoTransform"));
	RootComponent = GeoTransform;

	MeshComponent = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Mesh"));
	MeshComponent->SetupAttachment(RootComponent);
	MeshComponent->SetMobility(EComponentMobility::Movable);
	MeshComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);

	// ------------------------------------------------------------------
	//  NOTA UE: ConstructorHelpers::FObjectFinder funziona SOLO dentro il
	//  costruttore di una UClass, perche' gira una volta sola alla creazione
	//  del CDO e il risultato viene memorizzato staticamente. Usarlo altrove
	//  (in BeginPlay, in una funzione) e' un errore che si manifesta come crash
	//  o come asset nullo. A runtime si usa invece LoadObject/StaticLoadObject.
	//
	//  Il cubo base di Unreal misura 100 unita' = 1 metro di lato: la scala
	//  applicata in OnConstruction lo porta a SizeMeters.
	// ------------------------------------------------------------------
	static ConstructorHelpers::FObjectFinder<UStaticMesh> CubeMesh(
		TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (CubeMesh.Succeeded())
	{
		MeshComponent->SetStaticMesh(CubeMesh.Object);
	}

	static ConstructorHelpers::FObjectFinder<UMaterialInterface> CubeMaterial(
		TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
	if (CubeMaterial.Succeeded())
	{
		MeshComponent->SetMaterial(0, CubeMaterial.Object);
	}
}

void AGeoMarkerActor::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);

	// Il cubo base di Unreal misura 1 metro di lato, quindi la scala coincide
	// numericamente con la dimensione in metri (questo NON e' una conversione
	// di unita': e' una proprieta' dell'asset).
	const double Scale = FMath::Max(0.01, SizeMeters);
	MeshComponent->SetRelativeScale3D(FVector(Scale));

	// Lo alziamo di mezzo lato cosi' poggia sul punto geografico invece di
	// esserne attraversato a meta'.
	const double HalfSizeUu = SizeMeters * 0.5 * GeoWorld::Units::MetersToUu;
	MeshComponent->SetRelativeLocation(FVector(0.0, 0.0, HalfSizeUu));
}

void AGeoMarkerActor::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	const UGeoreferenceSubsystem* Georeference = World->GetSubsystem<UGeoreferenceSubsystem>();
	if (!Georeference || !Georeference->IsDebugOverlayEnabled())
	{
		return;
	}

	// SCELTA UE: DrawDebugString invece di UTextRenderComponent.
	// DrawDebugString e' disegnato dal canvas di debug ed e' SEMPRE rivolto
	// verso la camera, quindi leggibile da qualunque angolo. Un
	// UTextRenderComponent e' un mesh nello spazio: bellissimo per etichette
	// fisse, inutile per un'etichetta che devi poter leggere volandoci attorno.
	// Durata 0 = un solo frame, ridisegnata a ogni tick.
	const double LabelHeightUu = SizeMeters * 1.2 * GeoWorld::Units::MetersToUu;
	const FVector LabelLocation = GetActorLocation() + FVector(0.0, 0.0, LabelHeightUu);

	DrawDebugString(World, LabelLocation,
		FString::Printf(TEXT("%s\n%s"), *Label,
			*GeoTransform->Coordinate.ToDisplayString()),
		/*TestBaseActor=*/nullptr, FColor::Yellow, /*Duration=*/0.0f, /*bDrawShadow=*/true);

	// Terna locale del marker: mostra che il cubo e' allineato alla verticale
	// LOCALE, non a quella dell'origine. E' la verifica visiva piu' importante
	// della Fase 1 dopo l'assenza di jitter.
	const FVector Origin = GetActorLocation();
	const double AxisLength = SizeMeters * 1.5 * GeoWorld::Units::MetersToUu;
	DrawDebugLine(World, Origin, Origin + GetActorForwardVector() * AxisLength, FColor::Red,   false, -1.f, 0, 20.f);
	DrawDebugLine(World, Origin, Origin + GetActorRightVector()   * AxisLength, FColor::Green, false, -1.f, 0, 20.f);
	DrawDebugLine(World, Origin, Origin + GetActorUpVector()      * AxisLength, FColor::Blue,  false, -1.f, 0, 20.f);
}

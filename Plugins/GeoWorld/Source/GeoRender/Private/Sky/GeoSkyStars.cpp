// =============================================================================
//  GeoSkyStars.cpp -- Le stelle vere e i pianeti di UGeoSkySubsystem.
//
//  Un componente di istanze attorno alla camera, a 150 km: un quadratino per
//  stella del catalogo (Sky/Stars.h) e uno per pianeta. Le istanze stanno
//  ferme nel cielo specchiato J2000; a ogni frame si gira il componente
//  (SkyFieldRotation: precessione, tempo siderale, latitudine, poi il mondo)
//  e lo si mette sulla camera. Il materiale (M_GeoStars, Sky/StarShader.inl)
//  apre i quadratini verso la camera e li accende secondo la magnitudine
//  limite del momento (MPC_GeoWeather SkyStars).
// =============================================================================
#include "Sky/GeoSkySubsystem.h"

#include "GeoCoreModule.h"
#include "Geo/GeoFormat.h"
#include "Geo/GeoUnits.h"
#include "Georeference/GeoreferenceSubsystem.h"
#include "Roads/StreetLights.h"
#include "Vegetation/GeoTreeModels.h"

#include "Camera/PlayerCameraManager.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/PlayerController.h"
#include "Materials/MaterialInterface.h"
#include "MeshDescription.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "StaticMeshResources.h"
#include "UObject/Package.h"

using namespace GeoWorld;

namespace
{
	/** Il raggio della sfera delle stelle: oltre le montagne vicine, dentro la distanza a cui si disegna. */
	constexpr double SkyStarSphereRadiusM = 150000.0;
	/** I bounds del quadratino, allargati (m): il materiale lo apre fino a ~1 km (7 pixel a 150 km). */
	constexpr double SkyStarBoundsM = 1500.0;
	/** Fino a che magnitudine si mettono le stelle: tutte quelle del catalogo (6,5). */
	constexpr double SkyStarMaxMagnitude = 6.5;
	/** La Polare, J2000 (per geo.Sky.LookAt). */
	constexpr double SkyPolarisRaDeg = 37.95454, SkyPolarisDecDeg = 89.26411;

	/** Il quadratino delle stelle come UStaticMesh (lo stesso dei punti luce dei lampioni). */
	UStaticMesh* BuildSkyStarQuad(UMaterialInterface* Material)
	{
		Vegetation::FTreeMesh Quad;
		StreetLights::BuildSpriteQuad(Quad);
		FMeshDescription Description;
		GeoVegetationModels::FillMeshDescription(Quad, false, Description);
		UStaticMesh* Mesh = NewObject<UStaticMesh>(GetTransientPackage(), NAME_None, RF_Transient);
		Mesh->GetStaticMaterials().Add(FStaticMaterial(Material, GeoVegetationModels::SlotName()));
		UStaticMesh::FBuildMeshDescriptionsParams BuildParameters;
		BuildParameters.bMarkPackageDirty = false;
		BuildParameters.bBuildSimpleCollision = false;
		BuildParameters.bCommitMeshDescription = false;
		BuildParameters.bFastBuild = true;
		TArray<const FMeshDescription*> Lods;
		Lods.Add(&Description);
		if (!Mesh->BuildFromMeshDescriptions(Lods, BuildParameters)) { return nullptr; }
		// NOTA UE: come per i lampioni, i bounds del quadratino di 5 cm vanno
		// allargati, o il motore lo scarta appena il centro esce dallo schermo.
		Mesh->SetPositiveBoundsExtension(FVector(SkyStarBoundsM));
		Mesh->SetNegativeBoundsExtension(FVector(SkyStarBoundsM));
		Mesh->CalculateExtendedBounds();
		return Mesh;
	}

	/** Il nome di un astro, senza maiuscole e accenti: "Venere", "venere", "VENUS". */
	bool SkyNameIs(const FString& Name, std::initializer_list<const TCHAR*> Names)
	{
		for (const TCHAR* Candidate : Names)
		{
			if (Name.Equals(Candidate, ESearchCase::IgnoreCase)) { return true; }
		}
		return false;
	}
}

// --- Il catalogo -----------------------------------------------------------------------

bool UGeoSkySubsystem::LoadStarCatalog(const FString& Path, FString& OutError)
{
	FString Full = Path;
	if (FPaths::IsRelative(Full)) { Full = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir(), Full); }
	FPaths::NormalizeFilename(Full);
	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *Full))
	{
		OutError = FString::Printf(TEXT("non trovo %s (lo scrive geoworld build-stars)"), *Full);
		return false;
	}
	auto Catalog = std::make_shared<std::vector<Sky::FCatalogStar>>();
	const char* Error = nullptr;
	if (!Sky::ParseStarCatalog(Bytes.GetData(), static_cast<size_t>(Bytes.Num()), *Catalog, Error))
	{
		OutError = FString::Printf(TEXT("%s: %s"), *Full, UTF8_TO_TCHAR(Error ? Error : "?"));
		return false;
	}
	StarCatalog = Catalog;
	StarCatalogPath = Full;
	DestroyStarField();
	UE_LOG(LogGeoWorld, Log, TEXT("[GeoSky] %d stelle dal catalogo %s"), GetCatalogStarCount(), *Full);
	return true;
}

void UGeoSkySubsystem::CloseStarCatalog()
{
	StarCatalog.reset();
	StarCatalogPath.Reset();
	DestroyStarField();
}

void UGeoSkySubsystem::SetPlanetsEnabled(bool bInEnabled)
{
	bPlanets = bInEnabled;
	UpdatePlanets(true);
}

double UGeoSkySubsystem::StarFieldMode() const
{
	if (!bNightSky || !StarMaterial.IsValid() || bStarParametersMissing) { return 0.0; }
	if (StarCatalog && !StarCatalog->empty()) { return 2.0; }
	return bPlanets ? 1.0 : 0.0;
}

double UGeoSkySubsystem::ViewPixelAngle() const
{
	// Il campo visivo orizzontale della camera del giocatore e la larghezza
	// della finestra; nell'editor (niente giocatore) 90 gradi e 1920 pixel.
	double FovDeg = 90.0;
	double WidthPixels = 1920.0;
	if (const UWorld* World = GetWorld())
	{
		if (const APlayerController* Player = World->GetFirstPlayerController())
		{
			if (Player->PlayerCameraManager) { FovDeg = Player->PlayerCameraManager->GetFOVAngle(); }
		}
	}
	if (GEngine && GEngine->GameViewport)
	{
		FVector2D Size = FVector2D::ZeroVector;
		GEngine->GameViewport->GetViewportSize(Size);
		if (Size.X > 1.0) { WidthPixels = Size.X; }
	}
	return 2.0 * FMath::Tan(FMath::DegreesToRadians(FMath::Clamp(FovDeg, 5.0, 170.0)) * 0.5) / WidthPixels;
}

// --- I pianeti -------------------------------------------------------------------------

void UGeoSkySubsystem::UpdatePlanets(bool bForce)
{
	// Un pianeta si sposta fra le stelle al piu' di qualche decimo di grado al
	// giorno: si rifanno ogni ora di cielo (e a ogni salto dell'orologio).
	if (!bForce && PlanetsJd != 0.0 && FMath::Abs(JdUt - PlanetsJd) < 1.0 / 24.0) { return; }
	PlanetsJd = JdUt;
	const double JdTt = Sky::TerrestrialTime(JdUt);
	for (int32 Index = 0; Index < Sky::PlanetCount; ++Index)
	{
		Planets[Index] = Sky::ObservePlanet(static_cast<Sky::EPlanet>(Index), JdTt);
	}
	UHierarchicalInstancedStaticMeshComponent* Field = StarField.Get();
	if (!Field) { return; }
	for (int32 Index = 0; Index < Sky::PlanetCount; ++Index)
	{
		const Sky::FStarInstance Planet = Sky::PlanetInstance(static_cast<Sky::EPlanet>(Index), Planets[Index], SkyStarSphereRadiusM);
		const int32 Instance = StarInstanceCount + Index;
		Field->UpdateInstanceTransform(Instance, FTransform(FVector(Planet.Position[0], Planet.Position[1], Planet.Position[2])),
			/*bWorldSpace=*/false, /*bMarkRenderStateDirty=*/false, /*bTeleport=*/true);
		// Spenti: una magnitudine che nessun cielo fa vedere.
		Field->SetCustomDataValue(Instance, 3, bPlanets ? Planet.Magnitude : 99.0f, /*bMarkRenderStateDirty=*/false);
	}
	Field->MarkRenderStateDirty();
}

Sky::FHorizontal UGeoSkySubsystem::PlanetHorizontal(Sky::EPlanet Planet) const
{
	const Sky::FPlanet& Seen = Planets[static_cast<int32>(Planet)];
	double Rotation[3][3], Mirrored[3], Local[3];
	Sky::SkyFieldRotation(State.LatDeg, State.Bodies.LocalSiderealDeg, Sky::TerrestrialTime(JdUt), Rotation);
	Sky::MirroredUnit(Seen.RaDeg, Seen.DecDeg, Mirrored);
	for (int32 Row = 0; Row < 3; ++Row)
	{
		Local[Row] = Rotation[Row][0] * Mirrored[0] + Rotation[Row][1] * Mirrored[1] + Rotation[Row][2] * Mirrored[2];
	}
	return Sky::HorizontalOf(Local);
}

FString UGeoSkySubsystem::DescribeVisiblePlanets() const
{
	if (!State.bValid || !bPlanets) { return FString(); }
	FString Text;
	for (int32 Index = 0; Index < Sky::PlanetCount; ++Index)
	{
		const Sky::EPlanet Planet = static_cast<Sky::EPlanet>(Index);
		const Sky::FHorizontal Where = PlanetHorizontal(Planet);
		if (Where.AltitudeDeg < 0.5 || Planets[Index].Magnitude > LimitingMag) { continue; }
		if (!Text.IsEmpty()) { Text += TEXT(", "); }
		Text += FString::Printf(TEXT("%s %+.1f a %s, alto %.0f"), UTF8_TO_TCHAR(Sky::PlanetName(Planet)), Planets[Index].Magnitude,
			UTF8_TO_TCHAR(Format::Heading(Where.AzimuthDeg).c_str()), Where.AltitudeDeg);
		Text += UTF8_TO_TCHAR(Format::Degree);
	}
	return Text;
}

bool UGeoSkySubsystem::WorldDirectionOf(const FString& Name, FVector& OutDirection) const
{
	const UWorld* World = GetWorld();
	const UGeoreferenceSubsystem* Georeference = World ? World->GetSubsystem<UGeoreferenceSubsystem>() : nullptr;
	FVector Camera = FVector::ZeroVector;
	if (!State.bValid || !Georeference || !Georeference->GetActiveViewLocation(Camera)) { return false; }
	const FGeoreferenceSnapshot Snapshot = Georeference->GetSnapshot();
	const FQuat LocalToWorld = Snapshot.GetLocalNeuRotation(Snapshot.UnrealToGeodetic(Camera));

	double Local[3] = { 0.0, 0.0, 1.0 };
	if (SkyNameIs(Name, { TEXT("sole"), TEXT("sun") })) { State.Bodies.Sun.ToNorthEastUp(Local); }
	else if (SkyNameIs(Name, { TEXT("luna"), TEXT("moon") })) { State.Bodies.Moon.ToNorthEastUp(Local); }
	else if (SkyNameIs(Name, { TEXT("polare"), TEXT("stella polare"), TEXT("polaris") }))
	{
		double Rotation[3][3], Mirrored[3];
		Sky::SkyFieldRotation(State.LatDeg, State.Bodies.LocalSiderealDeg, Sky::TerrestrialTime(JdUt), Rotation);
		Sky::MirroredUnit(SkyPolarisRaDeg, SkyPolarisDecDeg, Mirrored);
		for (int32 Row = 0; Row < 3; ++Row)
		{
			Local[Row] = Rotation[Row][0] * Mirrored[0] + Rotation[Row][1] * Mirrored[1] + Rotation[Row][2] * Mirrored[2];
		}
	}
	else
	{
		static const TCHAR* English[Sky::PlanetCount] = { TEXT("mercury"), TEXT("venus"), TEXT("mars"), TEXT("jupiter"), TEXT("saturn") };
		int32 Found = INDEX_NONE;
		for (int32 Index = 0; Index < Sky::PlanetCount; ++Index)
		{
			const FString Italian(UTF8_TO_TCHAR(Sky::PlanetName(static_cast<Sky::EPlanet>(Index))));
			if (SkyNameIs(Name, { *Italian, English[Index] })) { Found = Index; }
		}
		if (Found == INDEX_NONE) { return false; }
		PlanetHorizontal(static_cast<Sky::EPlanet>(Found)).ToNorthEastUp(Local);
	}
	OutDirection = LocalToWorld.RotateVector(FVector(Local[0], Local[1], Local[2])).GetSafeNormal();
	return true;
}

// --- Il campo delle stelle ---------------------------------------------------------------

FQuat UGeoSkySubsystem::StarFieldRotation(const FQuat& LocalToWorld) const
{
	// Dal cielo specchiato J2000 al cielo locale (una rotazione vera, Stars.h),
	// poi dal cielo locale al mondo. In un FMatrix di Unreal le righe sono le
	// immagini degli assi: le colonne della matrice del libro.
	double R[3][3];
	Sky::SkyFieldRotation(State.LatDeg, State.Bodies.LocalSiderealDeg, Sky::TerrestrialTime(JdUt), R);
	const FMatrix Matrix(FVector(R[0][0], R[1][0], R[2][0]), FVector(R[0][1], R[1][1], R[2][1]),
		FVector(R[0][2], R[1][2], R[2][2]), FVector::ZeroVector);
	return LocalToWorld * FQuat(Matrix);
}

bool UGeoSkySubsystem::EnsureStarField()
{
	if (StarField.IsValid()) { return true; }
	UWorld* World = GetWorld();
	if (!World || !StarMaterial.IsValid()) { return false; }
	if (!StarQuad.IsValid())
	{
		StarQuad.Reset(BuildSkyStarQuad(StarMaterial.Get()));
		if (!StarQuad.IsValid())
		{
			UE_LOG(LogGeoWorld, Warning, TEXT("[GeoSky] il quadratino delle stelle non si costruisce"));
			return false;
		}
	}

	FActorSpawnParameters SpawnParameters;
	SpawnParameters.ObjectFlags |= RF_Transient;
	AActor* Actor = World->SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity, SpawnParameters);
	if (!Actor) { return false; }
#if WITH_EDITOR
	Actor->SetActorLabel(TEXT("GeoWorld Stelle"));
#endif

	// Le istanze: le stelle del catalogo (se c'e'), poi i cinque pianeti.
	std::vector<Sky::FStarInstance> Instances;
	if (StarCatalog) { Sky::BuildStarInstances(*StarCatalog, SkyStarSphereRadiusM, SkyStarMaxMagnitude, Instances); }
	StarInstanceCount = static_cast<int32>(Instances.size());
	UpdatePlanets(true);
	for (int32 Index = 0; Index < Sky::PlanetCount; ++Index)
	{
		Sky::FStarInstance Planet = Sky::PlanetInstance(static_cast<Sky::EPlanet>(Index), Planets[Index], SkyStarSphereRadiusM);
		if (!bPlanets) { Planet.Magnitude = 99.0f; }
		Instances.push_back(Planet);
	}
	TArray<FTransform> Transforms;
	TArray<float> Data;
	Transforms.Reserve(static_cast<int32>(Instances.size()));
	Data.Reserve(static_cast<int32>(Instances.size()) * Sky::StarDataFloats);
	for (const Sky::FStarInstance& Star : Instances)
	{
		Transforms.Emplace(FVector(Star.Position[0], Star.Position[1], Star.Position[2]));
		float Values[Sky::StarDataFloats];
		Sky::StarData(Star, Values);
		Data.Append(Values, Sky::StarDataFloats);
	}

	UHierarchicalInstancedStaticMeshComponent* Field = NewObject<UHierarchicalInstancedStaticMeshComponent>(Actor, TEXT("Stars"));
	Field->SetMobility(EComponentMobility::Movable);
	Field->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Field->SetCanEverAffectNavigation(false);
	Field->SetCastShadow(false);
	Field->SetStaticMesh(StarQuad.Get());
	Field->SetMaterial(0, StarMaterial.Get());
	// Dopo la sfera del cielo notturno (-1000), prima di ogni altra cosa trasparente.
	Field->SetTranslucentSortPriority(-999);
	Field->SetNumCustomDataFloats(Sky::StarDataFloats);
	Field->AddInstances(Transforms, /*bShouldReturnIndices=*/false, /*bWorldSpace=*/false);
	for (int32 Index = 0; Index < Transforms.Num(); ++Index)
	{
		Field->SetCustomData(Index, TArrayView<const float>(Data.GetData() + Index * Sky::StarDataFloats, Sky::StarDataFloats),
			/*bMarkRenderStateDirty=*/false);
	}
	Actor->SetRootComponent(Field);
	Field->SetWorldScale3D(FVector(Units::MetersToUu));
	Field->RegisterComponent();
	StarActor = Actor;
	StarField = Field;
	return true;
}

void UGeoSkySubsystem::DestroyStarField()
{
	if (AActor* Actor = StarActor.Get()) { Actor->Destroy(); }
	StarActor.Reset();
	StarField.Reset();
	StarInstanceCount = 0;
}

void UGeoSkySubsystem::ApplyStars(const FVector& CameraLocation, const FQuat& LocalToWorld)
{
	UpdatePlanets(false);
	// Di giorno (nessuna stella sopra la magnitudine limite) il componente si nasconde.
	const bool bWanted = State.bValid && StarFieldMode() > 0.0 && LimitingMag > -5.0;
	if (!bWanted)
	{
		if (UHierarchicalInstancedStaticMeshComponent* Field = StarField.Get())
		{
			if (Field->IsVisible()) { Field->SetVisibility(false); }
		}
		return;
	}
	if (!EnsureStarField()) { return; }
	UHierarchicalInstancedStaticMeshComponent* Field = StarField.Get();
	if (!Field->IsVisible()) { Field->SetVisibility(true); }
	Field->SetWorldLocationAndRotation(CameraLocation, StarFieldRotation(LocalToWorld));
}

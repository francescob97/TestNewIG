#include "Weather/GeoPrecipitationRenderer.h"

#include "GeoCoreModule.h"
#include "Geo/GeoUnits.h"
#include "Sky/GeoSkySubsystem.h"

#include "Components/DynamicMeshComponent.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "Engine/ExponentialHeightFog.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "HAL/PlatformTime.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialParameterCollection.h"
#include "Materials/MaterialParameterCollectionInstance.h"

using namespace GeoWorld;

namespace
{
	const TCHAR* PrecipitationMaterialPath = TEXT("/GeoWorld/Materials/M_GeoPrecipitation.M_GeoPrecipitation");
	const TCHAR* WeatherCollectionPath = TEXT("/GeoWorld/Materials/MPC_GeoWeather.MPC_GeoWeather");

	/**
	 * La nebbia di Unreal: trasmissione 2^(-densita'/1000 * distanza in cm)
	 * (FExponentialHeightFogSceneInfo divide per 1000, lo shader usa exp2).
	 * Visibilita' = dove resta il 2%: densita' = 1000 * log2(50) / (100 * V) = 56,4 / V(m).
	 * Oltre 50 km non e' la precipitazione a limitare la vista: la nebbia non si tocca.
	 */
	double FogDensityForVisibility(double VisibilityM)
	{
		return (VisibilityM > 0.0 && VisibilityM < 50000.0) ? 56.4 / VisibilityM : 0.0;
	}
	/**
	 * Sotto la pioggia la nebbia e' quasi uniforme fino alle nuvole: dimezza
	 * ogni ~1 km (Unreal: densita' * 2^(-caduta/1000 * dz in cm)).
	 */
	constexpr float PrecipitationFogFalloff = 0.01f;
	/** Quanto si puo' moltiplicare la densita' di una nebbia del livello che sta piu' in basso del suolo. */
	constexpr double MaxFogCompensation = 16.0;

	FLinearColor ToLinearColour(double A, double B, double C, double D)
	{
		return FLinearColor(static_cast<float>(A), static_cast<float>(B), static_cast<float>(C), static_cast<float>(D));
	}
}

FGeoPrecipitationRenderer::~FGeoPrecipitationRenderer()
{
	Shutdown();
}

bool FGeoPrecipitationRenderer::EnsureAssets()
{
	// Si cercano ogni due secondi finche' mancano: si possono creare con il
	// Play gia' partito (geo.Imagery.CreateMaterial), come i materiali dell'acqua.
	const double Now = FPlatformTime::Seconds();
	if ((!Collection || !Material) && Now - LastAssetLookup > 2.0)
	{
		LastAssetLookup = Now;
		if (!Collection)
		{
			Collection.Reset(LoadObject<UMaterialParameterCollection>(nullptr, WeatherCollectionPath, nullptr, LOAD_NoWarn | LOAD_Quiet));
		}
		if (!Material)
		{
			Material.Reset(LoadObject<UMaterialInterface>(nullptr, PrecipitationMaterialPath, nullptr, LOAD_NoWarn | LOAD_Quiet));
		}
	}
	if (!Collection)
	{
		Problem = TEXT("manca MPC_GeoWeather: niente meteo a schermo (ne' particelle, ne' suolo bagnato). Creala con geo.Imagery.CreateMaterial");
	}
	else if (!Material)
	{
		Problem = TEXT("manca M_GeoPrecipitation: niente pioggia ne' neve a schermo. Crealo con geo.Imagery.CreateMaterial");
	}
	else
	{
		Problem.Reset();
	}
	return Collection.IsValid();
}

bool FGeoPrecipitationRenderer::EnsureActor(UWorld* World)
{
	if (Component.IsValid()) { return true; }
	if (!World || !Material) { return false; }

	// Un attore transient (non finisce nel livello salvato), senza nome fisso:
	// vedi FDynamicMeshTerrainProvider::Initialize.
	FActorSpawnParameters Parameters;
	Parameters.ObjectFlags |= RF_Transient;
	AActor* Spawned = World->SpawnActor<AActor>(AActor::StaticClass(), Parameters);
	if (!Spawned) { return false; }
#if WITH_EDITOR
	Spawned->SetActorLabel(TEXT("GeoWorld Precipitation"));
#endif

	UDynamicMeshComponent* Mesh = NewObject<UDynamicMeshComponent>(Spawned);
	Spawned->SetRootComponent(Mesh);
	Mesh->SetMobility(EComponentMobility::Movable);
	Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Mesh->SetCastShadow(false);
	Mesh->SetEnableRaytracing(false);
	// La mesh e' in METRI: la scala del componente fa metri -> unita' Unreal,
	// come per le tile del terreno. Niente rotazione: gli assi locali sono
	// quelli del mondo (nord, est, su), e il materiale ci conta.
	Mesh->SetWorldScale3D(FVector(Units::MetersToUu));
	Mesh->SetMaterial(0, Material.Get());

	// ------------------------------------------------------------------
	//  La mesh: Count quadrati, 4 vertici nello stesso punto (degeneri finche'
	//  il materiale non li apre). UV 0 = angolo, UV 1 = due semi a caso. Le
	//  normali non servono a un materiale Unlit, ma un overlay senza elementi
	//  darebbe al motore normali indefinite: (0, 0, 1) per tutti.
	// ------------------------------------------------------------------
	Weather::FPrecipitationMesh Data;
	const Weather::FPrecipitationParticles Defaults;
	Weather::BuildPrecipitationMesh(ParticleCount, Defaults.Box, 2024u, Data);

	UE::Geometry::FDynamicMesh3 Built;
	Built.EnableAttributes();
	Built.Attributes()->SetNumUVLayers(2);
	UE::Geometry::FDynamicMeshUVOverlay* Corners = Built.Attributes()->GetUVLayer(0);
	UE::Geometry::FDynamicMeshUVOverlay* Seeds = Built.Attributes()->GetUVLayer(1);
	UE::Geometry::FDynamicMeshNormalOverlay* Normals = Built.Attributes()->PrimaryNormals();
	const int32 VertexCount = static_cast<int32>(Data.Positions.size() / 3);
	for (int32 Index = 0; Index < VertexCount; ++Index)
	{
		Built.AppendVertex(FVector3d(Data.Positions[Index * 3 + 0], Data.Positions[Index * 3 + 1], Data.Positions[Index * 3 + 2]));
		Corners->AppendElement(FVector2f(Data.Corners[Index * 2 + 0], Data.Corners[Index * 2 + 1]));
		Seeds->AppendElement(FVector2f(Data.Seeds[Index * 2 + 0], Data.Seeds[Index * 2 + 1]));
		Normals->AppendElement(FVector3f(0.0f, 0.0f, 1.0f));
	}
	const int32 TriangleCount = static_cast<int32>(Data.Indices.size() / 3);
	for (int32 Index = 0; Index < TriangleCount; ++Index)
	{
		const UE::Geometry::FIndex3i Triangle(static_cast<int32>(Data.Indices[Index * 3 + 0]),
			static_cast<int32>(Data.Indices[Index * 3 + 1]), static_cast<int32>(Data.Indices[Index * 3 + 2]));
		// NOTA UE: AppendTriangle rifiuta i triangoli che ripetono uno spigolo
		// gia' usato nello stesso verso; qui ogni quadrato ha i suoi 4 vertici,
		// quindi non succede. Con i vertici coincidenti il triangolo e'
		// degenere ma valido: la topologia guarda gli indici, non le posizioni.
		const int32 TriangleId = Built.AppendTriangle(Triangle);
		if (TriangleId >= 0)
		{
			Corners->SetTriangle(TriangleId, Triangle);
			Seeds->SetTriangle(TriangleId, Triangle);
			Normals->SetTriangle(TriangleId, Triangle);
		}
	}
	Mesh->SetMesh(MoveTemp(Built));
	Mesh->RegisterComponent();
	Mesh->NotifyMeshUpdated();

	Actor = Spawned;
	Component = Mesh;
	UE_LOG(LogGeoWorld, Log, TEXT("[GeoWeather] particelle pronte: %d quadrati"), ParticleCount);
	return true;
}

void FGeoPrecipitationRenderer::WriteParameters(UWorld* World, const Weather::FPrecipitationParticles& Particles,
                                                const FGroundParameters& Ground)
{
	UMaterialParameterCollectionInstance* Instance = World ? World->GetParameterCollectionInstance(Collection.Get()) : nullptr;
	if (!Instance) { return; }

	Instance->SetVectorParameterValue(TEXT("GroundWeather"),
		ToLinearColour(Ground.Wetness, Ground.SnowCoverM, Ground.SnowTransitionM, Ground.SnowAmount));
	// Il raggio di curvatura: quello medio della Terra basta (l'errore sulla
	// quota a 30 km e' meno di un metro).
	Instance->SetVectorParameterValue(TEXT("GroundFrame"), ToLinearColour(Ground.GeoidAtOriginM, 6371000.0, 0.0, 0.0));
	// Il ghiaccio e la grandine al suolo: una collezione fatta prima non li
	// ha (Set ritorna false), e allora lo si dice.
	const bool bIce = Instance->SetVectorParameterValue(TEXT("GroundIce"),
		ToLinearColour(Ground.IceAmount, Ground.IceLineM, Ground.IceTransitionM, 0.0));
	const bool bHail = Instance->SetVectorParameterValue(TEXT("GroundHailMap"), Ground.HailMap);
	if ((!bIce || !bHail) && Problem.IsEmpty())
	{
		Problem = TEXT("MPC_GeoWeather e' di prima del ghiaccio e della grandine al suolo: rilancia geo.Imagery.CreateMaterial");
	}

	static const TCHAR* LayerNames[Weather::ParticleTypeCount] = { TEXT("PrecipRain"), TEXT("PrecipSnow"), TEXT("PrecipHail") };
	static const TCHAR* VelocityNames[Weather::ParticleTypeCount] = { TEXT("PrecipRainVel"), TEXT("PrecipSnowVel"), TEXT("PrecipHailVel") };
	for (int32 Type = 0; Type < Weather::ParticleTypeCount; ++Type)
	{
		const Weather::FParticleLayer& Layer = Particles.GetLayer(static_cast<Weather::EParticleType>(Type));
		Instance->SetVectorParameterValue(LayerNames[Type], ToLinearColour(Layer.Phase[0], Layer.Phase[1], Layer.Phase[2], Layer.Density));
		Instance->SetVectorParameterValue(VelocityNames[Type],
			ToLinearColour(Layer.Velocity[0], Layer.Velocity[1], Layer.Velocity[2], Layer.Size));
	}
	// La luminosita' delle gocce: il materiale e' unlit, la luce non lo
	// tocca. Di notte la da' il cielo (1 di giorno, 1/600 con la luna).
	const UGeoSkySubsystem* SkySubsystem = World->GetSubsystem<UGeoSkySubsystem>();
	const double Brightness = 0.9 * (SkySubsystem ? SkySubsystem->GetUnlitBrightness() : 1.0);
	Instance->SetVectorParameterValue(TEXT("PrecipBox"), ToLinearColour(Particles.Box[0], Particles.Box[1], Particles.Box[2], Brightness));
	double Rain = 1.0, Snow = 0.0, Hail = 0.0;
	Particles.GetMix(Rain, Snow, Hail);
	Instance->SetVectorParameterValue(TEXT("PrecipMix"), ToLinearColour(Rain, Snow, Hail, 0.0));
}

void FGeoPrecipitationRenderer::Update(UWorld* World, const FVector& CameraLocation,
                                       const Weather::FPrecipitationParticles& Particles, const FGroundParameters& Ground,
                                       double VisibilityM, bool bDriveFog, double GroundZ)
{
	if (!World) { return; }
	if (EnsureAssets()) { WriteParameters(World, Particles, Ground); }

	const bool bFalling = Particles.GetLayer(Weather::EParticleType::Rain).Density > 0.0
	                   || Particles.GetLayer(Weather::EParticleType::Snow).Density > 0.0
	                   || Particles.GetLayer(Weather::EParticleType::Hail).Density > 0.0;
	// L'attore nasce alla prima goccia: con il sereno non costa niente.
	if (bFalling && Material && Collection) { EnsureActor(World); }
	if (UDynamicMeshComponent* Mesh = Component.Get())
	{
		Mesh->SetWorldLocation(CameraLocation);
		Mesh->SetVisibility(bFalling);
	}
	UpdateFog(World, VisibilityM, bDriveFog, GroundZ);
}

void FGeoPrecipitationRenderer::UpdateFog(UWorld* World, double VisibilityM, bool bDriveFog, double GroundZ)
{
	const double Wanted = bDriveFog ? FogDensityForVisibility(VisibilityM) : 0.0;
	if (!bFogManaged)
	{
		if (Wanted <= 0.0)
		{
			FogState = bDriveFog ? TEXT("non serve") : TEXT("spenta (geo.Weather.Fog 0)");
			return;
		}
		// La prima volta che serve: la nebbia del livello, o una nostra.
		AExponentialHeightFog* Found = Fog.Get();
		if (!Found)
		{
			for (TActorIterator<AExponentialHeightFog> It(World); It; ++It) { Found = *It; break; }
			// Nell'editor quella del livello non e' nostra: salvando il livello
			// mentre piove, finirebbero nel file i valori della pioggia.
			if (Found && World->WorldType == EWorldType::Editor)
			{
				FogState = TEXT("quella del livello non si tocca nell'editor: la visibilita' cala in Play");
				return;
			}
		}
		if (!Found)
		{
			FActorSpawnParameters Parameters;
			Parameters.ObjectFlags |= RF_Transient;
			Found = World->SpawnActor<AExponentialHeightFog>(AExponentialHeightFog::StaticClass(), FTransform::Identity, Parameters);
			bFogSpawned = Found != nullptr;
			if (Found && Found->GetComponent())
			{
				Found->GetComponent()->SetMobility(EComponentMobility::Movable);
				Found->GetComponent()->SetFogDensity(0.0f);
			}
		}
		UExponentialHeightFogComponent* FoundComponent = Found ? Found->GetComponent() : nullptr;
		if (!FoundComponent) { return; }
		Fog = Found;
		OriginalFogDensity = FoundComponent->FogDensity;
		OriginalFogFalloff = FoundComponent->FogHeightFalloff;
		bFogManaged = true;
	}

	AExponentialHeightFog* Managed = Fog.Get();
	UExponentialHeightFogComponent* FogComponent = Managed ? Managed->GetComponent() : nullptr;
	if (!FogComponent) { bFogManaged = false; return; }

	// Mai meno nebbia di quella che il livello aveva: la precipitazione ne
	// aggiunge. Quando la sua e' sotto quella del livello, tutto torna com'era.
	if (Wanted <= OriginalFogDensity)
	{
		RestoreFog();
		return;
	}
	const float Blend = static_cast<float>(FMath::Clamp((Wanted - OriginalFogDensity) / FMath::Max(static_cast<double>(OriginalFogDensity), 0.005), 0.0, 1.0));
	const float Falloff = FMath::Lerp(OriginalFogFalloff, PrecipitationFogFalloff, Blend);

	// ------------------------------------------------------------------
	//  La nebbia di Unreal e' piu' fitta in basso, a partire dalla Z
	//  dell'attore. La nostra si mette sul suolo sotto la camera; quella del
	//  livello resta dov'e' (il livello l'ha messa li'), e la densita' si
	//  corregge perche' sul suolo sia quella voluta.
	// ------------------------------------------------------------------
	double Density = Wanted;
	if (bFogSpawned)
	{
		const FVector Location = Managed->GetActorLocation();
		Managed->SetActorLocation(FVector(Location.X, Location.Y, GroundZ));
		FogState = TEXT("nostra, sul suolo sotto la camera");
	}
	else
	{
		const double Doublings = Falloff / 1000.0 * (GroundZ - Managed->GetActorLocation().Z);
		Density = Wanted * FMath::Min(FMath::Pow(2.0, Doublings), MaxFogCompensation);
		FogState = TEXT("quella del livello");
	}
	FogComponent->SetFogDensity(static_cast<float>(Density));
	FogComponent->SetFogHeightFalloff(Falloff);
	AppliedFogDensity = Density;
}

void FGeoPrecipitationRenderer::RestoreFog()
{
	if (AExponentialHeightFog* Managed = Fog.Get())
	{
		if (UExponentialHeightFogComponent* FogComponent = Managed->GetComponent())
		{
			FogComponent->SetFogDensity(OriginalFogDensity);
			FogComponent->SetFogHeightFalloff(OriginalFogFalloff);
		}
	}
	bFogManaged = false;
	AppliedFogDensity = 0.0;
	FogState = TEXT("non serve");
}

void FGeoPrecipitationRenderer::Shutdown()
{
	if (bFogManaged) { RestoreFog(); }
	if (bFogSpawned)
	{
		if (AExponentialHeightFog* Spawned = Fog.Get()) { Spawned->Destroy(); }
		bFogSpawned = false;
	}
	Fog.Reset();
	if (AActor* Spawned = Actor.Get()) { Spawned->Destroy(); }
	Actor.Reset();
	Component.Reset();
}

#include "Weather/GeoCloudRenderer.h"

#include "GeoCoreModule.h"
#include "Geo/GeoUnits.h"
#include "Imagery/GeoRuntimeTexture.h"
#include "Weather/GeoLightningRenderer.h"

#include "Components/DirectionalLightComponent.h"
#include "Components/SkyAtmosphereComponent.h"
#include "Components/VolumetricCloudComponent.h"
#include "Engine/Engine.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/PlatformTime.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialParameterCollection.h"
#include "Materials/MaterialParameterCollectionInstance.h"
#include "UObject/Package.h"
#include "UObject/UObjectIterator.h"

using namespace GeoWorld;

namespace
{
	const TCHAR* CloudMaterialPath = TEXT("/GeoWorld/Materials/M_GeoClouds.M_GeoClouds");
	const TCHAR* CloudCollectionPath = TEXT("/GeoWorld/Materials/MPC_GeoWeather.MPC_GeoWeather");

	/** Lo strato del motore sta un po' fuori dalle nuvole: la base piatta e le cime non si tagliano. */
	constexpr double LayerMarginM = 100.0;
	/** Il raggio del pianeta quando nel livello non c'e' una SkyAtmosphere (km): la curvatura dello shader. */
	constexpr float EarthRadiusKm = 6371.0f;
	/**
	 * Quanto si accende la nuvola attorno a un lampo, per lux del sole della
	 * scena. Una STIMA: una nuvola al sole ha una luminanza di circa 0,3 x
	 * lux, e l'emissivo del volume si somma lungo il cammino dentro la
	 * nuvola (~1 / estinzione, una trentina di metri). 0,01 fa il cuore del
	 * lampo luminoso quanto la nuvola al sole: di giorno si vede appena, di
	 * notte (il sole a pochi lux) l'esposizione lo fa accecante. Se sembra
	 * troppo o troppo poco, e' questo il numero da toccare.
	 */
	constexpr double CloudGlowPerLux = 0.01;

	/** Le impostazioni che cambiano la mappa: con altre, la si rifa' subito. */
	bool SameMap(const Weather::FPrecipitationSettings& A, const Weather::FPrecipitationSettings& B)
	{
		return A.Kind == B.Kind && A.RateMmH == B.RateMmH && A.Coverage == B.Coverage
			&& A.CellSizeKm == B.CellSizeKm && A.Seed == B.Seed;
	}

	/** Un componente vivo e registrato di QUESTO mondo (non un modello di classe, non un altro mondo). */
	bool InWorld(const UActorComponent* Component, const UWorld* World)
	{
		return IsValid(Component) && !Component->HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject)
			&& Component->IsRegistered() && Component->GetWorld() == World;
	}
}

FGeoCloudRenderer::~FGeoCloudRenderer()
{
	Shutdown();
}

bool FGeoCloudRenderer::EnsureAssets(UWorld* World)
{
	// Si cercano ogni due secondi finche' mancano: si possono creare con il
	// Play gia' partito (geo.Imagery.CreateMaterial).
	const double Clock = FPlatformTime::Seconds();
	if ((!Material || !Collection) && Clock - LastAssetLookup > 2.0)
	{
		LastAssetLookup = Clock;
		if (!Collection)
		{
			Collection.Reset(LoadObject<UMaterialParameterCollection>(nullptr, CloudCollectionPath, nullptr, LOAD_NoWarn | LOAD_Quiet));
		}
		if (!Material)
		{
			Material.Reset(LoadObject<UMaterialInterface>(nullptr, CloudMaterialPath, nullptr, LOAD_NoWarn | LOAD_Quiet));
		}
	}
	if (!Material || !Collection)
	{
		Problem = !Collection
			? TEXT("manca MPC_GeoWeather: niente nuvole. Creala con geo.Imagery.CreateMaterial")
			: TEXT("manca M_GeoClouds: niente nuvole. Crealo con geo.Imagery.CreateMaterial");
		return false;
	}
	if (!Instance)
	{
		// Un M_GeoClouds fatto prima della correzione non ha il flag "Used with
		// Volumetric Cloud": la VolumetricCloud lo sostituirebbe con il materiale
		// di ripiego, una nebbia bianca su tutto il cielo e sul terreno. Meglio
		// niente nuvole, e la frase che dice cosa fare. (Il flag si cerca per nome:
		// se questa versione del motore non lo ha, non serve.)
		if (const UMaterial* Base = Material->GetMaterial())
		{
			const FBoolProperty* Flag = FindFProperty<FBoolProperty>(UMaterial::StaticClass(), FName(TEXT("bUsedWithVolumetricCloud")));
			if (Flag && !Flag->GetPropertyValue_InContainer(Base))
			{
				Problem = TEXT("M_GeoClouds e' di prima (manca \"Used with Volumetric Cloud\"): niente nuvole. Rilancia geo.Imagery.CreateMaterial");
				if (!bReportedOldMaterial)
				{
					bReportedOldMaterial = true;
					UE_LOG(LogGeoWorld, Warning, TEXT("[GeoWeather] %s"), *Problem);
					if (GEngine) { GEngine->AddOnScreenDebugMessage(-1, 15.0f, FColor::Red, Problem); }
				}
				return false;
			}
		}
		// L'istanza porta la mappa meteo: una texture, che una collezione non puo' portare.
		Instance.Reset(UMaterialInstanceDynamic::Create(Material.Get(), GetTransientPackage()));
		if (!Instance) { Problem = TEXT("M_GeoClouds: non riesco a farne un'istanza"); return false; }
		if (MapTexture) { Instance->SetTextureParameterValue(TEXT("CloudWeatherMap"), MapTexture.Get()); }
	}
	Problem.Reset();
	return World != nullptr;
}

UVolumetricCloudComponent* FGeoCloudRenderer::EnsureCloud(UWorld* World)
{
	if (UVolumetricCloudComponent* Current = Cloud.Get()) { return Current; }
	const double Clock = FPlatformTime::Seconds();
	if (Clock - LastCloudSearch < 2.0) { return nullptr; }
	LastCloudSearch = Clock;

	// ------------------------------------------------------------------
	//  Una VolumetricCloud del livello? Una sola si vede per volta.
	//  NOTA UE: TObjectIterator su una classe precisa scorre solo gli oggetti
	//  di quella classe (c'e' un indice per classe): costa poco anche in un
	//  livello grande. Si scartano i modelli (CDO) e gli altri mondi.
	// ------------------------------------------------------------------
	UVolumetricCloudComponent* Level = nullptr;
	for (TObjectIterator<UVolumetricCloudComponent> It; It; ++It)
	{
		if (InWorld(*It, World)) { Level = *It; break; }
	}
	if (Level && World->WorldType == EWorldType::Editor)
	{
		// Nell'editor non si tocca: nascosta, si salverebbe nascosta.
		State = TEXT("quella del livello non si tocca nell'editor: le nuvole GeoWorld si vedono in Play");
		return nullptr;
	}
	if (Level)
	{
		if (Level->IsVisible())
		{
			Level->SetVisibility(false);
			bLevelCloudHidden = true;
		}
		LevelCloud = Level;
	}

	FActorSpawnParameters Parameters;
	Parameters.ObjectFlags |= RF_Transient;
	AActor* Spawned = World->SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity, Parameters);
	if (!Spawned) { return nullptr; }
#if WITH_EDITOR
	Spawned->SetActorLabel(TEXT("GeoWorld Clouds"));
#endif
	UVolumetricCloudComponent* Made = NewObject<UVolumetricCloudComponent>(Spawned);
	Made->SetMobility(EComponentMobility::Movable);
	Spawned->SetRootComponent(Made);
	// Senza SkyAtmosphere il guscio delle nuvole usa questo raggio, con la cima
	// del pianeta nell'origine del mondo: la' dove il rebase tiene l'ellissoide.
	Made->SetPlanetRadius(EarthRadiusKm);
	Made->SetMaterial(Instance.Get());
	Made->RegisterComponent();
	AppliedBottomKm = AppliedHeightKm = -1.0f;

	SpawnedActor = Spawned;
	Cloud = Made;
	State = Level ? TEXT("nostra (quella del livello nascosta finche' servono le nostre)") : TEXT("nostra");
	UE_LOG(LogGeoWorld, Log, TEXT("[GeoWeather] nuvole pronte: VolumetricCloud %s"), State);
	return Made;
}

void FGeoCloudRenderer::ReleaseCloud()
{
	if (AActor* Spawned = SpawnedActor.Get()) { Spawned->Destroy(); }
	SpawnedActor.Reset();
	Cloud.Reset();
	if (UVolumetricCloudComponent* Level = LevelCloud.Get())
	{
		if (bLevelCloudHidden) { Level->SetVisibility(true); }
	}
	LevelCloud.Reset();
	bLevelCloudHidden = false;
	AppliedBottomKm = AppliedHeightKm = -1.0f;
	LastCloudSearch = -1.0e9;
	State = TEXT("spente");
}

void FGeoCloudRenderer::StartMap(const FInputs& Inputs)
{
	// Tutto per copia, niente "this": il lavoro puo' finire dopo di noi
	// (Shutdown lo aspetta comunque). La precipitazione si copia com'e'
	// adesso: la mappa la fotografa in questo istante, deriva compresa.
	Job = MakeShared<FMapJob, ESPMode::ThreadSafe>();
	TSharedPtr<FMapJob, ESPMode::ThreadSafe> Out = Job;
	const Weather::FPrecipitationField Field = *Inputs.Field;
	const double LatDeg = Inputs.CameraLatDeg, LonDeg = Inputs.CameraLonDeg;
	MapSettings = Field.GetSettings();
	JobStartedAt = Inputs.TimeSeconds;
	JobTask = UE::Tasks::Launch(TEXT("GeoCloudMap"),
		[Out, Field, LatDeg, LonDeg]()
		{
			Weather::BuildCloudMap(Field, LatDeg, LonDeg, MapExtentM, MapSize, Out->Map);
			Out->bReady.store(true, std::memory_order_release);
		},
		UE::Tasks::ETaskPriority::BackgroundNormal);
}

void FGeoCloudRenderer::FinishMap()
{
	if (!Job || !Job->bReady.load(std::memory_order_acquire)) { return; }
	Weather::FCloudMap& Made = Job->Map;
	// Dati, non colori: lineare (niente sRGB), bordi bloccati.
	UTexture2D* Texture = CreateGeoRuntimeDataTexture(Made.Size, Made.Size, Made.Bgra, {}, /*bSrgb=*/false, false, false);
	if (Texture)
	{
		MapTexture.Reset(Texture);
		if (Instance) { Instance->SetTextureParameterValue(TEXT("CloudWeatherMap"), Texture); }
		CurrentMap = MoveTemp(Made);
		// I pixel ora stanno sulla scheda video: qui servono solo centro e deriva.
		CurrentMap.Bgra.clear();
		CurrentMap.Bgra.shrink_to_fit();
		bHaveMap = true;
		MapBuiltAt = JobStartedAt;
	}
	Job.Reset();
	JobTask = UE::Tasks::FTask();
}

void FGeoCloudRenderer::CheckScene(UWorld* World)
{
	const double Clock = FPlatformTime::Seconds();
	if (Clock - LastSceneCheck < 2.0) { return; }
	LastSceneCheck = Clock;

	bool bAtmosphere = false;
	for (TObjectIterator<USkyAtmosphereComponent> It; It; ++It)
	{
		if (InWorld(*It, World)) { bAtmosphere = true; break; }
	}
	// Le nuvole le illumina la luce direzionale marcata "Atmosphere Sun
	// Light"; le ombre sul terreno le fa se ha "Cast Cloud Shadows".
	bool bSun = false, bShadows = false;
	for (TObjectIterator<UDirectionalLightComponent> It; It; ++It)
	{
		if (!InWorld(*It, World) || !It->bAtmosphereSunLight) { continue; }
		bSun = true;
		bShadows = bShadows || It->bCastCloudShadows;
	}
	if (!bSun)
	{
		Advice = TEXT("nessuna luce direzionale con \"Atmosphere Sun Light\": le nuvole restano scure. Accendila sulla luce del sole");
	}
	else if (!bShadows)
	{
		Advice = TEXT("per le ombre delle nuvole sul terreno: \"Cast Cloud Shadows\" sulla luce del sole");
	}
	else if (!bAtmosphere)
	{
		Advice = TEXT("nessuna SkyAtmosphere: le nuvole ci sono, ma il cielo attorno no (aggiungine una al livello)");
	}
	else
	{
		Advice.Reset();
	}
}

void FGeoCloudRenderer::WriteParameters(UWorld* World, const FGeoreferenceSnapshot& Snapshot, const FInputs& Inputs)
{
	UMaterialParameterCollectionInstance* Parameters = World->GetParameterCollectionInstance(Collection.Get());
	if (!Parameters) { return; }

	// Il disegno dei cumuli si ancora al primo posto visto: un rebase lo
	// sposta solo di quanto si sposta l'origine (Clouds.h).
	const Core::FGeodetic Origin = Snapshot.Georeference.GetOriginGeodetic();
	const double OriginLatDeg = Origin.LatRad / Core::DegToRad, OriginLonDeg = Origin.LonRad / Core::DegToRad;
	if (!bHaveReference)
	{
		ReferenceLatDeg = OriginLatDeg;
		ReferenceLonDeg = OriginLonDeg;
		bHaveReference = true;
	}
	// Il centro della mappa negli assi di Unreal: cambia con il rebase.
	FVector Centre = FVector::ZeroVector;
	if (bHaveMap)
	{
		Centre = Snapshot.GeodeticToUnreal(Core::FGeodetic::FromDegrees(CurrentMap.CenterLatDeg, CurrentMap.CenterLonDeg, 0.0));
	}
	Weather::FCloudShaderParameters Shader = Weather::MakeCloudShaderParameters(Inputs.Settings, *Inputs.Field, CurrentMap,
		Centre.X * Units::UuToMeters, Centre.Y * Units::UuToMeters, OriginLatDeg, OriginLonDeg,
		ReferenceLatDeg, ReferenceLonDeg, Inputs.GeoidAtOriginM, Inputs.TimeSeconds);
	if (Inputs.bFlash && Inputs.FlashBrightness > 0.0)
	{
		const FVector Flash = Inputs.FlashLocation * Units::UuToMeters;
		Shader.Flash[0] = static_cast<float>(Flash.X);
		Shader.Flash[1] = static_cast<float>(Flash.Y);
		Shader.Flash[2] = static_cast<float>(Flash.Z);
		Shader.Flash[3] = static_cast<float>(Inputs.FlashBrightness * FGeoLightningRenderer::ReferenceLux(World) * CloudGlowPerLux);
	}

	// Una collezione fatta prima delle nuvole non ha questi parametri: Set ritorna false.
	bool bAll = true;
	auto Write = [Parameters, &bAll](const TCHAR* Name, const float Values[4])
	{
		bAll = Parameters->SetVectorParameterValue(Name, FLinearColor(Values[0], Values[1], Values[2], Values[3])) && bAll;
	};
	Write(TEXT("CloudMap"), Shader.Map);
	Write(TEXT("CloudShift"), Shader.Shift);
	Write(TEXT("CloudLayer"), Shader.Layer);
	Write(TEXT("CloudLook"), Shader.Look);
	Write(TEXT("CloudFlash"), Shader.Flash);
	Write(TEXT("CloudFair"), Shader.Fair);
	if (!bAll)
	{
		Problem = TEXT("MPC_GeoWeather non ha i parametri delle nuvole (e' di prima): rilancia geo.Imagery.CreateMaterial");
	}
}

void FGeoCloudRenderer::Update(UWorld* World, const FGeoreferenceSnapshot& Snapshot, const FInputs& Inputs)
{
	if (!World || !Inputs.Field) { return; }
	FinishMap();

	// ------------------------------------------------------------------
	//  1. La mappa: si rifa' se manca, se e' vecchia, se la camera si e'
	//     allontanata dal suo centro o se il tempo e' cambiato (un comando).
	//     Una per volta: finche' una e' in lavorazione, si aspetta.
	// ------------------------------------------------------------------
	if (Inputs.bHaveCamera && !Job)
	{
		bool bStale = !bHaveMap || Inputs.TimeSeconds - MapBuiltAt > MapRefreshSeconds
			|| !SameMap(MapSettings, Inputs.Field->GetSettings());
		if (!bStale)
		{
			const double North = (Inputs.CameraLatDeg - CurrentMap.CenterLatDeg) * 111132.0;
			const double East = (Inputs.CameraLonDeg - CurrentMap.CenterLonDeg) * 111320.0
				* FMath::Cos(FMath::DegreesToRadians(CurrentMap.CenterLatDeg));
			bStale = North * North + East * East > MapRefreshDistanceM * MapRefreshDistanceM;
		}
		if (bStale) { StartMap(Inputs); }
	}

	if (!EnsureAssets(World)) { return; }
	CheckScene(World);

	// ------------------------------------------------------------------
	//  2. La VolumetricCloud: nasce alla prima nuvola, e si nasconde (non
	//     costa niente) quando non ce ne sono.
	// ------------------------------------------------------------------
	const bool bVisible = Weather::HasClouds(Inputs.Settings, *Inputs.Field);
	UVolumetricCloudComponent* Component = bVisible ? EnsureCloud(World) : Cloud.Get();
	if (!Component) { return; }
	if (Component->IsVisible() != bVisible) { Component->SetVisibility(bVisible); }
	if (!bVisible) { return; }

	// ------------------------------------------------------------------
	//  3. Lo strato (km sopra il pianeta, cioe' sull'ellissoide all'origine):
	//     quello delle nuvole in quota sul mare, piu' il geoide, con un
	//     margine. Si ridanno solo se cambiano: ogni volta il motore rifa'
	//     le nuvole nella scena.
	// ------------------------------------------------------------------
	double Bottom = 0.0, Top = 0.0;
	Weather::CloudLayerBounds(Inputs.Settings, *Inputs.Field, Bottom, Top);
	LayerBottomM = FMath::Max(0.0, Bottom + Inputs.GeoidAtOriginM - LayerMarginM);
	LayerTopM = Top + Inputs.GeoidAtOriginM + LayerMarginM;
	const float BottomKm = static_cast<float>(LayerBottomM / 1000.0);
	const float HeightKm = static_cast<float>(FMath::Max(0.1, (LayerTopM - LayerBottomM) / 1000.0));
	if (FMath::Abs(BottomKm - AppliedBottomKm) > 0.001f || FMath::Abs(HeightKm - AppliedHeightKm) > 0.001f)
	{
		Component->SetLayerBottomAltitude(BottomKm);
		Component->SetLayerHeight(HeightKm);
		AppliedBottomKm = BottomKm;
		AppliedHeightKm = HeightKm;
	}
	WriteParameters(World, Snapshot, Inputs);
}

void FGeoCloudRenderer::Shutdown()
{
	// Il lavoro in volo esegue codice di questo modulo: si aspetta.
	if (Job) { JobTask.Wait(); }
	Job.Reset();
	JobTask = UE::Tasks::FTask();
	ReleaseCloud();
}

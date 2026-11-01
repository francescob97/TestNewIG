// =============================================================================
//  GeoSkySubsystem.h -- Il giorno e la notte. STRATO: UNREAL.
//
//  Un orologio (data e ora, che corre alla velocita' scelta) e, a ogni frame,
//  il sole e la luna visti dalla camera (Sky/Astronomy.h). Con quelli:
//
//    - IL SOLE DEL LIVELLO (la luce direzionale "Atmosphere Sun Light") si
//      gira verso il sole vero. L'intensita' non cambia: con la SkyAtmosphere
//      e' Unreal che la attenua attraverso l'aria, la arrossa al tramonto, la
//      spegne quando il sole va sotto la Terra e accende il crepuscolo;
//    - LA LUNA e' una seconda luce direzionale (atmosfera, indice 1), creata
//      dal cielo, accesa solo di notte con la luce della sua fase;
//    - L'ESPOSIZIONE: di notte un Post Process Volume la abbassa di qualche
//      stop, perche' l'esposizione automatica non trasformi la notte in un
//      giorno grigio;
//    - LA NEBBIA del livello: il suo colore scende con la luce (una nebbia
//      chiara di notte "brilla");
//    - LE STELLE e LA LUNA con le fasi: una sfera enorme attorno alla camera
//      con M_GeoNightSky (Sky/NightSkyShader.inl);
//    - MPC_GeoWeather: SkySun, SkyMoon, SkyEquatorX/Y/Z, SkyNight, SkyStars,
//      per i materiali (le finestre accese degli edifici, il cielo notturno);
//    - LE STELLE VERE (il catalogo di Yale, geoworld build-stars) e I PIANETI:
//      un punto per astro in un componente di istanze attorno alla camera
//      (M_GeoStars, Sky/StarShader.inl), girato con il cielo
//      (Sky/Stars.h, SkyFieldRotation). Codice in GeoSkyStars.cpp.
//
//  Tutto quello che tocca del livello (il sole, la nebbia) lo rimette com'era
//  quando si spegne (geo.Sky.Enable 0) o quando il mondo si chiude.
// =============================================================================
#pragma once

#include "CoreMinimal.h"
#include "Engine/EngineTypes.h"
#include "Subsystems/WorldSubsystem.h"
#include "UObject/StrongObjectPtr.h"

#include "Sky/Astronomy.h"
#include "Sky/DayLight.h"
#include "Sky/Stars.h"

#include <memory>
#include <vector>

#include "GeoSkySubsystem.generated.h"

class AActor;
class ADirectionalLight;
class AExponentialHeightFog;
class APostProcessVolume;
class UDirectionalLightComponent;
class UHierarchicalInstancedStaticMeshComponent;
class UMaterialInterface;
class UMaterialParameterCollection;
class USkyLightComponent;
class UStaticMesh;
class UStaticMeshComponent;

/** Quello che il cielo sa in questo momento: per le stampe, l'overlay, i Blueprint. */
struct FGeoSkyState
{
	bool bValid = false;
	double JdUt = 0.0;
	FString LocalDate, LocalClock, ZoneName;
	double LocalHour = 12.0;
	double LatDeg = 0.0, LonDeg = 0.0;
	GeoWorld::Sky::FSkyBodies Bodies;
	/** Quanta luce c'e', 1 = sole allo zenit. */
	double RelativeLight = 1.0;
	double StarVisibility = 0.0;
	double CityLights = 0.0;
	double NightBlend = 0.0;
	/** Levata e tramonto di oggi, ora locale ("--:--" se non c'e'). */
	FString Sunrise, Sunset, Moonrise, Moonset;
};

UCLASS()
class GEORENDER_API UGeoSkySubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual bool IsTickableInEditor() const override { return true; }

	/** Spento: il sole e la nebbia del livello tornano come erano. */
	void SetEnabled(bool bInEnabled);
	bool IsEnabled() const { return bEnabled; }

	// --- Il tempo ------------------------------------------------------------

	double GetTimeUt() const { return JdUt; }
	void SetTimeUt(double InJdUt);
	/** L'ora locale (0..24) dello stesso giorno locale. */
	void SetLocalHour(double Hours);
	/** Il giorno locale, alla stessa ora locale. */
	void SetLocalDate(int32 Year, int32 Month, int32 Day);
	/** Adesso, e il tempo che corre come l'orologio. */
	void SetToNow();
	/** 0 fermo, 1 come l'orologio, 60 un minuto al secondo; anche all'indietro. */
	void SetSpeed(double InSpeed) { Speed = InSpeed; }
	double GetSpeed() const { return Speed; }
	void SetTimeZone(const GeoWorld::Sky::FTimeZone& InZone) { Zone = InZone; DayCacheKey = -1.0; }
	const GeoWorld::Sky::FTimeZone& GetTimeZone() const { return Zone; }

	// --- La luce -------------------------------------------------------------

	void SetMoonRatio(double Ratio) { Daylight.MoonRatio = FMath::Clamp(Ratio, 1.0e-6, 0.1); }
	double GetMoonRatio() const { return Daylight.MoonRatio; }
	void SetNightExposureBias(double Stops) { NightExposureBias = FMath::Clamp(Stops, -8.0, 0.0); }
	double GetNightExposureBias() const { return NightExposureBias; }
	void SetNightSkyEnabled(bool bInEnabled) { bNightSky = bInEnabled; }
	bool IsNightSkyEnabled() const { return bNightSky; }
	void SetCityLightsEnabled(bool bInEnabled) { bCityLights = bInEnabled; }
	bool AreCityLightsEnabled() const { return bCityLights; }
	/** Il sole del livello, lux (0: quello che aveva la luce quando il cielo l'ha presa). */
	void SetSunLux(double Lux);
	double GetSunLux() const { return DayLux; }

	/**
	 * Quanta luce c'e' rispetto al giorno pieno, per quello che Unreal non
	 * illumina da solo (la pioggia unlit): 1 col sole a ~45 gradi, fino a 1,5
	 * a mezzogiorno d'estate, 1/600 con la luna piena. 1 se il cielo e' spento.
	 */
	double GetUnlitBrightness() const;
	/** La luce sulla scena adesso, lux (per i fulmini); 0 se il cielo e' spento. */
	double GetSceneLux() const;

	const FGeoSkyState& GetState() const { return State; }

	void SetDebugOverlayEnabled(bool bInShow) { bShowDebugOverlay = bInShow; }
	bool IsDebugOverlayEnabled() const { return bShowDebugOverlay; }

	// --- Le stelle vere e i pianeti (GeoSkyStars.cpp) --------------------------

	/** Apre il catalogo delle stelle (.gst, geoworld build-stars): le stelle vere al posto di quelle finte. */
	bool LoadStarCatalog(const FString& Path, FString& OutError);
	/** Torna alle stelle finte (i pianeti restano). */
	void CloseStarCatalog();
	int32 GetCatalogStarCount() const { return StarCatalog ? static_cast<int32>(StarCatalog->size()) : 0; }
	const FString& GetStarCatalogPath() const { return StarCatalogPath; }

	void SetPlanetsEnabled(bool bInEnabled);
	bool ArePlanetsEnabled() const { return bPlanets; }
	/** Quanta luce fanno le stelle e i pianeti (1 = la magnitudine 0 brilla come 1/50 del sole del livello). */
	void SetStarBrightness(double InBrightness) { StarBrightness = FMath::Clamp(InBrightness, 0.0, 50.0); }
	double GetStarBrightness() const { return StarBrightness; }
	/** La magnitudine delle stelle piu' deboli che si vedono adesso (Sky/Stars.h LimitingMagnitude). */
	double GetLimitingMagnitude() const { return LimitingMag; }

	/** Un pianeta adesso, visto dal centro della Terra (J2000). */
	const GeoWorld::Sky::FPlanet& GetPlanet(GeoWorld::Sky::EPlanet Planet) const { return Planets[static_cast<int32>(Planet)]; }
	/** Dove sta un pianeta nel cielo della camera adesso (con la precessione). */
	GeoWorld::Sky::FHorizontal PlanetHorizontal(GeoWorld::Sky::EPlanet Planet) const;
	/** "Venere -4,2 a ovest, Giove -2,5 a sud-est": i pianeti sopra l'orizzonte che si vedono adesso. */
	FString DescribeVisiblePlanets() const;
	/** La direzione nel mondo di un astro per nome (sole, luna, i pianeti, la Polare): per geo.Sky.LookAt. */
	bool WorldDirectionOf(const FString& Name, FVector& OutDirection) const;

	/** Vuoto se va tutto bene; altrimenti cosa manca (in rosso nelle stampe). */
	FString GetProblem() const { return Problem; }
	/** Un consiglio sulla scena (le ombre, la SkyAtmosphere), se c'e'. */
	FString GetAdvice() const { return Advice; }

private:
	/** Quello del livello che il cielo cambia, com'era prima: per rimetterlo. */
	struct FOriginalSun
	{
		TWeakObjectPtr<UDirectionalLightComponent> Light;
		FRotator Rotation = FRotator::ZeroRotator;
		float Intensity = 0.0f;
		bool bCastShadows = true;
		bool bAtmosphereSunLight = true;
		float ShadowDistance = 0.0f;
		EComponentMobility::Type Mobility = EComponentMobility::Movable;
	};
	struct FOriginalFog
	{
		TWeakObjectPtr<AExponentialHeightFog> Fog;
		FLinearColor Inscattering = FLinearColor::Black;
		FLinearColor Directional = FLinearColor::Black;
	};

	void FindScene(UWorld* World);
	void Restore();
	void UpdateState(double LatDeg, double LonDeg);
	void UpdateDayEvents(double LatDeg, double LonDeg);
	void ApplySun(const FQuat& LocalToWorld);
	void ApplyMoon(const FQuat& LocalToWorld);
	void ApplyFog();
	void ApplyExposure();
	void ApplySkyLight(const FVector& SunDirection);
	void ApplyParameters(const FQuat& LocalToWorld);
	void ApplyNightSky(const FVector& CameraLocation);
	/** Le stelle vere e i pianeti: il componente attorno alla camera, girato con il cielo. */
	void ApplyStars(const FVector& CameraLocation, const FQuat& LocalToWorld);
	bool EnsureStarField();
	void UpdatePlanets(bool bForce);
	void DestroyStarField();
	/** La rotazione dal cielo specchiato J2000 al mondo, adesso. */
	FQuat StarFieldRotation(const FQuat& LocalToWorld) const;
	/** Il valore di SkyStars.x: 2 catalogo e pianeti, 1 solo i pianeti, 0 niente. */
	double StarFieldMode() const;
	/** L'angolo di un pixel adesso, radianti: dal campo visivo e dalla larghezza della finestra. */
	double ViewPixelAngle() const;
	void EnsureAssets();
	double CloudClearness() const;
	void DrawDebugOverlay() const;

	bool bEnabled = true;
	bool bApplied = false;
	double JdUt = GeoWorld::Sky::J2000;
	double Speed = 0.0;
	GeoWorld::Sky::FTimeZone Zone;
	GeoWorld::Sky::FDaylightSettings Daylight;
	double NightExposureBias = -2.0;
	bool bNightSky = true;
	bool bCityLights = true;
	double DayLux = 0.0;
	double ConfiguredLux = 0.0;
	double ShadowDistanceM = 3000.0;
	bool bShowDebugOverlay = false;

	FOriginalSun OriginalSun;
	FOriginalFog OriginalFog;
	TWeakObjectPtr<ADirectionalLight> SpawnedSun;
	TWeakObjectPtr<ADirectionalLight> MoonLight;
	TWeakObjectPtr<APostProcessVolume> Exposure;
	TWeakObjectPtr<AActor> Dome;
	TWeakObjectPtr<UStaticMeshComponent> DomeMesh;
	TWeakObjectPtr<USkyLightComponent> SkyLight;
	bool bHasAtmosphere = false;
	double LastSceneCheck = -100.0;

	TStrongObjectPtr<UMaterialInterface> NightSkyMaterial;
	TStrongObjectPtr<UMaterialParameterCollection> WeatherCollection;
	TStrongObjectPtr<UStaticMesh> SphereMesh;
	double LastAssetLookup = -100.0;
	bool bParametersMissing = false;

	/** L'ultima direzione data al sole e alla luna: si girano solo quando cambia davvero (le ombre costano). */
	FVector LastSunDirection = FVector::ZeroVector;
	FVector LastMoonDirection = FVector::ZeroVector;
	FVector LastCaptureDirection = FVector::ZeroVector;
	double LastCaptureClock = -100.0;
	bool bSunShadows = true;

	FGeoSkyState State;
	double DayCacheKey = -1.0;
	double DayCacheLatDeg = 0.0, DayCacheLonDeg = 0.0;
	FString Problem, Advice;

	// --- Le stelle vere e i pianeti ----------------------------------------------
	std::shared_ptr<const std::vector<GeoWorld::Sky::FCatalogStar>> StarCatalog;
	FString StarCatalogPath;
	TWeakObjectPtr<AActor> StarActor;
	TWeakObjectPtr<UHierarchicalInstancedStaticMeshComponent> StarField;
	TStrongObjectPtr<UStaticMesh> StarQuad;
	TStrongObjectPtr<UMaterialInterface> StarMaterial;
	/** Le istanze del catalogo nel componente: i pianeti vengono dopo. */
	int32 StarInstanceCount = 0;
	bool bPlanets = true;
	double StarBrightness = 1.0;
	double LimitingMag = 6.0;
	GeoWorld::Sky::FPlanet Planets[GeoWorld::Sky::PlanetCount];
	/** L'istante (UT) dei pianeti nel componente: si rifanno ogni ora di cielo, o a un salto. */
	double PlanetsJd = 0.0;
	/** MPC_GeoWeather e' di prima delle stelle vere (manca SkyStars). */
	bool bStarParametersMissing = false;
};

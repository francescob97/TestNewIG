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
//    - MPC_GeoWeather: SkySun, SkyMoon, SkyEquatorX/Y/Z, SkyNight, per i
//      materiali (le finestre accese degli edifici, il cielo notturno).
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

#include "GeoSkySubsystem.generated.h"

class AActor;
class ADirectionalLight;
class AExponentialHeightFog;
class APostProcessVolume;
class UDirectionalLightComponent;
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
};

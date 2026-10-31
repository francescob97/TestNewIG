#include "Sky/GeoSkySubsystem.h"

#include "GeoCoreModule.h"
#include "Geo/GeoUnits.h"
#include "Georeference/GeoreferenceSubsystem.h"
#include "Sky/GeoSkySettings.h"
#include "Terrain/GeoTerrainSubsystem.h"
#include "Weather/GeoWeatherSubsystem.h"

#include "Components/DirectionalLightComponent.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Components/SkyAtmosphereComponent.h"
#include "Components/SkyLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/DirectionalLight.h"
#include "Engine/Engine.h"
#include "Engine/ExponentialHeightFog.h"
#include "Engine/PostProcessVolume.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialParameterCollection.h"
#include "Materials/MaterialParameterCollectionInstance.h"
#include "Misc/DateTime.h"
#include "UObject/UObjectIterator.h"

using namespace GeoWorld;

namespace
{
	const TCHAR* NightSkyMaterialPath = TEXT("/GeoWorld/Materials/M_GeoNightSky.M_GeoNightSky");
	const TCHAR* SkyCollectionPath = TEXT("/GeoWorld/Materials/MPC_GeoWeather.MPC_GeoWeather");
	const TCHAR* SkySpherePath = TEXT("/Engine/BasicShapes/Sphere.Sphere");
	/** La sfera delle stelle: 100 km di raggio (la sfera del motore ne ha 50 cm). */
	constexpr double SkyDomeRadiusM = 100000.0;
	constexpr double SkyBasicSphereRadiusUu = 50.0;
	/** Sotto questo cambio di direzione (gradi) le luci non si girano: girarle rifa' le ombre. */
	constexpr double SkyLightStepDeg = 0.01;
	/** La luce di un giorno qualunque, per la pioggia: il sole a 45 gradi. */
	const double SkyUnlitReference = Sky::SunlightShare(45.0);

	/** Un componente vivo e registrato di QUESTO mondo. */
	bool SkyComponentInWorld(const UActorComponent* Component, const UWorld* World)
	{
		return IsValid(Component) && !Component->HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject)
			&& Component->IsRegistered() && Component->GetWorld() == World;
	}

	/**
	 * Legge un colore di un oggetto PER NOME, fra piu' nomi possibili.
	 * NOTA UE: il colore della nebbia si chiamava FogInscatteringColor e in UE
	 * 5.1 e' diventato FogInscatteringLuminance; i setter hanno tenuto il nome
	 * vecchio. Per nome, il file compila con tutte e due.
	 */
	bool SkyReadColour(const UObject* Object, std::initializer_list<const TCHAR*> Names, FLinearColor& Out)
	{
		if (!Object) { return false; }
		for (const TCHAR* Name : Names)
		{
			const FStructProperty* Property = FindFProperty<FStructProperty>(Object->GetClass(), FName(Name));
			if (Property && Property->Struct == TBaseStructure<FLinearColor>::Get())
			{
				Out = *Property->ContainerPtrToValuePtr<FLinearColor>(Object);
				return true;
			}
		}
		return false;
	}

	FString SkyText(const std::string& Text) { return FString(UTF8_TO_TCHAR(Text.c_str())); }

	FLinearColor SkyVector(const FVector& Direction, double W)
	{
		return FLinearColor(static_cast<float>(Direction.X), static_cast<float>(Direction.Y), static_cast<float>(Direction.Z), static_cast<float>(W));
	}

	double SkySmooth(double Edge0, double Edge1, double X)
	{
		const double T = FMath::Clamp((X - Edge0) / (Edge1 - Edge0), 0.0, 1.0);
		return T * T * (3.0 - 2.0 * T);
	}

	bool SkyUsesVirtualShadowMaps()
	{
		const IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(TEXT("r.Shadow.Virtual.Enable"));
		return Variable && Variable->GetInt() != 0;
	}
}

// ============================================================================

bool UGeoSkySubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	if (!Super::ShouldCreateSubsystem(Outer)) { return false; }
	const UWorld* World = Cast<UWorld>(Outer);
	return World && (World->WorldType == EWorldType::Game
	              || World->WorldType == EWorldType::PIE
	              || World->WorldType == EWorldType::Editor);
}

void UGeoSkySubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	const UGeoSkySettings* Settings = GetDefault<UGeoSkySettings>();
	bEnabled = Settings->bEnabled;
	Speed = Settings->Speed;
	Zone.Kind = Settings->bCentralEuropeTime ? Sky::ETimeZoneKind::CentralEurope : Sky::ETimeZoneKind::Fixed;
	Zone.FixedOffsetHours = Settings->FixedUtcOffsetHours;
	Daylight.MoonRatio = Settings->MoonLightRatio;
	NightExposureBias = Settings->NightExposureBias;
	bNightSky = Settings->bNightSky;
	bCityLights = Settings->bCityLights;
	ConfiguredLux = Settings->SunIlluminanceLux;
	ShadowDistanceM = Settings->ShadowDistanceM;

	// Si comincia da adesso, o da oggi all'ora scelta (ora locale).
	const double Now = FDateTime::UtcNow().GetJulianDay();
	if (Settings->bStartAtRealTime)
	{
		JdUt = Now;
	}
	else
	{
		const Sky::FCalendarDate Today = Sky::CalendarDate(Sky::LocalJulianDay(Zone, Now));
		JdUt = Sky::UtFromLocal(Zone, Sky::JulianDay(Today.Year, Today.Month, static_cast<double>(Today.Day))
			+ FMath::Clamp(static_cast<double>(Settings->StartHour), 0.0, 24.0) / 24.0);
	}
}

void UGeoSkySubsystem::Deinitialize()
{
	Restore();
	NightSkyMaterial.Reset();
	WeatherCollection.Reset();
	SphereMesh.Reset();
	Super::Deinitialize();
}

TStatId UGeoSkySubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UGeoSkySubsystem, STATGROUP_Tickables);
}

// --- Il tempo e le impostazioni -------------------------------------------------

void UGeoSkySubsystem::SetEnabled(bool bInEnabled)
{
	bEnabled = bInEnabled;
	if (!bEnabled) { Restore(); }
}

void UGeoSkySubsystem::SetTimeUt(double InJdUt)
{
	JdUt = InJdUt;
	LastSunDirection = FVector::ZeroVector;
	LastMoonDirection = FVector::ZeroVector;
}

void UGeoSkySubsystem::SetLocalHour(double Hours)
{
	const double Midnight = Sky::LocalMidnightUt(Zone, JdUt);
	const Sky::FCalendarDate Day = Sky::CalendarDate(Sky::LocalJulianDay(Zone, Midnight + 1.0 / 24.0));
	SetTimeUt(Sky::UtFromLocal(Zone, Sky::JulianDay(Day.Year, Day.Month, static_cast<double>(Day.Day)) + FMath::Clamp(Hours, 0.0, 24.0) / 24.0));
}

void UGeoSkySubsystem::SetLocalDate(int32 Year, int32 Month, int32 Day)
{
	const double Hours = Sky::LocalHour(Zone, JdUt);
	SetTimeUt(Sky::UtFromLocal(Zone, Sky::JulianDay(Year, Month, static_cast<double>(Day)) + Hours / 24.0));
}

void UGeoSkySubsystem::SetToNow()
{
	SetTimeUt(FDateTime::UtcNow().GetJulianDay());
	Speed = 1.0;
}

void UGeoSkySubsystem::SetSunLux(double Lux)
{
	ConfiguredLux = FMath::Max(0.0, Lux);
	if (ConfiguredLux > 0.0) { DayLux = ConfiguredLux; }
	else if (OriginalSun.Light.IsValid()) { DayLux = OriginalSun.Intensity > 0.01f ? static_cast<double>(OriginalSun.Intensity) : 10.0; }
	LastSunDirection = FVector::ZeroVector;
}

double UGeoSkySubsystem::GetUnlitBrightness() const
{
	if (!bEnabled || !bApplied || !State.bValid) { return 1.0; }
	return FMath::Clamp(State.RelativeLight / SkyUnlitReference, 0.0, 1.5);
}

double UGeoSkySubsystem::GetSceneLux() const
{
	if (!bEnabled || !bApplied || !State.bValid) { return 0.0; }
	return DayLux * State.RelativeLight;
}

// --- Il giro ----------------------------------------------------------------------

void UGeoSkySubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	if (!bEnabled)
	{
		if (bShowDebugOverlay) { DrawDebugOverlay(); }
		return;
	}
	UWorld* World = GetWorld();
	const UGeoreferenceSubsystem* Georeference = World ? World->GetSubsystem<UGeoreferenceSubsystem>() : nullptr;
	FVector Camera = FVector::ZeroVector;
	if (!Georeference || !Georeference->GetActiveViewLocation(Camera)) { return; }

	// Il tempo corre (anche all'indietro, con una velocita' negativa).
	JdUt += static_cast<double>(DeltaTime) * Speed / 86400.0;

	EnsureAssets();
	FindScene(World);

	// Il cielo visto dalla camera, e gli assi del suo cielo locale nel mondo:
	// al rebase l'origine si sposta, e con lei la direzione del sole nel mondo.
	const FGeoreferenceSnapshot Snapshot = Georeference->GetSnapshot();
	const Core::FGeodetic Here = Snapshot.UnrealToGeodetic(Camera);
	const double LatDeg = Here.LatRad * Core::RadToDeg, LonDeg = Here.LonRad * Core::RadToDeg;
	UpdateState(LatDeg, LonDeg);
	UpdateDayEvents(LatDeg, LonDeg);
	const FQuat LocalToWorld = Snapshot.GetLocalNeuRotation(Here);

	ApplySun(LocalToWorld);
	ApplyMoon(LocalToWorld);
	ApplyFog();
	ApplyExposure();
	ApplyParameters(LocalToWorld);
	ApplyNightSky(Camera);
	bApplied = true;
	Problem = (bNightSky && !NightSkyMaterial.IsValid())
		? FString(TEXT("manca M_GeoNightSky: niente stelle ne' luna con le fasi. Crealo con geo.Imagery.CreateMaterial"))
		: (bParametersMissing ? FString(TEXT("MPC_GeoWeather e' di prima del cielo (manca SkySun): rilancia geo.Imagery.CreateMaterial"))
		                      : FString());
	if (bShowDebugOverlay) { DrawDebugOverlay(); }
}

void UGeoSkySubsystem::UpdateState(double LatDeg, double LonDeg)
{
	State.bValid = true;
	State.JdUt = JdUt;
	State.LatDeg = LatDeg;
	State.LonDeg = LonDeg;
	State.Bodies = Sky::ObserveSky(JdUt, LatDeg, LonDeg);
	State.RelativeLight = Sky::RelativeLight(State.Bodies, Daylight);
	const double SunAltitude = State.Bodies.Sun.AltitudeDeg;
	State.StarVisibility = Sky::StarVisibility(SunAltitude);
	State.CityLights = Sky::CityLightsOn(SunAltitude);
	State.NightBlend = Sky::NightBlend(SunAltitude);
	State.LocalHour = Sky::LocalHour(Zone, JdUt);
	State.LocalDate = SkyText(Sky::FormatLocalDate(Zone, JdUt));
	State.LocalClock = SkyText(Sky::FormatLocalClock(Zone, JdUt));
	State.ZoneName = SkyText(Sky::ZoneName(Zone, JdUt));
}

void UGeoSkySubsystem::UpdateDayEvents(double LatDeg, double LonDeg)
{
	// Levate e tramonti: una volta al giorno (locale), o se ci si sposta di mezzo grado.
	const double Midnight = Sky::LocalMidnightUt(Zone, JdUt);
	if (FMath::Abs(Midnight - DayCacheKey) < 1e-6 && FMath::Abs(LatDeg - DayCacheLatDeg) < 0.5 && FMath::Abs(LonDeg - DayCacheLonDeg) < 0.5)
	{
		return;
	}
	DayCacheKey = Midnight;
	DayCacheLatDeg = LatDeg;
	DayCacheLonDeg = LonDeg;
	auto Clock = [this](double Jd) { return Jd > 0.0 ? SkyText(Sky::FormatLocalClock(Zone, Jd)) : FString(TEXT("--:--")); };
	double Rise = 0.0, Set = 0.0;
	Sky::RiseAndSet(Sky::ESkyEvent::Sunrise, Midnight, LatDeg, LonDeg, Rise, Set);
	State.Sunrise = Clock(Rise);
	State.Sunset = Clock(Set);
	Sky::RiseAndSet(Sky::ESkyEvent::Moonrise, Midnight, LatDeg, LonDeg, Rise, Set);
	State.Moonrise = Clock(Rise);
	State.Moonset = Clock(Set);
}

// --- La scena -------------------------------------------------------------------------

void UGeoSkySubsystem::EnsureAssets()
{
	const double Now = FPlatformTime::Seconds();
	if (NightSkyMaterial.IsValid() && WeatherCollection.IsValid() && SphereMesh.IsValid()) { return; }
	if (Now - LastAssetLookup < 2.0) { return; }
	LastAssetLookup = Now;
	if (!WeatherCollection.IsValid())
	{
		WeatherCollection.Reset(LoadObject<UMaterialParameterCollection>(nullptr, SkyCollectionPath, nullptr, LOAD_NoWarn | LOAD_Quiet));
	}
	if (!NightSkyMaterial.IsValid())
	{
		NightSkyMaterial.Reset(LoadObject<UMaterialInterface>(nullptr, NightSkyMaterialPath, nullptr, LOAD_NoWarn | LOAD_Quiet));
	}
	if (!SphereMesh.IsValid())
	{
		SphereMesh.Reset(LoadObject<UStaticMesh>(nullptr, SkySpherePath, nullptr, LOAD_NoWarn | LOAD_Quiet));
	}
}

void UGeoSkySubsystem::FindScene(UWorld* World)
{
	const double Now = FPlatformTime::Seconds();
	const bool bSunMissing = !OriginalSun.Light.IsValid();
	if (!bSunMissing && Now - LastSceneCheck < 2.0) { return; }
	LastSceneCheck = Now;

	bHasAtmosphere = false;
	for (TObjectIterator<USkyAtmosphereComponent> It; It; ++It)
	{
		if (SkyComponentInWorld(*It, World)) { bHasAtmosphere = true; break; }
	}
	SkyLight.Reset();
	for (TObjectIterator<USkyLightComponent> It; It; ++It)
	{
		if (SkyComponentInWorld(*It, World)) { SkyLight = *It; break; }
	}

	// Il sole: la luce direzionale del livello marcata "Atmosphere Sun Light"
	// (indice 0), altrimenti la prima; se non c'e', una nostra.
	if (bSunMissing)
	{
		UDirectionalLightComponent* Found = nullptr;
		UDirectionalLightComponent* First = nullptr;
		for (TObjectIterator<UDirectionalLightComponent> It; It; ++It)
		{
			UDirectionalLightComponent* Light = *It;
			if (!SkyComponentInWorld(Light, World) || (MoonLight.IsValid() && Light->GetOwner() == MoonLight.Get())) { continue; }
			if (!First) { First = Light; }
			if (Light->bAtmosphereSunLight && Light->AtmosphereSunLightIndex == 0) { Found = Light; break; }
		}
		if (!Found) { Found = First; }
		if (!Found)
		{
			FActorSpawnParameters SpawnParameters;
			SpawnParameters.ObjectFlags |= RF_Transient;
			ADirectionalLight* Spawned = World->SpawnActor<ADirectionalLight>(ADirectionalLight::StaticClass(), FTransform::Identity, SpawnParameters);
			if (Spawned)
			{
#if WITH_EDITOR
				Spawned->SetActorLabel(TEXT("GeoWorld Sole"));
#endif
				SpawnedSun = Spawned;
				Found = Cast<UDirectionalLightComponent>(Spawned->GetLightComponent());
				if (Found)
				{
					Found->SetMobility(EComponentMobility::Movable);
					Found->SetIntensity(ConfiguredLux > 0.0 ? static_cast<float>(ConfiguredLux) : 10.0f);
				}
				UE_LOG(LogGeoWorld, Log, TEXT("[GeoSky] nel livello non c'e' una luce direzionale: ne creo una per il sole"));
			}
		}
		if (Found)
		{
			OriginalSun.Light = Found;
			OriginalSun.Rotation = Found->GetComponentRotation();
			OriginalSun.Intensity = Found->Intensity;
			OriginalSun.bCastShadows = Found->CastShadows != 0;
			OriginalSun.bAtmosphereSunLight = Found->bAtmosphereSunLight != 0;
			OriginalSun.ShadowDistance = Found->DynamicShadowDistanceMovableLight;
			OriginalSun.Mobility = Found->Mobility;
			DayLux = ConfiguredLux > 0.0 ? ConfiguredLux : (OriginalSun.Intensity > 0.01f ? static_cast<double>(OriginalSun.Intensity) : 10.0);
			if (Found->Mobility != EComponentMobility::Movable) { Found->SetMobility(EComponentMobility::Movable); }
			if (!Found->bAtmosphereSunLight) { Found->SetAtmosphereSunLight(true); }
			if (!SkyUsesVirtualShadowMaps())
			{
				Found->SetDynamicShadowDistanceMovableLight(static_cast<float>(ShadowDistanceM * Units::MetersToUu));
			}
			LastSunDirection = FVector::ZeroVector;
			UE_LOG(LogGeoWorld, Log, TEXT("[GeoSky] il sole e' '%s' (%.1f lux)"), *GetNameSafe(Found->GetOwner()), DayLux);
		}
	}

	// La nebbia: il colore com'era, per scalarlo con la luce.
	if (!OriginalFog.Fog.IsValid())
	{
		for (TActorIterator<AExponentialHeightFog> It(World); It; ++It)
		{
			const UExponentialHeightFogComponent* FogComponent = It->GetComponent();
			if (!FogComponent) { continue; }
			FLinearColor Inscattering, Directional;
			if (SkyReadColour(FogComponent, { TEXT("FogInscatteringLuminance"), TEXT("FogInscatteringColor") }, Inscattering))
			{
				SkyReadColour(FogComponent, { TEXT("DirectionalInscatteringLuminance"), TEXT("DirectionalInscatteringColor") }, Directional);
				OriginalFog.Fog = *It;
				OriginalFog.Inscattering = Inscattering;
				OriginalFog.Directional = Directional;
			}
			break;
		}
	}

	// I consigli: le cose che si vedono solo col sole che si muove.
	Advice.Reset();
	const UGeoTerrainSubsystem* Terrain = World->GetSubsystem<UGeoTerrainSubsystem>();
	if (!bHasAtmosphere)
	{
		Advice = TEXT("nessuna SkyAtmosphere nel livello: niente cielo che si colora al tramonto (aggiungine una)");
	}
	else if (Terrain && !Terrain->IsCastingShadows() && State.bValid && State.Bodies.Sun.AltitudeDeg > 0.0 && State.Bodies.Sun.AltitudeDeg < 25.0)
	{
		Advice = TEXT("sole basso: per le ombre delle montagne geo.Terrain.Shadows 1");
	}
}

void UGeoSkySubsystem::ApplySun(const FQuat& LocalToWorld)
{
	UDirectionalLightComponent* Sun = OriginalSun.Light.Get();
	if (!Sun) { return; }
	double Local[3];
	State.Bodies.Sun.ToNorthEastUp(Local);
	const FVector Direction = LocalToWorld.RotateVector(FVector(Local[0], Local[1], Local[2])).GetSafeNormal();
	const double Altitude = State.Bodies.Sun.AltitudeDeg;
	if (LastSunDirection.IsZero() || FVector::DotProduct(Direction, LastSunDirection) < FMath::Cos(FMath::DegreesToRadians(SkyLightStepDeg)))
	{
		// La luce va DAL sole verso la scena: il suo asse X guarda all'opposto del sole.
		Sun->SetWorldRotation((-Direction).Rotation());
		LastSunDirection = Direction;
		ApplySkyLight(Direction);
	}
	// Con la SkyAtmosphere l'intensita' resta quella del giorno: l'aria la
	// attenua, la arrossa e, quando il sole va sotto, la Terra la copre; il
	// cielo del crepuscolo resta acceso. Per sicurezza (e perche' la notte sia
	// notte anche se la Terra non la coprisse) la si spegne fra -4 e -10
	// gradi, quando del crepuscolo resta poco. Senza SkyAtmosphere, a mano
	// sull'orizzonte.
	const double Lux = DayLux * (bHasAtmosphere ? SkySmooth(-10.0, -4.0, Altitude) : SkySmooth(-1.0, 2.0, Altitude));
	if (FMath::Abs(Sun->Intensity - static_cast<float>(Lux)) > 1e-4f * static_cast<float>(DayLux + 1.0)) { Sun->SetIntensity(static_cast<float>(Lux)); }
	// Le ombre del sole finche' fa luce (con un po' d'isteresi).
	const bool bWantShadows = bSunShadows ? Altitude > -5.0 : Altitude > -4.0;
	if (bWantShadows != bSunShadows || (Sun->CastShadows != 0) != (bWantShadows && OriginalSun.bCastShadows))
	{
		bSunShadows = bWantShadows;
		Sun->SetCastShadows(bWantShadows && OriginalSun.bCastShadows);
	}
}

void UGeoSkySubsystem::ApplyMoon(const FQuat& LocalToWorld)
{
	UWorld* World = GetWorld();
	const double SunAltitude = State.Bodies.Sun.AltitudeDeg, MoonAltitude = State.Bodies.Moon.AltitudeDeg;
	// Accesa quando il sole si e' spento (sotto i -6 gradi) e lei e' sopra l'orizzonte: mai due luci con le ombre.
	const bool bWanted = SunAltitude < -6.0 && MoonAltitude > -1.0;
	ADirectionalLight* Moon = MoonLight.Get();
	if (!Moon && bWanted && World)
	{
		FActorSpawnParameters SpawnParameters;
		SpawnParameters.ObjectFlags |= RF_Transient;
		Moon = World->SpawnActor<ADirectionalLight>(ADirectionalLight::StaticClass(), FTransform::Identity, SpawnParameters);
		if (Moon)
		{
#if WITH_EDITOR
			Moon->SetActorLabel(TEXT("GeoWorld Luna"));
#endif
			if (UDirectionalLightComponent* NewLight = Cast<UDirectionalLightComponent>(Moon->GetLightComponent()))
			{
				NewLight->SetMobility(EComponentMobility::Movable);
				NewLight->SetLightColor(FLinearColor(0.78f, 0.85f, 1.0f));
				// La seconda luce dell'atmosfera: illumina il cielo e le nuvole di notte.
				NewLight->SetAtmosphereSunLight(true);
				NewLight->SetAtmosphereSunLightIndex(1);
				NewLight->SetCastShadows(true);
			}
			MoonLight = Moon;
			LastMoonDirection = FVector::ZeroVector;
		}
	}
	UDirectionalLightComponent* Light = Moon ? Cast<UDirectionalLightComponent>(Moon->GetLightComponent()) : nullptr;
	if (!Light) { return; }
	if (Light->IsVisible() != bWanted) { Light->SetVisibility(bWanted); }
	if (!bWanted) { return; }

	// Il disco della luna lo disegna il cielo notturno, con la fase: quello
	// dell'atmosfera (pieno, senza fase) si spegne quando c'e' il nostro.
	const FLinearColor Disc = (bNightSky && NightSkyMaterial.IsValid()) ? FLinearColor::Black : FLinearColor(0.3f, 0.3f, 0.3f);
	if (!Light->AtmosphereSunDiskColorScale.Equals(Disc))
	{
		Light->AtmosphereSunDiskColorScale = Disc;
		Light->MarkRenderStateDirty();
	}
	double Local[3];
	State.Bodies.Moon.ToNorthEastUp(Local);
	const FVector Direction = LocalToWorld.RotateVector(FVector(Local[0], Local[1], Local[2])).GetSafeNormal();
	if (LastMoonDirection.IsZero() || FVector::DotProduct(Direction, LastMoonDirection) < FMath::Cos(FMath::DegreesToRadians(SkyLightStepDeg)))
	{
		Light->SetWorldRotation((-Direction).Rotation());
		LastMoonDirection = Direction;
	}
	double Lux = DayLux * Daylight.MoonRatio * Sky::MoonPhaseBrightness(State.Bodies.MoonPhaseAngleDeg);
	if (!bHasAtmosphere) { Lux *= SkySmooth(-1.0, 2.0, MoonAltitude); }
	if (FMath::Abs(Light->Intensity - static_cast<float>(Lux)) > 1e-3f * static_cast<float>(Lux + 1e-4)) { Light->SetIntensity(static_cast<float>(Lux)); }
}

void UGeoSkySubsystem::ApplySkyLight(const FVector& SunDirection)
{
	// Con "Real Time Capture" la luce del cielo si aggiorna da sola; senza, va
	// ricatturata (costa: al piu' ogni due secondi, se il sole si e' spostato).
	USkyLightComponent* Capture = SkyLight.Get();
	if (!Capture || Capture->bRealTimeCapture) { return; }
	const double Now = FPlatformTime::Seconds();
	const bool bMoved = LastCaptureDirection.IsZero()
		|| FVector::DotProduct(SunDirection, LastCaptureDirection) < FMath::Cos(FMath::DegreesToRadians(1.5));
	if (bMoved && Now - LastCaptureClock > 2.0)
	{
		Capture->RecaptureSky();
		LastCaptureDirection = SunDirection;
		LastCaptureClock = Now;
	}
}

void UGeoSkySubsystem::ApplyFog()
{
	AExponentialHeightFog* Fog = OriginalFog.Fog.Get();
	UExponentialHeightFogComponent* FogComponent = Fog ? Fog->GetComponent() : nullptr;
	if (!FogComponent) { return; }
	// La nebbia e' aria illuminata: di notte scura come il resto.
	const float Scale = static_cast<float>(FMath::Clamp(State.RelativeLight / SkyUnlitReference, 0.0, 1.0));
	FLinearColor Current;
	SkyReadColour(FogComponent, { TEXT("FogInscatteringLuminance"), TEXT("FogInscatteringColor") }, Current);
	const FLinearColor Wanted = OriginalFog.Inscattering * Scale;
	if (!Current.Equals(Wanted, 1e-5f))
	{
		FogComponent->SetFogInscatteringColor(Wanted);
		FogComponent->SetDirectionalInscatteringColor(OriginalFog.Directional * Scale);
	}
}

void UGeoSkySubsystem::ApplyExposure()
{
	UWorld* World = GetWorld();
	const double Weight = (NightExposureBias < 0.0) ? State.NightBlend : 0.0;
	APostProcessVolume* Volume = Exposure.Get();
	if (!Volume && Weight > 0.001 && World)
	{
		FActorSpawnParameters SpawnParameters;
		SpawnParameters.ObjectFlags |= RF_Transient;
		Volume = World->SpawnActor<APostProcessVolume>(APostProcessVolume::StaticClass(), FTransform::Identity, SpawnParameters);
		if (Volume)
		{
#if WITH_EDITOR
			Volume->SetActorLabel(TEXT("GeoWorld Esposizione notte"));
#endif
			// Senza confini: vale ovunque. Cambia solo la correzione
			// dell'esposizione, e solo quanto e' notte (BlendWeight).
			Volume->bUnbound = true;
			Volume->Priority = 50.0f;
			Volume->Settings.bOverride_AutoExposureBias = true;
			Exposure = Volume;
		}
	}
	if (!Volume) { return; }
	Volume->Settings.AutoExposureBias = static_cast<float>(NightExposureBias);
	Volume->BlendWeight = static_cast<float>(Weight);
}

double UGeoSkySubsystem::CloudClearness() const
{
	const UWorld* World = GetWorld();
	const UGeoWeatherSubsystem* Weather = World ? World->GetSubsystem<UGeoWeatherSubsystem>() : nullptr;
	if (!Weather) { return 1.0; }
	if (Weather->GetObserverPrecipitation().AtCamera.IsFalling() || Weather->GetObserverPrecipitation().AtGround.IsFalling()) { return 0.0; }
	const double Cover = Weather->AreCloudsEnabled() ? Weather->GetCloudSettings().Cover : 0.0;
	return FMath::Clamp(1.0 - 0.85 * Cover, 0.0, 1.0);
}

void UGeoSkySubsystem::ApplyParameters(const FQuat& LocalToWorld)
{
	UWorld* World = GetWorld();
	UMaterialParameterCollectionInstance* Parameters = (World && WeatherCollection.IsValid()) ? World->GetParameterCollectionInstance(WeatherCollection.Get()) : nullptr;
	if (!Parameters) { return; }
	double SunLocal[3], MoonLocal[3];
	State.Bodies.Sun.ToNorthEastUp(SunLocal);
	State.Bodies.Moon.ToNorthEastUp(MoonLocal);
	const FVector SunWorld = LocalToWorld.RotateVector(FVector(SunLocal[0], SunLocal[1], SunLocal[2]));
	const FVector MoonWorld = LocalToWorld.RotateVector(FVector(MoonLocal[0], MoonLocal[1], MoonLocal[2]));
	// Le righe della rotazione mondo -> equatoriale: quelle locali girate nel mondo.
	double Rotation[3][3];
	Sky::HorizonToEquatorial(State.LatDeg, State.Bodies.LocalSiderealDeg, Rotation);
	const double Clear = CloudClearness();
	const double Windows = bCityLights ? State.CityLights * Sky::LitWindowShare(State.LocalHour) : 0.0;
	const double Stars = bNightSky ? State.StarVisibility * Clear : 0.0;
	bool bSet = Parameters->SetVectorParameterValue(TEXT("SkySun"), SkyVector(SunWorld, State.RelativeLight));
	bSet = Parameters->SetVectorParameterValue(TEXT("SkyMoon"), SkyVector(MoonWorld, State.Bodies.MoonRadiusDeg * Core::DegToRad)) && bSet;
	static const TCHAR* RowNames[3] = { TEXT("SkyEquatorX"), TEXT("SkyEquatorY"), TEXT("SkyEquatorZ") };
	for (int32 Row = 0; Row < 3; ++Row)
	{
		const FVector RowWorld = LocalToWorld.RotateVector(FVector(Rotation[Row][0], Rotation[Row][1], Rotation[Row][2]));
		bSet = Parameters->SetVectorParameterValue(RowNames[Row], SkyVector(RowWorld, 0.0)) && bSet;
	}
	bSet = Parameters->SetVectorParameterValue(TEXT("SkyNight"),
		FLinearColor(static_cast<float>(Stars), static_cast<float>(DayLux), static_cast<float>(Windows), static_cast<float>(Clear))) && bSet;
	bParametersMissing = !bSet;
}

void UGeoSkySubsystem::ApplyNightSky(const FVector& CameraLocation)
{
	UWorld* World = GetWorld();
	const bool bUsable = bNightSky && NightSkyMaterial.IsValid() && SphereMesh.IsValid() && !bParametersMissing;
	const bool bWanted = bUsable && (State.StarVisibility > 0.001 || State.Bodies.Moon.AltitudeDeg > -2.0);
	UStaticMeshComponent* Mesh = DomeMesh.Get();
	if (!Mesh && bWanted && World)
	{
		FActorSpawnParameters SpawnParameters;
		SpawnParameters.ObjectFlags |= RF_Transient;
		AActor* Actor = World->SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity, SpawnParameters);
		if (Actor)
		{
#if WITH_EDITOR
			Actor->SetActorLabel(TEXT("GeoWorld Cielo notturno"));
#endif
			Mesh = NewObject<UStaticMeshComponent>(Actor, TEXT("Sky"));
			Mesh->SetMobility(EComponentMobility::Movable);
			Mesh->SetStaticMesh(SphereMesh.Get());
			Mesh->SetMaterial(0, NightSkyMaterial.Get());
			Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			Mesh->SetCanEverAffectNavigation(false);
			Mesh->SetCastShadow(false);
			// Dietro a ogni altra cosa trasparente (pioggia, fulmini, acqua).
			Mesh->SetTranslucentSortPriority(-1000);
			Mesh->SetWorldScale3D(FVector(SkyDomeRadiusM * Units::MetersToUu / SkyBasicSphereRadiusUu));
			Actor->SetRootComponent(Mesh);
			Mesh->RegisterComponent();
			Dome = Actor;
			DomeMesh = Mesh;
		}
	}
	if (!Mesh) { return; }
	if (Mesh->IsVisible() != bWanted) { Mesh->SetVisibility(bWanted); }
	if (bWanted) { Mesh->SetWorldLocation(CameraLocation); }
}

// --- Rimettere a posto -----------------------------------------------------------------

void UGeoSkySubsystem::Restore()
{
	if (UDirectionalLightComponent* Sun = OriginalSun.Light.Get())
	{
		if (!SpawnedSun.IsValid())
		{
			Sun->SetWorldRotation(OriginalSun.Rotation);
			Sun->SetIntensity(OriginalSun.Intensity);
			Sun->SetCastShadows(OriginalSun.bCastShadows);
			Sun->SetAtmosphereSunLight(OriginalSun.bAtmosphereSunLight);
			Sun->SetDynamicShadowDistanceMovableLight(OriginalSun.ShadowDistance);
			if (Sun->Mobility != OriginalSun.Mobility) { Sun->SetMobility(OriginalSun.Mobility); }
		}
	}
	if (AExponentialHeightFog* Fog = OriginalFog.Fog.Get())
	{
		if (UExponentialHeightFogComponent* FogComponent = Fog->GetComponent())
		{
			FogComponent->SetFogInscatteringColor(OriginalFog.Inscattering);
			FogComponent->SetDirectionalInscatteringColor(OriginalFog.Directional);
		}
	}
	if (UWorld* World = GetWorld())
	{
		if (UMaterialParameterCollectionInstance* Parameters = WeatherCollection.IsValid() ? World->GetParameterCollectionInstance(WeatherCollection.Get()) : nullptr)
		{
			// Niente finestre accese, niente stelle.
			Parameters->SetVectorParameterValue(TEXT("SkyNight"), FLinearColor(0.0f, static_cast<float>(DayLux), 0.0f, 1.0f));
		}
	}
	// Quello che il cielo ha creato (transient: non finisce mai nel livello salvato).
	TArray<AActor*, TInlineAllocator<4>> Created;
	Created.Add(SpawnedSun.Get());
	Created.Add(MoonLight.Get());
	Created.Add(Exposure.Get());
	Created.Add(Dome.Get());
	for (AActor* Actor : Created)
	{
		if (Actor) { Actor->Destroy(); }
	}
	SpawnedSun.Reset();
	MoonLight.Reset();
	Exposure.Reset();
	Dome.Reset();
	DomeMesh.Reset();
	OriginalSun = FOriginalSun();
	OriginalFog = FOriginalFog();
	LastSunDirection = FVector::ZeroVector;
	LastMoonDirection = FVector::ZeroVector;
	LastCaptureDirection = FVector::ZeroVector;
	bSunShadows = true;
	bApplied = false;
}

// --- L'overlay ------------------------------------------------------------------------

void UGeoSkySubsystem::DrawDebugOverlay() const
{
	if (!GEngine) { return; }
	int32 MessageKey = 0x7F60;
	auto Line = [&MessageKey](const FColor& Colour, const FString& Text)
	{
		GEngine->AddOnScreenDebugMessage(MessageKey++, 0.0f, Colour, Text);
	};
	Line(FColor::Cyan, TEXT("--- GeoWorld | cielo ---"));
	if (!bEnabled)
	{
		Line(FColor::Yellow, TEXT("Spento: il sole del livello resta com'e' (geo.Sky.Enable 1)"));
		return;
	}
	if (!State.bValid)
	{
		Line(FColor::Yellow, TEXT("In attesa della camera"));
		return;
	}
	const Sky::FSkyBodies& Bodies = State.Bodies;
	Line(FColor::White, FString::Printf(TEXT("Ora        : %s %s %s, velocita' x%g; a %.3f N %.3f E"),
		*State.LocalDate, *State.LocalClock, *State.ZoneName, Speed, State.LatDeg, State.LonDeg));
	Line(FColor::White, FString::Printf(TEXT("Sole       : azimut %.1f, altezza %.2f gradi; sorge %s, tramonta %s"),
		Bodies.Sun.AzimuthDeg, Bodies.Sun.AltitudeDeg, *State.Sunrise, *State.Sunset));
	Line(FColor::White, FString::Printf(TEXT("Luna       : %s %.0f%%, azimut %.1f, altezza %.2f gradi; sorge %s, tramonta %s"),
		Bodies.bMoonWaxing ? TEXT("crescente") : TEXT("calante"), 100.0 * Bodies.MoonIllumination,   /* percentuale, non-unita */
		Bodies.Moon.AzimuthDeg, Bodies.Moon.AltitudeDeg, *State.Moonrise, *State.Moonset));
	Line(FColor::White, FString::Printf(TEXT("Luce       : %.2e del giorno pieno (%.3g lux); notte %.2f, stelle %.2f, finestre %.2f"),
		State.RelativeLight, DayLux * State.RelativeLight, State.NightBlend, State.StarVisibility,
		bCityLights ? State.CityLights * Sky::LitWindowShare(State.LocalHour) : 0.0));
	const UDirectionalLightComponent* Sun = OriginalSun.Light.Get();
	Line(FColor::White, FString::Printf(TEXT("Scena      : sole '%s' %.3g lux, %s; luna %s; ombre %s"),
		Sun ? *GetNameSafe(Sun->GetOwner()) : TEXT("--"), DayLux,
		bHasAtmosphere ? TEXT("con SkyAtmosphere") : TEXT("senza SkyAtmosphere"),
		MoonLight.IsValid() ? TEXT("accesa") : TEXT("spenta"),
		SkyUsesVirtualShadowMaps() ? TEXT("Virtual Shadow Maps") : *FString::Printf(TEXT("a cascata fino a %.0f m"), ShadowDistanceM)));
	if (!Problem.IsEmpty()) { Line(FColor::Red, Problem); }
	if (!Advice.IsEmpty()) { Line(FColor::Yellow, Advice); }
}

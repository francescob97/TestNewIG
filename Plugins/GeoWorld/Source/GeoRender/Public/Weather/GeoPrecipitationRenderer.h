// =============================================================================
//  GeoPrecipitationRenderer.h -- Il meteo a schermo. STRATO: UNREAL.
//
//  Tre cose, tutte guidate da UGeoWeatherSubsystem una volta per frame:
//
//   1. LE PARTICELLE: una mesh di quadrati degeneri (Weather/Precipitation.h,
//      BuildPrecipitationMesh) su un attore transient che sta sempre sulla
//      camera. Il materiale M_GeoPrecipitation li apre e li muove sulla
//      scheda video: nessuna particella passa dalla CPU.
//   2. I PARAMETRI: MPC_GeoWeather, letta dalle particelle e dal terreno e
//      dalle strade (suolo bagnato, neve per quota).
//   3. LA NEBBIA: la precipitazione toglie visibilita'. Si usa la
//      ExponentialHeightFog del livello (o una nostra, se non c'e'):
//      si alza la densita' finche' piove e si rimettono i valori di prima
//      quando smette. Nell'editor la nebbia del livello NON si tocca: con
//      il livello salvato mentre piove, si salverebbero i valori della
//      pioggia. Li' la si usa solo se e' nostra.
// =============================================================================
#pragma once

#include "CoreMinimal.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/WeakObjectPtrTemplates.h"

#include "Weather/Precipitation.h"

class AActor;
class AExponentialHeightFog;
class UDynamicMeshComponent;
class UMaterialInterface;
class UMaterialParameterCollection;
class UWorld;

class GEORENDER_API FGeoPrecipitationRenderer
{
public:
	/** Quante particelle: 32.768 (65.536 triangoli), circa una ogni due metri cubi nella scatola. */
	static constexpr int32 ParticleCount = 32768;

	/** Lo stato del suolo per il terreno: bagnato, quota della neve, transizione, quanta neve. */
	struct FGroundParameters
	{
		double Wetness = 0.0;
		double SnowCoverM = 1.0e5;
		double SnowTransitionM = 150.0;
		double SnowAmount = 0.0;
		double GeoidAtOriginM = 47.0;
	};

	~FGeoPrecipitationRenderer();

	/**
	 * Una volta per frame. CameraLocation in unita' Unreal; VisibilityM la
	 * visibilita' della precipitazione attorno alla camera (un numero enorme
	 * se non cade niente); GroundZ la Z (unita' Unreal) del suolo sotto la
	 * camera: la nebbia della pioggia parte da li'.
	 */
	void Update(UWorld* World, const FVector& CameraLocation, const GeoWorld::Weather::FPrecipitationParticles& Particles,
	            const FGroundParameters& Ground, double VisibilityM, bool bDriveFog, double GroundZ);

	/** Via l'attore e la nebbia torna com'era. */
	void Shutdown();

	/** Vuoto se tutto c'e'; altrimenti cosa manca (da dire in giallo). */
	const FString& GetProblem() const { return Problem; }
	/** La densita' che la nebbia ha adesso per colpa della precipitazione (0 = non la tocchiamo). */
	double GetFogDensity() const { return AppliedFogDensity; }
	/** Quale nebbia si usa, in parole: per l'overlay. */
	const TCHAR* GetFogState() const { return FogState; }

private:
	bool EnsureAssets();
	bool EnsureActor(UWorld* World);
	void WriteParameters(UWorld* World, const GeoWorld::Weather::FPrecipitationParticles& Particles, const FGroundParameters& Ground);
	void UpdateFog(UWorld* World, double VisibilityM, bool bDriveFog, double GroundZ);
	void RestoreFog();

	TStrongObjectPtr<UMaterialInterface> Material;
	TStrongObjectPtr<UMaterialParameterCollection> Collection;
	bool bAssetsLookedUp = false;
	double LastAssetLookup = -1.0e9;

	TWeakObjectPtr<AActor> Actor;
	TWeakObjectPtr<UDynamicMeshComponent> Component;

	/** La nebbia che stiamo guidando, e come era prima. */
	TWeakObjectPtr<AExponentialHeightFog> Fog;
	bool bFogSpawned = false;
	bool bFogManaged = false;
	float OriginalFogDensity = 0.0f;
	float OriginalFogFalloff = 0.2f;
	double AppliedFogDensity = 0.0;
	const TCHAR* FogState = TEXT("non serve");

	FString Problem;
};

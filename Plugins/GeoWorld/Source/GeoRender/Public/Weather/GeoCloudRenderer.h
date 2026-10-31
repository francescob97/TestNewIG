// =============================================================================
//  GeoCloudRenderer.h -- Le nuvole a schermo. STRATO: UNREAL.
//
//  Le nuvole le disegna la VolumetricCloud di Unreal: un guscio sferico
//  attorno al pianeta, attraversato a passi dalla scheda video, che a ogni
//  passo chiede al MATERIALE quanto e' densa l'aria li'. Il materiale e'
//  M_GeoClouds, e la sua densita' e' Weather/CloudShader.inl: lo stesso testo
//  che i test compilano come C++.
//
//  Cosa fa questa classe, una volta per frame (la guida UGeoWeatherSubsystem):
//
//   - LA MAPPA METEO (Weather/Clouds.h, BuildCloudMap): 256 x 256 texel, 160
//     km attorno alla camera, calcolata su un thread di lavoro ogni 8 secondi
//     (o subito, se la camera si e' spostata di 20 km o il tempo e' cambiato).
//     Quando e' pronta diventa una texture e la si da' all'istanza del
//     materiale (parametro "CloudWeatherMap"): le collezioni di parametri non
//     portano texture.
//   - I PARAMETRI (MPC_GeoWeather: CloudMap, CloudShift, CloudLayer,
//     CloudLook, CloudFlash, CloudFair): dove sta la mappa negli assi di
//     Unreal (cambia con il rebase), quanto il vento l'ha spostata da quando
//     e' stata fatta, lo strato, il lampo di un fulmine dentro la nuvola.
//   - LO STRATO della VolumetricCloud (base e spessore, in km): quello che il
//     motore attraversa. Fuori non guarda, quindi deve contenere tutto.
//
//  QUALE VolumetricCloud. Una sola si vede per volta, quindi se ne usa una
//  NOSTRA, transient (non finisce nel livello salvato). Se il livello ne ha
//  gia' una: in Play la si nasconde finche' le nuvole sono di GeoWorld (e
//  torna con "geo.Weather.Clouds off"); nell'EDITOR non si tocca -- salvando
//  il livello si salverebbe nascosta -- e lo si dice.
//
//  NOTA UE: della VolumetricCloud si usano solo SetMaterial, SetVisibility,
//  SetPlanetRadius, SetLayerBottomAltitude e SetLayerHeight (in km). Un
//  componente nascosto non entra nella scena (CreateRenderState_Concurrent
//  guarda la visibilita'): e' cosi' che quella del livello si fa da parte.
// =============================================================================
#pragma once

#include "CoreMinimal.h"
#include "Tasks/Task.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/WeakObjectPtrTemplates.h"

#include <atomic>

#include "Georeference/GeoreferenceSnapshot.h"
#include "Weather/Clouds.h"

class AActor;
class UMaterialInstanceDynamic;
class UMaterialInterface;
class UMaterialParameterCollection;
class UTexture2D;
class UVolumetricCloudComponent;
class UWorld;

class GEORENDER_API FGeoCloudRenderer
{
public:
	/** La mappa meteo: lato in texel e in metri (un texel = 625 m). */
	static constexpr int32 MapSize = 256;
	static constexpr double MapExtentM = 160000.0;
	/** Si rifa' ogni tanti secondi, o quando la camera si e' allontanata tanto dal suo centro. */
	static constexpr double MapRefreshSeconds = 8.0;
	static constexpr double MapRefreshDistanceM = 20000.0;

	/** Quello che serve per un frame. */
	struct FInputs
	{
		GeoWorld::Weather::FCloudSettings Settings;
		/** La precipitazione: le nubi di pioggia vengono dalla sua mappa. */
		const GeoWorld::Weather::FPrecipitationField* Field = nullptr;
		/** La camera (per centrare la mappa); senza, la mappa resta dov'e'. */
		bool bHaveCamera = false;
		double CameraLatDeg = 0.0, CameraLonDeg = 0.0;
		/** Il geoide all'origine: la quota sul mare (come per il suolo bagnato). */
		double GeoidAtOriginM = 0.0;
		/** L'orologio delle nuvole (s): fa evolvere i cumuli. */
		double TimeSeconds = 0.0;
		/** Il lampo di un fulmine dentro la nuvola: dove (unita' Unreal) e quanto (0..1, FlashBrightness). */
		bool bFlash = false;
		FVector FlashLocation = FVector::ZeroVector;
		double FlashBrightness = 0.0;
	};

	~FGeoCloudRenderer();

	void Update(UWorld* World, const FGeoreferenceSnapshot& Snapshot, const FInputs& Inputs);

	/** Via le nuvole: la nostra VolumetricCloud sparisce, quella del livello torna com'era. */
	void Shutdown();

	/** Vuoto se tutto c'e'; altrimenti cosa manca (da dire in giallo). */
	const FString& GetProblem() const { return Problem; }
	/** Un consiglio, se c'e' (la luce che non fa ombre di nuvole, nessuna SkyAtmosphere). */
	const FString& GetAdvice() const { return Advice; }
	/** Quale VolumetricCloud si usa, in parole: per l'overlay. */
	const TCHAR* GetState() const { return State; }
	/** Da quanti secondi la mappa e' quella di adesso (negativo: nessuna ancora). */
	double GetMapAge(double TimeSeconds) const { return bHaveMap ? TimeSeconds - MapBuiltAt : -1.0; }
	/** Lo strato che il motore attraversa adesso, m sull'ellissoide (per l'overlay). */
	void GetLayer(double& OutBottomM, double& OutTopM) const { OutBottomM = LayerBottomM; OutTopM = LayerTopM; }

private:
	/** La mappa fatta dal thread di lavoro: si legge solo con bReady. */
	struct FMapJob
	{
		std::atomic<bool> bReady{ false };
		GeoWorld::Weather::FCloudMap Map;
	};

	bool EnsureAssets(UWorld* World);
	/** La nostra VolumetricCloud (nascondendo quella del livello, in Play); nullptr nell'editor se il livello ne ha una. */
	UVolumetricCloudComponent* EnsureCloud(UWorld* World);
	void ReleaseCloud();
	void StartMap(const FInputs& Inputs);
	void FinishMap();
	void WriteParameters(UWorld* World, const FGeoreferenceSnapshot& Snapshot, const FInputs& Inputs);
	/** Ogni due secondi: c'e' una SkyAtmosphere? la luce del sole fa le ombre delle nuvole? */
	void CheckScene(UWorld* World);

	TStrongObjectPtr<UMaterialInterface> Material;
	TStrongObjectPtr<UMaterialParameterCollection> Collection;
	TStrongObjectPtr<UMaterialInstanceDynamic> Instance;
	double LastAssetLookup = -1.0e9;
	double LastSceneCheck = -1.0e9;

	/** La nostra VolumetricCloud, e quella del livello che si e' nascosta (in Play). */
	TWeakObjectPtr<AActor> SpawnedActor;
	TWeakObjectPtr<UVolumetricCloudComponent> Cloud;
	TWeakObjectPtr<UVolumetricCloudComponent> LevelCloud;
	bool bLevelCloudHidden = false;
	double LastCloudSearch = -1.0e9;
	/** Lo strato dato al motore (km), per non ridarlo a ogni frame: ogni volta rifa' la scena delle nuvole. */
	float AppliedBottomKm = -1.0f, AppliedHeightKm = -1.0f;

	/** La mappa sulla scheda video, e quella che si sta facendo. */
	TStrongObjectPtr<UTexture2D> MapTexture;
	GeoWorld::Weather::FCloudMap CurrentMap;
	bool bHaveMap = false;
	double MapBuiltAt = 0.0;
	GeoWorld::Weather::FPrecipitationSettings MapSettings;
	TSharedPtr<FMapJob, ESPMode::ThreadSafe> Job;
	UE::Tasks::FTask JobTask;
	double JobStartedAt = 0.0;

	/** Il punto fisso a cui si ancora il disegno dei cumuli (MakeCloudShaderParameters). */
	bool bHaveReference = false;
	double ReferenceLatDeg = 0.0, ReferenceLonDeg = 0.0;

	double LayerBottomM = 0.0, LayerTopM = 0.0;
	const TCHAR* State = TEXT("spente");
	FString Problem;
	/** Il materiale vecchio (senza "Used with Volumetric Cloud") si dice a schermo una volta sola. */
	bool bReportedOldMaterial = false;
	FString Advice;
};

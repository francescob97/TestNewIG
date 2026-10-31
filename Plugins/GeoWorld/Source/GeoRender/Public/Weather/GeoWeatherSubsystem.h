// =============================================================================
//  GeoWeatherSubsystem.h -- Il meteo del mondo. STRATO: UNREAL.
//
//  Il VENTO, simulato (Weather/WindField.h): un campo che in ogni punto e a
//  ogni quota da' un vettore (nord, est, su) in m/s, con lo strato limite, la
//  rugosita' (acqua o terra), i rilievi e le raffiche che corrono sul
//  paesaggio. Qui gli si danno il terreno e l'acqua che sono in memoria, e lo
//  si espone a chi lo usa: le onde (il vento sul mare sotto la camera), le
//  stampe a schermo, le frecce del debug, e chiunque chiami
//  GetWindNorthEastUp (Blueprint, attori, domani le entita' della Fase 7).
//
//  La PRECIPITAZIONE (Weather/Precipitation.h): pioggia, neve e grandine, in
//  rovesci che viaggiano con il vento in quota; cosa cade lo decide la
//  temperatura con la quota. Il suolo si bagna e si imbianca, le particelle
//  cadono attorno alla camera, la visibilita' cala (GeoPrecipitationRenderer).
//  Il suolo gela (il ghiaccio, sopra lo zero termico: Precipitation.h,
//  FGroundWeather) e la grandine si posa in strisce sotto i nuclei dei
//  temporali, poi fonde (GroundHail.h, GeoHailMap).
//
//  I FULMINI (Weather/Lightning.h): nei nuclei dei temporali, nube-suolo o
//  dentro la nube, con il lampo che sfarfalla, la luce sul paesaggio e il
//  tuono che arriva dopo (GeoLightningRenderer).
//
//  LE NUVOLE (Weather/Clouds.h): i cumuli di bel tempo, quanti ne vuoi, e le
//  nubi di pioggia, che vengono dalla stessa mappa dei rovesci -- dove piove
//  c'e' sempre la nuvola sopra. Le disegna la VolumetricCloud di Unreal con
//  il materiale M_GeoClouds (GeoCloudRenderer); i fulmini nascono nella loro
//  base e le accendono da dentro.
//
//  geo.Weather.Wind 8 270           8 m/s da ovest (a 10 m su un prato, come un METAR)
//  geo.Weather.Turbulence 1.5       raffiche piu' forti del normale
//  geo.Weather.Variability 1        il tempo cambia da solo, in decine di minuti
//  geo.Weather.WindAloft 3000 25 290   vento in quota (s.l.m.); "clear" li toglie
//  geo.Weather.Rain 4               pioggia, 4 mm/h (anche Snow, Hail, Precipitation)
//  geo.Weather.Temperature 2        2 °C al livello del mare: neve dai ~0 m
//  geo.Weather.Storm                temporali: rovesci con fulmini
//  geo.Weather.Strike 1500          un fulmine a 1,5 km davanti alla camera
//  geo.Weather.Clouds 0.5 1200      cumuli su meta' del cielo, base a 1200 m ("off": niente nuvole)
//  geo.Weather.Ice 1                il suolo ghiacciato sopra lo zero termico, adesso
//  geo.Weather.HailGround 8 3       8 mm di grandine posata in 3 km attorno a te
//  geo.Weather.Clear                torna il sereno
//  geo.Weather.Debug 1              overlay; geo.Weather.Arrows 50: frecce a 50 m dal suolo
// =============================================================================
#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"

#include "Georeference/GeoreferenceSnapshot.h"
#include "Weather/Breeze.h"
#include "Weather/Clouds.h"
#include "Weather/GeoCloudRenderer.h"
#include "Weather/GeoHailMap.h"
#include "Weather/GeoLightningRenderer.h"
#include "Weather/GeoPrecipitationRenderer.h"
#include "Weather/Lightning.h"
#include "Weather/Precipitation.h"
#include "Weather/WindField.h"

#include "GeoWeatherSubsystem.generated.h"

/** Un fulmine: dove (unita' Unreal, il punto colpito o il suolo sotto il lampo), se e' arrivato a terra, a che distanza dalla camera (m). */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FGeoLightningStrikeSignature, FVector, WorldLocation, bool, bCloudToGround, float, DistanceM);

/** La precipitazione in un punto, per i Blueprint. */
USTRUCT(BlueprintType)
struct FGeoPrecipitationInfo
{
	GENERATED_BODY()

	/** Quanta acqua cade, mm/h (0 = niente). */
	UPROPERTY(BlueprintReadOnly, Category = "GeoWorld|Meteo")
	float RateMmH = 0.0f;
	/** Le frazioni di pioggia, neve e grandine: sommano a 1 se cade qualcosa. */
	UPROPERTY(BlueprintReadOnly, Category = "GeoWorld|Meteo")
	float Rain = 0.0f;
	UPROPERTY(BlueprintReadOnly, Category = "GeoWorld|Meteo")
	float Snow = 0.0f;
	UPROPERTY(BlueprintReadOnly, Category = "GeoWorld|Meteo")
	float Hail = 0.0f;
	/** La temperatura dell'aria, °C. */
	UPROPERTY(BlueprintReadOnly, Category = "GeoWorld|Meteo")
	float TemperatureC = 15.0f;
	/** Quanto lontano si vede per colpa della precipitazione, m (un numero enorme se non cade niente). */
	UPROPERTY(BlueprintReadOnly, Category = "GeoWorld|Meteo")
	float VisibilityM = 1.0e9f;
	/** Il suolo li': quanto e' ghiacciato e quanto e' coperto di grandine, 0..1 (per l'aderenza di un veicolo, un suono di passi). */
	UPROPERTY(BlueprintReadOnly, Category = "GeoWorld|Meteo")
	float GroundIce = 0.0f;
	UPROPERTY(BlueprintReadOnly, Category = "GeoWorld|Meteo")
	float GroundHail = 0.0f;
};

UCLASS()
class GEORENDER_API UGeoWeatherSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual bool IsTickableInEditor() const override { return true; }

	// --- Il tempo che fa ----------------------------------------------------

	/** Il vento di riferimento: velocita' a 10 m su un prato (m/s), direzione DA CUI soffia (gradi). */
	void SetWind(double SpeedMs, double FromDegrees);
	/** 0 = aria liscia, 1 = la turbolenza dell'atmosfera vera, fino a 2. */
	void SetTurbulence(double Scale);
	/** 0 = il tempo non cambia, 1 = velocita' e direzione derivano da sole in decine di minuti. */
	void SetVariability(double Amount);
	void SetSeed(uint32 Seed);
	/** Uno strato di vento in quota, s.l.m.: sostituisce quello entro 100 m di quota. */
	void SetWindAloft(double AltitudeM, double SpeedMs, double FromDegrees);
	void ClearWindAloft();
	const GeoWorld::Weather::FWindSettings& GetSettings() const { return Field.GetSettings(); }

	// --- Le brezze (Weather/Breeze.h) ----------------------------------------

	/** Le brezze di mare, di pendio e di valle (default si'). Seguono l'orologio del cielo. */
	void SetBreezesEnabled(bool bInEnabled) { BreezeSettings.bEnabled = bInEnabled; }
	bool AreBreezesEnabled() const { return BreezeSettings.bEnabled; }
	/** 1 come in natura, 0 nessuna, fino a 3. */
	void SetBreezeStrength(double Strength) { BreezeSettings.Strength = FMath::Clamp(Strength, 0.0, 3.0); }
	double GetBreezeStrength() const { return BreezeSettings.Strength; }
	const GeoWorld::Weather::FBreezeClimate& GetBreezeClimate() const { return BreezeClimate; }
	/** La mappa dell'acqua attorno alla camera c'e' (serve il dataset dell'acqua per la brezza di mare). */
	bool HasWaterMap() const { return WaterMap.IsBuilt(); }
	/** Le brezze qui sotto a 10 m, in parole ("di mare 3,4 m/s da 270"); vuota se non ce ne sono. */
	FString DescribeBreezes(double MinSpeedMs = 0.5) const;

	// --- Il vento -----------------------------------------------------------

	/** Il vento di riferimento adesso, con la deriva della variabilita'. */
	GeoWorld::Weather::FWind GetReferenceWind() const { return Field.ReferenceWind(); }
	/** Il vento MEDIO a 10 m sul mare aperto, sotto la camera: ci crescono le onde. */
	GeoWorld::Weather::FWind GetSeaWind() const { return SeaWind; }
	/** Lo stesso con la raffica di adesso: l'increspatura e la schiuma. */
	GeoWorld::Weather::FWind GetSeaGust() const { return SeaGust; }

	/**
	 * Il vento in un punto (quota ELLISSOIDICA in metri), con il terreno e
	 * l'acqua che sono in memoria: senza quote il suolo e' piatto al livello
	 * del mare, senza acqua e' terra.
	 */
	GeoWorld::Weather::FWindSample SampleWind(double LatDeg, double LonDeg, double HeightM) const;

	/** (nord, est, su) in m/s nel punto del mondo dato. Per i Blueprint e gli attori. */
	UFUNCTION(BlueprintCallable, Category = "GeoWorld|Meteo")
	FVector GetWindNorthEastUp(const FVector& WorldLocation) const;

	/** Il vento sotto la camera: alla sua quota e a 10 m dal suolo. */
	struct FObserverWind
	{
		bool bValid = false;
		double LatDeg = 45.0, LonDeg = 0.0;
		/** Quote ELLISSOIDICHE della camera e del suolo sotto, e il geoide li'. */
		double HeightM = 0.0;
		double GroundHeightM = 0.0;
		double GeoidM = 0.0;
		double HeightAboveGroundM = 0.0;
		bool bOverWater = false;
		GeoWorld::Weather::FSurfaceInfluence Surface;
		GeoWorld::Weather::FWindSample AtCamera;
		GeoWorld::Weather::FWindSample AtTenMetres;
	};
	const FObserverWind& GetObserverWind() const { return Observer; }

	// --- La precipitazione ---------------------------------------------------

	/**
	 * Cosa cade e quanto: intensita' nel cuore della precipitazione (mm/h),
	 * quanta parte del suolo copre (1 = dappertutto, 0,3 = rovesci sparsi),
	 * quanto sono grandi i rovesci (km). Intensita' 0 = sereno.
	 */
	void SetPrecipitation(GeoWorld::Weather::EPrecipitationKind Kind, double RateMmH, double Coverage, double CellSizeKm);
	void ClearPrecipitation();
	/** La temperatura al livello del mare (°C) e quanto cala salendo (°C/km): decide pioggia o neve. */
	void SetTemperature(double SeaLevelC, double LapseRateCPerKm);
	const GeoWorld::Weather::FPrecipitationSettings& GetPrecipitationSettings() const { return Precipitation.GetSettings(); }
	const GeoWorld::Weather::FPrecipitationField& GetPrecipitationField() const { return Precipitation; }

	/** Il suolo: quanto e' bagnato (0..1) e da che quota in su e' bianco (m s.l.m.). */
	void SetWetness(double Wetness) { GroundState.SetWetness(Wetness); }
	void SetSnowCover(double AltitudeM) { GroundState.SetSnowCover(AltitudeM); }
	void ClearSnowCover() { GroundState.ClearSnowCover(); }
	/** Il ghiaccio adesso: quanto (0..1) e sopra che quota (m s.l.m.). Poi segue il tempo da solo. */
	void SetIce(double Amount, double AltitudeM) { GroundState.SetIce(Amount, AltitudeM); }
	void ClearIce() { GroundState.ClearIce(); }

	/** Grandine posata a mano: un disco attorno alla camera (raggio m, strato mm). False senza camera. */
	bool AddHailOnGround(double RadiusM, double DepthMm);
	void ClearHailOnGround() { if (HailMap) { HailMap->Clear(); } }
	/** Quanto e' bianco di grandine il suolo in un punto, 0..1. */
	double GetHailCoverAt(double LatDeg, double LonDeg) const { return HailMap ? HailMap->CoverAt(LatDeg, LonDeg) : 0.0; }
	/** Lo strato piu' spesso di grandine posata, mm (0: niente). */
	double GetHailMaxDepth() const { return HailMap ? HailMap->MaxDepth() : 0.0; }
	/** Il ghiaccio in un punto a una quota sul mare, 0..1. */
	double GetIceAt(double AltitudeM) const;
	const GeoWorld::Weather::FGroundWeather& GetGround() const { return GroundState; }

	/** La precipitazione toglie visibilita' con la nebbia del livello (default si'). */
	void SetFogEnabled(bool bInEnabled) { bDriveFog = bInEnabled; }
	bool IsFogEnabled() const { return bDriveFog; }

	/** La precipitazione nel punto del mondo dato. Per i Blueprint e gli attori. */
	UFUNCTION(BlueprintCallable, Category = "GeoWorld|Meteo")
	FGeoPrecipitationInfo GetPrecipitationAt(const FVector& WorldLocation) const;

	/** La precipitazione attorno alla camera: alla sua quota e al suolo sotto. */
	struct FObserverPrecipitation
	{
		GeoWorld::Weather::FPrecipitationSample AtCamera;
		GeoWorld::Weather::FPrecipitationSample AtGround;
		/** La visibilita' attorno alla camera, smussata (la nebbia non salta), m. */
		double VisibilityM = 1.0e9;
	};
	const FObserverPrecipitation& GetObserverPrecipitation() const { return ObserverPrecipitation; }
	/** Vuoto se le particelle e il suolo si vedono; altrimenti cosa manca. */
	FString GetRenderProblem() const { return Renderer ? Renderer->GetProblem() : FString(); }
	/** La densita' che la nebbia ha per colpa della precipitazione (0 = non la si tocca). */
	double GetFogDensity() const { return Renderer ? Renderer->GetFogDensity() : 0.0; }

	// --- I fulmini -----------------------------------------------------------

	/** 0 = niente fulmini, 1 = un temporale normale, fino a 5. Servono i rovesci (copertura sotto 1). */
	void SetLightningActivity(double Activity);
	const GeoWorld::Weather::FLightningSettings& GetLightningSettings() const { return Lightning.GetSettings(); }
	/** Un fulmine adesso, a DistanceM davanti alla camera: per provare senza aspettare un temporale. */
	bool ForceStrike(double DistanceM, bool bCloudToGround);
	void SetThunderVolume(double Volume) { if (LightningRenderer) { LightningRenderer->SetThunderVolume(Volume); } }
	double GetThunderVolume() const { return LightningRenderer ? LightningRenderer->GetThunderVolume() : 0.0; }

	/** Ogni fulmine, per i Blueprint: un suono, un effetto, una telecamera che trema. */
	UPROPERTY(BlueprintAssignable, Category = "GeoWorld|Meteo")
	FGeoLightningStrikeSignature OnLightningStrike;

	/** Per le stampe: quanti nell'ultimo minuto, e l'ultimo. */
	struct FLightningReport
	{
		int32 LastMinute = 0;
		bool bAny = false;
		bool bCloudToGround = true;
		double DistanceM = 0.0;
		double AgeSeconds = 0.0;
		/** Fra quanto arriva il prossimo tuono (s); negativo se nessuno. */
		double ThunderInSeconds = -1.0;
	};
	FLightningReport GetLightningReport() const;

	// --- Le nuvole -----------------------------------------------------------

	/**
	 * I cumuli di bel tempo: quanta parte del cielo coprono (0..1), la base
	 * (m s.l.m.) e quanto sono alti i piu' grossi (m). Le nubi di pioggia non
	 * si impostano: vengono dalla precipitazione.
	 */
	void SetClouds(double Cover, double BaseM, double ThicknessM);
	/** Quanto sono fitte le nuvole: 1 normale, 2 il doppio (piu' scure, piu' opache). */
	void SetCloudDensity(double Scale);
	/** Spente: GeoWorld non disegna nuvole, e la VolumetricCloud del livello torna com'era. */
	void SetCloudsEnabled(bool bInEnabled) { bCloudsEnabled = bInEnabled; }
	bool AreCloudsEnabled() const { return bCloudsEnabled; }
	const GeoWorld::Weather::FCloudSettings& GetCloudSettings() const { return CloudSettings; }
	/** Vuoto se le nuvole si vedono; altrimenti cosa manca. */
	FString GetCloudProblem() const { return CloudRenderer ? CloudRenderer->GetProblem() : FString(); }
	/** Un consiglio sulla scena (la luce del sole, le ombre delle nuvole), se c'e'. */
	FString GetCloudAdvice() const { return CloudRenderer ? CloudRenderer->GetAdvice() : FString(); }

	// --- Debug --------------------------------------------------------------

	void SetDebugOverlayEnabled(bool bInEnabled) { bShowDebugOverlay = bInEnabled; }
	bool IsDebugOverlayEnabled() const { return bShowDebugOverlay; }
	/** Frecce del vento attorno alla camera, a questa quota sul suolo; 0 = spente. */
	void SetArrowHeight(double HeightAboveGroundM) { ArrowHeightM = FMath::Max(0.0, HeightAboveGroundM); }
	double GetArrowHeight() const { return ArrowHeightM; }

private:
	/**
	 * Quota del terreno in un punto, dalle tile in memoria. bFinest: la piu'
	 * fine (il suolo sotto un punto); altrimenti una grossolana (livello 11,
	 * post da ~75 m), liscia, per i rilievi. Al largo (tile virtuali) il
	 * suolo e' il mare: il geoide.
	 */
	bool TerrainHeightAt(double LonDeg, double LatDeg, bool bFinest, double& OutHeight) const;
	/** Rugosita' (acqua o terra), rilievi e brezze sotto un punto. */
	GeoWorld::Weather::FSurfaceInfluence SurfaceAt(double LatDeg, double LonDeg, bool& bOutOverWater) const;
	/** Lo stato termico con l'orologio del cielo, e la mappa dell'acqua attorno alla camera. */
	void UpdateBreezes();
	/** N del geoide, se il dataset d'acqua lo porta; 0 altrimenti. */
	double GeoidAt(double LonDeg, double LatDeg) const;
	void UpdateObserver();
	/** Rovesci, suolo, particelle e quel che si vede: dopo UpdateObserver. */
	void UpdatePrecipitation(double DeltaSeconds);
	/** I fulmini nati in questo frame, e la loro luce: dopo UpdatePrecipitation. */
	void UpdateLightning(double DeltaSeconds);
	void FireStrike(const GeoWorld::Weather::FLightningStrike& Strike);
	/** Le nuvole: dopo UpdateLightning (il lampo dentro la nuvola e' di questo frame). */
	void UpdateClouds(double DeltaSeconds);
	void OnGeoreferenceRebased(const FGeoreferenceSnapshot& Snapshot);
	void DrawDebugOverlay() const;
	void DrawPrecipitationOverlay() const;
	void DrawCloudOverlay() const;
	void DrawArrows() const;

	GeoWorld::Weather::FWindField Field;
	FObserverWind Observer;
	GeoWorld::Weather::FBreezeSettings BreezeSettings;
	GeoWorld::Weather::FBreezeClimate BreezeClimate;
	GeoWorld::Weather::FWaterMap WaterMap;
	double LastWaterMapClock = -100.0;
	GeoWorld::Weather::FWind SeaWind{ 5.0, 240.0 };
	GeoWorld::Weather::FWind SeaGust{ 5.0, 240.0 };
	bool bShowDebugOverlay = false;
	double ArrowHeightM = 0.0;

	/** La camera in unita' Unreal, se c'e'. */
	FVector ViewLocation = FVector::ZeroVector;

	GeoWorld::Weather::FPrecipitationField Precipitation;
	/** Il suolo: bagnato e neve. (Non "Ground": e' il nome delle quote del suolo nelle funzioni.) */
	GeoWorld::Weather::FGroundWeather GroundState;
	GeoWorld::Weather::FPrecipitationParticles Particles;
	FObserverPrecipitation ObserverPrecipitation;
	/** Il coefficiente di estinzione smussato (1/m): la visibilita' cambia in qualche secondo. */
	double SmoothedExtinction = 0.0;
	bool bDriveFog = true;
	TUniquePtr<FGeoPrecipitationRenderer> Renderer;
	/** La grandine posata: la griglia al suolo e la sua texture. */
	TUniquePtr<FGeoHailMap> HailMap;
	/** Il geoide all'origine del mondo: la quota sul mare nei materiali (suolo, nuvole). */
	double GeoidAtOriginM = 0.0;

	FGeoreferenceSnapshot LastSnapshot;
	bool bHaveSnapshot = false;
	FDelegateHandle RebaseHandle;

	GeoWorld::Weather::FLightningScheduler Lightning;
	TUniquePtr<FGeoLightningRenderer> LightningRenderer;
	/** Quando sono caduti i fulmini dell'ultimo minuto (orologio dello scheduler). */
	TArray<double> RecentStrikeTimes;
	GeoWorld::Weather::FLightningStrike LatestStrike;
	bool bHaveLatestStrike = false;
	double LatestStrikeDistanceM = 0.0;

	GeoWorld::Weather::FCloudSettings CloudSettings;
	bool bCloudsEnabled = true;
	TUniquePtr<FGeoCloudRenderer> CloudRenderer;
	/** L'orologio delle nuvole (s): i cumuli nascono e si disfano. */
	double CloudClock = 0.0;
};

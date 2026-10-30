// =============================================================================
//  GeoWeatherSubsystem.h -- Il meteo del mondo. STRATO: UNREAL.
//
//  Oggi c'e' il VENTO, simulato (Weather/WindField.h): un campo che in ogni
//  punto e a ogni quota da' un vettore (nord, est, su) in m/s, con lo strato
//  limite, la rugosita' (acqua o terra), i rilievi e le raffiche che corrono
//  sul paesaggio. Qui gli si danno il terreno e l'acqua che sono in memoria,
//  e lo si espone a chi lo usa: le onde (il vento sul mare sotto la camera),
//  le stampe a schermo, le frecce del debug, e chiunque chiami
//  GetWindNorthEastUp (Blueprint, attori, domani le entita' della Fase 7).
//
//  Nuvole, pioggia, nebbia, temperatura si aggiungeranno qui (Fase 12).
//
//  geo.Weather.Wind 8 270           8 m/s da ovest (a 10 m su un prato, come un METAR)
//  geo.Weather.Turbulence 1.5       raffiche piu' forti del normale
//  geo.Weather.Variability 1        il tempo cambia da solo, in decine di minuti
//  geo.Weather.WindAloft 3000 25 290   vento in quota (s.l.m.); "clear" li toglie
//  geo.Weather.Debug 1              overlay; geo.Weather.Arrows 50: frecce a 50 m dal suolo
// =============================================================================
#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"

#include "Weather/WindField.h"

#include "GeoWeatherSubsystem.generated.h"

UCLASS()
class GEORENDER_API UGeoWeatherSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
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
		double GroundHeightM = 0.0;
		double HeightAboveGroundM = 0.0;
		bool bOverWater = false;
		GeoWorld::Weather::FSurfaceInfluence Surface;
		GeoWorld::Weather::FWindSample AtCamera;
		GeoWorld::Weather::FWindSample AtTenMetres;
	};
	const FObserverWind& GetObserverWind() const { return Observer; }

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
	/** Rugosita' (acqua o terra) e rilievi sotto un punto. */
	GeoWorld::Weather::FSurfaceInfluence SurfaceAt(double LatDeg, double LonDeg, bool& bOutOverWater) const;
	/** N del geoide, se il dataset d'acqua lo porta; 0 altrimenti. */
	double GeoidAt(double LonDeg, double LatDeg) const;
	void UpdateObserver();
	void DrawDebugOverlay() const;
	void DrawArrows() const;

	GeoWorld::Weather::FWindField Field;
	FObserverWind Observer;
	GeoWorld::Weather::FWind SeaWind{ 5.0, 240.0 };
	GeoWorld::Weather::FWind SeaGust{ 5.0, 240.0 };
	bool bShowDebugOverlay = false;
	double ArrowHeightM = 0.0;
};

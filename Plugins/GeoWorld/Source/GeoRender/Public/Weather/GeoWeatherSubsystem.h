// =============================================================================
//  GeoWeatherSubsystem.h -- Il meteo del mondo. STRATO: UNREAL.
//
//  Oggi c'e' solo il VENTO (velocita', direzione, raffiche), e lo legge
//  l'acqua per le onde. E' il seme della fase del meteo: nuvole, pioggia,
//  nebbia, temperatura si aggiungeranno qui, e chi ne ha bisogno (il cielo,
//  la vegetazione, l'acqua) le leggera' da qui, come oggi il vento.
//
//  geo.Weather.Wind 8 270       8 m/s da ovest
//  geo.Weather.Wind 15 180 0.4  15 m/s da sud, raffiche del 40%
// =============================================================================
#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"

#include "Weather/Wind.h"

#include "GeoWeatherSubsystem.generated.h"

UCLASS()
class GEORENDER_API UGeoWeatherSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual bool IsTickableInEditor() const override { return true; }

	/** Il vento medio: velocita' in m/s a 10 m, direzione DA CUI soffia in gradi. */
	void SetWind(double SpeedMs, double FromDegrees);
	/** Raffiche: 0 = vento costante, 0,3 = +-30% attorno alla media. */
	void SetGustiness(double Fraction) { Gustiness = FMath::Clamp(Fraction, 0.0, 1.0); }

	/** Il vento di adesso, con le raffiche. */
	GeoWorld::Weather::FWind GetWind() const { return Current; }
	GeoWorld::Weather::FWind GetMeanWind() const { return Mean; }
	double GetGustiness() const { return Gustiness; }

private:
	/** Default: un libeccio leggero, 5 m/s da sud-ovest, con un po' di raffiche. */
	GeoWorld::Weather::FWind Mean{ 5.0, 240.0 };
	GeoWorld::Weather::FWind Current{ 5.0, 240.0 };
	double Gustiness = 0.2;
	double Seconds = 0.0;
};

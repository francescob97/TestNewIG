#include "Weather/GeoWeatherSubsystem.h"

#include "Engine/World.h"

bool UGeoWeatherSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	if (!Super::ShouldCreateSubsystem(Outer)) { return false; }
	const UWorld* World = Cast<UWorld>(Outer);
	return World && (World->WorldType == EWorldType::Game
	              || World->WorldType == EWorldType::PIE
	              || World->WorldType == EWorldType::Editor);
}

TStatId UGeoWeatherSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UGeoWeatherSubsystem, STATGROUP_Tickables);
}

void UGeoWeatherSubsystem::SetWind(double SpeedMs, double FromDegrees)
{
	Mean.SpeedMs = FMath::Clamp(SpeedMs, 0.0, 60.0);
	double Wrapped = FMath::Fmod(FromDegrees, 360.0);
	if (Wrapped < 0.0) { Wrapped += 360.0; }
	Mean.FromDegrees = Wrapped;
}

void UGeoWeatherSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	// In double: dopo giorni di gioco la precisione resta sotto il microsecondo.
	Seconds += DeltaTime;
	Current = GeoWorld::Weather::GustyWind(Mean, Gustiness, Seconds);
}

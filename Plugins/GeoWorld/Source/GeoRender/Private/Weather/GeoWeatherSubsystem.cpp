#include "Weather/GeoWeatherSubsystem.h"

#include "Geo/GeoFormat.h"
#include "Geo/GeoUnits.h"
#include "Georeference/GeoreferenceSubsystem.h"
#include "Mesh/TileMesh.h"
#include "Sky/GeoSkySubsystem.h"
#include "Streaming/GeoTileStreamingSubsystem.h"
#include "Terrain/GeoTerrainSubsystem.h"
#include "Water/GeoWaterSubsystem.h"

#include "DrawDebugHelpers.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "HAL/PlatformTime.h"
#include "Misc/DateTime.h"

using namespace GeoWorld;
using GeoWorld::Weather::FWind;
using GeoWorld::Weather::FWindSample;
using GeoWorld::Weather::FSurfaceInfluence;
using GeoWorld::Weather::FPrecipitationSample;

namespace
{
	/** Il livello delle quote per i rilievi: post da ~75 m, lisci, quasi sempre in memoria. */
	constexpr int32 OrographyLevel = 11;
	/** Le frecce: 9 x 9 attorno alla camera, lunghe quanto il vento fa in 20 s. */
	constexpr int32 ArrowGrid = 9;
	constexpr double ArrowSeconds = 20.0;
	/** I rovesci viaggiano con il vento a questa quota (s.l.m.): ~700 hPa, il "vento guida" dei meteorologi. */
	constexpr double SteeringAltitudeM = 3000.0;
	/** La visibilita' segue la precipitazione in qualche secondo: la nebbia non scatta. */
	constexpr double VisibilitySeconds = 3.0;
	/** Koschmieder: visibilita' = 3,912 / estinzione (dove resta il 2% di contrasto). */
	constexpr double Koschmieder = 3.912;
	/** La mappa dell'acqua per le brezze: rifatta ogni tanto, o se la camera si allontana dal centro. */
	constexpr double WaterMapPeriodS = 5.0;
	constexpr double WaterMapRecentreM = 8000.0;
	/** I pendii (+-150 m) e le valli (+-2,5 km) per le brezze. */
	constexpr double BreezeSlopeBaseM = 150.0;
	constexpr double BreezeValleyBaseM = 2500.0;

	FWind WindOf(double North, double East)
	{
		FWind Wind;
		Wind.SpeedMs = FMath::Sqrt(North * North + East * East);
		double From = FMath::RadiansToDegrees(FMath::Atan2(-East, -North));
		if (From < 0.0) { From += 360.0; }
		Wind.FromDegrees = From;
		return Wind;
	}

	/** Blu (calma) -> verde (10 m/s) -> giallo (20) -> rosso (30 e oltre). */
	FColor SpeedColour(double SpeedMs)
	{
		const float T = static_cast<float>(FMath::Clamp(SpeedMs / 30.0, 0.0, 1.0));
		const FLinearColor Calm(0.1f, 0.3f, 1.0f), Fresh(0.1f, 0.9f, 0.2f), Strong(1.0f, 0.9f, 0.1f), Storm(1.0f, 0.15f, 0.1f);
		if (T < 1.0f / 3.0f) { return FLinearColor::LerpUsingHSV(Calm, Fresh, T * 3.0f).ToFColor(true); }
		if (T < 2.0f / 3.0f) { return FLinearColor::LerpUsingHSV(Fresh, Strong, T * 3.0f - 1.0f).ToFColor(true); }
		return FLinearColor::LerpUsingHSV(Strong, Storm, T * 3.0f - 2.0f).ToFColor(true);
	}

	FString Utf8(const std::string& Text) { return FString(UTF8_TO_TCHAR(Text.c_str())); }
	/** "2,5 °C" (il simbolo del grado arriva in UTF-8 da GeoFormat.h). */
	FString Celsius(double Value) { return Utf8(Format::Number(Value, 1) + " " + Format::Degree + "C"); }
}

bool UGeoWeatherSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	if (!Super::ShouldCreateSubsystem(Outer)) { return false; }
	const UWorld* World = Cast<UWorld>(Outer);
	return World && (World->WorldType == EWorldType::Game
	              || World->WorldType == EWorldType::PIE
	              || World->WorldType == EWorldType::Editor);
}

void UGeoWeatherSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	// Default: un libeccio leggero, 5 m/s da sud-ovest, con la turbolenza vera.
	Weather::FWindSettings Settings;
	Settings.Reference = FWind{ 5.0, 240.0 };
	Field.SetSettings(Settings);
	SeaWind = SeaGust = Field.SurfaceWind(45.0, Weather::RoughnessWater);

	// Il rebase sposta il mondo sotto le particelle: si segue (vedi
	// OnGeoreferenceRebased). Lo stato di partenza serve gia' al primo.
	Collection.InitializeDependency<UGeoreferenceSubsystem>();
	if (UGeoreferenceSubsystem* Georeference = GetWorld()->GetSubsystem<UGeoreferenceSubsystem>())
	{
		LastSnapshot = Georeference->GetSnapshot();
		bHaveSnapshot = true;
		RebaseHandle = Georeference->OnGeoreferenceRebased.AddUObject(this, &UGeoWeatherSubsystem::OnGeoreferenceRebased);
	}
	Renderer = MakeUnique<FGeoPrecipitationRenderer>();
	LightningRenderer = MakeUnique<FGeoLightningRenderer>();
	CloudRenderer = MakeUnique<FGeoCloudRenderer>();
	HailMap = MakeUnique<FGeoHailMap>();
}

void UGeoWeatherSubsystem::Deinitialize()
{
	if (UWorld* World = GetWorld())
	{
		if (UGeoreferenceSubsystem* Georeference = World->GetSubsystem<UGeoreferenceSubsystem>())
		{
			Georeference->OnGeoreferenceRebased.Remove(RebaseHandle);
		}
	}
	// Via le particelle, e la nebbia del livello torna com'era.
	if (Renderer) { Renderer->Shutdown(); }
	Renderer.Reset();
	if (LightningRenderer) { LightningRenderer->Shutdown(); }
	LightningRenderer.Reset();
	// Via le nostre nuvole, e quelle del livello tornano com'erano.
	if (CloudRenderer) { CloudRenderer->Shutdown(); }
	CloudRenderer.Reset();
	if (HailMap) { HailMap->Shutdown(); }
	HailMap.Reset();
	Super::Deinitialize();
}

TStatId UGeoWeatherSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UGeoWeatherSubsystem, STATGROUP_Tickables);
}

// ---------------------------------------------------------------------------
//  Le impostazioni
// ---------------------------------------------------------------------------

void UGeoWeatherSubsystem::SetWind(double SpeedMs, double FromDegrees)
{
	Weather::FWindSettings Settings = Field.GetSettings();
	Settings.Reference = FWind{ SpeedMs, FromDegrees };
	Field.SetSettings(Settings);
}

void UGeoWeatherSubsystem::SetTurbulence(double Scale)
{
	Weather::FWindSettings Settings = Field.GetSettings();
	Settings.Turbulence = Scale;
	Field.SetSettings(Settings);
}

void UGeoWeatherSubsystem::SetVariability(double Amount)
{
	Weather::FWindSettings Settings = Field.GetSettings();
	Settings.Variability = Amount;
	Field.SetSettings(Settings);
}

void UGeoWeatherSubsystem::SetSeed(uint32 Seed)
{
	Weather::FWindSettings Settings = Field.GetSettings();
	Settings.Seed = Seed;
	Field.SetSettings(Settings);
}

void UGeoWeatherSubsystem::SetWindAloft(double AltitudeM, double SpeedMs, double FromDegrees)
{
	Weather::FWindSettings Settings = Field.GetSettings();
	Settings.Aloft.erase(std::remove_if(Settings.Aloft.begin(), Settings.Aloft.end(),
		[AltitudeM](const Weather::FWindLayer& Layer) { return FMath::Abs(Layer.AltitudeM - AltitudeM) < 100.0; }),
		Settings.Aloft.end());
	Settings.Aloft.push_back(Weather::FWindLayer{ AltitudeM, FMath::Clamp(SpeedMs, 0.0, 150.0), FromDegrees });
	Field.SetSettings(Settings);
}

void UGeoWeatherSubsystem::ClearWindAloft()
{
	Weather::FWindSettings Settings = Field.GetSettings();
	Settings.Aloft.clear();
	Field.SetSettings(Settings);
}

void UGeoWeatherSubsystem::SetPrecipitation(Weather::EPrecipitationKind Kind, double RateMmH, double Coverage, double CellSizeKm)
{
	Weather::FPrecipitationSettings Settings = Precipitation.GetSettings();
	Settings.Kind = Kind;
	Settings.RateMmH = RateMmH;
	Settings.Coverage = Coverage;
	Settings.CellSizeKm = CellSizeKm;
	Precipitation.SetSettings(Settings);
}

void UGeoWeatherSubsystem::ClearPrecipitation()
{
	Weather::FPrecipitationSettings Settings = Precipitation.GetSettings();
	Settings.RateMmH = 0.0;
	Precipitation.SetSettings(Settings);
}

void UGeoWeatherSubsystem::SetClouds(double Cover, double BaseM, double ThicknessM)
{
	CloudSettings.Cover = FMath::Clamp(Cover, 0.0, 1.0);
	CloudSettings.BaseM = FMath::Clamp(BaseM, 100.0, 8000.0);
	CloudSettings.FairThicknessM = FMath::Clamp(ThicknessM, 100.0, 8000.0);
}

void UGeoWeatherSubsystem::SetCloudDensity(double Scale)
{
	CloudSettings.ExtinctionPerM = Weather::FCloudSettings().ExtinctionPerM * FMath::Clamp(Scale, 0.05, 10.0);
}

void UGeoWeatherSubsystem::SetTemperature(double SeaLevelC, double LapseRateCPerKm)
{
	Weather::FPrecipitationSettings Settings = Precipitation.GetSettings();
	Settings.SeaLevelTemperatureC = SeaLevelC;
	Settings.LapseRateCPerKm = LapseRateCPerKm;
	Precipitation.SetSettings(Settings);
}

// ---------------------------------------------------------------------------
//  Il suolo sotto il vento: quote, acqua, rilievi
// ---------------------------------------------------------------------------

double UGeoWeatherSubsystem::GeoidAt(double LonDeg, double LatDeg) const
{
	const UGeoWaterSubsystem* Water = GetWorld() ? GetWorld()->GetSubsystem<UGeoWaterSubsystem>() : nullptr;
	double Undulation = 0.0;
	return (Water && Water->GetGeoidUndulation(LonDeg, LatDeg, Undulation)) ? Undulation : 0.0;
}

bool UGeoWeatherSubsystem::TerrainHeightAt(double LonDeg, double LatDeg, bool bFinest, double& OutHeight) const
{
	const UGeoTileStreamingSubsystem* Heights = GetWorld() ? GetWorld()->GetSubsystem<UGeoTileStreamingSubsystem>() : nullptr;
	if (!Heights || !Heights->IsDatasetOpen()) { return false; }
	const FGeoTileDataset& Dataset = Heights->GetDataset();
	const int32 Coarse = FMath::Clamp(OrographyLevel, Dataset.GetMinLevel(), Dataset.GetMaxLevel());

	// Prima i livelli voluti (dal piu' fine, o dal grossolano in giu'); poi, per
	// i rilievi, anche quelli piu' fini, se il grossolano non e' in memoria.
	auto Try = [&](int32 Level) -> bool
	{
		uint32 X = 0, Y = 0;
		Tiles::TileForLonLat(static_cast<uint32>(Level), LonDeg, LatDeg, X, Y);
		const Tiles::FTileKey Key{ static_cast<uint32>(Level), X, Y };
		const UGeoTileStreamingSubsystem::FTilePtr Tile = Heights->PeekLoadedTile(Key);
		if (!Tile || !Tile->IsValid()) { return false; }
		// Al largo la tile e' virtuale e le sue quote sono il FONDALE: per il
		// vento il suolo e' il mare.
		if (Heights->IsVirtualTile(Key))
		{
			OutHeight = GeoidAt(LonDeg, LatDeg);
			return true;
		}
		OutHeight = Mesh::FSurfaceSampler(*Tile, 1).HeightAt(LonDeg, LatDeg);
		return true;
	};
	for (int32 Level = bFinest ? Dataset.GetMaxLevel() : Coarse; Level >= Dataset.GetMinLevel(); --Level)
	{
		if (Try(Level)) { return true; }
	}
	if (!bFinest)
	{
		for (int32 Level = Coarse + 1; Level <= Dataset.GetMaxLevel(); ++Level)
		{
			if (Try(Level)) { return true; }
		}
	}
	return false;
}

FSurfaceInfluence UGeoWeatherSubsystem::SurfaceAt(double LatDeg, double LonDeg, bool& bOutOverWater) const
{
	const UGeoWaterSubsystem* Water = GetWorld() ? GetWorld()->GetSubsystem<UGeoWaterSubsystem>() : nullptr;
	bOutOverWater = Water && Water->IsWaterAt(LonDeg, LatDeg);
	const double Roughness = bOutOverWater ? Weather::RoughnessWater : Field.GetSettings().LandRoughnessM;

	const FWind Gradient = Field.GradientWind(LatDeg);
	double TowardNorth = 0.0, TowardEast = 0.0;
	Gradient.Toward(TowardNorth, TowardEast);
	auto HeightAt = [this](double Lon, double Lat, double& OutHeight) { return TerrainHeightAt(Lon, Lat, false, OutHeight); };
	FSurfaceInfluence Surface = Weather::ComputeOrography(HeightAt, LatDeg, LonDeg, TowardNorth, TowardEast, Roughness);
	// L'acqua e' piana: niente vento verticale di pendio (il ridosso di una
	// scogliera sopravvento invece resta).
	if (bOutOverWater) { Surface.SlopeAlongWind = 0.0; }

	// Le brezze: la costa dalla mappa dell'acqua, i pendii e le valli dalle quote.
	if (BreezeSettings.bEnabled && BreezeClimate.IsValid())
	{
		Weather::FBreezeTerrain Terrain;
		Terrain.bOverWater = bOutOverWater;
		Terrain.bHaveWater = WaterMap.Sample(LatDeg, LonDeg, Terrain.WaterFraction, Terrain.WaterGradNorth, Terrain.WaterGradEast);
		const double MetresNorth = 111132.0, MetresEast = 111320.0 * FMath::Cos(FMath::DegreesToRadians(LatDeg));
		auto Rise = [&](double BaseM, double& OutNorth, double& OutEast)
		{
			double N1 = 0.0, N0 = 0.0, E1 = 0.0, E0 = 0.0;
			const bool bKnown = HeightAt(LonDeg, LatDeg + BaseM / MetresNorth, N1) && HeightAt(LonDeg, LatDeg - BaseM / MetresNorth, N0)
				&& HeightAt(LonDeg + BaseM / MetresEast, LatDeg, E1) && HeightAt(LonDeg - BaseM / MetresEast, LatDeg, E0);
			OutNorth = bKnown ? (N1 - N0) / (2.0 * BaseM) : 0.0;
			OutEast = bKnown ? (E1 - E0) / (2.0 * BaseM) : 0.0;
		};
		if (!bOutOverWater)
		{
			Rise(BreezeSlopeBaseM, Terrain.SlopeNorth, Terrain.SlopeEast);
			Rise(BreezeValleyBaseM, Terrain.ValleyNorth, Terrain.ValleyEast);
		}
		Weather::ComputeBreezes(BreezeClimate, Terrain, Field.ReferenceWind().SpeedMs, BreezeSettings, Surface.Breezes);
	}
	return Surface;
}

void UGeoWeatherSubsystem::UpdateBreezes()
{
	if (!BreezeSettings.bEnabled) { return; }
	UWorld* World = GetWorld();
	if (!World) { return; }
	// L'ora: quella del cielo (corre, salta, si ferma con lui); senza, l'orologio.
	const UGeoSkySubsystem* SkySubsystem = World->GetSubsystem<UGeoSkySubsystem>();
	const double JdUt = SkySubsystem ? SkySubsystem->GetTimeUt() : FDateTime::UtcNow().GetJulianDay();
	const double Cover = bCloudsEnabled ? CloudSettings.Cover : 0.0;
	const bool bRaining = ObserverPrecipitation.AtGround.IsFalling();
	BreezeClimate.Advance(JdUt, Observer.LatDeg, Observer.LonDeg, Cover, bRaining);

	// La mappa dell'acqua: solo con il dataset dell'acqua (senza, niente brezza di mare).
	const UGeoWaterSubsystem* Water = World->GetSubsystem<UGeoWaterSubsystem>();
	if (!Water || !Observer.bValid) { return; }
	const double Now = FPlatformTime::Seconds();
	const double MovedM = WaterMap.IsBuilt()
		? FMath::Sqrt(FMath::Square((Observer.LatDeg - WaterMap.GetCentreLat()) * 111132.0)
			+ FMath::Square((Observer.LonDeg - WaterMap.GetCentreLon()) * 111320.0 * FMath::Cos(FMath::DegreesToRadians(Observer.LatDeg))))
		: 0.0;
	if (Now - LastWaterMapClock < WaterMapPeriodS && MovedM < WaterMapRecentreM) { return; }
	LastWaterMapClock = Now;
	WaterMap.Build(Observer.LatDeg, Observer.LonDeg, [Water](double LonDeg, double LatDeg) { return Water->IsWaterAt(LonDeg, LatDeg); });
}

FString UGeoWeatherSubsystem::DescribeBreezes(double MinimumSpeedMs) const
{
	if (!BreezeSettings.bEnabled || !Observer.bValid) { return FString(); }
	static const TCHAR* Names[Weather::BreezeLayerCount][2] = {
		{ TEXT("di mare"), TEXT("di terra") }, { TEXT("di pendio, su"), TEXT("di pendio, giu'") }, { TEXT("di valle, su"), TEXT("di valle, giu'") } };
	FString Text;
	for (int32 Index = 0; Index < Weather::BreezeLayerCount; ++Index)
	{
		const Weather::FBreezeLayer& Layer = Observer.Surface.Breezes[Index];
		const double Share = Weather::BreezeProfile(Weather::ReferenceHeightM, Layer);
		const FWind Breeze = WindOf(Layer.North * Share, Layer.East * Share);
		if (Breeze.SpeedMs < MinimumSpeedMs) { continue; }
		// Quale delle due: la brezza di mare e' quella del giorno, come l'anabatica e quella che risale la valle.
		const bool bDay = Index == 0 ? BreezeClimate.Coast() > 0.0 : (Index == 1 ? Layer.SlopeUp > 0.0 : BreezeClimate.Valley() > 0.0);
		if (!Text.IsEmpty()) { Text += TEXT("; "); }
		Text += FString::Printf(TEXT("%s %s"), Names[Index][bDay ? 0 : 1], *Utf8(Format::Wind(Breeze.SpeedMs, Breeze.FromDegrees)));
	}
	return Text;
}

FWindSample UGeoWeatherSubsystem::SampleWind(double LatDeg, double LonDeg, double HeightM) const
{
	double Ground = 0.0;
	if (!TerrainHeightAt(LonDeg, LatDeg, true, Ground)) { Ground = GeoidAt(LonDeg, LatDeg); }
	bool bOverWater = false;
	const FSurfaceInfluence Surface = SurfaceAt(LatDeg, LonDeg, bOverWater);
	return Field.Sample(LatDeg, LonDeg, HeightM - Ground, HeightM - GeoidAt(LonDeg, LatDeg), Surface);
}

FVector UGeoWeatherSubsystem::GetWindNorthEastUp(const FVector& WorldLocation) const
{
	const UGeoreferenceSubsystem* Georeference = GetWorld() ? GetWorld()->GetSubsystem<UGeoreferenceSubsystem>() : nullptr;
	if (!Georeference) { return FVector::ZeroVector; }
	const Core::FGeodetic Here = Georeference->GetSnapshot().UnrealToGeodetic(WorldLocation);
	const FWindSample Sample = SampleWind(Here.LatRad / Core::DegToRad, Here.LonRad / Core::DegToRad, Here.HeightM);
	return FVector(Sample.North, Sample.East, Sample.Up);
}

FGeoPrecipitationInfo UGeoWeatherSubsystem::GetPrecipitationAt(const FVector& WorldLocation) const
{
	FGeoPrecipitationInfo Info;
	const UGeoreferenceSubsystem* Georeference = GetWorld() ? GetWorld()->GetSubsystem<UGeoreferenceSubsystem>() : nullptr;
	if (!Georeference) { return Info; }
	const Core::FGeodetic Where = Georeference->GetSnapshot().UnrealToGeodetic(WorldLocation);
	const double LatDeg = Where.LatRad / Core::DegToRad, LonDeg = Where.LonRad / Core::DegToRad;
	// La temperatura e la neve vanno con la quota sul MARE: si toglie il geoide.
	const FPrecipitationSample Sample = Precipitation.Sample(LatDeg, LonDeg, Where.HeightM - GeoidAt(LonDeg, LatDeg));
	Info.RateMmH = static_cast<float>(Sample.RateMmH);
	Info.Rain = static_cast<float>(Sample.Rain);
	Info.Snow = static_cast<float>(Sample.Snow);
	Info.Hail = static_cast<float>(Sample.Hail);
	Info.TemperatureC = static_cast<float>(Sample.TemperatureC);
	Info.VisibilityM = static_cast<float>(FMath::Min(Sample.VisibilityM, 1.0e9));
	Info.GroundIce = static_cast<float>(GetIceAt(Where.HeightM - GeoidAt(LonDeg, LatDeg)));
	Info.GroundHail = static_cast<float>(GetHailCoverAt(LatDeg, LonDeg));
	return Info;
}

double UGeoWeatherSubsystem::GetIceAt(double AltitudeM) const
{
	// Come il materiale (GroundShader.inl): sopra la quota del ghiaccio, con un passaggio.
	if (!GroundState.HasIce()) { return 0.0; }
	const double Transition = Weather::FGroundWeather::IceTransitionM;
	return GroundState.GetIce() * FMath::Clamp((AltitudeM - GroundState.GetIceLineM()) / Transition + 0.5, 0.0, 1.0);
}

bool UGeoWeatherSubsystem::AddHailOnGround(double RadiusM, double DepthMm)
{
	if (!HailMap || !Observer.bValid) { return false; }
	HailMap->AddDisc(Observer.LatDeg, Observer.LonDeg, FMath::Max(RadiusM, 1.0), FMath::Max(DepthMm, 0.0));
	return true;
}

// ---------------------------------------------------------------------------
//  A ogni frame
// ---------------------------------------------------------------------------

void UGeoWeatherSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	// Un frame lunghissimo (il debugger, un caricamento) non deve far correre
	// le raffiche di chilometri: si avanza al massimo di un secondo.
	const double Step = FMath::Min(static_cast<double>(DeltaTime), 1.0);
	Field.Advance(Step, Observer.LatDeg);
	UpdateBreezes();
	UpdateObserver();
	UpdatePrecipitation(Step);
	UpdateLightning(Step);
	UpdateClouds(Step);
	if (bShowDebugOverlay)
	{
		DrawDebugOverlay();
		DrawPrecipitationOverlay();
		DrawCloudOverlay();
	}
	if (ArrowHeightM > 0.0) { DrawArrows(); }
}

void UGeoWeatherSubsystem::UpdateObserver()
{
	const UGeoreferenceSubsystem* Georeference = GetWorld() ? GetWorld()->GetSubsystem<UGeoreferenceSubsystem>() : nullptr;
	FVector Location = FVector::ZeroVector;
	if (!Georeference || !Georeference->GetActiveViewLocation(Location))
	{
		Observer.bValid = false;
		SeaWind = SeaGust = Field.SurfaceWind(Observer.LatDeg, Weather::RoughnessWater);
		return;
	}
	ViewLocation = Location;
	const Core::FGeodetic Here = Georeference->GetSnapshot().UnrealToGeodetic(Location);
	Observer.LatDeg = Here.LatRad / Core::DegToRad;
	Observer.LonDeg = Here.LonRad / Core::DegToRad;
	Observer.HeightM = Here.HeightM;
	const double Geoid = GeoidAt(Observer.LonDeg, Observer.LatDeg);
	Observer.GeoidM = Geoid;
	if (!TerrainHeightAt(Observer.LonDeg, Observer.LatDeg, true, Observer.GroundHeightM)) { Observer.GroundHeightM = Geoid; }
	Observer.HeightAboveGroundM = FMath::Max(0.0, Here.HeightM - Observer.GroundHeightM);
	Observer.Surface = SurfaceAt(Observer.LatDeg, Observer.LonDeg, Observer.bOverWater);
	Observer.AtCamera = Field.Sample(Observer.LatDeg, Observer.LonDeg, Observer.HeightAboveGroundM,
		Here.HeightM - Geoid, Observer.Surface);
	Observer.AtTenMetres = Field.Sample(Observer.LatDeg, Observer.LonDeg, Weather::ReferenceHeightM,
		Observer.GroundHeightM + Weather::ReferenceHeightM - Geoid, Observer.Surface);
	Observer.bValid = true;

	// Le onde crescono con il vento MEDIO sul mare aperto (niente rilievi:
	// un'onda lunga viene da lontano); l'increspatura segue la raffica di
	// adesso, qui sotto, a 10 m sull'acqua.
	SeaWind = Field.SurfaceWind(Observer.LatDeg, Weather::RoughnessWater);
	FSurfaceInfluence Sea;
	Sea.RoughnessM = Weather::RoughnessWater;
	// La brezza di mare (o di terra) soffia anche sull'acqua vicino alla costa: le onde la sentono.
	Sea.Breezes[0] = Observer.Surface.Breezes[0];
	const Weather::FBreezeLayer& Coastal = Sea.Breezes[0];
	const double CoastalShare = Weather::BreezeProfile(Weather::ReferenceHeightM, Coastal);
	double SeaNorth = 0.0, SeaEast = 0.0;
	SeaWind.Toward(SeaNorth, SeaEast);
	SeaWind = WindOf(SeaNorth * SeaWind.SpeedMs + Coastal.North * CoastalShare, SeaEast * SeaWind.SpeedMs + Coastal.East * CoastalShare);
	const FWindSample Gust = Field.Sample(Observer.LatDeg, Observer.LonDeg, Weather::ReferenceHeightM, Weather::ReferenceHeightM, Sea);
	SeaGust = WindOf(Gust.North, Gust.East);
}

void UGeoWeatherSubsystem::UpdatePrecipitation(double DeltaSeconds)
{
	// ------------------------------------------------------------------
	//  1. I rovesci viaggiano con il vento in quota (medio: le raffiche non
	//     spostano un temporale).
	// ------------------------------------------------------------------
	const FSurfaceInfluence FreeAir;
	const double SteeringAboveGround = FMath::Max(SteeringAltitudeM + Observer.GeoidM - Observer.GroundHeightM, Weather::BoundaryLayerTopM);
	const FWindSample Steering = Field.Sample(Observer.LatDeg, Observer.LonDeg, SteeringAboveGround, SteeringAltitudeM, FreeAir);
	Precipitation.Advance(DeltaSeconds, Steering.MeanNorth, Steering.MeanEast);

	// ------------------------------------------------------------------
	//  2. Cosa cade qui: alla quota della camera (le particelle, la
	//     visibilita') e al suolo sotto (bagnato, neve). Quote sul MARE.
	// ------------------------------------------------------------------
	FObserverPrecipitation& Here = ObserverPrecipitation;
	if (Observer.bValid)
	{
		Here.AtCamera = Precipitation.Sample(Observer.LatDeg, Observer.LonDeg, Observer.HeightM - Observer.GeoidM);
		Here.AtGround = Precipitation.Sample(Observer.LatDeg, Observer.LonDeg, Observer.GroundHeightM - Observer.GeoidM);
	}
	else
	{
		Here.AtCamera = Here.AtGround = FPrecipitationSample{};
	}
	GroundState.Advance(DeltaSeconds, Here.AtGround, Precipitation);
	// La grandine posata: si accumula sotto i nuclei e fonde con l'aria al suolo qui sotto.
	if (HailMap)
	{
		HailMap->Update(Precipitation, Observer.bValid, Observer.LatDeg, Observer.LonDeg,
			Observer.bValid ? Here.AtGround.TemperatureC : Precipitation.TemperatureAt(0.0));
	}

	// La visibilita' segue piano: si smussa l'ESTINZIONE (che si somma), non la distanza.
	const double Extinction = std::isfinite(Here.AtCamera.VisibilityM) ? Koschmieder / FMath::Max(Here.AtCamera.VisibilityM, 1.0) : 0.0;
	SmoothedExtinction += (Extinction - SmoothedExtinction) * (1.0 - FMath::Exp(-DeltaSeconds / VisibilitySeconds));
	Here.VisibilityM = SmoothedExtinction > 1.0e-7 ? Koschmieder / SmoothedExtinction : 1.0e9;

	// ------------------------------------------------------------------
	//  3. Le particelle: cadono con il vento di adesso (raffiche comprese),
	//     attorno alla camera in metri (nord, est, su: gli assi di Unreal
	//     all'origine).
	// ------------------------------------------------------------------
	const FWindSample& CameraWind = Observer.AtCamera;
	const double Camera[3] = { ViewLocation.X * Units::UuToMeters, ViewLocation.Y * Units::UuToMeters, ViewLocation.Z * Units::UuToMeters };
	if (Observer.bValid)
	{
		Particles.Advance(DeltaSeconds, Here.AtCamera, CameraWind.North, CameraWind.East, CameraWind.Up, Camera);
	}
	else
	{
		Particles.Advance(DeltaSeconds, FPrecipitationSample{}, 0.0, 0.0, 0.0, Camera);
	}

	// ------------------------------------------------------------------
	//  4. A schermo: particelle, suolo (MPC_GeoWeather) e nebbia.
	// ------------------------------------------------------------------
	if (!Renderer) { return; }
	FGeoPrecipitationRenderer::FGroundParameters Parameters;
	Parameters.Wetness = GroundState.GetWetness();
	Parameters.SnowCoverM = GroundState.GetSnowCoverM();
	Parameters.SnowAmount = GroundState.HasSnow() ? 1.0 : 0.0;
	Parameters.IceAmount = GroundState.HasIce() ? GroundState.GetIce() : 0.0;
	Parameters.IceLineM = GroundState.GetIceLineM();
	Parameters.IceTransitionM = Weather::FGroundWeather::IceTransitionM;
	// Il materiale ricava la quota dalla Z: all'origine Z = 0 e' l'ellissoide,
	// e per avere la quota sul mare toglie il geoide di li'.
	if (bHaveSnapshot)
	{
		const Core::FGeodetic Origin = LastSnapshot.Georeference.GetOriginGeodetic();
		GeoidAtOriginM = GeoidAt(Origin.LonRad / Core::DegToRad, Origin.LatRad / Core::DegToRad);
		Parameters.GeoidAtOriginM = GeoidAtOriginM;
		if (HailMap) { Parameters.HailMap = HailMap->GetPlacement(LastSnapshot); }
	}
	// La texture della grandine ai materiali del terreno e delle strade: e'
	// sempre la stessa, il provider la da' una volta sola.
	if (HailMap && HailMap->GetTexture())
	{
		const UGeoTerrainSubsystem* Terrain = GetWorld() ? GetWorld()->GetSubsystem<UGeoTerrainSubsystem>() : nullptr;
		if (IGeoTerrainMeshProvider* Provider = Terrain ? Terrain->GetProvider() : nullptr)
		{
			Provider->SetGroundHailTexture(HailMap->GetTexture());
		}
	}
	const double GroundZ = (Observer.GroundHeightM - Observer.HeightM) * Units::MetersToUu + ViewLocation.Z;
	Renderer->Update(GetWorld(), ViewLocation, Particles, Parameters, Here.VisibilityM, bDriveFog && Observer.bValid, GroundZ);
}

// ---------------------------------------------------------------------------
//  I fulmini
// ---------------------------------------------------------------------------

void UGeoWeatherSubsystem::SetLightningActivity(double Activity)
{
	Weather::FLightningSettings Settings = Lightning.GetSettings();
	Settings.Activity = Activity;
	Lightning.SetSettings(Settings);
}

void UGeoWeatherSubsystem::UpdateLightning(double DeltaSeconds)
{
	// Senza camera si fa solo passare il tempo (con una mappa senza pioggia).
	static const Weather::FPrecipitationField NoRain;
	std::vector<Weather::FLightningStrike> Born;
	Lightning.Advance(DeltaSeconds, Observer.LatDeg, Observer.LonDeg, Observer.bValid ? Precipitation : NoRain, Born);
	// Un frame lunghissimo non deve sparare decine di fulmini insieme.
	for (size_t Index = 0; Index < Born.size() && Index < 4; ++Index) { FireStrike(Born[Index]); }

	const double Now = Lightning.GetTime();
	RecentStrikeTimes.RemoveAll([Now](double When) { return Now - When > 60.0; });
	const UGeoreferenceSubsystem* Georeference = GetWorld() ? GetWorld()->GetSubsystem<UGeoreferenceSubsystem>() : nullptr;
	if (LightningRenderer && Georeference) { LightningRenderer->Update(GetWorld(), Georeference->GetSnapshot(), Now); }
}

void UGeoWeatherSubsystem::FireStrike(const Weather::FLightningStrike& Strike)
{
	UWorld* World = GetWorld();
	const UGeoreferenceSubsystem* Georeference = World ? World->GetSubsystem<UGeoreferenceSubsystem>() : nullptr;
	if (!Georeference || !LightningRenderer || !Observer.bValid) { return; }
	const FGeoreferenceSnapshot Snapshot = Georeference->GetSnapshot();

	// Il suolo nel punto: le quote in memoria (lontano, quelle grossolane), o il mare.
	const double Geoid = GeoidAt(Strike.LonDeg, Strike.LatDeg);
	double GroundHeight = 0.0;
	if (!TerrainHeightAt(Strike.LonDeg, Strike.LatDeg, true, GroundHeight)) { GroundHeight = Geoid; }
	const double TemperatureC = Precipitation.TemperatureAt(GroundHeight - Geoid);
	// Il canale nasce nella base della nube di pioggia, il lampo nella nube
	// sta nel suo cuore: le stesse quote che disegnano le nuvole (Clouds.h),
	// qui sopra il suolo del punto.
	const double RainBase = Weather::RainCloudBaseM(CloudSettings);
	const double CloudBase = RainBase + Geoid - GroundHeight;
	const double CloudMiddle = 0.5 * (RainBase + Precipitation.CloudTopM()) + Geoid - GroundHeight;

	const double Distance = LightningRenderer->AddStrike(World, Snapshot, Strike, GroundHeight, CloudBase, CloudMiddle,
		ViewLocation, TemperatureC, Lightning.GetTime());
	RecentStrikeTimes.Add(Strike.TimeSeconds);
	LatestStrike = Strike;
	bHaveLatestStrike = true;
	LatestStrikeDistanceM = Distance;

	const bool bToGround = Strike.Kind == Weather::ELightningKind::CloudToGround;
	const FVector Location = Snapshot.GeodeticToUnreal(Core::FGeodetic::FromDegrees(Strike.LatDeg, Strike.LonDeg, GroundHeight));
	OnLightningStrike.Broadcast(Location, bToGround, static_cast<float>(Distance));
}

bool UGeoWeatherSubsystem::ForceStrike(double DistanceM, bool bCloudToGround)
{
	const UGeoreferenceSubsystem* Georeference = GetWorld() ? GetWorld()->GetSubsystem<UGeoreferenceSubsystem>() : nullptr;
	UGeoreferenceSubsystem::FActiveViewInfo View;
	if (!Georeference || !Observer.bValid || !Georeference->GetActiveViewInfo(View)) { return false; }
	// Davanti alla camera, in orizzontale: lo yaw di Unreal e' l'azimut (X nord, Y est).
	const double Heading = FMath::DegreesToRadians(View.Rotation.Yaw);
	const double LatDeg = Observer.LatDeg + DistanceM * FMath::Cos(Heading) / 111132.0;
	const double LonDeg = Observer.LonDeg + DistanceM * FMath::Sin(Heading) / (111320.0 * FMath::Cos(FMath::DegreesToRadians(Observer.LatDeg)));
	FireStrike(Lightning.Strike(bCloudToGround ? Weather::ELightningKind::CloudToGround : Weather::ELightningKind::IntraCloud, LatDeg, LonDeg));
	return true;
}

// ---------------------------------------------------------------------------
//  Le nuvole
// ---------------------------------------------------------------------------

void UGeoWeatherSubsystem::UpdateClouds(double DeltaSeconds)
{
	CloudClock += DeltaSeconds;
	if (!CloudRenderer) { return; }
	if (!bCloudsEnabled)
	{
		// Spente: niente VolumetricCloud nostra, quella del livello torna com'era.
		CloudRenderer->Shutdown();
		return;
	}
	UWorld* World = GetWorld();
	const UGeoreferenceSubsystem* Georeference = World ? World->GetSubsystem<UGeoreferenceSubsystem>() : nullptr;
	if (!Georeference) { return; }
	const FGeoreferenceSnapshot Snapshot = Georeference->GetSnapshot();

	FGeoCloudRenderer::FInputs Inputs;
	Inputs.Settings = CloudSettings;
	Inputs.Field = &Precipitation;
	Inputs.bHaveCamera = Observer.bValid;
	Inputs.CameraLatDeg = Observer.LatDeg;
	Inputs.CameraLonDeg = Observer.LonDeg;
	Inputs.GeoidAtOriginM = GeoidAtOriginM;
	Inputs.TimeSeconds = CloudClock;
	if (LightningRenderer)
	{
		Inputs.bFlash = LightningRenderer->GetBrightestFlash(Snapshot, Lightning.GetTime(), Inputs.FlashLocation, Inputs.FlashBrightness);
	}
	CloudRenderer->Update(World, Snapshot, Inputs);
}

UGeoWeatherSubsystem::FLightningReport UGeoWeatherSubsystem::GetLightningReport() const
{
	FLightningReport Report;
	Report.LastMinute = RecentStrikeTimes.Num();
	Report.bAny = bHaveLatestStrike;
	if (bHaveLatestStrike)
	{
		Report.bCloudToGround = LatestStrike.Kind == Weather::ELightningKind::CloudToGround;
		Report.DistanceM = LatestStrikeDistanceM;
		Report.AgeSeconds = Lightning.GetTime() - LatestStrike.TimeSeconds;
	}
	Report.ThunderInSeconds = LightningRenderer ? LightningRenderer->NextThunderIn(Lightning.GetTime()) : -1.0;
	return Report;
}

void UGeoWeatherSubsystem::OnGeoreferenceRebased(const FGeoreferenceSnapshot& Snapshot)
{
	// Il punto che prima stava nella vecchia posizione della nuova origine ora
	// sta a zero: tutto il mondo si e' spostato di -Delta. Le particelle si
	// spostano con lui, come le onde (UGeoWaterSubsystem::OnGeoreferenceRebased).
	if (bHaveSnapshot)
	{
		const FVector OldPosition = LastSnapshot.GeodeticToUnreal(Snapshot.Georeference.GetOriginGeodetic());
		const double Delta[3] = { OldPosition.X * Units::UuToMeters, OldPosition.Y * Units::UuToMeters, OldPosition.Z * Units::UuToMeters };
		Particles.ApplyOriginShift(Delta);
	}
	LastSnapshot = Snapshot;
	bHaveSnapshot = true;
}

// ---------------------------------------------------------------------------
//  Debug
// ---------------------------------------------------------------------------

void UGeoWeatherSubsystem::DrawDebugOverlay() const
{
	if (!GEngine) { return; }
	int32 Key = 0x6F60;
	auto Line = [&Key](const FColor& Colour, const FString& Text)
	{
		GEngine->AddOnScreenDebugMessage(Key++, 0.0f, Colour, Text);
	};
	const Weather::FWindSettings& Settings = Field.GetSettings();
	const FWind Reference = Field.ReferenceWind();
	const FWind Gradient = Field.GradientWind(Observer.LatDeg);

	Line(FColor::Cyan, TEXT("--- GeoWorld | Meteo: il vento ---"));
	Line(FColor::White, FString::Printf(TEXT("Riferimento  : %s  (10 m su un prato; impostato %s)"),
		*Utf8(Format::Wind(Reference.SpeedMs, Reference.FromDegrees)),
		*Utf8(Format::Wind(Settings.Reference.SpeedMs, Settings.Reference.FromDegrees))));
	Line(FColor::White, FString::Printf(TEXT("Gradiente    : %s  (sopra %.0f m dal suolo)"),
		*Utf8(Format::Wind(Gradient.SpeedMs, Gradient.FromDegrees)), Weather::BoundaryLayerTopM));
	Line(FColor::White, FString::Printf(TEXT("Turbolenza   : x%.2f   variabilita' %.2f   seme %u   venti in quota: %d"),
		Settings.Turbulence, Settings.Variability, Settings.Seed, static_cast<int32>(Settings.Aloft.size())));
	for (const Weather::FWindLayer& Layer : Settings.Aloft)
	{
		Line(FColor::White, FString::Printf(TEXT("   a %.0f m s.l.m.: %s"), Layer.AltitudeM,
			*Utf8(Format::Wind(Layer.SpeedMs, Layer.FromDegrees))));
	}
	Line(FColor::White, FString::Printf(TEXT("Sul mare     : %s  (le onde)   raffica adesso %s"),
		*Utf8(Format::Wind(SeaWind.SpeedMs, SeaWind.FromDegrees)), *Utf8(Format::Wind(SeaGust.SpeedMs, SeaGust.FromDegrees))));
	if (!BreezeSettings.bEnabled)
	{
		Line(FColor::White, TEXT("Brezze       : spente (geo.Weather.Breezes 1)"));
	}
	else
	{
		Line(FColor::White, FString::Printf(TEXT("Brezze       : mare %+.2f, pendii %+.2f, valli %+.2f (+ di giorno, - di notte); sole a %.0f gradi; forza x%.1f%s"),
			BreezeClimate.Coast(), BreezeClimate.Slope(), BreezeClimate.Valley(), BreezeClimate.SunAltitudeDeg(), BreezeSettings.Strength,
			WaterMap.IsBuilt() ? (WaterMap.HasWater() ? TEXT("") : TEXT("; niente acqua entro 50 km"))
			                   : TEXT("; senza il dataset dell'acqua niente brezza di mare")));
	}

	if (!Observer.bValid)
	{
		Line(FColor::Yellow, TEXT("Nessuna camera: niente vento \"qui\""));
		return;
	}
	const FSurfaceInfluence& Surface = Observer.Surface;
	Line(FColor::White, FString::Printf(TEXT("Sotto di te  : %s, rugosita' %s m, rilievi x%.2f, ridosso %.0f%%, pendenza %+.2f"),
		Observer.bOverWater ? TEXT("ACQUA") : TEXT("terra"), *Utf8(Format::Number(Surface.RoughnessM, 4)),
		Surface.SpeedUp, 100.0 * Surface.Shelter, Surface.SlopeAlongWind));   /* percentuale, non-unita */
	const FWindSample& Ten = Observer.AtTenMetres;
	const FWindSample& AtCamera = Observer.AtCamera;
	Line(FColor::Green, FString::Printf(TEXT("A 10 m       : %s, raffiche fino a %s m/s (sigma %.1f, scala %.0f m)"),
		*Utf8(Format::Wind(Ten.MeanSpeed(), Ten.MeanFromDegrees())), *Utf8(Format::Number(Ten.GustSpeed(), 1)),
		Ten.SigmaU, Ten.TurbulenceLengthM));
	const FString Breezes = DescribeBreezes(0.2);
	if (!Breezes.IsEmpty()) { Line(FColor::Green, FString::Printf(TEXT("Brezze a 10 m: %s"), *Breezes)); }
	Line(FColor::Green, FString::Printf(TEXT("Qui (%s m)   : %s, adesso %s m/s, verticale %+.2f m/s"),
		*Utf8(Format::Number(Observer.HeightAboveGroundM, 0)),
		*Utf8(Format::Wind(AtCamera.MeanSpeed(), AtCamera.MeanFromDegrees())), *Utf8(Format::Number(AtCamera.Speed(), 1)), AtCamera.Up));
	Line(FColor::Yellow, FString::Printf(TEXT("Frecce       : %s (geo.Weather.Arrows <quota sul suolo>)"),
		ArrowHeightM > 0.0 ? *FString::Printf(TEXT("a %.0f m dal suolo"), ArrowHeightM) : TEXT("spente")));
}

void UGeoWeatherSubsystem::DrawPrecipitationOverlay() const
{
	if (!GEngine) { return; }
	int32 Key = 0x6F80;
	auto Line = [&Key](const FColor& Colour, const FString& Text)
	{
		GEngine->AddOnScreenDebugMessage(Key++, 0.0f, Colour, Text);
	};
	const Weather::FPrecipitationSettings& Sky = Precipitation.GetSettings();
	static const TCHAR* KindNames[] = { TEXT("secondo la temperatura"), TEXT("solo pioggia"), TEXT("solo neve"), TEXT("con grandine") };
	Line(FColor::Cyan, TEXT("--- GeoWorld | Meteo: pioggia, neve, grandine ---"));
	if (Sky.RateMmH > 0.0)
	{
		Line(FColor::White, FString::Printf(TEXT("Precipitazione: %s mm/h nel cuore, %s, copertura %.0f%%, rovesci da %s km"),
			*Utf8(Format::Number(Sky.RateMmH, 1)), KindNames[static_cast<int32>(Sky.Kind)], 100.0 * Sky.Coverage,   /* percentuale, non-unita */
			*Utf8(Format::Number(Sky.CellSizeKm, 0))));
	}
	else
	{
		Line(FColor::White, TEXT("Precipitazione: sereno (geo.Weather.Rain 4, Snow 1, Hail 30)"));
	}
	Line(FColor::White, FString::Printf(TEXT("Temperatura   : %s al mare, cala di %s/km: zero termico %s, quota neve %s, nuvole fino a %s"),
		*Celsius(Sky.SeaLevelTemperatureC), *Celsius(Sky.LapseRateCPerKm),
		*Utf8(Format::Metres(Precipitation.FreezingLevelM())),
		Sky.Kind == Weather::EPrecipitationKind::Rain ? TEXT("mai") : Sky.Kind == Weather::EPrecipitationKind::Snow ? TEXT("ovunque")
			: *Utf8(Format::Metres(Precipitation.SnowLineM())),
		*Utf8(Format::Metres(Precipitation.CloudTopM()))));
	Line(FColor::White, FString::Printf(TEXT("Suolo         : bagnato %.0f%%, %s, %s"), 100.0 * GroundState.GetWetness(),   /* percentuale, non-unita */
		GroundState.HasSnow() ? *FString::Printf(TEXT("neve sopra %s"), *Utf8(Format::Metres(GroundState.GetSnowCoverM()))) : TEXT("niente neve"),
		GroundState.HasIce() ? *FString::Printf(TEXT("ghiaccio %.0f%% sopra %s"), 100.0 * GroundState.GetIce(),   /* percentuale, non-unita */
			*Utf8(Format::Metres(GroundState.GetIceLineM()))) : TEXT("niente ghiaccio")));
	if (HailMap && !HailMap->IsEmpty())
	{
		Line(FColor::White, FString::Printf(TEXT("Grandine      : posata fino a %s mm; qui sotto %.0f%% bianco%s"),
			*Utf8(Format::Number(HailMap->MaxDepth(), 1)),
			Observer.bValid ? 100.0 * HailMap->CoverAt(Observer.LatDeg, Observer.LonDeg) : 0.0,   /* percentuale, non-unita */
			HailMap->IsWorking() ? TEXT(" (in aggiornamento)") : TEXT("")));
	}
	if (Observer.bValid)
	{
		const FObserverPrecipitation& Here = ObserverPrecipitation;
		Line(FColor::Green, FString::Printf(TEXT("Qui           : %s, %s; al suolo %s"),
			*Utf8(Format::Precipitation(Here.AtCamera.RateMmH, Here.AtCamera.Rain, Here.AtCamera.Snow, Here.AtCamera.Hail)),
			*Celsius(Here.AtCamera.TemperatureC),
			*Utf8(Format::Precipitation(Here.AtGround.RateMmH, Here.AtGround.Rain, Here.AtGround.Snow, Here.AtGround.Hail))));
		const Weather::FParticleLayer& Drops = Particles.GetLayer(Weather::EParticleType::Rain);
		const Weather::FParticleLayer& Flakes = Particles.GetLayer(Weather::EParticleType::Snow);
		const Weather::FParticleLayer& Stones = Particles.GetLayer(Weather::EParticleType::Hail);
		Line(FColor::Green, FString::Printf(TEXT("Particelle    : gocce %.0f%%, fiocchi %.0f%%, chicchi %.0f%% (cadono a %.1f / %.1f / %.1f m/s)"),
			100.0 * Drops.Density, 100.0 * Flakes.Density, 100.0 * Stones.Density,   /* percentuale, non-unita */
			-Drops.Velocity[2], -Flakes.Velocity[2], -Stones.Velocity[2]));
		Line(FColor::Green, FString::Printf(TEXT("Visibilita'   : %s; nebbia %s (densita' %.4f)"),
			*Utf8(Format::Distance(Here.VisibilityM)),
			!bDriveFog ? TEXT("spenta") : Renderer ? Renderer->GetFogState() : TEXT("--"),
			Renderer ? Renderer->GetFogDensity() : 0.0));
	}
	const FString Problem = GetRenderProblem();
	if (!Problem.IsEmpty()) { Line(FColor::Yellow, Problem); }

	// --- I fulmini -------------------------------------------------------------
	const Weather::FLightningSettings& Storm = Lightning.GetSettings();
	if (Storm.Activity <= 0.0 && !bHaveLatestStrike) { return; }
	const FLightningReport Flashes = GetLightningReport();
	FString Latest = TEXT("nessuno ancora");
	if (Flashes.bAny)
	{
		Latest = FString::Printf(TEXT("l'ultimo %s a %s, %.0f s fa"), Flashes.bCloudToGround ? TEXT("nube-suolo") : TEXT("nella nube"),
			*Utf8(Format::Distance(Flashes.DistanceM)), Flashes.AgeSeconds);
	}
	Line(FColor::Green, FString::Printf(TEXT("Fulmini       : attivita' %.1f, %d nell'ultimo minuto entro %.0f km; %s%s"),
		Storm.Activity, Flashes.LastMinute, Storm.RadiusM / 1000.0, *Latest,
		Flashes.ThunderInSeconds >= 0.0 ? *FString::Printf(TEXT("; tuono fra %.1f s"), Flashes.ThunderInSeconds) : TEXT("")));
	if (Storm.Activity > 0.0 && Sky.Coverage >= 0.999)
	{
		Line(FColor::Yellow, TEXT("Con la pioggia estesa i fulmini sono rari: i temporali sono rovesci (geo.Weather.Storm)"));
	}
	if (LightningRenderer && !LightningRenderer->GetProblem().IsEmpty()) { Line(FColor::Yellow, LightningRenderer->GetProblem()); }
}

void UGeoWeatherSubsystem::DrawCloudOverlay() const
{
	if (!GEngine) { return; }
	int32 Key = 0x6FA0;
	auto Line = [&Key](const FColor& Colour, const FString& Text)
	{
		GEngine->AddOnScreenDebugMessage(Key++, 0.0f, Colour, Text);
	};
	Line(FColor::Cyan, TEXT("--- GeoWorld | Meteo: le nuvole ---"));
	if (!bCloudsEnabled)
	{
		Line(FColor::White, TEXT("Nuvole        : spente (geo.Weather.Clouds on)"));
		return;
	}
	const Weather::FCloudSettings& Sky = CloudSettings;
	Line(FColor::White, FString::Printf(TEXT("Cumuli        : %.0f%% del cielo, base %s, alti fino a %s; densita' x%.2f"),
		100.0 * Sky.Cover, *Utf8(Format::Metres(Sky.BaseM)), *Utf8(Format::Metres(Sky.FairThicknessM)),   /* percentuale, non-unita */
		Sky.ExtinctionPerM / Weather::FCloudSettings().ExtinctionPerM));
	const Weather::FPrecipitationSettings& Rain = Precipitation.GetSettings();
	if (Rain.RateMmH > 0.0 && Rain.Coverage > 0.0)
	{
		Line(FColor::White, FString::Printf(TEXT("Nubi di pioggia: base %s, cime fino a %s (%s)"),
			*Utf8(Format::Metres(Weather::RainCloudBaseM(Sky))), *Utf8(Format::Metres(Precipitation.CloudTopM())),
			Rain.Coverage >= 0.999 ? TEXT("cielo coperto") : TEXT("torri sui rovesci")));
	}
	if (Observer.bValid)
	{
		const Weather::FCloudColumn Above = Weather::CloudColumn(Precipitation, Observer.LatDeg, Observer.LonDeg);
		if (Above.Cover > 0.05)
		{
			Line(FColor::Green, FString::Printf(TEXT("Sopra di te   : nube di pioggia (%.0f%%) fino a %s"), 100.0 * Above.Cover,   /* percentuale, non-unita */
				*Utf8(Format::Metres(Weather::RainCloudTopM(Sky, Precipitation, Above.TopFraction)))));
		}
	}
	if (CloudRenderer)
	{
		double Bottom = 0.0, Top = 0.0;
		CloudRenderer->GetLayer(Bottom, Top);
		const double Age = CloudRenderer->GetMapAge(CloudClock);
		Line(FColor::Green, FString::Printf(TEXT("VolumetricCloud: %s; strato %s - %s; mappa meteo %s"),
			CloudRenderer->GetState(), *Utf8(Format::Metres(Bottom)), *Utf8(Format::Metres(Top)),
			Age >= 0.0 ? *FString::Printf(TEXT("di %.0f s fa"), Age) : TEXT("in arrivo")));
		if (!CloudRenderer->GetProblem().IsEmpty()) { Line(FColor::Yellow, CloudRenderer->GetProblem()); }
		if (!CloudRenderer->GetAdvice().IsEmpty()) { Line(FColor::Yellow, CloudRenderer->GetAdvice()); }
	}
}

void UGeoWeatherSubsystem::DrawArrows() const
{
	UWorld* World = GetWorld();
	const UGeoreferenceSubsystem* Georeference = World ? World->GetSubsystem<UGeoreferenceSubsystem>() : nullptr;
	if (!Georeference || !Observer.bValid) { return; }
	const FGeoreferenceSnapshot Snapshot = Georeference->GetSnapshot();

	// Il passo segue la quota della camera: da vicino una freccia ogni 200 m,
	// da 5 km una ogni 1,2 km. Le frecce stanno su una griglia FISSA nel mondo
	// (multipli del passo), cosi' non scivolano quando la camera si muove.
	const double Spacing = FMath::Max(200.0, Observer.HeightAboveGroundM / 4.0);
	const double MetresNorth = 111132.0;
	const double MetresEast = 111320.0 * FMath::Cos(FMath::DegreesToRadians(Observer.LatDeg));
	const double CentreNorth = FMath::RoundToDouble(Observer.LatDeg * MetresNorth / Spacing) * Spacing;
	const double CentreEast = FMath::RoundToDouble(Observer.LonDeg * MetresEast / Spacing) * Spacing;
	const int32 Half = ArrowGrid / 2;
	for (int32 Row = -Half; Row <= Half; ++Row)
	{
		for (int32 Column = -Half; Column <= Half; ++Column)
		{
			const double Lat = (CentreNorth + Row * Spacing) / MetresNorth;
			const double Lon = (CentreEast + Column * Spacing) / MetresEast;
			double Ground = 0.0;
			if (!TerrainHeightAt(Lon, Lat, true, Ground)) { continue; }
			const double Height = Ground + ArrowHeightM;
			const FWindSample Sample = SampleWind(Lat, Lon, Height);

			const FVector Start = Snapshot.GeodeticToUnreal(Core::FGeodetic::FromDegrees(Lat, Lon, Height));
			const FVector End = Snapshot.GeodeticToUnreal(Core::FGeodetic::FromDegrees(
				Lat + Sample.North * ArrowSeconds / MetresNorth, Lon + Sample.East * ArrowSeconds / MetresEast,
				Height + Sample.Up * ArrowSeconds));
			// Spesse un cinquantesimo del passo: si vedono da qualunque quota.
			const float Thickness = static_cast<float>(Spacing / 50.0 * Units::MetersToUu);
			const float Head = static_cast<float>((End - Start).Size() * 0.25);
			DrawDebugDirectionalArrow(World, Start, End, Head, SpeedColour(Sample.Speed()), false, -1.0f, 0, Thickness);
		}
	}
}

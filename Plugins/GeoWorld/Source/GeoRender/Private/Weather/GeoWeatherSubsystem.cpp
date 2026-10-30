#include "Weather/GeoWeatherSubsystem.h"

#include "Geo/GeoFormat.h"
#include "Geo/GeoUnits.h"
#include "Georeference/GeoreferenceSubsystem.h"
#include "Mesh/TileMesh.h"
#include "Streaming/GeoTileStreamingSubsystem.h"
#include "Water/GeoWaterSubsystem.h"

#include "DrawDebugHelpers.h"
#include "Engine/Engine.h"
#include "Engine/World.h"

using namespace GeoWorld;
using GeoWorld::Weather::FWind;
using GeoWorld::Weather::FWindSample;
using GeoWorld::Weather::FSurfaceInfluence;

namespace
{
	/** Il livello delle quote per i rilievi: post da ~75 m, lisci, quasi sempre in memoria. */
	constexpr int32 OrographyLevel = 11;
	/** Le frecce: 9 x 9 attorno alla camera, lunghe quanto il vento fa in 20 s. */
	constexpr int32 ArrowGrid = 9;
	constexpr double ArrowSeconds = 20.0;

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
	return Surface;
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

// ---------------------------------------------------------------------------
//  A ogni frame
// ---------------------------------------------------------------------------

void UGeoWeatherSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	// Un frame lunghissimo (il debugger, un caricamento) non deve far correre
	// le raffiche di chilometri: si avanza al massimo di un secondo.
	Field.Advance(FMath::Min(static_cast<double>(DeltaTime), 1.0), Observer.LatDeg);
	UpdateObserver();
	if (bShowDebugOverlay) { DrawDebugOverlay(); }
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
	const Core::FGeodetic Here = Georeference->GetSnapshot().UnrealToGeodetic(Location);
	Observer.LatDeg = Here.LatRad / Core::DegToRad;
	Observer.LonDeg = Here.LonRad / Core::DegToRad;
	const double Geoid = GeoidAt(Observer.LonDeg, Observer.LatDeg);
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
	const FWindSample Gust = Field.Sample(Observer.LatDeg, Observer.LonDeg, Weather::ReferenceHeightM, Weather::ReferenceHeightM, Sea);
	SeaGust = WindOf(Gust.North, Gust.East);
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
	Line(FColor::Green, FString::Printf(TEXT("Qui (%s m)   : %s, adesso %s m/s, verticale %+.2f m/s"),
		*Utf8(Format::Number(Observer.HeightAboveGroundM, 0)),
		*Utf8(Format::Wind(AtCamera.MeanSpeed(), AtCamera.MeanFromDegrees())), *Utf8(Format::Number(AtCamera.Speed(), 1)), AtCamera.Up));
	Line(FColor::Yellow, FString::Printf(TEXT("Frecce       : %s (geo.Weather.Arrows <quota sul suolo>)"),
		ArrowHeightM > 0.0 ? *FString::Printf(TEXT("a %.0f m dal suolo"), ArrowHeightM) : TEXT("spente")));
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

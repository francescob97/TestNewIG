#include "GeoPositionHudSubsystem.h"

#include "Buildings/GeoBuildingsSubsystem.h"
#include "Sky/GeoSkySubsystem.h"
#include "Geo/GeoFormat.h"
#include "Georeference/GeoreferenceSubsystem.h"
#include "Mesh/TileMesh.h"
#include "Roads/GeoStreetLightsSubsystem.h"
#include "Streaming/GeoTileStreamingSubsystem.h"
#include "Terrain/GeoTerrainSubsystem.h"
#include "Vegetation/GeoVegetationSubsystem.h"
#include "Water/GeoWaterSubsystem.h"
#include "Weather/GeoWeatherSubsystem.h"

#include "CanvasItem.h"
#include "Debug/DebugDrawService.h"
#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "Engine/Font.h"
#include "Engine/World.h"
#include "SceneInterface.h"
#include "SceneView.h"

using namespace GeoWorld;

bool UGeoPositionHudSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	if (!Super::ShouldCreateSubsystem(Outer)) { return false; }
	const UWorld* World = Cast<UWorld>(Outer);
	return World && (World->WorldType == EWorldType::Game
	              || World->WorldType == EWorldType::PIE
	              || World->WorldType == EWorldType::Editor);
}

void UGeoPositionHudSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	// ------------------------------------------------------------------
	//  NOTA UE: UDebugDrawService chiama le funzioni registrate per ogni
	//  viewport che ha acceso il "canale" con quel nome. Il viewport del
	//  gioco (anche nel Play dell'editor) ha "Game", il viewport dell'editor
	//  ha "Editor". Ci si registra su tutti e due, e Draw scarta i viewport
	//  di altri mondi e i doppioni.
	// ------------------------------------------------------------------
	GameDrawHandle = UDebugDrawService::Register(TEXT("Game"),
		FDebugDrawDelegate::CreateUObject(this, &UGeoPositionHudSubsystem::Draw));
	EditorDrawHandle = UDebugDrawService::Register(TEXT("Editor"),
		FDebugDrawDelegate::CreateUObject(this, &UGeoPositionHudSubsystem::Draw));
}

void UGeoPositionHudSubsystem::Deinitialize()
{
	// La funzione registrata punta a "this": va tolta prima che l'oggetto sparisca.
	UDebugDrawService::Unregister(GameDrawHandle);
	UDebugDrawService::Unregister(EditorDrawHandle);
	Super::Deinitialize();
}

bool UGeoPositionHudSubsystem::GroundHeightAt(double LonDeg, double LatDeg, double& OutHeight) const
{
	UWorld* World = GetWorld();
	UGeoTileStreamingSubsystem* Streaming = World ? World->GetSubsystem<UGeoTileStreamingSubsystem>() : nullptr;
	if (!Streaming || !Streaming->IsDatasetOpen()) { return false; }

	const UGeoTerrainSubsystem* Terrain = World->GetSubsystem<UGeoTerrainSubsystem>();
	const int32 Step = Terrain ? Terrain->GetMeshStep() : 1;

	// Dalla tile piu' fine in memoria: e' quella che si vede sotto di te, o la
	// piu' vicina a lei. Si prende la superficie DISEGNATA (stesso passo e
	// stessa diagonale della mesh), cosi' "dal suolo" e' la distanza dal
	// terreno che vedi, non dal DEM.
	const FGeoTileDataset& Dataset = Streaming->GetDataset();
	for (int32 Level = Dataset.GetMaxLevel(); Level >= Dataset.GetMinLevel(); --Level)
	{
		uint32 X = 0, Y = 0;
		Tiles::TileForLonLat(static_cast<uint32>(Level), LonDeg, LatDeg, X, Y);
		const Tiles::FTileKey Key{ static_cast<uint32>(Level), X, Y };
		const UGeoTileStreamingSubsystem::FTilePtr Tile = Streaming->FindLoadedTile(Key);
		if (!Tile || !Tile->IsValid()) { continue; }

		// Al largo la tile e' virtuale e le sue quote sono il FONDALE: il
		// suolo che conta e' il mare, cioe' il geoide.
		const UGeoWaterSubsystem* Water = World->GetSubsystem<UGeoWaterSubsystem>();
		if (Streaming->IsVirtualTile(Key) && Water && Water->GetGeoidUndulation(LonDeg, LatDeg, OutHeight))
		{
			return true;
		}

		const Mesh::FSurfaceSampler Surface(*Tile, Step);
		OutHeight = Surface.HeightAt(LonDeg, LatDeg);
		return true;
	}
	return false;
}

void UGeoPositionHudSubsystem::Draw(UCanvas* Canvas, APlayerController* /*Controller*/)
{
	if (!bEnabled || !Canvas || !Canvas->SceneView || !Canvas->SceneView->Family || !GEngine) { return; }

	// Solo i viewport di QUESTO mondo: con il Play aperto, il subsystem del
	// mondo dell'editor e quello del mondo di gioco ricevono entrambi tutti i
	// viewport.
	const FSceneInterface* Scene = Canvas->SceneView->Family->Scene;
	if (!Scene || Scene->GetWorld() != GetWorld()) { return; }

	if (LastDrawnFrame == GFrameCounter && LastDrawnView == Canvas->SceneView) { return; }
	LastDrawnFrame = GFrameCounter;
	LastDrawnView = Canvas->SceneView;

	const UGeoreferenceSubsystem* Georeference = GetWorld()->GetSubsystem<UGeoreferenceSubsystem>();
	if (!Georeference) { return; }

	const FGeoreferenceSnapshot Snapshot = Georeference->GetSnapshot();
	const Core::FGeodetic Here = Snapshot.UnrealToGeodetic(Canvas->SceneView->ViewLocation);
	const double LatDeg = Here.LatRad / Core::DegToRad;
	const double LonDeg = Here.LonRad / Core::DegToRad;

	// Le righe: etichetta a sinistra, valore a destra.
	TArray<TPair<FString, FString>> Lines;
	auto Add = [&Lines](const TCHAR* Label, const std::string& Value)
	{
		Lines.Emplace(FString(Label), FString(UTF8_TO_TCHAR(Value.c_str())));
	};

	Add(TEXT("Lat"), Format::DecimalDegrees(LatDeg, true) + "   " + Format::DegreesMinutesSeconds(LatDeg, true));
	Add(TEXT("Lon"), Format::DecimalDegrees(LonDeg, false) + "   " + Format::DegreesMinutesSeconds(LonDeg, false));
	Add(TEXT("Quota"), Format::Metres(Here.HeightM) + "  ellissoide");

	// Sul livello del mare: serve il geoide, che arriva con il dataset
	// dell'acqua (Fase 9). Senza, la riga non c'e': meglio niente che un
	// numero sbagliato di ~45 m.
	const UGeoWaterSubsystem* Water = GetWorld()->GetSubsystem<UGeoWaterSubsystem>();
	double Undulation = 0.0;
	if (Water && Water->GetGeoidUndulation(LonDeg, LatDeg, Undulation))
	{
		Add(TEXT("s.l.m."), Format::Metres(Here.HeightM - Undulation) + "  geoide");
	}

	double Ground = 0.0;
	if (GroundHeightAt(LonDeg, LatDeg, Ground))
	{
		Add(TEXT("Dal suolo"), Format::Metres(Here.HeightM - Ground));
	}

	// La rotta e' lo yaw della vista: con gli assi del progetto (X nord, Y
	// est) lo yaw di Unreal e' l'azimut della bussola, vicino all'origine.
	Add(TEXT("Rotta"), Format::Heading(Canvas->SceneView->ViewRotation.Yaw));

	// Il vento: qui, alla quota della vista (con la raffica di adesso), e a
	// 10 m dal suolo sotto di te, come lo darebbe una stazione meteo.
	if (const UGeoWeatherSubsystem* WeatherSubsystem = GetWorld()->GetSubsystem<UGeoWeatherSubsystem>())
	{
		const Weather::FWindSample Wind = WeatherSubsystem->SampleWind(LatDeg, LonDeg, Here.HeightM);
		Add(TEXT("Vento"), Format::Wind(Wind.MeanSpeed(), Wind.MeanFromDegrees()));
		Add(TEXT("Raffiche"), Format::Number(Wind.GustSpeed(), 1) + " m/s, adesso " + Format::Number(Wind.Speed(), 1)
			+ ", verticale " + (Wind.Up >= 0.0 ? "+" : "") + Format::Number(Wind.Up, 1) + " m/s");
		const Weather::FWindSample Surface = WeatherSubsystem->SampleWind(LatDeg, LonDeg,
			(GroundHeightAt(LonDeg, LatDeg, Ground) ? Ground : Here.HeightM) + Weather::ReferenceHeightM);
		Add(TEXT("A 10 m"), Format::Wind(Surface.MeanSpeed(), Surface.MeanFromDegrees()));
		// Le brezze qui sotto, se si sentono: di mare, di pendio, di valle.
		const FString Breezes = WeatherSubsystem->DescribeBreezes();
		if (!Breezes.IsEmpty()) { Add(TEXT("Brezza"), std::string(TCHAR_TO_UTF8(*Breezes))); }

		// Il tempo che fa qui: cosa cade (alla quota della vista), la
		// temperatura e lo zero termico, quanto si vede, il suolo.
		const Weather::FPrecipitationField& Sky = WeatherSubsystem->GetPrecipitationField();
		const Weather::FPrecipitationSample Falling = Sky.Sample(LatDeg, LonDeg, Here.HeightM - Undulation);
		Add(TEXT("Meteo"), Format::Precipitation(Falling.RateMmH, Falling.Rain, Falling.Snow, Falling.Hail));
		Add(TEXT("Temperatura"), Format::Number(Falling.TemperatureC, 1) + " " + Format::Degree + "C, zero termico "
			+ Format::Metres(Sky.FreezingLevelM()));
		const double Visibility = WeatherSubsystem->GetObserverPrecipitation().VisibilityM;
		if (Visibility < 50000.0) { Add(TEXT("Visibilita'"), Format::Distance(Visibility)); }
		const Weather::FGroundWeather& Soil = WeatherSubsystem->GetGround();
		const double HailHere = WeatherSubsystem->GetHailCoverAt(LatDeg, LonDeg);
		if (Soil.GetWetness() > 0.01 || Soil.HasSnow() || Soil.HasIce() || HailHere > 0.01)
		{
			std::string Text = Soil.GetWetness() > 0.01 ? "bagnato " + Format::Number(100.0 * Soil.GetWetness(), 0) + "%" : "asciutto"; /* percentuale, non-unita */
			if (Soil.HasSnow()) { Text += ", neve sopra " + Format::Metres(Soil.GetSnowCoverM()); }
			if (Soil.HasIce()) { Text += ", ghiaccio sopra " + Format::Metres(Soil.GetIceLineM()); }
			if (HailHere > 0.01) { Text += ", grandine " + Format::Number(100.0 * HailHere, 0) + "%"; } /* percentuale, non-unita */
			Add(TEXT("Suolo"), Text);
		}
		// Le nuvole: i cumuli che ci sono, e la nube di pioggia sopra di te.
		if (WeatherSubsystem->AreCloudsEnabled())
		{
			const Weather::FCloudSettings& Clouds = WeatherSubsystem->GetCloudSettings();
			std::string Text = Clouds.Cover > 0.0
				? "cumuli " + Format::Number(100.0 * Clouds.Cover, 0) + "%, base " + Format::Metres(Clouds.BaseM) /* percentuale, non-unita */
				: std::string("niente cumuli");
			const Weather::FCloudColumn Above = Weather::CloudColumn(Sky, LatDeg, LonDeg);
			if (Above.Cover > 0.05)
			{
				Text += "; sopra: nube di pioggia " + Format::Metres(Weather::RainCloudBaseM(Clouds)) + " - "
					+ Format::Metres(Weather::RainCloudTopM(Clouds, Sky, Above.TopFraction));
			}
			Add(TEXT("Nuvole"), Text);
		}
		// L'ultimo fulmine, e quando arriva il suo tuono: "tre secondi per chilometro".
		const UGeoWeatherSubsystem::FLightningReport Flashes = WeatherSubsystem->GetLightningReport();
		if (Flashes.bAny && Flashes.AgeSeconds < 120.0)
		{
			std::string Strike = "a " + Format::Distance(Flashes.DistanceM) + ", " + Format::Number(Flashes.AgeSeconds, 0) + " s fa";
			if (Flashes.ThunderInSeconds >= 0.0) { Strike += ", tuono fra " + Format::Number(Flashes.ThunderInSeconds, 0) + " s"; }
			Add(TEXT("Fulmine"), Strike);
		}
	}

	// La vegetazione: quanti alberi a schermo, in quante celle, quante aspettano.
	if (const UGeoVegetationSubsystem* Vegetation = GetWorld()->GetSubsystem<UGeoVegetationSubsystem>())
	{
		const FGeoVegetationStats Trees = Vegetation->GetStats();
		const int32 Shown = Trees.CelleVicine + Trees.CelleMedie + Trees.CelleLontane;
		if (Vegetation->IsEnabled() && (Shown > 0 || Trees.CelleInAttesa > 0))
		{
			std::string Text = Format::Number(Trees.GetTreeCount(), 0) + " alberi in " + Format::Number(Shown, 0) + " celle";
			if (Trees.CelleInAttesa > 0) { Text += ", " + Format::Number(Trees.CelleInAttesa, 0) + " in attesa"; }
			if (!Vegetation->GetMaterialProblem().IsEmpty()) { Text += " (manca il materiale)"; }
			Add(TEXT("Vegetazione"), Text);
		}
	}

	// Gli edifici: quanti a schermo, i monumenti, quante celle aspettano.
	if (const UGeoBuildingsSubsystem* Buildings = GetWorld()->GetSubsystem<UGeoBuildingsSubsystem>())
	{
		const FGeoBuildingsStats Built = Buildings->GetStats();
		const int32 Shown = Built.CelleVicine + Built.CelleMedie + Built.CelleLontane;
		if (Buildings->IsEnabled() && (Shown > 0 || Built.CelleInAttesa > 0))
		{
			std::string Text = Format::Number(Built.Edifici, 0) + " in " + Format::Number(Shown, 0) + " celle";
			if (Built.Monumenti > 0) { Text += ", " + Format::Number(Built.Monumenti, 0) + " monumenti"; }
			if (Built.CelleInAttesa > 0) { Text += ", " + Format::Number(Built.CelleInAttesa, 0) + " in attesa"; }
			if (!Buildings->GetMaterialProblem().IsEmpty()) { Text += " (manca il materiale)"; }
			Add(TEXT("Edifici"), Text);
		}
	}

	// Il cielo: l'ora (locale) e dove sta il sole.
	if (const UGeoSkySubsystem* SkySubsystem = GetWorld()->GetSubsystem<UGeoSkySubsystem>())
	{
		const FGeoSkyState& Sky = SkySubsystem->GetState();
		if (SkySubsystem->IsEnabled() && Sky.bValid)
		{
			std::string Clock = std::string(TCHAR_TO_UTF8(*Sky.LocalDate)) + " " + TCHAR_TO_UTF8(*Sky.LocalClock) + " " + TCHAR_TO_UTF8(*Sky.ZoneName);
			if (SkySubsystem->GetSpeed() != 1.0)
			{
				Clock += SkySubsystem->GetSpeed() == 0.0 ? ", fermo" : ", x" + Format::Number(SkySubsystem->GetSpeed(), 0);
			}
			Add(TEXT("Ora"), Clock);
			const double Altitude = Sky.Bodies.Sun.AltitudeDeg;
			std::string Sun = Altitude > -0.8 ? "alto " + Format::Number(Altitude, 0) + Format::Degree + ", " + Format::Heading(Sky.Bodies.Sun.AzimuthDeg)
			                                  : "sotto l'orizzonte (" + Format::Number(Altitude, 0) + Format::Degree + ")";
			Sun += std::string(", alba ") + TCHAR_TO_UTF8(*Sky.Sunrise) + ", tramonto " + TCHAR_TO_UTF8(*Sky.Sunset);
			Add(TEXT("Sole"), Sun);
			if (Altitude < 0.0 && Sky.Bodies.Moon.AltitudeDeg > 0.0)
			{
				Add(TEXT("Luna"), std::string(Sky.Bodies.bMoonWaxing ? "crescente " : "calante ")
					+ Format::Number(100.0 * Sky.Bodies.MoonIllumination, 0) + "%, alta " + Format::Number(Sky.Bodies.Moon.AltitudeDeg, 0) + Format::Degree);   /* percentuale, non-unita */
			}
		}
	}

	// I lampioni: solo quando sono accesi (di giorno la riga non serve).
	if (const UGeoStreetLightsSubsystem* StreetLights = GetWorld()->GetSubsystem<UGeoStreetLightsSubsystem>())
	{
		if (StreetLights->IsEnabled() && StreetLights->GetOnLevel() > 0.0 && StreetLights->GetStats().Lampioni > 0)
		{
			Add(TEXT("Lampioni"), std::string(TCHAR_TO_UTF8(*StreetLights->Describe())));
		}
	}

	// --- Disegno --------------------------------------------------------------
	const UFont* Font = GEngine->GetMediumFont();
	if (!Font) { return; }

	const float LineHeight = Font->GetMaxCharHeight() + 2.0f;
	const float Gap = 16.0f;
	const float Padding = 8.0f;
	float LabelWidth = 0.0f, ValueWidth = 0.0f;
	for (const TPair<FString, FString>& Line : Lines)
	{
		LabelWidth = FMath::Max(LabelWidth, static_cast<float>(Font->GetStringSize(*Line.Key)));
		ValueWidth = FMath::Max(ValueWidth, static_cast<float>(Font->GetStringSize(*Line.Value)));
	}

	const float BoxWidth = LabelWidth + Gap + ValueWidth + 2.0f * Padding;
	const float BoxHeight = LineHeight * Lines.Num() + 2.0f * Padding;
	const float Right = static_cast<float>(Canvas->ClipX) - 12.0f;
	// Un quarto dello schermo sotto il bordo: in alto a destra ci sono gia'
	// "stat fps" e "stat unit".
	const float Top = static_cast<float>(Canvas->ClipY) * 0.22f;
	const float Left = Right - BoxWidth;

	FCanvasTileItem Background(FVector2D(Left, Top), FVector2D(BoxWidth, BoxHeight), FLinearColor(0.0f, 0.0f, 0.0f, 0.45f));
	Background.BlendMode = SE_BLEND_Translucent;
	Canvas->DrawItem(Background);

	float Y = Top + Padding;
	for (const TPair<FString, FString>& Line : Lines)
	{
		FCanvasTextItem Label(FVector2D(Left + Padding, Y), FText::FromString(Line.Key), Font,
			FLinearColor(0.70f, 0.78f, 0.85f));
		Label.EnableShadow(FLinearColor::Black);
		Canvas->DrawItem(Label);

		const float ValueX = Right - Padding - static_cast<float>(Font->GetStringSize(*Line.Value));
		FCanvasTextItem Value(FVector2D(ValueX, Y), FText::FromString(Line.Value), Font, FLinearColor::White);
		Value.EnableShadow(FLinearColor::Black);
		Canvas->DrawItem(Value);

		Y += LineHeight;
	}
}

#include "GeoPositionHudSubsystem.h"

#include "Geo/GeoFormat.h"
#include "Georeference/GeoreferenceSubsystem.h"
#include "Mesh/TileMesh.h"
#include "Streaming/GeoTileStreamingSubsystem.h"
#include "Terrain/GeoTerrainSubsystem.h"
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
		if (Soil.GetWetness() > 0.01 || Soil.HasSnow())
		{
			std::string Text = Soil.GetWetness() > 0.01 ? "bagnato " + Format::Number(100.0 * Soil.GetWetness(), 0) + "%" : "asciutto"; /* percentuale, non-unita */
			if (Soil.HasSnow()) { Text += ", neve sopra " + Format::Metres(Soil.GetSnowCoverM()); }
			Add(TEXT("Suolo"), Text);
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

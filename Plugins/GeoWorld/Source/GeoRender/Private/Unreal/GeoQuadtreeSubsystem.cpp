#include "Unreal/GeoQuadtreeSubsystem.h"

#include "GeoCoreModule.h"
#include "Tiles/TilingScheme.h"
#include "Unreal/GeoTileStreamingSubsystem.h"
#include "Unreal/GeoreferenceSubsystem.h"

#include "DrawDebugHelpers.h"
#include "Engine/Engine.h"
#include "Engine/World.h"

using namespace GeoWorld::Quadtree;
namespace Tiles = GeoWorld::Tiles;

namespace
{
	/**
	 * Adattatore fra il selettore puro e il dataset/loader di Unreal.
	 *
	 * Esiste perche' il selettore non deve sapere niente ne' di GDAL ne' di
	 * Unreal: gli si passa un'interfaccia con tre domande e lui attraversa
	 * l'albero. Nei test la stessa interfaccia e' implementata da un dataset
	 * finto, il che rende la logica di attraversamento verificabile senza
	 * toccare nulla.
	 */
	class FStreamingAvailability : public ITileAvailability
	{
	public:
		explicit FStreamingAvailability(UGeoTileStreamingSubsystem* InStreaming)
			: Streaming(InStreaming) {}

		virtual bool TileExists(const FTileKey& Key) const override
		{
			if (!Streaming) { return false; }

			// L'indice del livello deve essere in memoria, altrimenti la
			// risposta sarebbe "non esiste" per il solo fatto di non averlo
			// ancora caricato. Costa una lettura la prima volta per livello.
			FString Error;
			const_cast<FGeoTileDataset&>(Streaming->GetDataset())
				.EnsureLevelIndex(static_cast<int32>(Key.Level), Error);

			return Streaming->GetDataset().TileExists(Key);
		}

		virtual bool GetHeightRange(const FTileKey& Key, double& OutMin, double& OutMax) const override
		{
			if (!Streaming) { return false; }
			float Minimum = 0.0f, Maximum = 0.0f;
			if (!Streaming->GetDataset().GetTileHeightRange(Key, Minimum, Maximum)) { return false; }
			OutMin = Minimum;
			OutMax = Maximum;
			return true;
		}

		virtual bool IsTileLoaded(const FTileKey& Key) const override
		{
			return Streaming && Streaming->GetTileState(Key) == EGeoTileState::Pronta;
		}

	private:
		UGeoTileStreamingSubsystem* Streaming = nullptr;
	};
}

bool UGeoQuadtreeSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	if (!Super::ShouldCreateSubsystem(Outer)) { return false; }
	const UWorld* World = Cast<UWorld>(Outer);
	return World && (World->WorldType == EWorldType::Game
	              || World->WorldType == EWorldType::PIE
	              || World->WorldType == EWorldType::Editor);
}

void UGeoQuadtreeSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	// Dipendenza esplicita: il quadtree non puo' funzionare senza il loader.
	// Dichiararla qui fa si' che Unreal costruisca i subsystem nell'ordine
	// giusto invece di lasciarlo al caso.
	Collection.InitializeDependency<UGeoTileStreamingSubsystem>();
	Streaming = GetWorld()->GetSubsystem<UGeoTileStreamingSubsystem>();
}

void UGeoQuadtreeSubsystem::Deinitialize()
{
	SelectedTiles.Empty();
	Result.Reset();
	Streaming = nullptr;
	Super::Deinitialize();
}

TStatId UGeoQuadtreeSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UGeoQuadtreeSubsystem, STATGROUP_Tickables);
}

void UGeoQuadtreeSubsystem::SetMaxScreenSpaceError(double Pixels)
{
	// Sotto il pixel non ha senso: si chiederebbe piu' dettaglio di quanto lo
	// schermo possa mostrare, pagandolo in memoria e in caricamenti.
	MaxScreenSpaceError = FMath::Clamp(Pixels, 0.5, 256.0);
}

void UGeoQuadtreeSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	if (bEnabled) { RunSelection(); }
	if (bShowDebugOverlay) { DrawDebugOverlay(); }
	if (bDrawSelection) { DrawSelection(); }
}

bool UGeoQuadtreeSubsystem::BuildViewParameters(FViewParameters& OutView)
{
	UWorld* World = GetWorld();
	if (!World) { return false; }

	UGeoreferenceSubsystem* Georeference = World->GetSubsystem<UGeoreferenceSubsystem>();
	if (!Georeference) { return false; }

	UGeoreferenceSubsystem::FActiveViewInfo Info;
	if (!Georeference->GetActiveViewInfo(Info)) { return false; }

	const FGeoreferenceSnapshot Snapshot = Georeference->GetSnapshot();

	OutView.CameraEcef = Snapshot.UnrealToEcef(Info.Location);

	// La base della camera passa da spazio Unreal a ECEF con una ROTAZIONE
	// soltanto: sono direzioni, non punti.
	const FRotationMatrix Rotation(Info.Rotation);
	OutView.Forward = Snapshot.UnrealDirectionToEcef(Rotation.GetScaledAxis(EAxis::X));
	OutView.Right   = Snapshot.UnrealDirectionToEcef(Rotation.GetScaledAxis(EAxis::Y));
	OutView.Up      = Snapshot.UnrealDirectionToEcef(Rotation.GetScaledAxis(EAxis::Z));

	// NOTA UE: il FOV di Unreal e' ORIZZONTALE. La formula dell'errore su
	// schermo vuole quello verticale, e le due grandezze differiscono per le
	// proporzioni dello schermo: confonderle sbaglia il LOD di quasi il doppio
	// su un monitor 16:9.
	const double HalfHorizontal = FMath::DegreesToRadians(Info.HorizontalFovDegrees) * 0.5;
	const double Aspect = FMath::Max(0.1f, Info.AspectRatio);
	OutView.VerticalFovRad = 2.0 * FMath::Atan(FMath::Tan(HalfHorizontal) / Aspect);
	OutView.AspectRatio = Aspect;
	OutView.ScreenHeightPixels = FMath::Max(1, Info.ScreenSize.Y);
	OutView.MaxScreenSpaceError = MaxScreenSpaceError;
	OutView.NearClipMetres = 1.0;

	return true;
}

bool UGeoQuadtreeSubsystem::RunSelection()
{
	if (!Streaming || !Streaming->IsDatasetOpen()) { return false; }

	FViewParameters View;
	if (bFrozen && bHasFrozenView)
	{
		View = FrozenView;
		View.MaxScreenSpaceError = MaxScreenSpaceError;
	}
	else
	{
		if (!BuildViewParameters(View)) { return false; }
		FrozenView = View;
		bHasFrozenView = true;
	}

	const double Started = FPlatformTime::Seconds();

	const FGeoTileDataset& Dataset = Streaming->GetDataset();
	const FStreamingAvailability Availability(Streaming);

	SelectTiles(View, Availability,
		static_cast<uint32>(Dataset.GetMinLevel()),
		static_cast<uint32>(Dataset.GetMaxLevel()), Result);

	const double Elapsed = FPlatformTime::Seconds() - Started;

	// Le richieste partono in ordine di urgenza: il selettore le ha gia'
	// ordinate, e il loader ha la sua coda a priorita'.
	for (const FTileRequest& Request : Result.ToLoad)
	{
		Streaming->RequestTile(Request.Key, Request.Priority);
	}

	// Le tile che stiamo disegnando si pinnano: sfrattarle mentre sono a
	// schermo significherebbe ricaricarle subito dopo.
	SelectedTiles.Reset(Result.ToRender.size());
	for (const FSelectedTile& Tile : Result.ToRender)
	{
		SelectedTiles.Add(Tile);
	}

	Stats.NodiVisitati = Result.NodesVisited;
	Stats.TileDisegnate = static_cast<int32>(Result.ToRender.size());
	Stats.TileRichieste = static_cast<int32>(Result.ToLoad.size());
	Stats.ScartateFrustum = Result.CulledByFrustum;
	Stats.ScartateOrizzonte = Result.CulledByHorizon;
	Stats.ScartateAssenti = Result.CulledByMissing;
	Stats.NodiRaffinati = Result.RefinedNodes;
	Stats.ErrorePeggiorePx = static_cast<float>(Result.WorstScreenSpaceError);
	Stats.SogliaErrorePx = static_cast<float>(MaxScreenSpaceError);
	Stats.TempoSelezioneMs = static_cast<float>(Elapsed * 1000.0);

	Stats.LivelloMinimo = MAX_int32;
	Stats.LivelloMassimo = 0;
	for (const FSelectedTile& Tile : SelectedTiles)
	{
		Stats.LivelloMinimo = FMath::Min(Stats.LivelloMinimo, static_cast<int32>(Tile.Key.Level));
		Stats.LivelloMassimo = FMath::Max(Stats.LivelloMassimo, static_cast<int32>(Tile.Key.Level));
	}
	if (SelectedTiles.Num() == 0) { Stats.LivelloMinimo = 0; }

	return true;
}

// ============================================================================
//  Debug
// ============================================================================

void UGeoQuadtreeSubsystem::DrawDebugOverlay()
{
	if (!GEngine) { return; }

	int32 Key = 0x6E80;
	auto Line = [&Key](const FColor& Colour, const FString& Text)
	{
		GEngine->AddOnScreenDebugMessage(Key++, 0.0f, Colour, Text);
	};

	Line(FColor::Cyan, TEXT("--- GeoWorld | Fase 4: quadtree e LOD ---"));

	if (!Streaming || !Streaming->IsDatasetOpen())
	{
		Line(FColor::Yellow, TEXT("Nessun dataset. Usa: geo.Tiles.Open <cartella>"));
		return;
	}
	if (!bEnabled)
	{
		Line(FColor::Yellow, TEXT("Selezione ferma. Attiva con: geo.Lod.Enable 1"));
		return;
	}

	Line(bFrozen ? FColor::Orange : FColor::Green,
		FString::Printf(TEXT("Soglia errore : %.1f px%s"),
			Stats.SogliaErrorePx, bFrozen ? TEXT("   [VISTA CONGELATA]") : TEXT("")));

	// Se l'errore peggiore supera stabilmente la soglia, il LOD non sta
	// tenendo il passo: o mancano tile, o il dataset non arriva abbastanza in
	// profondita' per questa distanza.
	Line(Stats.ErrorePeggiorePx > Stats.SogliaErrorePx * 1.5f ? FColor::Yellow : FColor::White,
		FString::Printf(TEXT("Errore peggiore: %.1f px"), Stats.ErrorePeggiorePx));

	Line(FColor::White, FString::Printf(TEXT("Tile disegnate: %d   livelli %d..%d"),
		Stats.TileDisegnate, Stats.LivelloMinimo, Stats.LivelloMassimo));

	Line(Stats.TileRichieste > 0 ? FColor::Cyan : FColor::White,
		FString::Printf(TEXT("Da caricare   : %d"), Stats.TileRichieste));

	Line(FColor::White, FString::Printf(
		TEXT("Nodi visitati : %d   (frustum %d, orizzonte %d, assenti %d)"),
		Stats.NodiVisitati, Stats.ScartateFrustum, Stats.ScartateOrizzonte, Stats.ScartateAssenti));

	Line(Stats.TempoSelezioneMs > 2.0f ? FColor::Yellow : FColor::White,
		FString::Printf(TEXT("Tempo selezione: %.3f ms"), Stats.TempoSelezioneMs));
}

void UGeoQuadtreeSubsystem::DrawSelection() const
{
	const UWorld* World = GetWorld();
	if (!World || SelectedTiles.Num() == 0) { return; }

	const UGeoreferenceSubsystem* Georeference = World->GetSubsystem<UGeoreferenceSubsystem>();
	if (!Georeference) { return; }

	const FGeoreferenceSnapshot Snapshot = Georeference->GetSnapshot();

	static const FColor LevelColours[] = {
		FColor::White, FColor(180, 180, 180), FColor::Silver, FColor::Emerald,
		FColor::Green, FColor::Cyan, FColor::Blue, FColor::Purple,
		FColor::Magenta, FColor::Orange, FColor::Yellow, FColor::Red };

	constexpr int32 SegmentsPerEdge = 6;
	constexpr int32 MaxTilesToDraw = 600;

	int32 Drawn = 0;
	for (const FSelectedTile& Tile : SelectedTiles)
	{
		if (Drawn++ >= MaxTilesToDraw) { break; }

		float MinHeight = 0.0f, MaxHeight = 0.0f;
		Streaming->GetDataset().GetTileHeightRange(Tile.Key, MinHeight, MaxHeight);

		const Tiles::FTileBounds Bounds =
			Tiles::GetTileBounds(Tile.Key.Level, Tile.Key.X, Tile.Key.Y);
		const FColor Colour = LevelColours[Tile.Key.Level % UE_ARRAY_COUNT(LevelColours)];

		auto ToWorld = [&](double Lon, double Lat)
		{
			return Snapshot.GeodeticToUnreal(
				GeoWorld::Core::FGeodetic::FromDegrees(Lat, Lon, MaxHeight));
		};

		// Solo il perimetro alla quota massima: qui interessa vedere la
		// TASSELLATURA scelta dal LOD, non i volumi. I volumi completi li
		// disegna geo.Tiles.Draw.
		const double CornerLons[4] = { Bounds.West, Bounds.East, Bounds.East, Bounds.West };
		const double CornerLats[4] = { Bounds.North, Bounds.North, Bounds.South, Bounds.South };

		for (int32 Edge = 0; Edge < 4; ++Edge)
		{
			const int32 Next = (Edge + 1) % 4;
			for (int32 Step = 0; Step < SegmentsPerEdge; ++Step)
			{
				const double T0 = static_cast<double>(Step) / SegmentsPerEdge;
				const double T1 = static_cast<double>(Step + 1) / SegmentsPerEdge;
				DrawDebugLine(World,
					ToWorld(FMath::Lerp(CornerLons[Edge], CornerLons[Next], T0),
					        FMath::Lerp(CornerLats[Edge], CornerLats[Next], T0)),
					ToWorld(FMath::Lerp(CornerLons[Edge], CornerLons[Next], T1),
					        FMath::Lerp(CornerLats[Edge], CornerLats[Next], T1)),
					Colour, false, -1.f, 0, 25.f);
			}
		}

		// Etichetta solo sulle tile piu' grandi a schermo, altrimenti a livello
		// 14 si sovrappongono a centinaia e non si legge piu' niente.
		if (Tile.ScreenSpaceError > MaxScreenSpaceError * 0.5)
		{
			DrawDebugString(const_cast<UWorld*>(World),
				ToWorld(Bounds.CentreLon(), Bounds.CentreLat()),
				FString::Printf(TEXT("L%u  %.1f px"), Tile.Key.Level, Tile.ScreenSpaceError),
				nullptr, Colour, 0.0f, true);
		}
	}
}

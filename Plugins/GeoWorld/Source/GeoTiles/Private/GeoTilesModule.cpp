// =============================================================================
//  Modulo GeoTiles -- formato dei tile, dataset, loader asincrono, cache LRU.
//  Console command per la Fase 3.
// =============================================================================
#include "Modules/ModuleManager.h"

#include "GeoCoreModule.h"
#include "Unreal/GeoTileStreamingSubsystem.h"
#include "Unreal/GeoreferenceSubsystem.h"
#include "Tiles/TilingScheme.h"

#include "Engine/Engine.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"

IMPLEMENT_MODULE(FDefaultModuleImpl, GeoTiles)

namespace GeoTilesConsole
{
	static UGeoTileStreamingSubsystem* Get(UWorld* World)
	{
		if (!World) { return nullptr; }
		return World->GetSubsystem<UGeoTileStreamingSubsystem>();
	}

	static void Report(const FString& Message, const FColor& Color = FColor::Cyan)
	{
		UE_LOG(LogGeoWorld, Log, TEXT("%s"), *Message);
		if (GEngine) { GEngine->AddOnScreenDebugMessage(-1, 10.0f, Color, Message); }
	}
}

// --- geo.Tiles.Open ---------------------------------------------------------
static FAutoConsoleCommandWithWorldAndArgs GeoTilesOpenCommand(
	TEXT("geo.Tiles.Open"),
	TEXT("geo.Tiles.Open <cartella> - apre un dataset prodotto dalla pipeline."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
		[](const TArray<FString>& Args, UWorld* World)
	{
		UGeoTileStreamingSubsystem* Streaming = GeoTilesConsole::Get(World);
		if (!Streaming) { return; }

		if (Args.Num() < 1)
		{
			GeoTilesConsole::Report(TEXT("Uso: geo.Tiles.Open <cartella del dataset>"), FColor::Red);
			return;
		}

		FString Error;
		if (Streaming->OpenDataset(Args[0], Error))
		{
			const FGeoTileDataset& Dataset = Streaming->GetDataset();
			double West, South, East, North;
			Dataset.GetBoundingBox(West, South, East, North);
			GeoTilesConsole::Report(FString::Printf(
				TEXT("Dataset '%s': livelli %d..%d, bbox %.4f %.4f %.4f %.4f"),
				*Dataset.GetDatasetName(), Dataset.GetMinLevel(), Dataset.GetMaxLevel(),
				West, South, East, North));
		}
		else
		{
			GeoTilesConsole::Report(FString::Printf(TEXT("Errore: %s"), *Error), FColor::Red);
		}
	}));

// --- geo.Tiles.Info ---------------------------------------------------------
static FAutoConsoleCommandWithWorld GeoTilesInfoCommand(
	TEXT("geo.Tiles.Info"),
	TEXT("Descrive il dataset aperto, livello per livello."),
	FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* World)
	{
		UGeoTileStreamingSubsystem* Streaming = GeoTilesConsole::Get(World);
		if (!Streaming) { return; }

		if (!Streaming->IsDatasetOpen())
		{
			GeoTilesConsole::Report(TEXT("Nessun dataset aperto. Usa geo.Tiles.Open"), FColor::Yellow);
			return;
		}

		const FGeoTileDataset& Dataset = Streaming->GetDataset();
		GeoTilesConsole::Report(FString::Printf(TEXT("Dataset  : %s"), *Dataset.GetDatasetName()));
		GeoTilesConsole::Report(FString::Printf(TEXT("Cartella : %s"), *Dataset.GetRootDirectory()));
		GeoTilesConsole::Report(FString::Printf(TEXT("Datum Z  : %s"), *Dataset.GetVerticalDatum()));

		for (const FGeoTileDataset::FLevelInfo& Level : Dataset.GetLevels())
		{
			const double SpacingDeg = GeoWorld::Tiles::PostSpacingDeg(Level.Level);
			GeoTilesConsole::Report(FString::Printf(
				TEXT("  livello %2d: %7d tile, quote %.1f..%.1f m, passo %.2f m%s"),
				Level.Level, Level.TileCount, Level.MinHeight, Level.MaxHeight,
				SpacingDeg * 111132.0,
				Level.bIndexLoaded ? TEXT("  [indice in memoria]") : TEXT("")));
		}
	}));

// --- geo.Tiles.Stats --------------------------------------------------------
static FAutoConsoleCommandWithWorld GeoTilesStatsCommand(
	TEXT("geo.Tiles.Stats"),
	TEXT("Statistiche di streaming e cache."),
	FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* World)
	{
		UGeoTileStreamingSubsystem* Streaming = GeoTilesConsole::Get(World);
		if (!Streaming) { return; }

		const FGeoTileStreamingStats Stats = Streaming->GetStats();
		GeoTilesConsole::Report(FString::Printf(TEXT("Tile in cache   : %d  (pinnate %d)"),
			Stats.TileCaricate, Stats.TileVisibili));
		GeoTilesConsole::Report(FString::Printf(TEXT("Memoria cache   : %.1f / %.0f MB"),
			Stats.MemoriaCacheMB, Stats.BudgetCacheMB));
		GeoTilesConsole::Report(FString::Printf(TEXT("Tasso di hit    : %.1f%%   evizioni %d"),
			Stats.TassoHit * 100.0f, Stats.Evizioni));   // non-unita: frazione -> percentuale
		GeoTilesConsole::Report(FString::Printf(TEXT("Richieste attive: %d"), Stats.RichiesteInCorso));
		GeoTilesConsole::Report(FString::Printf(TEXT("Tempo medio     : %.2f ms per tile"),
			Stats.TempoMedioCaricamentoMs));
		if (Stats.Errori > 0)
		{
			GeoTilesConsole::Report(FString::Printf(TEXT("ERRORI          : %d"), Stats.Errori), FColor::Red);
		}
	}));

// --- geo.Tiles.Load ---------------------------------------------------------
static FAutoConsoleCommandWithWorldAndArgs GeoTilesLoadCommand(
	TEXT("geo.Tiles.Load"),
	TEXT("geo.Tiles.Load <livello> [lat] [lon] - carica la tile sotto la camera o sul punto dato."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
		[](const TArray<FString>& Args, UWorld* World)
	{
		UGeoTileStreamingSubsystem* Streaming = GeoTilesConsole::Get(World);
		if (!Streaming || !Streaming->IsDatasetOpen())
		{
			GeoTilesConsole::Report(TEXT("Nessun dataset aperto."), FColor::Red);
			return;
		}

		const int32 Level = (Args.Num() >= 1) ? FCString::Atoi(*Args[0])
		                                      : Streaming->GetDataset().GetMaxLevel();
		double Latitude = 0.0, Longitude = 0.0;

		if (Args.Num() >= 3)
		{
			Latitude = FCString::Atod(*Args[1]);
			Longitude = FCString::Atod(*Args[2]);
		}
		else if (UGeoreferenceSubsystem* Georeference = World->GetSubsystem<UGeoreferenceSubsystem>())
		{
			// Senza coordinate esplicite si usa il punto sotto la camera: e' il
			// modo piu' rapido di provare il caricamento mentre si vola.
			FVector ViewLocation;
			if (Georeference->GetActiveViewLocation(ViewLocation))
			{
				const FGeoCoordinate Where = FGeoCoordinate::FromGeodetic(
					Georeference->GetSnapshot().UnrealToGeodetic(ViewLocation));
				Latitude = Where.Latitude;
				Longitude = Where.Longitude;
			}
		}

		uint32 X = 0, Y = 0;
		GeoWorld::Tiles::TileForLonLat(static_cast<uint32>(Level), Longitude, Latitude, X, Y);
		const GeoWorld::Tiles::FTileKey Key{ static_cast<uint32>(Level), X, Y };

		const EGeoTileState State = Streaming->RequestTile(Key, /*Priority=*/3);
		const TCHAR* StateText = TEXT("?");
		switch (State)
		{
			case EGeoTileState::Pronta:        StateText = TEXT("gia' in cache"); break;
			case EGeoTileState::InCaricamento: StateText = TEXT("caricamento avviato"); break;
			case EGeoTileState::Assente:       StateText = TEXT("non esiste nel dataset"); break;
			case EGeoTileState::Errore:        StateText = TEXT("errore"); break;
			default: break;
		}

		GeoTilesConsole::Report(FString::Printf(
			TEXT("Tile %d/%u/%u per %.5f, %.5f -> %s"), Level, X, Y, Latitude, Longitude, StateText));
	}));

// --- geo.Tiles.Budget -------------------------------------------------------
static FAutoConsoleCommandWithWorldAndArgs GeoTilesBudgetCommand(
	TEXT("geo.Tiles.Budget"),
	TEXT("geo.Tiles.Budget <MB> - cambia a caldo il budget della cache."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
		[](const TArray<FString>& Args, UWorld* World)
	{
		UGeoTileStreamingSubsystem* Streaming = GeoTilesConsole::Get(World);
		if (!Streaming) { return; }

		if (Args.Num() >= 1) { Streaming->SetCacheBudgetMB(FCString::Atoi(*Args[0])); }
		GeoTilesConsole::Report(FString::Printf(TEXT("Budget della cache: %d MB"),
			Streaming->GetCacheBudgetMB()));
	}));

// --- geo.Tiles.Clear --------------------------------------------------------
static FAutoConsoleCommandWithWorld GeoTilesClearCommand(
	TEXT("geo.Tiles.Clear"),
	TEXT("Svuota la cache e azzera le statistiche."),
	FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* World)
	{
		if (UGeoTileStreamingSubsystem* Streaming = GeoTilesConsole::Get(World))
		{
			Streaming->ClearCache();
			Streaming->ResetStats();
			GeoTilesConsole::Report(TEXT("Cache svuotata."));
		}
	}));

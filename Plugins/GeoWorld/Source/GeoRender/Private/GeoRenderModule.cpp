// =============================================================================
//  Modulo GeoRender. In Fase 1 contiene solo gli strumenti di verifica visiva;
//  dalla Fase 4 ospitera' il quadtree, la selezione LOD e la generazione mesh.
// =============================================================================
#include "Modules/ModuleManager.h"

#include "GeoMarkerActor.h"
#include "Georeference/GeoTransformComponent.h"
#include "Lod/GeoQuadtreeSubsystem.h"
#include "Terrain/GeoTerrainSubsystem.h"
#include "Streaming/GeoTileStreamingSubsystem.h"
#include "Georeference/GeoreferenceSubsystem.h"
#include "Georeference/GeoWorldTypes.h"
#include "GeoCoreModule.h"

#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/IConsoleManager.h"
#include "Geo/GeoUnits.h"

#include <cmath>

IMPLEMENT_MODULE(FDefaultModuleImpl, GeoRender)

namespace GeoMarkers
{
	struct FNamedPlace
	{
		const TCHAR* Name;
		double Latitude;
		double Longitude;
		double HeightMeters;
	};

	// Punti di verifica sull'Italia. Le coordinate sono controllabili su una
	// qualunque mappa: e' proprio questo il senso del test visivo.
	static const FNamedPlace Places[] =
	{
		{ TEXT("Colosseo"),          41.890210, 12.492231,   40.0 },
		{ TEXT("Duomo di Milano"),   45.464200,  9.191900,  120.0 },
		{ TEXT("Torre di Pisa"),     43.722950, 10.396600,   20.0 },
		{ TEXT("Monte Bianco"),      45.832600,  6.865200, 4808.0 },
		{ TEXT("Etna"),              37.751000, 14.993400, 3357.0 },
	};
}

// --- geo.SpawnMarkers -------------------------------------------------------
static FAutoConsoleCommandWithWorld GeoSpawnMarkersCommand(
	TEXT("geo.SpawnMarkers"),
	TEXT("Piazza i cubi di verifica sui punti noti dell'Italia."),
	FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* World)
	{
		if (!World)
		{
			return;
		}

		int32 SpawnedCount = 0;

		for (const GeoMarkers::FNamedPlace& Place : GeoMarkers::Places)
		{
			// NOTA UE: SpawnActor richiede i parametri di spawn se vogliamo
			// impostare proprieta' PRIMA che l'attore sia completamente
			// inizializzato. Qui ci basta il caso semplice: spawn e poi
			// configurazione, perche' UGeoTransformComponent si riposiziona da
			// solo appena gli si cambia la coordinata.
			AGeoMarkerActor* Marker = World->SpawnActor<AGeoMarkerActor>();
			if (!Marker)
			{
				continue;
			}

			Marker->Label = Place.Name;

#if WITH_EDITOR
			// NOTA UE: SetActorLabel (il nome leggibile nel World Outliner)
			// esiste SOLO nelle build con editor. Senza la guardia il progetto
			// compila nell'editor e poi si rompe al packaging.
			Marker->SetActorLabel(Place.Name);
#endif
			Marker->GetGeoTransform()->SetGeoCoordinate(
				FGeoCoordinate(Place.Latitude, Place.Longitude, Place.HeightMeters));

			++SpawnedCount;
		}

		const FString Message = FString::Printf(TEXT("[GeoWorld] %d marker piazzati."), SpawnedCount);
		UE_LOG(LogGeoWorld, Log, TEXT("%s"), *Message);
		if (GEngine)
		{
			GEngine->AddOnScreenDebugMessage(-1, 8.0f, FColor::Cyan, Message);
		}
	}));

// --- geo.ClearMarkers -------------------------------------------------------
static FAutoConsoleCommandWithWorld GeoClearMarkersCommand(
	TEXT("geo.ClearMarkers"),
	TEXT("Rimuove tutti i marker di verifica."),
	FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* World)
	{
		if (!World)
		{
			return;
		}

		// NOTA UE: TActorIterator e' il modo corretto di iterare gli attori di
		// un mondo. Non si puo' distruggere mentre si itera, quindi si raccoglie
		// prima e si distrugge dopo.
		TArray<AActor*> ToDestroy;
		for (TActorIterator<AGeoMarkerActor> It(World); It; ++It)
		{
			ToDestroy.Add(*It);
		}

		for (AActor* Actor : ToDestroy)
		{
			Actor->Destroy();
		}

		UE_LOG(LogGeoWorld, Log, TEXT("[GeoWorld] %d marker rimossi."), ToDestroy.Num());
	}));


// ============================================================================
//  geo.Diag -- diagnostica del jitter.
//
//  PERCHE' ESISTE: "il cubo balla" ha almeno tre cause completamente diverse,
//  visivamente quasi identiche e con rimedi opposti:
//
//    1. il Tick del subsystem non gira  -> il rebasing non parte mai e la
//       camera resta a coordinate enormi;
//    2. il rebasing gira ma la soglia e' troppo alta per il caso d'uso;
//    3. le coordinate sono piccole e corrette, e a ballare e' l'ANTIALIASING
//       TEMPORALE (TSR/TAA), che su uno spigolo netto contro il cielo vuoto
//       produce esattamente un tremolio orizzontale.
//
//  Discutere di impressioni visive fa perdere pomeriggi. Questo comando stampa
//  i tre numeri che distinguono i casi e propone un verdetto.
// ============================================================================

namespace GeoDiag
{
	/**
	 * Distanza fra due float consecutivi (ULP) alla magnitudine data, in unita'
	 * Unreal. E' la "granularita'" con cui un float puo' rappresentare una
	 * posizione: se vale 30 unita', nessuna geometria potra' mai stare ferma
	 * meglio di 30 cm, per quanto giusta sia la matematica a monte.
	 */
	static double FloatUlpAt(double MagnitudeUu)
	{
		const float AsFloat = static_cast<float>(FMath::Abs(MagnitudeUu));
		if (AsFloat <= 0.0f)
		{
			return 0.0;
		}
		const float NextFloat = std::nextafterf(AsFloat, AsFloat * 2.0f);
		return static_cast<double>(NextFloat) - static_cast<double>(AsFloat);
	}

	static int32 GetCVarInt(const TCHAR* Name, int32 Fallback)
	{
		if (const IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(Name))
		{
			return Variable->GetInt();
		}
		return Fallback;
	}

	static const TCHAR* AntiAliasingName(int32 Method)
	{
		switch (Method)
		{
			case 0:  return TEXT("nessuno");
			case 1:  return TEXT("FXAA");
			case 2:  return TEXT("TAA (temporale)");
			case 3:  return TEXT("MSAA");
			case 4:  return TEXT("TSR (temporale)");
			default: return TEXT("sconosciuto");
		}
	}

	static void Line(const FString& Text, const FColor& Color = FColor::White)
	{
		UE_LOG(LogGeoWorld, Log, TEXT("%s"), *Text);
		if (GEngine)
		{
			GEngine->AddOnScreenDebugMessage(-1, 25.0f, Color, Text);
		}
	}
}

static FAutoConsoleCommandWithWorld GeoDiagCommand(
	TEXT("geo.Diag"),
	TEXT("Diagnostica del jitter: stampa i numeri che distinguono precisione, rebasing e antialiasing."),
	FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* World)
	{
		if (!World)
		{
			return;
		}

		UGeoreferenceSubsystem* Georeference = World->GetSubsystem<UGeoreferenceSubsystem>();
		if (!Georeference)
		{
			GeoDiag::Line(TEXT("[geo.Diag] Subsystem assente in questo mondo."), FColor::Red);
			return;
		}

		const FGeoreferenceSnapshot Snapshot = Georeference->GetSnapshot();
		const double ThresholdKm = Georeference->GetRebaseThresholdMeters() / 1000.0;
		const uint64 TickCount = Georeference->GetTickCount();

		GeoDiag::Line(TEXT("================ geo.Diag ================"), FColor::Cyan);

		// --- 1. Il tick gira? E' la domanda che va risolta per prima: se la
		//        risposta e' no, tutto il resto e' rumore.
		GeoDiag::Line(FString::Printf(TEXT("Tick eseguiti     : %llu"), TickCount),
			TickCount == 0 ? FColor::Red : FColor::Green);

		GeoDiag::Line(FString::Printf(TEXT("Rebase eseguiti   : %u   (auto: %s, soglia %.2f km)"),
			Georeference->GetRebaseCount(),
			Georeference->IsAutoRebaseEnabled() ? TEXT("ON") : TEXT("OFF"),
			ThresholdKm));

		GeoDiag::Line(FString::Printf(TEXT("Origine           : %s"),
			*Georeference->GetOriginCoordinate().ToDisplayString()));

		// --- 2. In che regime di precisione siamo?
		double CameraDistanceKm = 0.0;
		double CameraUlpUu = 0.0;
		FVector ViewLocation = FVector::ZeroVector;
		const bool bHasView = Georeference->GetActiveViewLocation(ViewLocation);

		if (bHasView)
		{
			CameraDistanceKm = Snapshot.DistanceFromOriginMeters(ViewLocation) / 1000.0;
			CameraUlpUu = GeoDiag::FloatUlpAt(ViewLocation.GetAbsMax());

			GeoDiag::Line(FString::Printf(TEXT("Camera (unreal)   : %.0f, %.0f, %.0f uu"),
				ViewLocation.X, ViewLocation.Y, ViewLocation.Z));
			GeoDiag::Line(FString::Printf(TEXT("Dist. dall'origine: %.3f km"), CameraDistanceKm),
				CameraDistanceKm > ThresholdKm ? FColor::Red : FColor::Green);
			GeoDiag::Line(FString::Printf(TEXT("ULP float qui     : %.4f uu  (%.2f mm)"),
				CameraUlpUu, CameraUlpUu * GeoWorld::Units::UuToMeters * 1000.0),
				CameraUlpUu > 1.0 ? FColor::Red : FColor::Green);
		}
		else
		{
			GeoDiag::Line(TEXT("Camera            : NON TROVATA"), FColor::Red);
		}

		// --- 3. Dove stanno i marker?
		int32 MarkerCount = 0;
		double WorstMarkerUlpUu = 0.0;
		for (TActorIterator<AGeoMarkerActor> It(World); It; ++It)
		{
			const FVector Location = It->GetActorLocation();
			const double Ulp = GeoDiag::FloatUlpAt(Location.GetAbsMax());
			WorstMarkerUlpUu = FMath::Max(WorstMarkerUlpUu, Ulp);

			GeoDiag::Line(FString::Printf(TEXT("  marker %-16s %8.1f km dall'origine, ULP %.3f uu"),
				*It->Label,
				Snapshot.DistanceFromOriginMeters(Location) / 1000.0,
				Ulp));
			++MarkerCount;
		}
		if (MarkerCount == 0)
		{
			GeoDiag::Line(TEXT("  (nessun marker: lancia geo.SpawnMarkers)"), FColor::Yellow);
		}

		// --- 4. Antialiasing
		const int32 AntiAliasingMethod = GeoDiag::GetCVarInt(TEXT("r.AntiAliasingMethod"), -1);
		const bool bTemporalAA = (AntiAliasingMethod == 2 || AntiAliasingMethod == 4);
		GeoDiag::Line(FString::Printf(TEXT("Antialiasing      : %d (%s)"),
			AntiAliasingMethod, GeoDiag::AntiAliasingName(AntiAliasingMethod)),
			bTemporalAA ? FColor::Yellow : FColor::White);

		// --- 5. Verdetto
		GeoDiag::Line(TEXT("------------------ verdetto ------------------"), FColor::Cyan);

		if (TickCount == 0)
		{
			GeoDiag::Line(TEXT("CAUSA 1: il Tick del subsystem non viene mai chiamato."), FColor::Red);
			GeoDiag::Line(TEXT("  Il rebasing non puo' partire e la camera resta a coordinate enormi."), FColor::Red);
		}
		else if (bHasView && CameraDistanceKm > ThresholdKm * 1.1)
		{
			GeoDiag::Line(TEXT("CAUSA 2: il tick gira ma il rebase non sta scattando."), FColor::Red);
			GeoDiag::Line(FString::Printf(
				TEXT("  Camera a %.1f km con soglia %.1f km. Prova: geo.Rebase"),
				CameraDistanceKm, ThresholdKm), FColor::Red);
		}
		else if (CameraUlpUu > 1.0 || WorstMarkerUlpUu > 1.0)
		{
			GeoDiag::Line(TEXT("CAUSA 2b: coordinate troppo grandi per un float."), FColor::Yellow);
			GeoDiag::Line(FString::Printf(
				TEXT("  ULP fino a %.2f uu: abbassa la soglia con geo.RebaseThreshold 2"),
				FMath::Max(CameraUlpUu, WorstMarkerUlpUu)), FColor::Yellow);
		}
		else if (bTemporalAA)
		{
			GeoDiag::Line(TEXT("Le coordinate sono PICCOLE e corrette: il jitter non e' di precisione."), FColor::Green);
			GeoDiag::Line(TEXT("CAUSA 3 probabile: antialiasing temporale (TSR/TAA)."), FColor::Yellow);
			GeoDiag::Line(TEXT("  Verifica in 5 secondi:  r.AntiAliasingMethod 0"), FColor::Yellow);
			GeoDiag::Line(TEXT("  Se il tremolio sparisce, era quello e NON la geodesia."), FColor::Yellow);
		}
		else
		{
			GeoDiag::Line(TEXT("Coordinate piccole e antialiasing non temporale:"), FColor::Green);
			GeoDiag::Line(TEXT("  il jitter non e' spiegato da nessuna delle cause note. Segnalalo."), FColor::Yellow);
		}

		GeoDiag::Line(TEXT("=========================================="), FColor::Cyan);
	}));

// ============================================================================
//  FASE 4 -- comandi del quadtree e della selezione LOD
// ============================================================================

namespace GeoLodConsole
{
	static UGeoQuadtreeSubsystem* Get(UWorld* World)
	{
		return World ? World->GetSubsystem<UGeoQuadtreeSubsystem>() : nullptr;
	}

	static void Report(const FString& Message, const FColor& Colour = FColor::Cyan)
	{
		UE_LOG(LogGeoWorld, Log, TEXT("%s"), *Message);
		if (GEngine) { GEngine->AddOnScreenDebugMessage(-1, 10.0f, Colour, Message); }
	}
}

// --- geo.Lod.Enable ---------------------------------------------------------
static FAutoConsoleCommandWithWorldAndArgs GeoLodEnableCommand(
	TEXT("geo.Lod.Enable"),
	TEXT("geo.Lod.Enable <0|1> - attiva la selezione LOD per frame."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
		[](const TArray<FString>& Args, UWorld* World)
	{
		UGeoQuadtreeSubsystem* Quadtree = GeoLodConsole::Get(World);
		if (!Quadtree) { return; }

		const bool bEnabled = (Args.Num() >= 1) ? (FCString::Atoi(*Args[0]) != 0)
		                                        : !Quadtree->IsEnabled();
		Quadtree->SetEnabled(bEnabled);
		GeoLodConsole::Report(FString::Printf(TEXT("Selezione LOD: %s"),
			bEnabled ? TEXT("ON") : TEXT("OFF")));
	}));

// --- geo.Lod.Error ----------------------------------------------------------
static FAutoConsoleCommandWithWorldAndArgs GeoLodErrorCommand(
	TEXT("geo.Lod.Error"),
	TEXT("geo.Lod.Error <pixel> - errore su schermo tollerato. Piu' basso = piu' dettaglio."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
		[](const TArray<FString>& Args, UWorld* World)
	{
		UGeoQuadtreeSubsystem* Quadtree = GeoLodConsole::Get(World);
		if (!Quadtree) { return; }

		if (Args.Num() >= 1) { Quadtree->SetMaxScreenSpaceError(FCString::Atod(*Args[0])); }
		GeoLodConsole::Report(FString::Printf(
			TEXT("Soglia errore su schermo: %.2f px  (dimezzarla quadruplica le tile)"),
			Quadtree->GetMaxScreenSpaceError()));
	}));

// --- geo.Lod.Freeze ---------------------------------------------------------
static FAutoConsoleCommandWithWorldAndArgs GeoLodFreezeCommand(
	TEXT("geo.Lod.Freeze"),
	TEXT("geo.Lod.Freeze <0|1> - congela la vista usata dal LOD e lascia muovere la camera."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
		[](const TArray<FString>& Args, UWorld* World)
	{
		UGeoQuadtreeSubsystem* Quadtree = GeoLodConsole::Get(World);
		if (!Quadtree) { return; }

		const bool bFrozen = (Args.Num() >= 1) ? (FCString::Atoi(*Args[0]) != 0)
		                                       : !Quadtree->IsFrozen();
		Quadtree->SetFrozen(bFrozen);

		// Congelare e poi allontanarsi e' il modo per VEDERE da fuori cosa il
		// culling ha scartato: la selezione resta quella di prima, ma ora la si
		// guarda dall'esterno.
		GeoLodConsole::Report(FString::Printf(
			TEXT("Vista del LOD: %s.%s"),
			bFrozen ? TEXT("CONGELATA") : TEXT("libera"),
			bFrozen ? TEXT(" Allontanati per vedere da fuori cosa e' stato scelto.") : TEXT("")));
	}));

// --- geo.Lod.Debug / geo.Lod.Draw -------------------------------------------
static FAutoConsoleCommandWithWorldAndArgs GeoLodDebugCommand(
	TEXT("geo.Lod.Debug"),
	TEXT("geo.Lod.Debug <0|1> - overlay con le statistiche di selezione."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
		[](const TArray<FString>& Args, UWorld* World)
	{
		UGeoQuadtreeSubsystem* Quadtree = GeoLodConsole::Get(World);
		if (!Quadtree) { return; }
		const bool bEnabled = (Args.Num() >= 1) ? (FCString::Atoi(*Args[0]) != 0)
		                                        : !Quadtree->IsDebugOverlayEnabled();
		Quadtree->SetDebugOverlayEnabled(bEnabled);
		if (!bEnabled && GEngine) { GEngine->ClearOnScreenDebugMessages(); }
		GeoLodConsole::Report(FString::Printf(TEXT("Overlay LOD: %s"),
			bEnabled ? TEXT("ON") : TEXT("OFF")));
	}));

static FAutoConsoleCommandWithWorldAndArgs GeoLodDrawCommand(
	TEXT("geo.Lod.Draw"),
	TEXT("geo.Lod.Draw <0|1> - disegna la tassellatura scelta, un colore per livello."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
		[](const TArray<FString>& Args, UWorld* World)
	{
		UGeoQuadtreeSubsystem* Quadtree = GeoLodConsole::Get(World);
		if (!Quadtree) { return; }
		const bool bEnabled = (Args.Num() >= 1) ? (FCString::Atoi(*Args[0]) != 0)
		                                        : !Quadtree->IsDebugDrawEnabled();
		Quadtree->SetDebugDrawEnabled(bEnabled);
		GeoLodConsole::Report(FString::Printf(TEXT("Disegno della tassellatura: %s"),
			bEnabled ? TEXT("ON") : TEXT("OFF")));
	}));

// --- geo.Lod.Stats ----------------------------------------------------------
static FAutoConsoleCommandWithWorld GeoLodStatsCommand(
	TEXT("geo.Lod.Stats"),
	TEXT("Statistiche dell'ultima selezione."),
	FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* World)
	{
		UGeoQuadtreeSubsystem* Quadtree = GeoLodConsole::Get(World);
		if (!Quadtree) { return; }

		const FGeoQuadtreeStats Stats = Quadtree->GetStats();
		GeoLodConsole::Report(FString::Printf(TEXT("Soglia / peggiore : %.1f / %.1f px"),
			Stats.SogliaErrorePx, Stats.ErrorePeggiorePx));
		GeoLodConsole::Report(FString::Printf(TEXT("Tile disegnate    : %d   livelli %d..%d"),
			Stats.TileDisegnate, Stats.LivelloMinimo, Stats.LivelloMassimo));
		GeoLodConsole::Report(FString::Printf(TEXT("Da caricare       : %d"), Stats.TileRichieste));
		GeoLodConsole::Report(FString::Printf(TEXT("Nodi visitati     : %d"), Stats.NodiVisitati));
		GeoLodConsole::Report(FString::Printf(TEXT("  scartati frustum: %d"), Stats.ScartateFrustum));
		GeoLodConsole::Report(FString::Printf(TEXT("  scartati orizzonte: %d"), Stats.ScartateOrizzonte));
		GeoLodConsole::Report(FString::Printf(TEXT("  inesistenti     : %d"), Stats.ScartateAssenti));
		GeoLodConsole::Report(FString::Printf(TEXT("Tempo selezione   : %.3f ms"), Stats.TempoSelezioneMs));
	}));

// --- geo.Lod.Demo -----------------------------------------------------------
static FAutoConsoleCommandWithWorldAndArgs GeoLodDemoCommand(
	TEXT("geo.Lod.Demo"),
	TEXT("geo.Lod.Demo <cartella> - apre un dataset, ci si posiziona sopra e accende tutto."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
		[](const TArray<FString>& Args, UWorld* World)
	{
		UGeoQuadtreeSubsystem* Quadtree = GeoLodConsole::Get(World);
		UGeoTileStreamingSubsystem* Streaming = World ? World->GetSubsystem<UGeoTileStreamingSubsystem>() : nullptr;
		UGeoreferenceSubsystem* Georeference = World ? World->GetSubsystem<UGeoreferenceSubsystem>() : nullptr;
		if (!Quadtree || !Streaming || !Georeference) { return; }

		if (Args.Num() < 1 && !Streaming->IsDatasetOpen())
		{
			GeoLodConsole::Report(TEXT("Uso: geo.Lod.Demo <cartella del dataset>"), FColor::Red);
			return;
		}

		if (Args.Num() >= 1)
		{
			FString Error;
			if (!Streaming->OpenDataset(Args[0], Error))
			{
				GeoLodConsole::Report(FString::Printf(TEXT("Errore: %s"), *Error), FColor::Red);
				return;
			}
		}

		const FGeoTileDataset& Dataset = Streaming->GetDataset();
		double West, South, East, North;
		Dataset.GetBoundingBox(West, South, East, North);

		// Quota da cui si inquadra tutto il dataset: senza, si resta lontani e
		// non si vede niente, oppure si finisce dentro il terreno.
		const double SpanDeg = FMath::Max(North - South, East - West);
		const double Altitude = FMath::Max(5000.0, SpanDeg * 111000.0);

		Georeference->TeleportViewTo(GeoWorld::Core::FGeodetic::FromDegrees(
			(South + North) * 0.5, (West + East) * 0.5, Altitude));

		Quadtree->SetEnabled(true);
		Quadtree->SetDebugOverlayEnabled(true);
		Quadtree->SetDebugDrawEnabled(true);

		GeoLodConsole::Report(FString::Printf(
			TEXT("LOD attivo su '%s'. Quota %.0f m, soglia %.1f px."),
			*Dataset.GetDatasetName(), Altitude, Quadtree->GetMaxScreenSpaceError()));
		GeoLodConsole::Report(TEXT("Scendi di quota: le tile devono suddividersi da sole."));
		GeoLodConsole::Report(TEXT("Prova geo.Lod.Error 1 (piu' dettaglio) e geo.Lod.Freeze 1."));
	}));

// ============================================================================
//  FASE 5 -- comandi del terreno
// ============================================================================

namespace GeoTerrainConsole
{
	static UGeoTerrainSubsystem* Get(UWorld* World)
	{
		return World ? World->GetSubsystem<UGeoTerrainSubsystem>() : nullptr;
	}

	static void Report(const FString& Message, const FColor& Colour = FColor::Cyan)
	{
		UE_LOG(LogGeoWorld, Log, TEXT("%s"), *Message);
		if (GEngine) { GEngine->AddOnScreenDebugMessage(-1, 10.0f, Colour, Message); }
	}

	template <typename SetterType, typename GetterType>
	static void Toggle(const TArray<FString>& Args, UWorld* World, const TCHAR* Label,
	                   SetterType Setter, GetterType Getter)
	{
		UGeoTerrainSubsystem* Terrain = Get(World);
		if (!Terrain) { return; }
		const bool bValue = (Args.Num() >= 1) ? (FCString::Atoi(*Args[0]) != 0) : !Getter(Terrain);
		Setter(Terrain, bValue);
		Report(FString::Printf(TEXT("%s: %s"), Label, bValue ? TEXT("ON") : TEXT("OFF")));
	}
}

static FAutoConsoleCommandWithWorldAndArgs GeoTerrainEnableCommand(
	TEXT("geo.Terrain.Enable"),
	TEXT("geo.Terrain.Enable <0|1> - costruisce la geometria delle tile selezionate."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
		[](const TArray<FString>& Args, UWorld* World)
	{
		GeoTerrainConsole::Toggle(Args, World, TEXT("Terreno"),
			[](UGeoTerrainSubsystem* T, bool b) { T->SetEnabled(b); },
			[](UGeoTerrainSubsystem* T) { return T->IsEnabled(); });
	}));

static FAutoConsoleCommandWithWorldAndArgs GeoTerrainWireframeCommand(
	TEXT("geo.Terrain.Wireframe"),
	TEXT("geo.Terrain.Wireframe <0|1> - mostra il reticolo dei triangoli."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
		[](const TArray<FString>& Args, UWorld* World)
	{
		GeoTerrainConsole::Toggle(Args, World, TEXT("Wireframe"),
			[](UGeoTerrainSubsystem* T, bool b) { T->SetWireframe(b); },
			[](UGeoTerrainSubsystem* T) { return T->IsWireframe(); });
	}));

static FAutoConsoleCommandWithWorldAndArgs GeoTerrainSkirtCommand(
	TEXT("geo.Terrain.Skirt"),
	TEXT("geo.Terrain.Skirt <0|1> - gonne ai bordi. Spegnile per VEDERE le crepe."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
		[](const TArray<FString>& Args, UWorld* World)
	{
		GeoTerrainConsole::Toggle(Args, World, TEXT("Gonne"),
			[](UGeoTerrainSubsystem* T, bool b) { T->SetSkirtEnabled(b); },
			[](UGeoTerrainSubsystem* T) { return T->IsSkirtEnabled(); });
		GeoTerrainConsole::Report(
			TEXT("  (spente, le crepe fra livelli diversi diventano visibili: e' la prova"), FColor::White);
		GeoTerrainConsole::Report(
			TEXT("   che le gonne servono, e che servono SOLO fra livelli diversi)"), FColor::White);
	}));

static FAutoConsoleCommandWithWorldAndArgs GeoTerrainFlipCommand(
	TEXT("geo.Terrain.FlipWinding"),
	TEXT("geo.Terrain.FlipWinding <0|1> - inverte l'orientamento dei triangoli."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
		[](const TArray<FString>& Args, UWorld* World)
	{
		GeoTerrainConsole::Toggle(Args, World, TEXT("Orientamento invertito"),
			[](UGeoTerrainSubsystem* T, bool b) { T->SetFlipWinding(b); },
			[](UGeoTerrainSubsystem* T) { return T->IsFlipWinding(); });
		GeoTerrainConsole::Report(
			TEXT("  (usalo se il terreno e' invisibile dall'alto e visibile da sotto)"), FColor::White);
	}));

static FAutoConsoleCommandWithWorldAndArgs GeoTerrainBudgetCommand(
	TEXT("geo.Terrain.Budget"),
	TEXT("geo.Terrain.Budget <N> - tile di cui costruire la geometria per frame."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
		[](const TArray<FString>& Args, UWorld* World)
	{
		UGeoTerrainSubsystem* Terrain = GeoTerrainConsole::Get(World);
		if (!Terrain) { return; }
		if (Args.Num() >= 1) { Terrain->SetMaxTilesPerFrame(FCString::Atoi(*Args[0])); }
		GeoTerrainConsole::Report(FString::Printf(
			TEXT("Budget: %d tile per frame (alzandolo il terreno si riempie prima, ma puo' scattare)"),
			Terrain->GetMaxTilesPerFrame()));
	}));

static FAutoConsoleCommandWithWorld GeoTerrainStatsCommand(
	TEXT("geo.Terrain.Stats"),
	TEXT("Statistiche della geometria."),
	FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* World)
	{
		UGeoTerrainSubsystem* Terrain = GeoTerrainConsole::Get(World);
		if (!Terrain) { return; }
		const FGeoTerrainStats Stats = Terrain->GetStats();
		GeoTerrainConsole::Report(FString::Printf(TEXT("Provider     : %s"), *Terrain->GetProviderName()));
		GeoTerrainConsole::Report(FString::Printf(TEXT("Tile con mesh: %d   triangoli %d"),
			Stats.TileConGeometria, Stats.TriangoliTotali));
		GeoTerrainConsole::Report(FString::Printf(TEXT("In attesa    : %d"), Stats.TileInAttesa));
		GeoTerrainConsole::Report(FString::Printf(TEXT("Costruzione  : %.2f ms per tile"),
			Stats.TempoCostruzioneMediaMs));
		GeoTerrainConsole::Report(FString::Printf(TEXT("Rebase gestiti: %d  (nessun vertice rigenerato)"),
			Stats.Rebase));
	}));

// --- geo.Terrain.Demo -------------------------------------------------------
static FAutoConsoleCommandWithWorldAndArgs GeoTerrainDemoCommand(
	TEXT("geo.Terrain.Demo"),
	TEXT("geo.Terrain.Demo <cartella> - apre un dataset e disegna il terreno."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
		[](const TArray<FString>& Args, UWorld* World)
	{
		UGeoTerrainSubsystem* Terrain = GeoTerrainConsole::Get(World);
		UGeoTileStreamingSubsystem* Streaming = World ? World->GetSubsystem<UGeoTileStreamingSubsystem>() : nullptr;
		UGeoreferenceSubsystem* Georeference = World ? World->GetSubsystem<UGeoreferenceSubsystem>() : nullptr;
		UGeoQuadtreeSubsystem* Quadtree = World ? World->GetSubsystem<UGeoQuadtreeSubsystem>() : nullptr;
		if (!Terrain || !Streaming || !Georeference || !Quadtree) { return; }

		if (Args.Num() >= 1)
		{
			FString Error;
			if (!Streaming->OpenDataset(Args[0], Error))
			{
				GeoTerrainConsole::Report(FString::Printf(TEXT("Errore: %s"), *Error), FColor::Red);
				return;
			}
		}
		else if (!Streaming->IsDatasetOpen())
		{
			GeoTerrainConsole::Report(TEXT("Uso: geo.Terrain.Demo <cartella del dataset>"), FColor::Red);
			return;
		}

		const FGeoTileDataset& Dataset = Streaming->GetDataset();
		double West, South, East, North;
		Dataset.GetBoundingBox(West, South, East, North);

		// Una quota da cui si vede un pezzo di terreno con del rilievo, non
		// l'intero dataset schiacciato: a 15 km si distinguono le valli.
		Georeference->TeleportViewTo(GeoWorld::Core::FGeodetic::FromDegrees(
			(South + North) * 0.5, (West + East) * 0.5, 15000.0));

		Quadtree->SetEnabled(true);
		Terrain->SetEnabled(true);
		Terrain->SetDebugOverlayEnabled(true);

		GeoTerrainConsole::Report(FString::Printf(
			TEXT("Terreno attivo su '%s'. Quota 15 km sul centro del dataset."),
			*Dataset.GetDatasetName()));
		GeoTerrainConsole::Report(TEXT("Scendi di quota: la geometria si raffina da sola."));
		GeoTerrainConsole::Report(TEXT("Se non vedi niente dall'alto: geo.Terrain.FlipWinding"));
		GeoTerrainConsole::Report(TEXT("Per capire cosa fanno le gonne: geo.Terrain.Skirt 0"));
	}));

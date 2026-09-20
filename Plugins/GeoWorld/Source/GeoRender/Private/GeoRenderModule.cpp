// =============================================================================
//  Modulo GeoRender. In Fase 1 contiene solo gli strumenti di verifica visiva;
//  dalla Fase 4 ospitera' il quadtree, la selezione LOD e la generazione mesh.
// =============================================================================
#include "Modules/ModuleManager.h"

#include "GeoFlyPawn.h"
#include "GeoMarkerActor.h"
#include "Georeference/GeoTransformComponent.h"
#include "Imagery/GeoImagerySubsystem.h"
#include "Lod/GeoQuadtreeSubsystem.h"
#include "Terrain/GeoTerrainSubsystem.h"
#include "Streaming/GeoImageryStreamingSubsystem.h"
#include "Streaming/GeoTileStreamingSubsystem.h"
#include "Georeference/GeoreferenceSubsystem.h"
#include "Georeference/GeoPlaces.h"
#include "Georeference/GeoWorldTypes.h"
#include "GeoCoreModule.h"

#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "Geo/GeoUnits.h"

#include <cmath>

IMPLEMENT_MODULE(FDefaultModuleImpl, GeoRender)

namespace GeoMarkers
{
	// I luoghi vivono in GeoCore/Public/Georeference/GeoPlaces.h: la stessa
	// tabella che usa geo.Goto. Averne due copie significa vederle divergere.
	// Qui si sceglie solo QUALI marcare: i cubi servono a controllare che la
	// geodesia metta le cose al posto giusto, e cinque punti sparsi bastano.
	static const TCHAR* MarkedPlaces[] =
	{
		TEXT("Colosseo"), TEXT("Duomo"), TEXT("Pisa"),
		TEXT("MonteBianco"), TEXT("Etna"),
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

		for (const TCHAR* PlaceName : GeoMarkers::MarkedPlaces)
		{
			// NOTA UE: SpawnActor richiede i parametri di spawn se vogliamo
			// impostare proprieta' PRIMA che l'attore sia completamente
			// inizializzato. Qui ci basta il caso semplice: spawn e poi
			// configurazione, perche' UGeoTransformComponent si riposiziona da
			// solo appena gli si cambia la coordinata.
			const GeoWorld::Places::FNamedPlace* Place = GeoWorld::Places::Find(PlaceName);
			if (!Place) { continue; }

			AGeoMarkerActor* Marker = World->SpawnActor<AGeoMarkerActor>();
			if (!Marker)
			{
				continue;
			}

			Marker->Label = Place->Name;

#if WITH_EDITOR
			// NOTA UE: SetActorLabel (il nome leggibile nel World Outliner)
			// esiste SOLO nelle build con editor. Senza la guardia il progetto
			// compila nell'editor e poi si rompe al packaging.
			Marker->SetActorLabel(Place->Name);
#endif
			// Quota SUL SUOLO zero: il cubo sta sul terreno, non sospeso. La
			// conversione da ortometrica a ellissoidica la fa ToGeodetic.
			Marker->GetGeoTransform()->SetGeoCoordinate(
				FGeoCoordinate::FromGeodetic(GeoWorld::Places::ToGeodetic(*Place, 0.0)));

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
		const bool bWas = Getter(Terrain);
		const bool bValue = (Args.Num() >= 1) ? (FCString::Atoi(*Args[0]) != 0) : !bWas;
		Setter(Terrain, bValue);

		// PERCHE' QUESTO RAMO ESISTE: "geo.Terrain.FlipWinding 1" su un valore
		// gia' a 1 non fa NIENTE, perche' il setter esce subito se il valore non
		// cambia -- e la geometria non viene ricostruita. La prima versione
		// rispondeva comunque "Orientamento invertito: ON", cioe' dava per fatto
		// qualcosa che non era successo, ed e' costato un giro intero di
		// diagnosi. Un comando che non ha cambiato niente deve dirlo.
		if (bValue == bWas)
		{
			Report(FString::Printf(
				TEXT("%s: era GIA' %s, nessun cambiamento (per invertirlo: %s)"),
				Label, bValue ? TEXT("ON") : TEXT("OFF"), bValue ? TEXT("0") : TEXT("1")),
				FColor::Yellow);
			return;
		}
		Report(FString::Printf(TEXT("%s: %s (era %s)"), Label,
			bValue ? TEXT("ON") : TEXT("OFF"), bWas ? TEXT("ON") : TEXT("OFF")));
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

static FAutoConsoleCommandWithWorldAndArgs GeoTerrainBoxesCommand(
	TEXT("geo.Terrain.Boxes"),
	TEXT("geo.Terrain.Boxes <0|1> - scatole di debug sui bounds delle tile."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
		[](const TArray<FString>& Args, UWorld* World)
	{
		GeoTerrainConsole::Toggle(Args, World, TEXT("Scatole sui bounds"),
			[](UGeoTerrainSubsystem* T, bool b) { T->SetDrawBounds(b); },
			[](UGeoTerrainSubsystem* T) { return T->IsDrawBounds(); });
		GeoTerrainConsole::Report(
			TEXT("  (linee di debug: non passano per il materiale e non vengono nebbiate."), FColor::White);
		GeoTerrainConsole::Report(
			TEXT("   Se vedi le scatole ma non il terreno, la geometria e' al posto giusto)"), FColor::White);
	}));

// =============================================================================
//  ORTOFOTO (Fase 6)
// =============================================================================
namespace GeoImageryConsole
{
	static UGeoImagerySubsystem* Get(UWorld* World)
	{
		return World ? World->GetSubsystem<UGeoImagerySubsystem>() : nullptr;
	}

	static UGeoImageryStreamingSubsystem* GetStreaming(UWorld* World)
	{
		return World ? World->GetSubsystem<UGeoImageryStreamingSubsystem>() : nullptr;
	}

	template <typename SetterType, typename GetterType>
	static void Toggle(const TArray<FString>& Args, UWorld* World, const TCHAR* Label,
	                   SetterType Setter, GetterType Getter)
	{
		UGeoImagerySubsystem* Imagery = Get(World);
		if (!Imagery) { return; }

		const bool bWas = Getter(Imagery);
		const bool bValue = (Args.Num() >= 1) ? (FCString::Atoi(*Args[0]) != 0) : !bWas;
		Setter(Imagery, bValue);

		if (bValue == bWas)
		{
			GeoTerrainConsole::Report(FString::Printf(
				TEXT("%s: era GIA' %s, nessun cambiamento (per invertirlo: %s)"),
				Label, bValue ? TEXT("ON") : TEXT("OFF"), bValue ? TEXT("0") : TEXT("1")),
				FColor::Yellow);
			return;
		}
		GeoTerrainConsole::Report(FString::Printf(TEXT("%s: %s (era %s)"), Label,
			bValue ? TEXT("ON") : TEXT("OFF"), bWas ? TEXT("ON") : TEXT("OFF")));
	}
}

static FAutoConsoleCommandWithWorldAndArgs GeoImageryOpenCommand(
	TEXT("geo.Imagery.Open"),
	TEXT("geo.Imagery.Open <cartella> - apre un dataset di ortofoto."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
		[](const TArray<FString>& Args, UWorld* World)
	{
		UGeoImageryStreamingSubsystem* Streaming = GeoImageryConsole::GetStreaming(World);
		if (!Streaming) { return; }

		if (Args.Num() < 1)
		{
			GeoTerrainConsole::Report(TEXT("Uso: geo.Imagery.Open <cartella del dataset>"), FColor::Red);
			return;
		}

		FString Error;
		if (!Streaming->OpenDataset(Args[0], Error))
		{
			GeoTerrainConsole::Report(FString::Printf(TEXT("Errore: %s"), *Error), FColor::Red);
			return;
		}

		const FGeoImageryDataset& Dataset = Streaming->GetDataset();
		double West, South, East, North;
		Dataset.GetBoundingBox(West, South, East, North);

		GeoTerrainConsole::Report(FString::Printf(
			TEXT("Ortofoto '%s': livelli %d..%d, %lld tile indicizzate"),
			*Dataset.GetDatasetName(), Dataset.GetMinLevel(), Dataset.GetMaxLevel(),
			static_cast<long long>(Dataset.GetIndexedTileCount())));
		GeoTerrainConsole::Report(FString::Printf(
			TEXT("Area: ovest %.4f  sud %.4f  est %.4f  nord %.4f"), West, South, East, North));
	}));

static FAutoConsoleCommandWithWorldAndArgs GeoImageryEnableCommand(
	TEXT("geo.Imagery.Enable"),
	TEXT("geo.Imagery.Enable <0|1> - veste il terreno con le ortofoto."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
		[](const TArray<FString>& Args, UWorld* World)
	{
		GeoImageryConsole::Toggle(Args, World, TEXT("Drappeggio"),
			[](UGeoImagerySubsystem* I, bool b) { I->SetEnabled(b); },
			[](UGeoImagerySubsystem* I) { return I->IsEnabled(); });
	}));

static FAutoConsoleCommandWithWorldAndArgs GeoImageryCheckerCommand(
	TEXT("geo.Imagery.Checker"),
	TEXT("geo.Imagery.Checker <0|1> - scacchiera al posto delle foto: mostra le UV."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
		[](const TArray<FString>& Args, UWorld* World)
	{
		GeoImageryConsole::Toggle(Args, World, TEXT("Scacchiera"),
			[](UGeoImagerySubsystem* I, bool b) { I->SetCheckerboard(b); },
			[](UGeoImagerySubsystem* I) { return I->IsCheckerboard(); });
		GeoTerrainConsole::Report(
			TEXT("  (il bordo rosso di ogni tile deve combaciare con quello della vicina:"), FColor::White);
		GeoTerrainConsole::Report(
			TEXT("   se si vede doppio o sfalsato, il problema e' nelle UV)"), FColor::White);
	}));

static FAutoConsoleCommandWithWorldAndArgs GeoImageryDebugCommand(
	TEXT("geo.Imagery.Debug"),
	TEXT("geo.Imagery.Debug <0|1> - overlay con le statistiche del drappeggio."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
		[](const TArray<FString>& Args, UWorld* World)
	{
		GeoImageryConsole::Toggle(Args, World, TEXT("Overlay ortofoto"),
			[](UGeoImagerySubsystem* I, bool b) { I->SetDebugOverlayEnabled(b); },
			[](UGeoImagerySubsystem* I) { return I->IsDebugOverlayEnabled(); });
	}));

static FAutoConsoleCommandWithWorldAndArgs GeoImageryBudgetCommand(
	TEXT("geo.Imagery.Budget"),
	TEXT("geo.Imagery.Budget <N> - texture create per frame (default 4)."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
		[](const TArray<FString>& Args, UWorld* World)
	{
		UGeoImagerySubsystem* Imagery = GeoImageryConsole::Get(World);
		if (!Imagery) { return; }
		if (Args.Num() >= 1) { Imagery->SetMaxTexturesPerFrame(FCString::Atoi(*Args[0])); }
		GeoTerrainConsole::Report(FString::Printf(
			TEXT("Budget: %d texture per frame"), Imagery->GetMaxTexturesPerFrame()));
	}));

static FAutoConsoleCommandWithWorld GeoImageryStatsCommand(
	TEXT("geo.Imagery.Stats"),
	TEXT("Statistiche del drappeggio e dello streaming delle ortofoto."),
	FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* World)
	{
		UGeoImagerySubsystem* Imagery = GeoImageryConsole::Get(World);
		UGeoImageryStreamingSubsystem* Streaming = GeoImageryConsole::GetStreaming(World);
		if (!Imagery || !Streaming) { return; }

		const FGeoImageryStats Stats = Imagery->GetStats();
		const FGeoImageryStreamingStats StreamStats = Streaming->GetStats();

		GeoTerrainConsole::Report(TEXT("--- Ortofoto ---"));
		GeoTerrainConsole::Report(FString::Printf(
			TEXT("Tile vestite : %d   senza immagine %d   con immagine grossolana %d"),
			Stats.TileVestite, Stats.TileSenzaImmagine, Stats.TileConImmagineGrossolana));
		GeoTerrainConsole::Report(FString::Printf(
			TEXT("Livelli usati: %d..%d"), Stats.LivelloImmagineMin, Stats.LivelloImmagineMax));
		GeoTerrainConsole::Report(FString::Printf(
			TEXT("Texture      : %d in memoria video (%.1f MB)"),
			Stats.TextureInMemoria, Stats.MemoriaTextureMB));
		GeoTerrainConsole::Report(FString::Printf(
			TEXT("Cache        : %d tile, %.1f/%.0f MB, hit rate %.0f%%"),
			StreamStats.TileResidenti, StreamStats.MemoriaMB, StreamStats.BudgetMB,
			StreamStats.HitRate * 100.0f));   // non-unita: frazione -> percentuale
		GeoTerrainConsole::Report(FString::Printf(
			TEXT("Per tile     : lettura %.2f ms, decodifica %.2f ms   (errori: %d)"),
			StreamStats.TempoMedioCaricamentoMs, StreamStats.TempoMedioDecodificaMs,
			StreamStats.ErroriDiCaricamento));
	}));

// --- geo.Imagery.Demo -------------------------------------------------------
static FAutoConsoleCommandWithWorldAndArgs GeoImageryDemoCommand(
	TEXT("geo.Imagery.Demo"),
	TEXT("geo.Imagery.Demo <cartella_terreno> <cartella_ortofoto> - apre tutto e accende."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
		[](const TArray<FString>& Args, UWorld* World)
	{
		UGeoTerrainSubsystem* Terrain = GeoTerrainConsole::Get(World);
		UGeoImagerySubsystem* Imagery = GeoImageryConsole::Get(World);
		UGeoImageryStreamingSubsystem* ImageStreaming = GeoImageryConsole::GetStreaming(World);
		UGeoTileStreamingSubsystem* Heights = World ? World->GetSubsystem<UGeoTileStreamingSubsystem>() : nullptr;
		UGeoQuadtreeSubsystem* Quadtree = World ? World->GetSubsystem<UGeoQuadtreeSubsystem>() : nullptr;
		UGeoreferenceSubsystem* Georeference = World ? World->GetSubsystem<UGeoreferenceSubsystem>() : nullptr;
		if (!Terrain || !Imagery || !ImageStreaming || !Heights || !Quadtree || !Georeference) { return; }

		if (Args.Num() < 2)
		{
			GeoTerrainConsole::Report(
				TEXT("Uso: geo.Imagery.Demo <cartella del terreno> <cartella delle ortofoto>"), FColor::Red);
			return;
		}

		FString Error;
		if (!Heights->OpenDataset(Args[0], Error))
		{
			GeoTerrainConsole::Report(FString::Printf(TEXT("Terreno: %s"), *Error), FColor::Red);
			return;
		}
		if (!ImageStreaming->OpenDataset(Args[1], Error))
		{
			GeoTerrainConsole::Report(FString::Printf(TEXT("Ortofoto: %s"), *Error), FColor::Red);
			return;
		}

		const FGeoTileDataset& TerrainDataset = Heights->GetDataset();
		double West, South, East, North;
		TerrainDataset.GetBoundingBox(West, South, East, North);

		Georeference->TeleportViewTo(GeoWorld::Core::FGeodetic::FromDegrees(
			(South + North) * 0.5, (West + East) * 0.5, 6000.0));

		Quadtree->SetEnabled(true);
		Terrain->SetEnabled(true);
		// Qui il wireframe si SPEGNE, al contrario di geo.Terrain.Demo: con
		// un'ortofoto addosso il terreno non si confonde piu' con il cielo, e il
		// reticolo coprirebbe proprio quello che si e' venuti a vedere.
		Terrain->SetWireframe(false);
		Imagery->SetEnabled(true);
		Imagery->SetDebugOverlayEnabled(true);

		GeoTerrainConsole::Report(FString::Printf(
			TEXT("Terreno '%s' vestito con '%s'. Quota 6 km."),
			*TerrainDataset.GetDatasetName(), *ImageStreaming->GetDataset().GetDatasetName()));
		GeoTerrainConsole::Report(TEXT("Per vedere le UV: geo.Imagery.Checker 1"));
		GeoTerrainConsole::Report(TEXT("Se resta grigio: serve il materiale, geo.Imagery.CreateMaterial"));
	}));

static FAutoConsoleCommandWithWorldAndArgs GeoLodMarginCommand(
	TEXT("geo.Lod.Margin"),
	TEXT("geo.Lod.Margin <fattore> - allarga il frustum per caricare PRIMA le tile ai bordi."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
		[](const TArray<FString>& Args, UWorld* World)
	{
		UGeoQuadtreeSubsystem* Quadtree = World ? World->GetSubsystem<UGeoQuadtreeSubsystem>() : nullptr;
		if (!Quadtree) { return; }

		if (Args.Num() >= 1) { Quadtree->SetFrustumMargin(FCString::Atod(*Args[0])); }

		GeoTerrainConsole::Report(FString::Printf(
			TEXT("Margine del frustum: %.2f  (1.0 = esatto, nessun precaricamento)"),
			Quadtree->GetFrustumMargin()));
		GeoTerrainConsole::Report(
			TEXT("  Con 1.0 le tile vengono chieste quando sono GIA' visibili, e"), FColor::White);
		GeoTerrainConsole::Report(
			TEXT("  ruotando la camera il bordo resta vuoto finche' non arrivano."), FColor::White);
	}));

// --- geo.Terrain.Diag -------------------------------------------------------
//
//  "Disegna 108 tile e 3,5 milioni di triangoli, ma non vedo niente" e' un
//  sintomo che i contatori dell'overlay non sanno spiegare, perche' contavano
//  quello che IO avevo costruito e non quello che il RENDERER aveva ricevuto.
//  Questo comando stampa i numeri dell'altro lato: la mesh vera dentro il
//  componente, i suoi bounds, dove sta rispetto alla camera.
//
//  Come si legge il risultato:
//    triangoli 0            -> la mesh non e' arrivata al componente
//    raggio 0               -> bounds degeneri: il renderer scarta la primitiva
//    distanza enorme        -> la geometria e' altrove, problema di trasformazione
//    tutto sensato          -> la geometria c'e' ed e' al posto giusto: allora
//                              e' orientamento delle facce o materiale
static FAutoConsoleCommandWithWorld GeoTerrainDiagCommand(
	TEXT("geo.Terrain.Diag"),
	TEXT("Diagnosi: cosa il renderer ha davvero, e dove sta rispetto alla camera."),
	FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* World)
	{
		UGeoTerrainSubsystem* Terrain = GeoTerrainConsole::Get(World);
		UGeoreferenceSubsystem* Georeference =
			World ? World->GetSubsystem<UGeoreferenceSubsystem>() : nullptr;
		if (!Terrain || !Georeference) { return; }

		const FGeoTerrainStats Stats = Terrain->GetStats();
		GeoTerrainConsole::Report(TEXT("--- geo.Terrain.Diag ---"));
		GeoTerrainConsole::Report(FString::Printf(
			TEXT("Terreno %s   provider %s   tile %d"),
			Terrain->IsEnabled() ? TEXT("ON") : TEXT("OFF"),
			*Terrain->GetProviderName(), Stats.TileConGeometria));

		GeoTerrainConsole::Report(FString::Printf(
			TEXT("Triangoli: costruiti %d, nel renderer %d"),
			Stats.TriangoliCostruiti, Stats.TriangoliTotali),
			Stats.TriangoliCostruiti == Stats.TriangoliTotali ? FColor::Green : FColor::Red);

		const FGeoreferenceSnapshot Snapshot = Georeference->GetSnapshot();

		UGeoreferenceSubsystem::FActiveViewInfo View;
		if (!Georeference->GetActiveViewInfo(View))
		{
			GeoTerrainConsole::Report(TEXT("Nessuna camera attiva: non posso dire dove guardi."), FColor::Red);
			return;
		}

		const GeoWorld::Core::FGeodetic CameraGeodetic = Snapshot.UnrealToGeodetic(View.Location);
		GeoTerrainConsole::Report(FString::Printf(
			TEXT("Camera: uu (%.0f, %.0f, %.0f)  =  lat %.4f  lon %.4f  quota %.0f m"),
			View.Location.X, View.Location.Y, View.Location.Z,
			CameraGeodetic.LatDeg(), CameraGeodetic.LonDeg(), CameraGeodetic.HeightM));

		const FVector Forward = View.Rotation.Vector();
		GeoTerrainConsole::Report(FString::Printf(
			TEXT("Sguardo: pitch %.1f  yaw %.1f   (pitch negativo = verso il basso)"),
			View.Rotation.Pitch, View.Rotation.Yaw));

		TArray<FGeoTerrainTileDiagnostic> Diagnostics;
		Terrain->GetTileDiagnostics(Diagnostics, 3);
		if (Diagnostics.Num() == 0)
		{
			GeoTerrainConsole::Report(TEXT("Nessun componente da ispezionare."), FColor::Red);
			return;
		}

		for (const FGeoTerrainTileDiagnostic& Tile : Diagnostics)
		{
			const FVector ToTile = Tile.WorldLocation - View.Location;
			const double DistanceKm = ToTile.Size() / GeoWorld::Units::MetersToUu / 1000.0;

			// Un valore vicino a +1 significa "davanti alla camera", vicino a -1
			// "dietro". Se le tile selezionate risultassero dietro, il problema
			// sarebbe nel frustum della selezione, non nella mesh.
			const double Ahead = ToTile.IsNearlyZero()
				? 1.0 : FVector::DotProduct(ToTile.GetSafeNormal(), Forward);

			GeoTerrainConsole::Report(FString::Printf(
				TEXT("L%u (%u,%u): vertici %d  triangoli %d  raggio %.1f km"),
				Tile.Key.Level, Tile.Key.X, Tile.Key.Y,
				Tile.RealVertexCount, Tile.RealTriangleCount,
				Tile.BoundsRadiusUu / GeoWorld::Units::MetersToUu / 1000.0),
				Tile.RealTriangleCount > 0 ? FColor::Green : FColor::Red);

			GeoTerrainConsole::Report(FString::Printf(
				TEXT("      distanza %.1f km  davanti %.2f  %s  %s  materiale %s"),
				DistanceKm, Ahead,
				Tile.bRegistered ? TEXT("registrato") : TEXT("NON REGISTRATO"),
				Tile.bVisible ? TEXT("visibile") : TEXT("NASCOSTO"),
				*Tile.MaterialName),
				(Tile.bRegistered && Tile.bVisible) ? FColor::White : FColor::Red);
		}

		// Se i numeri sono sani, il problema non e' piu' "dove sta la geometria"
		// ma "perche' non la vedo", e sono domande diverse con strumenti diversi.
		GeoTerrainConsole::Report(TEXT("Numeri sani? Allora non e' la geometria. In quest'ordine:"), FColor::Yellow);
		GeoTerrainConsole::Report(TEXT("  viewmode wireframe   ignora materiali, luci e facce: se appare, e' ombreggiatura"), FColor::Yellow);
		GeoTerrainConsole::Report(TEXT("  geo.Terrain.Boxes 1  linee di debug, non nebbiate: se appaiono, la posizione e' giusta"), FColor::Yellow);
		GeoTerrainConsole::Report(TEXT("  r.Fog 0              a 20 km la nebbia di default sostituisce il terreno col cielo"), FColor::Yellow);
		GeoTerrainConsole::Report(TEXT("  r.SkyAtmosphere 0    come sopra, prospettiva aerea"), FColor::Yellow);
		GeoTerrainConsole::Report(TEXT("  geo.Terrain.FlipWinding 0/1  orientamento delle facce"), FColor::Yellow);
	}));

// =============================================================================
//  VOLO -- geo.Fly, geo.Fly.Speed
// =============================================================================
//
//  QUALE CAMERA STAI MUOVENDO. Nell'editor fuori dal Play la "camera" non e' un
//  attore: e' lo stato del viewport client, e si guida con i suoi comandi (tasto
//  destro + WASD, rotella per la velocita', o geo.ViewSpeed). Dentro il Play la
//  camera E' un attore, e allora si puo' sostituire con qualcosa di adatto alla
//  scala planetaria: e' quello che fa geo.Fly.
static FAutoConsoleCommandWithWorld GeoFlyCommand(
	TEXT("geo.Fly"),
	TEXT("Sostituisce la camera del Play con una che vola a velocita' proporzionale alla quota."),
	FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* World)
	{
		if (!World) { return; }

		APlayerController* PlayerController = World->GetFirstPlayerController();
		if (!PlayerController)
		{
			GeoTerrainConsole::Report(
				TEXT("Nessun PlayerController: sei nel viewport dell'editor, non nel Play."), FColor::Yellow);
			GeoTerrainConsole::Report(
				TEXT("Li' la camera e' del viewport: usa tasto destro + WASD, e geo.ViewSpeed per la velocita'."));
			return;
		}

		// Si parte esattamente da dove si sta guardando: un teletrasporto
		// involontario a ogni geo.Fly sarebbe disorientante.
		FVector Location;
		FRotator Rotation;
		PlayerController->GetPlayerViewPoint(Location, Rotation);

		FActorSpawnParameters Parameters;
		Parameters.ObjectFlags |= RF_Transient;
		Parameters.SpawnCollisionHandlingOverride =
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

		AGeoFlyPawn* Pawn = World->SpawnActor<AGeoFlyPawn>(
			AGeoFlyPawn::StaticClass(), Location, Rotation, Parameters);
		if (!Pawn)
		{
			GeoTerrainConsole::Report(TEXT("Non sono riuscito a creare la camera di volo."), FColor::Red);
			return;
		}

		// NOTA UE: Possess cambia il pawn controllato E il view target, quindi
		// non serve chiamare SetViewTarget separatamente. Il pawn precedente
		// resta nel livello: non lo distruggo perche' potrebbe essere il pawn
		// del gioco, e una camera di debug non ha il diritto di cancellarlo.
		PlayerController->Possess(Pawn);

		GeoTerrainConsole::Report(TEXT("Camera di volo attiva. WASD per muoverti, Q/E per salire e scendere."));
		GeoTerrainConsole::Report(TEXT("La velocita' segue la quota: mezza quota al secondo."));
		GeoTerrainConsole::Report(TEXT("Troppo veloce o troppo lenta: geo.Fly.Speed 0.3 / geo.Fly.Speed 3"));
	}));

static FAutoConsoleCommandWithWorldAndArgs GeoFlySpeedCommand(
	TEXT("geo.Fly.Speed"),
	TEXT("geo.Fly.Speed <moltiplicatore> - scala la velocita' calcolata dalla quota."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
		[](const TArray<FString>& Args, UWorld* World)
	{
		APlayerController* PlayerController = World ? World->GetFirstPlayerController() : nullptr;
		AGeoFlyPawn* Pawn = PlayerController ? Cast<AGeoFlyPawn>(PlayerController->GetPawn()) : nullptr;
		if (!Pawn)
		{
			GeoTerrainConsole::Report(TEXT("Nessuna camera di volo attiva: lancia prima geo.Fly."), FColor::Yellow);
			return;
		}

		if (Args.Num() >= 1) { Pawn->SetSpeedMultiplier(FCString::Atof(*Args[0])); }

		GeoTerrainConsole::Report(FString::Printf(
			TEXT("Moltiplicatore x%.2f  ->  %.0f km/h alla quota attuale di %.0f m"),
			Pawn->GetSpeedMultiplier(), Pawn->GetCurrentSpeedKmh(), Pawn->GetCurrentHeightM()));
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

		// QUOTA. La prima versione partiva da 15 km "per vedere le valli". E'
		// una quota sbagliata per un primo avvio: con la nebbia atmosferica di
		// default di UE (ExponentialHeightFog piu' SkyAtmosphere) il terreno a
		// 20 km di distanza e' quasi interamente sostituito dal colore del
		// cielo, e si conclude che non viene disegnato. 6 km e' sopra le Alpi e
		// abbastanza vicino da vedere la geometria anche con la nebbia accesa.
		Georeference->TeleportViewTo(GeoWorld::Core::FGeodetic::FromDegrees(
			(South + North) * 0.5, (West + East) * 0.5, 6000.0));

		Quadtree->SetEnabled(true);
		Terrain->SetEnabled(true);
		Terrain->SetDebugOverlayEnabled(true);

		// WIREFRAME ACCESO AL PRIMO AVVIO, di proposito.
		// Il materiale di base e' una superficie grigia senza texture: illuminata
		// solo dalla luce del cielo riempie lo schermo di un azzurrino uniforme,
		// indistinguibile dal cielo vuoto. Il reticolo dei triangoli invece non
		// si confonde con niente. Si spegne con geo.Terrain.Wireframe 0.
		Terrain->SetWireframe(true);

		GeoTerrainConsole::Report(FString::Printf(
			TEXT("Terreno attivo su '%s'. Quota 15 km sul centro del dataset."),
			*Dataset.GetDatasetName()));
		GeoTerrainConsole::Report(TEXT("Wireframe acceso: il terreno grigio senza texture si confonde col cielo."));
		GeoTerrainConsole::Report(TEXT("Spegnilo quando lo vedi: geo.Terrain.Wireframe 0"));
		GeoTerrainConsole::Report(TEXT("Scendi di quota: la geometria si raffina da sola."));
		GeoTerrainConsole::Report(TEXT("Per capire cosa fanno le gonne: geo.Terrain.Skirt 0"));
		GeoTerrainConsole::Report(TEXT("Se non vedi niente, in quest'ordine:"), FColor::Yellow);
		GeoTerrainConsole::Report(TEXT("  viewmode wireframe   (del motore: ignora materiali e facce)"), FColor::Yellow);
		GeoTerrainConsole::Report(TEXT("  geo.Terrain.Boxes 1  (scatole di debug sui bounds)"), FColor::Yellow);
		GeoTerrainConsole::Report(TEXT("  r.Fog 0              (la nebbia cancella il terreno lontano)"), FColor::Yellow);
		GeoTerrainConsole::Report(TEXT("  geo.Terrain.Diag     (i numeri del renderer)"), FColor::Yellow);
	}));

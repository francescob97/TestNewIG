// =============================================================================
//  Modulo GeoRender. In Fase 1 contiene solo gli strumenti di verifica visiva;
//  dalla Fase 4 ospitera' il quadtree, la selezione LOD e la generazione mesh.
// =============================================================================
#include "Modules/ModuleManager.h"

#include "GeoMarkerActor.h"
#include "Unreal/GeoTransformComponent.h"
#include "Unreal/GeoreferenceSubsystem.h"
#include "Unreal/GeoWorldTypes.h"
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

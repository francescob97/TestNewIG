#include "GeoCoreModule.h"

#include "Georeference/GeoreferenceSubsystem.h"
#include "Georeference/GeoPlaces.h"
#include "Georeference/GeoWorldTypes.h"

#include "Engine/Engine.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"

#if WITH_EDITOR
	// Vedi la nota in GeoCore.Build.cs: la dipendenza da UnrealEd e' ammessa
	// solo quando si costruisce un target che contiene l'editor.
	#include "Editor.h"
	#include "EditorViewportClient.h"
#endif

DEFINE_LOG_CATEGORY(LogGeoWorld);

#define LOCTEXT_NAMESPACE "FGeoCoreModule"

void FGeoCoreModule::StartupModule()
{
	UE_LOG(LogGeoWorld, Log, TEXT("GeoCore avviato. Console: geo.Help"));
}

void FGeoCoreModule::ShutdownModule()
{
}

#undef LOCTEXT_NAMESPACE

// IMPLEMENT_MODULE collega la classe al nome del modulo. Deve comparire in
// esattamente un .cpp per modulo.
IMPLEMENT_MODULE(FGeoCoreModule, GeoCore)

// ============================================================================
//  CONSOLE COMMANDS
//
//  NOTA UE: FAutoConsoleCommandWithWorldAndArgs si registra da sola quando la
//  variabile globale viene costruita (cioe' al caricamento del modulo) e si
//  deregistra quando viene distrutta. Non serve scrivere codice di
//  registrazione ne' ricordarsi di pulire.
//
//  La variante "WithWorld" e' quella giusta per noi: la console passa il UWorld
//  corretto (PIE o editor). Usando la variante senza mondo bisognerebbe
//  indovinarlo con GWorld, che in PIE e' spesso quello sbagliato: e' una delle
//  fonti classiche di bug "funziona nell'editor ma non in gioco".
// ============================================================================

namespace GeoConsole
{
	static UGeoreferenceSubsystem* GetGeoreference(UWorld* World)
	{
		if (!World)
		{
			UE_LOG(LogGeoWorld, Warning, TEXT("Nessun mondo attivo."));
			return nullptr;
		}

		UGeoreferenceSubsystem* Georeference = World->GetSubsystem<UGeoreferenceSubsystem>();
		if (!Georeference)
		{
			UE_LOG(LogGeoWorld, Warning, TEXT("Subsystem di georeferenziazione non disponibile in questo mondo."));
		}
		return Georeference;
	}

	/** Stampa a schermo E nel log: a schermo per vederlo subito, nel log per
	 *  poterlo copiare. */
	static void Report(const FString& Message, const FColor& Color = FColor::Cyan)
	{
		UE_LOG(LogGeoWorld, Log, TEXT("%s"), *Message);
		if (GEngine)
		{
			GEngine->AddOnScreenDebugMessage(-1, 8.0f, Color, Message);
		}
	}
}

// --- geo.Help ---------------------------------------------------------------
static FAutoConsoleCommand GeoHelpCommand(
	TEXT("geo.Help"),
	TEXT("Elenca i comandi di GeoWorld."),
	FConsoleCommandDelegate::CreateStatic([]()
	{
		GeoConsole::Report(TEXT("geo.Where                    - posizione geografica della camera"));
		GeoConsole::Report(TEXT("geo.Origin                   - origine corrente e statistiche di rebase"));
		GeoConsole::Report(TEXT("geo.Goto <lat> <lon> [quota]  - teletrasporta su coordinate"));
		GeoConsole::Report(TEXT("geo.Goto <nome> [quota]      - teletrasporta su un luogo noto, quota SUL SUOLO"));
		GeoConsole::Report(TEXT("geo.Places                   - elenco dei luoghi noti"));
		GeoConsole::Report(TEXT("geo.Rebase                   - forza un rebase sulla posizione attuale"));
		GeoConsole::Report(TEXT("geo.AutoRebase <0|1>         - attiva/disattiva il rebasing automatico"));
		GeoConsole::Report(TEXT("geo.RebaseThreshold <km>     - cambia la soglia di rebasing"));
		GeoConsole::Report(TEXT("geo.Debug <0|1>              - overlay di debug"));
		GeoConsole::Report(TEXT("geo.SpawnMarkers             - piazza i cubi di verifica sull'Italia"));
		GeoConsole::Report(TEXT("--- Muoversi ---"));
		GeoConsole::Report(TEXT("geo.Fly                      - camera di volo (solo nel Play)"));
		GeoConsole::Report(TEXT("geo.Fly.Speed <x>            - moltiplicatore della velocita' di volo"));
		GeoConsole::Report(TEXT("geo.ViewSpeed <1..8> [x]     - velocita' della camera del viewport dell'editor"));
	}));

// --- geo.Where --------------------------------------------------------------
static FAutoConsoleCommandWithWorld GeoWhereCommand(
	TEXT("geo.Where"),
	TEXT("Stampa la posizione geografica della camera attiva."),
	FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* World)
	{
		UGeoreferenceSubsystem* Georeference = GeoConsole::GetGeoreference(World);
		if (!Georeference) { return; }

		FVector ViewLocation;
		if (!Georeference->GetActiveViewLocation(ViewLocation))
		{
			GeoConsole::Report(TEXT("Nessuna camera attiva."), FColor::Red);
			return;
		}

		const FGeoreferenceSnapshot Snapshot = Georeference->GetSnapshot();
		const FGeoCoordinate Coordinate =
			FGeoCoordinate::FromGeodetic(Snapshot.UnrealToGeodetic(ViewLocation));

		GeoConsole::Report(FString::Printf(TEXT("Camera: %s"), *Coordinate.ToDisplayString()));
		GeoConsole::Report(FString::Printf(TEXT("Unreal: %.1f, %.1f, %.1f uu  |  %.3f km dall'origine"),
			ViewLocation.X, ViewLocation.Y, ViewLocation.Z,
			Snapshot.DistanceFromOriginMeters(ViewLocation) / 1000.0));
	}));

// --- geo.Origin -------------------------------------------------------------
static FAutoConsoleCommandWithWorld GeoOriginCommand(
	TEXT("geo.Origin"),
	TEXT("Stampa l'origine corrente e le statistiche di rebasing."),
	FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* World)
	{
		UGeoreferenceSubsystem* Georeference = GeoConsole::GetGeoreference(World);
		if (!Georeference) { return; }

		const FGeoreferenceSnapshot Snapshot = Georeference->GetSnapshot();
		const GeoWorld::Core::FEcef OriginEcef = Snapshot.Georeference.GetOriginEcef();

		GeoConsole::Report(FString::Printf(TEXT("Origine   : %s"),
			*Georeference->GetOriginCoordinate().ToDisplayString()));
		GeoConsole::Report(FString::Printf(TEXT("ECEF      : %.3f, %.3f, %.3f m"),
			OriginEcef.X, OriginEcef.Y, OriginEcef.Z));
		GeoConsole::Report(FString::Printf(TEXT("Rebase    : %u   generation %u"),
			Georeference->GetRebaseCount(), Snapshot.Generation));
		GeoConsole::Report(FString::Printf(TEXT("Soglia    : %.2f km   (auto: %s)"),
			Georeference->GetRebaseThresholdMeters() / 1000.0,
			Georeference->IsAutoRebaseEnabled() ? TEXT("on") : TEXT("off")));
		GeoConsole::Report(FString::Printf(TEXT("Componenti: %d registrati"),
			Georeference->GetRegisteredComponentCount()));
	}));

// --- geo.Goto ---------------------------------------------------------------
//
//  Due forme, perche' servono due cose diverse: le coordinate quando si sta
//  verificando un numero, il nome quando si sta guardando il terreno.
//
//      geo.Goto 45.07 7.69 3000     lat, lon, quota ELLISSOIDICA
//      geo.Goto Torino              nome, quota di default SOPRA IL SUOLO
//      geo.Goto Monte Bianco 500    nome con spazi, 500 m sopra la vetta
//
//  La differenza di significato della quota fra le due forme e' voluta ed e'
//  spiegata in GeoPlaces.h: sopra una vetta di 4800 m, "quota 2000" inteso
//  sull'ellissoide metterebbe la camera dentro la montagna.
static FAutoConsoleCommandWithWorldAndArgs GeoGotoCommand(
	TEXT("geo.Goto"),
	TEXT("geo.Goto <lat> <lon> [quota] | <nome> [quota_sul_suolo] - porta la camera su un punto."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
		[](const TArray<FString>& Args, UWorld* World)
	{
		UGeoreferenceSubsystem* Georeference = GeoConsole::GetGeoreference(World);
		if (!Georeference) { return; }

		if (Args.Num() == 0)
		{
			GeoConsole::Report(TEXT("Uso: geo.Goto <lat> <lon> [quota]   oppure   geo.Goto <nome> [quota_sul_suolo]"), FColor::Red);
			GeoConsole::Report(TEXT("Nomi disponibili: geo.Places"));
			return;
		}

		// Il primo argomento decide la forma: un numero sono coordinate, una
		// parola e' un nome. Nessun flag da ricordare.
		if (Args[0].IsNumeric())
		{
			if (Args.Num() < 2)
			{
				GeoConsole::Report(TEXT("Uso: geo.Goto <lat> <lon> [quota_m]"), FColor::Red);
				return;
			}

			const FGeoCoordinate Destination(
				FCString::Atod(*Args[0]), FCString::Atod(*Args[1]),
				(Args.Num() >= 3) ? FCString::Atod(*Args[2]) : 2500.0);

			if (Georeference->TeleportViewTo(Destination.ToGeodetic()))
			{
				GeoConsole::Report(FString::Printf(TEXT("Teletrasporto -> %s"), *Destination.ToDisplayString()));
			}
			else
			{
				GeoConsole::Report(TEXT("Nessuna camera da spostare."), FColor::Red);
			}
			return;
		}

		// Forma per nome. L'ultimo argomento, se e' un numero, e' la quota sul
		// suolo; tutto il resto e' il nome, perche' "Monte Bianco" arriva qui
		// gia' spezzato in due argomenti dalla console.
		int32 NameArgCount = Args.Num();
		double AboveGround = GeoWorld::Places::DefaultAglM;
		if (Args.Num() >= 2 && Args.Last().IsNumeric())
		{
			AboveGround = FCString::Atod(*Args.Last());
			--NameArgCount;
		}

		FString Name;
		for (int32 Index = 0; Index < NameArgCount; ++Index) { Name += Args[Index]; }

		const GeoWorld::Places::FNamedPlace* Place = GeoWorld::Places::Find(Name);
		if (!Place)
		{
			GeoConsole::Report(FString::Printf(TEXT("Non conosco '%s'. Elenco: geo.Places"), *Name), FColor::Red);
			return;
		}

		const GeoWorld::Core::FGeodetic Destination =
			GeoWorld::Places::ToGeodetic(*Place, AboveGround);

		if (Georeference->TeleportViewTo(Destination))
		{
			GeoConsole::Report(FString::Printf(
				TEXT("-> %s   suolo %.0f m, camera %.0f m sopra (quota ellissoidica %.0f m)"),
				Place->Name, Place->GroundElevationM, AboveGround, Destination.HeightM));
		}
		else
		{
			GeoConsole::Report(TEXT("Nessuna camera da spostare."), FColor::Red);
		}
	}));

// --- geo.Places -------------------------------------------------------------
static FAutoConsoleCommand GeoPlacesCommand(
	TEXT("geo.Places"),
	TEXT("Elenca i luoghi noti utilizzabili con geo.Goto."),
	FConsoleCommandDelegate::CreateStatic([]()
	{
		GeoConsole::Report(TEXT("--- Luoghi noti (geo.Goto <nome> [quota_sul_suolo]) ---"));
		for (const GeoWorld::Places::FNamedPlace& Place : GeoWorld::Places::Table)
		{
			GeoConsole::Report(FString::Printf(TEXT("  %-14s %8.4f  %8.4f   suolo %5.0f m"),
				Place.Name, Place.LatitudeDeg, Place.LongitudeDeg, Place.GroundElevationM));
		}
		GeoConsole::Report(TEXT("Il nome non distingue maiuscole ne' spazi, e basta un pezzo: 'gar' -> LagoDiGarda."));
	}));

// --- geo.ViewSpeed ----------------------------------------------------------
//
//  PERCHE' SERVE UN COMANDO SOLO PER QUESTO. Nel viewport dell'editor la camera
//  non e' un attore e non la si puo' sostituire: e' uno stato del viewport
//  client. La sua velocita' si regola con la rotella tenendo premuto il tasto
//  destro, ma la scala del progetto (1 uu = 1 cm) rende il massimo
//  dell'interfaccia comunque lento per un pianeta: la moltiplica serve.
//
//  Se sei nel Play e non nel viewport, questo comando non c'entra: li' la
//  camera e' un attore e la risposta e' geo.Fly.
#if WITH_EDITOR
// NOTA UE: il tipo e' FAutoConsoleCommandWithWorldAndArgs anche se il mondo qui
// non servirebbe. Non e' pigrizia: FAutoConsoleCommandWithArgs NON ESISTE nel
// motore. I tipi disponibili sono FAutoConsoleCommand (che accetta anche un
// delegato con argomenti), ...WithWorld, ...WithWorldAndArgs,
// ...WithOutputDevice, ...WithArgsAndOutputDevice e
// ...WithWorldArgsAndOutputDevice. Lo impone ora la regola 5 di
// CheckSourceDiscipline.sh, perche' il compilatore non e' disponibile qui.
// Il mondo, gia' che c'e', serve a dare un messaggio migliore.
static FAutoConsoleCommandWithWorldAndArgs GeoViewSpeedCommand(
	TEXT("geo.ViewSpeed"),
	TEXT("geo.ViewSpeed <1..8> [moltiplicatore] - velocita' della camera del viewport dell'editor."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
		[](const TArray<FString>& Args, UWorld* World)
	{
		if (!GEditor)
		{
			GeoConsole::Report(TEXT("Nessun editor."), FColor::Red);
			return;
		}

		// Se si sta giocando, la camera non e' quella del viewport: e' un
		// attore, e questo comando non la toccherebbe. Meglio dirlo che
		// lasciar credere di aver cambiato qualcosa.
		if (World && World->WorldType == EWorldType::PIE)
		{
			GeoConsole::Report(
				TEXT("Sei nel Play: qui la camera e' un attore. Usa geo.Fly e geo.Fly.Speed."), FColor::Yellow);
			return;
		}

		FViewport* Viewport = GEditor->GetActiveViewport();
		FEditorViewportClient* Client = Viewport
			? static_cast<FEditorViewportClient*>(Viewport->GetClient()) : nullptr;
		if (!Client)
		{
			GeoConsole::Report(TEXT("Nessun viewport attivo. Clicca dentro il viewport e riprova."), FColor::Red);
			return;
		}

		if (Args.Num() >= 1)
		{
			Client->SetCameraSpeedSetting(FMath::Clamp(FCString::Atoi(*Args[0]), 1, 8));
		}
		if (Args.Num() >= 2)
		{
			Client->SetCameraSpeedScalar(FMath::Clamp(FCString::Atof(*Args[1]), 1.0f, 128.0f));
		}

		GeoConsole::Report(FString::Printf(
			TEXT("Viewport dell'editor: velocita' %d, moltiplicatore x%.1f"),
			Client->GetCameraSpeedSetting(), Client->GetCameraSpeedScalar()));
		GeoConsole::Report(TEXT("Per volare sul terreno da vicino: 4 e moltiplicatore 8 e' un buon punto di partenza."));
	}));
#endif

// --- geo.Rebase -------------------------------------------------------------
static FAutoConsoleCommandWithWorld GeoRebaseCommand(
	TEXT("geo.Rebase"),
	TEXT("Forza un rebase dell'origine sulla posizione attuale della camera."),
	FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* World)
	{
		UGeoreferenceSubsystem* Georeference = GeoConsole::GetGeoreference(World);
		if (!Georeference) { return; }

		FVector ViewLocation;
		if (!Georeference->GetActiveViewLocation(ViewLocation))
		{
			GeoConsole::Report(TEXT("Nessuna camera attiva."), FColor::Red);
			return;
		}

		const FGeoreferenceSnapshot Snapshot = Georeference->GetSnapshot();
		Georeference->SetOrigin(Snapshot.UnrealToGeodetic(ViewLocation));

		GeoConsole::Report(FString::Printf(TEXT("Rebase forzato -> %s"),
			*Georeference->GetOriginCoordinate().ToDisplayString()));
	}));

// --- geo.AutoRebase ---------------------------------------------------------
static FAutoConsoleCommandWithWorldAndArgs GeoAutoRebaseCommand(
	TEXT("geo.AutoRebase"),
	TEXT("geo.AutoRebase <0|1> - attiva o disattiva il rebasing automatico."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
		[](const TArray<FString>& Args, UWorld* World)
	{
		UGeoreferenceSubsystem* Georeference = GeoConsole::GetGeoreference(World);
		if (!Georeference) { return; }

		const bool bEnabled = (Args.Num() >= 1) ? (FCString::Atoi(*Args[0]) != 0)
		                                        : !Georeference->IsAutoRebaseEnabled();
		Georeference->SetAutoRebaseEnabled(bEnabled);
		GeoConsole::Report(FString::Printf(TEXT("Rebasing automatico: %s"),
			bEnabled ? TEXT("ON") : TEXT("OFF")));
	}));

// --- geo.RebaseThreshold ----------------------------------------------------
static FAutoConsoleCommandWithWorldAndArgs GeoRebaseThresholdCommand(
	TEXT("geo.RebaseThreshold"),
	TEXT("geo.RebaseThreshold <km> - cambia a caldo la soglia di rebasing."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
		[](const TArray<FString>& Args, UWorld* World)
	{
		UGeoreferenceSubsystem* Georeference = GeoConsole::GetGeoreference(World);
		if (!Georeference) { return; }

		if (Args.Num() >= 1)
		{
			Georeference->SetRebaseThresholdMeters(FCString::Atod(*Args[0]) * 1000.0);
		}

		GeoConsole::Report(FString::Printf(TEXT("Soglia di rebasing: %.2f km"),
			Georeference->GetRebaseThresholdMeters() / 1000.0));
	}));

// --- geo.Debug --------------------------------------------------------------
static FAutoConsoleCommandWithWorldAndArgs GeoDebugCommand(
	TEXT("geo.Debug"),
	TEXT("geo.Debug <0|1> - mostra o nasconde l'overlay di debug."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
		[](const TArray<FString>& Args, UWorld* World)
	{
		UGeoreferenceSubsystem* Georeference = GeoConsole::GetGeoreference(World);
		if (!Georeference) { return; }

		const bool bEnabled = (Args.Num() >= 1) ? (FCString::Atoi(*Args[0]) != 0)
		                                        : !Georeference->IsDebugOverlayEnabled();
		Georeference->SetDebugOverlayEnabled(bEnabled);

		// Ripulisce le righe rimaste a schermo quando si spegne l'overlay.
		if (!bEnabled && GEngine)
		{
			GEngine->ClearOnScreenDebugMessages();
		}

		GeoConsole::Report(FString::Printf(TEXT("Overlay di debug: %s"),
			bEnabled ? TEXT("ON") : TEXT("OFF")));
	}));

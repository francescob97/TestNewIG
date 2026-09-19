#include "GeoCoreModule.h"

#include "Georeference/GeoreferenceSubsystem.h"
#include "Georeference/GeoWorldTypes.h"

#include "Engine/Engine.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"

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
		GeoConsole::Report(TEXT("geo.Goto <lat> <lon> [quota] - teletrasporta la camera su un punto"));
		GeoConsole::Report(TEXT("geo.Rebase                   - forza un rebase sulla posizione attuale"));
		GeoConsole::Report(TEXT("geo.AutoRebase <0|1>         - attiva/disattiva il rebasing automatico"));
		GeoConsole::Report(TEXT("geo.RebaseThreshold <km>     - cambia la soglia di rebasing"));
		GeoConsole::Report(TEXT("geo.Debug <0|1>              - overlay di debug"));
		GeoConsole::Report(TEXT("geo.SpawnMarkers             - piazza i cubi di verifica sull'Italia"));
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
static FAutoConsoleCommandWithWorldAndArgs GeoGotoCommand(
	TEXT("geo.Goto"),
	TEXT("geo.Goto <lat> <lon> [quota_m] - teletrasporta la camera su un punto geografico."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
		[](const TArray<FString>& Args, UWorld* World)
	{
		UGeoreferenceSubsystem* Georeference = GeoConsole::GetGeoreference(World);
		if (!Georeference) { return; }

		if (Args.Num() < 2)
		{
			GeoConsole::Report(TEXT("Uso: geo.Goto <lat> <lon> [quota_m]"), FColor::Red);
			return;
		}

		const double Latitude  = FCString::Atod(*Args[0]);
		const double Longitude = FCString::Atod(*Args[1]);
		const double Height    = (Args.Num() >= 3) ? FCString::Atod(*Args[2]) : 500.0;

		const FGeoCoordinate Destination(Latitude, Longitude, Height);

		if (Georeference->TeleportViewTo(Destination.ToGeodetic()))
		{
			GeoConsole::Report(FString::Printf(TEXT("Teletrasporto -> %s"), *Destination.ToDisplayString()));
		}
		else
		{
			GeoConsole::Report(TEXT("Nessuna camera da spostare."), FColor::Red);
		}
	}));

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

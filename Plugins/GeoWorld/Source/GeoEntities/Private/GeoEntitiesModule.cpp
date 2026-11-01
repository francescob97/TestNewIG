// =============================================================================
//  Modulo GeoEntities (Fase 7): i comandi geo.Entities.* e la riga del
//  riquadro della posizione.
// =============================================================================
#include "Modules/ModuleManager.h"

#include "GeoCoreModule.h"
#include "GeoConsoleArgs.h"
#include "GeoEntitySubsystem.h"
#include "GeoPositionHudSubsystem.h"

#include "Engine/Engine.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"

namespace GeoEntitiesConsole
{
	static void Report(const FString& Message, const FColor& Colour = FColor::Cyan)
	{
		UE_LOG(LogGeoWorld, Log, TEXT("%s"), *Message);
		if (GEngine) { GEngine->AddOnScreenDebugMessage(-1, 10.0f, Colour, Message); }
	}

	static UGeoEntitySubsystem* Get(UWorld* World)
	{
		UGeoEntitySubsystem* Entities = World ? World->GetSubsystem<UGeoEntitySubsystem>() : nullptr;
		if (!Entities) { Report(TEXT("Entita' non disponibili in questo mondo."), FColor::Red); }
		return Entities;
	}

	static void ReportState(const UGeoEntitySubsystem* Entities)
	{
		const FGeoEntityStats Stats = Entities->GetStats();
		if (!Entities->IsListening())
		{
			Report(TEXT("Entita': nessuna sorgente. geo.Entities.Listen (e fuori: geoworld demo-entities)"), FColor::Yellow);
		}
		for (const FString& Line : Entities->DescribeSources()) { Report(TEXT("  ") + Line); }
		Report(FString::Printf(TEXT("Entita': %d nel registro, %d a schermo (entro %.0f km, al massimo %d), %d con la forma di ripiego"),
			Stats.Entita, Stats.Attori, Entities->GetActorRadiusM() / 1000.0, Entities->GetMaxActors(), Stats.Ripieghi));
		Report(FString::Printf(TEXT("  %lld aggiornamenti (%.0f al secondo), %lld in disordine, %lld rientri saltati; rientro %.2f s, se ne va dopo %.1f s di silenzio"),
			Stats.Aggiornamenti, Stats.AggiornamentiAlSecondo, Stats.InRitardo, Stats.Salti, Entities->GetBlendSeconds(), Entities->GetTimeoutSeconds()));
		if (Stats.bGeoideDiRipiego)
		{
			Report(TEXT("  Quote sul livello del mare alzate di 48 m (l'Italia): per il geoide vero apri il dataset dell'acqua (geo.Water.Open)."), FColor::Yellow);
		}
	}
}

// --- geo.Entities.Listen ----------------------------------------------------
static FAutoConsoleCommandWithWorldAndArgs GeoEntitiesListenCommand(
	TEXT("geo.Entities.Listen"),
	TEXT("geo.Entities.Listen [nome] - in ascolto della memoria condivisa (default GeoWorldEntities): aerei, veicoli, navi da un simulatore, o da 'geoworld demo-entities'."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>& RawArgs, UWorld* World)
	{
		UGeoEntitySubsystem* Entities = GeoEntitiesConsole::Get(World);
		if (!Entities) { return; }
		const TArray<FString> Args = GeoWorld::Console::QuotedArgs(RawArgs);
		FString Error;
		if (!Entities->ListenSharedMemory(Args.Num() >= 1 ? Args[0] : FString(), Error))
		{
			GeoEntitiesConsole::Report(FString::Printf(TEXT("Entita': %s"), *Error), FColor::Red);
			return;
		}
		GeoEntitiesConsole::Report(TEXT("Entita': in ascolto. Il generatore di prova: Pipeline> geoworld demo-entities (Torino; --lat --lon per altrove)"));
		GeoEntitiesConsole::ReportState(Entities);
	}));

// --- geo.Entities.Stop --------------------------------------------------------
static FAutoConsoleCommandWithWorld GeoEntitiesStopCommand(
	TEXT("geo.Entities.Stop"),
	TEXT("Ferma tutte le sorgenti di entita' e le toglie dalla scena."),
	FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* World)
	{
		UGeoEntitySubsystem* Entities = GeoEntitiesConsole::Get(World);
		if (!Entities) { return; }
		Entities->StopAll();
		GeoEntitiesConsole::Report(TEXT("Entita': sorgenti ferme, scena vuota."));
	}));

// --- geo.Entities.List --------------------------------------------------------
static FAutoConsoleCommandWithWorldAndArgs GeoEntitiesListCommand(
	TEXT("geo.Entities.List"),
	TEXT("geo.Entities.List [quante] - le sorgenti e le entita' piu' vicine: tipo, modello, posizione, velocita', estrapolazione (default 20)."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>& Args, UWorld* World)
	{
		UGeoEntitySubsystem* Entities = GeoEntitiesConsole::Get(World);
		if (!Entities) { return; }
		GeoEntitiesConsole::ReportState(Entities);
		const int32 Count = Args.Num() >= 1 ? FMath::Clamp(FCString::Atoi(*Args[0]), 1, 1000) : 20;
		for (const FString& Line : Entities->DescribeEntities(Count)) { GeoEntitiesConsole::Report(Line, FColor::White); }
	}));

// --- geo.Entities.Blend -------------------------------------------------------
static FAutoConsoleCommandWithWorldAndArgs GeoEntitiesBlendCommand(
	TEXT("geo.Entities.Blend"),
	TEXT("geo.Entities.Blend <s> - quanto dura il rientro morbido quando un aggiornamento non coincide con l'estrapolazione (0: salta subito; default 0,4)."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>& Args, UWorld* World)
	{
		UGeoEntitySubsystem* Entities = GeoEntitiesConsole::Get(World);
		if (!Entities) { return; }
		if (Args.Num() >= 1) { Entities->SetBlendSeconds(FCString::Atod(*Args[0])); }
		GeoEntitiesConsole::Report(FString::Printf(TEXT("Entita': rientro in %.2f s%s"), Entities->GetBlendSeconds(),
			Entities->GetBlendSeconds() <= 0.0 ? TEXT(" (salta: si vedono gli scatti)") : TEXT("")));
	}));

// --- geo.Entities.Timeout -----------------------------------------------------
static FAutoConsoleCommandWithWorldAndArgs GeoEntitiesTimeoutCommand(
	TEXT("geo.Entities.Timeout"),
	TEXT("geo.Entities.Timeout <s> - un'entita' che tace da tanto se ne va (default 5)."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>& Args, UWorld* World)
	{
		UGeoEntitySubsystem* Entities = GeoEntitiesConsole::Get(World);
		if (!Entities) { return; }
		if (Args.Num() >= 1) { Entities->SetTimeoutSeconds(FCString::Atod(*Args[0])); }
		GeoEntitiesConsole::Report(FString::Printf(TEXT("Entita': se ne vanno dopo %.1f s di silenzio"), Entities->GetTimeoutSeconds()));
	}));

// --- geo.Entities.Radius ------------------------------------------------------
static FAutoConsoleCommandWithWorldAndArgs GeoEntitiesRadiusCommand(
	TEXT("geo.Entities.Radius"),
	TEXT("geo.Entities.Radius <m> [max] - fin dove un'entita' ha un attore, e quanti attori al massimo (i piu' vicini). Le altre restano nel registro."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>& Args, UWorld* World)
	{
		UGeoEntitySubsystem* Entities = GeoEntitiesConsole::Get(World);
		if (!Entities) { return; }
		if (Args.Num() >= 1) { Entities->SetActorRadiusM(FCString::Atod(*Args[0])); }
		if (Args.Num() >= 2) { Entities->SetMaxActors(FCString::Atoi(*Args[1])); }
		GeoEntitiesConsole::Report(FString::Printf(TEXT("Entita': attori entro %.1f km, al massimo %d"),
			Entities->GetActorRadiusM() / 1000.0, Entities->GetMaxActors()));
	}));

// --- geo.Entities.Models ------------------------------------------------------
static FAutoConsoleCommandWithWorldAndArgs GeoEntitiesModelsCommand(
	TEXT("geo.Entities.Models"),
	TEXT("geo.Entities.Models [file|default] - la tabella dei modelli (tipo DIS o indice CIGI -> forma o asset), ricaricata a caldo; senza argomenti la elenca."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>& RawArgs, UWorld* World)
	{
		UGeoEntitySubsystem* Entities = GeoEntitiesConsole::Get(World);
		if (!Entities) { return; }
		const TArray<FString> Args = GeoWorld::Console::QuotedArgs(RawArgs);
		if (Args.Num() >= 1)
		{
			TArray<FString> Errors;
			const FString Path = Args[0].Equals(TEXT("default"), ESearchCase::IgnoreCase) ? FString() : Args[0];
			Entities->LoadModelTable(Path, Errors);
			for (const FString& Error : Errors) { GeoEntitiesConsole::Report(TEXT("  ") + Error, FColor::Red); }
		}
		const GeoWorld::Entities::FModelTable& Table = Entities->GetModelTable();
		GeoEntitiesConsole::Report(FString::Printf(TEXT("Modelli: %s, %d regole (piu' quelle di serie)"),
			Entities->GetModelTablePath().IsEmpty() ? TEXT("la tabella di serie") : *Entities->GetModelTablePath(),
			static_cast<int32>(Table.NumFromFile())));
		for (const GeoWorld::Entities::FModelRule& Rule : Table.All())
		{
			GeoEntitiesConsole::Report(FString::Printf(TEXT("  %-20s %s%s"), UTF8_TO_TCHAR(Rule.PatternText().c_str()),
				UTF8_TO_TCHAR(Rule.Model.Text.c_str()), Rule.bBuiltIn ? TEXT("   (di serie)") : TEXT("")), FColor::White);
		}
		for (const FString& Missing : Entities->GetMissingAssets())
		{
			GeoEntitiesConsole::Report(FString::Printf(TEXT("  l'asset %s non si carica: al suo posto una scatola"), *Missing), FColor::Red);
		}
	}));

// --- geo.Entities.Follow ------------------------------------------------------
static FAutoConsoleCommandWithWorldAndArgs GeoEntitiesFollowCommand(
	TEXT("geo.Entities.Follow"),
	TEXT("geo.Entities.Follow <nome|numero|off> - la camera del Play segue un'entita', da dietro e da sopra (es. geo.Entities.Follow AEREO-1)."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>& RawArgs, UWorld* World)
	{
		UGeoEntitySubsystem* Entities = GeoEntitiesConsole::Get(World);
		if (!Entities) { return; }
		const TArray<FString> Args = GeoWorld::Console::QuotedArgs(RawArgs);
		if (Args.Num() < 1 || Args[0].Equals(TEXT("off"), ESearchCase::IgnoreCase) || Args[0] == TEXT("0"))
		{
			Entities->StopFollowing();
			GeoEntitiesConsole::Report(TEXT("Entita': la camera e' di nuovo tua."));
			return;
		}
		FString Who;
		if (!Entities->Follow(Args[0], Who))
		{
			GeoEntitiesConsole::Report(FString::Printf(TEXT("Entita': non trovo '%s' (geo.Entities.List per i nomi)"), *Args[0]), FColor::Red);
			return;
		}
		const bool bPlaying = World && World->GetFirstPlayerController() != nullptr;
		GeoEntitiesConsole::Report(FString::Printf(TEXT("Entita': seguo %s%s"), *Who,
			bPlaying ? TEXT(" (geo.Entities.Follow off per smettere)") : TEXT(" appena parte il Play: nell'editor la camera la muovi tu")));
	}));

// --- geo.Entities.Labels ------------------------------------------------------
static FAutoConsoleCommandWithWorldAndArgs GeoEntitiesLabelsCommand(
	TEXT("geo.Entities.Labels"),
	TEXT("geo.Entities.Labels [0|1] - il nome sopra ogni entita' (magenta, con il tipo, se la forma e' di ripiego)."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>& Args, UWorld* World)
	{
		UGeoEntitySubsystem* Entities = GeoEntitiesConsole::Get(World);
		if (!Entities) { return; }
		Entities->SetLabelsEnabled(Args.Num() >= 1 ? FCString::Atoi(*Args[0]) != 0 : !Entities->AreLabelsEnabled());
		GeoEntitiesConsole::Report(FString::Printf(TEXT("Entita': nomi %s"), Entities->AreLabelsEnabled() ? TEXT("accesi") : TEXT("spenti")));
	}));

// --- geo.Entities.Debug -------------------------------------------------------
static FAutoConsoleCommandWithWorldAndArgs GeoEntitiesDebugCommand(
	TEXT("geo.Entities.Debug"),
	TEXT("geo.Entities.Debug [0|1] - la velocita' (verde: dove sara' fra un secondo; rossa: notizie ferme), l'errore che il rientro sta smaltendo (giallo), la distanza nei nomi."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>& Args, UWorld* World)
	{
		UGeoEntitySubsystem* Entities = GeoEntitiesConsole::Get(World);
		if (!Entities) { return; }
		Entities->SetDebugOverlayEnabled(Args.Num() >= 1 ? FCString::Atoi(*Args[0]) != 0 : !Entities->IsDebugOverlayEnabled());
		GeoEntitiesConsole::Report(FString::Printf(TEXT("Entita': diagnostica %s"), Entities->IsDebugOverlayEnabled() ? TEXT("accesa") : TEXT("spenta")));
	}));

// ============================================================================
//  Il modulo: la riga del riquadro della posizione
// ============================================================================

namespace GeoEntitiesHud
{
	static void AddLines(const UWorld* World, const FVector& /*ViewLocation*/, FGeoHudLines& Lines)
	{
		const UGeoEntitySubsystem* Entities = World ? World->GetSubsystem<UGeoEntitySubsystem>() : nullptr;
		if (!Entities) { return; }
		const FString State = Entities->DescribeForHud();
		if (!State.IsEmpty()) { Lines.Emplace(TEXT("Entita'"), State); }
		const FString Followed = Entities->DescribeFollowed();
		if (!Followed.IsEmpty()) { Lines.Emplace(TEXT("Seguo"), Followed); }
	}
}

class FGeoEntitiesModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		HudHandle = UGeoPositionHudSubsystem::ExtraLines().AddStatic(&GeoEntitiesHud::AddLines);
	}

	virtual void ShutdownModule() override
	{
		UGeoPositionHudSubsystem::ExtraLines().Remove(HudHandle);
	}

private:
	FDelegateHandle HudHandle;
};

IMPLEMENT_MODULE(FGeoEntitiesModule, GeoEntities)

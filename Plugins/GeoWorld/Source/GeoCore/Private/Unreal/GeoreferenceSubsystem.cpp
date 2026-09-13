#include "Unreal/GeoreferenceSubsystem.h"

#include "Unreal/GeoTransformComponent.h"
#include "Unreal/GeoWorldSettings.h"

#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/PlayerController.h"
#include "Camera/PlayerCameraManager.h"
#include "DrawDebugHelpers.h"
#include "Geo/GeoUnits.h"

#if WITH_EDITOR
	// Vedi la nota in GeoCore.Build.cs: dipendenza da UnrealEd ammessa solo
	// quando si costruisce un target che contiene l'editor.
	#include "Editor.h"
	#include "EditorViewportClient.h"
#endif

namespace
{
	using namespace GeoWorld::Core;

	/** Colori dell'overlay, raccolti qui per non sparpagliarli nel codice. */
	constexpr int32 DebugMessageKeyBase = 0x6E60; // chiavi stabili: le righe si
	                                              // aggiornano invece di scorrere
}

// ============================================================================
//  Ciclo di vita
// ============================================================================

bool UGeoreferenceSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	if (!Super::ShouldCreateSubsystem(Outer))
	{
		return false;
	}

	// Unreal crea in continuazione mondi "di servizio": anteprime delle
	// miniature degli asset, preview dei materiali, mondi di importazione.
	// Creare il subsystem li' significherebbe decine di istanze che ticchettano
	// a vuoto. Ci interessano solo i mondi veri.
	const UWorld* World = Cast<UWorld>(Outer);
	if (!World)
	{
		return false;
	}

	return World->WorldType == EWorldType::Game
	    || World->WorldType == EWorldType::PIE
	    || World->WorldType == EWorldType::Editor;
}

void UGeoreferenceSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	// GetDefault<T>() restituisce il CDO (Class Default Object): l'istanza
	// prototipo che Unreal crea per ogni classe e che, per un UDeveloperSettings,
	// contiene i valori letti dal .ini. Non va istanziato nulla a mano.
	const UGeoWorldSettings* Settings = GetDefault<UGeoWorldSettings>();

	RebaseThresholdMeters   = Settings->RebaseThresholdKm * 1000.0;
	MinFramesBetweenRebases = Settings->MinFramesBetweenRebases;
	bAutoRebase             = Settings->bAutoRebase;
	bMoveViewTargetOnRebase = Settings->bMoveViewTargetOnRebase;
	bShowDebugOverlay       = Settings->bShowDebugOverlay;

	{
		FRWScopeLock Lock(SnapshotLock, SLT_Write);
		Snapshot.Georeference.SetOrigin(Settings->InitialOrigin.ToGeodetic());
		Snapshot.Generation = 0;
	}

	// ------------------------------------------------------------------
	//  Controllo di sanita' sulle unita'.
	//
	//  WorldToMeters e' la convenzione del MOTORE (default 100) e viene letta
	//  da audio, VR e fisica. Noi non la usiamo nei calcoli - vogliamo una
	//  costante di compilazione, non una variabile di livello - ma se qualcuno
	//  la cambia in un livello e' meglio saperlo subito, invece di scoprire il
	//  mondo scalato 100x tre fasi piu' avanti.
	//
	//  ensureMsgf: a differenza di check() non interrompe l'esecuzione, logga
	//  un callstack la prima volta e prosegue. Giusto per una condizione
	//  anomala ma non fatale.
	// ------------------------------------------------------------------
	if (const UWorld* World = GetWorld())
	{
		if (const AWorldSettings* WorldSettings = World->GetWorldSettings())
		{
			const double EngineWorldToMeters = static_cast<double>(WorldSettings->WorldToMeters);
			ensureMsgf(
				FMath::IsNearlyEqual(EngineWorldToMeters, GeoWorld::Units::MetersToUu, 0.01),
				TEXT("GeoWorld assume 1 unita' Unreal = 1 cm, ma WorldSettings->WorldToMeters vale %.3f ")
				TEXT("invece di %.3f: tutte le distanze sarebbero sbagliate di un fattore %.3f."),
				EngineWorldToMeters, GeoWorld::Units::MetersToUu,
				EngineWorldToMeters / GeoWorld::Units::MetersToUu);
		}
	}

	UE_LOG(LogTemp, Log, TEXT("[GeoWorld] Georeferenziazione inizializzata. Origine: %s, soglia rebase: %.1f km"),
		*GetOriginCoordinate().ToDisplayString(), RebaseThresholdMeters / 1000.0);
}

void UGeoreferenceSubsystem::Deinitialize()
{
	RegisteredComponents.Reset();
	OnGeoreferenceRebased.Clear();
	Super::Deinitialize();
}

// ============================================================================
//  Tick e rebasing
// ============================================================================

TStatId UGeoreferenceSubsystem::GetStatId() const
{
	// Macro di Unreal che dichiara il contatore per il profiler (stat tickables).
	RETURN_QUICK_DECLARE_CYCLE_STAT(UGeoreferenceSubsystem, STATGROUP_Tickables);
}

bool UGeoreferenceSubsystem::IsTickable() const
{
	// Il CDO non deve mai ticchettare: e' un prototipo, non un'istanza viva.
	return !IsTemplate() && GetWorld() != nullptr;
}

void UGeoreferenceSubsystem::Tick(float DeltaTime)
{
	if (bShowDebugOverlay)
	{
		DrawDebugOverlay();
	}

	if (FramesSinceLastRebase < MinFramesBetweenRebases)
	{
		++FramesSinceLastRebase;
		return;
	}

	if (!bAutoRebase)
	{
		return;
	}

	FVector ViewLocation;
	if (!GetActiveViewLocation(ViewLocation))
	{
		return;
	}

	// SEMPLIFICAZIONE UTILE: l'origine della georeferenziazione mappa SEMPRE
	// nell'origine dello spazio di Unreal (0,0,0), per costruzione della
	// trasformazione. Quindi la distanza dall'origine e' semplicemente la norma
	// della posizione: non serve passare per l'ECEF.
	const double DistanceMeters = Snapshot.DistanceFromOriginMeters(ViewLocation);

	if (DistanceMeters < RebaseThresholdMeters)
	{
		return;
	}

	LastRebaseDistanceMeters = DistanceMeters;

	// La nuova origine e' la posizione ATTUALE della camera: cosi' la distanza
	// riparte esattamente da zero ed e' praticamente impossibile che un secondo
	// rebase scatti subito dopo (il MinFramesBetweenRebases e' il paracadute).
	ApplyRebase(Snapshot.UnrealToGeodetic(ViewLocation));
}

void UGeoreferenceSubsystem::SetOrigin(const FGeodetic& NewOrigin)
{
	ApplyRebase(NewOrigin);
}

void UGeoreferenceSubsystem::ApplyRebase(const FGeodetic& NewOrigin)
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	// --------------------------------------------------------------------
	//  PASSO 1 - Congelare in ECEF cio' che NON e' georeferenziato e che
	//  dobbiamo comunque tenere fermo: la camera.
	//
	//  Tutto cio' che e' georeferenziato (i nostri componenti, e dalla Fase 4
	//  il terreno) non va "spostato": ricalcola la propria posizione dalla
	//  propria coordinata geodetica, che e' l'unica autorita'. E' questo che
	//  rende il rebasing privo di deriva - non si compone mai una
	//  trasformazione con la precedente.
	//
	//  LIMITE NOTO E VOLUTO: un attore qualunque piazzato a mano nel livello e
	//  privo di UGeoTransformComponent APPARIRA' SPOSTATO dopo un rebase,
	//  perche' la sua posizione e' in unita' Unreal e quelle cambiano
	//  significato. In Fase 1 il mondo e' vuoto a parte i nostri marker, e dalla
	//  Fase 4 il terreno e' tutto georeferenziato. Se in futuro servisse
	//  supportare contenuto non georeferenziato, la soluzione e' agganciarlo a
	//  un attore-ancora georeferenziato, non spostarlo a mano.
	// --------------------------------------------------------------------
	AActor* ViewTarget = nullptr;
	FEcef   ViewTargetEcef{};
	bool    bHasViewTarget = false;

	if (bMoveViewTargetOnRebase)
	{
		if (APlayerController* PC = World->GetFirstPlayerController())
		{
			ViewTarget = PC->GetViewTarget();
			if (ViewTarget)
			{
				ViewTargetEcef = Snapshot.UnrealToEcef(ViewTarget->GetActorLocation());
				bHasViewTarget = true;
			}
		}
	}

#if WITH_EDITOR
	// Nell'editor (fuori dal Play) la camera non e' un attore: e' uno stato del
	// viewport client. Va salvata e ripristinata separatamente, altrimenti
	// muovendosi nel viewport dell'editor il mondo "scappa" al primo rebase.
	FEditorViewportClient* EditorViewportClient = nullptr;
	FEcef EditorCameraEcef{};
	bool  bHasEditorCamera = false;

	if (bMoveViewTargetOnRebase && GEditor && World->WorldType == EWorldType::Editor)
	{
		if (FViewport* Viewport = GEditor->GetActiveViewport())
		{
			EditorViewportClient = static_cast<FEditorViewportClient*>(Viewport->GetClient());
			if (EditorViewportClient)
			{
				EditorCameraEcef = Snapshot.UnrealToEcef(EditorViewportClient->GetViewLocation());
				bHasEditorCamera = true;
			}
		}
	}
#endif

	// --------------------------------------------------------------------
	//  PASSO 2 - Cambiare origine.
	// --------------------------------------------------------------------
	{
		FRWScopeLock Lock(SnapshotLock, SLT_Write);
		Snapshot.Georeference.SetOrigin(NewOrigin);
		++Snapshot.Generation;
	}

	++RebaseCount;
	FramesSinceLastRebase = 0;

	// --------------------------------------------------------------------
	//  PASSO 3 - Rimettere la camera dove stava GEOGRAFICAMENTE.
	// --------------------------------------------------------------------
	if (bHasViewTarget && IsValid(ViewTarget))
	{
		const FVector NewLocation = Snapshot.EcefToUnreal(ViewTargetEcef);

		// NOTA UE, due parametri che sembrano dettagli e non lo sono:
		//
		//  bSweep = false: questo NON e' un movimento, e' un cambio di sistema
		//  di riferimento. Uno sweep testerebbe le collisioni lungo un segmento
		//  di 10 km e genererebbe urti fantasma.
		//
		//  ETeleportType::TeleportPhysics: dice a Chaos di AZZERARE la velocita'
		//  implicita e di non interpolare. Senza, il motore fisico deduce da
		//  solo una velocita' di 10 km in un frame e spara il pawn nello spazio.
		ViewTarget->SetActorLocation(NewLocation, /*bSweep=*/false,
			/*OutSweepHitResult=*/nullptr, ETeleportType::TeleportPhysics);
	}

#if WITH_EDITOR
	if (bHasEditorCamera && EditorViewportClient)
	{
		EditorViewportClient->SetViewLocation(Snapshot.EcefToUnreal(EditorCameraEcef));
		EditorViewportClient->Invalidate();
	}
#endif

	// --------------------------------------------------------------------
	//  PASSO 4 - Notificare chi deve ricalcolarsi.
	// --------------------------------------------------------------------
	RefreshRegisteredComponents();
	OnGeoreferenceRebased.Broadcast(Snapshot);

	UE_LOG(LogTemp, Verbose, TEXT("[GeoWorld] Rebase #%u -> %s (camera era a %.1f km)"),
		RebaseCount, *GetOriginCoordinate().ToDisplayString(), LastRebaseDistanceMeters / 1000.0);
}

void UGeoreferenceSubsystem::RefreshRegisteredComponents()
{
	// RemoveAll compatta l'array eliminando i componenti nel frattempo
	// distrutti dal garbage collector: le weak pointer sono diventate nulle.
	RegisteredComponents.RemoveAll(
		[](const TWeakObjectPtr<UGeoTransformComponent>& Weak) { return !Weak.IsValid(); });

	for (const TWeakObjectPtr<UGeoTransformComponent>& Weak : RegisteredComponents)
	{
		if (UGeoTransformComponent* Component = Weak.Get())
		{
			Component->RefreshFromGeoreference();
		}
	}
}

void UGeoreferenceSubsystem::SetRebaseThresholdMeters(double NewThresholdMeters)
{
	RebaseThresholdMeters = FMath::Max(100.0, NewThresholdMeters);
}

// ============================================================================
//  Accesso allo stato
// ============================================================================

FGeoreferenceSnapshot UGeoreferenceSubsystem::GetSnapshot() const
{
	FRWScopeLock Lock(SnapshotLock, SLT_ReadOnly);
	return Snapshot;   // copia per valore: e' il punto di tutta la struct
}

FGeoCoordinate UGeoreferenceSubsystem::GetOriginCoordinate() const
{
	FRWScopeLock Lock(SnapshotLock, SLT_ReadOnly);
	return FGeoCoordinate::FromGeodetic(Snapshot.Georeference.GetOriginGeodetic());
}

// ============================================================================
//  Registro dei componenti
// ============================================================================

void UGeoreferenceSubsystem::RegisterGeoComponent(UGeoTransformComponent* Component)
{
	if (!IsValid(Component))
	{
		return;
	}

	RegisteredComponents.AddUnique(Component);
	Component->RefreshFromGeoreference();
}

void UGeoreferenceSubsystem::UnregisterGeoComponent(UGeoTransformComponent* Component)
{
	RegisteredComponents.RemoveAll(
		[Component](const TWeakObjectPtr<UGeoTransformComponent>& Weak)
		{
			return !Weak.IsValid() || Weak.Get() == Component;
		});
}

// ============================================================================
//  Camera
// ============================================================================

bool UGeoreferenceSubsystem::GetActiveViewLocation(FVector& OutLocation) const
{
	const UWorld* World = GetWorld();
	if (!World)
	{
		return false;
	}

	// In gioco e in PIE: la camera vera e' quella del PlayerCameraManager, che
	// e' gia' il risultato di tutti i modificatori (camera shake, lag, ecc.).
	// Prendere la posizione del pawn darebbe un valore leggermente diverso.
	if (const APlayerController* PC = World->GetFirstPlayerController())
	{
		if (const APlayerCameraManager* CameraManager = PC->PlayerCameraManager)
		{
			OutLocation = CameraManager->GetCameraLocation();
			return true;
		}
	}

#if WITH_EDITOR
	// Nell'editor fuori dal Play non esiste nessun PlayerController: la "camera"
	// e' lo stato del viewport client, raggiungibile solo via UnrealEd.
	if (GEditor && World->WorldType == EWorldType::Editor)
	{
		if (const FViewport* Viewport = GEditor->GetActiveViewport())
		{
			if (const FEditorViewportClient* Client =
					static_cast<const FEditorViewportClient*>(Viewport->GetClient()))
			{
				OutLocation = Client->GetViewLocation();
				return true;
			}
		}
	}
#endif

	return false;
}

bool UGeoreferenceSubsystem::TeleportViewTo(const FGeodetic& Destination)
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return false;
	}

	// Il rebase PRIMA del teletrasporto: cosi' la destinazione finisce
	// nell'origine (0,0,0) e non si passa mai per coordinate enormi, nemmeno
	// per un frame.
	ApplyRebase(Destination);

	// Un po' di quota per non ritrovarsi dentro il terreno (che in Fase 1 non
	// esiste ancora, ma dalla Fase 5 si').
	const FVector TargetLocation = Snapshot.GeodeticToUnreal(Destination);

	bool bMoved = false;

	if (APlayerController* PC = World->GetFirstPlayerController())
	{
		if (AActor* ViewTarget = PC->GetViewTarget())
		{
			ViewTarget->SetActorLocation(TargetLocation, /*bSweep=*/false,
				nullptr, ETeleportType::TeleportPhysics);
			bMoved = true;
		}
	}

#if WITH_EDITOR
	if (!bMoved && GEditor && World->WorldType == EWorldType::Editor)
	{
		if (FViewport* Viewport = GEditor->GetActiveViewport())
		{
			if (FEditorViewportClient* Client =
					static_cast<FEditorViewportClient*>(Viewport->GetClient()))
			{
				Client->SetViewLocation(TargetLocation);
				Client->Invalidate();
				bMoved = true;
			}
		}
	}
#endif

	return bMoved;
}

// ============================================================================
//  Overlay di debug
// ============================================================================

void UGeoreferenceSubsystem::DrawDebugOverlay() const
{
	if (!GEngine)
	{
		return;
	}

	// SCELTA UE: usiamo AddOnScreenDebugMessage invece di un AHUD custom.
	// Un AHUD richiede che il GameMode del livello lo dichiari come HUDClass,
	// cioe' obbliga a creare e assegnare un GameMode solo per vedere due righe
	// di testo: e' esattamente il tipo di setup che fa perdere un pomeriggio a
	// chi sta imparando il motore. Questo funziona in qualunque livello, subito.
	//
	// La CHIAVE (primo parametro) e' importante: con una chiave stabile la riga
	// viene AGGIORNATA in posto; con -1 ogni frame ne aggiunge una nuova e lo
	// schermo diventa una cascata illeggibile.

	FVector ViewLocation = FVector::ZeroVector;
	const bool bHasView = GetActiveViewLocation(ViewLocation);

	const FGeoCoordinate Origin = GetOriginCoordinate();
	const double DistanceKm = bHasView ? Snapshot.DistanceFromOriginMeters(ViewLocation) / 1000.0 : 0.0;

	int32 Key = DebugMessageKeyBase;
	auto Line = [&Key](const FColor& Color, const FString& Text)
	{
		GEngine->AddOnScreenDebugMessage(Key++, 0.0f, Color, Text);
	};

	Line(FColor::Cyan,  TEXT("--- GeoWorld | Fase 1: georeferenziazione ---"));
	Line(FColor::White, FString::Printf(TEXT("Origine      : %s"), *Origin.ToDisplayString()));

	if (bHasView)
	{
		const FGeoCoordinate CameraCoord =
			FGeoCoordinate::FromGeodetic(Snapshot.UnrealToGeodetic(ViewLocation));

		Line(FColor::Green, FString::Printf(TEXT("Camera       : %s"), *CameraCoord.ToDisplayString()));
		Line(FColor::White, FString::Printf(TEXT("Unreal       : %.0f, %.0f, %.0f uu"),
			ViewLocation.X, ViewLocation.Y, ViewLocation.Z));
		Line(DistanceKm > (RebaseThresholdMeters / 1000.0) * 0.8 ? FColor::Yellow : FColor::White,
			FString::Printf(TEXT("Dist origine : %.3f km   (soglia %.1f km)"),
				DistanceKm, RebaseThresholdMeters / 1000.0));
	}
	else
	{
		Line(FColor::Red, TEXT("Camera       : non disponibile"));
	}

	Line(FColor::White, FString::Printf(TEXT("Rebase       : %u   (auto: %s)"),
		RebaseCount, bAutoRebase ? TEXT("on") : TEXT("off")));
	Line(FColor::White, FString::Printf(TEXT("Componenti   : %d registrati"), RegisteredComponents.Num()));
	Line(FColor::White, FString::Printf(TEXT("Generation   : %u"), Snapshot.Generation));

	// Terna di assi nell'origine: X rosso (Nord), Y verde (Est), Z blu (Alto).
	// Lunghezza 1 km in unita' Unreal.
	if (const UWorld* World = GetWorld())
	{
		// 1 km per asse, espresso passando dal punto unico di conversione.
		const double AxisLengthUu = 1000.0 * GeoWorld::Units::MetersToUu;
		DrawDebugLine(World, FVector::ZeroVector, FVector(AxisLengthUu, 0, 0), FColor::Red,   false, -1.f, 0, 200.f);
		DrawDebugLine(World, FVector::ZeroVector, FVector(0, AxisLengthUu, 0), FColor::Green, false, -1.f, 0, 200.f);
		DrawDebugLine(World, FVector::ZeroVector, FVector(0, 0, AxisLengthUu), FColor::Blue,  false, -1.f, 0, 200.f);
	}
}

#include "Terrain/GeoTerrainSubsystem.h"

#include "GeoCoreModule.h"
#include "Georeference/GeoreferenceSubsystem.h"
#include "Lod/GeoQuadtreeSubsystem.h"
#include "Streaming/GeoTileStreamingSubsystem.h"
#include "Terrain/DynamicMeshTerrainProvider.h"
#include "Geo/GeoUnits.h"

#include "DrawDebugHelpers.h"
#include "Engine/Engine.h"
#include "Engine/World.h"

using namespace GeoWorld;
using GeoWorld::Tiles::FTileKey;

bool UGeoTerrainSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	if (!Super::ShouldCreateSubsystem(Outer)) { return false; }
	const UWorld* World = Cast<UWorld>(Outer);
	return World && (World->WorldType == EWorldType::Game
	              || World->WorldType == EWorldType::PIE
	              || World->WorldType == EWorldType::Editor);
}

void UGeoTerrainSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	Collection.InitializeDependency<UGeoTileStreamingSubsystem>();
	Collection.InitializeDependency<UGeoQuadtreeSubsystem>();

	UWorld* World = GetWorld();
	Streaming = World->GetSubsystem<UGeoTileStreamingSubsystem>();
	Quadtree = World->GetSubsystem<UGeoQuadtreeSubsystem>();

	// Il rebasing non tocca i vertici: si limita a ricalcolare le
	// trasformazioni dei componenti. E' il guadagno del frame locale alla tile,
	// e qui e' una riga.
	if (UGeoreferenceSubsystem* Georeference = World->GetSubsystem<UGeoreferenceSubsystem>())
	{
		RebaseHandle = Georeference->OnGeoreferenceRebased.AddUObject(
			this, &UGeoTerrainSubsystem::OnGeoreferenceRebased);
	}
}

void UGeoTerrainSubsystem::Deinitialize()
{
	if (UWorld* World = GetWorld())
	{
		if (UGeoreferenceSubsystem* Georeference = World->GetSubsystem<UGeoreferenceSubsystem>())
		{
			Georeference->OnGeoreferenceRebased.Remove(RebaseHandle);
		}
	}

	// La funzione registrata nel quadtree cattura "this": va tolta prima che
	// questo oggetto sparisca, altrimenti il quadtree chiamerebbe un morto.
	if (Quadtree) { Quadtree->ClearRenderReadiness(); }

	if (Provider.IsValid()) { Provider->Shutdown(); }
	Provider.Reset();
	BuiltTiles.Empty();
	PlanMeshKeys.Empty();

	Super::Deinitialize();
}

TStatId UGeoTerrainSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UGeoTerrainSubsystem, STATGROUP_Tickables);
}

void UGeoTerrainSubsystem::SetEnabled(bool bInEnabled)
{
	if (bTerrainEnabled == bInEnabled) { return; }
	bTerrainEnabled = bInEnabled;

	if (bTerrainEnabled)
	{
		if (!Provider.IsValid())
		{
			Provider = MakeUnique<FDynamicMeshTerrainProvider>();
			Provider->Initialize(GetWorld());
		}
		// Il quadtree deve girare: senza selezione non c'e' niente da costruire.
		if (Quadtree)
		{
			Quadtree->SetEnabled(true);

			// Da qui in poi "pronta per il disegno" vuol dire "ha la mesh":
			// vedi UGeoQuadtreeSubsystem::SetRenderReadiness. La lambda cattura
			// this; la si toglie in Deinitialize e allo spegnimento.
			Quadtree->SetRenderReadiness([this](const FTileKey& Key)
			{
				return BuiltTiles.Contains(Key.Pack());
			});
		}
		bWarmingUp = true;
	}
	else
	{
		if (Quadtree) { Quadtree->ClearRenderReadiness(); }
		RebuildAll();
	}
}

void UGeoTerrainSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	if (bTerrainEnabled) { SynchroniseWithSelection(); }
	if (bShowDebugOverlay) { DrawDebugOverlay(); }
	if (bDrawBounds) { DrawTileBounds(); }
}

void UGeoTerrainSubsystem::SynchroniseWithSelection()
{
	if (!Provider.IsValid() || !Quadtree || !Streaming) { return; }

	UGeoreferenceSubsystem* Georeference = GetWorld()->GetSubsystem<UGeoreferenceSubsystem>();
	if (!Georeference) { return; }

	const FGeoreferenceSnapshot Snapshot = Georeference->GetSnapshot();
	const TArray<Quadtree::FSelectedTile>& Selected = Quadtree->GetSelectedTiles();
	const double Now = FPlatformTime::Seconds();

	Stats.CostruiteQuestoFrame = 0;
	Stats.RimosseQuestoFrame = 0;
	Stats.PrecostruiteQuestoFrame = 0;
	Stats.SfrattateQuestoFrame = 0;

	RefreshPlanSet();

	// Un teletrasporto rimette il terreno in riscaldamento: tutto cio' che
	// serve e' nuovo, conviene costruirlo in fretta una volta sola.
	if (Quadtree->GetTeleportCount() != TeleportsSeen)
	{
		TeleportsSeen = Quadtree->GetTeleportCount();
		bWarmingUp = true;
	}

	// --- 1. Cosa va A SCHERMO: la selezione del quadtree ----------------------
	//
	// La selezione e' vista-indipendente e usa "ha la mesh" come criterio
	// (SetRenderReadiness), quindi contiene solo tile gia' costruite: qui non
	// si costruisce niente, si decide solo cosa mostrare.
	TSet<uint64> Visible;
	Visible.Reserve(Selected.Num());
	for (const Quadtree::FSelectedTile& Tile : Selected) { Visible.Add(Tile.Key.Pack()); }

	for (TPair<uint64, FBuiltTile>& Pair : BuiltTiles)
	{
		const bool bShouldShow = Visible.Contains(Pair.Key);
		SetBuiltVisible(Pair.Value, bShouldShow);

		if (bShouldShow || PlanMeshKeys.Contains(Pair.Key))
		{
			Pair.Value.LastWantedSeconds = Now;
		}
	}

	// Se il quadtree NON ha la readiness registrata (terreno appena acceso,
	// o chi usa questo subsystem senza) la selezione puo' contenere tile senza
	// mesh: vanno costruite subito e visibili, come nella Fase 5.
	TArray<FTileKey> Urgent;
	for (const Quadtree::FSelectedTile& Tile : Selected)
	{
		if (!BuiltTiles.Contains(Tile.Key.Pack())) { Urgent.Add(Tile.Key); }
	}
	// Le tile che il disegno aspetta per poter scendere di livello: il padre
	// resta a schermo finche' non sono pronte TUTTE, quindi sono le piu' urgenti.
	for (const Quadtree::FTileRequest& Request : Quadtree->GetRenderRequests())
	{
		if (!BuiltTiles.Contains(Request.Key.Pack())) { Urgent.Add(Request.Key); }
	}

	// --- 2. Fare spazio: via le mesh nascoste che nessuno vuole piu' ------------
	EvictHiddenMeshes(Visible);

	// --- 3. Riscaldamento --------------------------------------------------------
	//
	// Si misura sulla fascia "adesso" del piano: quanta parte di cio' che
	// servirebbe qui ha gia' la mesh. Con isteresi: si entra sotto il 60%,
	// si esce sopra il 95%, cosi' non si oscilla attorno a una soglia unica.
	const Quadtree::FResidencyPlan& Plan = Quadtree->GetResidencyPlan();
	int32 ReadyNow = 0, ReadyPredicted = 0, CountPredicted = 0;
	for (const Quadtree::FResidencyEntry& Entry : Plan.Entries)
	{
		const bool bBuilt = BuiltTiles.Contains(Entry.Key.Pack());
		if (Entry.Tier == 0) { ReadyNow += bBuilt ? 1 : 0; }
		else if (Entry.Tier <= Plan.PredictionTiers) { ++CountPredicted; ReadyPredicted += bBuilt ? 1 : 0; }
	}
	const double ReadyFraction = (Plan.CountNow > 0)
		? static_cast<double>(ReadyNow) / Plan.CountNow : 1.0;

	if (WarmupTilesPerFrame <= 0) { bWarmingUp = false; }
	else if (ReadyFraction < 0.60) { bWarmingUp = true; }
	else if (ReadyFraction >= 0.95) { bWarmingUp = false; }

	// --- 4. Costruire, in ordine di urgenza, entro il budget del frame ---------
	int32 Budget = bWarmingUp ? FMath::Max(MaxTilesPerFrame, WarmupTilesPerFrame) : MaxTilesPerFrame;
	int32 Waiting = 0;

	// 4a. Urgenti: servono al disegno adesso. Nessun tetto di memoria: una
	// tile che manca a schermo e' un buco, e un buco e' peggio di un budget
	// sforato.
	for (const FTileKey& Key : Urgent)
	{
		if (BuiltTiles.Contains(Key.Pack())) { continue; }   // doppione fra le due liste
		if (Budget <= 0) { ++Waiting; continue; }
		if (!Streaming->FindLoadedTile(Key)) { ++Waiting; continue; }
		// Le tile della selezione vanno mostrate subito; quelle attese dal
		// padre restano nascoste finche' il quadtree non scende su di loro.
		if (BuildTile(Key, Snapshot, Visible.Contains(Key.Pack()))) { ++Stats.CostruiteQuestoFrame; --Budget; }
	}

	// 4b. Il piano: prima "adesso", poi le posizioni previste, in ordine. Qui
	// si' che vale il budget di memoria: sono costruzioni in anticipo, e se
	// non c'e' posto e' meglio aspettare che buttare via qualcosa che serve.
	for (const Quadtree::FResidencyEntry& Entry : Plan.Entries)
	{
		if (Budget <= 0) { break; }
		if (Entry.Tier > Plan.PredictionTiers) { break; }   // la sicurezza resta in RAM
		if (BuiltTiles.Num() >= MeshBudget) { break; }

		const uint64 Packed = Entry.Key.Pack();
		if (BuiltTiles.Contains(Packed)) { continue; }
		if (!Streaming->FindLoadedTile(Entry.Key)) { continue; }   // le quote arriveranno

		if (BuildTile(Entry.Key, Snapshot, /*bVisible=*/false))
		{
			++Stats.PrecostruiteQuestoFrame;
			--Budget;
		}
	}

	// --- Statistiche -------------------------------------------------------------
	Stats.TileInAttesa = Waiting;
	Stats.TileConGeometria = Provider->GetTileCount();
	Stats.InRiscaldamento = bWarmingUp;
	Stats.PronteAdesso = ReadyNow;
	Stats.PianoAdesso = Plan.CountNow;
	Stats.ProntePreviste = ReadyPredicted;
	Stats.PianoPreviste = CountPredicted;

	Stats.MeshVisibili = 0;
	Stats.TriangoliCostruiti = 0;
	for (const TPair<uint64, FBuiltTile>& Pair : BuiltTiles)
	{
		Stats.MeshVisibili += Pair.Value.bVisible ? 1 : 0;
		Stats.TriangoliCostruiti += Pair.Value.TriangleCount;
	}
	Stats.MeshNascoste = BuiltTiles.Num() - Stats.MeshVisibili;

	// I triangoli che ho COSTRUITO e quelli che il renderer ha DAVVERO sono due
	// numeri diversi, e la differenza e' esattamente cio' che serve sapere
	// quando "i conteggi ci sono ma non si vede niente". La prima versione
	// mostrava solo il primo dei due, cioe' la mia intenzione: inutile.
	// (Entrambi contano anche le mesh nascoste: esistono, occupano memoria.)
	Stats.TriangoliTotali = Provider->GetRealizedTriangleCount();

	// Stima: posizioni, normali e UV in float piu' gli indici.
	Stats.MemoriaGeometriaMB = static_cast<float>(
		Stats.TriangoliTotali * (3.0 * 4.0) / (1024.0 * 1024.0));
	Stats.TempoCostruzioneMediaMs = (BuildCount > 0)
		? static_cast<float>(TotalBuildSeconds / BuildCount * 1000.0) : 0.0f;
}

void UGeoTerrainSubsystem::RefreshPlanSet()
{
	const int32 Generation = Quadtree->GetPlanGeneration();
	if (Generation == PlanGenerationSeen) { return; }
	PlanGenerationSeen = Generation;

	const Quadtree::FResidencyPlan& Plan = Quadtree->GetResidencyPlan();
	PlanMeshKeys.Reset();
	for (const Quadtree::FResidencyEntry& Entry : Plan.Entries)
	{
		if (Entry.Tier > Plan.PredictionTiers) { break; }   // in ordine di fascia
		PlanMeshKeys.Add(Entry.Key.Pack());
	}
}

bool UGeoTerrainSubsystem::BuildTile(const FTileKey& Key, const FGeoreferenceSnapshot& Snapshot, bool bVisible)
{
	const auto TileData = Streaming->FindLoadedTile(Key);
	if (!TileData) { return false; }

	const double Started = FPlatformTime::Seconds();

	Mesh::FTileMeshData MeshData;
	Mesh::BuildTileMesh(*TileData, MeshParameters, MeshData);
	if (!MeshData.IsValid()) { return false; }

	// La trasformazione porta il frame locale della tile nello spazio di
	// Unreal. La SCALA fa la conversione metri -> unita': i vertici restano
	// in metri, quindi piccoli, e la regola del punto unico di conversione
	// resta rispettata.
	FTransform Transform = Snapshot.GetLocalNeuTransform(MeshData.Origin);
	Transform.SetScale3D(FVector(GeoWorld::Units::MetersToUu));

	if (!Provider->CreateOrUpdateTile(Key, MeshData, Transform)) { return false; }

	FBuiltTile& Built = BuiltTiles.Add(Key.Pack(),
		FBuiltTile{ Key, static_cast<int32>(MeshData.TriangleCount), /*bVisible=*/true, FPlatformTime::Seconds() });

	// Il provider crea i componenti visibili. Una mesh costruita in anticipo
	// va nascosta subito, nello stesso frame: il renderer non la vedra' mai.
	SetBuiltVisible(Built, bVisible);
	if (bVisible) { Streaming->SetTilePinned(Key, true); }

	TotalBuildSeconds += FPlatformTime::Seconds() - Started;
	++BuildCount;
	return true;
}

void UGeoTerrainSubsystem::SetBuiltVisible(FBuiltTile& Built, bool bVisible)
{
	if (Built.bVisible == bVisible) { return; }
	Built.bVisible = bVisible;
	Provider->SetTileVisible(Built.Key, bVisible);

	// Le quote di una tile a schermo restano pinnate in cache: e' l'uso che la
	// Fase 3 mostra come "pinnate" nel suo overlay. Una mesh nascosta non ha
	// bisogno delle sue quote (la geometria c'e' gia'), quindi si spinnano.
	Streaming->SetTilePinned(Built.Key, bVisible);
}

void UGeoTerrainSubsystem::EvictHiddenMeshes(const TSet<uint64>& Visible)
{
	if (BuiltTiles.Num() <= MeshBudget) { return; }

	// Candidate: nascoste e non volute dal piano. Le piu' vecchie per prime.
	TArray<TPair<double, uint64>> Candidates;
	for (const TPair<uint64, FBuiltTile>& Pair : BuiltTiles)
	{
		if (Pair.Value.bVisible || Visible.Contains(Pair.Key)) { continue; }
		if (PlanMeshKeys.Contains(Pair.Key)) { continue; }
		Candidates.Emplace(Pair.Value.LastWantedSeconds, Pair.Key);
	}
	Candidates.Sort([](const TPair<double, uint64>& A, const TPair<double, uint64>& B)
	{
		return A.Key < B.Key;
	});

	for (const TPair<double, uint64>& Candidate : Candidates)
	{
		if (BuiltTiles.Num() <= MeshBudget) { break; }

		FBuiltTile Removed;
		if (BuiltTiles.RemoveAndCopyValue(Candidate.Value, Removed))
		{
			Provider->RemoveTile(Removed.Key);
			++Stats.SfrattateQuestoFrame;
			++Stats.RimosseQuestoFrame;
		}
	}
}

void UGeoTerrainSubsystem::OnGeoreferenceRebased(const FGeoreferenceSnapshot& Snapshot)
{
	++Stats.Rebase;
	if (Provider.IsValid()) { Provider->RefreshTransforms(Snapshot); }
}

void UGeoTerrainSubsystem::SetSkirtEnabled(bool bInSkirt)
{
	if (MeshParameters.bGenerateSkirt == bInSkirt) { return; }
	MeshParameters.bGenerateSkirt = bInSkirt;
	RebuildAll();
}

void UGeoTerrainSubsystem::SetFlipWinding(bool bInFlip)
{
	if (MeshParameters.bFlipWinding == bInFlip) { return; }
	MeshParameters.bFlipWinding = bInFlip;
	RebuildAll();
}

void UGeoTerrainSubsystem::SetWireframe(bool bInWireframe)
{
	if (Provider.IsValid()) { Provider->SetWireframe(bInWireframe); }
}

void UGeoTerrainSubsystem::RebuildAll()
{
	if (!Provider.IsValid()) { return; }

	// Spinnare tutto prima di buttare: altrimenti le tile resterebbero
	// inamovibili nella cache pur non essendo piu' disegnate.
	if (Streaming)
	{
		for (const TPair<uint64, FBuiltTile>& Pair : BuiltTiles)
		{
			Streaming->SetTilePinned(Pair.Value.Key, false);
		}
	}

	Provider->RemoveAllTiles();
	BuiltTiles.Empty();

	// Il piano non e' cambiato, ma le mesh del piano sono sparite: si
	// ricostruisce tutto come dopo un teletrasporto.
	bWarmingUp = true;
}

void UGeoTerrainSubsystem::DrawDebugOverlay()
{
	if (!GEngine) { return; }

	int32 Key = 0x6E90;
	auto Line = [&Key](const FColor& Colour, const FString& Text)
	{
		GEngine->AddOnScreenDebugMessage(Key++, 0.0f, Colour, Text);
	};

	Line(FColor::Cyan, TEXT("--- GeoWorld | Fase 5: terreno ---"));

	if (!bTerrainEnabled)
	{
		Line(FColor::Yellow, TEXT("Terreno spento. Attiva con: geo.Terrain.Enable 1"));
		return;
	}

	Line(FColor::White, FString::Printf(TEXT("Provider      : %s%s"),
		*GetProviderName(), IsWireframe() ? TEXT("   [wireframe]") : TEXT("")));

	// Verde solo se il renderer ha davvero i triangoli che credo di aver
	// costruito. Rosso se li ha persi per strada: in quel caso il problema e'
	// nel provider, non nella generazione della mesh.
	const bool bGeometriaArrivata = (Stats.TriangoliTotali == Stats.TriangoliCostruiti);
	const FString Discrepanza = bGeometriaArrivata
		? FString()
		: FString::Printf(TEXT("  (ne ho costruiti %d: geo.Terrain.Diag)"), Stats.TriangoliCostruiti);

	Line(bGeometriaArrivata ? FColor::Green : FColor::Red,
		FString::Printf(TEXT("Mesh          : %d a schermo + %d nascoste = %d / %d   triangoli nel renderer %d%s"),
			Stats.MeshVisibili, Stats.MeshNascoste, Stats.TileConGeometria, MeshBudget,
			Stats.TriangoliTotali, *Discrepanza));

	// "Pronte" = hanno gia' la mesh. Adesso sotto il 100% vuol dire che si
	// sta aspettando; previste basse in volo vuol dire che la costruzione non
	// sta al passo con la velocita' (alza geo.Terrain.Budget o la previsione).
	auto Percent = [](int32 Part, int32 Whole)
	{
		return Whole > 0 ? 100.0f * static_cast<float>(Part) / static_cast<float>(Whole) : 100.0f;   // non-unita: frazione -> percentuale
	};
	const float NowPct = Percent(Stats.PronteAdesso, Stats.PianoAdesso);
	Line(NowPct < 99.0f ? FColor::Yellow : FColor::Green, FString::Printf(
		TEXT("Pronte (mesh) : adesso %d/%d (%.0f%%)   previste %d/%d (%.0f%%)"),
		Stats.PronteAdesso, Stats.PianoAdesso, NowPct,
		Stats.ProntePreviste, Stats.PianoPreviste, Percent(Stats.ProntePreviste, Stats.PianoPreviste)));

	if (Stats.InRiscaldamento)
	{
		Line(FColor::Orange, FString::Printf(
			TEXT("RISCALDAMENTO : %d mesh per frame finche' non e' pronto il 95%% (geo.Terrain.Warmup)"),
			FMath::Max(MaxTilesPerFrame, WarmupTilesPerFrame)));
	}

	// Se molte tile restano in attesa frame dopo frame, il budget non basta per
	// quanto in fretta ci si muove: il terreno si riempie in ritardo.
	Line(Stats.TileInAttesa > 0 ? FColor::Yellow : FColor::White,
		FString::Printf(TEXT("In attesa     : %d   (budget %d per frame)"),
			Stats.TileInAttesa, MaxTilesPerFrame));

	Line(FColor::White, FString::Printf(TEXT("Questo frame  : +%d a schermo  +%d in anticipo  -%d (sfrattate %d)"),
		Stats.CostruiteQuestoFrame, Stats.PrecostruiteQuestoFrame,
		Stats.RimosseQuestoFrame, Stats.SfrattateQuestoFrame));

	Line(Stats.TempoCostruzioneMediaMs > 8.0f ? FColor::Yellow : FColor::White,
		FString::Printf(TEXT("Costruzione   : %.2f ms per tile"), Stats.TempoCostruzioneMediaMs));

	Line(FColor::White, FString::Printf(TEXT("Gonne         : %s   rebase gestiti: %d"),
		IsSkirtEnabled() ? TEXT("on") : TEXT("OFF"), Stats.Rebase));
}

// ---------------------------------------------------------------------------
//  Le scatole di debug: un canale di disegno INDIPENDENTE dalla mesh.
//
//  Non passano per il materiale, non hanno faccia frontale da orientare e non
//  vengono attenuate dalla nebbia atmosferica come la geometria opaca. Servono
//  a separare due domande che altrimenti restano confuse: "la geometria e' dove
//  credo?" e "la geometria si vede?".
//
//  Si disegnano i bounds letti dal COMPONENTE, non quelli che ho calcolato io:
//  sono esattamente il volume che il renderer usa per decidere se la primitiva
//  e' nel frustum.
// ---------------------------------------------------------------------------
void UGeoTerrainSubsystem::DrawTileBounds()
{
	UWorld* World = GetWorld();
	if (!World || !Provider.IsValid()) { return; }

	TArray<FGeoTerrainTileDiagnostic> Diagnostics;
	Provider->GetDiagnostics(Diagnostics, 4096);

	for (const FGeoTerrainTileDiagnostic& Tile : Diagnostics)
	{
		// Le mesh nascoste (costruite in anticipo, o tenute dopo l'uso) non si
		// disegnano: le scatole devono mostrare cio' che il renderer disegna.
		if (!Tile.bVisible) { continue; }

		// Un colore per livello, come nella tassellatura della Fase 4, cosi' i
		// due disegni si possono confrontare a colpo d'occhio.
		static const FColor LevelColours[] = {
			FColor::Red, FColor::Green, FColor::Blue, FColor::Yellow,
			FColor::Cyan, FColor::Magenta, FColor::Orange, FColor::Turquoise
		};
		const FColor Colour = LevelColours[Tile.Key.Level % UE_ARRAY_COUNT(LevelColours)];

		// LifeTime 0 = un solo frame: si ridisegnano a ogni tick, quindi
		// seguono il rebasing senza doverli cancellare.
		DrawDebugBox(World, Tile.BoundsOrigin, Tile.BoundsExtent, Colour,
		             /*bPersistentLines=*/false, /*LifeTime=*/0.0f,
		             /*DepthPriority=*/0, /*Thickness=*/200.0f);
	}
}

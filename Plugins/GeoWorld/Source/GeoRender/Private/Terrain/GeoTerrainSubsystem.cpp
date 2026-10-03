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

	BuildQueue = MakeShared<FMeshBuildQueue, ESPMode::ThreadSafe>();

	// Un post ogni due: un quarto dei triangoli per tile, con il LOD che lo sa
	// (SetGeometricErrorScale in SetEnabled). Il default dello strato puro resta
	// 1, la geometria completa, che e' quella che i test verificano.
	MeshParameters.Step = 2;

	// Budget di mesh in base alla RAM. Una mesh costa qualche MB fra la copia
	// sul game thread (la FDynamicMesh3, con la topologia) e i buffer della
	// scheda video. Con 16 GB, di cui l'editor ne usa parecchi da solo, 2.000
	// mesh avrebbero mandato la macchina a usare il file di paging.
	const uint32 MemoryGB = FPlatformMemory::GetConstants().TotalPhysicalGB;
	MeshBudget = (MemoryGB <= 16) ? 700 : (MemoryGB <= 32) ? 1500 : 3000;
	UE_LOG(LogGeoWorld, Log, TEXT("[GeoTerrain] RAM %u GB: budget di %d mesh (geo.Terrain.MeshBudget)"),
		MemoryGB, MeshBudget);

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

	// I lavori in volo eseguono codice di questo modulo. Chiudere l'editor (e
	// scaricare la DLL) mentre uno gira vorrebbe dire eseguire codice che non
	// c'e' piu': si aspetta che finiscano. Durano pochi millisecondi.
	WaitForAllBuilds();

	if (Provider.IsValid()) { Provider->Shutdown(); }
	Provider.Reset();
	BuiltTiles.Empty();
	PlanMeshKeys.Empty();
	DressPredicates.Empty();

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
			Provider->SetCastShadows(bCastShadowsWanted);
			Provider->SetTransitionSeconds(TransitionSeconds);
		}
		// Il quadtree deve girare: senza selezione non c'e' niente da costruire.
		if (Quadtree)
		{
			Quadtree->SetEnabled(true);
			Quadtree->SetGeometricErrorScale(static_cast<double>(MeshParameters.Step));

			// Da qui in poi "pronta per il disegno" vuol dire "ha la mesh":
			// vedi UGeoQuadtreeSubsystem::SetRenderReadiness. La lambda cattura
			// this; la si toglie in Deinitialize e allo spegnimento.
			Quadtree->SetRenderReadiness([this](const FTileKey& Key)
			{
				// Costruita, e (se le ortofoto sono accese) vestita: vedi
				// SetDressPredicate. Altrimenti il padre resta a schermo.
				return BuiltTiles.Contains(Key.Pack()) && IsTileDressed(Key);
			});
		}
		bWarmingUp = true;
	}
	else
	{
		if (Quadtree)
		{
			Quadtree->ClearRenderReadiness();
			Quadtree->SetGeometricErrorScale(1.0);
		}
		RebuildAll();
	}
}

void UGeoTerrainSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	if (bTerrainEnabled) { SynchroniseWithSelection(); }

	// Le transizioni avanzano a ogni frame, anche da fermi: una tile comparsa
	// un attimo prima di fermarsi deve finire di scivolare.
	if (Provider.IsValid()) { Provider->TickTransitions(FPlatformTime::Seconds()); }

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

	// Chi COMPARE adesso al posto di un antenato che era a schermo fino al
	// frame prima: e' il raffinamento, il momento del lampo. Va guardato PRIMA
	// di cambiare le visibilita', che sono proprio lo stato del frame prima.
	// Il caso inverso (le figlie che tornano padre) non si sfuma: il padre non
	// sa com'erano le figlie, e allontanandosi il cambio si nota molto meno.
	TArray<TPair<FTileKey, FTileKey>> Refinements;
	for (const TPair<uint64, FBuiltTile>& Pair : BuiltTiles)
	{
		if (Pair.Value.bVisible || !Visible.Contains(Pair.Key)) { continue; }
		FTileKey Ancestor = Pair.Value.Key;
		while (Ancestor.Level > 0)
		{
			Ancestor = Ancestor.GetParent();
			const FBuiltTile* AncestorTile = BuiltTiles.Find(Ancestor.Pack());
			if (AncestorTile && AncestorTile->bVisible)
			{
				Refinements.Emplace(Pair.Value.Key, Ancestor);
				break;
			}
		}
	}

	for (TPair<uint64, FBuiltTile>& Pair : BuiltTiles)
	{
		const bool bShouldShow = Visible.Contains(Pair.Key);
		SetBuiltVisible(Pair.Value, bShouldShow);

		if (bShouldShow || PlanMeshKeys.Contains(Pair.Key))
		{
			Pair.Value.LastWantedSeconds = Now;
		}
	}

	// Le figlie nascono con la forma, la luce e la foto del padre, e diventano
	// se stesse in qualche decimo di secondo (geo.Terrain.Morph).
	for (const TPair<FTileKey, FTileKey>& Refinement : Refinements)
	{
		Provider->BeginTransitionFromParent(Refinement.Key, Refinement.Value);
	}
	Stats.RaffinateQuestoFrame = Refinements.Num();
	Stats.InTransizione = Provider->GetTransitionCount();

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

	// --- 4. Consegnare le mesh pronte, entro il budget del frame --------------
	//
	// Il budget per frame ora vale per la CONSEGNA (SetMesh sul componente e,
	// a fine frame, la creazione del proxy di scena): la costruzione vera sta
	// sui thread di lavoro e non pesa sul frame.
	const int32 CommitBudget = bWarmingUp ? FMath::Max(MaxTilesPerFrame, WarmupTilesPerFrame) : MaxTilesPerFrame;
	CommitCompletedBuilds(Snapshot, Visible, CommitBudget);

	// --- 5. Mandare in costruzione, entro il tetto dei lavori in volo ---------
	OrphanBuilds.RemoveAll([](const UE::Tasks::FTask& Task) { return Task.IsCompleted(); });

	const int32 MaxInFlight = bWarmingUp ? BuildsInFlight * 2 : BuildsInFlight;
	int32 Waiting = 0;

	// 5a. Urgenti: servono al disegno adesso. Nessun tetto di memoria: una
	// tile che manca a schermo e' un buco, e un buco e' peggio di un budget
	// sforato.
	for (const FTileKey& Key : Urgent)
	{
		const uint64 Packed = Key.Pack();
		if (BuiltTiles.Contains(Packed) || InFlightBuilds.Contains(Packed)) { continue; }
		if (InFlightBuilds.Num() >= MaxInFlight) { ++Waiting; continue; }
		if (!DispatchBuild(Key, /*bUrgent=*/true)) { ++Waiting; }
	}

	// 5b. Il piano: prima "adesso", poi le posizioni previste, in ordine. Qui
	// si' che vale il budget di memoria: sono costruzioni in anticipo, e se
	// non c'e' posto e' meglio aspettare che buttare via qualcosa che serve.
	for (const Quadtree::FResidencyEntry& Entry : Plan.Entries)
	{
		if (InFlightBuilds.Num() >= MaxInFlight) { break; }
		if (Entry.Tier > Plan.PredictionTiers) { break; }   // la sicurezza resta in RAM
		if (BuiltTiles.Num() + InFlightBuilds.Num() >= MeshBudget) { break; }

		const uint64 Packed = Entry.Key.Pack();
		if (BuiltTiles.Contains(Packed) || InFlightBuilds.Contains(Packed)) { continue; }
		DispatchBuild(Entry.Key, /*bUrgent=*/false);   // senza quote torna false: arriveranno
	}

	// --- Statistiche -------------------------------------------------------------
	Stats.TileInAttesa = Waiting;
	Stats.InCostruzione = InFlightBuilds.Num();
	Stats.OmbreAccese = Provider->IsCastingShadows();
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

	// Stima della RAM, per triangolo, di una FDynamicMesh3: vertici in double,
	// topologia (spigoli e loro riferimenti), sovrapposizioni di normali e UV.
	// Fatti i conti struttura per struttura sono ~125 byte. La versione
	// precedente contava 12 byte (solo le posizioni in float): dieci volte meno
	// del vero, e il numero sull'overlay non spiegava la RAM "alle stelle".
	constexpr double BytesPerTriangle = 125.0;
	Stats.MemoriaGeometriaMB = static_cast<float>(
		Stats.TriangoliTotali * BytesPerTriangle / (1024.0 * 1024.0));
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

bool UGeoTerrainSubsystem::DispatchBuild(const FTileKey& Key, bool bUrgent)
{
	// Le quote devono essere in RAM: il lavoro ne prende un riferimento
	// condiviso (shared_ptr a una tile immutabile), quindi la cache puo'
	// sfrattarle nel frattempo senza che il lavoro se ne accorga.
	GeoWorld::Tiles::FTileCache::FTilePtr TileData = Streaming->FindLoadedTile(Key);
	if (!TileData || !Provider.IsValid() || !BuildQueue.IsValid()) { return false; }

	// Le quote del PADRE, per il geomorphing: la tile nasce con la forma che
	// il padre aveva a schermo. Di solito ci sono (il padre e' a schermo, le
	// sue quote sono pinnate); se mancano, la tile compare senza scivolare.
	GeoWorld::Tiles::FTileCache::FTilePtr ParentData =
		(Key.Level > 0) ? Streaming->FindLoadedTile(Key.GetParent()) : nullptr;

	// Tutto cio' che il lavoro usa viene COPIATO nella lambda: parametri,
	// generazione, funzione di preparazione, coda. Niente "this": il lavoro
	// non deve poter toccare il subsystem, che vive sul game thread.
	const Mesh::FTileMeshParameters Parameters = MeshParameters;
	const FGeoPrepareTileMeshFunction Prepare = Provider->GetPrepareFunction();
	const int32 Generation = BuildGeneration;
	TSharedPtr<FMeshBuildQueue, ESPMode::ThreadSafe> Queue = BuildQueue;

	// ------------------------------------------------------------------
	//  NOTA UE: UE::Tasks::Launch mette il lavoro nel task graph del motore,
	//  che lo esegue su uno dei suoi thread. Qui e' la scelta giusta, mentre
	//  per le letture da disco (Fase 3) avevamo scelto un pool nostro: il
	//  task graph e' fatto per lavoro che CALCOLA, e costruire una mesh e'
	//  calcolo puro, senza attese. Priorita' "Background": non deve rubare i
	//  thread a chi prepara il frame (animazioni, fisica, rendering).
	// ------------------------------------------------------------------
	UE::Tasks::FTask Task = UE::Tasks::Launch(TEXT("GeoTerrainMeshBuild"),
		[TileData, ParentData, Parameters, Prepare, Generation, Key, Queue]()
		{
			const double Started = FPlatformTime::Seconds();

			FMeshBuildResult Result;
			Result.Key = Key;
			Result.Generation = Generation;

			Mesh::FTileMeshData MeshData;
			Mesh::BuildTileMesh(*TileData, Parameters, MeshData);
			if (ParentData && MeshData.IsValid())
			{
				Mesh::ComputeMorphTargets(*TileData, *ParentData, Parameters.Step, MeshData);
			}
			if (MeshData.IsValid() && Prepare)
			{
				Result.Prepared = Prepare(MeshData);
			}

			Result.Seconds = FPlatformTime::Seconds() - Started;
			Queue->Completed.Enqueue(MoveTemp(Result));
		},
		UE::Tasks::ETaskPriority::BackgroundNormal);

	InFlightBuilds.Add(Key.Pack(), FInFlightBuild{ Task, bUrgent });
	return true;
}

void UGeoTerrainSubsystem::CommitCompletedBuilds(const FGeoreferenceSnapshot& Snapshot,
                                                 const TSet<uint64>& Visible, int32 Budget)
{
	Stats.ConsegnaMsQuestoFrame = 0.0f;
	if (!BuildQueue.IsValid()) { return; }

	const double Started = FPlatformTime::Seconds();
	int32 Committed = 0;

	FMeshBuildResult Result;
	while (Committed < Budget && BuildQueue->Completed.Dequeue(Result))
	{
		const uint64 Packed = Result.Key.Pack();

		bool bUrgent = false;
		if (const FInFlightBuild* InFlight = InFlightBuilds.Find(Packed))
		{
			bUrgent = InFlight->bUrgent;
		}

		// Un risultato di una generazione vecchia e' orfano: la sua voce in
		// InFlightBuilds, se c'e', appartiene a un lavoro nuovo per la stessa
		// tile, e non va toccata.
		if (Result.Generation != BuildGeneration) { continue; }
		InFlightBuilds.Remove(Packed);

		if (!Result.Prepared.IsValid() || BuiltTiles.Contains(Packed)) { continue; }

		// La trasformazione si calcola ADESSO, non quando il lavoro e' partito:
		// se nel frattempo c'e' stato un rebase, l'origine e' cambiata. I vertici
		// invece sono nel frame locale della tile e non dipendono dall'origine.
		FTransform Transform = Snapshot.GetLocalNeuTransform(Result.Prepared->Origin);
		Transform.SetScale3D(FVector(GeoWorld::Units::MetersToUu));

		const int32 Triangles = Result.Prepared->TriangleCount;
		if (!Provider->CommitPreparedTile(Result.Key, *Result.Prepared, Transform)) { continue; }

		FBuiltTile& Built = BuiltTiles.Add(Packed,
			FBuiltTile{ Result.Key, Triangles, /*bVisible=*/true, FPlatformTime::Seconds() });

		// Il provider crea i componenti visibili. Una mesh costruita in anticipo
		// va nascosta subito, nello stesso frame: il renderer non la vedra' mai.
		const bool bShow = Visible.Contains(Packed);
		SetBuiltVisible(Built, bShow);
		if (bShow) { Streaming->SetTilePinned(Result.Key, true); }

		TotalBuildSeconds += Result.Seconds;
		++BuildCount;
		++Committed;
		if (bUrgent) { ++Stats.CostruiteQuestoFrame; } else { ++Stats.PrecostruiteQuestoFrame; }
	}

	Stats.ConsegnaMsQuestoFrame = static_cast<float>((FPlatformTime::Seconds() - Started) * 1000.0);
}

void UGeoTerrainSubsystem::WaitForAllBuilds()
{
	for (TPair<uint64, FInFlightBuild>& Pair : InFlightBuilds) { Pair.Value.Task.Wait(); }
	for (UE::Tasks::FTask& Task : OrphanBuilds) { Task.Wait(); }
	InFlightBuilds.Empty();
	OrphanBuilds.Empty();

	// I risultati rimasti in coda non servono piu': si buttano.
	if (BuildQueue.IsValid())
	{
		FMeshBuildResult Discarded;
		while (BuildQueue->Completed.Dequeue(Discarded)) {}
	}
}

void UGeoTerrainSubsystem::SetCastShadows(bool bInCastShadows)
{
	bCastShadowsWanted = bInCastShadows;
	if (Provider.IsValid()) { Provider->SetCastShadows(bInCastShadows); }
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

void UGeoTerrainSubsystem::SetMeshStep(int32 InStep)
{
	// Solo divisori di 128 che abbiano senso: 1, 2, 4, 8.
	const int32 Valid = (InStep == 1 || InStep == 2 || InStep == 4 || InStep == 8) ? InStep : 2;
	if (MeshParameters.Step == Valid) { return; }
	MeshParameters.Step = Valid;

	// Il LOD deve sapere che una mesh col passo 2 sbaglia il doppio: altrimenti
	// sceglierebbe le stesse tile di prima, con un quarto del dettaglio.
	if (Quadtree && bTerrainEnabled) { Quadtree->SetGeometricErrorScale(static_cast<double>(Valid)); }
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

	// I lavori in volo sono stati lanciati con i parametri di prima: si
	// abbandonano (restano da attendere solo alla chiusura) e i loro risultati
	// verranno riconosciuti come vecchi dalla generazione.
	++BuildGeneration;
	for (TPair<uint64, FInFlightBuild>& Pair : InFlightBuilds) { OrphanBuilds.Add(Pair.Value.Task); }
	InFlightBuilds.Empty();

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
		FString::Printf(TEXT("Mesh          : %d a schermo + %d nascoste = %d / %d   passo %d%s"),
			Stats.MeshVisibili, Stats.MeshNascoste, Stats.TileConGeometria, MeshBudget,
			MeshParameters.Step, *Discrepanza));

	// Il numero che serve per capire se il terreno sta chiedendo troppo: sopra i
	// 5 milioni di triangoli un portatile comincia a soffrire.
	const float MillionTriangles = static_cast<float>(Stats.TriangoliTotali) / 1.0e6f;
	Line(MillionTriangles > 5.0f ? FColor::Red : (MillionTriangles > 3.0f ? FColor::Yellow : FColor::White),
		FString::Printf(TEXT("Triangoli     : %.2f milioni in memoria, ~%.0f MB di RAM (stima)"),
			MillionTriangles, Stats.MemoriaGeometriaMB));

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

	// Due tempi diversi, su thread diversi. La costruzione (sui thread di
	// lavoro) non rallenta il frame; la consegna (sul game thread) si'. Se il
	// gioco scatta e la consegna e' alta, abbassa geo.Terrain.Budget.
	Line(FColor::White, FString::Printf(TEXT("Costruzione   : %.2f ms per tile, su thread   (%d in corso)"),
		Stats.TempoCostruzioneMediaMs, Stats.InCostruzione));
	Line(Stats.ConsegnaMsQuestoFrame > 4.0f ? FColor::Yellow : FColor::White,
		FString::Printf(TEXT("Consegna      : %.2f ms questo frame, sul game thread"), Stats.ConsegnaMsQuestoFrame));

	// Le transizioni: quante tile stanno scivolando dalla forma del padre.
	// Rosso se il materiale non le sa fare (il lampo resta).
	const FString TransitionProblem = Provider.IsValid() ? Provider->GetTransitionMaterialProblem() : FString();
	if (!TransitionProblem.IsEmpty())
	{
		Line(FColor::Red, FString::Printf(TEXT("Transizioni   : %s"), *TransitionProblem));
	}
	else
	{
		Line(FColor::White, FString::Printf(TEXT("Transizioni   : %d in corso, %.2f s ciascuna (geo.Terrain.Morph)"),
			Stats.InTransizione, TransitionSeconds));
	}

	Line(FColor::White, FString::Printf(TEXT("Gonne         : %s   ombre: %s   rebase gestiti: %d"),
		IsSkirtEnabled() ? TEXT("on") : TEXT("OFF"),
		Stats.OmbreAccese ? TEXT("ON") : TEXT("off"), Stats.Rebase));
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

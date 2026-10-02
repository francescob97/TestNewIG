#include "Roads/GeoRoadsSubsystem.h"

#include "GeoCoreModule.h"
#include "Imagery/GeoRuntimeTexture.h"
#include "Imagery/ImageryMapping.h"
#include "Lod/GeoQuadtreeSubsystem.h"
#include "Streaming/GeoTileStreamingSubsystem.h"
#include "Streaming/GeoVectorStreamingSubsystem.h"
#include "Terrain/GeoTerrainMeshProvider.h"
#include "Terrain/GeoTerrainSubsystem.h"
#include "Tiles/ImageTileFormat.h"

#include "Engine/Engine.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "HAL/PlatformMemory.h"
#include "HAL/PlatformTime.h"

using namespace GeoWorld;
using GeoWorld::Imagery::FDrapeTransform;
using GeoWorld::Tiles::FTileKey;

namespace
{
	/** Il nome con cui le strade si registrano fra i vestitori del terreno. */
	const FName RoadsDressOwnerName(TEXT("Strade"));

	bool IsPowerOfTwoSide(int32 Value)
	{
		return Value >= 64 && Value <= 2048 && (Value & (Value - 1)) == 0;
	}
}

// ============================================================================
//  Ciclo di vita
// ============================================================================

bool UGeoRoadsSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	if (!Super::ShouldCreateSubsystem(Outer)) { return false; }
	const UWorld* World = Cast<UWorld>(Outer);
	return World && (World->WorldType == EWorldType::Game
	              || World->WorldType == EWorldType::PIE
	              || World->WorldType == EWorldType::Editor);
}

void UGeoRoadsSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	Collection.InitializeDependency<UGeoVectorStreamingSubsystem>();
	Collection.InitializeDependency<UGeoTerrainSubsystem>();

	UWorld* World = GetWorld();
	Streaming = World->GetSubsystem<UGeoVectorStreamingSubsystem>();
	Terrain = World->GetSubsystem<UGeoTerrainSubsystem>();

	RasterQueue = MakeShared<FRasterQueue, ESPMode::ThreadSafe>();
	Style = Roads::MakeRoadStyle(StyleKind);

	// La memoria video non si legge in modo portabile; la RAM si', ed e' un
	// buon indizio della classe di macchina. Un portatile da 16 GB ha di norma
	// 8 GB di memoria video, di cui il motore ne usa gia' la meta'.
	const uint32 MemoryGB = FPlatformMemory::GetConstants().TotalPhysicalGB;
	if (MemoryGB <= 16)
	{
		BaseResolution = 256;  FinestResolution = 512;  VideoBudgetMB = 384;
	}
	else if (MemoryGB <= 32)
	{
		BaseResolution = 512;  FinestResolution = 1024; VideoBudgetMB = 768;
	}
	else
	{
		BaseResolution = 512;  FinestResolution = 1024; VideoBudgetMB = 1536;
	}
}

void UGeoRoadsSubsystem::Deinitialize()
{
	if (Terrain) { Terrain->ClearDressPredicate(RoadsDressOwnerName); }

	// I disegni in corso eseguono codice di questo modulo: si aspetta che
	// finiscano prima di lasciar scaricare la DLL. Durano millisecondi.
	for (TPair<uint64, UE::Tasks::FTask>& Pair : InFlight) { Pair.Value.Wait(); }
	for (UE::Tasks::FTask& Task : Orphans) { Task.Wait(); }
	InFlight.Empty();
	Orphans.Empty();

	Overlays.Empty();
	PendingUploads.Empty();
	Dressed.Empty();
	RasterQueue.Reset();

	Super::Deinitialize();
}

TStatId UGeoRoadsSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UGeoRoadsSubsystem, STATGROUP_Tickables);
}

void UGeoRoadsSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	if (bRoadsEnabled) { Synchronise(); }
	if (bShowDebugOverlay) { DrawDebugOverlay(); }
}

// ============================================================================
//  Impostazioni
// ============================================================================

void UGeoRoadsSubsystem::SetEnabled(bool bInEnabled)
{
	if (bRoadsEnabled == bInEnabled) { return; }
	bRoadsEnabled = bInEnabled;

	if (!Terrain) { return; }

	if (bRoadsEnabled)
	{
		// Una tile compare solo con le sue strade (o con quelle di un
		// antenato). La lambda cattura this: la si toglie allo spegnimento e
		// in Deinitialize.
		Terrain->SetDressPredicate(RoadsDressOwnerName,
			[this](const FTileKey& Key) { return IsDressedOrUndressable(Key); });
	}
	else
	{
		// PRIMA di togliere le strade: altrimenti per un frame nessuna tile
		// risulterebbe pronta.
		Terrain->ClearDressPredicate(RoadsDressOwnerName);
		RemoveAllOverlays();
		InvalidateAll();
	}
}

void UGeoRoadsSubsystem::SetStyle(Roads::ERoadStyle InStyle)
{
	if (StyleKind == InStyle) { return; }
	StyleKind = InStyle;
	Style = Roads::MakeRoadStyle(StyleKind);

	// Si ridisegna tutto, ma le strade vecchie restano addosso finche' non
	// arrivano le nuove: niente buchi, niente lampi.
	++Generation;
	for (TPair<uint64, UE::Tasks::FTask>& Pair : InFlight) { Orphans.Add(Pair.Value); }
	InFlight.Empty();
	PendingUploads.Empty();
}

void UGeoRoadsSubsystem::SetResolution(int32 Base, int32 Finest)
{
	if (!IsPowerOfTwoSide(Base) || !IsPowerOfTwoSide(Finest)) { return; }
	if (Base == BaseResolution && Finest == FinestResolution) { return; }
	BaseResolution = Base;
	FinestResolution = FMath::Max(Base, Finest);

	++Generation;
	for (TPair<uint64, UE::Tasks::FTask>& Pair : InFlight) { Orphans.Add(Pair.Value); }
	InFlight.Empty();
	PendingUploads.Empty();
}

void UGeoRoadsSubsystem::SetVideoBudgetMB(int32 Megabytes)
{
	VideoBudgetMB = FMath::Clamp(Megabytes, 32, 16384);
}

void UGeoRoadsSubsystem::SetStrength(float InStrength)
{
	Strength = FMath::Clamp(InStrength, 0.0f, 1.0f);
	if (Terrain)
	{
		if (IGeoTerrainMeshProvider* Provider = Terrain->GetProvider())
		{
			Provider->SetOverlayStrength(Strength);
		}
	}
}

void UGeoRoadsSubsystem::InvalidateAll()
{
	++Generation;
	for (TPair<uint64, UE::Tasks::FTask>& Pair : InFlight) { Orphans.Add(Pair.Value); }
	InFlight.Empty();
	Overlays.Empty();
	PendingUploads.Empty();
	Dressed.Empty();
}

void UGeoRoadsSubsystem::RemoveAllOverlays()
{
	if (!Terrain) { return; }
	IGeoTerrainMeshProvider* Provider = Terrain->GetProvider();
	if (!Provider) { return; }

	TArray<FTileKey> Keys;
	Terrain->GetBuiltTileKeys(Keys);
	for (const FTileKey& Key : Keys)
	{
		Provider->SetTileOverlay(Key, nullptr, FDrapeTransform{});
	}
}

int32 UGeoRoadsSubsystem::ResolutionFor(const FTileKey& TerrainKey) const
{
	// Le tile al livello piu' profondo del terreno sono quelle vicine quando si
	// vola bassi: il terreno non si affina oltre, quindi le strade devono avere
	// gia' li' tutto il dettaglio che serve.
	return (static_cast<int32>(TerrainKey.Level) >= DeepestTerrainLevel) ? FinestResolution : BaseResolution;
}

bool UGeoRoadsSubsystem::IsDressedOrUndressable(const FTileKey& Key) const
{
	if (!bRoadsEnabled || !Streaming || !Streaming->IsDatasetOpen()) { return true; }
	if (Dressed.Contains(Key.Pack())) { return true; }

	// Con un materiale senza i parametri delle strade le strade non si
	// vedrebbero comunque: aspettarle bloccherebbe il terreno per niente.
	if (Terrain)
	{
		if (const IGeoTerrainMeshProvider* Provider = Terrain->GetProvider())
		{
			if (!Provider->GetOverlayMaterialProblem().IsEmpty()) { return true; }
		}
	}

	// Niente da aspettare dove non c'e' niente da disegnare: livello troppo
	// alto per qualunque classe, o nessuna linea in quel pezzo di mondo.
	const FGeoVectorDataset& Dataset = Streaming->GetDataset();
	const int32 VectorLevel = Roads::ChooseVectorLevel(Key.Level, Dataset.GetLevelNumbers());
	if (VectorLevel < 0) { return true; }
	return !Dataset.TileExists(Imagery::AncestorOf(Key, static_cast<uint32>(VectorLevel)));
}

// ============================================================================
//  Il lavoro vero
// ============================================================================

bool UGeoRoadsSubsystem::LaunchJob(const FTileKey& TerrainKey, uint32 VectorLevel, int32 Size)
{
	const FTileKey VectorKey = Imagery::AncestorOf(TerrainKey, VectorLevel);
	const UGeoVectorStreamingSubsystem::FVectorTilePtr Tile = Streaming->FindLoadedTile(VectorKey);
	if (!Tile || !RasterQueue.IsValid()) { return false; }

	// Tutto per copia: la tile (shared_ptr, nessuna copia dei dati), la
	// richiesta, lo stile, la generazione, la coda. Niente "this": il lavoro
	// non deve toccare il subsystem, che vive sul game thread.
	const Roads::FRoadRasterRequest Request = Roads::MakeRasterRequest(TerrainKey, VectorLevel, Tile->Extent, Size);
	const Roads::FRoadStyle StyleCopy = Style;
	const int32 JobGeneration = Generation;
	TSharedPtr<FRasterQueue, ESPMode::ThreadSafe> Queue = RasterQueue;

	// NOTA UE: lo stesso task graph delle mesh del terreno (UE::Tasks::Launch),
	// con priorita' di sfondo: e' calcolo puro, senza attese, e non deve
	// rubare i thread a chi prepara il frame.
	UE::Tasks::FTask Task = UE::Tasks::Launch(TEXT("GeoRoadsRaster"),
		[Tile, Request, StyleCopy, JobGeneration, TerrainKey, Queue]()
		{
			const double Started = FPlatformTime::Seconds();

			FRasterResult Result;
			Result.Key = TerrainKey;
			Result.Generation = JobGeneration;
			Result.Size = Request.Size;
			Result.bAnything = Roads::RasterizeRoads(*Tile, Request, StyleCopy, Result.Pixels);
			if (Result.bAnything)
			{
				Tiles::BuildMipChain(Result.Pixels, Request.Size, Request.Size, Result.Mips);
			}
			else
			{
				// Niente strade in questo pezzo: non serve tenere un'immagine
				// trasparente da un megabyte.
				Result.Pixels.clear();
				Result.Pixels.shrink_to_fit();
			}
			Result.Seconds = FPlatformTime::Seconds() - Started;
			Queue->Completed.Enqueue(MoveTemp(Result));
		},
		UE::Tasks::ETaskPriority::BackgroundNormal);

	InFlight.Add(TerrainKey.Pack(), Task);
	return true;
}

void UGeoRoadsSubsystem::DrainFinishedJobs()
{
	if (!RasterQueue.IsValid()) { return; }

	FRasterResult Result;
	while (RasterQueue->Completed.Dequeue(Result))
	{
		// Un disegno di una generazione vecchia (stile o risoluzione cambiati
		// nel frattempo) non serve piu'. Il suo lavoro e' gia' fra gli orfani.
		if (Result.Generation != Generation) { continue; }

		const uint64 Packed = Result.Key.Pack();
		InFlight.Remove(Packed);
		++CompletedJobs;
		TotalJobSeconds += Result.Seconds;
		++Stats.DisegniQuestoFrame;

		if (!Result.bAnything)
		{
			FOverlay& Empty = Overlays.FindOrAdd(Packed);
			Empty.Texture.Reset();
			Empty.Size = 0;
			Empty.Generation = Result.Generation;
			PendingUploads.Remove(Packed);
			continue;
		}
		PendingUploads.Add(Packed, MakeShared<FRasterResult>(MoveTemp(Result)));
	}

	Orphans.RemoveAll([](const UE::Tasks::FTask& Task) { return Task.IsCompleted(); });
}

void UGeoRoadsSubsystem::Synchronise()
{
	Stats = FGeoRoadsStats();
	Dressed.Reset();

	if (!Terrain || !Streaming || !Streaming->IsDatasetOpen()) { return; }
	IGeoTerrainMeshProvider* Provider = Terrain->GetProvider();
	if (!Provider) { return; }

	const UGeoQuadtreeSubsystem* Lod = GetWorld()->GetSubsystem<UGeoQuadtreeSubsystem>();

	// Dopo un teletrasporto le tile di linee in coda sono quelle del posto da
	// cui si e' partiti: si buttano, come fanno le quote e le foto.
	if (Lod && Lod->GetTeleportCount() != TeleportsSeen)
	{
		TeleportsSeen = Lod->GetTeleportCount();
		Streaming->CancelPendingRequests();
	}

	DrainFinishedJobs();

	// Il livello piu' profondo del terreno, letto una volta per frame: serve a
	// ResolutionFor per ogni tile.
	const UGeoTileStreamingSubsystem* Heights = GetWorld()->GetSubsystem<UGeoTileStreamingSubsystem>();
	DeepestTerrainLevel = (Heights && Heights->IsDatasetOpen()) ? Heights->GetDataset().GetMaxLevel() : MAX_int32;

	// L'ordine di lavoro: prima le tile a schermo, poi quelle che il quadtree
	// vorrebbe mostrare e sta aspettando (anche per colpa nostra), poi le
	// altre tenute pronte dal piano di residenza.
	TArray<FTileKey> Keys;
	const int32 VisibleCount = Terrain->GetBuiltTileKeys(Keys);

	TSet<uint64> Wanted;
	if (Lod)
	{
		for (const Quadtree::FTileRequest& Request : Lod->GetRenderRequests()) { Wanted.Add(Request.Key.Pack()); }
	}
	if (Wanted.Num() > 0 && VisibleCount < Keys.Num())
	{
		TArray<FTileKey> Hidden(Keys.GetData() + VisibleCount, Keys.Num() - VisibleCount);
		Keys.SetNum(VisibleCount);
		for (const FTileKey& Key : Hidden) { if (Wanted.Contains(Key.Pack())) { Keys.Add(Key); } }
		for (const FTileKey& Key : Hidden) { if (!Wanted.Contains(Key.Pack())) { Keys.Add(Key); } }
	}

	const FGeoVectorDataset& Dataset = Streaming->GetDataset();
	const std::vector<uint32_t>& Levels = Dataset.GetLevelNumbers();
	if (Levels.empty()) { return; }

	const double BudgetBytes = static_cast<double>(VideoBudgetMB) * 1024.0 * 1024.0;
	double UsedBytes = 0.0;
	int32 UploadBudget = Terrain->IsWarmingUp()
		? FMath::Max(MaxTexturesPerFrame, WarmupTexturesPerFrame) : MaxTexturesPerFrame;

	// Le texture ancora utili: quelle non elencate qui si liberano in fondo.
	TSet<uint64> StillUsed;
	TSet<uint64> Built;
	Built.Reserve(Keys.Num());

	for (int32 Index = 0; Index < Keys.Num(); ++Index)
	{
		const FTileKey& Key = Keys[Index];
		const uint64 Packed = Key.Pack();
		Built.Add(Packed);

		// Le tile a schermo e quelle attese dal quadtree non restano mai senza
		// strade per colpa del budget: il budget limita solo il lavoro in
		// anticipo, mai cio' che si vede.
		const bool bUrgent = (Index < VisibleCount) || Wanted.Contains(Packed);

		// --- Niente da disegnare qui? -------------------------------------
		const int32 VectorLevel = Roads::ChooseVectorLevel(Key.Level, Levels);
		const FTileKey VectorKey = (VectorLevel >= 0)
			? Imagery::AncestorOf(Key, static_cast<uint32>(VectorLevel)) : Key;
		if (VectorLevel < 0 || !Dataset.TileExists(VectorKey))
		{
			Provider->SetTileOverlay(Key, nullptr, FDrapeTransform{});
			Dressed.Add(Packed);
			++Stats.TileSenzaStrade;
			continue;
		}

		const int32 Size = ResolutionFor(Key);
		const double Bytes = GeoRuntimeTextureBytes(Size);

		// --- 1. Un disegno nuovo e' pronto: diventa texture ----------------
		if (const TSharedPtr<FRasterResult>* Pending = PendingUploads.Find(Packed))
		{
			const double PendingBytes = GeoRuntimeTextureBytes((*Pending)->Size);
			if (UploadBudget > 0 && (bUrgent || UsedBytes + PendingBytes <= BudgetBytes))
			{
				UTexture2D* Texture = CreateGeoRuntimeTexture((*Pending)->Size, (*Pending)->Size,
					(*Pending)->Pixels, (*Pending)->Mips);
				if (Texture)
				{
					FOverlay& Own = Overlays.FindOrAdd(Packed);
					Own.Texture.Reset(Texture);
					Own.Size = (*Pending)->Size;
					Own.Generation = (*Pending)->Generation;
					--UploadBudget;
					++Stats.TextureCreateQuestoFrame;
				}
				PendingUploads.Remove(Packed);
			}
		}

		// --- 2. Le sue strade ci sono gia' ---------------------------------
		if (const FOverlay* Own = Overlays.Find(Packed))
		{
			// Stile o risoluzione cambiati: il disegno vecchio resta addosso
			// finche' non arriva quello nuovo (che passa dal punto 1). Niente
			// buchi, niente lampi.
			const bool bStale = (Own->Generation != Generation)
				|| (Own->Texture.IsValid() && Own->Size != Size);
			if (bStale && !InFlight.Contains(Packed) && !PendingUploads.Contains(Packed))
			{
				if (!Streaming->FindLoadedTile(VectorKey))
				{
					Streaming->RequestTile(VectorKey, 2);
				}
				else if (InFlight.Num() < MaxJobsInFlight)
				{
					LaunchJob(Key, static_cast<uint32>(VectorLevel), Size);
				}
			}

			if (!Own->Texture.IsValid())
			{
				Provider->SetTileOverlay(Key, nullptr, FDrapeTransform{});
				StillUsed.Add(Packed);
				Dressed.Add(Packed);
				++Stats.TileSenzaStrade;
				continue;
			}

			const double OwnBytes = GeoRuntimeTextureBytes(Own->Size);
			if (bUrgent || UsedBytes + OwnBytes <= BudgetBytes)
			{
				UsedBytes += OwnBytes;
				StillUsed.Add(Packed);
				Provider->SetTileOverlay(Key, Own->Texture.Get(), FDrapeTransform{});
				Dressed.Add(Packed);
				++Stats.TileConStrade;
				continue;
			}

			// Fuori budget e non a schermo: la texture si libera (non e' in
			// StillUsed). Se la tile servira', si ridisegna.
			Provider->SetTileOverlay(Key, nullptr, FDrapeTransform{});
			++Stats.TileFuoriBudget;
			continue;
		}

		// --- 3. Da disegnare ------------------------------------------------
		if (!InFlight.Contains(Packed) && !PendingUploads.Contains(Packed))
		{
			if (Streaming->FindLoadedTile(VectorKey))
			{
				if (InFlight.Num() < MaxJobsInFlight && (bUrgent || UsedBytes + Bytes <= BudgetBytes))
				{
					LaunchJob(Key, static_cast<uint32>(VectorLevel), Size);
				}
			}
			else if (Streaming->RequestTile(VectorKey, bUrgent ? 0 : 2) == EGeoTileState::Errore)
			{
				// Priorita' del loader: piu' basso = piu' urgente. Se la tile di
				// linee non si puo' leggere (file rovinato, gia' segnalato nel
				// log), la tile di terreno non la aspetta per sempre.
				Provider->SetTileOverlay(Key, nullptr, FDrapeTransform{});
				Dressed.Add(Packed);
				++Stats.TileSenzaStrade;
				continue;
			}
		}

		// --- 4. Nel frattempo, le strade di un antenato -------------------
		// Un antenato senza strade non e' un ripiego: le classi minori
		// compaiono ai livelli piu' fini, e mostrarlo vorrebbe dire far
		// spuntare le strade un attimo dopo. Si cerca solo fra chi ha una
		// texture vera.
		bool bFallback = false;
		for (int32 Level = static_cast<int32>(Key.Level) - 1;
		     Level >= static_cast<int32>(Levels.front()) && !bFallback; --Level)
		{
			const FTileKey Ancestor = Imagery::AncestorOf(Key, static_cast<uint32>(Level));
			const FOverlay* Theirs = Overlays.Find(Ancestor.Pack());
			if (!Theirs) { continue; }
			if (!Theirs->Texture.IsValid()) { break; }

			FDrapeTransform Window = Imagery::MakeDrapeTransform(Key, static_cast<uint32>(Level));
			Window.ImageKey = Ancestor;
			Provider->SetTileOverlay(Key, Theirs->Texture.Get(), Window);
			StillUsed.Add(Ancestor.Pack());
			Dressed.Add(Packed);
			++Stats.TileConStradeDiAntenato;
			bFallback = true;
		}

		if (!bFallback) { ++Stats.TileInAttesa; }
	}

	// Le texture che nessuno usa piu' si liberano; i disegni pronti per tile
	// che non esistono piu' si buttano.
	for (auto It = Overlays.CreateIterator(); It; ++It)
	{
		if (!StillUsed.Contains(It.Key())) { It.RemoveCurrent(); }
	}
	for (auto It = PendingUploads.CreateIterator(); It; ++It)
	{
		if (!Built.Contains(It.Key())) { It.RemoveCurrent(); }
	}

	double TotalBytes = 0.0;
	for (const TPair<uint64, FOverlay>& Pair : Overlays)
	{
		if (Pair.Value.Texture.IsValid())
		{
			++Stats.TextureInMemoria;
			TotalBytes += GeoRuntimeTextureBytes(Pair.Value.Size);
		}
	}
	Stats.MemoriaVideoMB = static_cast<float>(TotalBytes / (1024.0 * 1024.0));
	Stats.BudgetVideoMB = static_cast<float>(VideoBudgetMB);
	Stats.DisegniInCorso = InFlight.Num();
	Stats.MillisecondiPerDisegno = (CompletedJobs > 0)
		? static_cast<float>(TotalJobSeconds / CompletedJobs * 1000.0) : 0.0f;
}

// ============================================================================
//  Overlay a schermo
// ============================================================================

void UGeoRoadsSubsystem::DrawDebugOverlay()
{
	if (!GEngine) { return; }

	int32 Key = 0x6EC0;
	auto Line = [&Key](const FColor& Colour, const FString& Text)
	{
		GEngine->AddOnScreenDebugMessage(Key++, 0.0f, Colour, Text);
	};

	Line(FColor::Cyan, TEXT("--- GeoWorld | Fase 8: strade, ferrovie, piste ---"));

	if (!bRoadsEnabled)
	{
		Line(FColor::Yellow, TEXT("Strade spente. Accendi con: geo.Roads.Enable 1"));
		return;
	}
	if (!Streaming || !Streaming->IsDatasetOpen())
	{
		Line(FColor::Red, TEXT("Nessun dataset di strade aperto: geo.Roads.Open <cartella>"));
		return;
	}

	if (Terrain)
	{
		if (const IGeoTerrainMeshProvider* Provider = Terrain->GetProvider())
		{
			const FString Problem = Provider->GetOverlayMaterialProblem();
			if (!Problem.IsEmpty()) { Line(FColor::Red, Problem); }
		}
	}
	if (!Streaming->GetDataset().GetClassWarning().IsEmpty())
	{
		Line(FColor::Yellow, Streaming->GetDataset().GetClassWarning());
	}

	Line(FColor::Green, FString::Printf(
		TEXT("Tile con strade: %d proprie, %d di un antenato   senza niente %d   in attesa %d"),
		Stats.TileConStrade, Stats.TileConStradeDiAntenato, Stats.TileSenzaStrade, Stats.TileInAttesa));

	Line(Stats.TileFuoriBudget > 0 ? FColor::Yellow : FColor::White, FString::Printf(
		TEXT("Texture      : %d, %.0f / %.0f MB video   +%d questo frame   fuori budget %d"),
		Stats.TextureInMemoria, Stats.MemoriaVideoMB, Stats.BudgetVideoMB,
		Stats.TextureCreateQuestoFrame, Stats.TileFuoriBudget));

	Line(Stats.MillisecondiPerDisegno > 30.0f ? FColor::Yellow : FColor::White, FString::Printf(
		TEXT("Disegno      : %d in corso, %d finiti questo frame, %.1f ms l'uno (sui worker)   %d/%d px"),
		Stats.DisegniInCorso, Stats.DisegniQuestoFrame, Stats.MillisecondiPerDisegno,
		BaseResolution, FinestResolution));

	const FGeoVectorStreamingStats StreamStats = Streaming->GetStats();
	Line(FColor::White, FString::Printf(
		TEXT("Tile di linee: %d in cache (%.1f/%.0f MB), %d in volo, %.2f ms a lettura   errori %d"),
		StreamStats.TileResidenti, StreamStats.MemoriaMB, StreamStats.BudgetMB,
		StreamStats.TileInVolo, StreamStats.TempoMedioCaricamentoMs, StreamStats.ErroriDiCaricamento));

	if (StyleKind == Roads::ERoadStyle::Map)
	{
		Line(FColor::Magenta, TEXT("Stile MAPPA: geo.Roads.Style realistico per tornare ai colori veri"));
	}
}

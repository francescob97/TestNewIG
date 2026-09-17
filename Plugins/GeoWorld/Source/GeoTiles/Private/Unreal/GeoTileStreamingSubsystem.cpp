#include "Unreal/GeoTileStreamingSubsystem.h"

#include "GeoCoreModule.h"

#include "Unreal/GeoreferenceSubsystem.h"

#include "DrawDebugHelpers.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "HAL/RunnableThread.h"
#include "Misc/FileHelper.h"
#include "Misc/QueuedThreadPool.h"
#include "Misc/ScopeLock.h"

using namespace GeoWorld::Tiles;

namespace
{
	FORCEINLINE uint64 PackKey(const FTileKey& Key)
	{
		return (static_cast<uint64>(Key.Level) << 58) ^ (static_cast<uint64>(Key.Y) << 29)
		     ^ static_cast<uint64>(Key.X);
	}

	/** Thread del pool. Non serve uno per core: il collo di bottiglia e' il disco. */
	constexpr int32 LoadThreadCount = 4;
}

/**
 * Unita' di lavoro eseguita su un thread del pool.
 *
 * NOTA UE: IQueuedWork ha DUE punti d'uscita e vanno gestiti entrambi.
 *   DoThreadedWork() -- il lavoro e' stato eseguito;
 *   Abandon()        -- il pool sta chiudendo, o il lavoro e' stato annullato
 *                       PRIMA di partire.
 * Dimenticare Abandon() e' la causa classica delle perdite di memoria alla
 * chiusura del gioco: gli oggetti in coda non vengono mai distrutti.
 */
class FGeoTileLoadWork : public IQueuedWork
{
public:
	FGeoTileLoadWork(UGeoTileStreamingSubsystem* InOwner, const FTileKey& InKey, const FString& InPath)
		: Owner(InOwner), Key(InKey), Path(InPath) {}

	virtual void DoThreadedWork() override
	{
		const double Started = FPlatformTime::Seconds();

		UGeoTileStreamingSubsystem::FLoadResult Result;
		Result.Key = Key;

		TArray<uint8> Blob;
		if (!FFileHelper::LoadFileToArray(Blob, *Path))
		{
			Result.Error = FString::Printf(TEXT("file non leggibile: %s"), *Path);
		}
		else
		{
			auto Tile = MakeShared<FHeightTile>();
			std::string DecodeError;
			if (DecodeTile(Blob.GetData(), static_cast<size_t>(Blob.Num()), Key, *Tile, DecodeError))
			{
				Result.Tile = Tile;
			}
			else
			{
				Result.Error = FString::Printf(TEXT("%hs"), DecodeError.c_str());
			}
		}

		Result.DurationSeconds = FPlatformTime::Seconds() - Started;

		// Unico punto di contatto con il game thread: una coda senza lock.
		// Da qui NON si tocca la cache, non si chiama nessun delegate e non si
		// guarda nessun UObject: sono tutti di competenza esclusiva del game
		// thread e il garbage collector puo' muoverli in qualunque momento.
		Owner->CompletedLoads.Enqueue(MoveTemp(Result));
		delete this;
	}

	virtual void Abandon() override
	{
		UGeoTileStreamingSubsystem::FLoadResult Result;
		Result.Key = Key;
		Result.Error = TEXT("annullata");
		Owner->CompletedLoads.Enqueue(MoveTemp(Result));
		delete this;
	}

private:
	UGeoTileStreamingSubsystem* Owner = nullptr;
	FTileKey Key;
	FString Path;
};

// ============================================================================
//  Ciclo di vita
// ============================================================================

bool UGeoTileStreamingSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	if (!Super::ShouldCreateSubsystem(Outer)) { return false; }
	const UWorld* World = Cast<UWorld>(Outer);
	return World && (World->WorldType == EWorldType::Game
	              || World->WorldType == EWorldType::PIE
	              || World->WorldType == EWorldType::Editor);
}

void UGeoTileStreamingSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	LoadPool = FQueuedThreadPool::Allocate();
	// TPri_BelowNormal: il caricamento delle tile non deve mai contendere la CPU
	// con il game thread. Meglio una tile che arriva un frame dopo che un frame
	// che salta.
	LoadPool->Create(LoadThreadCount, 128 * 1024, TPri_BelowNormal, TEXT("GeoTileLoadPool"));

	UE_LOG(LogGeoWorld, Log, TEXT("[GeoTiles] Streaming avviato: %d thread, cache %d MB"),
		LoadThreadCount, GetCacheBudgetMB());
}

void UGeoTileStreamingSubsystem::Deinitialize()
{
	if (LoadPool)
	{
		// Destroy() chiama Abandon() su tutto cio' che e' ancora in coda e
		// aspetta i lavori gia' partiti. Senza, si distruggerebbe il subsystem
		// mentre dei thread ne stanno ancora usando la coda.
		LoadPool->Destroy();
		delete LoadPool;
		LoadPool = nullptr;
	}

	CompletedLoads.Empty();
	InFlight.Empty();
	Cache.Clear();
	OnTileLoaded.Clear();

	Super::Deinitialize();
}

TStatId UGeoTileStreamingSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UGeoTileStreamingSubsystem, STATGROUP_Tickables);
}

void UGeoTileStreamingSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	DrainCompletedLoads();

	// Il disegno viene DOPO lo svuotamento della coda: cosi' l'overlay mostra
	// lo stato di questo frame e non quello del precedente.
	if (bShowDebugOverlay) { DrawDebugOverlay(); }
	if (bDrawTileBounds)   { DrawTileBounds(); }
}

void UGeoTileStreamingSubsystem::DrainCompletedLoads()
{
	// Si svuota tutta la coda: i risultati sono gia' pronti, rimandarli
	// aggiungerebbe solo latenza. Il costo per elemento e' un inserimento in
	// cache, cioe' nulla rispetto alla lettura che li ha prodotti.
	FLoadResult Result;
	while (CompletedLoads.Dequeue(Result))
	{
		InFlight.Remove(PackKey(Result.Key));

		if (Result.Tile.IsValid())
		{
			// shared_ptr const: la cache consegna tile immutabili, cosi' piu'
			// consumatori possono tenerle senza rischio di modifiche incrociate.
			Cache.Insert(Result.Key, std::shared_ptr<const FHeightTile>(
				new FHeightTile(*Result.Tile)));

			++CompletedCount;
			TotalLoadSeconds += Result.DurationSeconds;
			OnTileLoaded.Broadcast(Result.Key, true);
		}
		else
		{
			if (Result.Error != TEXT("annullata"))
			{
				++ErrorCount;
				UE_LOG(LogGeoWorld, Warning, TEXT("[GeoTiles] %u/%u/%u: %s"),
					Result.Key.Level, Result.Key.X, Result.Key.Y, *Result.Error);
			}
			OnTileLoaded.Broadcast(Result.Key, false);
		}
	}
}

// ============================================================================
//  Dataset e richieste
// ============================================================================

bool UGeoTileStreamingSubsystem::OpenDataset(const FString& RootDirectory, FString& OutError)
{
	ClearCache();
	if (!Dataset.Open(RootDirectory, OutError)) { return false; }

	// Gli indici dei livelli grossolani servono comunque e costano pochissimo:
	// caricarli subito evita un accesso al disco al primo frame utile.
	for (int32 Level = Dataset.GetMinLevel();
	     Level <= FMath::Min(Dataset.GetMinLevel() + 3, Dataset.GetMaxLevel()); ++Level)
	{
		FString IndexError;
		if (!Dataset.EnsureLevelIndex(Level, IndexError))
		{
			UE_LOG(LogGeoWorld, Warning, TEXT("[GeoTiles] %s"), *IndexError);
		}
	}
	return true;
}

UGeoTileStreamingSubsystem::FTilePtr UGeoTileStreamingSubsystem::FindLoadedTile(const FTileKey& Key)
{
	return Cache.Find(Key);
}

EGeoTileState UGeoTileStreamingSubsystem::GetTileState(const FTileKey& Key) const
{
	if (Cache.Peek(Key)) { return EGeoTileState::Pronta; }
	if (InFlight.Contains(PackKey(Key))) { return EGeoTileState::InCaricamento; }
	if (!Dataset.TileExists(Key)) { return EGeoTileState::Assente; }
	return EGeoTileState::NonCaricata;
}

EGeoTileState UGeoTileStreamingSubsystem::RequestTile(const FTileKey& Key, int32 Priority)
{
	if (Cache.Peek(Key)) { return EGeoTileState::Pronta; }

	const uint64 Packed = PackKey(Key);
	if (InFlight.Contains(Packed)) { return EGeoTileState::InCaricamento; }

	// L'indice del livello deve esserci: senza, non si puo' sapere se la tile
	// esiste e si finirebbe per tentare letture di file inesistenti.
	FString IndexError;
	if (!Dataset.EnsureLevelIndex(static_cast<int32>(Key.Level), IndexError))
	{
		return EGeoTileState::Errore;
	}
	if (!Dataset.TileExists(Key)) { return EGeoTileState::Assente; }

	if (!LoadPool) { return EGeoTileState::Errore; }

	InFlight.Add(Packed);

	// NOTA UE: AddQueuedWork prende la PROPRIETA' dell'oggetto. Si distrugge da
	// solo in DoThreadedWork o in Abandon: non va cancellato da qui.
	// La priorita' alta = servito prima, quindi si passa il negato perche' il
	// pool ordina per valore crescente.
	LoadPool->AddQueuedWork(new FGeoTileLoadWork(this, Key, Dataset.GetTileFilePath(Key)),
		static_cast<EQueuedWorkPriority>(FMath::Clamp(3 - Priority, 0, 6)));

	return EGeoTileState::InCaricamento;
}

void UGeoTileStreamingSubsystem::CancelPendingRequests()
{
	// I lavori gia' partiti non si interrompono: durano pochi millisecondi e
	// fermarli a meta' costerebbe piu' di quanto si risparmi. Quelli ancora in
	// coda invece si abbandonano.
	if (LoadPool)
	{
		LoadPool->Destroy();
		delete LoadPool;
		LoadPool = FQueuedThreadPool::Allocate();
		LoadPool->Create(LoadThreadCount, 128 * 1024, TPri_BelowNormal, TEXT("GeoTileLoadPool"));
	}
	DrainCompletedLoads();
	InFlight.Empty();
}

// ============================================================================
//  Cache e statistiche
// ============================================================================

void UGeoTileStreamingSubsystem::SetCacheBudgetMB(int32 Megabytes)
{
	Cache.SetBudgetBytes(static_cast<size_t>(FMath::Max(1, Megabytes)) * 1024ull * 1024ull);
}

int32 UGeoTileStreamingSubsystem::GetCacheBudgetMB() const
{
	return static_cast<int32>(Cache.GetStats().BudgetBytes / (1024ull * 1024ull));
}

void UGeoTileStreamingSubsystem::ClearCache() { Cache.Clear(); }

void UGeoTileStreamingSubsystem::SetTilePinned(const FTileKey& Key, bool bPinned)
{
	Cache.SetPinned(Key, bPinned);
}

FGeoTileStreamingStats UGeoTileStreamingSubsystem::GetStats() const
{
	const FCacheStats CacheStats = Cache.GetStats();

	FGeoTileStreamingStats Stats;
	Stats.TileCaricate = static_cast<int32>(CacheStats.ResidentTiles);
	Stats.TileVisibili = static_cast<int32>(CacheStats.PinnedTiles);
	Stats.RichiesteInCorso = InFlight.Num();
	Stats.Errori = ErrorCount;
	Stats.MemoriaCacheMB = static_cast<float>(CacheStats.GetUsedMegabytes());
	Stats.BudgetCacheMB = static_cast<float>(CacheStats.BudgetBytes) / (1024.0f * 1024.0f);
	Stats.TassoHit = static_cast<float>(CacheStats.GetHitRate());
	Stats.Evizioni = static_cast<int32>(CacheStats.Evictions);
	Stats.TempoMedioCaricamentoMs = (CompletedCount > 0)
		? static_cast<float>(TotalLoadSeconds / CompletedCount * 1000.0) : 0.0f;
	return Stats;
}

void UGeoTileStreamingSubsystem::ResetStats()
{
	ErrorCount = 0;
	CompletedCount = 0;
	TotalLoadSeconds = 0.0;
	Cache.ResetStatistics();
}

// ============================================================================
//  Visualizzazione di debug
//
//  La Fase 3 non ha un output visivo suo: carica tile e le tiene in memoria.
//  Chi le chiedera' davvero e' il quadtree della Fase 4. Questi strumenti
//  esistono per poterla comunque OSSERVARE: se non si vede cosa sta caricando
//  e cosa sta sfrattando, un problema di streaming si manifesta solo come
//  "ogni tanto scatta", che e' il tipo di sintomo che non si diagnostica.
// ============================================================================

int32 UGeoTileStreamingSubsystem::RequestTilesAround(double Latitude, double Longitude,
                                                     int32 Level, int32 Radius)
{
	if (!Dataset.IsOpen()) { return 0; }

	Level = FMath::Clamp(Level, Dataset.GetMinLevel(), Dataset.GetMaxLevel());

	FString IndexError;
	if (!Dataset.EnsureLevelIndex(Level, IndexError))
	{
		UE_LOG(LogGeoWorld, Warning, TEXT("[GeoTiles] %s"), *IndexError);
		return 0;
	}

	uint32 CentreX = 0, CentreY = 0;
	TileForLonLat(static_cast<uint32>(Level), Longitude, Latitude, CentreX, CentreY);

	const uint32 MaxX = TilesX(static_cast<uint32>(Level)) - 1;
	const uint32 MaxY = TilesY(static_cast<uint32>(Level)) - 1;

	int32 Requested = 0;
	for (int32 OffsetY = -Radius; OffsetY <= Radius; ++OffsetY)
	{
		for (int32 OffsetX = -Radius; OffsetX <= Radius; ++OffsetX)
		{
			const int64 X = static_cast<int64>(CentreX) + OffsetX;
			const int64 Y = static_cast<int64>(CentreY) + OffsetY;
			if (X < 0 || Y < 0 || X > MaxX || Y > MaxY) { continue; }

			const FTileKey Key{ static_cast<uint32>(Level),
			                    static_cast<uint32>(X), static_cast<uint32>(Y) };

			// Priorita' decrescente con la distanza dal centro: le tile sotto
			// la camera arrivano prima di quelle ai bordi. E' la stessa regola
			// che in Fase 4 sara' guidata dall'errore su schermo.
			const int32 Distance = FMath::Max(FMath::Abs(OffsetX), FMath::Abs(OffsetY));
			const EGeoTileState State = RequestTile(Key, FMath::Max(0, 3 - Distance));

			if (State == EGeoTileState::InCaricamento) { ++Requested; }
		}
	}
	return Requested;
}

void UGeoTileStreamingSubsystem::DrawDebugOverlay()
{
	if (!GEngine) { return; }

	const FGeoTileStreamingStats Stats = GetStats();

	// Chiavi stabili: le righe si aggiornano in posto invece di accumularsi.
	int32 Key = 0x6E70;
	auto Line = [&Key](const FColor& Colour, const FString& Text)
	{
		GEngine->AddOnScreenDebugMessage(Key++, 0.0f, Colour, Text);
	};

	Line(FColor::Cyan, TEXT("--- GeoWorld | Fase 3: streaming delle tile ---"));

	if (!Dataset.IsOpen())
	{
		Line(FColor::Yellow, TEXT("Nessun dataset aperto. Usa: geo.Tiles.Open <cartella>"));
		return;
	}

	Line(FColor::White, FString::Printf(TEXT("Dataset    : %s   livelli %d..%d"),
		*Dataset.GetDatasetName(), Dataset.GetMinLevel(), Dataset.GetMaxLevel()));

	Line(FColor::Green, FString::Printf(TEXT("Tile in cache : %d   (pinnate %d)"),
		Stats.TileCaricate, Stats.TileVisibili));

	// Il colore passa a giallo quando la cache e' quasi piena: e' il momento in
	// cui cominciano gli sfratti, e quindi i ricaricamenti.
	const float Fill = (Stats.BudgetCacheMB > 0.0f) ? Stats.MemoriaCacheMB / Stats.BudgetCacheMB : 0.0f;
	Line(Fill > 0.9f ? FColor::Yellow : FColor::White,
		FString::Printf(TEXT("Memoria cache : %.1f / %.0f MB   (%.0f%%)"),
			Stats.MemoriaCacheMB, Stats.BudgetCacheMB, Fill * 100.0f));   // non-unita: frazione -> percentuale

	// Un tasso di hit basso con molte evizioni significa che il budget e'
	// troppo piccolo per quello che si sta guardando: si ricarica in continuazione.
	Line(Stats.TassoHit < 0.5f && Stats.Evizioni > 0 ? FColor::Yellow : FColor::White,
		FString::Printf(TEXT("Tasso di hit  : %.1f%%   evizioni %d"),
			Stats.TassoHit * 100.0f, Stats.Evizioni));   // non-unita: frazione -> percentuale

	Line(Stats.RichiesteInCorso > 0 ? FColor::Cyan : FColor::White,
		FString::Printf(TEXT("In caricamento: %d"), Stats.RichiesteInCorso));

	Line(FColor::White, FString::Printf(TEXT("Tempo medio   : %.2f ms per tile"),
		Stats.TempoMedioCaricamentoMs));

	if (Stats.Errori > 0)
	{
		Line(FColor::Red, FString::Printf(TEXT("ERRORI        : %d  (vedi Output Log)"), Stats.Errori));
	}
}

void UGeoTileStreamingSubsystem::DrawTileBounds() const
{
	const UWorld* World = GetWorld();
	if (!World) { return; }

	const UGeoreferenceSubsystem* Georeference = World->GetSubsystem<UGeoreferenceSubsystem>();
	if (!Georeference) { return; }

	const FGeoreferenceSnapshot Snapshot = Georeference->GetSnapshot();

	// Un colore per livello: si vede a colpo d'occhio quale risoluzione e'
	// residente in quale zona.
	static const FColor LevelColours[] = {
		FColor::White, FColor(180, 180, 180), FColor::Silver, FColor::Emerald,
		FColor::Green, FColor::Cyan, FColor::Blue, FColor::Purple,
		FColor::Magenta, FColor::Orange, FColor::Yellow, FColor::Red };

	// Ogni lato si suddivide, altrimenti un box disegnato con quattro segmenti
	// rettilinei attraverserebbe la superficie invece di seguirla: su una tile
	// di livello 8, larga centinaia di km, la differenza e' di chilometri.
	constexpr int32 SegmentsPerEdge = 8;
	constexpr int32 MaxTilesToDraw = 400;

	int32 Drawn = 0;
	Cache.ForEachResident(
		[&](const FTileKey& Key, const FTileCache::FTilePtr& Tile, bool bPinned)
	{
		if (Drawn >= MaxTilesToDraw || !Tile) { return; }
		++Drawn;

		const FTileBounds Bounds = GetTileBounds(Key.Level, Key.X, Key.Y);
		const FColor Colour = LevelColours[Key.Level % UE_ARRAY_COUNT(LevelColours)];
		const float Thickness = bPinned ? 40.0f : 12.0f;

		// I quattro spigoli in senso orario, per costruire il perimetro.
		const double CornerLons[4] = { Bounds.West, Bounds.East, Bounds.East, Bounds.West };
		const double CornerLats[4] = { Bounds.North, Bounds.North, Bounds.South, Bounds.South };

		auto ToWorld = [&](double Lon, double Lat, double Height)
		{
			return Snapshot.GeodeticToUnreal(
				GeoWorld::Core::FGeodetic::FromDegrees(Lat, Lon, Height));
		};

		// Anello inferiore (quota minima) e superiore (quota massima): insieme
		// mostrano il volume che in Fase 4 servira' per il frustum culling.
		for (int32 Edge = 0; Edge < 4; ++Edge)
		{
			const int32 Next = (Edge + 1) % 4;
			for (int32 Step = 0; Step < SegmentsPerEdge; ++Step)
			{
				const double T0 = static_cast<double>(Step) / SegmentsPerEdge;
				const double T1 = static_cast<double>(Step + 1) / SegmentsPerEdge;

				const double Lon0 = FMath::Lerp(CornerLons[Edge], CornerLons[Next], T0);
				const double Lat0 = FMath::Lerp(CornerLats[Edge], CornerLats[Next], T0);
				const double Lon1 = FMath::Lerp(CornerLons[Edge], CornerLons[Next], T1);
				const double Lat1 = FMath::Lerp(CornerLats[Edge], CornerLats[Next], T1);

				DrawDebugLine(World, ToWorld(Lon0, Lat0, Tile->MinHeight),
					ToWorld(Lon1, Lat1, Tile->MinHeight), Colour, false, -1.f, 0, Thickness);
				DrawDebugLine(World, ToWorld(Lon0, Lat0, Tile->MaxHeight),
					ToWorld(Lon1, Lat1, Tile->MaxHeight), Colour, false, -1.f, 0, Thickness);
			}

			// Montante verticale sullo spigolo: chiude il volume.
			DrawDebugLine(World, ToWorld(CornerLons[Edge], CornerLats[Edge], Tile->MinHeight),
				ToWorld(CornerLons[Edge], CornerLats[Edge], Tile->MaxHeight),
				Colour, false, -1.f, 0, Thickness);
		}

		// Etichetta al centro, sopra la quota massima. DrawDebugString e' sempre
		// rivolta verso la camera, quindi leggibile da qualunque angolo.
		DrawDebugString(const_cast<UWorld*>(World),
			ToWorld(Bounds.CentreLon(), Bounds.CentreLat(), Tile->MaxHeight),
			FString::Printf(TEXT("L%u  %u/%u\n%.0f..%.0f m"),
				Key.Level, Key.X, Key.Y, Tile->MinHeight, Tile->MaxHeight),
			nullptr, Colour, 0.0f, true);
	});
}

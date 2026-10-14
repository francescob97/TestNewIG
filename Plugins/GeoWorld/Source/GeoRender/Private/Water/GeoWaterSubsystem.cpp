#include "Water/GeoWaterSubsystem.h"

#include "GeoCoreModule.h"
#include "Geo/GeoUnits.h"
#include "Georeference/GeoreferenceSubsystem.h"
#include "Imagery/GeoRuntimeTexture.h"
#include "Streaming/GeoTileStreamingSubsystem.h"
#include "Streaming/GeoWaterStreamingSubsystem.h"
#include "Terrain/GeoTerrainSubsystem.h"
#include "Tiles/ImageTileFormat.h"
#include "Water/WaterMesh.h"
#include "Water/WaterTextures.h"
#include "Weather/GeoWeatherSubsystem.h"

#include "Engine/Engine.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "Materials/MaterialParameterCollection.h"
#include "Materials/MaterialParameterCollectionInstance.h"

using namespace GeoWorld;
using GeoWorld::Tiles::FTileKey;

namespace
{
	const FName WaterDressOwnerName(TEXT("Acqua"));

	/** I nomi dei parametri della collezione, costruiti una volta. */
	struct FWaveParameterNames
	{
		FName A[Water::GerstnerWaveCount];
		FName B[Water::GerstnerWaveCount];
		FName Wind = TEXT("WaterWind");
		FName Detail = TEXT("WaterDetail");
		FName Flow = TEXT("WaterFlow");

		FWaveParameterNames()
		{
			for (int32 Index = 0; Index < Water::GerstnerWaveCount; ++Index)
			{
				A[Index] = FName(*FString::Printf(TEXT("Wave%dA"), Index));
				B[Index] = FName(*FString::Printf(TEXT("Wave%dB"), Index));
			}
		}
	};

	const FWaveParameterNames& WaveNames()
	{
		static const FWaveParameterNames Names;
		return Names;
	}

	/** Lato delle due normali di dettaglio, in metri, e quanto scorrono rispetto al vento. */
	constexpr double DetailTileMetres[2] = { 9.0, 31.0 };
	constexpr double DetailSpeedFactor[2] = { 0.22, 0.11 };
	/** Ciclo della corrente dei fiumi (flow map a due fasi), in secondi. */
	constexpr double FlowCycleSeconds = 4.0;
}

bool UGeoWaterSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	if (!Super::ShouldCreateSubsystem(Outer)) { return false; }
	const UWorld* World = Cast<UWorld>(Outer);
	return World && (World->WorldType == EWorldType::Game
	              || World->WorldType == EWorldType::PIE
	              || World->WorldType == EWorldType::Editor);
}

void UGeoWaterSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Collection.InitializeDependency<UGeoTerrainSubsystem>();
	Collection.InitializeDependency<UGeoWaterStreamingSubsystem>();
	Collection.InitializeDependency<UGeoWeatherSubsystem>();

	UWorld* World = GetWorld();
	Terrain = World->GetSubsystem<UGeoTerrainSubsystem>();
	Streaming = World->GetSubsystem<UGeoWaterStreamingSubsystem>();
	Queue = MakeShared<FMeshQueue, ESPMode::ThreadSafe>();

	if (UGeoreferenceSubsystem* Georeference = World->GetSubsystem<UGeoreferenceSubsystem>())
	{
		RebaseHandle = Georeference->OnGeoreferenceRebased.AddUObject(this, &UGeoWaterSubsystem::OnGeoreferenceRebased);
	}

	// Il predicato "Acqua": una tile e' pronta se non le serve acqua, o se la
	// sua acqua e' consegnata. La lambda cattura this: si toglie in
	// Deinitialize.
	if (Terrain)
	{
		Terrain->SetDressPredicate(WaterDressOwnerName, [this](const FTileKey& Key)
		{
			if (!bEnabled || !bCarvingInstalled || !Streaming || !Streaming->IsDatasetOpen()) { return true; }
			return Dressed.Contains(Key.Pack());
		});
	}
}

void UGeoWaterSubsystem::Deinitialize()
{
	if (UWorld* World = GetWorld())
	{
		if (UGeoreferenceSubsystem* Georeference = World->GetSubsystem<UGeoreferenceSubsystem>())
		{
			Georeference->OnGeoreferenceRebased.Remove(RebaseHandle);
		}
	}
	if (Terrain)
	{
		Terrain->ClearDressPredicate(WaterDressOwnerName);
		if (bCarvingInstalled) { Terrain->ClearHeightEditor(); }
	}
	bCarvingInstalled = false;

	// I lavori in volo eseguono codice di questo modulo: si aspettano.
	for (TPair<uint64, UE::Tasks::FTask>& Pair : InFlight) { Pair.Value.Wait(); }
	InFlight.Empty();
	PendingCommits.Empty();
	Delivered.Empty();
	DetailNormal.Reset();
	Foam.Reset();
	Super::Deinitialize();
}

TStatId UGeoWaterSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UGeoWaterSubsystem, STATGROUP_Tickables);
}

bool UGeoWaterSubsystem::GetGeoidUndulation(double LonDeg, double LatDeg, double& OutUndulation) const
{
	return Streaming && Streaming->IsDatasetOpen() && Streaming->GetDataset().SampleGeoid(LonDeg, LatDeg, OutUndulation);
}

FString UGeoWaterSubsystem::GetMaterialProblem() const
{
	const IGeoTerrainMeshProvider* Provider = Terrain ? Terrain->GetProvider() : nullptr;
	if (Provider && !Provider->GetWaterMaterialProblem().IsEmpty()) { return Provider->GetWaterMaterialProblem(); }
	if (!WaveCollection)
	{
		return TEXT("manca MPC_GeoWater: l'acqua resta piatta (niente onde). Creala con geo.Imagery.CreateMaterial");
	}
	return FString();
}

void UGeoWaterSubsystem::SetEnabled(bool bInEnabled)
{
	if (bEnabled == bInEnabled) { return; }
	bEnabled = bInEnabled;
	OnDatasetChanged();
}

void UGeoWaterSubsystem::OnDatasetChanged()
{
	// Tutto cio' che e' stato costruito con l'acqua di prima non vale piu':
	// le superfici si buttano, i lavori in volo diventano orfani, e il
	// terreno si ricostruisce (scavato o no).
	++Generation;
	RemoveAllWater();
	InstallCarving(false);
	// Il terreno si ricostruisce al prossimo Synchronise, una volta sola,
	// con lo scavo gia' deciso (acceso o spento).
	bNeedsTerrainRebuild = true;
}

void UGeoWaterSubsystem::RemoveAllWater()
{
	IGeoTerrainMeshProvider* Provider = Terrain ? Terrain->GetProvider() : nullptr;
	if (Provider)
	{
		for (const TPair<uint64, FWaterState>& Pair : Delivered)
		{
			if (!Pair.Value.bEmpty) { Provider->RemoveWaterMesh(Pair.Value.Key); }
		}
	}
	Delivered.Empty();
	PendingCommits.Empty();
	Dressed.Empty();
}

void UGeoWaterSubsystem::InstallCarving(bool bInstall)
{
	if (!Terrain || bCarvingInstalled == bInstall) { return; }
	bCarvingInstalled = bInstall;
	if (!bInstall)
	{
		Terrain->ClearHeightEditor();
		return;
	}

	// ------------------------------------------------------------------
	//  Lo scavo, consegnato al terreno. La funzione che ritorniamo gira su un
	//  thread di lavoro: cattura solo la tile d'acqua (shared_ptr a dati
	//  immutabili) e la chiave, niente "this".
	// ------------------------------------------------------------------
	Terrain->SetHeightEditor([this](const FTileKey& Key, FGeoHeightEdit& OutEdit) -> EGeoHeightEdit
	{
		if (!Streaming || !Streaming->IsDatasetOpen()) { return EGeoHeightEdit::None; }
		FTileKey WaterKey;
		if (!Streaming->GetDataset().ResolveWaterKey(Key, WaterKey)) { return EGeoHeightEdit::None; }

		const UGeoWaterStreamingSubsystem::FWaterTilePtr Tile = Streaming->FindLoadedTile(WaterKey);
		if (!Tile)
		{
			const EGeoTileState State = Streaming->RequestTile(WaterKey, 0);
			// Una tile illeggibile vale "niente acqua": aspettarla per sempre
			// lascerebbe un buco nel terreno.
			return (State == EGeoTileState::Errore || State == EGeoTileState::Assente)
				? EGeoHeightEdit::None : EGeoHeightEdit::NotReady;
		}
		OutEdit = [Tile, Key](Tiles::FHeightTile& Heights)
		{
			Water::CarveHeights(Heights, Water::FWaterView::Make(*Tile, Key));
		};
		return EGeoHeightEdit::Ready;
	});
}

void UGeoWaterSubsystem::EnsureSharedTextures(IGeoTerrainMeshProvider* Provider)
{
	if (!DetailNormal.IsValid() || !Foam.IsValid())
	{
		std::vector<uint8_t> Pixels;
		std::vector<std::vector<uint8_t>> Mips;
		Water::BuildWaterNormalTexture(Pixels);
		Tiles::BuildMipChain(Pixels, Water::WaterTextureSize, Water::WaterTextureSize, Mips, /*bSrgbColour=*/false);
		DetailNormal.Reset(CreateGeoRuntimeDataTexture(Water::WaterTextureSize, Water::WaterTextureSize, Pixels, Mips,
			/*bSrgb=*/false, /*bWrapU=*/true, /*bWrapV=*/true));

		Water::BuildWaterFoamTexture(Pixels);
		Tiles::BuildMipChain(Pixels, Water::WaterTextureSize, Water::WaterTextureSize, Mips, /*bSrgbColour=*/false);
		Foam.Reset(CreateGeoRuntimeDataTexture(Water::WaterTextureSize, Water::WaterTextureSize, Pixels, Mips,
			/*bSrgb=*/false, /*bWrapU=*/true, /*bWrapV=*/true));
	}
	if (Provider && !Provider->HasWaterTextures() && DetailNormal.IsValid() && Foam.IsValid())
	{
		Provider->SetWaterTextures(DetailNormal.Get(), Foam.Get());
	}
}

void UGeoWaterSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	// La collezione dei parametri si cerca ogni due secondi finche' manca: si
	// puo' creare con il Play gia' partito (come i materiali, vedi il provider).
	const double Now = FPlatformTime::Seconds();
	if (!WaveCollection && Now - LastCollectionCheck > 2.0)
	{
		LastCollectionCheck = Now;
		WaveCollection = LoadObject<UMaterialParameterCollection>(nullptr,
			TEXT("/GeoWorld/Materials/MPC_GeoWater.MPC_GeoWater"), nullptr, LOAD_NoWarn | LOAD_Quiet);
	}

	UpdateWaves(DeltaTime);

	IGeoTerrainMeshProvider* Provider = Terrain ? Terrain->GetProvider() : nullptr;
	if (Provider && Terrain->IsEnabled()) { Synchronise(Provider); }
	if (bShowDebugOverlay) { DrawDebugOverlay(); }
	else { DrawStaleWarning(); }
}

void UGeoWaterSubsystem::DrawStaleWarning() const
{
	// Un dataset fatto con regole vecchie si vede SEMPRE in rosso, overlay o
	// no: dieci secondi di messaggio all'apertura si perdono, e il sintomo (i
	// fiumi in fondo a una fossa) sembra un difetto del motore, non del dato.
	if (!GEngine || !bEnabled || !Streaming || !Streaming->IsDatasetOpen()) { return; }
	const FString Stale = Streaming->GetDataset().GetStaleReason();
	if (!Stale.IsEmpty()) { GEngine->AddOnScreenDebugMessage(0x6F1F, 0.0f, FColor::Red, Stale); }
}

void UGeoWaterSubsystem::Synchronise(IGeoTerrainMeshProvider* Provider)
{
	Stats.ConsegneQuestoFrame = 0;
	const bool bDatasetOpen = Streaming && Streaming->IsDatasetOpen();
	// Senza il materiale l'acqua non si disegna: allora neanche si scava, o
	// i laghi sarebbero buche vuote nel terreno.
	const bool bMaterialOk = Provider->GetWaterMaterialProblem().IsEmpty();
	const bool bWanted = bEnabled && bDatasetOpen && bMaterialOk;

	if (bWanted != bCarvingInstalled || bNeedsTerrainRebuild)
	{
		++Generation;
		RemoveAllWater();
		InstallCarving(bWanted);
		Terrain->RebuildAll();
		bNeedsTerrainRebuild = false;
	}
	if (!bWanted) { return; }

	EnsureSharedTextures(Provider);
	DrainFinished(Provider, Terrain->IsWarmingUp() ? MaxCommitsPerFrame * 4 : MaxCommitsPerFrame);

	TArray<FTileKey> Keys;
	const int32 VisibleCount = Terrain->GetBuiltTileKeys(Keys);
	UGeoTileStreamingSubsystem* Heights = GetWorld()->GetSubsystem<UGeoTileStreamingSubsystem>();
	const int32 Step = Terrain->GetMeshStep();
	const FGeoWaterDataset& Dataset = Streaming->GetDataset();

	TSet<uint64> Built;
	Built.Reserve(Keys.Num());
	Dressed.Reset();
	Stats.TileConAcqua = Stats.TileVuote = Stats.TileInAttesa = 0;

	for (int32 Index = 0; Index < Keys.Num(); ++Index)
	{
		const FTileKey& Key = Keys[Index];
		const uint64 Packed = Key.Pack();
		const bool bOnScreen = Index < VisibleCount;
		Built.Add(Packed);

		FTileKey WaterKey;
		if (!Dataset.ResolveWaterKey(Key, WaterKey))
		{
			// Niente acqua qui. Se c'era (un altro dataset), via.
			if (const FWaterState* Old = Delivered.Find(Packed))
			{
				if (!Old->bEmpty) { Provider->RemoveWaterMesh(Key); }
				Delivered.Remove(Packed);
			}
			Dressed.Add(Packed);
			continue;
		}

		const FWaterState* State = Delivered.Find(Packed);
		const bool bHas = State && State->Step == Step && State->WaterKey == WaterKey
		               && (State->bEmpty || Provider->HasWaterMesh(Key));
		if (bHas)
		{
			(State->bEmpty ? Stats.TileVuote : Stats.TileConAcqua)++;
			Dressed.Add(Packed);
			continue;
		}

		// Una tile GIA' A SCHERMO non si "sveste" per l'acqua che manca: si
		// tiene com'e' e l'acqua arriva (come per le strade 3D).
		if (bOnScreen) { Dressed.Add(Packed); }
		++Stats.TileInAttesa;

		const bool bPending = PendingCommits.ContainsByPredicate(
			[Packed](const FMeshResult& Pending) { return Pending.Key.Pack() == Packed; });
		if (bPending || InFlight.Contains(Packed) || InFlight.Num() >= MaxJobsInFlight) { continue; }

		// Servono la tile d'acqua e le quote; se mancano si chiedono, e si
		// riprova al prossimo frame.
		if (!Streaming->FindLoadedTile(WaterKey))
		{
			const EGeoTileState WaterState = Streaming->RequestTile(WaterKey, bOnScreen ? 0 : 2);
			if (WaterState == EGeoTileState::Errore || WaterState == EGeoTileState::Assente)
			{
				Delivered.Add(Packed, FWaterState{ Key, WaterKey, Step, true });
				Dressed.Add(Packed);
			}
			continue;
		}
		if (Heights && !Heights->FindLoadedTile(Key))
		{
			Heights->RequestTile(Key, bOnScreen ? 0 : 2);
			continue;
		}
		LaunchJob(Provider, Key, WaterKey, Step);
	}

	// Lo stato delle tile che il terreno non ha piu' si dimentica: la loro
	// acqua e' sparita con loro nel provider.
	for (auto It = Delivered.CreateIterator(); It; ++It)
	{
		if (!Built.Contains(It.Key())) { It.RemoveCurrent(); }
	}
	PendingCommits.RemoveAll([&Built](const FMeshResult& Pending) { return !Built.Contains(Pending.Key.Pack()); });

	Stats.CostruzioniInCorso = InFlight.Num();
	Stats.MillisecondiPerCostruzione = CompletedJobs > 0 ? static_cast<float>(TotalJobSeconds / CompletedJobs * 1000.0) : 0.0f;
	Stats.Triangoli = Provider->GetWaterTriangleCount();
}

bool UGeoWaterSubsystem::LaunchJob(IGeoTerrainMeshProvider* Provider, const FTileKey& Key, const FTileKey& WaterKey,
                                   int32 Step)
{
	UGeoTileStreamingSubsystem* Heights = GetWorld()->GetSubsystem<UGeoTileStreamingSubsystem>();
	const UGeoTileStreamingSubsystem::FTilePtr Height = Heights ? Heights->FindLoadedTile(Key) : nullptr;
	const UGeoWaterStreamingSubsystem::FWaterTilePtr WaterTile = Streaming->FindLoadedTile(WaterKey);
	const FGeoPrepareTileMeshFunction Prepare = Provider->GetPrepareFunction();
	if (!Height || !WaterTile || !Prepare || !Queue.IsValid()) { return false; }

	Water::FWaterMeshParameters Parameters;
	Parameters.Step = Step;
	Parameters.bFlipWinding = Terrain->IsFlipWinding();
	const int32 JobGeneration = Generation;
	TSharedPtr<FMeshQueue, ESPMode::ThreadSafe> JobQueue = Queue;

	// Tutto per copia, niente "this": come le mesh del terreno e delle strade.
	UE::Tasks::FTask Task = UE::Tasks::Launch(TEXT("GeoWaterBuild"),
		[Height, WaterTile, Key, WaterKey, Parameters, Prepare, JobGeneration, JobQueue]()
		{
			const double Started = FPlatformTime::Seconds();
			FMeshResult Result;
			Result.Key = Key;
			Result.WaterKey = WaterKey;
			Result.Generation = JobGeneration;
			Result.Step = Parameters.Step;

			// La stessa tile scavata che usa il terreno: stessa origine.
			const Water::FWaterView View = Water::FWaterView::Make(*WaterTile, Key);
			Tiles::FHeightTile Carved = *Height;
			Water::CarveHeights(Carved, View);

			Mesh::FTileMeshData MeshData;
			Water::BuildWaterMesh(Carved, View, Parameters, MeshData);
			if (MeshData.TriangleCount > 0)
			{
				Water::BuildWaterRasterPixels(View, Result.Pixels);
				Tiles::BuildMipChain(Result.Pixels, Tiles::WaterRaster, Tiles::WaterRaster, Result.Mips, /*bSrgbColour=*/false);
				Result.Prepared = Prepare(MeshData);
			}
			Result.bEmpty = !Result.Prepared.IsValid();
			Result.MetresPerPixel = static_cast<float>(Tiles::WaterMetresPerPixel(Key.Level, Key.Y));
			Result.CellMetres = static_cast<float>(Tiles::PostSpacingDeg(Key.Level) * Parameters.Step * 111132.0);
			Result.Seconds = FPlatformTime::Seconds() - Started;
			JobQueue->Completed.Enqueue(MoveTemp(Result));
		},
		UE::Tasks::ETaskPriority::BackgroundNormal);

	InFlight.Add(Key.Pack(), Task);
	return true;
}

void UGeoWaterSubsystem::DrainFinished(IGeoTerrainMeshProvider* Provider, int32 Budget)
{
	FMeshResult Result;
	while (Queue.IsValid() && Queue->Completed.Dequeue(Result))
	{
		// Il lavoro e' finito comunque, anche se di una generazione vecchia:
		// fuori da InFlight SEMPRE, o quella tile (e, a forza di accumularne,
		// tutte) non si ricostruirebbe mai piu'.
		InFlight.Remove(Result.Key.Pack());
		if (Result.Generation != Generation) { continue; }
		++CompletedJobs;
		TotalJobSeconds += Result.Seconds;
		PendingCommits.Add(MoveTemp(Result));
	}

	int32 Index = 0;
	while (Index < PendingCommits.Num())
	{
		FMeshResult& Pending = PendingCommits[Index];
		const uint64 Packed = Pending.Key.Pack();
		if (Pending.bEmpty)
		{
			// Acqua solo nel margine, o solo scavo: niente superficie qui.
			const FWaterState* Old = Delivered.Find(Packed);
			if (Old && !Old->bEmpty) { Provider->RemoveWaterMesh(Pending.Key); }
			Delivered.Add(Packed, FWaterState{ Pending.Key, Pending.WaterKey, Pending.Step, true });
			PendingCommits.RemoveAt(Index);
			continue;
		}
		if (Budget <= 0) { ++Index; continue; }

		UTexture2D* Raster = CreateGeoRuntimeDataTexture(Tiles::WaterRaster, Tiles::WaterRaster, Pending.Pixels,
			Pending.Mips, /*bSrgb=*/false, /*bWrapU=*/false, /*bWrapV=*/false);
		if (Raster && Pending.Prepared.IsValid()
			&& Provider->CommitWaterMesh(Pending.Key, *Pending.Prepared, Raster, Pending.MetresPerPixel, Pending.CellMetres))
		{
			Delivered.Add(Packed, FWaterState{ Pending.Key, Pending.WaterKey, Pending.Step, false });
			++Stats.ConsegneQuestoFrame;
			--Budget;
		}
		PendingCommits.RemoveAt(Index);
	}
}

// ---------------------------------------------------------------------------
//  Le onde
// ---------------------------------------------------------------------------

void UGeoWaterSubsystem::UpdateWaves(float DeltaTime)
{
	UWorld* World = GetWorld();
	const UGeoWeatherSubsystem* WeatherSubsystem = World ? World->GetSubsystem<UGeoWeatherSubsystem>() : nullptr;
	// Le onde lunghe seguono il vento MEDIO; le raffiche muovono solo
	// l'increspatura e la schiuma (vedi FWaveField: perche').
	if (WeatherSubsystem) { Waves.SetTargetWind(WeatherSubsystem->GetMeanWind()); }
	if (!bWavesStarted)
	{
		// Al primo frame il mare e' gia' quello del vento, non nasce piatto.
		Waves.SnapToTarget();
		bWavesStarted = true;
	}
	Waves.Advance(DeltaTime);

	// Il vento di adesso, CON le raffiche: per l'increspatura e la schiuma.
	const Weather::FWind Wind = WeatherSubsystem ? WeatherSubsystem->GetWind() : Waves.GetWind();
	double TowardNorth = 0.0, TowardEast = 0.0;
	Wind.Toward(TowardNorth, TowardEast);

	// Le normali di dettaglio scorrono con il vento, piu' piano le grandi. Il
	// materiale legge la texture in uv = X / lato + offset: perche' il disegno
	// vada VERSO cui soffia il vento, l'offset deve CALARE (il punto che ora
	// sta in X stava prima in X - v dt).
	for (int32 Layer = 0; Layer < 2; ++Layer)
	{
		const double Speed = DetailSpeedFactor[Layer] * Wind.SpeedMs / DetailTileMetres[Layer];
		const double Shift[2] = { TowardNorth * Speed * DeltaTime, TowardEast * Speed * DeltaTime };
		for (int32 Axis = 0; Axis < 2; ++Axis)
		{
			double& Offset = DetailOffset[Layer * 2 + Axis];
			Offset = FMath::Fmod(Offset - Shift[Axis], 1.0);
			if (Offset < 0.0) { Offset += 1.0; }
		}
	}
	FlowSeconds = FMath::Fmod(FlowSeconds + DeltaTime, FlowCycleSeconds * 1000.0);

	if (!WaveCollection || !World) { return; }
	UMaterialParameterCollectionInstance* Instance = World->GetParameterCollectionInstance(WaveCollection);
	if (!Instance) { return; }

	const FWaveParameterNames& Names = WaveNames();
	for (int32 Index = 0; Index < Water::GerstnerWaveCount; ++Index)
	{
		const Water::FGerstnerWave& Wave = Waves.GetWaves()[static_cast<size_t>(Index)];
		// A: direzione (nord, est), numero d'onda k (1/m), fase (gia' ridotta).
		// B: ampiezza (m), ripidita' Q, lunghezza (m).
		Instance->SetVectorParameterValue(Names.A[Index], FLinearColor(
			static_cast<float>(Wave.DirNorth), static_cast<float>(Wave.DirEast),
			static_cast<float>(Wave.Number()), static_cast<float>(Waves.GetPhase(Index))));
		Instance->SetVectorParameterValue(Names.B[Index], FLinearColor(
			static_cast<float>(Wave.Amplitude), static_cast<float>(Wave.Steepness),
			static_cast<float>(Wave.Wavelength), 0.0f));
	}
	Instance->SetVectorParameterValue(Names.Wind, FLinearColor(static_cast<float>(TowardNorth), static_cast<float>(TowardEast),
		static_cast<float>(Wind.SpeedMs), static_cast<float>(Water::WhitecapAmount(Wind.SpeedMs))));
	Instance->SetVectorParameterValue(Names.Detail, FLinearColor(
		static_cast<float>(DetailOffset[0]), static_cast<float>(DetailOffset[1]),
		static_cast<float>(DetailOffset[2]), static_cast<float>(DetailOffset[3])));
	const double Phase = FlowSeconds / FlowCycleSeconds;
	Instance->SetVectorParameterValue(Names.Flow, FLinearColor(
		static_cast<float>(Phase - FMath::FloorToDouble(Phase)), static_cast<float>(Phase + 0.5 - FMath::FloorToDouble(Phase + 0.5)),
		static_cast<float>(FlowCycleSeconds), 0.0f));
}

void UGeoWaterSubsystem::OnGeoreferenceRebased(const FGeoreferenceSnapshot& Snapshot)
{
	// ------------------------------------------------------------------
	//  Il rebase sposta il mondo sotto le onde: il punto che prima stava
	//  nella vecchia posizione della nuova origine ora sta a zero. Le onde
	//  dipendono dalla posizione nel mondo, quindi senza correzione il mare
	//  intero salterebbe di colpo (un altro "flash"). Si sposta la fase di
	//  ogni onda e l'offset delle normali di dettaglio della stessa quantita'.
	// ------------------------------------------------------------------
	if (bHaveSnapshot)
	{
		const FVector OldPosition = LastSnapshot.GeodeticToUnreal(Snapshot.Georeference.GetOriginGeodetic());
		const double DeltaNorth = OldPosition.X * Units::UuToMeters;
		const double DeltaEast = OldPosition.Y * Units::UuToMeters;
		Waves.ApplyOriginShift(DeltaNorth, DeltaEast);
		for (int32 Layer = 0; Layer < 2; ++Layer)
		{
			DetailOffset[Layer * 2 + 0] = FMath::Fmod(DetailOffset[Layer * 2 + 0] + DeltaNorth / DetailTileMetres[Layer], 1.0);
			DetailOffset[Layer * 2 + 1] = FMath::Fmod(DetailOffset[Layer * 2 + 1] + DeltaEast / DetailTileMetres[Layer], 1.0);
			for (int32 Axis = 0; Axis < 2; ++Axis)
			{
				if (DetailOffset[Layer * 2 + Axis] < 0.0) { DetailOffset[Layer * 2 + Axis] += 1.0; }
			}
		}
	}
	LastSnapshot = Snapshot;
	bHaveSnapshot = true;
}

// ---------------------------------------------------------------------------
//  Overlay
// ---------------------------------------------------------------------------

void UGeoWaterSubsystem::DrawDebugOverlay()
{
	if (!GEngine) { return; }
	int32 Key = 0x6F20;
	auto Line = [&Key](const FColor& Colour, const FString& Text)
	{
		GEngine->AddOnScreenDebugMessage(Key++, 0.0f, Colour, Text);
	};

	Line(FColor::Cyan, TEXT("--- GeoWorld | Fase 9: acqua ---"));
	if (!Streaming || !Streaming->IsDatasetOpen())
	{
		Line(FColor::Yellow, TEXT("Nessun dataset d'acqua: geo.Water.Open <cartella> (python run.py build-water)"));
	}
	else
	{
		const FGeoWaterDataset& Dataset = Streaming->GetDataset();
		Line(FColor::White, FString::Printf(TEXT("Dataset    : '%s', livelli %d..%d, %lld tile, regole %d, geoide %s"),
			*Dataset.GetDatasetName(), Dataset.GetMinLevel(), Dataset.GetMaxLevel(),
			static_cast<long long>(Dataset.GetIndexedTileCount()), Dataset.GetRulesVersion(),
			Dataset.HasGeoid() ? TEXT("si'") : TEXT("no")));
		Line(FColor::White, FString::Printf(TEXT("Cartella   : %s"), *Dataset.GetRootDirectory()));
		const FString Stale = Dataset.GetStaleReason();
		if (!Stale.IsEmpty()) { Line(FColor::Red, Stale); }
	}
	if (!bEnabled) { Line(FColor::Yellow, TEXT("Acqua spenta (geo.Water.Enable 1)")); }

	const FString Problem = GetMaterialProblem();
	if (!Problem.IsEmpty()) { Line(FColor::Red, Problem); }

	Line(Stats.TileInAttesa > 0 ? FColor::Yellow : FColor::Green, FString::Printf(
		TEXT("Superfici   : %d tile (%d solo scavo), %d in attesa, %d in costruzione (%.1f ms l'una), +%d questo frame   %.1f k triangoli"),
		Stats.TileConAcqua, Stats.TileVuote, Stats.TileInAttesa, Stats.CostruzioniInCorso, Stats.MillisecondiPerCostruzione,
		Stats.ConsegneQuestoFrame, Stats.Triangoli / 1000.0f));

	const Weather::FWind Wind = Waves.GetWind();
	Line(FColor::White, FString::Printf(TEXT("Vento       : %.1f m/s da %.0f gradi   mare: Hs %.2f m, onda di picco %.0f m, schiuma %.0f%%"),
		Wind.SpeedMs, Wind.FromDegrees, Water::SignificantWaveHeight(Wind.SpeedMs), Water::PeakWavelength(FMath::Max(Wind.SpeedMs, 0.5)),
		100.0 * Water::WhitecapAmount(Wind.SpeedMs)));   /* percentuale, non-unita */
	if (Waves.GetSwappingCount() > 0)
	{
		Line(FColor::Yellow, FString::Printf(TEXT("Mare        : %d onde stanno cambiando (sfumano e rinascono col vento nuovo)"),
			Waves.GetSwappingCount()));
	}

	if (Streaming)
	{
		const FGeoWaterStreamingStats Loaded = Streaming->GetStats();
		Line(FColor::White, FString::Printf(TEXT("Tile d'acqua: %d in cache (%.1f/%.0f MB), %d in volo, %.2f ms a lettura   errori %d"),
			Loaded.TileResidenti, Loaded.MemoriaMB, Loaded.BudgetMB, Loaded.TileInVolo, Loaded.TempoMedioCaricamentoMs,
			Loaded.ErroriDiCaricamento));
	}
}

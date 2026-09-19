#include "Terrain/GeoTerrainSubsystem.h"

#include "GeoCoreModule.h"
#include "Georeference/GeoreferenceSubsystem.h"
#include "Lod/GeoQuadtreeSubsystem.h"
#include "Streaming/GeoTileStreamingSubsystem.h"
#include "Terrain/DynamicMeshTerrainProvider.h"
#include "Geo/GeoUnits.h"

#include "Engine/Engine.h"
#include "Engine/World.h"

using namespace GeoWorld;
using GeoWorld::Tiles::FTileKey;

namespace
{
	FORCEINLINE uint64 PackKey(const FTileKey& Key)
	{
		return (static_cast<uint64>(Key.Level) << 58) ^ (static_cast<uint64>(Key.Y) << 29)
		     ^ static_cast<uint64>(Key.X);
	}
}

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

	if (Provider.IsValid()) { Provider->Shutdown(); }
	Provider.Reset();
	BuiltTiles.Empty();

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
		if (Quadtree) { Quadtree->SetEnabled(true); }
	}
	else
	{
		RebuildAll();
	}
}

void UGeoTerrainSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	if (bTerrainEnabled) { SynchroniseWithSelection(); }
	if (bShowDebugOverlay) { DrawDebugOverlay(); }
}

void UGeoTerrainSubsystem::SynchroniseWithSelection()
{
	if (!Provider.IsValid() || !Quadtree || !Streaming) { return; }

	UGeoreferenceSubsystem* Georeference = GetWorld()->GetSubsystem<UGeoreferenceSubsystem>();
	if (!Georeference) { return; }

	const FGeoreferenceSnapshot Snapshot = Georeference->GetSnapshot();
	const TArray<Quadtree::FSelectedTile>& Selected = Quadtree->GetSelectedTiles();

	Stats.CostruiteQuestoFrame = 0;
	Stats.RimosseQuestoFrame = 0;

	// --- Chi dovrebbe esserci ---------------------------------------------
	TSet<uint64> Wanted;
	Wanted.Reserve(Selected.Num());
	for (const Quadtree::FSelectedTile& Tile : Selected) { Wanted.Add(PackKey(Tile.Key)); }

	// Le tile ancora selezionate restano pinnate: sfrattarle dalla cache mentre
	// si stanno disegnando significherebbe ricaricarle subito dopo. E' il limite
	// che la Fase 4 aveva lasciato aperto.
	for (const Quadtree::FSelectedTile& Tile : Selected)
	{
		Streaming->SetTilePinned(Tile.Key, true);
	}

	// --- Via la geometria che non serve piu' -------------------------------
	//
	// Prima di costruire, non dopo: liberare i componenti che non servono
	// restituisce memoria subito e tiene piu' basso il picco.
	TArray<FBuiltTile> ToRemove;
	for (const TPair<uint64, FBuiltTile>& Pair : BuiltTiles)
	{
		if (!Wanted.Contains(Pair.Key)) { ToRemove.Add(Pair.Value); }
	}

	for (const FBuiltTile& Built : ToRemove)
	{
		Provider->RemoveTile(Built.Key);
		// Spinnata: ora puo' tornare a essere sfrattabile dalla cache.
		Streaming->SetTilePinned(Built.Key, false);
		BuiltTiles.Remove(PackKey(Built.Key));
	}

	// --- Costruisci cio' che manca, entro il budget ------------------------
	int32 Budget = MaxTilesPerFrame;
	int32 Waiting = 0;

	for (const Quadtree::FSelectedTile& Tile : Selected)
	{
		const uint64 Packed = PackKey(Tile.Key);
		if (BuiltTiles.Contains(Packed)) { continue; }

		if (Budget <= 0) { ++Waiting; continue; }

		const auto TileData = Streaming->FindLoadedTile(Tile.Key);
		if (!TileData) { ++Waiting; continue; }

		const double Started = FPlatformTime::Seconds();

		Mesh::FTileMeshData MeshData;
		Mesh::BuildTileMesh(*TileData, MeshParameters, MeshData);
		if (!MeshData.IsValid()) { continue; }

		// La trasformazione porta il frame locale della tile nello spazio di
		// Unreal. La SCALA fa la conversione metri -> unita': i vertici restano
		// in metri, quindi piccoli, e la regola del punto unico di conversione
		// resta rispettata.
		FTransform Transform = Snapshot.GetLocalNeuTransform(MeshData.Origin);
		Transform.SetScale3D(FVector(GeoWorld::Units::MetersToUu));

		if (Provider->CreateOrUpdateTile(Tile.Key, MeshData, Transform))
		{
			BuiltTiles.Add(Packed, FBuiltTile{ Tile.Key, static_cast<int32>(MeshData.TriangleCount) });
			++Stats.CostruiteQuestoFrame;
			--Budget;

			TotalBuildSeconds += FPlatformTime::Seconds() - Started;
			++BuildCount;
		}
	}

	Stats.RimosseQuestoFrame = ToRemove.Num();
	Stats.TileInAttesa = Waiting;
	Stats.TileConGeometria = Provider->GetTileCount();

	Stats.TriangoliTotali = 0;
	for (const TPair<uint64, FBuiltTile>& Pair : BuiltTiles)
	{
		Stats.TriangoliTotali += Pair.Value.TriangleCount;
	}

	// Stima: posizioni, normali e UV in float piu' gli indici.
	Stats.MemoriaGeometriaMB = static_cast<float>(
		Stats.TriangoliTotali * (3.0 * 4.0) / (1024.0 * 1024.0));
	Stats.TempoCostruzioneMediaMs = (BuildCount > 0)
		? static_cast<float>(TotalBuildSeconds / BuildCount * 1000.0) : 0.0f;
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

	Line(FColor::Green, FString::Printf(TEXT("Tile disegnate: %d   triangoli %d"),
		Stats.TileConGeometria, Stats.TriangoliTotali));

	// Se molte tile restano in attesa frame dopo frame, il budget non basta per
	// quanto in fretta ci si muove: il terreno si riempie in ritardo.
	Line(Stats.TileInAttesa > 0 ? FColor::Yellow : FColor::White,
		FString::Printf(TEXT("In attesa     : %d   (budget %d per frame)"),
			Stats.TileInAttesa, MaxTilesPerFrame));

	Line(FColor::White, FString::Printf(TEXT("Questo frame  : +%d  -%d"),
		Stats.CostruiteQuestoFrame, Stats.RimosseQuestoFrame));

	Line(Stats.TempoCostruzioneMediaMs > 8.0f ? FColor::Yellow : FColor::White,
		FString::Printf(TEXT("Costruzione   : %.2f ms per tile"), Stats.TempoCostruzioneMediaMs));

	Line(FColor::White, FString::Printf(TEXT("Gonne         : %s   rebase gestiti: %d"),
		IsSkirtEnabled() ? TEXT("on") : TEXT("OFF"), Stats.Rebase));
}

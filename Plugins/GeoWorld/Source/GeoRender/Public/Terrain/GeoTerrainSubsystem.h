// =============================================================================
//  GeoTerrainSubsystem.h -- Dalla selezione LOD alla geometria. STRATO: UNREAL.
// =============================================================================
#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"

#include "Mesh/TileMesh.h"
#include "Terrain/GeoTerrainMeshProvider.h"
#include "Tiles/TileKey.h"

#include "GeoTerrainSubsystem.generated.h"

class UGeoQuadtreeSubsystem;
class UGeoTileStreamingSubsystem;

USTRUCT()
struct FGeoTerrainStats
{
	GENERATED_BODY()

	UPROPERTY() int32 TileConGeometria = 0;
	UPROPERTY() int32 TileInAttesa = 0;
	UPROPERTY() int32 CostruiteQuestoFrame = 0;
	UPROPERTY() int32 RimosseQuestoFrame = 0;
	UPROPERTY() int32 TriangoliTotali = 0;
	UPROPERTY() float MemoriaGeometriaMB = 0.0f;
	UPROPERTY() float TempoCostruzioneMediaMs = 0.0f;
	UPROPERTY() int32 Rebase = 0;
};

/**
 * Trasforma la decisione del quadtree in geometria vera.
 *
 * =============================================================================
 *  IL BUDGET PER FRAME
 * =============================================================================
 *  Costruire la mesh di una tile significa 16.641 conversioni geodetiche piu'
 *  33.792 triangoli: qualche millisecondo. Farne dieci in un frame lo fa
 *  saltare. Percio' se ne costruisce un numero limitato per frame, e il resto
 *  aspetta: il terreno si riempie in qualche decimo di secondo invece di
 *  comparire tutto insieme facendo scattare l'immagine.
 *
 *  Nel frattempo non si vedono buchi, perche' il quadtree continua a
 *  selezionare il padre finche' i figli non sono pronti (regola della Fase 4).
 *
 *  La costruzione sta comunque sul game thread. E' il limite principale di
 *  questa fase, ed e' un limite VOLUTO per ora: FTileMeshData e BuildTileMesh
 *  sono C++ puro senza alcuno stato condiviso, quindi spostarli sul thread pool
 *  della Fase 3 e' un lavoro localizzato. Farlo adesso avrebbe aggiunto
 *  asincronia a una fase che ha gia' abbastanza modi di essere sbagliata.
 */
UCLASS()
class GEORENDER_API UGeoTerrainSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual bool IsTickableInEditor() const override { return true; }

	void SetEnabled(bool bInEnabled);
	bool IsEnabled() const { return bTerrainEnabled; }

	void SetMaxTilesPerFrame(int32 Count) { MaxTilesPerFrame = FMath::Clamp(Count, 1, 64); }
	int32 GetMaxTilesPerFrame() const { return MaxTilesPerFrame; }

	void SetSkirtEnabled(bool bInSkirt);
	bool IsSkirtEnabled() const { return MeshParameters.bGenerateSkirt; }

	/** Inverte l'orientamento dei triangoli. Se il terreno e' invisibile
	 *  dall'alto e visibile da sotto, e' questo. */
	void SetFlipWinding(bool bInFlip);
	bool IsFlipWinding() const { return MeshParameters.bFlipWinding; }

	void SetWireframe(bool bInWireframe);
	bool IsWireframe() const { return Provider.IsValid() && Provider->IsWireframe(); }

	void SetDebugOverlayEnabled(bool bInShow) { bShowDebugOverlay = bInShow; }
	bool IsDebugOverlayEnabled() const { return bShowDebugOverlay; }

	/** Butta tutta la geometria e la ricostruisce. Utile dopo un cambio di parametri. */
	void RebuildAll();

	FGeoTerrainStats GetStats() const { return Stats; }
	FString GetProviderName() const { return Provider.IsValid() ? Provider->GetName() : TEXT("nessuno"); }

private:
	void SynchroniseWithSelection();
	void DrawDebugOverlay();
	void OnGeoreferenceRebased(const FGeoreferenceSnapshot& Snapshot);

	TUniquePtr<IGeoTerrainMeshProvider> Provider;

	UPROPERTY(Transient) TObjectPtr<UGeoQuadtreeSubsystem> Quadtree;
	UPROPERTY(Transient) TObjectPtr<UGeoTileStreamingSubsystem> Streaming;

	/**
	 * Tile che hanno geometria costruita.
	 *
	 * Si conserva la CHIAVE COMPLETA e non solo il conteggio: la chiave
	 * impacchettata usata per la mappa non e' invertibile, e senza quella vera
	 * non si potrebbe dire al provider quale componente distruggere ne' al
	 * loader quale tile spinnare.
	 */
	struct FBuiltTile
	{
		GeoWorld::Tiles::FTileKey Key;
		int32 TriangleCount = 0;
	};
	TMap<uint64, FBuiltTile> BuiltTiles;

	GeoWorld::Mesh::FTileMeshParameters MeshParameters;
	FGeoTerrainStats Stats;
	FDelegateHandle RebaseHandle;

	int32 MaxTilesPerFrame = 4;
	bool bTerrainEnabled = false;
	bool bShowDebugOverlay = false;

	int32 BuildCount = 0;
	double TotalBuildSeconds = 0.0;
};

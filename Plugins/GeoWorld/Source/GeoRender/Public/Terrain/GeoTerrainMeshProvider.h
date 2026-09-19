// =============================================================================
//  GeoTerrainMeshProvider.h -- L'interfaccia dietro cui sta il disegno.
//  STRATO: UNREAL.
// =============================================================================
#pragma once

#include "CoreMinimal.h"

#include "Georeference/GeoreferenceSnapshot.h"
#include "Mesh/TileMesh.h"
#include "Tiles/TileKey.h"

/**
 * =============================================================================
 *  PERCHE' UN'INTERFACCIA
 * =============================================================================
 *  Era un requisito esplicito: partire da UDynamicMeshComponent per semplicita',
 *  ma poter passare a un FPrimitiveSceneProxy custom senza riscrivere il resto.
 *
 *  La divisione e' questa: chi sta SOPRA (UGeoTerrainSubsystem) decide quali
 *  tile devono esistere, quando costruirne la geometria e quando buttarla; chi
 *  sta SOTTO sa soltanto trasformare FTileMeshData in qualcosa che il renderer
 *  disegna. Il primo non nomina mai un tipo di Unreal legato al disegno, il
 *  secondo non sa niente di quadtree, LOD o streaming.
 *
 *  UDynamicMeshComponent e' comodo ma paga: un componente per tile significa un
 *  UObject, un proxy di scena e un draw call per tile, piu' un aggiornamento che
 *  passa per il game thread. Con qualche centinaio di tile diventa il collo di
 *  bottiglia, ed e' il momento in cui si scrive un secondo provider.
 *
 * =============================================================================
 *  RefreshTransforms E IL REBASING
 * =============================================================================
 *  E' il metodo che rende esplicito il guadagno del frame locale: quando
 *  l'origine del mondo cambia, NON si rigenera un solo vertice. Si ricalcolano
 *  soltanto le trasformazioni dei componenti, che sono una manciata di double
 *  per tile.
 */
class GEORENDER_API IGeoTerrainMeshProvider
{
public:
	virtual ~IGeoTerrainMeshProvider() = default;

	/** Nome leggibile, per l'overlay di debug. */
	virtual FString GetName() const = 0;

	virtual void Initialize(UWorld* World) = 0;
	virtual void Shutdown() = 0;

	/**
	 * Crea o aggiorna la geometria di una tile.
	 * `Transform` porta il frame locale della mesh nello spazio di Unreal.
	 */
	virtual bool CreateOrUpdateTile(const GeoWorld::Tiles::FTileKey& Key,
	                                const GeoWorld::Mesh::FTileMeshData& Mesh,
	                                const FTransform& Transform) = 0;

	virtual void RemoveTile(const GeoWorld::Tiles::FTileKey& Key) = 0;
	virtual void RemoveAllTiles() = 0;

	/** Ricalcola le trasformazioni dopo un rebase. Nessun vertice viene toccato. */
	virtual void RefreshTransforms(const FGeoreferenceSnapshot& Snapshot) = 0;

	virtual int32 GetTileCount() const = 0;
	virtual void SetWireframe(bool bInWireframe) = 0;
	virtual bool IsWireframe() const = 0;
};

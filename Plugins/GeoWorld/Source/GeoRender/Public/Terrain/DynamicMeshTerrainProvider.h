// =============================================================================
//  DynamicMeshTerrainProvider.h -- Provider basato su UDynamicMeshComponent.
//  STRATO: UNREAL.
// =============================================================================
#pragma once

#include "CoreMinimal.h"
#include "Terrain/GeoTerrainMeshProvider.h"

class AActor;
class UDynamicMeshComponent;
class UMaterialInterface;

/**
 * Un UDynamicMeshComponent per tile, tutti figli di un attore contenitore.
 *
 * E' il provider "di partenza": si scrive in poche righe e si vede subito se la
 * geometria e' giusta. Non e' quello definitivo — vedi la nota
 * sull'interfaccia in GeoTerrainMeshProvider.h.
 */
class GEORENDER_API FDynamicMeshTerrainProvider : public IGeoTerrainMeshProvider
{
public:
	virtual FString GetName() const override { return TEXT("UDynamicMeshComponent"); }

	virtual void Initialize(UWorld* World) override;
	virtual void Shutdown() override;

	virtual bool CreateOrUpdateTile(const GeoWorld::Tiles::FTileKey& Key,
	                                const GeoWorld::Mesh::FTileMeshData& Mesh,
	                                const FTransform& Transform) override;

	virtual void RemoveTile(const GeoWorld::Tiles::FTileKey& Key) override;
	virtual void RemoveAllTiles() override;
	virtual void RefreshTransforms(const FGeoreferenceSnapshot& Snapshot) override;

	virtual int32 GetTileCount() const override { return Tiles.Num(); }
	virtual void SetWireframe(bool bInWireframe) override;
	virtual bool IsWireframe() const override { return bWireframe; }

private:
	struct FTileEntry
	{
		TWeakObjectPtr<UDynamicMeshComponent> Component;
		/** Origine geodetica del frame locale: serve per ricalcolare la
		 *  trasformazione dopo un rebase, senza toccare i vertici. */
		GeoWorld::Core::FGeodetic Origin;
	};

	static uint64 PackKey(const GeoWorld::Tiles::FTileKey& Key)
	{
		return (static_cast<uint64>(Key.Level) << 58) ^ (static_cast<uint64>(Key.Y) << 29)
		     ^ static_cast<uint64>(Key.X);
	}

	TWeakObjectPtr<AActor> Container;
	TWeakObjectPtr<UMaterialInterface> Material;
	TMap<uint64, FTileEntry> Tiles;
	bool bWireframe = false;
};

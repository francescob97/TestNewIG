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

	virtual int32 GetRealizedTriangleCount() const override;
	virtual void GetDiagnostics(TArray<FGeoTerrainTileDiagnostic>& Out,
	                            int32 MaxEntries) const override;

private:
	struct FTileEntry
	{
		TWeakObjectPtr<UDynamicMeshComponent> Component;
		/** Origine geodetica del frame locale: serve per ricalcolare la
		 *  trasformazione dopo un rebase, senza toccare i vertici. */
		GeoWorld::Core::FGeodetic Origin;
		/** La chiave INTERA. PackKey non e' invertibile, e tenere solo quella
		 *  significa non poter piu' dire di quale tile si sta parlando: e' gia'
		 *  costato un bug di rimozione nel subsystem. */
		GeoWorld::Tiles::FTileKey Key;
	};

	static uint64 PackKey(const GeoWorld::Tiles::FTileKey& Key)
	{
		return (static_cast<uint64>(Key.Level) << 58) ^ (static_cast<uint64>(Key.Y) << 29)
		     ^ static_cast<uint64>(Key.X);
	}

	TWeakObjectPtr<AActor> Container;

	// NOTA UE: puntatore FORTE, non debole. Questa classe non e' un UObject,
	// quindi non puo' dichiarare una UPROPERTY, e il materiale caricato con
	// LoadObject non ha nessun altro che lo tenga in vita: con un TWeakObjectPtr
	// il garbage collector se lo porterebbe via al primo passaggio e le tile
	// costruite dopo resterebbero con il materiale di default, senza un errore.
	// TStrongObjectPtr e' il modo corretto per un oggetto non-UObject di
	// dichiarare al GC che quel riferimento conta.
	TStrongObjectPtr<UMaterialInterface> Material;
	TMap<uint64, FTileEntry> Tiles;
	bool bWireframe = false;
};

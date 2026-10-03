// =============================================================================
//  DynamicMeshTerrainProvider.h -- Provider basato su UDynamicMeshComponent.
//  STRATO: UNREAL.
// =============================================================================
#pragma once

#include "CoreMinimal.h"
#include "Terrain/GeoTerrainMeshProvider.h"
#include "UObject/StrongObjectPtr.h"

class AActor;
class UDynamicMeshComponent;
class UMaterialInstanceDynamic;
class UMaterialInterface;
class UTexture2D;

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

	virtual FGeoPrepareTileMeshFunction GetPrepareFunction() const override { return &PrepareTileMesh; }
	virtual bool CommitPreparedTile(const GeoWorld::Tiles::FTileKey& Key,
	                                FGeoPreparedTileMesh& Prepared,
	                                const FTransform& Transform) override;

	/** Thread-safe: costruisce la FDynamicMesh3, senza toccare UObject. */
	static FGeoPreparedTileMeshPtr PrepareTileMesh(const GeoWorld::Mesh::FTileMeshData& Mesh);

	virtual void RemoveTile(const GeoWorld::Tiles::FTileKey& Key) override;
	virtual void RemoveAllTiles() override;
	virtual void SetCastShadows(bool bInCastShadows) override;
	virtual bool IsCastingShadows() const override { return bCastShadows; }
	virtual void SetTileVisible(const GeoWorld::Tiles::FTileKey& Key, bool bVisible) override;
	virtual void RefreshTransforms(const FGeoreferenceSnapshot& Snapshot) override;

	virtual int32 GetTileCount() const override { return Tiles.Num(); }
	virtual void SetWireframe(bool bInWireframe) override;
	virtual bool IsWireframe() const override { return bWireframe; }

	virtual void SetTileDrape(const GeoWorld::Tiles::FTileKey& Key,
	                          UTexture2D* Texture,
	                          const GeoWorld::Imagery::FDrapeTransform& Drape) override;
	virtual int32 GetDrapedTileCount() const override;
	virtual bool IsTileDraped(const GeoWorld::Tiles::FTileKey& Key) const override
	{
		const FTileEntry* Entry = Tiles.Find(Key.Pack());
		return Entry && Entry->bDraped;
	}
	virtual FString GetMaterialProblem() const override { return MaterialProblem; }

	virtual void SetTileOverlay(const GeoWorld::Tiles::FTileKey& Key,
	                            UTexture2D* Texture,
	                            const GeoWorld::Imagery::FDrapeTransform& Window) override;
	virtual bool IsTileOverlaid(const GeoWorld::Tiles::FTileKey& Key) const override
	{
		const FTileEntry* Entry = Tiles.Find(Key.Pack());
		return Entry && Entry->bOverlaid;
	}
	virtual int32 GetOverlaidTileCount() const override;
	virtual void SetOverlayStrength(float Strength) override;
	virtual FString GetOverlayMaterialProblem() const override { return OverlayMaterialProblem; }

	virtual bool CommitRoadMesh(const GeoWorld::Tiles::FTileKey& Key, FGeoPreparedTileMesh& Prepared) override;
	virtual void RemoveRoadMesh(const GeoWorld::Tiles::FTileKey& Key) override;
	virtual bool HasRoadMesh(const GeoWorld::Tiles::FTileKey& Key) const override
	{
		const FTileEntry* Entry = Tiles.Find(Key.Pack());
		return Entry && Entry->RoadComponent.IsValid();
	}
	virtual int32 GetRoadMeshCount() const override;
	virtual int32 GetRoadTriangleCount() const override;
	virtual void SetRoadAtlas(UTexture2D* Atlas) override;
	virtual bool HasRoadAtlas() const override { return RoadMaterialInstance.IsValid() || !RoadMaterial.IsValid(); }
	virtual FString GetRoadMaterialProblem() const override { return RoadMaterialProblem; }

	virtual void SetTileMorph(const GeoWorld::Tiles::FTileKey& Key, float Morph) override;
	virtual void BeginTransitionFromParent(const GeoWorld::Tiles::FTileKey& Child,
	                                       const GeoWorld::Tiles::FTileKey& Parent) override;
	virtual void TickTransitions(double NowSeconds) override;
	virtual void SetTransitionSeconds(float Seconds) override { TransitionSeconds = FMath::Max(0.0f, Seconds); }
	virtual float GetTransitionSeconds() const override { return TransitionSeconds; }
	virtual int32 GetTransitionCount() const override { return Transitioning.Num(); }
	virtual FString GetTransitionMaterialProblem() const override { return TransitionMaterialProblem; }

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

		/** Istanza dinamica del materiale: una per tile, perche' ognuna ha la
		 *  propria texture e il proprio ritaglio. Creata solo quando serve. */
		TWeakObjectPtr<UMaterialInstanceDynamic> Material;
		bool bDraped = false;

		/** Ultimo drappeggio applicato: se non cambia, non si tocca il materiale. */
		TWeakObjectPtr<UTexture2D> DrapedTexture;
		GeoWorld::Imagery::FDrapeTransform DrapedTransform;

		/** Strade 3D: un secondo componente, nello stesso frame locale della tile. */
		TWeakObjectPtr<UDynamicMeshComponent> RoadComponent;

		/** Strade (Fase 8): stesso schema del drappeggio, su parametri propri. */
		bool bOverlaid = false;
		TWeakObjectPtr<UTexture2D> OverlayTexture;
		GeoWorld::Imagery::FDrapeTransform OverlayTransform;

		// --- Transizioni (vedi GeoTerrainMeshProvider.h) ---------------------
		/** La mesh porta i dati del geomorphing (UV 1 e 2). */
		bool bHasMorph = false;
		/** Morphing in corso: 1 -> 0 da MorphStart. Negativo = fermo. */
		double MorphStart = -1.0;
		/** Dissolvenza in corso: peso della foto precedente 1 -> 0. Negativo = ferma. */
		double FadeStart = -1.0;
		/** Ultimo valore scritto nei Custom Primitive Data, per non riscriverlo. */
		float MorphValue = 0.0f;
		float FadeValue = 0.0f;
	};

	/** Lo stato di vestizione di una tile, come la vede il materiale. */
	struct FDressState
	{
		TWeakObjectPtr<UTexture2D> Drape;
		GeoWorld::Imagery::FDrapeTransform DrapeTransform;
		TWeakObjectPtr<UTexture2D> Overlay;
		GeoWorld::Imagery::FDrapeTransform OverlayTransform;
	};

	/** Cosa ha a schermo adesso la tile (per farla diventare la "precedente"). */
	static FDressState CaptureDress(const FTileEntry& Entry);

	/** Fa partire la dissolvenza da `Previous` verso lo stato attuale della tile. */
	void StartFade(FTileEntry& Entry, const FDressState& Previous);

	/** Scrive i Custom Primitive Data della tile (e della sua strada). */
	static void WriteTransitionData(FTileEntry& Entry, float Morph, float Fade);

	/** Chiude subito le transizioni della tile. */
	void FinishTransitions(FTileEntry& Entry);

	/** La foto "precedente" torna a puntare a quella attuale: libera la vecchia. */
	void ReleasePrevious(FTileEntry& Entry) const;

	/** Cerca i materiali che mancano e ricontrolla i parametri (vedi il .cpp). */
	void CheckMaterials(bool bLog);
	double LastMaterialCheckSeconds = 0.0;

	/** L'istanza di materiale della tile, creata se manca. Nullptr senza M_GeoTerrain. */
	UMaterialInstanceDynamic* EnsureDrapeInstance(FTileEntry& Entry, UDynamicMeshComponent* Component);

	/** Scrive sull'istanza i parametri delle strade che l'entry ricorda. */
	void ApplyOverlayParameters(const FTileEntry& Entry, UMaterialInstanceDynamic* Instance) const;

	TWeakObjectPtr<AActor> Container;

	// NOTA UE: puntatore FORTE, non debole. Questa classe non e' un UObject,
	// quindi non puo' dichiarare una UPROPERTY, e il materiale caricato con
	// LoadObject non ha nessun altro che lo tenga in vita: con un TWeakObjectPtr
	// il garbage collector se lo porterebbe via al primo passaggio e le tile
	// costruite dopo resterebbero con il materiale di default, senza un errore.
	// TStrongObjectPtr e' il modo corretto per un oggetto non-UObject di
	// dichiarare al GC che quel riferimento conta.
	TStrongObjectPtr<UMaterialInterface> Material;

	/** Materiale con i parametri del drappeggio. Manca finche' non lo si crea
	 *  (geo.Imagery.CreateMaterial, o a mano: vedi fase6-verifica.md). */
	TStrongObjectPtr<UMaterialInterface> DrapeMaterial;
	TMap<uint64, FTileEntry> Tiles;
	bool bWireframe = false;
	bool bCastShadows = false;

	/** Vuota se il materiale del drappeggio e' quello giusto. */
	FString MaterialProblem;

	/** Vuota se il materiale ha i parametri delle strade (Overlay, OverlayUv, OverlayStrength). */
	FString OverlayMaterialProblem;

	float OverlayStrength = 1.0f;

	/** Materiale delle strade 3D, e la sua istanza con l'atlante. */
	TStrongObjectPtr<UMaterialInterface> RoadMaterial;
	TStrongObjectPtr<UMaterialInstanceDynamic> RoadMaterialInstance;
	FString RoadMaterialProblem;

	/** Durata delle transizioni (geo.Terrain.Morph). */
	float TransitionSeconds = 0.6f;
	/** Le tile con una transizione in corso: TickTransitions guarda solo queste. */
	TSet<uint64> Transitioning;
	/** M_GeoTerrain ha i parametri della dissolvenza (BaseColorPrevious...)? */
	bool bFadeSupported = false;
	FString TransitionMaterialProblem;

	/** Il materiale da dare a un componente di strada: l'istanza, o il ripiego. */
	UMaterialInterface* GetRoadMaterialForComponent() const;
};

// =============================================================================
//  GeoTerrainMeshProvider.h -- L'interfaccia dietro cui sta il disegno.
//  STRATO: UNREAL.
// =============================================================================
#pragma once

#include "CoreMinimal.h"

#include "Georeference/GeoreferenceSnapshot.h"
#include "Imagery/ImageryMapping.h"
#include "Mesh/TileMesh.h"
#include "Tiles/TileKey.h"

class UTexture2D;

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
/**
 * Cosa il RENDERER ha davvero in mano per una tile.
 *
 * PERCHE' ESISTE. La prima versione dell'overlay contava i triangoli dalla
 * FTileMeshData, cioe' dal MIO lato: diceva "3.538.944 triangoli" anche se il
 * componente di Unreal non ne aveva ricevuto neanche uno. Un contatore che
 * misura l'intenzione invece del risultato non e' debug, e' rumore che nasconde
 * il problema. Questi numeri vengono letti dal componente vero.
 */
struct FGeoTerrainTileDiagnostic
{
	GeoWorld::Tiles::FTileKey Key;

	/** Posizione del componente in spazio Unreal (uu) e raggio dei suoi bounds. */
	FVector WorldLocation = FVector::ZeroVector;
	double BoundsRadiusUu = 0.0;

	/** Il volume che il RENDERER usa per il culling, in spazio mondo. */
	FVector BoundsOrigin = FVector::ZeroVector;
	FVector BoundsExtent = FVector::ZeroVector;

	/** Letti dalla mesh del componente, non dalla FTileMeshData. */
	int32 RealVertexCount = 0;
	int32 RealTriangleCount = 0;

	bool bRegistered = false;
	bool bVisible = false;
	FString MaterialName;
};

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

	/**
	 * Veste una tile con un pezzo di ortofoto.
	 *
	 * PERCHE' L'OFFSET E LA SCALA PASSANO DI QUI E NON FINISCONO NELLA MESH.
	 * La prima idea era scrivere le UV ritagliate direttamente nei vertici,
	 * cosi' da tenere il materiale banale. E' sbagliata: quando arriva
	 * un'immagine di livello piu' fine il ritaglio cambia, e con le UV nei
	 * vertici bisognerebbe riscrivere 17.157 coordinate di texture per ogni
	 * tile che si affina -- proprio mentre ci si sta muovendo, cioe' nel
	 * momento peggiore.
	 *
	 * Con un parametro vettoriale del materiale la stessa cosa costa
	 * l'assegnazione di quattro float, e la mesh non viene toccata mai. Il
	 * prezzo e' un nodo in piu' nel materiale.
	 *
	 * Passare `nullptr` come texture toglie il drappeggio e riporta la tile al
	 * materiale grigio.
	 */
	virtual void SetTileDrape(const GeoWorld::Tiles::FTileKey& Key,
	                          UTexture2D* Texture,
	                          const GeoWorld::Imagery::FDrapeTransform& Drape) = 0;

	/** Quante tile hanno davvero una texture addosso. */
	virtual int32 GetDrapedTileCount() const = 0;

	virtual int32 GetTileCount() const = 0;
	virtual void SetWireframe(bool bInWireframe) = 0;
	virtual bool IsWireframe() const = 0;

	/**
	 * Triangoli che il renderer ha davvero, sommati su tutte le tile.
	 * Se non coincide con quelli che il subsystem crede di aver costruito, il
	 * problema sta nel provider e non nella generazione della mesh.
	 */
	virtual int32 GetRealizedTriangleCount() const = 0;

	/** Diagnosi delle prime MaxEntries tile, per geo.Terrain.Diag. */
	virtual void GetDiagnostics(TArray<FGeoTerrainTileDiagnostic>& Out,
	                            int32 MaxEntries) const = 0;
};

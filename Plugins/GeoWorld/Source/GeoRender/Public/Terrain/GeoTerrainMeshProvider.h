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
 * Una mesh gia' convertita nella forma interna del provider, pronta da
 * consegnare al renderer. Opaca per chi sta sopra: la produce la funzione di
 * preparazione del provider e la consuma CommitPreparedTile dello stesso.
 *
 * PERCHE' ESISTE. Costruire la geometria di una tile ha due parti costose: la
 * geodesia (BuildTileMesh, C++ puro) e la conversione nella struttura del
 * provider (per UDynamicMeshComponent una FDynamicMesh3, con la sua topologia
 * di spigoli). Entrambe si possono fare su un thread di lavoro, perche' non
 * toccano UObject. Resta sul game thread solo la consegna al componente.
 */
struct FGeoPreparedTileMesh
{
	virtual ~FGeoPreparedTileMesh() = default;

	/** Origine del frame locale: serve per calcolare la trasformazione alla consegna. */
	GeoWorld::Core::FGeodetic Origin;
	int32 TriangleCount = 0;
};

using FGeoPreparedTileMeshPtr = TSharedPtr<FGeoPreparedTileMesh, ESPMode::ThreadSafe>;

/**
 * Funzione di preparazione: gira su un thread di lavoro.
 *
 * E' un puntatore a funzione LIBERA e non un metodo virtuale di proposito: un
 * lavoro in volo non deve tenere un puntatore al provider, che potrebbe
 * essere distrutto mentre il lavoro gira. Una funzione libera non ha stato.
 */
using FGeoPrepareTileMeshFunction = FGeoPreparedTileMeshPtr (*)(const GeoWorld::Mesh::FTileMeshData& Mesh);

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

	/** La funzione che prepara una mesh fuori dal game thread (vedi FGeoPreparedTileMesh). */
	virtual FGeoPrepareTileMeshFunction GetPrepareFunction() const = 0;

	/**
	 * Consegna al renderer una mesh preparata. GAME THREAD. `Prepared` deve
	 * venire dalla funzione di preparazione di QUESTO provider, e viene
	 * consumata (i dati si spostano, non si copiano).
	 */
	virtual bool CommitPreparedTile(const GeoWorld::Tiles::FTileKey& Key,
	                                FGeoPreparedTileMesh& Prepared,
	                                const FTransform& Transform) = 0;

	virtual void RemoveTile(const GeoWorld::Tiles::FTileKey& Key) = 0;
	virtual void RemoveAllTiles() = 0;

	/**
	 * Il terreno proietta ombre? Default NO, per due ragioni.
	 *
	 * 1. Le ortofoto le ombre le hanno gia': sono fotografie fatte con il sole.
	 *    Aggiungerne di calcolate le raddoppia, spesso in una direzione diversa.
	 * 2. Costano moltissimo: centinaia di mesh da decine di migliaia di
	 *    triangoli, non Nanite, disegnate di nuovo nelle mappe d'ombra (le
	 *    Virtual Shadow Maps di UE5 in particolare soffrono la geometria non
	 *    Nanite). Su un portatile e' spesso la voce piu' pesante del frame.
	 */
	virtual void SetCastShadows(bool bInCastShadows) = 0;
	virtual bool IsCastingShadows() const = 0;

	/**
	 * Mostra o nasconde una tile gia' costruita, senza distruggerla.
	 *
	 * PERCHE' ESISTE. Con il piano di residenza il terreno costruisce le mesh
	 * PRIMA che servano (dove la camera sta andando) e le tiene dopo che non
	 * servono piu' (se ci si torna, sono gia' pronte). In entrambi i casi la
	 * tile esiste ma non va disegnata: sovrapposta al padre o ai figli
	 * produrrebbe z-fighting. Nasconderla costa un flag; ricostruirla costa
	 * migliaia di conversioni geodetiche e la topologia della mesh.
	 */
	virtual void SetTileVisible(const GeoWorld::Tiles::FTileKey& Key, bool bVisible) = 0;

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

	/** Questa tile ha una texture addosso? Serve a non mostrarla grigia. */
	virtual bool IsTileDraped(const GeoWorld::Tiles::FTileKey& Key) const = 0;

	/**
	 * Mette sopra l'ortofoto una seconda immagine trasparente: le strade, le
	 * ferrovie e le piste della Fase 8.
	 *
	 * La texture e' in alfa PREMOLTIPLICATO (vedi Roads/RoadRasterizer.h) e il
	 * materiale la compone cosi':  colore = foto * (1 - alfa) + strade.
	 * `Window` ha lo stesso significato del drappeggio: identita' quando la
	 * texture e' stata disegnata per questa tile, un ritaglio quando e' quella
	 * di un antenato, usata in attesa della propria.
	 *
	 * Passare `nullptr` toglie le strade dalla tile.
	 */
	virtual void SetTileOverlay(const GeoWorld::Tiles::FTileKey& Key,
	                            UTexture2D* Texture,
	                            const GeoWorld::Imagery::FDrapeTransform& Window) = 0;

	/** Questa tile ha le strade addosso (proprie o di un antenato)? */
	virtual bool IsTileOverlaid(const GeoWorld::Tiles::FTileKey& Key) const = 0;

	/** Quante tile hanno le strade addosso. */
	virtual int32 GetOverlaidTileCount() const = 0;

	/** Quanto si vedono le strade: 0 = per niente, 1 = come disegnate. Vale per tutte. */
	virtual void SetOverlayStrength(float Strength) = 0;

	/** Come GetMaterialProblem, per i parametri delle strade (vuota se a posto). */
	virtual FString GetOverlayMaterialProblem() const { return FString(); }

	// --- Strade 3D ---------------------------------------------------------
	//
	// Una seconda mesh per tile: i nastri delle strade (Roads/RoadMesh.h),
	// costruiti nello STESSO frame locale della tile. Il provider la tratta
	// come parte della tile: stessa trasformazione, stessa visibilita', stesso
	// rebase, e sparisce con lei. Cosi' una strada non puo' restare a schermo
	// senza il suo terreno, ne' il terreno comparire senza la sua strada per
	// colpa di chi aggiorna cosa per primo.

	/** Consegna la mesh delle strade di una tile ESISTENTE. Preparata con GetPrepareFunction(). */
	virtual bool CommitRoadMesh(const GeoWorld::Tiles::FTileKey& Key, FGeoPreparedTileMesh& Prepared) = 0;
	virtual void RemoveRoadMesh(const GeoWorld::Tiles::FTileKey& Key) = 0;
	virtual bool HasRoadMesh(const GeoWorld::Tiles::FTileKey& Key) const = 0;
	virtual int32 GetRoadMeshCount() const = 0;
	virtual int32 GetRoadTriangleCount() const = 0;

	/** L'atlante delle superfici (asfalto, binari...): una texture per tutte le strade 3D. */
	virtual void SetRoadAtlas(UTexture2D* Atlas) = 0;

	/** L'atlante e' gia' stato dato (o non serve, perche' manca il materiale)? */
	virtual bool HasRoadAtlas() const = 0;

	/** Vuota se il materiale delle strade 3D (M_GeoRoad) c'e'. */
	virtual FString GetRoadMaterialProblem() const { return FString(); }

	/**
	 * Un problema noto del materiale del drappeggio, da mostrare a schermo, o
	 * stringa vuota. Esiste perche' un materiale sbagliato non da' errori: da'
	 * un'immagine sbagliata, e il primo sospettato non e' mai il materiale.
	 */
	virtual FString GetMaterialProblem() const { return FString(); }

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

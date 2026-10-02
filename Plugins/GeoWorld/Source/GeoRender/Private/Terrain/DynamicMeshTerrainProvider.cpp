#include "Terrain/DynamicMeshTerrainProvider.h"

#include "GeoCoreModule.h"
#include "Geo/GeoUnits.h"

#include "Components/DynamicMeshComponent.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Engine/Texture2D.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "UObject/ConstructorHelpers.h"

using namespace GeoWorld;

// ---------------------------------------------------------------------------
//  NOTA UE: il wireframe di un UDynamicMeshComponent non si accende con un
//  setter dedicato sul componente -- quel metodo non esiste. Vive su
//  UBaseDynamicMeshComponent come proprieta' pubblica bExplicitShowWireframe
//  ("disegna il reticolo SOPRA la mesh ombreggiata"), accompagnata dagli
//  accessori virtuali SetEnableWireframeRenderPass/EnableWireframeRenderPass.
//
//  Qui si scrive la proprieta' e si invalida esplicitamente lo stato di
//  rendering: il proxy di scena viene costruito una volta e non rilegge da solo
//  i flag del componente, quindi senza MarkRenderStateDirty() il cambiamento si
//  vedrebbe solo al prossimo aggiornamento della mesh -- cioe' il comando
//  sembrerebbe non fare niente finche' non ci si muove.
// ---------------------------------------------------------------------------
static void ApplyWireframe(UDynamicMeshComponent* Component, bool bInWireframe)
{
	if (!Component || Component->bExplicitShowWireframe == bInWireframe) { return; }

	Component->bExplicitShowWireframe = bInWireframe;
	Component->MarkRenderStateDirty();
}

void FDynamicMeshTerrainProvider::Initialize(UWorld* World)
{
	if (!World) { return; }

	// Un attore contenitore, cosi' tutte le tile stanno sotto un solo nodo del
	// World Outliner e si cancellano insieme.
	// NOTA UE: NON si impone un nome fisso all'attore. Se quel nome risultasse
	// gia' occupato -- una Initialize chiamata due volte, un attore transient
	// non ancora raccolto -- SpawnActor restituirebbe nullptr e il terreno non
	// comparirebbe mai, senza un solo messaggio di errore. Il nome leggibile
	// nel World Outliner lo da' SetActorLabel qui sotto, che non ha vincoli di
	// unicita'.
	FActorSpawnParameters Parameters;
	Parameters.ObjectFlags |= RF_Transient;   // non finisce nel livello salvato

	AActor* Actor = World->SpawnActor<AActor>(AActor::StaticClass(), Parameters);
	if (!Actor) { return; }

	Actor->SetRootComponent(NewObject<USceneComponent>(Actor, TEXT("Root")));
	Actor->GetRootComponent()->SetMobility(EComponentMobility::Movable);
	Actor->GetRootComponent()->RegisterComponent();
#if WITH_EDITOR
	Actor->SetActorLabel(TEXT("GeoWorld Terrain"));
#endif
	Container = Actor;

	// Materiale di base del motore: serve solo a vedere la geometria. Il
	// materiale vero arriva in Fase 6, quando ci sara' l'imagery da mostrare.
	Material.Reset(LoadObject<UMaterialInterface>(
		nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial")));

	// Il materiale del drappeggio vive nel Content del plugin. Non esiste finche'
	// non lo si crea, perche' un .uasset e' un file binario: non puo' nascere da
	// una riga di codice sorgente. Se manca, il terreno resta grigio e il
	// messaggio dice come ottenerlo, invece di lasciare l'utente davanti a un
	// drappeggio che "non funziona".
	DrapeMaterial.Reset(LoadObject<UMaterialInterface>(
		nullptr, TEXT("/GeoWorld/Materials/M_GeoTerrain.M_GeoTerrain")));

	// Il materiale c'e', ma e' quello giusto? La versione con il bug dell'offset
	// V non ha il parametro DrapeUv (si chiamava UvOffsetScale). Si controlla
	// qui, una volta, e lo si dice chiaramente: altrimenti il sintomo e' un
	// mosaico di pezzi di immagine fuori posto, che non fa pensare al materiale.
	if (UMaterialInterface* Drape = DrapeMaterial.Get())
	{
		FLinearColor Unused;
		if (!Drape->GetVectorParameterValue(FHashedMaterialParameterInfo(TEXT("DrapeUv")), Unused))
		{
			MaterialProblem = TEXT("M_GeoTerrain e' la versione VECCHIA (bug dell'offset V): ")
			                  TEXT("rifallo con geo.Imagery.CreateMaterial");
			UE_LOG(LogGeoWorld, Warning, TEXT("[GeoTerrain] %s"), *MaterialProblem);
		}

		// Fase 8: le strade vogliono tre parametri in piu'. Un M_GeoTerrain fatto
		// prima della Fase 8 non li ha, e le strade semplicemente non si
		// vedrebbero, senza un errore: lo si dice qui.
		UTexture* UnusedTexture = nullptr;
		if (!Drape->GetTextureParameterValue(FHashedMaterialParameterInfo(TEXT("Overlay")), UnusedTexture))
		{
			OverlayMaterialProblem = TEXT("M_GeoTerrain non ha i parametri delle strade (Overlay): ")
			                         TEXT("rifallo con geo.Imagery.CreateMaterial");
			UE_LOG(LogGeoWorld, Warning, TEXT("[GeoTerrain] %s"), *OverlayMaterialProblem);
		}
	}

	if (!DrapeMaterial.IsValid())
	{
		UE_LOG(LogGeoWorld, Warning,
			TEXT("[GeoTerrain] manca /GeoWorld/Materials/M_GeoTerrain: le ortofoto non ")
			TEXT("si vedranno. Crealo con geo.Imagery.CreateMaterial, oppure a mano ")
			TEXT("seguendo docs/fase6-verifica.md."));
	}

	UE_LOG(LogGeoWorld, Log, TEXT("[GeoTerrain] Provider '%s' pronto."), *GetName());
}

void FDynamicMeshTerrainProvider::Shutdown()
{
	RemoveAllTiles();
	if (AActor* Actor = Container.Get()) { Actor->Destroy(); }
	Container = nullptr;
}

namespace
{
	/** La mesh preparata di questo provider: una FDynamicMesh3 pronta da consegnare. */
	struct FDynamicMeshPrepared : public FGeoPreparedTileMesh
	{
		UE::Geometry::FDynamicMesh3 Mesh;
	};
}

FGeoPreparedTileMeshPtr FDynamicMeshTerrainProvider::PrepareTileMesh(const Mesh::FTileMeshData& MeshData)
{
	if (!MeshData.IsValid()) { return nullptr; }

	// ------------------------------------------------------------------
	//  NOTA UE: una FDynamicMesh3 e' una struttura dati di GeometryCore, non
	//  un UObject. Si puo' costruire su qualunque thread, purche' nessun altro
	//  la tocchi nel frattempo: qui e' locale al lavoro, quindi e' cosi' per
	//  costruzione. E' la parte costosa della consegna (la topologia degli
	//  spigoli si costruisce a ogni AppendTriangle), ed e' il motivo per cui
	//  conviene farla qui e non sul game thread.
	// ------------------------------------------------------------------
	TSharedPtr<FDynamicMeshPrepared, ESPMode::ThreadSafe> Prepared =
		MakeShared<FDynamicMeshPrepared, ESPMode::ThreadSafe>();
	Prepared->Origin = MeshData.Origin;
	Prepared->TriangleCount = static_cast<int32>(MeshData.TriangleCount);

	using namespace UE::Geometry;
	FDynamicMesh3& Mesh = Prepared->Mesh;
	Mesh.EnableAttributes();

	FDynamicMeshNormalOverlay* Normals = Mesh.Attributes()->PrimaryNormals();
	FDynamicMeshUVOverlay* UVs = Mesh.Attributes()->PrimaryUV();

	const int32 VertexCount = static_cast<int32>(MeshData.Positions.size() / 3);

	for (int32 Index = 0; Index < VertexCount; ++Index)
	{
		// I vertici arrivano in METRI. La conversione in unita' Unreal non si
		// fa qui: e' nella SCALA della trasformazione del componente, cosi' i
		// float restano piccoli e la regola del punto unico di conversione
		// resta valida.
		Mesh.AppendVertex(FVector3d(MeshData.Positions[Index * 3 + 0],
		                            MeshData.Positions[Index * 3 + 1],
		                            MeshData.Positions[Index * 3 + 2]));

		Normals->AppendElement(FVector3f(MeshData.Normals[Index * 3 + 0],
		                                 MeshData.Normals[Index * 3 + 1],
		                                 MeshData.Normals[Index * 3 + 2]));

		UVs->AppendElement(FVector2f(MeshData.UVs[Index * 2 + 0],
		                             MeshData.UVs[Index * 2 + 1]));
	}

	const int32 TriangleCount = static_cast<int32>(MeshData.Indices.size() / 3);
	for (int32 Index = 0; Index < TriangleCount; ++Index)
	{
		const int32 A = static_cast<int32>(MeshData.Indices[Index * 3 + 0]);
		const int32 B = static_cast<int32>(MeshData.Indices[Index * 3 + 1]);
		const int32 C = static_cast<int32>(MeshData.Indices[Index * 3 + 2]);

		const int32 TriangleId = Mesh.AppendTriangle(A, B, C);
		if (TriangleId >= 0)
		{
			// Normali e UV usano gli stessi indici dei vertici: ogni post ha
			// una normale sola, quindi non servono elementi separati.
			Normals->SetTriangle(TriangleId, FIndex3i(A, B, C));
			UVs->SetTriangle(TriangleId, FIndex3i(A, B, C));
		}
	}

	return Prepared;
}

bool FDynamicMeshTerrainProvider::CreateOrUpdateTile(
	const Tiles::FTileKey& Key, const Mesh::FTileMeshData& MeshData, const FTransform& Transform)
{
	// La via sincrona e' la stessa di quella asincrona, fatta tutta qui.
	const FGeoPreparedTileMeshPtr Prepared = PrepareTileMesh(MeshData);
	return Prepared.IsValid() && CommitPreparedTile(Key, *Prepared, Transform);
}

bool FDynamicMeshTerrainProvider::CommitPreparedTile(
	const Tiles::FTileKey& Key, FGeoPreparedTileMesh& Prepared, const FTransform& Transform)
{
	AActor* Actor = Container.Get();
	if (!Actor) { return false; }

	// Il cast e' sicuro per contratto: CommitPreparedTile riceve solo cio' che
	// ha prodotto la funzione di preparazione di questo stesso provider.
	FDynamicMeshPrepared& Ready = static_cast<FDynamicMeshPrepared&>(Prepared);

	const uint64 Packed = Key.Pack();
	FTileEntry& Entry = Tiles.FindOrAdd(Packed);
	Entry.Origin = Ready.Origin;
	Entry.Key = Key;

	UDynamicMeshComponent* Component = Entry.Component.Get();
	if (!Component)
	{
		Component = NewObject<UDynamicMeshComponent>(Actor);
		Component->SetupAttachment(Actor->GetRootComponent());
		Component->SetMobility(EComponentMobility::Movable);
		// Niente collisione: il terreno serve a essere guardato. Generarla per
		// 33.000 triangoli per tile costerebbe piu' della geometria stessa.
		Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);

		// Niente ombre (vedi SetCastShadows nell'interfaccia) e niente ray
		// tracing: con il ray tracing attivo nel progetto, ogni componente
		// costruirebbe la propria struttura di accelerazione, a ogni tile.
		// NOTA: se SetEnableRaytracing non compila sulla tua versione di UE,
		// togli la riga: e' un'ottimizzazione, non una necessita'.
		Component->SetCastShadow(bCastShadows);
		Component->SetEnableRaytracing(false);

		if (UMaterialInterface* BaseMaterial = Material.Get())
		{
			Component->SetMaterial(0, BaseMaterial);
		}
		ApplyWireframe(Component, bWireframe);
		Component->RegisterComponent();
		Entry.Component = Component;
	}

	// ------------------------------------------------------------------
	//  NOTA UE: SetMesh con uno spostamento (&&). La FDynamicMesh3 preparata
	//  sul worker passa al componente senza essere copiata: e' un paio di
	//  puntatori che cambiano proprietario. Il componente sa cosi' di dover
	//  ricostruire il proprio proxy di scena, che avverra' a fine frame.
	//
	//  La versione precedente costruiva la mesh qui dentro, con EditMesh: stessa
	//  geometria, ma tutto il costo sul game thread.
	// ------------------------------------------------------------------
	Component->SetMesh(MoveTemp(Ready.Mesh));
	Component->NotifyMeshUpdated();
	Component->SetWorldTransform(Transform);
	return true;
}

void FDynamicMeshTerrainProvider::SetCastShadows(bool bInCastShadows)
{
	if (bCastShadows == bInCastShadows) { return; }
	bCastShadows = bInCastShadows;
	for (TPair<uint64, FTileEntry>& Pair : Tiles)
	{
		if (UDynamicMeshComponent* Component = Pair.Value.Component.Get())
		{
			Component->SetCastShadow(bCastShadows);
		}
	}
}

void FDynamicMeshTerrainProvider::RemoveTile(const Tiles::FTileKey& Key)
{
	FTileEntry Entry;
	if (Tiles.RemoveAndCopyValue(Key.Pack(), Entry))
	{
		if (UDynamicMeshComponent* Component = Entry.Component.Get())
		{
			Component->DestroyComponent();
		}
	}
}

void FDynamicMeshTerrainProvider::SetTileVisible(const Tiles::FTileKey& Key, bool bVisible)
{
	const FTileEntry* Entry = Tiles.Find(Key.Pack());
	UDynamicMeshComponent* Component = Entry ? Entry->Component.Get() : nullptr;
	if (!Component || Component->GetVisibleFlag() == bVisible) { return; }

	// ------------------------------------------------------------------
	//  NOTA UE: SetVisibility e non SetHiddenInGame.
	//
	//  SetHiddenInGame vale solo in gioco: nel viewport dell'editor la tile
	//  resterebbe visibile, sovrapposta al padre, e il terreno "da fermo"
	//  sembrerebbe pieno di z-fighting. SetVisibility vale ovunque.
	//
	//  Cosa succede sotto: un componente invisibile viene TOLTO dalla scena
	//  del renderer (il suo proxy si distrugge) ma la FDynamicMesh3 resta
	//  intatta sul game thread. Rimostrarlo ricrea il proxy copiando i
	//  vertici nei buffer della scheda video: una copia lineare, molto meno
	//  della costruzione vera (geodesia + topologia della mesh). E' il motivo
	//  per cui conviene tenere le mesh invece di ricostruirle.
	// ------------------------------------------------------------------
	Component->SetVisibility(bVisible);
}

void FDynamicMeshTerrainProvider::RemoveAllTiles()
{
	for (TPair<uint64, FTileEntry>& Pair : Tiles)
	{
		if (UDynamicMeshComponent* Component = Pair.Value.Component.Get())
		{
			Component->DestroyComponent();
		}
	}
	Tiles.Empty();
}

void FDynamicMeshTerrainProvider::RefreshTransforms(const FGeoreferenceSnapshot& Snapshot)
{
	// IL punto della fase: dopo un rebase si ricalcolano solo le trasformazioni.
	// Nessun vertice viene toccato, perche' i vertici sono relativi al centro
	// della propria tile e quello non e' cambiato.
	for (TPair<uint64, FTileEntry>& Pair : Tiles)
	{
		UDynamicMeshComponent* Component = Pair.Value.Component.Get();
		if (!Component) { continue; }

		FTransform Transform = Snapshot.GetLocalNeuTransform(Pair.Value.Origin);
		Transform.SetScale3D(FVector(GeoWorld::Units::MetersToUu));
		Component->SetWorldTransform(Transform);
	}
}

void FDynamicMeshTerrainProvider::SetWireframe(bool bInWireframe)
{
	bWireframe = bInWireframe;
	for (TPair<uint64, FTileEntry>& Pair : Tiles)
	{
		if (UDynamicMeshComponent* Component = Pair.Value.Component.Get())
		{
			ApplyWireframe(Component, bWireframe);
		}
	}
}

// ---------------------------------------------------------------------------
//  Diagnostica: i numeri del RENDERER, non i miei.
// ---------------------------------------------------------------------------
int32 FDynamicMeshTerrainProvider::GetRealizedTriangleCount() const
{
	int32 Total = 0;
	for (const TPair<uint64, FTileEntry>& Pair : Tiles)
	{
		const UDynamicMeshComponent* Component = Pair.Value.Component.Get();
		if (!Component) { continue; }

		if (const UE::Geometry::FDynamicMesh3* Mesh = Component->GetMesh())
		{
			Total += Mesh->TriangleCount();
		}
	}
	return Total;
}

void FDynamicMeshTerrainProvider::GetDiagnostics(TArray<FGeoTerrainTileDiagnostic>& Out,
                                                 int32 MaxEntries) const
{
	Out.Reset();
	for (const TPair<uint64, FTileEntry>& Pair : Tiles)
	{
		if (Out.Num() >= MaxEntries) { break; }

		const UDynamicMeshComponent* Component = Pair.Value.Component.Get();
		if (!Component) { continue; }

		FGeoTerrainTileDiagnostic& Entry = Out.AddDefaulted_GetRef();
		Entry.Key = Pair.Value.Key;
		Entry.WorldLocation = Component->GetComponentLocation();

		// NOTA UE: Bounds e' il membro pubblico di USceneComponent aggiornato
		// dal motore, ed e' ESATTAMENTE il volume che il renderer usa per
		// decidere se la primitiva e' nel frustum. Se e' degenere (raggio zero)
		// la geometria viene scartata prima ancora di essere disegnata: e' la
		// causa classica di "i conteggi ci sono ma non si vede niente".
		Entry.BoundsRadiusUu = Component->Bounds.SphereRadius;
		Entry.BoundsOrigin = Component->Bounds.Origin;
		Entry.BoundsExtent = Component->Bounds.BoxExtent;

		if (const UE::Geometry::FDynamicMesh3* Mesh = Component->GetMesh())
		{
			Entry.RealVertexCount = Mesh->VertexCount();
			Entry.RealTriangleCount = Mesh->TriangleCount();
		}

		Entry.bRegistered = Component->IsRegistered();
		Entry.bVisible = Component->IsVisible();

		const UMaterialInterface* TileMaterial = Component->GetMaterial(0);
		Entry.MaterialName = TileMaterial ? TileMaterial->GetName() : TEXT("(nessuno)");
	}
}

// ---------------------------------------------------------------------------
//  Drappeggio
// ---------------------------------------------------------------------------

void FDynamicMeshTerrainProvider::SetTileDrape(const Tiles::FTileKey& Key,
                                               UTexture2D* Texture,
                                               const GeoWorld::Imagery::FDrapeTransform& Drape)
{
	FTileEntry* Entry = Tiles.Find(Key.Pack());
	if (!Entry) { return; }

	UDynamicMeshComponent* Component = Entry->Component.Get();
	if (!Component) { return; }

	// Togliere il drappeggio: si torna al materiale grigio di base. Ma se la
	// tile ha le strade addosso, l'istanza resta: si azzerano i suoi parametri
	// (la foto torna quella di default del materiale) e si rimettono le strade.
	if (!Texture)
	{
		if (Entry->bDraped)
		{
			Entry->bDraped = false;
			Entry->DrapedTexture.Reset();

			UMaterialInstanceDynamic* Instance = Entry->Material.Get();
			if (Entry->bOverlaid && Instance)
			{
				Instance->ClearParameterValues();
				ApplyOverlayParameters(*Entry, Instance);
			}
			else
			{
				if (UMaterialInterface* BaseMaterial = Material.Get())
				{
					Component->SetMaterial(0, BaseMaterial);
				}
				Entry->Material.Reset();
			}
		}
		return;
	}

	UMaterialInterface* Parent = DrapeMaterial.Get();
	if (!Parent) { return; }     // gia' segnalato in Initialize: non si insiste

	// Il subsystem delle ortofoto richiama questo metodo per tutte le tile a
	// ogni frame. Riassegnare gli stessi parametri a centinaia di istanze di
	// materiale non cambia l'immagine ma costa (ogni assegnazione aggiorna il
	// proxy di rendering del materiale): se niente e' cambiato, si esce.
	if (Entry->bDraped && Entry->DrapedTexture.Get() == Texture &&
	    Entry->DrapedTransform.OffsetU == Drape.OffsetU &&
	    Entry->DrapedTransform.OffsetV == Drape.OffsetV &&
	    Entry->DrapedTransform.Scale == Drape.Scale)
	{
		return;
	}

	UMaterialInstanceDynamic* Instance = EnsureDrapeInstance(*Entry, Component);
	if (!Instance) { return; }

	Instance->SetTextureParameterValue(TEXT("BaseColor"), Texture);

	// (offsetU, offsetV, scala, scala). Il materiale prende la scala dalla
	// componente B e l'offset da R e G attraverso una ComponentMask.
	//
	// La prima versione si chiamava UvOffsetScale e risparmiava proprio quella
	// maschera ("un nodo in meno nello shader"): il risultato era che l'offset
	// U finiva anche in V, e le tile vestite con un'immagine antenata
	// prendevano il quarto sbagliato. Un nodo risparmiato, un mosaico di
	// rettangoli fuori posto. Vedi GeoTerrainMaterialFactory.cpp.
	Instance->SetVectorParameterValue(TEXT("DrapeUv"),
		FLinearColor(Drape.OffsetU, Drape.OffsetV, Drape.Scale, Drape.Scale));

	Entry->bDraped = true;
	Entry->DrapedTexture = Texture;
	Entry->DrapedTransform = Drape;
}

int32 FDynamicMeshTerrainProvider::GetDrapedTileCount() const
{
	int32 Count = 0;
	for (const TPair<uint64, FTileEntry>& Pair : Tiles)
	{
		if (Pair.Value.bDraped) { ++Count; }
	}
	return Count;
}

// ---------------------------------------------------------------------------
//  Strade, ferrovie, piste (Fase 8)
// ---------------------------------------------------------------------------

UMaterialInstanceDynamic* FDynamicMeshTerrainProvider::EnsureDrapeInstance(FTileEntry& Entry,
                                                                           UDynamicMeshComponent* Component)
{
	if (UMaterialInstanceDynamic* Existing = Entry.Material.Get()) { return Existing; }

	UMaterialInterface* Parent = DrapeMaterial.Get();
	if (!Parent || !Component) { return nullptr; }

	// NOTA UE: l'outer dell'istanza e' il COMPONENTE, non l'attore. Cosi'
	// quando il componente viene distrutto l'istanza lo segue, senza doverla
	// liberare a mano e senza che il garbage collector la trovi orfana.
	UMaterialInstanceDynamic* Instance = UMaterialInstanceDynamic::Create(Parent, Component);
	if (!Instance) { return nullptr; }

	Entry.Material = Instance;
	Component->SetMaterial(0, Instance);
	return Instance;
}

void FDynamicMeshTerrainProvider::ApplyOverlayParameters(const FTileEntry& Entry,
                                                        UMaterialInstanceDynamic* Instance) const
{
	if (!Instance) { return; }
	if (!Entry.bOverlaid || !Entry.OverlayTexture.IsValid())
	{
		// Senza strade la forza va a zero: la texture di default del parametro
		// (quella del motore) non e' trasparente, e si vedrebbe.
		Instance->SetScalarParameterValue(TEXT("OverlayStrength"), 0.0f);
		return;
	}

	const GeoWorld::Imagery::FDrapeTransform& Window = Entry.OverlayTransform;
	Instance->SetTextureParameterValue(TEXT("Overlay"), Entry.OverlayTexture.Get());
	Instance->SetVectorParameterValue(TEXT("OverlayUv"),
		FLinearColor(Window.OffsetU, Window.OffsetV, Window.Scale, Window.Scale));
	Instance->SetScalarParameterValue(TEXT("OverlayStrength"), OverlayStrength);
}

void FDynamicMeshTerrainProvider::SetTileOverlay(const Tiles::FTileKey& Key, UTexture2D* Texture,
                                                 const GeoWorld::Imagery::FDrapeTransform& Window)
{
	FTileEntry* Entry = Tiles.Find(Key.Pack());
	if (!Entry) { return; }

	UDynamicMeshComponent* Component = Entry->Component.Get();
	if (!Component) { return; }

	if (!Texture)
	{
		if (!Entry->bOverlaid) { return; }
		Entry->bOverlaid = false;
		Entry->OverlayTexture.Reset();

		UMaterialInstanceDynamic* Instance = Entry->Material.Get();
		if (Entry->bDraped && Instance)
		{
			ApplyOverlayParameters(*Entry, Instance);     // forza a zero
		}
		else
		{
			// Ne' foto ne' strade: il materiale grigio di base, come prima.
			if (UMaterialInterface* BaseMaterial = Material.Get())
			{
				Component->SetMaterial(0, BaseMaterial);
			}
			Entry->Material.Reset();
		}
		return;
	}

	// Come per il drappeggio: chiamato per tutte le tile a ogni frame, tocca il
	// materiale solo se qualcosa e' cambiato.
	if (Entry->bOverlaid && Entry->OverlayTexture.Get() == Texture &&
	    Entry->OverlayTransform.OffsetU == Window.OffsetU &&
	    Entry->OverlayTransform.OffsetV == Window.OffsetV &&
	    Entry->OverlayTransform.Scale == Window.Scale)
	{
		return;
	}

	UMaterialInstanceDynamic* Instance = EnsureDrapeInstance(*Entry, Component);
	if (!Instance) { return; }     // manca M_GeoTerrain: gia' segnalato in Initialize

	Entry->bOverlaid = true;
	Entry->OverlayTexture = Texture;
	Entry->OverlayTransform = Window;
	ApplyOverlayParameters(*Entry, Instance);
}

int32 FDynamicMeshTerrainProvider::GetOverlaidTileCount() const
{
	int32 Count = 0;
	for (const TPair<uint64, FTileEntry>& Pair : Tiles)
	{
		if (Pair.Value.bOverlaid) { ++Count; }
	}
	return Count;
}

void FDynamicMeshTerrainProvider::SetOverlayStrength(float Strength)
{
	OverlayStrength = FMath::Clamp(Strength, 0.0f, 1.0f);
	for (TPair<uint64, FTileEntry>& Pair : Tiles)
	{
		if (Pair.Value.bOverlaid)
		{
			ApplyOverlayParameters(Pair.Value, Pair.Value.Material.Get());
		}
	}
}

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

bool FDynamicMeshTerrainProvider::CreateOrUpdateTile(
	const Tiles::FTileKey& Key, const Mesh::FTileMeshData& MeshData, const FTransform& Transform)
{
	AActor* Actor = Container.Get();
	if (!Actor || !MeshData.IsValid()) { return false; }

	const uint64 Packed = PackKey(Key);
	FTileEntry& Entry = Tiles.FindOrAdd(Packed);
	Entry.Origin = MeshData.Origin;
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
		if (UMaterialInterface* BaseMaterial = Material.Get())
		{
			Component->SetMaterial(0, BaseMaterial);
		}
		ApplyWireframe(Component, bWireframe);
		Component->RegisterComponent();
		Entry.Component = Component;
	}

	// ------------------------------------------------------------------
	//  NOTA UE: EditMesh prende una lambda che riceve la FDynamicMesh3 vera e
	//  propria. E' il modo corretto di modificarla: il componente sa cosi'
	//  quando deve invalidare il proprio proxy di scena. Modificare la mesh
	//  fuori da qui lascerebbe il renderer con la versione vecchia.
	// ------------------------------------------------------------------
	Component->EditMesh([&MeshData](UE::Geometry::FDynamicMesh3& Mesh)
	{
		using namespace UE::Geometry;

		Mesh.Clear();
		Mesh.EnableAttributes();

		FDynamicMeshNormalOverlay* Normals = Mesh.Attributes()->PrimaryNormals();
		FDynamicMeshUVOverlay* UVs = Mesh.Attributes()->PrimaryUV();

		const int32 VertexCount = static_cast<int32>(MeshData.Positions.size() / 3);

		for (int32 Index = 0; Index < VertexCount; ++Index)
		{
			// I vertici arrivano in METRI. La conversione in unita' Unreal non
			// si fa qui: e' nella SCALA della trasformazione del componente,
			// cosi' i float restano piccoli e la regola del punto unico di
			// conversione resta valida.
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
	});

	Component->NotifyMeshUpdated();
	Component->SetWorldTransform(Transform);
	return true;
}

void FDynamicMeshTerrainProvider::RemoveTile(const Tiles::FTileKey& Key)
{
	FTileEntry Entry;
	if (Tiles.RemoveAndCopyValue(PackKey(Key), Entry))
	{
		if (UDynamicMeshComponent* Component = Entry.Component.Get())
		{
			Component->DestroyComponent();
		}
	}
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
	FTileEntry* Entry = Tiles.Find(PackKey(Key));
	if (!Entry) { return; }

	UDynamicMeshComponent* Component = Entry->Component.Get();
	if (!Component) { return; }

	// Togliere il drappeggio: si torna al materiale grigio di base.
	if (!Texture)
	{
		if (Entry->bDraped)
		{
			if (UMaterialInterface* BaseMaterial = Material.Get())
			{
				Component->SetMaterial(0, BaseMaterial);
			}
			Entry->Material.Reset();
			Entry->bDraped = false;
		}
		return;
	}

	UMaterialInterface* Parent = DrapeMaterial.Get();
	if (!Parent) { return; }     // gia' segnalato in Initialize: non si insiste

	UMaterialInstanceDynamic* Instance = Entry->Material.Get();
	if (!Instance)
	{
		// NOTA UE: l'outer dell'istanza e' il COMPONENTE, non l'attore. Cosi'
		// quando il componente viene distrutto l'istanza lo segue, senza doverla
		// liberare a mano e senza che il garbage collector la trovi orfana.
		Instance = UMaterialInstanceDynamic::Create(Parent, Component);
		if (!Instance) { return; }

		Entry->Material = Instance;
		Component->SetMaterial(0, Instance);
	}

	Instance->SetTextureParameterValue(TEXT("BaseColor"), Texture);

	// (offsetU, offsetV, scala, scala). La quarta componente ripete la scala
	// perche' il materiale la usa come vettore 2D per moltiplicare le UV, e
	// duplicarla qui evita un nodo di mascheratura in piu' nello shader.
	Instance->SetVectorParameterValue(TEXT("UvOffsetScale"),
		FLinearColor(Drape.OffsetU, Drape.OffsetV, Drape.Scale, Drape.Scale));

	Entry->bDraped = true;
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

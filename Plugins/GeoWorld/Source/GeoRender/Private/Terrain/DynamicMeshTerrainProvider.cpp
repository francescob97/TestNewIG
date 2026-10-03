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
#include "UObject/Package.h"

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

	CheckMaterials(/*bLog=*/true);

	if (!DrapeMaterial.IsValid())
	{
		UE_LOG(LogGeoWorld, Warning,
			TEXT("[GeoTerrain] manca /GeoWorld/Materials/M_GeoTerrain: le ortofoto non ")
			TEXT("si vedranno. Crealo con geo.Imagery.CreateMaterial, oppure a mano ")
			TEXT("seguendo docs/fase6-verifica.md."));
	}

	UE_LOG(LogGeoWorld, Log, TEXT("[GeoTerrain] Provider '%s' pronto."), *GetName());
}

// ---------------------------------------------------------------------------
//  I materiali giusti? Controllato all'avvio e poi ogni due secondi.
//
//  PERCHE' ANCHE DOPO L'AVVIO. geo.Imagery.CreateMaterial si puo' lanciare
//  con il Play gia' partito: il materiale nuovo esiste, ma il provider lo
//  aveva cercato all'avvio, non l'aveva trovato e non lo cercava piu'. Le
//  strade 3D restavano grigie fino al Play successivo, con l'overlay che
//  continuava a dire "manca M_GeoRoad" anche se c'era. Ora lo si ricerca.
// ---------------------------------------------------------------------------
void FDynamicMeshTerrainProvider::CheckMaterials(bool bLog)
{
	if (!DrapeMaterial.IsValid())
	{
		DrapeMaterial.Reset(LoadObject<UMaterialInterface>(nullptr,
			TEXT("/GeoWorld/Materials/M_GeoTerrain.M_GeoTerrain"), nullptr, LOAD_NoWarn | LOAD_Quiet));
	}
	if (!RoadMaterial.IsValid())
	{
		RoadMaterial.Reset(LoadObject<UMaterialInterface>(nullptr,
			TEXT("/GeoWorld/Materials/M_GeoRoad.M_GeoRoad"), nullptr, LOAD_NoWarn | LOAD_Quiet));
	}

	FString NewMaterialProblem, NewOverlayProblem, NewTransitionProblem, NewRoadProblem;

	// Il materiale c'e', ma e' quello giusto? La versione con il bug dell'offset
	// V non ha il parametro DrapeUv (si chiamava UvOffsetScale). Lo si dice
	// chiaramente: altrimenti il sintomo e' un mosaico di pezzi di immagine
	// fuori posto, che non fa pensare al materiale.
	if (UMaterialInterface* Drape = DrapeMaterial.Get())
	{
		FLinearColor UnusedColor;
		UTexture* UnusedTexture = nullptr;
		if (!Drape->GetVectorParameterValue(FHashedMaterialParameterInfo(TEXT("DrapeUv")), UnusedColor))
		{
			NewMaterialProblem = TEXT("M_GeoTerrain e' la versione VECCHIA (bug dell'offset V): ")
			                     TEXT("rifallo con geo.Imagery.CreateMaterial");
		}

		// Fase 8: le strade vogliono tre parametri in piu'. Un M_GeoTerrain fatto
		// prima della Fase 8 non li ha, e le strade semplicemente non si
		// vedrebbero, senza un errore: lo si dice qui.
		if (!Drape->GetTextureParameterValue(FHashedMaterialParameterInfo(TEXT("Overlay")), UnusedTexture))
		{
			NewOverlayProblem = TEXT("M_GeoTerrain non ha i parametri delle strade (Overlay): ")
			                    TEXT("rifallo con geo.Imagery.CreateMaterial");
		}

		// Transizioni morbide: la foto precedente e il geomorphing. Senza, le
		// tile cambiano di colpo come prima: niente di rotto, solo il lampo.
		bFadeSupported = Drape->GetTextureParameterValue(
			FHashedMaterialParameterInfo(TEXT("BaseColorPrevious")), UnusedTexture);
		if (!bFadeSupported)
		{
			NewTransitionProblem = TEXT("M_GeoTerrain senza transizioni morbide (il lampo quando la ")
			                       TEXT("geometria si affina): rifallo con geo.Imagery.CreateMaterial");
		}
	}

	// Il materiale delle strade 3D: come M_GeoTerrain, nasce da
	// geo.Imagery.CreateMaterial. Senza, le strade 3D usano il grigio di base.
	if (!RoadMaterial.IsValid())
	{
		NewRoadProblem = TEXT("manca M_GeoRoad: le strade 3D restano grigie. ")
		                 TEXT("Crealo con geo.Imagery.CreateMaterial");
	}
	else
	{
		UTexture* UnusedTexture = nullptr;
		if (!RoadMaterial->GetTextureParameterValue(FHashedMaterialParameterInfo(TEXT("RoadAtlas")), UnusedTexture))
		{
			NewRoadProblem = TEXT("M_GeoRoad non ha il parametro RoadAtlas: le strade 3D restano senza ")
			                 TEXT("texture. Rifallo con geo.Imagery.CreateMaterial");
		}
	}

	// Si scrive nel log solo cio' che e' cambiato: ogni due secondi lo stesso
	// avviso sommergerebbe tutto il resto.
	const auto Update = [bLog](FString& Current, const FString& New)
	{
		if (Current == New) { return; }
		Current = New;
		if (!New.IsEmpty()) { UE_LOG(LogGeoWorld, Warning, TEXT("[GeoTerrain] %s"), *New); }
		else if (!bLog) { UE_LOG(LogGeoWorld, Log, TEXT("[GeoTerrain] materiale ritrovato e a posto")); }
	};
	Update(MaterialProblem, NewMaterialProblem);
	Update(OverlayMaterialProblem, NewOverlayProblem);
	Update(TransitionMaterialProblem, NewTransitionProblem);
	Update(RoadMaterialProblem, NewRoadProblem);
}

void FDynamicMeshTerrainProvider::Shutdown()
{
	RemoveAllTiles();
	if (AActor* Actor = Container.Get()) { Actor->Destroy(); }
	Container = nullptr;
	RoadMaterialInstance.Reset();
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

	// Tre canali UV: lo 0 per le texture; l'1 e il 2 per il GEOMORPHING
	// (Mesh/TileMesh.h, ComputeMorphTargets): normale del padre (x, y | z) e
	// quanto il vertice sta sopra la superficie del padre. Il materiale li
	// legge con TexCoord[1] e TexCoord[2].
	//
	// Si scrivono SEMPRE, anche senza dati di morphing (normale propria,
	// delta zero): un canale mancante il motore lo rimpiazza con il canale 0,
	// e il materiale normalizzerebbe una "normale" fatta di UV. Con il
	// morphing a zero non si vedrebbe, ma un vettore nullo normalizzato e' un
	// NaN, e un NaN moltiplicato per zero resta NaN: pixel neri.
	Mesh.Attributes()->SetNumUVLayers(3);

	FDynamicMeshNormalOverlay* Normals = Mesh.Attributes()->PrimaryNormals();
	FDynamicMeshUVOverlay* UVs = Mesh.Attributes()->PrimaryUV();
	FDynamicMeshUVOverlay* ParentNormalXY = Mesh.Attributes()->GetUVLayer(1);
	FDynamicMeshUVOverlay* ParentNormalZAndDelta = Mesh.Attributes()->GetUVLayer(2);

	const int32 VertexCount = static_cast<int32>(MeshData.Positions.size() / 3);
	const bool bHasMorph = MeshData.MorphDeltas.size() == static_cast<size_t>(VertexCount)
	                    && MeshData.ParentNormals.size() == static_cast<size_t>(VertexCount) * 3;
	Prepared->bHasMorph = bHasMorph;

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

		const float* ParentNormal = bHasMorph ? &MeshData.ParentNormals[Index * 3] : &MeshData.Normals[Index * 3];
		const float Delta = bHasMorph ? MeshData.MorphDeltas[Index] : 0.0f;
		ParentNormalXY->AppendElement(FVector2f(ParentNormal[0], ParentNormal[1]));
		ParentNormalZAndDelta->AppendElement(FVector2f(ParentNormal[2], Delta));
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
			ParentNormalXY->SetTriangle(TriangleId, FIndex3i(A, B, C));
			ParentNormalZAndDelta->SetTriangle(TriangleId, FIndex3i(A, B, C));
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
	Entry.bHasMorph = Ready.bHasMorph;

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
	Transitioning.Remove(Key.Pack());
	if (Tiles.RemoveAndCopyValue(Key.Pack(), Entry))
	{
		if (UDynamicMeshComponent* Component = Entry.Component.Get())
		{
			Component->DestroyComponent();
		}
		// La strada sparisce con la sua tile, nello stesso istante.
		if (UDynamicMeshComponent* Road = Entry.RoadComponent.Get())
		{
			Road->DestroyComponent();
		}
	}
}

void FDynamicMeshTerrainProvider::SetTileVisible(const Tiles::FTileKey& Key, bool bVisible)
{
	FTileEntry* Entry = Tiles.Find(Key.Pack());
	UDynamicMeshComponent* Component = Entry ? Entry->Component.Get() : nullptr;
	if (!Component) { return; }

	// Una tile che sparisce non ha piu' niente da cui sfumare: se torna, la
	// sua transizione la decide chi la rimostra (BeginTransitionFromParent).
	if (!bVisible) { FinishTransitions(*Entry); }

	// La strada 3D segue la sua tile: si mostra e si nasconde nello stesso
	// frame, altrimenti per un frame si vedrebbe una senza l'altra.
	if (UDynamicMeshComponent* Road = Entry->RoadComponent.Get())
	{
		if (Road->GetVisibleFlag() != bVisible) { Road->SetVisibility(bVisible); }
	}
	if (Component->GetVisibleFlag() == bVisible) { return; }

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
		if (UDynamicMeshComponent* Road = Pair.Value.RoadComponent.Get())
		{
			Road->DestroyComponent();
		}
	}
	Tiles.Empty();
	Transitioning.Empty();
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

		// Stessa origine, stessa trasformazione: la strada si sposta con il
		// suo terreno nello stesso rebase.
		if (UDynamicMeshComponent* Road = Pair.Value.RoadComponent.Get())
		{
			Road->SetWorldTransform(Transform);
		}
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

	// Una tile A SCHERMO che cambia foto (di solito: quella dell'antenato,
	// sgranata, sostituita dalla propria) non cambia piu' di colpo: la foto
	// di prima resta come "precedente" e sfuma via. Una tile nascosta cambia
	// e basta: nessuno la sta guardando.
	const bool bFade = Entry->bDraped && Component->GetVisibleFlag();
	const FDressState Previous = bFade ? CaptureDress(*Entry) : FDressState();

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

	if (bFade) { StartFade(*Entry, Previous); }
	else if (Entry->FadeStart < 0.0) { ReleasePrevious(*Entry); }
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

	// Come per la foto: sulle tile a schermo le strade nuove (quelle proprie
	// al posto di quelle dell'antenato) arrivano sfumando.
	const bool bFade = Entry->bDraped && Component->GetVisibleFlag();
	const FDressState Previous = bFade ? CaptureDress(*Entry) : FDressState();

	Entry->bOverlaid = true;
	Entry->OverlayTexture = Texture;
	Entry->OverlayTransform = Window;
	ApplyOverlayParameters(*Entry, Instance);

	if (bFade) { StartFade(*Entry, Previous); }
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

// ---------------------------------------------------------------------------
//  Strade 3D
// ---------------------------------------------------------------------------

UMaterialInterface* FDynamicMeshTerrainProvider::GetRoadMaterialForComponent() const
{
	if (UMaterialInstanceDynamic* Instance = RoadMaterialInstance.Get()) { return Instance; }
	if (UMaterialInterface* Road = RoadMaterial.Get()) { return Road; }
	return Material.Get();
}

void FDynamicMeshTerrainProvider::SetRoadAtlas(UTexture2D* Atlas)
{
	UMaterialInterface* Parent = RoadMaterial.Get();
	if (!Parent || !Atlas) { return; }

	// Un'istanza sola per tutte le strade: l'atlante e' lo stesso. L'outer e'
	// il pacchetto transitorio (non un componente: l'istanza deve vivere
	// finche' vive il provider, che la tiene con un puntatore forte).
	if (!RoadMaterialInstance.IsValid())
	{
		RoadMaterialInstance.Reset(UMaterialInstanceDynamic::Create(Parent, GetTransientPackage()));
	}
	if (UMaterialInstanceDynamic* Instance = RoadMaterialInstance.Get())
	{
		Instance->SetTextureParameterValue(TEXT("RoadAtlas"), Atlas);
	}

	for (TPair<uint64, FTileEntry>& Pair : Tiles)
	{
		if (UDynamicMeshComponent* Road = Pair.Value.RoadComponent.Get())
		{
			Road->SetMaterial(0, GetRoadMaterialForComponent());
		}
	}
}

bool FDynamicMeshTerrainProvider::CommitRoadMesh(const Tiles::FTileKey& Key, FGeoPreparedTileMesh& Prepared)
{
	AActor* Actor = Container.Get();
	FTileEntry* Entry = Tiles.Find(Key.Pack());
	if (!Actor || !Entry) { return false; }

	UDynamicMeshComponent* Tile = Entry->Component.Get();
	if (!Tile) { return false; }

	FDynamicMeshPrepared& Ready = static_cast<FDynamicMeshPrepared&>(Prepared);

	UDynamicMeshComponent* Road = Entry->RoadComponent.Get();
	if (!Road)
	{
		Road = NewObject<UDynamicMeshComponent>(Actor);
		Road->SetupAttachment(Actor->GetRootComponent());
		Road->SetMobility(EComponentMobility::Movable);
		Road->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		// Niente ombre: un nastro sollevato di 20 cm farebbe un'ombra sottile
		// e tremolante sul terreno sotto, che nessuna strada vera ha.
		Road->SetCastShadow(false);
		Road->SetEnableRaytracing(false);
		Road->SetMaterial(0, GetRoadMaterialForComponent());
		ApplyWireframe(Road, bWireframe);
		Road->RegisterComponent();
		Entry->RoadComponent = Road;
	}

	Road->SetMesh(MoveTemp(Ready.Mesh));
	Road->NotifyMeshUpdated();
	// La trasformazione e la visibilita' sono quelle della tile, adesso: la
	// strada nasce gia' allineata e gia' nascosta se la tile e' nascosta.
	Road->SetWorldTransform(Tile->GetComponentTransform());
	Road->SetVisibility(Tile->GetVisibleFlag());
	// E la stessa forma: se la tile sta ancora scivolando dalla forma del
	// padre, la strada parte dallo stesso punto.
	Road->SetCustomPrimitiveDataFloat(0, Entry->MorphValue);
	return true;
}

void FDynamicMeshTerrainProvider::RemoveRoadMesh(const Tiles::FTileKey& Key)
{
	FTileEntry* Entry = Tiles.Find(Key.Pack());
	if (!Entry) { return; }
	if (UDynamicMeshComponent* Road = Entry->RoadComponent.Get())
	{
		Road->DestroyComponent();
	}
	Entry->RoadComponent.Reset();
}

int32 FDynamicMeshTerrainProvider::GetRoadMeshCount() const
{
	int32 Count = 0;
	for (const TPair<uint64, FTileEntry>& Pair : Tiles)
	{
		if (Pair.Value.RoadComponent.IsValid()) { ++Count; }
	}
	return Count;
}

int32 FDynamicMeshTerrainProvider::GetRoadTriangleCount() const
{
	int32 Total = 0;
	for (const TPair<uint64, FTileEntry>& Pair : Tiles)
	{
		const UDynamicMeshComponent* Road = Pair.Value.RoadComponent.Get();
		if (!Road) { continue; }
		if (const UE::Geometry::FDynamicMesh3* RoadMeshData = Road->GetMesh())
		{
			Total += RoadMeshData->TriangleCount();
		}
	}
	return Total;
}

// ---------------------------------------------------------------------------
//  Transizioni morbide: geomorphing e dissolvenza
//
//  Il lampo che restava: quando il quadtree raffina, le quattro figlie
//  prendono il posto del padre nello stesso frame, e cambiano tutto insieme
//  la forma (piu' dettaglio), la luce (le normali nuove) e la foto (di solito
//  piu' nitida, a volte di un'altra data). Qui le figlie nascono IDENTICHE al
//  padre e diventano se stesse in TransitionSeconds:
//
//    Custom Primitive Data 0  "Morph"  1 = forma e normali del padre, 0 = proprie
//    Custom Primitive Data 1  "Fade"   1 = foto e strade del padre,   0 = proprie
//
//  NOTA UE: i Custom Primitive Data sono float che viaggiano con il
//  COMPONENTE (nella GPU Scene), non con il materiale. Cambiarli a ogni frame
//  costa l'aggiornamento di un piccolo buffer, mentre cambiare un parametro
//  dell'istanza di materiale ne ricostruirebbe il proxy di rendering. E vanno
//  bene anche per le strade 3D, che hanno un'istanza sola per tutte le tile.
// ---------------------------------------------------------------------------

namespace
{
	/** La finestra di un antenato, ristretta al pezzo che copre `Child`. */
	GeoWorld::Imagery::FDrapeTransform ComposeWindow(const GeoWorld::Imagery::FDrapeTransform& AncestorWindow,
	                                                 const GeoWorld::Imagery::FDrapeTransform& ChildInAncestor)
	{
		// uv_texture = A.Offset + A.Scale * uv_antenato
		// uv_antenato = S.Offset + S.Scale * uv_figlia
		GeoWorld::Imagery::FDrapeTransform Result;
		Result.OffsetU = AncestorWindow.OffsetU + AncestorWindow.Scale * ChildInAncestor.OffsetU;
		Result.OffsetV = AncestorWindow.OffsetV + AncestorWindow.Scale * ChildInAncestor.OffsetV;
		Result.Scale = AncestorWindow.Scale * ChildInAncestor.Scale;
		return Result;
	}

	/** Parte e arriva piano: l'occhio non vede l'inizio e la fine. */
	float SmoothFraction(double Elapsed, float Seconds)
	{
		if (Seconds <= 0.0f) { return 1.0f; }
		const float T = FMath::Clamp(static_cast<float>(Elapsed / Seconds), 0.0f, 1.0f);
		return T * T * (3.0f - 2.0f * T);
	}
}

FDynamicMeshTerrainProvider::FDressState FDynamicMeshTerrainProvider::CaptureDress(const FTileEntry& Entry)
{
	FDressState State;
	if (Entry.bDraped)
	{
		State.Drape = Entry.DrapedTexture;
		State.DrapeTransform = Entry.DrapedTransform;
	}
	if (Entry.bOverlaid)
	{
		State.Overlay = Entry.OverlayTexture;
		State.OverlayTransform = Entry.OverlayTransform;
	}
	return State;
}

void FDynamicMeshTerrainProvider::WriteTransitionData(FTileEntry& Entry, float Morph, float Fade)
{
	UDynamicMeshComponent* Component = Entry.Component.Get();
	if (!Component) { return; }

	if (Entry.MorphValue != Morph)
	{
		Entry.MorphValue = Morph;
		Component->SetCustomPrimitiveDataFloat(0, Morph);
		// La strada 3D scivola con il suo terreno: stessa quota a ogni frame.
		if (UDynamicMeshComponent* Road = Entry.RoadComponent.Get())
		{
			Road->SetCustomPrimitiveDataFloat(0, Morph);
		}
	}
	if (Entry.FadeValue != Fade)
	{
		Entry.FadeValue = Fade;
		Component->SetCustomPrimitiveDataFloat(1, Fade);
	}
}

void FDynamicMeshTerrainProvider::ReleasePrevious(FTileEntry& Entry) const
{
	// La foto "precedente" e' un riferimento FORTE dell'istanza di materiale:
	// finche' resta li', il garbage collector non puo' liberare quella
	// texture, anche se la cache delle ortofoto l'ha gia' buttata. Finita la
	// dissolvenza la si fa puntare alla foto attuale (che e' tenuta comunque).
	UMaterialInstanceDynamic* Instance = Entry.Material.Get();
	UTexture2D* Current = Entry.DrapedTexture.Get();
	if (!bFadeSupported || !Instance || !Current) { return; }

	Instance->SetTextureParameterValue(TEXT("BaseColorPrevious"), Current);
	Instance->SetTextureParameterValue(TEXT("OverlayPrevious"),
		Entry.OverlayTexture.IsValid() ? Entry.OverlayTexture.Get() : Current);
	Instance->SetScalarParameterValue(TEXT("OverlayPreviousStrength"), 0.0f);
}

void FDynamicMeshTerrainProvider::StartFade(FTileEntry& Entry, const FDressState& Previous)
{
	if (!bFadeSupported || TransitionSeconds <= 0.0f) { return; }

	// Gia' in dissolvenza: la "precedente" resta quella da cui si era partiti.
	// Ricatturarla ora vorrebbe dire saltare dallo stato a meta' strada.
	if (Entry.FadeStart >= 0.0) { return; }

	UMaterialInstanceDynamic* Instance = Entry.Material.Get();
	UTexture2D* PreviousDrape = Previous.Drape.Get();
	if (!Instance || !PreviousDrape) { return; }

	const GeoWorld::Imagery::FDrapeTransform& D = Previous.DrapeTransform;
	Instance->SetTextureParameterValue(TEXT("BaseColorPrevious"), PreviousDrape);
	Instance->SetVectorParameterValue(TEXT("DrapeUvPrevious"), FLinearColor(D.OffsetU, D.OffsetV, D.Scale, D.Scale));

	if (UTexture2D* PreviousOverlay = Previous.Overlay.Get())
	{
		const GeoWorld::Imagery::FDrapeTransform& O = Previous.OverlayTransform;
		Instance->SetTextureParameterValue(TEXT("OverlayPrevious"), PreviousOverlay);
		Instance->SetVectorParameterValue(TEXT("OverlayUvPrevious"), FLinearColor(O.OffsetU, O.OffsetV, O.Scale, O.Scale));
		Instance->SetScalarParameterValue(TEXT("OverlayPreviousStrength"), OverlayStrength);
	}
	else
	{
		Instance->SetTextureParameterValue(TEXT("OverlayPrevious"), PreviousDrape);
		Instance->SetScalarParameterValue(TEXT("OverlayPreviousStrength"), 0.0f);
	}

	Entry.FadeStart = FPlatformTime::Seconds();
	WriteTransitionData(Entry, Entry.MorphValue, 1.0f);
	Transitioning.Add(Entry.Key.Pack());
}

void FDynamicMeshTerrainProvider::FinishTransitions(FTileEntry& Entry)
{
	const bool bWasFading = Entry.FadeStart >= 0.0 || Entry.FadeValue != 0.0f;
	Entry.MorphStart = -1.0;
	Entry.FadeStart = -1.0;
	WriteTransitionData(Entry, 0.0f, 0.0f);
	if (bWasFading) { ReleasePrevious(Entry); }
	Transitioning.Remove(Entry.Key.Pack());
}

void FDynamicMeshTerrainProvider::SetTileMorph(const Tiles::FTileKey& Key, float Morph)
{
	FTileEntry* Entry = Tiles.Find(Key.Pack());
	if (!Entry) { return; }
	Entry->MorphStart = -1.0;
	WriteTransitionData(*Entry, FMath::Clamp(Morph, 0.0f, 1.0f), Entry->FadeValue);
	if (Entry->FadeStart < 0.0) { Transitioning.Remove(Key.Pack()); }
}

void FDynamicMeshTerrainProvider::BeginTransitionFromParent(const Tiles::FTileKey& Child,
                                                            const Tiles::FTileKey& Parent)
{
	FTileEntry* Entry = Tiles.Find(Child.Pack());
	const FTileEntry* ParentEntry = Tiles.Find(Parent.Pack());
	if (!Entry || TransitionSeconds <= 0.0f || Child.Level <= Parent.Level) { return; }

	// 1. La FORMA: la tile parte con la superficie e le normali del padre. Il
	//    materiale le sottrae Delta * Morph (UV 2) e mescola le normali (UV 1 e
	//    2). Serve l'M_GeoTerrain con le transizioni: con il grigio di base o
	//    con un materiale vecchio Morph non lo legge nessuno, e si salta.
	if (Entry->bHasMorph && bFadeSupported && Entry->Material.IsValid())
	{
		Entry->MorphStart = FPlatformTime::Seconds();
		WriteTransitionData(*Entry, 1.0f, Entry->FadeValue);
		Transitioning.Add(Child.Pack());
	}

	// 2. La FOTO e le strade dipinte: quelle del padre, ristrette al pezzo che
	//    copre questa tile, diventano la "precedente" e sfumano via.
	if (ParentEntry)
	{
		const GeoWorld::Imagery::FDrapeTransform ChildInParent =
			GeoWorld::Imagery::MakeDrapeTransform(Child, Parent.Level);

		FDressState Previous = CaptureDress(*ParentEntry);
		Previous.DrapeTransform = ComposeWindow(Previous.DrapeTransform, ChildInParent);
		Previous.OverlayTransform = ComposeWindow(Previous.OverlayTransform, ChildInParent);

		// Se la foto e' la stessa e la finestra pure (figlia vestita con la
		// texture del padre, in attesa della propria), non c'e' niente da sfumare.
		const bool bSameDrape = Previous.Drape == Entry->DrapedTexture
			&& FMath::IsNearlyEqual(Previous.DrapeTransform.OffsetU, Entry->DrapedTransform.OffsetU)
			&& FMath::IsNearlyEqual(Previous.DrapeTransform.OffsetV, Entry->DrapedTransform.OffsetV)
			&& FMath::IsNearlyEqual(Previous.DrapeTransform.Scale, Entry->DrapedTransform.Scale);
		const bool bSameOverlay = Previous.Overlay == Entry->OverlayTexture;
		if (!(bSameDrape && bSameOverlay))
		{
			StartFade(*Entry, Previous);
		}
	}
}

void FDynamicMeshTerrainProvider::TickTransitions(double NowSeconds)
{
	if (NowSeconds - LastMaterialCheckSeconds > 2.0)
	{
		LastMaterialCheckSeconds = NowSeconds;
		CheckMaterials(/*bLog=*/false);
	}

	if (Transitioning.Num() == 0) { return; }

	TArray<uint64> Keys = Transitioning.Array();
	for (const uint64 Packed : Keys)
	{
		FTileEntry* Entry = Tiles.Find(Packed);
		if (!Entry) { Transitioning.Remove(Packed); continue; }

		float Morph = Entry->MorphValue;
		if (Entry->MorphStart >= 0.0)
		{
			Morph = 1.0f - SmoothFraction(NowSeconds - Entry->MorphStart, TransitionSeconds);
			if (Morph <= 0.0f) { Morph = 0.0f; Entry->MorphStart = -1.0; }
		}

		float Fade = Entry->FadeValue;
		bool bFadeEnded = false;
		if (Entry->FadeStart >= 0.0)
		{
			Fade = 1.0f - SmoothFraction(NowSeconds - Entry->FadeStart, TransitionSeconds);
			if (Fade <= 0.0f) { Fade = 0.0f; Entry->FadeStart = -1.0; bFadeEnded = true; }
		}

		WriteTransitionData(*Entry, Morph, Fade);
		if (bFadeEnded) { ReleasePrevious(*Entry); }
		if (Entry->MorphStart < 0.0 && Entry->FadeStart < 0.0) { Transitioning.Remove(Packed); }
	}
}

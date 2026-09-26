# Capitolo 10 — Glossario e mappa dei file

---

## Glossario

Termini in ordine alfabetico. Tra parentesi, il capitolo dove sono spiegati.

**Antenato** — la tile di un livello più grossolano che contiene una tile data.
L'antenato di livello `La` di `(L, X, Y)` è `(La, X >> d, Y >> d)` con `d = L − La`. (7)

**Attore** (`AActor`) — un oggetto che esiste nella scena di Unreal, con una
posizione. È fatto di componenti. (1)

**Ballpark** — la "trasformazione approssimata" di PROJ, che in mancanza della
griglia geoidica non applica lo scostamento verticale, in silenzio. (3)

**BC1 / DXT1** — compressione delle texture che la scheda video legge
direttamente. Non usata oggi; il formato è pronto per aggiungerla. (7)

**Bounds** — il volume che contiene una primitiva. Il renderer lo usa per decidere
se è visibile. (1, 6)

**Bowring** — formula approssimata (1976) per passare da ECEF a latitudine,
longitudine, quota in un passo solo. Qui con una raffinazione. (2)

**Build unity** — Unreal compila più `.cpp` dello stesso modulo come un file solo,
per fare prima. Fa scontrare le funzioni omonime nei namespace anonimi. (1)

**CDO** (*Class Default Object*) — l'oggetto "prototipo" che Unreal crea per ogni
classe `UCLASS`, con i valori di default. I file `.ini` vengono applicati a lui. (1)

**Componente** (`UActorComponent`) — un pezzo di un attore che gli dà una capacità:
disegnarsi, muoversi, stare a una coordinata geografica. (1)

**Copertura** — la percentuale di una tile di immagine effettivamente coperta
dall'ortofoto. (7)

**COG** (*Cloud Optimized GeoTIFF*) — un GeoTIFF organizzato in modo che se ne
possano leggere pezzi via rete senza scaricarlo tutto. (7)

**Delegato** — un meccanismo di Unreal per "avvisami quando succede X".
`OnGeoreferenceRebased` è un delegato. (2)

**DEM** (*Digital Elevation Model*) — una griglia di quote del terreno. (3)

**Draw call** — una richiesta di disegno dalla CPU alla scheda video. Ogni tile
oggi ne costa una. (6)

**ECEF** (*Earth-Centered, Earth-Fixed*) — coordinate cartesiane con l'origine nel
centro della Terra. **L'unica autorità** del progetto. (2)

**Editor / PIE / Game** — i tre contesti in cui gira il codice. Nell'editor la
camera non è un attore. (1)

**Ellissoide** — la forma matematica della Terra: un'ellisse fatta ruotare. (2)

**Errore geometrico** — di quanti metri, al massimo, un livello si discosta dal
terreno vero. Qui approssimato con il passo fra i post. (5)

**Errore su schermo** (SSE) — l'errore geometrico proiettato in pixel. La manopola
del LOD. Soglia di default: 4 pixel. (5)

**Frame locale** — un sistema di assi appoggiato sulla superficie in un punto.
Nel progetto è sempre NEU. (2, 6)

**Frustum** — la piramide tronca che la camera vede. (5)

**Game thread** — il thread principale di Unreal, dove girano tutti i `Tick`. Non
va mai bloccato. (1)

**Garbage collector** (GC) — il meccanismo che cancella da solo gli `UObject`
che nessuno usa più. Vede solo i riferimenti che conosce. (1)

**GDAL** — la libreria standard per leggere e trasformare dati geografici. (3)

**Geoide** — la superficie del livello medio del mare, irregolare. (3)

**Gonna** (*skirt*) — striscia verticale che scende dal bordo di una tile, per
nascondere le crepe fra livelli diversi. (6)

**Header-only** — codice scritto interamente negli header, con `inline`. Lo strato
puro è così, per non dover esportare simboli dalle DLL. (0, 1)

**Horizon culling** — scartare ciò che sta oltre la curvatura della Terra. (5)

**Indice** (`index.bin`) — per ogni livello, l'elenco delle tile esistenti con
quota minima e massima (quote) o copertura (immagini). (3)

**LOD** (*Level Of Detail*) — usare versioni più semplici di ciò che è lontano. (5)

**LRU** (*Least Recently Used*) — cache che butta per primo ciò che non si usa da
più tempo. (4)

**Large World Coordinates** (LWC) — in Unreal 5, `FVector` in `double`. Non
arriva fino alla scheda video. (1, 2)

**Manifest** — il file `manifest.json` che descrive un dataset. (3)

**MarkRenderStateDirty** — dire a Unreal che una proprietà di disegno è cambiata e
il proxy di scena va rifatto. (6)

**MGRS** — il sistema di quadrati da 100 km usato per organizzare le scene
Sentinel-2. (7)

**MID** (`UMaterialInstanceDynamic`) — una copia di un materiale con parametri
modificabili a runtime. (1, 7)

**Modulo** — un'unità di compilazione di Unreal, che diventa una DLL. (1)

**MPSC** (*multiple producers, single consumer*) — una coda in cui molti thread
aggiungono e uno solo toglie. (4)

**NEU** (*North-East-Up*) — l'ordine degli assi scelto per il progetto: Nord → X,
Est → Y, Alto → Z. (2)

**Normale geodetica** — la perpendicolare all'ellissoide. Non punta al centro
della Terra. (2)

**Ondulazione** (`N`) — la differenza fra geoide ed ellissoide. In Italia fra +42
e +52 m. (3)

**Ortofoto** — immagine aerea o satellitare raddrizzata, in cui ogni pixel sta alla
sua coordinata. (7)

**Pin** — marcare una tile della cache come "non sfrattabile". (4)

**Plugin** — un pacchetto di codice e contenuti riutilizzabile. (1)

**Post** — un punto di campionamento di una griglia di quote. (3)

**Primitiva** (`UPrimitiveComponent`) — un componente che si disegna. (1)

**PROJ** — la libreria di proiezioni cartografiche usata da GDAL. (3)

**Provider** — l'implementazione dell'interfaccia `IGeoTerrainMeshProvider`, che
trasforma mesh astratte in qualcosa che Unreal disegna. (6)

**Proxy di scena** (`FPrimitiveSceneProxy`) — la copia di una primitiva che vive
sul thread di rendering. (1)

**Quadtree** — albero in cui ogni nodo ha quattro figli. La piramide delle tile. (5)

**Quota ellissoidica / ortometrica** — sopra l'ellissoide / sopra il livello del
mare. Il motore usa solo la prima. (3)

**Rebasing** — spostare l'origine di Unreal vicino alla camera, per tenere piccoli
i numeri. (2)

**Registrazione sui nodi / sulle aree** — un campione rappresenta un punto (le
quote) o un'area (i pixel). Decide se il bordo è condiviso. (3, 7)

**sRGB** — lo spazio di colore delle immagini normali. Una texture da JPEG va
marcata sRGB. (7)

**Snapshot** — la copia della georeferenziazione che i thread ricevono, invece di
toccare il subsystem. (2)

**Strato puro / ponte Unreal** — la divisione di ogni modulo in C++ standard
testabile e codice che usa il motore. (0)

**Subsystem** — un "gestore" che Unreal crea e distrugge da solo. (1)

**Tick** — il metodo chiamato a ogni frame. (1)

**Tile** — un pezzo quadrato della piramide: 129 × 129 quote o 256 × 256 pixel. (3, 7)

**TINITALY** — il modello del terreno dell'Italia a 10 m dell'INGV. (3)

**UBT / UHT** — Unreal Build Tool e Unreal Header Tool: compilano e generano codice. (1)

**ULP** (*unit in the last place*) — il passo fra due numeri `float` consecutivi.
A 6.371 km è 64 cm, a 10 km è 0,6 mm. (2)

**UObject** — la classe base universale di Unreal, con riflessione e garbage
collector. (1)

**VRT** — un raster virtuale di GDAL: un file XML che fa apparire più file come
uno solo. (3, 7)

**WGS84** — l'ellissoide del GPS, usato dal progetto. (2)

**Wireframe** — la visualizzazione a reticolo dei triangoli. `viewmode wireframe` è
il primo comando da provare quando non si vede niente. (6)

---

## Mappa completa dei file

### Il progetto

| File | Contenuto |
|---|---|
| `TestNewIG.uproject` | il progetto Unreal |
| `Source/TestNewIG/*` | modulo C++ minimo: serve solo a far compilare il plugin |
| `Config/DefaultEngine.ini` | impostazioni del motore |
| `Config/DefaultGame.ini` | impostazioni di GeoWorld (`[/Script/GeoCore.GeoWorldSettings]`) |
| `Plugins/GeoWorld/GeoWorld.uplugin` | il plugin e i suoi quattro moduli |

### GeoCore — geodesia e rebasing

| File | Contenuto |
|---|---|
| `GeoCore.Build.cs` | dipendenze; `UnrealEd` solo per l'editor |
| `Public/GeoCoreModule.h` | la categoria di log `LogGeoWorld` |
| `Public/Geo/GeoTypes.h` | `FGeodetic`, `FEcef`, `FEnu`, `FMat3` |
| `Public/Geo/Ellipsoid.h` | WGS84, conversioni, basi ENU/NEU |
| `Public/Geo/EnuFrame.h` | frame locale ENU |
| `Public/Geo/GeoUnits.h` | `MetersToUu`: l'unico posto del numero 100 |
| `Public/Geo/Georeference.h` | `P_unreal = M·(P − O)·100` |
| `Public/Georeference/GeoreferenceSnapshot.h` | la copia per i thread |
| `Public/Georeference/GeoreferenceSubsystem.h` | il subsystem del rebasing |
| `Public/Georeference/GeoTransformComponent.h` | tieni un attore a una coordinata |
| `Public/Georeference/GeoWorldSettings.h` | impostazioni in Project Settings |
| `Public/Georeference/GeoWorldTypes.h` | `FGeoCoordinate`, per i Blueprint |
| `Public/Georeference/GeoPlaces.h` | i 28 luoghi noti |
| `Private/GeoCoreModule.cpp` | comandi `geo.*` di base, `geo.Goto`, `geo.ViewSpeed` |
| `Private/Georeference/GeoreferenceSubsystem.cpp` | il rebasing in quattro passi |
| `Private/Georeference/GeoTransformComponent.cpp` | ricalcolo della posizione |
| `Private/Georeference/GeoWorldSettings.cpp` | registrazione delle impostazioni |
| `Private/Tests/GeoCoreTests.cpp` | Automation Test di Unreal |

### GeoTiles — formato, dataset, streaming

| File | Contenuto |
|---|---|
| `GeoTiles.Build.cs` | dipendenze, `ImageWrapper` per il JPEG |
| `Public/Tiles/TileKey.h` | `FTileKey`, `Pack()`, `PackXY()` |
| `Public/Tiles/TilingScheme.h` | lo schema di tiling |
| `Public/Tiles/TileFormat.h` | formato `.ght` e indice delle quote |
| `Public/Tiles/ImageTileFormat.h` | formato `.gim` e indice delle immagini |
| `Public/Tiles/TileCache.h` | `TTileCache<Payload>`, LRU con budget e pin |
| `Public/Streaming/GeoTileDataset.h` | dataset di quote su disco |
| `Public/Streaming/GeoImageryDataset.h` | dataset di immagini su disco |
| `Public/Streaming/GeoLoaderPool.h` | pool di thread per l'I/O |
| `Public/Streaming/GeoTileStreamingSubsystem.h` | streaming delle quote |
| `Public/Streaming/GeoImageryStreamingSubsystem.h` | streaming e decodifica delle immagini |
| `Private/GeoTilesModule.cpp` | comandi `geo.Tiles.*` |
| `Private/Streaming/*.cpp` | le implementazioni dei cinque header sopra |
| `TestData/tiling_vectors.txt` | vettori di prova generati dalla pipeline |

### GeoRender — decidere e disegnare

| File | Contenuto |
|---|---|
| `GeoRender.Build.cs` | `GeometryFramework`, `GeometryCore` |
| `Public/Quadtree/QuadtreeTypes.h` | vista, selezione, statistiche |
| `Public/Quadtree/Culling.h` | volume, frustum, margine, orizzonte |
| `Public/Quadtree/TileSelector.h` | errore su schermo, attraversamento |
| `Public/Mesh/TileMesh.h` | da quote a mesh, gonne |
| `Public/Imagery/ImageryMapping.h` | il ritaglio dell'immagine |
| `Public/Lod/GeoQuadtreeSubsystem.h` | il subsystem della selezione |
| `Public/Terrain/GeoTerrainMeshProvider.h` | l'interfaccia del disegno |
| `Public/Terrain/DynamicMeshTerrainProvider.h` | l'implementazione con `UDynamicMeshComponent` |
| `Public/Terrain/GeoTerrainSubsystem.h` | il subsystem del terreno |
| `Public/Imagery/GeoImagerySubsystem.h` | il subsystem del drappeggio |
| `Public/GeoMarkerActor.h` | il cubo di verifica |
| `Public/GeoFlyPawn.h` | la camera di volo |
| `Private/GeoRenderModule.cpp` | comandi `geo.Lod.*`, `geo.Terrain.*`, `geo.Imagery.*`, `geo.Fly*` |
| `Private/**/*.cpp` | le implementazioni |

### GeoWorldEditor — solo nell'editor

| File | Contenuto |
|---|---|
| `GeoWorldEditor.Build.cs` | `MaterialEditor`, `AssetTools` |
| `Private/GeoWorldEditorModule.cpp` | il modulo |
| `Private/GeoTerrainMaterialFactory.cpp` | `geo.Imagery.CreateMaterial` |

### Strumenti

| File | Contenuto |
|---|---|
| `Tools/StandaloneTests/*` | i cinque eseguibili di test senza Unreal |
| `Tools/CheckSourceDiscipline.sh` | cinque regole di disciplina |
| `Tools/CheckShadowedParameters.py` | parametri che nascondono campi |
| `Tools/CheckModuleExports.py` | simboli invisibili fra moduli |
| `Tools/CheckUnityCollisions.py` | collisioni da build unity |
| `Tools/CheckOwnApiCalls.py` | metodi inesistenti |

### Pipeline

| File | Contenuto |
|---|---|
| `run.py` | punto d'ingresso |
| `make_synthetic_source.py` | sorgente finto per i test |
| `README.md` | installazione e uso |
| `geoworld/cli.py` | comandi e orchestrazione delle quote |
| `geoworld/tiling.py` | schema di tiling |
| `geoworld/tileformat.py`, `manifest.py` | formato e indice delle quote |
| `geoworld/geoid.py` | ondulazione del geoide |
| `geoworld/raster.py` | VRT, riproiezione, risoluzione, piramide |
| `geoworld/tilecut.py` | taglio in tile |
| `geoworld/state.py` | riavvio |
| `geoworld/fetch.py` | download del Copernicus DEM |
| `geoworld/environment.py` | `check-env` |
| `geoworld/imageformat.py`, `imagerymanifest.py` | formato e indice delle immagini |
| `geoworld/imagecut.py`, `imagerybuild.py` | la pipeline delle immagini |
| `geoworld/fetchimagery.py`, `mgrs.py` | download di Sentinel-2 |
| `tests/*.py` | 77 test |

### Documenti

| File | Contenuto |
|---|---|
| `README.md` | il progetto in breve |
| `docs/manuale/` | **questo manuale** |
| `docs/consegna.md` | stato, comandi, errori noti, come continuare |
| `docs/programma.md` | il programma delle sei fasi |
| `docs/dati.md` | da dove vengono i dati, licenze |
| `docs/faseN-design.md` | decisioni e motivazioni, fase per fase |
| `docs/faseN-verifica.md` | come verificare, fase per fase |
| `docs/fase7-design.md` | entità e interoperabilità (solo design) |
| `docs/issues-aperte.md` | problemi noti non risolti |

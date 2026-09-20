# GeoWorld — documento di consegna

Tutto quello che serve per riprendere il lavoro da solo.
Ultimo aggiornamento: 2026-09-20. Branch: `claude/charming-goodall-xd2r3g`.

---

## 1. Dove siamo

| Fase | Contenuto | Stato |
|---|---|---|
| 1 | Geodesia, georeferenziazione, origin rebasing | codice completo, **issue #1 aperta** |
| 2 | Pipeline dati offline (Python + GDAL) | completa, gira su dati veri |
| 3 | Loader asincrono, cache LRU, lettura dataset | **codice completo, mai compilato in UE** |
| 4 | Quadtree, selezione LOD, culling | **codice completo, mai compilato in UE** |
| 5 | Mesh, gonne, terreno a schermo | **codice completo, mai compilato in UE** |
| 6 | Ortofoto drappeggiate | **codice completo, mai compilato in UE** |

**Avvertenza sul C++:** nessuna riga di C++ di questo progetto e' mai stata
compilata con Unreal Engine. L'ambiente in cui e' stato scritto e' Linux senza
il motore. Quello che **e'** stato verificato:

* tutta la matematica pura, con test numerici eseguiti: **164 test C++** in
  totale (37 Fase 1 + 23 Fase 3 + 37 Fase 4 + 28 Fase 5 + 44 Fase 6), piu' 72
  test Python;
* le convenzioni UE controllate staticamente (bilanciamento parentesi,
  posizione dei `.generated.h`, guardie `WITH_EDITOR`, macro di export);
* il formato dei file, letto dal codice C++ vero contro un dataset vero.

Al primo build su Windows aspettati errori di compilazione da sistemare. Quelli
gia' incontrati e corretti, per riconoscerli se tornano:

| Sintomo | Causa |
|---|---|
| `cannot open source file "Unreal/..."` | tre moduli avevano tutti una cartella `Public/Unreal`. Ora si chiamano `Georeference`, `Streaming`, `Lod`. Se compare su file nuovi: **rigenera i project file di Visual Studio** |
| errore su un parametro chiamato come un membro | Unreal tratta lo shadowing come ERRORE. Convenzione: prefisso `In` (`bInEnabled`). Lo intercetta `Tools/CheckShadowedParameters.py` |
| errori di `std::max`, `std::numeric_limits` | include della standard library che gcc tira dentro da solo e MSVC no |
| `ModuleNotFoundError: No module named 'PIL'` lanciando un comando Python | manca Pillow, che serve solo alle ortofoto: `conda install -c conda-forge pillow`. `check-env` lo dice, e ora **parte anche senza** |
| `C2084: function 'X' already has a body` su una funzione in un namespace anonimo | **build unity**: Unreal incolla piu' .cpp dello stesso modulo in una sola unita' di traduzione, e due namespace anonimi diventano lo stesso namespace. Non rinominare: metti la funzione in un header condiviso. Lo intercetta `Tools/CheckUnityCollisions.py` |
| `C2039: 'X' is not a member of Y` su una classe nostra | nome di metodo sbagliato. Lo intercetta `Tools/CheckOwnApiCalls.py`, che suggerisce anche il nome giusto |
| `missing type specifier` su una riga `static FAutoConsoleCommand...` | il tipo non esiste. **`FAutoConsoleCommandWithArgs` NON esiste**: i validi sono `FAutoConsoleCommand` (che accetta anche un delegato con argomenti), `...WithWorld`, `...WithWorldAndArgs`, `...WithOutputDevice`, `...WithArgsAndOutputDevice`, `...WithWorldArgsAndOutputDevice`. Lo intercetta la regola 5 di `CheckSourceDiscipline.sh` |
| `unresolved external symbol` su una funzione dello strato puro | in Unreal ogni modulo e' una DLL: un simbolo definito in un `.cpp` non e' visibile fuori se non esportato. **Lo strato puro e' header-only**, appunto per non doverlo esportare. Lo intercetta `Tools/CheckModuleExports.py` |

Dopo aver aggiunto file o cartelle: tasto destro sul `.uproject` ->
**Generate Visual Studio project files**. IntelliSense non li vede finche' non
lo fai, e segnala include inesistenti che il compilatore invece risolve.

---

## 2. Comandi, in ordine di utilita'

### Pipeline dati (Windows, ambiente conda `geoworld`)

```bat
cd Pipeline
python run.py check-env                              :: SEMPRE per primo
python run.py fetch --area test -o dati/copernicus   :: DEM libero, 19 MB
python run.py build -i "dati/copernicus/*.tif" -o dataset/test
python run.py verify -o dataset/test
python -m unittest discover -s tests                 :: 32 test
```

`check-env` e' l'unico comando che sta fra te e un dataset sbagliato di 48
metri: dice se la griglia geoidica c'e' davvero e quale installazione di PROJ
sta vincendo.

### Test C++ senza Unreal

```bat
cmake -S Plugins/GeoWorld/Tools/StandaloneTests -B build
cmake --build build
build\Debug\geocore_tests.exe                       :: 37 test
build\Debug\geotiles_tests.exe dataset\test         :: 23 test
build\Debug\geoquadtree_tests.exe                   :: 37 test
build\Debug\geomesh_tests.exe                       :: 28 test
build\Debug\geoimagery_tests.exe                    :: 44 test
Plugins\GeoWorld\Tools\CheckSourceDiscipline.sh      :: serve bash (Git Bash)
```

`geotiles_tests` accetta come argomento la cartella di un dataset e ne verifica
indici, header e giunzioni leggendoli davvero.

### Console di Unreal

```
geo.Help                       elenco dei comandi di Fase 1
geo.Diag                       diagnosi del jitter (vedi issue #1)
geo.Debug 1                    overlay
geo.SpawnMarkers               cubi di verifica sull'Italia
geo.Goto <lat> <lon> [quota]   teletrasporto su coordinate (quota ellissoidica)
geo.Goto <nome> [quota]        teletrasporto su un luogo noto (quota SUL SUOLO)
geo.Places                     elenco dei 28 luoghi noti
geo.Fly                        camera di volo nel Play, velocita' ~ quota
geo.Fly.Speed <x>              moltiplicatore della velocita' di volo
geo.ViewSpeed <1..8> [x]       velocita' della camera del viewport dell'editor

geo.Tiles.Demo <cartella>      fa tutto in un colpo: apri, vai, carica, mostra
geo.Tiles.Open <cartella>      apre un dataset
geo.Tiles.Info                 livelli, quote, indici in memoria
geo.Tiles.Stats                cache, hit rate, tempi di caricamento
geo.Tiles.LoadAround [liv] [r] carica un riquadro di tile attorno alla camera
geo.Tiles.Load <livello>       carica la tile sotto la camera
geo.Tiles.Debug <0|1>          overlay con le statistiche di streaming
geo.Tiles.Draw <0|1>           volumi delle tile disegnati nel mondo
geo.Tiles.Budget <MB>          budget della cache a caldo
geo.Tiles.Clear                svuota

geo.Lod.Demo <cartella>        apre, si posiziona e accende la selezione LOD
geo.Lod.Error <pixel>          soglia dell'errore su schermo (la manopola del LOD)
geo.Lod.Freeze <0|1>           congela la vista: mostra cosa il culling ha scartato
geo.Lod.Draw <0|1>             tassellatura scelta, un colore per livello
geo.Lod.Stats                  nodi visitati, scarti, tempo di selezione

geo.Terrain.Demo <cartella>    apre un dataset e disegna il terreno vero
geo.Terrain.Enable <0|1>       costruzione della geometria
geo.Terrain.Wireframe <0|1>    reticolo dei triangoli
geo.Terrain.Skirt <0|1>        gonne ai bordi: spegnile per VEDERE le crepe
geo.Terrain.FlipWinding <0|1>  se il terreno e' invisibile dall'alto, e' questo
geo.Terrain.Budget <N>         tile costruite per frame (default 4)
geo.Terrain.Stats              tile in scena, triangoli, tempo di costruzione

geo.Imagery.Demo <q> <o>       terreno vestito con le ortofoto, in un colpo
geo.Imagery.Open <cartella>    apre un dataset di ortofoto
geo.Imagery.Enable <0|1>       drappeggio
geo.Imagery.Checker <0|1>      scacchiera: e' cosi' che si verificano le UV
geo.Imagery.Budget <N>         texture create per frame
geo.Imagery.Stats              statistiche del drappeggio
geo.Imagery.CreateMaterial     costruisce M_GeoTerrain (solo editor)
```

Se hai un dataset e vuoi vedere subito qualcosa, il comando e'
`geo.Terrain.Demo <cartella>`: fa tutto, dalla apertura del dataset al terreno
a schermo.

---

## 3. Il tuo database da 200 GB

Verificato con GDAL 3.8 quali formati sono leggibili.

| Cartella | Estensione | Cosa e' | Verdetto |
|---|---|---|---|
| Elevation | `.dt2` | **DTED livello 2** | **usalo** |
| Imagery | `.ECW` + `.eww` + `.prj` | immagini compresse ERDAS | serve un driver, vedi sotto |
| Models | `.flt` | ambiguo, vedi sotto | fuori dalle Fasi 1-6 |
| SourceData | `.shp .shx .dbf .prj` | shapefile (vettoriale) | fuori dalle Fasi 1-6 |

### Elevation `.dt2` — va benissimo, provato

DTED2 e' 1 arcosecondo (~30 m), int16, nodata `-32767`, orizzontale WGS84,
**verticale MSL, che per DTED significa EGM96**.

Ho generato un DTED2 sintetico e ci ho fatto girare la pipeline completa:
livello 13 scelto in automatico, 2.906 tile, `verify` pulito. Il comando e':

```bat
python run.py build -i "D:\db\Elevation\**\*.dt2" -o dataset\italia ^
    --vertical-crs EPSG:5773 --name "DTED2"
```

**`--vertical-crs EPSG:5773` non e' opzionale**: il default e' EGM2008, giusto
per TINITALY e Copernicus, sbagliato per DTED. Sbagliarlo introduce fino a un
paio di metri di errore verticale — poco, ma sistematico e inutile.

Nota: `.dt2` e' spesso organizzato in `dted/e012/n41.dt2`. Se il glob ricorsivo
non funziona, passa piu' pattern: `-i "D:\db\Elevation\*\*.dt2"`.

Risoluzione equivalente al Copernicus GLO-30, quindi tre volte piu' grossolano
di TINITALY. Se il tuo obiettivo resta l'Italia a 10 m, TINITALY resta la scelta
migliore; il `.dt2` va benissimo per andare avanti subito e per l'estero.

### Imagery `.ECW` — serve una conversione

ECW e' un formato proprietario: GDAL puo' leggerlo solo se compilato con l'SDK
ERDAS, che per licenza **non e' incluso nelle build conda-forge**. Nella build
che ho qui il driver non c'e'. Controlla la tua:

```bat
gdalinfo --formats | findstr /I ecw
```

* **Se compare**, sei a posto: la Fase 6 lo leggera' direttamente.
* **Se non compare**, converti una volta sola in GeoTIFF o COG con uno
  strumento che sappia leggere ECW (QGIS con build ECW, il visualizzatore
  gratuito ERDAS, o GDAL compilato con l'SDK). E' un passaggio da fare una
  volta, non a ogni build.

`.eww` e' il world file (georeferenziazione) e `.prj` la proiezione: vanno
tenuti a fianco del `.ECW`, GDAL li legge da soli.

### Models `.flt` — probabilmente non quello che sembra

`.flt` ha due significati e vanno distinti prima di fare qualunque cosa:

1. **OpenFlight** — formato di modelli 3D per simulazione visiva. Data la
   cartella "Models" e la struttura del database (Elevation / Imagery / Models /
   SourceData, tipica dei database di simulazione), **e' quasi certamente
   questo**;
2. **ESRI float grid** — raster binario, sempre accompagnato da un `.hdr`.

Come distinguerli: se a fianco c'e' un `.hdr` e' un raster; altrimenti apri il
file con un editor esadecimale e guarda i primi byte — OpenFlight comincia con
un record di intestazione `db`.

Se e' OpenFlight: sono edifici, alberi, oggetti di scena. Ne' GDAL ne' Unreal li
leggono nativamente. Non servono per le Fasi 1-6, che riguardano il terreno.
Diventeranno interessanti dopo, e richiederanno una conversione (Blender con
plugin, o strumenti Presagis).

### SourceData `.shp` — utile, ma dopo

Shapefile: dati vettoriali (strade, costruzioni, coste, idrografia). GDAL/OGR li
legge senza problemi. Non servono per il terreno; serviranno se vorrai strade,
maschere d'acqua o confini disegnati sopra.

---

## 4. Issue aperte

### #1 — Jitter sui cubi di verifica (Fase 1)

Gli spigoli tremolano quando la camera si muove, su tutti i cubi. Un bug reale
e' stato trovato e corretto (`UGeoreferenceSubsystem` non era legato a nessun
mondo, quindi il rebasing non partiva mai), **ma il sintomo persiste**.

Nessuno ha ancora riportato l'output di `geo.Diag`, che e' esattamente lo
strumento per distinguere le tre cause possibili:

1. `Tick eseguiti: 0` -> il tick non gira, il rebasing non puo' funzionare;
2. distanza dall'origine oltre soglia -> il rebase non scatta;
3. coordinate piccole e corrette -> **e' l'antialiasing temporale**. TSR e' il
   default di UE5 e su uno spigolo netto senza texture contro il cielo vuoto
   produce esattamente questo sintomo. Verifica in cinque secondi con
   `r.AntiAliasingMethod 0`: se sparisce, la geodesia non c'entra.

La mia ipotesi, non verificata, e' la 3. La Fase 5 e' stata scritta comunque,
perche' bloccarla su una issue senza dati non avrebbe prodotto informazione: se
il jitter e' TSR, riguarda ogni geometria e non la mesh. Resta pero' vero che
**finche' non e' chiusa non si possono valutare le finiture visive** del
terreno: un bordo che balla puo' essere antialiasing o precisione, e i due casi
si correggono in posti opposti.

### #2 — `build` "non funziona ancora bene"

Segnalato senza dettagli, quindi non diagnosticato. Quando ci torni, servono:

* il comando esatto;
* l'output completo dello stadio 1 (bbox, formato, **risoluzione sul terreno**,
  livello consigliato);
* l'output di `python run.py check-env`.

Cose gia' viste e corrette, da escludere per prime:

* risoluzione letta nelle unita' del CRS invece che in metri (dava livello 24);
* `_work/` di un tentativo fallito che lascia uno stato incoerente: **cancellala
  prima di rilanciare** dopo un errore;
* griglia geoidica assente, che fa fallire lo stadio 2.

---

## 5. Le invarianti da non rompere

Sono le decisioni su cui poggia tutto il resto. Cambiarne una significa toccare
piu' fasi contemporaneamente.

| Invariante | Perche' |
|---|---|
| **La posizione ECEF e' l'unica autorita'**, quella in unita' Unreal e' sempre derivata | Non si compone mai una trasformazione con la precedente, quindi il rebasing non accumula deriva. Misurato: 1000 rebase, 1.2e-10 m |
| **1 unita' Unreal = 1 cm, convertito in un punto solo** | `GeoUnits.h` + `FGeoreference`. Imposto dai tipi (`FEcef` in metri vs `FVector` in cm) e da `CheckSourceDiscipline.sh` |
| **Assi North->X, East->Y, Up->Z** | ENU e' destrorso, Unreal sinistrorso: serve una riflessione. Scambiare North/Est la fornisce e in piu' fa coincidere lo yaw con l'azimuth della bussola |
| **Tile 129x129 registrate sui nodi** | Il post 128 E' il post 0 della tile adiacente: le giunzioni sono esatte per costruzione, non "quasi" |
| **Quote ellissoidiche ovunque** | Il motore lavora sull'ellissoide. Le quote ortometriche del sorgente sono convertite una volta sola, nella pipeline |
| **I vertici delle mesh in frame NEU locale alla tile, in metri** (Fase 5) | Tiene i float piccoli: misurato 0.0001 m al livello 14, contro i 64 cm che si avrebbero in coordinate mondo. NEU e non ENU perche' ENU e' destrorso e `ToQuat()` su una riflessione restituisce spazzatura **senza errore** |
| **Un rebase non rigenera geometria**, cambia solo le trasformazioni | E' la ragione per cui il frame locale esiste. Un provider che ricostruisse le mesh a ogni rebase vanificherebbe la Fase 1 |
| **Nessun I/O sul game thread** | Una lettura sfortunata costa piu' di un frame intero |
| **La cache e' solo del game thread** | Niente lock nel percorso caldo. I worker consegnano via coda MPSC |

---

## 6. Mappa del codice

```
Plugins/GeoWorld/Source/
  GeoCore/        geodesia, georeferenziazione, rebasing        [Fase 1]
    Public/Geo/       C++ PURO, zero Unreal, testabile senza motore
    Public/Georeference/  ponte verso FVector/FQuat, subsystem, componenti
  GeoTiles/       formato tile, dataset, loader, cache          [Fase 3]
    Public/Tiles/     C++ PURO: TileKey, TilingScheme, TileFormat, TileCache
    Public/Streaming/     FGeoTileDataset, UGeoTileStreamingSubsystem
    TestData/         vettori di riferimento generati da Python
  GeoRender/      quadtree, LOD, mesh, terreno, ortofoto        [Fasi 4-6]
    Public/Imagery/   C++ PURO: ImageryMapping.h, il ritaglio delle UV
                      + UGeoImagerySubsystem, che veste le tile
    Public/Quadtree/  C++ PURO: bounding volume, frustum, orizzonte, selezione
    Public/Mesh/      C++ PURO: TileMesh.h, da 129x129 quote a vertici e gonne
    Public/Lod/       UGeoQuadtreeSubsystem, overlay e disegno della selezione
    Public/Terrain/   IGeoTerrainMeshProvider, provider DynamicMesh, subsystem
  GeoWorldEditor/ strumenti di editor                           [vuoto]

  Tools/StandaloneTests/   test C++ senza Unreal (CMake)
  Tools/CheckSourceDiscipline.sh     5 regole di disciplina
  Tools/CheckShadowedParameters.py   parametri che nascondono membri
  Tools/CheckModuleExports.py        simboli invisibili fra moduli (DLL)
  Tools/CheckUnityCollisions.py      nomi che la build unity farebbe scontrare
  Tools/CheckOwnApiCalls.py          metodi inesistenti sulle nostre classi

Pipeline/         pipeline dati Python + GDAL                   [Fase 2]
  geoworld/       tiling, tileformat, geoid, raster, tilecut, fetch, cli
  tests/          32 test, con sorgente sintetico

docs/             programma.md, dati.md, consegna.md (questo),
                  fase1-*.md, fase2-*.md, issues-aperte.md
```

**Le cartelle pubbliche hanno nomi univoci per modulo** (`Georeference`,
`Streaming`, `Lod`) e non si chiamano tutte `Unreal`. Con tre cartelle omonime
sui percorsi di inclusione, `#include "Unreal/X.h"` diventa ambiguo e
IntelliSense ci si perde: e' costato un giro di compilazione.

**La regola dei due strati**, in due parti:

1. sotto `Public/Geo`, `Public/Tiles`, `Public/Quadtree` e `Public/Mesh` non
   entra **nessun** include di Unreal;
2. quegli strati sono **header-only**. Non e' stile: in Unreal ogni modulo e'
   una DLL, e un simbolo definito in un `.cpp` non e' visibile agli altri moduli
   se non viene esportato con la macro API del modulo. Esportarlo
   significherebbe pero' mettere una macro del motore dentro lo strato che per
   definizione non deve sapere di stare dentro Unreal — proprio la perdita che
   la separazione esiste per evitare. Header-only risolve alla radice, e su
   funzioni matematiche di poche righe non costa niente.

Sotto `Public/Geo` e `Public/Tiles` non entra nessun include di Unreal. Cosi' quel codice si compila e si testa in un
secondo con un normale `g++`, invece che aprendo l'editor. Non e' purismo: e'
il motivo per cui la Fase 1 e la Fase 3 sono verificate numericamente pur non
essendo mai state compilate nel motore. `CheckSourceDiscipline.sh` lo impone.

---

## 7. Come continuare: oltre le sei fasi

Le sei fasi del programma iniziale sono fatte. Quello che segue e' l'elenco
onesto di cio' che manca, in ordine di quanto si fa sentire.

**1. La issue #1 sul jitter.** E' l'unica cosa aperta che riguarda la
correttezza e non la resa. Finche' non e' chiusa non si possono giudicare le
finiture visive. Serve l'output di `geo.Diag`.

**2. Mipmap sulle ortofoto.** Senza, a viste radenti il terreno sfarfalla. La
soluzione e' scriverli nel file (il formato ha gia' il byte del tipo di payload
e si presta) e caricarli tutti in `UTexture2D::CreateTransient`, che accetta un
numero di mip.

**3. Un secondo provider di mesh.** Oggi ogni tile e' un
`UDynamicMeshComponent` con la propria material instance: un draw call per
tile. Con qualche centinaio di tile diventa il collo di bottiglia. Il rimedio e'
un `FPrimitiveSceneProxy` custom con un atlante di texture, ed e' esattamente il
motivo per cui esiste `IGeoTerrainMeshProvider`: si scrive una seconda
implementazione e non si tocca nient'altro.

**4. BC1 per le ortofoto.** Otto volte meno memoria video e nessuna
decodifica. Serve un encoder nella pipeline, che ne' GDAL ne' Pillow forniscono.

**5. Dissolvenza fra livelli di immagine.** Oggi il passaggio e' uno stacco
netto.

**6. I dati vettoriali del tuo database** (le `.shp` in SourceData). Strade,
edifici, confini: sono la fase che il programma iniziale non aveva previsto e
che cambierebbe di piu' l'aspetto del risultato.

---

## 8. Cosa e' stato deciso nelle Fasi 5 e 6, in breve

Se riapri il codice fra sei mesi, questi sono i tre punti che sembrano
arbitrari e non lo sono. Le motivazioni estese sono in `docs/fase5-design.md`.

* **I vertici sono in un frame NEU locale alla tile, in metri — non ENU.** ENU
  e' destrorso, Unreal e' sinistrorso: una mesh in ENU richiederebbe una
  trasformazione del componente con determinante -1, e `FMatrix::ToQuat()` su
  una riflessione non fallisce, restituisce silenziosamente spazzatura. Il
  terreno comparirebbe specchiato. *(La versione precedente di questo documento
  suggeriva ENU: era sbagliato, e questa e' la correzione.)*
* **Un rebase non rigenera nessun vertice**, cambia solo le trasformazioni dei
  componenti (`RefreshTransforms`). E' l'incasso di tutto il lavoro della
  Fase 1, ed e' verificabile a video: dopo `geo.Rebase`, `Questo frame` resta
  `+0 -0`.
* **Le gonne servono solo fra livelli diversi.** Fra tile dello stesso livello
  le crepe non esistono per costruzione (overlap di 1 post), e i test lo
  misurano: 129 vertici di bordo coincidenti a 0.0001 m in ECEF, pur essendo le
  due tile costruite in frame locali diversi. Profondita' = passo del livello x
  111132 x 8, cioe' 152.6 m al livello 13, raddoppiando per livello.

### Fase 6

* **Due piramidi separate, un solo schema di tiling.** Separate perche'
  risoluzione, cadenza di aggiornamento e payload non hanno niente in comune;
  stesso tiling perche' cosi' la tile (L,X,Y) di terreno e quella di immagine
  coprono lo stesso rettangolo, e nel caso normale il drappeggio non richiede
  nessun calcolo.
* **I pixel sono registrati sulle AREE, i post delle quote sui NODI.** 256x256
  pixel coprono il rettangolo senza sovrapposizione. Duplicare una colonna sul
  bordo, per analogia con i post, produrrebbe una striscia disegnata due volte
  che sfarfalla. E 256 = 2 x 128 fa si' che l'immagine di livello L abbia la
  risoluzione al suolo del terreno di livello L+1.
* **Quando l'immagine giusta non c'e' si usa l'antenato**, con offset e scala in
  aritmetica intera. Con d = 0 la formula degenera nel caso normale: non e' un
  ramo separato. E si chiede sempre quella giusta, altrimenti resterebbe
  grossolana per sempre.
* **Offset e scala stanno in un parametro del MATERIALE, non nelle UV della
  mesh.** Scriverli nei vertici obbligherebbe a riscrivere 17.157 coordinate
  ogni volta che una tile si affina, cioe' proprio mentre ci si muove.
* **La cache e' diventata un template** (`TTileCache<Payload>`) e il pool di
  thread e' uscito dal subsystem (`FGeoLoaderPool`). Il pool si condivide, il
  lavoro no: come si legge e si interpreta un file dipende dal payload, e una
  classe base che provasse a condividerlo avrebbe un metodo virtuale per ogni
  differenza.

Limitazioni note, lasciate aperte di proposito:

* la mesh si costruisce **sul game thread** con un budget di 4 tile per frame.
  `BuildTileMesh` e' puro e senza stato: spostarlo sul pool della Fase 3 e' una
  modifica localizzata, da fare quando `Costruzione` nell'overlay lo chiedera';
* le normali ai bordi usano differenze unilaterali: possibile cucitura di
  illuminazione, geometria comunque continua;
* `bFlipWinding` ha un default scelto senza mai aver visto lo schermo. Se il
  terreno e' invisibile dall'alto, `geo.Terrain.FlipWinding 0` e poi si cambia
  il default in `FTileMeshParameters`;
* le ortofoto non hanno mipmap ne' trasparenza sulle tile parziali, e ogni tile
  ha la propria texture e la propria material instance.

---

## 9. Riferimenti rapidi

| Cosa | Dove |
|---|---|
| Programma delle sei fasi | `docs/programma.md` |
| Da dove vengono i dati, licenze, ortofoto | `docs/dati.md` |
| Design e motivazioni Fase 1 | `docs/fase1-design.md` |
| Verifica Fase 1 | `docs/fase1-verifica.md` |
| Design e motivazioni Fase 2 | `docs/fase2-design.md` |
| Verifica Fase 2 | `docs/fase2-verifica.md` |
| Design e motivazioni Fase 3 | `docs/fase3-design.md` |
| Verifica Fase 3, con la demo visiva | `docs/fase3-verifica.md` |
| Design e motivazioni Fase 4 | `docs/fase4-design.md` |
| Verifica Fase 4, con la demo visiva | `docs/fase4-verifica.md` |
| Design e motivazioni Fase 5 | `docs/fase5-design.md` |
| Verifica Fase 5, con la demo visiva | `docs/fase5-verifica.md` |
| Verifica Fase 6, con la demo visiva | `docs/fase6-verifica.md` |
| Design e motivazioni Fase 6 | `docs/fase6-design.md` |
| Issue aperte | `docs/issues-aperte.md` |
| Uso della pipeline, installazione Windows | `Pipeline/README.md` |

Numeri utili da tenere a mente:

| | |
|---|---|
| Una tile | 66.596 byte esatti, a qualunque livello |
| ULP di un float a distanza del geocentro | 64 cm |
| ULP di un float entro 10 km dall'origine | 0.625 mm |
| Ondulazione del geoide in Italia | +42 .. +52 m |
| Livello nativo TINITALY (10 m) | 14 |
| Livello nativo DTED2 / Copernicus (30 m) | 13 |
| Italia a livello 14 | ~4x10^5 tile, ~35 GB |
| Una mesh di tile | 17.157 vertici, 33.792 triangoli, 0.91 MB |
| Una tile di ortofoto | 256x256, 15-25 KB su disco, 256 KB in memoria video |
| Livello nativo Sentinel-2 (10 m) | 13 (le immagini sono il doppio piu' fini del terreno) |
| Profondita' della gonna al livello 13 | 152.6 m (raddoppia per livello) |

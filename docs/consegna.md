# GeoWorld — documento di consegna

Tutto quello che serve per riprendere il lavoro da solo.
Ultimo aggiornamento: 2026-09-17. Branch: `claude/charming-goodall-xd2r3g`.

---

## 1. Dove siamo

| Fase | Contenuto | Stato |
|---|---|---|
| 1 | Geodesia, georeferenziazione, origin rebasing | codice completo, **issue #1 aperta** |
| 2 | Pipeline dati offline (Python + GDAL) | completa, gira su dati veri |
| 3 | Loader asincrono, cache LRU, lettura dataset | **codice completo, mai compilato in UE** |
| 4 | Quadtree, selezione LOD, culling | **codice completo, mai compilato in UE** |
| 5 | Mesh e skirt — **primo terreno visibile** | da fare |
| 6 | Imagery drappeggiata | da fare |

**Avvertenza sul C++:** nessuna riga di C++ di questo progetto e' mai stata
compilata con Unreal Engine. L'ambiente in cui e' stato scritto e' Linux senza
il motore. Quello che **e'** stato verificato:

* tutta la matematica pura, con test numerici eseguiti (37 test Fase 1 +
  23 test Fase 3);
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
build\Debug\geocore_tests.exe
build\Debug\geotiles_tests.exe dataset\test
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
geo.Goto <lat> <lon> [quota]   teletrasporto

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
```

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

La mia ipotesi, non verificata, e' la 3. Va risolta **prima della Fase 5**,
altrimenti sara' impossibile distinguere un problema di precisione da un
problema di generazione della geometria.

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
| **I vertici delle mesh in frame locale al tile** (Fase 5) | Tiene i float piccoli: entro 10 km l'ULP e' 0.6 mm, a distanza del geocentro 64 cm |
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
  GeoRender/      quadtree, LOD, mesh                           [Fasi 4-5]
  GeoWorldEditor/ strumenti di editor                           [vuoto]

  Tools/StandaloneTests/   test C++ senza Unreal (CMake)
  Tools/CheckSourceDiscipline.sh

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

1. sotto `Public/Geo`, `Public/Tiles` e `Public/Quadtree` non entra **nessun**
   include di Unreal;
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

## 7. Come continuare: Fase 4 (quadtree e LOD)

Quello che c'e' gia' e che serve:

* `FTileKey::GetParent()` / `GetChild(i)` — la struttura ad albero;
* `FGeoTileDataset::TileExists()` — se raffinare e' possibile, **senza toccare
  il disco**;
* `FGeoTileDataset::GetTileHeightRange()` — min/max quota **senza caricare la
  tile**: e' cio' che serve per costruire il bounding volume e fare culling
  prima di decidere;
* `UGeoTileStreamingSubsystem::RequestTile(Key, Priority)` — la priorita' va
  derivata dall'errore su schermo, cosi' le tile guardate arrivano per prime.

Lo scheletro dell'algoritmo:

```
per ogni nodo, partendo dalle radici (livello minimo):
    bounds = tile bounds + [minH, maxH] dall'indice
    se fuori dal frustum            -> scarta
    se oltre l'orizzonte            -> scarta        (culling sull'ellissoide)
    errore = ErroreGeometrico(livello) / DistanzaDallaCamera
    se errore < soglia  oppure  sono al livello massimo:
        richiedi la tile, disegnala
    altrimenti:
        se i 4 figli sono gia' in cache -> ricorri
        altrimenti -> richiedili e intanto disegna questo
```

Tre cose da non sbagliare:

1. **Non aspettare i figli.** Se il nodo corrente e' caricato, disegnalo e
   chiedi i figli per i frame successivi. Aspettare significa buchi visibili.
2. **Il bounding volume deve usare min/max veri**, non una quota costante:
   sull'Etna la differenza fra 0 e 3357 m decide se il tile e' visibile.
3. **Il horizon culling** sull'ellissoide e' cio' che evita di considerare meta'
   pianeta a ogni frame. La normale geodetica e il raggio ellissoidico sono gia'
   in `GeoCore/Public/Geo/Ellipsoid.h`.

L'errore geometrico di un livello e' il passo fra post in metri: e' gia'
calcolabile con `PostSpacingDeg(Level) * 111132`.

## 8. Fase 5 (mesh), le trappole note

* I vertici vanno generati in un **frame ENU centrato sul tile**
  (`GeoCore/Public/Geo/EnuFrame.h`). Solo la trasformazione del componente
  cambia a ogni rebase: **nessun vertice va rigenerato**.
* Le **skirt** sono bordi verticali che scendono dal perimetro del tile. Servono
  contro le crepe fra livelli **diversi**, dove i post non coincidono. Fra tile
  dello stesso livello le crepe non esistono per costruzione (overlap di 1
  post), ed e' verificato.
* Profondita' della skirt: proporzionale al passo dei post del livello, non
  costante. Troppo corta lascia crepe visibili in lontananza, troppo lunga
  produce pareti visibili sulle pendenze.
* Isola la generazione dietro un'interfaccia: si comincia con
  `UDynamicMeshComponent` (semplice, modulo `GeometryFramework` da aggiungere a
  `GeoRender.Build.cs`) ma si finira' con un `FPrimitiveSceneProxy` custom.
* **Risolvi la issue #1 prima di questa fase.**

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

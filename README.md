# GeoWorld

Motore di terreno geospaziale WGS84 per Unreal Engine 5.8, scritto da zero.
Nessun plugin di terzi, nessun Cesium, nessun Landscape di Unreal.
Primo target: l'Italia.

Il progetto `TestNewIG` e' un guscio vuoto: tutto il codice vive in
`Plugins/GeoWorld`.

## Moduli

| Modulo | Tipo | Contenuto |
|---|---|---|
| `GeoCore` | Runtime | Geodesia, georeferenziazione, origin rebasing |
| `GeoTiles` | Runtime | Formato tile, loader asincrono, cache LRU, ortofoto *(Fasi 2/3/6)* |
| `GeoRender` | Runtime | Quadtree, LOD, mesh, terreno, drappeggio, strade, acqua, vento, pioggia e neve, fulmini, nuvole *(Fasi 4/5/6/8/9)* |
| `GeoWorldEditor` | Editor | I materiali costruiti da codice (`geo.Imagery.CreateMaterial`) |

`GeoCore` e' diviso in due strati fisicamente separati:

* `Public/Geo`, `Private/Geo` — **C++ puro**, zero include di Unreal. Compilabile
  e testabile senza il motore.
* `Public/Georeference`, `Private/Georeference` — il ponte verso `FVector`, `FQuat`, i
  subsystem e i componenti.

## Stato

- [x] **Fase 1** — geodesia, georeferenziazione, origin rebasing, test
      *(nota: resta aperta la issue #1 sul jitter, vedi `docs/issues-aperte.md`)*
- [x] **Fase 2** — pipeline dati offline (Python + GDAL, TINITALY -> piramide di tile)
- [x] **Fase 3** — loader asincrono, cache LRU, lettura dataset *(mai compilata in UE)*
- [x] **Fase 4** — quadtree, selezione LOD, culling *(mai compilata in UE)*
- [x] **Fase 5** — generazione mesh, gonne, terreno a schermo *(mai compilata in UE)*
- [x] **Fase 6** — ortofoto drappeggiate *(mai compilata in UE)*
- [ ] Fase 7 — entità e interoperabilità (CIGI, DIS, HLA, memoria condivisa)
      *(solo design: `docs/fase7-design.md`)*
- [x] **Fase 8** — strade, ferrovie e piste da OpenStreetMap, dipinte da lontano e
      **3D da vicino** *(`docs/fase8-design.md`, prova: `docs/fase8-verifica.md`,
      strade 3D, transizioni morbide del terreno e flash: `docs/strade-3d.md`)*
- [x] **Fase 9** — acqua: mare (anche aperto, lontano dalla costa), laghi, fiumi,
      con le onde dal vento; stampe della posizione a destra *(`docs/fase9-design.md`,
      prova: `docs/fase9-verifica.md`, per chi impara: `docs/manuale/13-acqua.md`)*
- [x] **Vento** — simulazione: strato limite, rugosità, rilievi, raffiche che
      viaggiano, venti in quota; primo pezzo del meteo della Fase 12
      *(`docs/vento-design.md`, prova: `docs/vento-verifica.md`, per chi impara:
      `docs/manuale/14-vento.md`)*
- [x] **Pioggia, neve, grandine** — rovesci che viaggiano con il vento in quota,
      pioggia in valle e neve in montagna (zero termico e quota neve), grandine
      nei nuclei dei temporali; particelle attorno alla camera, suolo bagnato e
      innevato, visibilità con la nebbia; fulmini con il tuono sintetizzato;
      nuvole volumetriche (cumuli e nubi di pioggia sopra i rovesci);
      ghiaccio sul suolo (bagnato che gela, gelicidio) e grandine posata in
      strisce sotto i temporali
      *(`docs/meteo-design.md`, prova:
      `docs/meteo-verifica.md`, per chi impara: `docs/manuale/15-meteo.md`)*
- [x] **Fase 10 — edifici e monumenti** — le sagome di OpenStreetMap estruse
      a runtime attorno alla camera, con altezze, piani, tetti (a falde, a
      padiglione, a piramide, a cupola) e colori dai tag o stimati dalla
      classe; cortili e parti (`building:part`); finestre e marcapiani
      disegnati dal materiale, neve sui tetti; i **monumenti** da una
      libreria in Project Settings (id Wikidata o OSM → un modello, anche
      Nanite) al posto delle loro sagome *(`docs/edifici-design.md`, prova:
      `docs/edifici-verifica.md`, per chi impara: `docs/manuale/17-edifici.md`)*
- [x] **Fase 11, primo pezzo: le piante** — alberi e arbusti dove dice ESA
      WorldCover (10 m, letto dalla rete solo per la tua area), latifoglie,
      conifere, pini domestici e arbusti scelti dalla quota, fino al limite del
      bosco; modelli **Nanite** da una libreria in Project Settings (di default
      cotti da `geo.Vegetation.CreateModels`, o modelli tuoi), istanziati in celle attorno alla camera,
      appoggiate sul terreno disegnato; piu' rade da lontano, mosse dal vento,
      imbiancate dalla neve *(`docs/vegetazione-design.md`, prova:
      `docs/vegetazione-verifica.md`, per chi impara:
      `docs/manuale/16-vegetazione.md`)*. Gli animali verranno dopo.
- [x] **Fase 12, primo pezzo: il giorno e la notte** — il sole e la luna
      all'ora giusta per il luogo e la data (gli algoritmi di Meeus, provati
      sugli esempi del libro e sull'eclisse del 2024), l'ora legale, il tempo
      che corre alla velocità scelta; il sole del livello che gira (le ombre
      con lui), la luna con la luce della sua fase, la notte scura ma
      leggibile, stelle e Via Lattea che girano col cielo, la luna con le
      fasi, le finestre degli edifici accese la sera *(`docs/cielo-design.md`,
      prova: `docs/cielo-verifica.md`, per chi impara: `docs/manuale/18-cielo.md`)*
- [x] **Fase 12, secondo pezzo: le brezze** — i venti che fa il sole, dentro
      il campo del vento: brezza di mare il pomeriggio e di terra la notte
      (dove c'e' acqua entro una ventina di km), il vento che sale sui pendii
      al sole e scende di notte, quello di valle; ognuna con il suo ritardo,
      il suo profilo e la corrente di ritorno in quota, spente dal vento forte
      e dalle nuvole *(`docs/vento-design.md` 2.6, prova:
      `docs/vento-verifica.md` 5b, per chi impara: `docs/manuale/14-vento.md` 14.6)*
- [ ] Fase 12, il resto — le luci delle strade, le stelle vere da un catalogo
      *(il perché di quest'ordine: `docs/fase8-design.md`, sezione 0)*

## Formato dei dati

| | |
|---|---|
| Tiling | geografico WGS84, livello 0 = 2x1, livello L = 2^(L+1) x 2^L |
| Tile | 129x129 post float32, registrati sui nodi (1 post di overlap condiviso) |
| Percorso | `<root>/<level>/<x>/<y>.ght` (+ header di 32 byte) |
| Indice | `<root>/<level>/index.bin`, record di 16 byte per tile con min/max |
| Manifest | `<root>/manifest.json`, metadati globali |
| Quote | **ellissoidiche** WGS84 (ortometriche del sorgente + ondulazione del geoide) |
| Ortofoto | piramide **separata**, stesso tiling; 256x256 pixel registrati sulle AREE (nessuna sovrapposizione), payload JPEG |
| Strade | tile **vettoriali** `.gvt`, stesso tiling, livelli 10..13; linee in int16 locali (extent 16384, buffer 1/8), disegnate a runtime |
| Vegetazione | tile `.gvg` a UN livello (13: 2,4 km, 256x256 pixel da ~10 m): la classe della copertura del suolo (bosco, arbusti, prato, campi, costruito, ...) per pixel, zlib; solo dove cresce qualcosa |
| Edifici | tile `.gbd` a UN livello (15: ~600 m): le sagome di OSM (contorno e cortili, vertici int16) con classe, altezze, tetto, colori, id OSM e Wikidata, zlib; solo dove c'e' almeno un edificio |
| Acqua | tile `.gwa`, una per tile di terreno: superficie e fondale sui 129x129 post, raster 256x256 (distanza dalla riva, onde, corrente), zlib; piu' il geoide e il mare aperto (`opensea.bin`: le tile senza terreno tutte in mare, e il fondale) |

Dettagli e motivazioni in `docs/fase2-design.md`.

## Convenzioni fissate

| | |
|---|---|
| Ellissoide | WGS84 (`a = 6378137`, `1/f = 298.257223563`) |
| Angoli | radianti internamente, gradi solo al confine |
| Lunghezze | metri in `double` ovunque tranne il confine con Unreal |
| Unita' Unreal | 1 uu = 1 cm, conversione in **un solo punto** (`GeoUnits.h` + `FGeoreference`) |
| Assi | North -> +X, East -> +Y, Up -> +Z (yaw di Unreal = azimuth della bussola) |
| Quote | ellissoidiche, **non** ortometriche |

## Verifica rapida (senza Unreal, gira ovunque)

```bash
cmake -S Plugins/GeoWorld/Tools/StandaloneTests -B build
cmake --build build
./build/geocore_tests        # 51 test  -- geodesia, rebasing, formato delle stampe (lat, lon, quota, vento, meteo)
./build/geotiles_tests       # 22 test  -- formato tile, cache (piu' quelli su un dataset vero)
./build/geoquadtree_tests    # 86 test  -- LOD, culling, orizzonte, residenza
./build/geomesh_tests        # 41 test  -- mesh, gonne, giunzioni, passo, geomorphing
./build/geoimagery_tests     # 52 test  -- drappeggio, formato immagine, mipmap
./build/georoads_tests       # 80 test  -- strade: formato .gvt, larghezze misurate, ritagli, strade 3D, marciapiedi e incroci
./build/geowater_tests Plugins/GeoWorld/Source/GeoTiles/TestData/water_fixture.gwa
                             # 71 test  -- acqua: formato .gwa, scavo, mesh, onde dal vento, texture, il Po vero, mare aperto
./build/geoweather_tests     # 109 test -- vento (strato limite, rugosita', rilievi, raffiche, brezze), precipitazione (quota neve, rovesci, visibilita', suolo, particelle), fulmini e tuono, nuvole e suolo (gli shader compilati come C++), ghiaccio, grandine posata
./build/geovegetation_tests Plugins/GeoWorld/Source/GeoTiles/TestData/vegetation_fixture.gvg
                             # 34 test  -- vegetazione: formato .gvg, griglia, chi cresce dove, forme (anche Nanite), materiale (colore, neve, vento), celle e bande
./build/geobuildings_tests Plugins/GeoWorld/Source/GeoTiles/TestData/buildings_fixture.gbd
                             # 26 test  -- edifici: formato .gbd, triangolazione con i cortili, muri e tetti, una tile intera, monumenti (anche a parti), materiale (finestre, neve, bagnato, finestre accese di notte)
./build/geosky_tests         # 33 test  -- cielo: giorni giuliani, tempo siderale, ora legale, sole e luna (gli esempi di Meeus, l'eclisse del 2024), levate e tramonti, luce del giorno e della notte, stelle e luna (lo shader compilato come C++)

./Plugins/GeoWorld/Tools/CheckSourceDiscipline.sh
python3 Plugins/GeoWorld/Tools/CheckShadowedParameters.py
python3 Plugins/GeoWorld/Tools/CheckModuleExports.py
python3 Plugins/GeoWorld/Tools/CheckUnityCollisions.py
python3 Plugins/GeoWorld/Tools/CheckOwnApiCalls.py
```

I controlli statici esistono perche' qui non c'e' Unreal: intercettano in locale
le classi di errore di compilazione gia' incontrate su Windows -- shadowing,
simboli non esportati fra moduli, tipi di comando console inesistenti,
collisioni da build unity, metodi inesistenti chiamati sulle nostre classi.

605 test C++ in totale, piu' 229 test Python della pipeline.

**Dopo la prima prova su Torino** (materiale, scene Sentinel, prestazioni): `docs/prova-torino.md`.
**Dopo la seconda** (troppi triangoli, RAM): `docs/prova-torino-2.md`.

## Pipeline dati

Niente da installare: `Pipeline\geoworld.bat` (Windows) o `Pipeline/geoworld.sh`
(Linux, macOS) la prima volta prepara da solo un ambiente privato con GDAL,
PROJ e il resto, in `Pipeline/ambiente/`; le volte dopo parte subito.
`geoworld <comando>` e' `python run.py <comando>` con l'ambiente giusto.

```bat
cd Pipeline
geoworld check-env                            :: diagnosi: GDAL, PROJ, griglie
geoworld fetch --area test -o dati/copernicus :: scarica un DEM libero
geoworld build -i "dati/copernicus/*.tif" -o dataset/test
geoworld verify -o dataset/test
costruisci-dted                                :: i tuoi DTED (.dt2): chiede la cartella, fa tutto

:: ortofoto (Fase 6): stessa struttura, dataset separato
geoworld fetch-imagery --area roma -o dati/sentinel
geoworld build-imagery -i @dati/sentinel/ordine_scene.txt -o dataset/ortofoto
geoworld verify-imagery -o dataset/ortofoto

:: tutta Italia: quote 30 m e ortofoto (96 scene, ~25 GB: prima guarda il --dry-run)
geoworld fetch --area italia -o dati/copernicus_italia
geoworld fetch-imagery --area italia -o dati/sentinel_italia --dry-run

:: acqua (Fase 9): i poligoni del mare una volta, poi OSM + terreno
geoworld fetch-water -o dati/acqua
geoworld build-water --osm dati/osm/nord-ovest-latest.osm.pbf --sea dati/acqua/water-polygons-split-4326.zip --terrain dataset/terreno -o dataset/acqua
geoworld verify-water -o dataset/acqua

:: vegetazione: la copertura del suolo di ESA WorldCover sull'area del terreno (dalla rete, a pezzi)
costruisci-vegetazione                         :: chiede il dataset del terreno, fa tutto
geoworld build-vegetation --terrain dataset/terreno -o dataset/vegetazione
geoworld verify-vegetation -o dataset/vegetazione

:: edifici (Fase 10): le sagome di OpenStreetMap sull'area del terreno
costruisci-edifici                             :: chiede terreno ed estratto OSM (lo scarica se manca), fa tutto
geoworld build-buildings --osm dati/osm/nord-ovest-latest.osm.pbf --terrain dataset/terreno -o dataset/edifici
geoworld verify-buildings -o dataset/edifici

geoworld test                                      :: 229 test, sorgente sintetico
```

Installazione, opzioni e conda a mano (se lo preferisci): `Pipeline/README.md`.
**Tutti i comandi della console di Unreal: `docs/comandi-unreal.md`** (in Unreal: `geo.Help`).
Da dove vengono i dati (DEM e ortofoto): `docs/dati.md`.
Quadro d'insieme delle sei fasi: `docs/programma.md`.
**Per riprendere il lavoro: `docs/consegna.md`.**
**Per studiare il progetto (e Unreal) da zero: `docs/manuale/00-indice.md`.**

## Verifica in Unreal

Vedi `docs/fase1-verifica.md`, `docs/fase2-verifica.md`, `docs/fase3-verifica.md`,
`docs/fase4-verifica.md`, `docs/fase5-verifica.md`, `docs/fase6-verifica.md`,
`docs/fase8-verifica.md` e `docs/fase9-verifica.md`.

Scorciatoie: `geo.Terrain.Demo <quote>` per il terreno,
`geo.Imagery.Demo <quote> <ortofoto>` per il terreno vestito,
`geo.Water.Demo <quote> <ortofoto> <acqua>` per l'acqua,
`geo.Vegetation.Demo <quote> <ortofoto> <vegetazione>` per gli alberi,
`geo.Buildings.Demo <quote> <ortofoto> <edifici>` per gli edifici,
`geo.Sky.Demo` per un giorno intero di corsa (`geo.Sky.Time 18:30` per un'ora precisa).

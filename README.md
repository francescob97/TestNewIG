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
| `GeoRender` | Runtime | Quadtree, LOD, mesh, terreno, drappeggio *(Fasi 4/5/6)* |
| `GeoWorldEditor` | Editor | Strumenti di editor *(piu' avanti)* |

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
./build/geocore_tests        # 37 test  -- geodesia, rebasing
./build/geotiles_tests       # 23 test  -- formato tile, cache
./build/geoquadtree_tests    # 42 test  -- LOD, culling, margine
./build/geomesh_tests        # 28 test  -- mesh, gonne, giunzioni
./build/geoimagery_tests     # 44 test  -- drappeggio, formato immagine

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

169 test C++ in totale, piu' 77 test Python della pipeline.

## Pipeline dati

```bat
cd Pipeline
python run.py check-env                            :: diagnosi: GDAL, PROJ, griglie
python run.py fetch --area test -o dati/copernicus :: scarica un DEM libero
python run.py build -i "dati/copernicus/*.tif" -o dataset/test
python run.py verify -o dataset/test

:: ortofoto (Fase 6): stessa struttura, dataset separato
python run.py fetch-imagery --area roma -o dati/sentinel
python run.py build-imagery -i "dati/sentinel/*_TCI.tif" -o dataset/ortofoto
python run.py verify-imagery -o dataset/ortofoto

python -m unittest discover -s tests               :: 63 test, sorgente sintetico
```

Su Windows serve conda: vedi `Pipeline/README.md`.
Da dove vengono i dati (DEM e ortofoto): `docs/dati.md`.
Quadro d'insieme delle sei fasi: `docs/programma.md`.
**Per riprendere il lavoro: `docs/consegna.md`.**
**Per studiare il progetto (e Unreal) da zero: `docs/manuale/00-indice.md`.**

## Verifica in Unreal

Vedi `docs/fase1-verifica.md`, `docs/fase2-verifica.md`, `docs/fase3-verifica.md`,
`docs/fase4-verifica.md`, `docs/fase5-verifica.md` e `docs/fase6-verifica.md`.

Scorciatoie: `geo.Terrain.Demo <quote>` per il terreno,
`geo.Imagery.Demo <quote> <ortofoto>` per il terreno vestito.

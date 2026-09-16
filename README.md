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
| `GeoTiles` | Runtime | Formato tile, loader asincrono, cache LRU *(Fase 2/3)* |
| `GeoRender` | Runtime | Quadtree, LOD, generazione mesh *(Fase 4/5)* |
| `GeoWorldEditor` | Editor | Strumenti di editor *(piu' avanti)* |

`GeoCore` e' diviso in due strati fisicamente separati:

* `Public/Geo`, `Private/Geo` — **C++ puro**, zero include di Unreal. Compilabile
  e testabile senza il motore.
* `Public/Unreal`, `Private/Unreal` — il ponte verso `FVector`, `FQuat`, i
  subsystem e i componenti.

## Stato

- [x] **Fase 1** — geodesia, georeferenziazione, origin rebasing, test
      *(nota: resta aperta la issue #1 sul jitter, vedi `docs/issues-aperte.md`)*
- [x] **Fase 2** — pipeline dati offline (Python + GDAL, TINITALY -> piramide di tile)
- [ ] Fase 3 — loader asincrono e cache LRU
- [ ] Fase 4 — quadtree, selezione LOD, culling
- [ ] Fase 5 — generazione mesh e skirt
- [ ] Fase 6 — imagery drappeggiata

## Formato dei dati

| | |
|---|---|
| Tiling | geografico WGS84, livello 0 = 2x1, livello L = 2^(L+1) x 2^L |
| Tile | 129x129 post float32, registrati sui nodi (1 post di overlap condiviso) |
| Percorso | `<root>/<level>/<x>/<y>.ght` (+ header di 32 byte) |
| Indice | `<root>/<level>/index.bin`, record di 16 byte per tile con min/max |
| Manifest | `<root>/manifest.json`, metadati globali |
| Quote | **ellissoidiche** WGS84 (ortometriche del sorgente + ondulazione del geoide) |

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
./build/geocore_tests

./Plugins/GeoWorld/Tools/CheckSourceDiscipline.sh
```

## Pipeline dati

```bash
cd Pipeline
python run.py check-env                           # diagnosi: GDAL, PROJ, griglie
python run.py build -i "tinitaly/*.tif" -o dataset/italia
python -m unittest discover -s tests              # 26 test, sorgente sintetico
```

Su Windows serve conda: vedi `Pipeline/README.md`.
Per il quadro d'insieme delle sei fasi: `docs/programma.md`.

## Verifica in Unreal

Vedi `docs/fase1-verifica.md` e `docs/fase2-verifica.md`.

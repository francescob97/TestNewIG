# Pipeline dati di GeoWorld

Da GeoTIFF TINITALY 1.1 a piramide di tile di quote, pronta per il runtime.

## Prerequisiti

```bash
conda install -c conda-forge gdal pyproj numpy proj-data
# oppure, su Debian/Ubuntu:
apt install gdal-bin python3-gdal python3-pyproj python3-numpy proj-data
```

`proj-data` contiene la griglia geoidica EGM2008. Verifica prima di partire:

```bash
./run.py check-geoid
```

Deve stampare valori di N fra 42 e 52 m per i punti italiani. Se fallisce, il
messaggio dice esattamente cosa installare.

## Uso

```bash
./run.py build -i 'tinitaly/*.tif' -o /dati/geoworld/italia
```

Il livello massimo viene scelto da solo in base alla risoluzione del sorgente
(per TINITALY a 10 m: livello 14, passo ~9.5 m). Per una prova rapida:

```bash
./run.py build -i 'tinitaly/*.tif' -o /dati/prova --max-level 11
```

Rilanciare lo stesso comando riprende da dove si era interrotto: ogni stadio
concluso viene saltato, e le tile gia' scritte non si riscrivono.

### Opzioni che contano

| Opzione | Default | A cosa serve |
|---|---|---|
| `--max-level N` | risoluzione nativa | livello piu' fine da generare |
| `--min-level N` | 0 | livello piu' grossolano (radice del quadtree) |
| `--vertical-crs` | `EPSG:3855` (EGM2008) | datum verticale del sorgente; `EPSG:5773` = EGM96 |
| `--resampling` | `bilinear` | ricampionamento della riproiezione orizzontale |
| `--jobs N` | meta' dei core | processi per il ritaglio delle tile |
| `--redo STADIO...` | — | invalida e riesegue gli stadi indicati |
| `--force-tiles` | off | riscrive tutte le tile anche se presenti |

### Altri comandi

```bash
./run.py check-geoid                  # verifica la griglia geoidica
./run.py inspect dataset/14/17525/4389.ght
./run.py test-vectors -o vettori.json # valori di riferimento per il C++
```

## Stadi

| # | Stadio | Produce |
|---|---|---|
| 1 | `inventory` | `_work/source.vrt`, mosaico virtuale dei sorgenti |
| 2 | `geoid` | `_work/geoid_undulation.tif`, N sull'area (apribile in QGIS) |
| 3 | `warp` | `_work/height_L<max>.tif`, EPSG:4326 con quote ellissoidiche |
| 4 | `pyramid` | `_work/height_L<n>.tif` per ogni livello |
| 5 | `tiles` | `<level>/<x>/<y>.ght` e `<level>/index.bin` |
| 6 | `manifest` | `manifest.json` |

Gli intermedi in `_work/` si possono cancellare a piramide finita; servono solo
per riprendere un'elaborazione interrotta.

## Test

```bash
PYTHONPATH=. python -m unittest discover -s tests -v
```

I test di `test_dataset.py` generano un sorgente sintetico, ci fanno girare la
pipeline completa e verificano il risultato. Usano EGM96 perche' e' inclusa in
quasi tutte le distribuzioni di PROJ.

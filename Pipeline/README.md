# Pipeline dati di GeoWorld

Da GeoTIFF TINITALY 1.1 a piramide di tile di quote, pronta per il runtime.

## Installazione su Windows

Su Windows **usa conda** (Miniforge o Miniconda). Non e' preferenza stilistica:
`pip install gdal` non ha rotelle ufficiali su PyPI per Windows, quindi
proverebbe a compilare GDAL da sorgente, cosa che richiede il toolchain C++ e le
librerie di sistema. Con conda arriva tutto compilato e, soprattutto, con le
variabili `GDAL_DATA` e `PROJ_DATA` gia' impostate correttamente.

```bat
conda create -n geoworld python=3.11
conda activate geoworld
conda install -c conda-forge gdal pyproj numpy proj-data
```

`proj-data` e' il pacchetto che contiene la griglia geoidica EGM2008. Senza,
la pipeline si ferma allo stadio 2.

**Prima cosa da lanciare, sempre:**

```bat
cd Pipeline
python run.py check-env
```

Stampa versione di GDAL e PROJ, dove PROJ cerca i suoi dati, quali griglie
geoidiche trova e — soprattutto — se la trasformazione verticale **funziona
davvero**, provandola su punti italiani noti. Se qualcosa manca, dice cosa
installare. Su Windows capita spesso di avere piu' installazioni di PROJ
contemporaneamente (conda, OSGeo4W, quella dentro pyproj): `check-env` dice
quale sta vincendo.

Note specifiche Windows:

* si lancia `python run.py ...`, non `./run.py`;
* virgolette attorno ai pattern: `-i "tinitaly/*.tif"`;
* `--jobs` avvia processi separati, e su Windows ognuno reimporta GDAL: con
  poche tile conviene `--jobs 1`, il parallelismo ripaga sui livelli fini;
* l'antivirus che scansiona in tempo reale rallenta molto la scrittura di
  centinaia di migliaia di file piccoli. Vale la pena escludere la cartella di
  output.

## Installazione su Linux / macOS

```bash
conda install -c conda-forge gdal pyproj numpy proj-data
# oppure, su Debian/Ubuntu:
apt install gdal-bin python3-gdal python3-pyproj python3-numpy proj-data
```

## Non hai ancora i dati?

```bat
python run.py fetch --area test -o dati/copernicus
```

Scarica il Copernicus DEM GLO-30 (30 m, globale, libero, nessuna credenziale)
per l'area indicata. `--area test` e' una sola tile su Roma, 19 MB. Le altre
aree sono `roma`, `alpi`, `sicilia`, `italia`; oppure `--bbox OVEST SUD EST NORD`.
Con `--dry-run` elenca cosa scaricherebbe e quanto pesa, senza scaricare.

TINITALY (10 m, solo Italia) si scarica a mano dal sito INGV: vedi `docs/dati.md`.

## Uso

```bat
python run.py check-env                              :: sempre per primo
python run.py fetch --area test -o dati/copernicus   :: se non hai dati
python run.py build -i "dati/copernicus/*.tif" -o dataset/test
python run.py verify -o dataset/test                 :: controlla il risultato
```

Il livello massimo viene scelto da solo in base alla risoluzione del sorgente
(per TINITALY a 10 m: livello 14, passo ~9.5 m). Per una prova rapida:

```bash
python run.py build -i "tinitaly/*.tif" -o /dati/prova --max-level 11
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
| `--source-crs` | dal file | CRS del sorgente se i file non lo dichiarano (grid ESRI ASCII) |
| `--redo STADIO...` | — | invalida e riesegue gli stadi indicati |
| `--force-tiles` | off | riscrive tutte le tile anche se presenti |

### Altri comandi

```bat
python run.py check-env               :: diagnosi completa dell'ambiente
python run.py check-geoid             :: solo la griglia geoidica
python run.py fetch --area italia --dry-run -o dati    :: cosa scaricherei
python run.py verify -o dataset/italia                 :: controlla un dataset
python run.py inspect dataset/14/17525/4389.ght
python run.py test-vectors -o vettori.json   :: riferimento per il C++
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

Dalla cartella `Pipeline/`:

```bash
python -m unittest discover -s tests -v
```

I test di `test_resolution.py` verificano che la risoluzione del sorgente sia
misurata in metri sul terreno e non nelle unita' del CRS: e' la regressione di
un bug che portava la pipeline a chiedere il livello 24.

I test di `test_dataset.py` generano un TINITALY sintetico, ci fanno girare la
pipeline completa e verificano il risultato. Scelgono da soli il datum verticale
disponibile (EGM2008 se c'e', altrimenti EGM96).

Se nessuna griglia geoidica e' installata, i test **saltano** con un messaggio
che lo dice, invece di fallire. Se un passo della pipeline fallisce davvero, il
messaggio riporta il comando e lo stderr completo: se vedi un traceback che
finisce in `setUpClass` senza altro, stai usando una versione vecchia dei test.

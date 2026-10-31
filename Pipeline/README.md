# Pipeline dati di GeoWorld

Da GeoTIFF TINITALY 1.1 (o DTED, Copernicus, qualunque DEM che GDAL legge) a
piramide di tile di quote, pronta per il runtime. E poi ortofoto, strade, acqua.

## Installazione: nessuna

```bat
cd Pipeline
geoworld
```

(o doppio clic su `geoworld.bat`; su Linux e macOS `./geoworld.sh`)

La **prima volta** `geoworld`:

1. scarica **micromamba** (un solo eseguibile, ~15 MB, versione fissata e
   controllata con la sua impronta SHA-256: se il file non e' quello
   pubblicato, non lo usa);
2. prepara in `Pipeline/ambiente/` un Python con GDAL, PROJ, le griglie del
   geoide, numpy e Pillow, presi da `environment.yml` (~1 GB, da 5 a 15
   minuti secondo la rete);
3. lancia `check-env`.

Le volte dopo parte subito. Niente Miniforge, niente `conda activate`, niente
pip, niente PATH di sistema toccato: tutto sta in `Pipeline/ambiente/`.

| Comando | Cosa fa |
|---|---|
| `geoworld <comando> ...` | e' `python run.py <comando> ...` con l'ambiente giusto: **ogni comando di questa guida e della documentazione** si scrive cosi' |
| `geoworld test` | i test della pipeline |
| `geoworld ambiente rifai` | cancella e ricrea l'ambiente |
| cancellare `Pipeline/ambiente/` | toglie tutto; il prossimo `geoworld` lo rifa' |

Le versioni in `environment.yml` sono **fissate** (GDAL 3.8.5, PROJ 9.4,
numpy 1.26: quelle dei test). Se un aggiornamento del progetto le cambia,
`geoworld` se ne accorge e rifa' l'ambiente da solo.

In **PowerShell** la cartella corrente non e' nel PATH: si scrive
`.\geoworld ...`. Da un'altra cartella si puo' dare il percorso intero
(`D:\GeoWorld\Pipeline\geoworld build ...`): i percorsi relativi degli
argomenti restano relativi alla cartella da cui lanci.

### Se qualcosa non va

| Sintomo | Causa | Cosa fare |
|---|---|---|
| "non riesco a scaricare micromamba" o errori di rete durante la preparazione | niente internet, o un proxy aziendale | imposta `HTTPS_PROXY` (es. `set HTTPS_PROXY=http://proxy:8080`) e rilancia |
| la preparazione si interrompe a meta' | rete instabile | rilancia `geoworld`: riprende dai pacchetti gia' scaricati |
| errori su percorsi troppo lunghi | Windows limita i percorsi a 260 caratteri | sposta il repository in un percorso corto (`D:\GeoWorld`) |
| il disco si riempie, o OneDrive sincronizza per ore | l'ambiente pesa ~1 GB, e i dataset molto di piu' | tieni il repository fuori da OneDrive, o almeno `Pipeline\ambiente` e i dataset |
| tutto lentissimo | l'antivirus scansiona ogni file scritto | escludi `Pipeline\ambiente` e la cartella dei dataset |

### Con conda, a mano (se preferisci)

Funziona sempre anche la via classica, con Miniforge e lo stesso file:

```bat
mamba env create -f environment.yml
conda activate geoworld
python run.py check-env
```

`pip install gdal` invece **non** funziona su Windows: GDAL non ha rotelle
ufficiali su PyPI, e pip proverebbe a compilarlo.

Su Debian/Ubuntu c'e' anche la via del sistema:
`apt install gdal-bin python3-gdal python3-pyproj python3-numpy python3-pil proj-data`.

Note per Windows, in ogni caso:

* virgolette attorno ai pattern: `-i "tinitaly/*.tif"`;
* `--jobs` avvia processi separati, e su Windows ognuno reimporta GDAL: con
  poche tile conviene `--jobs 1`, il parallelismo ripaga sui livelli fini;
* l'antivirus che scansiona in tempo reale rallenta molto la scrittura di
  centinaia di migliaia di file piccoli. Vale la pena escludere la cartella di
  output.

## I tuoi DTED in un colpo: `costruisci-dted.bat`

Doppio clic su `costruisci-dted.bat` (o trascinaci sopra la cartella dei `.dt2`):

1. chiede la cartella e il nome del dataset;
2. cerca i `.dt2` anche nelle sottocartelle (`dted\E016\N38.dt2`...);
3. costruisce `Pipeline\dataset\<nome>` con il datum verticale dei DTED
   (`--vertical-crs EPSG:5773`, EGM96) e lo verifica;
4. mette negli appunti il comando per Unreal: nella console (tasto ~)
   Ctrl+V e Invio.

Da riga di comando: `costruisci-dted "D:\dati\dted" sud`. Interrotto, si
rilancia uguale e riprende. E' lo stesso di:

```bat
geoworld build -i "D:\dati\dted\**\*.dt2" -o dataset\sud --vertical-crs EPSG:5773 --name "DTED2 sud"
geoworld verify -o dataset\sud
```

`**` nei pattern scende in tutte le sottocartelle (vale per ogni comando che
prende dei file: `build`, `build-imagery`, `build-roads`, `build-water`).

## Non hai ancora i dati?

```bat
geoworld fetch --area test -o dati/copernicus
```

Scarica il Copernicus DEM GLO-30 (30 m, globale, libero, nessuna credenziale)
per l'area indicata. `--area test` e' una sola tile su Roma, 19 MB. Le altre
aree sono `roma`, `alpi`, `sicilia`, `italia`; oppure `--bbox OVEST SUD EST NORD`.
Con `--dry-run` elenca cosa scaricherebbe e quanto pesa, senza scaricare.

TINITALY (10 m, solo Italia) si scarica a mano dal sito INGV: vedi `docs/dati.md`.

## Uso

```bat
geoworld check-env                              :: sempre per primo
geoworld fetch --area test -o dati/copernicus   :: se non hai dati
geoworld build -i "dati/copernicus/*.tif" -o dataset/test
geoworld verify -o dataset/test                 :: controlla il risultato
```

Il livello massimo viene scelto da solo in base alla risoluzione del sorgente
(per TINITALY a 10 m: livello 14, passo ~9.5 m). Per una prova rapida:

```bash
geoworld build -i "tinitaly/*.tif" -o /dati/prova --max-level 11
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
geoworld check-env               :: diagnosi completa dell'ambiente
geoworld check-geoid             :: solo la griglia geoidica
geoworld fetch --area italia --dry-run -o dati    :: cosa scaricherei
geoworld verify -o dataset/italia                 :: controlla un dataset
geoworld inspect dataset/14/17525/4389.ght
geoworld test-vectors -o vettori.json   :: riferimento per il C++
```

### Strade, ferrovie, piste (Fase 8)

```bat
geoworld fetch-osm --area torino -o dati/osm           :: estratto OSM da Geofabrik
geoworld build-roads -i dati/osm/nord-ovest-latest.osm.pbf -o dataset/strade_torino --bbox 7.55 45.00 7.80 45.15
geoworld verify-roads -o dataset/strade_torino
geoworld inspect-roads dataset/strade_torino/13/8538/2044.gvt
```

Tre stadi (estrai, taglia, indici), riavviabili; il taglio va in parallelo
(`--jobs`). Serve GDAL con il driver OSM e GEOS: `check-env` lo dice. Le
decisioni sono in `docs/fase8-design.md`.

### Vegetazione (Fase 11)

```bat
geoworld build-vegetation --terrain dataset/terreno -o dataset/vegetazione
geoworld verify-vegetation -o dataset/vegetazione
geoworld inspect-vegetation dataset/vegetazione/13/8541/2044.gvg
```

La copertura del suolo di ESA WorldCover (10 m, gratuita, senza account) per
l'area del terreno (o `--bbox`), letta **dalla rete a pezzi**: si scaricano
solo i blocchi che servono. Senza rete: scarica i file `ESA_WorldCover_10m_2021_v200_*_Map.tif`
e passa la cartella con `--worldcover`. Riavviabile a blocchi di ~40 km
(`--force` rifà tutto). Le decisioni sono in `docs/vegetazione-design.md`.

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
geoworld test
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

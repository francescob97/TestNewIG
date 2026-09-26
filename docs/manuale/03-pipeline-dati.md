# Capitolo 3 — Dai file GIS alle tile (Fase 2)

> La parte del progetto che non gira in Unreal: un programma Python che prende
> i dati geografici così come li trovi in giro e li trasforma in qualcosa che il
> motore può caricare in un millisecondo.

---

## L'idea in breve

I dati di elevazione esistono già: TINITALY copre l'Italia a 10 metri, il
Copernicus DEM copre il mondo a 30 metri. Ma arrivano in forme scomode:

* in **proiezioni diverse** (TINITALY è in UTM, Copernicus in gradi);
* con quote misurate **dal livello del mare**, mentre il motore lavora
  sull'ellissoide;
* in **file grandi e irregolari**, pensati per essere aperti in un programma GIS,
  non letti a pezzetti in tempo reale.

La **pipeline** fa tutto il lavoro pesante **una volta sola**, offline, e produce
una piramide di piccole tile regolari. Il motore, a runtime, non deve fare
nessun calcolo geografico: legge un file, e ha già le quote giuste nel posto
giusto.

---

## 3.1 Perché una pipeline, e non leggere i GeoTIFF direttamente

La domanda è legittima: Unreal potrebbe aprire i GeoTIFF da solo.

| | Leggere i GeoTIFF a runtime | Pipeline offline |
|---|---|---|
| Riproiezione | a ogni caricamento | **una volta** |
| Conversione delle quote | a ogni caricamento | **una volta** |
| Livelli di dettaglio | da calcolare al volo | **già pronti** |
| Dipendenze del motore | GDAL, PROJ, griglie geoidiche | **nessuna** |
| Costo di una tile | decine di millisecondi | **meno di un millisecondo** |

L'ultima riga di "Dipendenze" è quella che decide: GDAL e PROJ sono librerie
enormi, con dati propri e comportamenti che cambiano fra versioni. Tenerle fuori
dal motore significa che il motore legge un formato semplice, fatto da noi,
documentato in un file, e che non può cambiare sotto i piedi.

**Scelta del linguaggio: Python + GDAL.** GDAL è lo standard di fatto per i
dati geografici (lo usano QGIS, ArcGIS, Google Earth Engine), e il suo binding
Python è il modo più diretto di usarlo. Su Windows si installa con **conda**,
perché GDAL non ha pacchetti ufficiali per `pip` su Windows:

```bat
conda create -n geoworld python=3.11
conda activate geoworld
conda install -c conda-forge gdal pyproj numpy pillow proj-data
```

---

## 3.2 Il tiling: come si taglia il mondo

Prima di tutto bisogna decidere **come dividere la Terra in pezzi**. È la scelta
più difficile da cambiare dopo, perché tutto — la pipeline, il motore, le
ortofoto — dipende da essa.

### La scelta: geografico WGS84

Il mondo si divide in rettangoli in **gradi di latitudine e longitudine**, e ogni
livello divide ogni rettangolo in quattro:

| Livello | Tile (X × Y) | Lato di una tile |
|---:|---|---|
| 0 | 2 × 1 | 180° |
| 1 | 4 × 2 | 90° |
| L | 2^(L+1) × 2^L | 180° / 2^L |
| 14 | 32.768 × 16.384 | 0,011° (circa 1 km) |

Al livello 0 ci sono **due** tile, non una: l'emisfero ovest e quello est. È così
perché il mondo è largo 360° e alto 180°, e due quadrati da 180° lo coprono
esattamente.

**X** cresce verso est partendo da longitudine −180°. **Y** cresce verso **sud**
partendo da latitudine +90°. Così la riga 0 di ogni tile è la più a nord, che è
anche la prima riga salvata nel file: niente inversioni da ricordare.

> 💡 **Esempio.** Il Colosseo (41,8902° N, 12,4922° E) al livello 14 sta nella
> tile `14/17521/4379`. Al livello 13 nella `13/8760/2189`: X e Y dimezzati, come
> deve essere per il padre.

File: `Pipeline/geoworld/tiling.py` (Python) e `GeoTiles/Public/Tiles/TilingScheme.h`
(C++). Sono **la stessa matematica scritta due volte**, e per assicurarsi che
coincidano la pipeline genera dei vettori di prova
(`GeoTiles/TestData/tiling_vectors.txt`) che il test C++ rilegge.

### Alternative considerate

| Schema | Usato da | Perché no |
|---|---|---|
| **Web Mercator** | Google Maps, OpenStreetMap | deforma enormemente verso i poli, e i poli non esistono proprio |
| **UTM** | cartografia militare | 60 zone diverse, con salti ai bordi di ogni zona |
| **Cubo sferico (S2)** | Google Earth | tile quasi uniformi, ma molto più complesso da capire e da debuggare |
| **Geografico WGS84** | Cesium, molti simulatori | ✓ semplice, continuo, un solo sistema per tutto il mondo |

Il difetto del geografico è che le tile **si restringono verso i poli**: a 45° di
latitudine una tile è larga il 70% di quanto è alta, a 70° il 34%. Per l'Italia
(36°–47°) non è un problema.

---

## 3.3 La tile: 129 × 129 punti

Ogni tile è una griglia di **129 × 129 quote** in `float32`. Le quote si chiamano
**post** (punti di campionamento).

### Perché 129 e non 128

Perché **i post del bordo sono condivisi con la tile accanto.**

Immagina due tile vicine. Se ognuna avesse 128 post, il bordo destro della prima
e il bordo sinistro della seconda sarebbero due righe di punti **diverse**,
distanti un passo. Fra le due mesh ci sarebbe un buco.

Con 129 post, la colonna 128 della prima tile **è la stessa** colonna 0 della
seconda: stesse coordinate, stessi valori. Le due mesh si toccano esattamente.

```
tile A                     tile B
post 0 ... 127  128        0   1 ... 128
               └──┴── sono lo STESSO punto
```

Si dice che i post sono **registrati sui nodi** (*gridline registration*). E il
numero 129 = 2⁷ + 1 ha una proprietà in più: due tile affiancate fanno
129 + 129 − 1 = **257 = 2 × 128 + 1** post, cioè esattamente i post di una tile
del livello superiore prima di dimezzarla. La piramide si costruisce senza resti.

Il capitolo 7 mostra che le **immagini** invece funzionano al contrario, e perché.

### Il formato del file `.ght`

Ogni tile è un file binario:

```
┌──────────────────────── 32 byte di header ────────────────────────┐
│ "GWHT" │ versione │ flag │ livello │ X │ Y │ 129 │ 129 │ min │ max │
└───────────────────────────────────────────────────────────────────┘
┌─────────────────── 129 × 129 × 4 = 66.564 byte ───────────────────┐
│  quote float32, little-endian, riga 0 a nord, colonna 0 a ovest   │
└───────────────────────────────────────────────────────────────────┘
                     totale: 66.596 byte, sempre
```

Il percorso è `<root>/<livello>/<X>/<Y>.ght`.

**Perché un header e non solo i numeri.** Il "magic" `GWHT` fa fallire subito
chi apre il file sbagliato — una tile di immagini, un file corrotto. Livello, X e
Y permettono di accorgersi di un file finito nella cartella sbagliata. Min e max
evitano di scorrere 16.641 valori quando serve solo sapere quanto è alta una
tile.

### Alternative per il formato

| Formato | Perché no |
|---|---|
| PNG a 16 bit | quantizza le quote: 65.536 livelli fra −500 e +5.000 m sono passi di 8 cm |
| GeoTIFF per tile | richiede GDAL nel motore (sezione 3.1) |
| Mesh quantizzate (come Cesium) | molto più compatte, ma molto più complesse da generare e da leggere |
| **Binario grezzo float32** | ✓ si legge con una `memcpy`, nessuna perdita, un solo formato da capire |

### L'indice: sapere prima di leggere

Accanto alle tile, ogni livello ha un file `index.bin`: per ogni tile esistente,
un record di 16 byte con X, Y, **quota minima e quota massima**.

A cosa serve? Il capitolo 5 lo spiega: per decidere se una tile è visibile serve
sapere quanto è alta, **prima** di leggerla. Senza l'indice, per sapere se una
tile di montagna è dietro una cresta bisognerebbe caricarla. Con l'indice, 16
byte bastano.

L'indice è ordinato per (Y, X): permette la ricerca binaria e rende il confronto
fra due versioni del dataset un semplice `diff`.

### Il manifest

`<root>/manifest.json` descrive tutto il dataset in un file leggibile: lo schema
di tiling, il formato, il bounding box, i livelli, la **provenienza** (quali file
sorgente, quale griglia geoidica). Fra sei mesi, è l'unico modo per sapere come
è stato fatto un dataset.

File: `tileformat.py` e `manifest.py` (Python), `TileFormat.h` (C++).

---

## 3.4 Le quote: livello del mare o ellissoide?

Questa è la trappola più insidiosa della pipeline, e merita spazio.

### Due modi di misurare un'altezza

* **Quota ortometrica**: rispetto al **livello medio del mare**. È quella delle
  carte topografiche, dei cartelli stradali, di TINITALY.
* **Quota ellissoidica**: rispetto all'**ellissoide WGS84**. È quella del GPS, e
  quella che serve al motore, perché il motore lavora in ECEF.

La superficie del livello del mare si chiama **geoide**, ed è irregolare: si alza
dove c'è più massa sotto (montagne, rocce dense) e si abbassa dove ce n'è meno.
La differenza fra geoide ed ellissoide si chiama **ondulazione**, `N`:

```
h_ellissoidica = H_ortometrica + N
```

In Italia `N` va da circa **+42 m** in Sicilia a **+52 m** sulle Alpi.

> 💡 **Esempio.** Il Colosseo è a circa 21 m sul livello del mare. L'ondulazione
> a Roma è di circa 48 m. Sull'ellissoide è quindi a circa **69 m**. Se ti
> dimentichi di `N`, tutta l'Italia sta **48 metri sotto** dove dovrebbe:
> il mare della tua scena non coincide con il mare dei dati, e ogni oggetto
> posizionato col GPS galleggia a mezz'aria.

### La trappola del "ballpark"

La via elegante sarebbe chiedere a PROJ (la libreria di proiezioni che GDAL usa)
di fare tutto insieme: riproiezione orizzontale e conversione verticale.

Il problema, **misurato su questa pipeline**: se la griglia del geoide non è
installata, PROJ ricade su una "trasformazione approssimata" (*ballpark*) che
**semplicemente non applica lo scostamento verticale**. Nessuna eccezione,
nessun avviso: restituisce le quote identiche a quelle di partenza. E il ballpark
è il comportamento **di default** di `gdalwarp`.

Il risultato sarebbe un dataset sbagliato di 48 metri su tutta l'Italia, e
plausibile a ogni ispezione superficiale.

### La soluzione: due passi espliciti

1. **Riproiezione orizzontale pura**, da UTM a gradi WGS84. Nessuna griglia,
   nessuna ambiguità.
2. **Somma esplicita di `N`**, preso da una griglia che la pipeline calcola da
   sé, con `allow_ballpark=False` — cioè: se la griglia manca, **fallisci**.

La griglia di `N` finisce su disco come GeoTIFF: puoi aprirla in QGIS, e se è
piena di zeri il problema si vede subito.

File: `Pipeline/geoworld/geoid.py`, che nel suo commento iniziale racconta tutto
questo in dettaglio.

---

## 3.5 La scoperta controintuitiva: l'allineamento

Questa sezione racconta un errore vero, perché insegna qualcosa di generale.

Per non chiedere `N` a PROJ milioni di volte, la pipeline lo calcola su una
griglia rada e poi interpola. L'errore di interpolazione ha una soglia: 10 mm.

Con i dati veri di TINITALY e la griglia EGM2008, l'errore misurato era
**11,06 mm**: sopra la soglia. L'intuizione dice: "infittisci la griglia". Ed è
sbagliata.

**Misurato**, sulla griglia EGM96 (nodi ogni 0,25°):

| Passo della nostra griglia | 0,25 / passo | Errore |
|---|---|---|
| 0,0104167 (= 1/96) | 24,0000 — **intero** | **0,0036 mm** |
| 0,0104 | 24,0385 | 7,94 mm |
| 0,0110 | 22,7273 | 17,35 mm |

**Diciassette micrometri di differenza nel passo cambiano l'errore di duemila
volte.** Non conta quanto è fitta la griglia: conta se i suoi nodi **cadono sui
nodi** della griglia di PROJ.

Il perché: PROJ interpola linearmente dentro la propria griglia, quindi il campo
`N` ha una "piega" su ogni linea di nodi. Se la nostra griglia campiona proprio
sulle pieghe, la ricostruzione è esatta; se campiona in mezzo, la piega viene
tagliata e compare l'errore.

**Regola:** il passo si sceglie come **sottomultiplo intero** del passo nativo
della griglia geoidica:

| Griglia | Passo nativo | Passo di default della pipeline |
|---|---|---|
| EGM2008 | 2,5′ = 1/24° | 1/96° |
| EGM96 | 15′ = 1/4° | 1/16° |

Il vecchio default (0,01°) funzionava con EGM96 **per caso**, e ha smesso di
funzionare appena si è passati a EGM2008. Il test `tests/test_geoid.py` ora
verifica l'allineamento esplicitamente.

---

## 3.6 I sei stadi

La pipeline è divisa in sei stadi, eseguiti in ordine:

| # | Stadio | Cosa fa |
|---|---|---|
| 1 | `inventory` | trova i file sorgente e li mette dietro un **mosaico virtuale** (VRT) |
| 2 | `geoid` | verifica la griglia geoidica e calcola `N` sull'area |
| 3 | `warp` | riproietta in gradi WGS84 al livello più fine e somma `N` |
| 4 | `pyramid` | costruisce i livelli più grossolani, uno dall'altro |
| 5 | `tiles` | taglia ogni livello in tile da 129 × 129 |
| 6 | `manifest` | scrive gli indici e il manifest |

### Il VRT: un mosaico che non costa niente

TINITALY è diviso in decine di file. Invece di incollarli in un file unico da
parecchi gigabyte, GDAL permette di creare un **VRT** (*Virtual Raster*): un file
XML di pochi kilobyte che dice "questi N file, visti come uno solo". Si crea in
un istante, non occupa spazio, e si ispeziona con `gdalinfo`.

> ⚠️ **Trappola trovata nella Fase 6.** Se i file hanno **proiezioni diverse**,
> `gdalbuildvrt` tiene la prima e **scarta le altre con un semplice warning**, poi
> prosegue. Il risultato è un mosaico che copre metà del territorio. Ora la
> pipeline **conta** i file effettivamente usati dal VRT e si ferma se mancano.
> Capitolo 7 per i dettagli.

---

## 3.7 La piramide: dimezzare senza spostare

I livelli più grossolani si costruiscono dimezzando quello più fine. La domanda
è: **come si dimezza una griglia di quote?**

### La scelta ovvia è sbagliata

La scelta ovvia è la media di ogni blocco 2 × 2. Ma il valore medio di quattro
post appartiene al **centro** del quadrato che formano, cioè a metà strada fra i
post. Usarlo come valore del post del livello superiore **sposta il terreno di
mezzo passo**. E l'errore si accumula: dopo dieci livelli, il terreno è
spostato di parecchi passi.

### La scelta giusta: un filtro centrato sul post che resta

La pipeline usa un filtro con pesi `[1, 4, 6, 4, 1] / 16`, **centrato sul post
che sopravvive** al dimezzamento:

```
post:    ... p₋₂   p₋₁   p₀   p₁   p₂ ...
pesi:         1     4     6    4    1        (diviso 16)
                          ↑
               questo post resta, con valore
               (p₋₂ + 4p₋₁ + 6p₀ + 4p₁ + p₂) / 16
```

Il post resta dove era, e il filtro rimuove le variazioni troppo fini per il
livello più grossolano — che altrimenti produrrebbero *aliasing*, cioè creste
seghettate. È la classica piramide gaussiana di **Burt e Adelson** (1983).

Il filtro si applica prima in orizzontale e poi in verticale (è *separabile*), il
che lo rende molto più veloce.

### I buchi nei dati

I dati sorgente hanno buchi: il mare, i bordi della copertura. Il filtro li
gestisce tenendo **separati numeratore e denominatore**: ogni post senza dato
pesa zero, e alla fine si divide la somma pesata per la somma dei pesi. Se
entrambi i passaggi (orizzontale e verticale) tengono i due totali separati, il
filtro resta esatto anche vicino ai buchi.

I post rimasti senza dato alla fine si riempiono con la quota ellissoidica del
livello del mare in quel punto — cioè `N`, la stessa griglia della sezione 3.4.

File: `Pipeline/geoworld/raster.py`, funzione `reduce_level()` e costante
`REDUCE_KERNEL`.

---

## 3.8 Riavviabile e idempotente

Una pipeline su dati nazionali può durare ore. Se si interrompe a metà —
corrente, errore, Ctrl-C — **rilanciarla deve riprendere da dove era**, non
ricominciare.

### Le impronte degli stadi

Ogni stadio ha un'**impronta**: un hash calcolato su input e parametri. A stadio
concluso, impronta e lista dei file prodotti finiscono in `_work/state.json`. Al
rilancio, uno stadio si salta **solo se**:

* l'impronta coincide (stessi input, stessi parametri), **e**
* tutti i suoi file di output esistono ancora, con la dimensione registrata.

La seconda condizione non è pedanteria: cancellare un intermedio per fare spazio
è una cosa che si fa, e la pipeline deve accorgersene.

Nell'impronta degli input entrano **percorso, dimensione e data di modifica**, non
il contenuto: calcolare l'hash di decine di gigabyte a ogni lancio costerebbe
più del lavoro che si vuole evitare.

File: `Pipeline/geoworld/state.py`.

### Le scritture atomiche

Un file interrotto a metà da un Ctrl-C è **indistinguibile** da uno completo: al
rilancio verrebbe saltato perché "esiste già". Per questo ogni file si scrive
prima come `.tmp` e poi si rinomina:

```python
with open(path + ".tmp", "wb") as handle:
    handle.write(blob)
os.replace(path + ".tmp", path)     # atomico: o c'è tutto, o non c'è niente
```

`os.replace` è un'operazione atomica del sistema operativo: non esiste un istante
in cui il file è a metà.

---

## 3.9 L'errore del livello 24

Un altro errore vero, che insegna a diffidare delle unità.

La pipeline sceglie il livello più fine confrontando la risoluzione del sorgente
con quella dei livelli. La prima versione leggeva la dimensione del pixel dal
file così com'è:

```
pixel = 0,000277   → "0,000277 metri, cioè 0,28 mm!"   → livello 24
```

Ma il Copernicus DEM è in **gradi**, non in metri. 0,000277 gradi sono circa
**30 metri**. La pipeline aveva chiesto il livello 24, cioè un raster da
**517,8 terabyte**. GDAL si è fermato con un errore su un file troppo grande.

Il bug era mascherato: tutti i test passavano `--max-level` esplicitamente, e il
calcolo automatico non era mai stato provato.

**Correzione:** `source_ground_resolution()` misura la **distanza vera sul
terreno** fra un pixel e il vicino, convertendo entrambi in coordinate
geografiche. Funziona con qualunque sistema di riferimento, senza dover sapere in
che unità sia il file. E ora:

* la pipeline stampa una **tabella dei livelli prima di cominciare**, con il
  numero di tile e la dimensione stimata — così mezzo terabyte si vede prima,
  non dopo tre ore;
* `MAX_SUPPORTED_LEVEL = 20` rifiuta i livelli assurdi;
* `tests/test_resolution.py` **non passa mai** `--max-level`, apposta.

---

## 3.10 Procurarsi i dati

### Copernicus DEM, automatico

```bat
python run.py fetch --area test -o dati/copernicus
```

Scarica le tile del Copernicus DEM GLO-30 (30 m, tutto il mondo, libero) da un
bucket pubblico su AWS. Le tile sul mare non esistono, e il server risponde 404:
è normale, e il comando lo sa. File: `Pipeline/geoworld/fetch.py`.

### TINITALY, DTED, qualunque altra cosa

La pipeline **non guarda mai il formato del file**: tutto passa da `gdal.Open`.
Per questo gli stessi comandi funzionano con formati diversissimi:

```bat
python run.py build -i "tinitaly/*.tif"   -o dataset/quote
python run.py build -i "Elevation/*.dt2"  -o dataset/quote-dted
```

L'unico caso in cui serve aiutarla: i file **ESRI ASCII** (`.asc`) di TINITALY
non contengono il sistema di riferimento. Lo si dichiara con
`--source-crs EPSG:32632`.

### La diagnosi dell'ambiente

```bat
python run.py check-env
```

Dice se GDAL, PROJ, numpy e Pillow ci sono, quali formati GDAL sa leggere, e se
le griglie geoidiche sono installate. **Lanciala per prima**, sempre.

---

## Dove sta nel codice

| File | Cosa contiene |
|---|---|
| `Pipeline/run.py` | il punto d'ingresso: `python run.py <comando>` |
| `geoworld/cli.py` | i comandi e l'orchestrazione dei sei stadi |
| `geoworld/tiling.py` | lo schema di tiling |
| `geoworld/tileformat.py` | il formato `.ght` |
| `geoworld/manifest.py` | indice e manifest |
| `geoworld/geoid.py` | la conversione delle quote, e il perché dei due passi |
| `geoworld/raster.py` | VRT, riproiezione, risoluzione vera, piramide |
| `geoworld/tilecut.py` | il taglio in tile, in parallelo |
| `geoworld/state.py` | impronte e riavvio |
| `geoworld/fetch.py` | download del Copernicus DEM |
| `geoworld/environment.py` | `check-env` |
| `tests/` | i test, su un sorgente sintetico |
| `docs/fase2-design.md`, `docs/dati.md` | i documenti originali |

### Comandi

```bat
python run.py check-env                         diagnosi dell'ambiente
python run.py fetch --area test -o <cartella>   scarica il Copernicus DEM
python run.py build -i "<file>" -o <dataset>    la pipeline completa
python run.py verify -o <dataset>               controlla un dataset vero
python run.py inspect <tile.ght>                stampa il contenuto di una tile
python -m unittest discover -s tests            i test
```

---

## Riepilogo

* La pipeline fa **offline, una volta**, tutto il lavoro geografico: il motore
  legge un formato semplice e non dipende da GDAL.
* **Tiling geografico WGS84**: livello 0 = 2 × 1, ogni livello divide per
  quattro, X verso est, Y verso sud.
* **129 × 129 post** registrati sui nodi: il bordo è condiviso, le tile vicine
  combaciano esattamente. File binario da 66.596 byte, sempre.
* L'**indice** porta min e max di ogni tile: permette di decidere la visibilità
  senza leggere il file.
* Le quote diventano **ellissoidiche** sommando l'ondulazione del geoide, in un
  passo esplicito: il "ballpark" di PROJ lo salterebbe **in silenzio**.
* La griglia di `N` va **allineata** a quella del geoide: conta l'allineamento,
  non la finezza.
* La piramide usa un filtro **centrato sul post che resta**, non la media 2 × 2
  che sposterebbe il terreno.
* **Riavviabile**: impronte per stadio, scritture atomiche.
* La risoluzione si misura **in metri sul terreno**, mai nelle unità del file.

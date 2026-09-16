# Da dove vengono i dati

Verificato il 2026-09-16. Tutto quello che segue e' stato provato davvero dove
indicato; dove non ho potuto verificare, e' scritto esplicitamente.

---

## 1. Quote del terreno (DEM) — la Fase 2

### Copernicus DEM GLO-30 — automatico, funziona oggi

```bat
cd Pipeline
python run.py fetch --area italia -o dati/copernicus
```

| | |
|---|---|
| Risoluzione | 30 m (1 arcosecondo) |
| Copertura | globale |
| CRS | EPSG:4326, quote riferite a **EGM2008** |
| Formato | GeoTIFF COG, 3600x3600, float32 |
| Peso | ~19 MB per tile di 1 grado |
| Accesso | bucket S3 pubblico, **nessuna credenziale** |
| Licenza | libera con attribuzione (ESA / Copernicus) |

Provato da qui: un tile su Roma scaricato in **1.7 s**, pipeline completa in
**6 s**, 748 tile prodotte su 5 livelli. Il comando salta le tile gia' scaricate
e quelle di mare aperto (che sul bucket non esistono: danno 404).

Aree predefinite: `test` (una tile su Roma), `roma`, `alpi`, `sicilia`,
`italia`. Oppure `--bbox OVEST SUD EST NORD`.

Prima di lanciare su tutta l'Italia conviene:

```bat
python run.py fetch --area italia --dry-run -o dati/copernicus
```

che elenca le tile e quanto pesano senza scaricare niente.

**Nota sul livello massimo.** Il Copernicus e' in EPSG:4326, quindi il suo
pixel e' in **gradi** (1/3600 = 0.000277), non in metri. La pipeline misura la
risoluzione vera sul terreno (~23 m in longitudine, ~31 m in latitudine alle
latitudini italiane) e sceglie il **livello 13**. Se vedi `livello nativo
consigliato: 20`, qualcosa non va nel riconoscimento del CRS: fermati e
segnalalo, non forzare il livello a mano.

**Nota sul mare.** Il Copernicus DEM non usa un valore di nodata: il mare vale
`0.000` (ortometrico), che la pipeline converte correttamente in ~48 m
ellissoidici, cioe' il livello medio del mare. Conseguenza pratica: **non ci
sono tile scartate**, si genera anche il mare. Per un motore planetario e' cio'
che si vuole, ma fa crescere il conteggio delle tile rispetto a TINITALY, che
invece ha un nodata vero.

### TINITALY 1.1 — tre volte piu' dettagliato, ma manuale

| | |
|---|---|
| Risoluzione | **10 m** |
| Copertura | solo Italia |
| CRS | UTM 32N WGS84 (EPSG:32632) |
| Struttura | 193 tile di ~50 km di lato |
| Licenza | **CC BY 4.0** — uso libero, anche parziale, **con citazione** |
| Accesso | pagina di download, un tile alla volta |

Pagina: [Download area 1.1 — TINITALY, INGV](https://tinitaly.pi.ingv.it/Download_Area1_1.html)

**Non e' automatizzabile in modo affidabile** e non ho scritto codice per
provarci: i tile si scaricano cliccandoli sulla pagina, e non esiste un endpoint
pubblico documentato per prenderli in blocco. Da questa macchina il sito e'
bloccato dal proxy, quindi non ho potuto verificare di persona ne' la procedura
ne' i nomi dei file.

**Due avvertenze sul formato**, perche' le fonti che ho trovato non concordano:

* una fonte indica **ESRI ASCII Raster** (`.asc`), un'altra **GeoTIFF**. La
  pipeline legge entrambi — `-i "tinitaly/*.asc"` funziona come `*.tif` — ma se
  sono ASCII conviene convertirli prima: sono file di testo, occupano circa
  dieci volte tanto e si leggono molto piu' lentamente. La pipeline se ne
  accorge e te lo dice nello stadio 1.

  ```bat
  gdal_translate -of GTiff -co COMPRESS=DEFLATE -co PREDICTOR=3 in.asc out.tif
  ```

* i grid ESRI ASCII **non contengono il sistema di riferimento**: sta in un file
  `.prj` a fianco, che a volte manca. In quel caso la pipeline si ferma con un
  messaggio chiaro e si passa il CRS a mano:

  ```bat
  python run.py build -i "tinitaly/*.asc" -o dataset/italia --source-crs EPSG:32632
  ```

INGV pubblica anche servizi **WMS / WMTS / WCS** per TINITALY. Un WCS in teoria
permetterebbe di scaricare per riquadro in modo automatico; non l'ho potuto
provare perche' il dominio e' bloccato da qui. Se ti interessa quella strada,
dimmelo e la guardiamo insieme quando hai il sito raggiungibile.

### Quale usare

Comincia con **Copernicus**: e' automatico e ti fa arrivare alla Fase 5 con
terreno vero sotto gli occhi. Quando hai scaricato TINITALY, rilanci la stessa
pipeline sui nuovi file — **non cambia una riga di codice**, cambia solo il
livello massimo, che passa da 13 a 14.

---

## 2. Ortofoto — la Fase 6

Non e' ancora implementato niente. Quello che segue e' ricognizione, con la
raggiungibilita' provata da qui.

| Fonte | Risoluzione | Automatizzabile | Provato da qui |
|---|---|---|---|
| **Sentinel-2 L2A** (bucket `sentinel-cogs`, AWS Open Data) | 10 m | si', COG su S3 pubblico | bucket **raggiungibile** |
| Ortofoto nazionali, Geoportale Nazionale | ~20-50 cm | via WMS/WMTS | dominio **bloccato** dal proxy |
| TINITALY WMTS (rilievo ombreggiato, non fotografico) | 10 m | si' | dominio **bloccato** |

**Sentinel-2 e' il candidato naturale** per iniziare: e' libero, globale, e i
suoi 10 m corrispondono bene alla risoluzione di TINITALY. Ha due complicazioni
che andranno affrontate nella Fase 6 e che vale la pena sapere in anticipo:

1. **le nuvole.** Una singola acquisizione e' coperta di nuvole a caso. Serve un
   mosaico composito su piu' date, scegliendo per ogni pixel l'acquisizione piu'
   limpida. E' il lavoro vero della Fase 6, piu' del drappeggio in se';
2. **la scoperta dei dati.** L'API STAC che si usa normalmente per trovare le
   scene (`earth-search.aws.element84.com`) da qui e' bloccata. Sul tuo Windows
   probabilmente funziona.

Le ortofoto nazionali sono molto piu' dettagliate (decimetri contro 10 metri) ma
passano da servizi WMS, cioe' immagini renderizzate su richiesta: piu' lente da
scaricare in massa e con condizioni d'uso da verificare.

**La struttura non cambia.** Le tile di colore useranno lo stesso identico
schema di tiling, lo stesso quadtree, lo stesso loader. Cambia il formato del
file — JPEG o PNG invece di float32 grezzo, perche' le immagini si comprimono
bene e le quote no — e il fatto che quota e colore per lo stesso nodo possono
stare a livelli diversi.

---

## 3. Verificare quello che hai generato

I test automatici girano su un sorgente sintetico. Sul dataset **vero** si usa:

```bat
python run.py verify -o dataset/italia
```

Controlla che manifest e indici concordino, che le tile campionate esistano e
abbiano header coerente, e che le **giunzioni** fra tile adiacenti siano
identiche bit per bit — la proprieta' da cui dipende l'assenza di crepe nella
Fase 5. Campiona invece di leggere tutto, altrimenti su centinaia di migliaia di
tile non lo lancerebbe nessuno.

---

## Fonti

* [Download area 1.1 — TINITALY, INGV](https://tinitaly.pi.ingv.it/Download_Area1_1.html)
* [TINITALY DEM — pagina principale INGV](https://tinitaly.pi.ingv.it/)
* [TINITALY 1.1, note di accompagnamento (PDF)](https://tinitaly.pi.ingv.it/Tinitaly_1_1_AccompanyingNotes.pdf)
* [TINITALY — servizio WMS](https://tinitaly.pi.ingv.it/wms_service.html)
* [TINITALY, record DataCite (DOI 10.13127/tinitaly/1.1)](https://oai.datacite.org/oai?verb=GetRecord&metadataPrefix=oai_datacite&identifier=doi%3A10.13127%2Ftinitaly%2F1.1)

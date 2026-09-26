# Capitolo 7 — Vestire il terreno (Fase 6)

> Il terreno grigio diventa la Terra: si drappeggiano sopra delle immagini vere.

---

## L'idea in breve

Un'**ortofoto** è una fotografia aerea o satellitare **raddrizzata**: corretta
per la prospettiva e per il rilievo, in modo che ogni pixel stia alla sua
coordinata geografica, come in una mappa.

Il lavoro della fase ha tre parti:

| Dove | Cosa |
|---|---|
| Pipeline Python | una seconda piramide di tile, fatta di immagini |
| Runtime C++ | un secondo streaming, una texture per tile, il calcolo del ritaglio |
| Materiale | lo shader che prende la texture e la mette sul terreno |

---

## 7.1 La decisione centrale: due piramidi, uno schema solo

**Quote e immagini stanno in due dataset separati, ma con lo stesso schema di
tiling.**

**Perché separati.** Le due sorgenti non hanno niente in comune se non il
territorio:

* **risoluzioni diverse**, e in modo imprevedibile: oggi 10 m per entrambe,
  domani ortofoto a 20 cm su quote a 30 m;
* **aggiornamenti diversi**: un DEM dura anni, un'ortofoto invecchia in fretta;
* **contenuti diversi**: `float32` da una parte, pixel compressi dall'altra.

Legarle costringerebbe per sempre la più fine a scendere al livello della più
grossolana, e aggiornarne una significherebbe rigenerare entrambe.

**Perché lo stesso schema.** Perché allora la tile di terreno `(L, X, Y)` e la
tile di immagine `(L, X, Y)` coprono **esattamente lo stesso rettangolo**. Nel
caso normale il drappeggio non richiede nessun calcolo: la texture copre la tile,
e le UV `[0, 1]` che la mesh ha già vanno bene così come sono.

---

## 7.2 Le immagini non si comportano come le quote

Questa è la differenza che, sbagliata, produce artefatti che nessuno sa più
spiegare.

**Le quote sono registrate sui nodi** (capitolo 3.3): 129 post, e il post 128 di
una tile **è** il post 0 della vicina.

**Le immagini sono registrate sulle aree.** Un pixel non è un punto: è un
quadratino di territorio. Una tile ha **256 × 256 pixel** che coprono il
rettangolo per intero, **senza sovrapposizione** con le vicine.

```
QUOTE (nodi)                    IMMAGINI (aree)
●───●───●   ●───●───●           ┌─┬─┬─┐┌─┬─┬─┐
        └───┘                   │ │ │ ││ │ │ │
   stesso punto                 └─┴─┴─┘└─┴─┴─┘
                                  nessun pixel in comune
```

Duplicare una colonna di pixel sul bordo, per analogia con le quote, sarebbe un
errore: quella striscia verrebbe disegnata due volte, una per tile, e alla minima
differenza di campionamento sfarfallerebbe.

### Perché proprio 256

1. **È una potenza di due**: serve per i *mipmap* (le versioni ridotte di una
   texture che la scheda video usa da lontano) e perché la riduzione da un
   livello all'altro sia una media 2 × 2 esatta.
2. **Dà due pixel per ogni cella di terreno**: la mesh ha 128 celle per lato, la
   texture 256 pixel. Abbastanza da non vedere sfocature, non tanto da sprecare
   memoria su dettaglio che lo schermo non mostra.
3. **Fa coincidere le due piramidi**: poiché 256 = 2 × 128, **l'immagine di
   livello L ha la stessa risoluzione al suolo del terreno di livello L+1**.

| Livello | Pixel immagine | Passo terreno |
|---:|---:|---:|
| 12 | 19,1 m | 38,2 m |
| 13 | **9,5 m** | 19,1 m |
| 14 | 4,8 m | **9,5 m** |
| 18 | 0,30 m | 0,60 m |

Da leggere così: **Sentinel-2** (10 m) è nativo al livello **13**; TINITALY
(10 m) al livello 14; un'ortofoto a 30 cm arriva al 18.

### La riduzione: qui la media 2 × 2 è giusta

Nel capitolo 3.7 la media 2 × 2 era sbagliata, perché i post sono punti e la media
cade in mezzo. **Per i pixel è giusta**: un pixel del livello superiore **è**
l'unione esatta dei quattro pixel figli, e la media dei quattro è il suo valore
corretto. Stessa operazione, conclusione opposta, perché la registrazione è
opposta.

> 💡 **Dettaglio.** La media si calcola in interi a 16 bit, non a 8: quattro
> pixel a 255 sommati fanno 1.020, che in 8 bit andrebbe in overflow silenzioso.
> C'è un test apposta.

---

## 7.3 Il formato del file

```
<root>/<livello>/<X>/<Y>.gim     header di 32 byte + JPEG
<root>/<livello>/index.bin       per ogni tile: X, Y, copertura
<root>/manifest.json             datasetKind: "imagery"
```

### Perché JPEG

256 × 256 pixel RGB non compressi sono 196 KB per tile. In JPEG la stessa tile
sta in **15–25 KB**: un fattore dieci.

### Perché non BC1

BC1 (o DXT1) è la compressione che le schede video usano **direttamente**: 32 KB
per tile, **nessuna decompressione**, e resta compressa in memoria video, dove
occupa otto volte meno. Sarebbe tecnicamente migliore.

Non è stato scelto per una ragione pratica: comprimere in BC1 richiede un
encoder che né GDAL né Pillow forniscono, cioè una dipendenza in più. Il vincolo
iniziale era "Python + GDAL". **Il formato è però già predisposto**: l'header ha
un byte che dichiara il tipo di contenuto, e aggiungere BC1 significa scrivere un
encoder e un ramo nel lettore, non cambiare formato.

### La copertura

Ogni tile porta un numero da 0 a 100: **la percentuale del rettangolo
effettivamente coperta** dall'ortofoto. Le ortofoto, a differenza dei DEM, hanno
bordi frastagliati: finisce il volo, finisce la regione, c'è il mare. Le tile a
copertura zero non vengono scritte; quelle parziali si riempiono di grigio.

### Due magic diversi

Il file di immagine inizia con `GWIM`, quello di quota con `GWHT`. L'indice di
immagine con `GWIA`, quello di quota con `GWIX`. Aprire un file di immagine
credendolo di quota **deve fallire subito** con un messaggio chiaro, non leggere
numeri a caso e produrre un dataset che sembra funzionare. I test lo verificano
in entrambe le direzioni.

---

## 7.4 La pipeline delle ortofoto

Tre passi, più corti di quelli delle quote perché un'immagine non ha un datum
verticale da convertire:

1. **mosaico virtuale** dei sorgenti, riproiettati in gradi WGS84;
2. **taglio del livello più fine**, con una riproiezione GDAL per ogni tile;
3. **riduzione verso l'alto**, dai figli, senza mai rileggere la sorgente.

### Una riproiezione per tile, non un raster gigante

La pipeline delle quote costruisce un raster intero per livello e poi lo taglia.
Per le ortofoto conviene il contrario: un raster unico al livello 16 sull'Italia
sarebbe di centinaia di gigabyte, e ne servirebbe il doppio perché sul disco ci
starebbero sia lui sia le tile. Una riproiezione per tile lavora a memoria
costante ed è banalmente riavviabile: la tile esiste oppure no.

### Indifferenza alla sorgente

Non c'è un solo punto in cui si assuma il formato del file d'ingresso: tutto
passa da `gdal.Open`. Funziona con GeoTIFF, JPEG2000, ECW (se il driver c'è),
perfino con URL remoti (`/vsicurl/...`).

> ⚠️ **Le ECW.** Il driver ECW di GDAL richiede l'SDK proprietario ERDAS, che
> **non** è incluso nei pacchetti di conda-forge. `check-env` stampa la tabella
> dei formati disponibili e, se ECW manca, lo dice. La strada è convertire le ECW
> in GeoTIFF una volta sola (con QGIS, o con il visualizzatore fornito col
> database) e dare i GeoTIFF alla pipeline.

### La trappola delle proiezioni diverse

Le scene Sentinel-2 sono organizzate per **quadrato MGRS** (sezione 7.5), e i
quadrati MGRS attraversano le zone UTM per costruzione. Bastano quattro scene
sull'Italia centrale per averne due in UTM 32N e due in 33N.

`gdalbuildvrt`, davanti a proiezioni diverse, tiene la prima e **scarta le altre
con un semplice warning**, poi prosegue:

```
Warning 1: gdalbuildvrt does not support heterogeneous projection ... Skipping
```

Il risultato sarebbe stato un dataset che copre metà del territorio.

**Soluzione:** `build_reprojected_vrt()` riproietta ogni sorgente in gradi WGS84
come VRT (un file XML, nessun pixel copiato), poi mosaica, poi **conta** che
nessun file sia rimasto fuori.

> ⚠️ **E il conteggio stesso ha avuto un bug.** Un VRT ripete il nome di ogni file
> sorgente **una volta per banda**. Su un'immagine RGB sono tre volte: due file
> davano sei occorrenze, e la pipeline si fermava dicendo che ne erano stati
> scartati −4. Sui DEM, a banda singola, il problema era invisibile. Ora si
> contano i file **distinti**, leggendo il VRT come XML.

### Il nero fra le scene

Quattro scene non tassellano un rettangolo: fra l'una e l'altra restano zone
senza dato, che Sentinel-2 marca con lo zero. Senza dichiararlo, finirebbero
nelle tile come pezzi di terreno neri. Il default di `--src-nodata` è quindi 0;
per ortofoto in cui il nero è un colore vero, `--src-nodata none`.

---

## 7.5 Scaricare Sentinel-2

```bat
python run.py fetch-imagery --area roma -o dati/sentinel
```

### Perché dal bucket e non dall'API

La via consueta per trovare le scene Sentinel-2 è un'API di ricerca (STAC) di
Element 84. Qui si legge invece **direttamente il bucket pubblico S3**
(`sentinel-cogs`). Il bucket è l'unica cosa da cui i dati devono comunque
passare; l'API è un servizio in più, che può cambiare o essere bloccato da una
rete aziendale — ed era bloccato, nell'ambiente in cui il codice è stato scritto.
Una dipendenza invece di due.

### Il costo: calcolare il quadrato MGRS

Sul bucket le scene stanno in cartelle così:

```
sentinel-s2-l2a-cogs/33/T/TG/2024/6/S2A_33TTG_20240603_0_L2A/TCI.tif
                     ^^ ^ ^^
                   zona banda quadrato
```

Per sapere quali scene coprono un'area bisogna calcolare quei tre pezzi dalle
coordinate. È il modulo `mgrs.py`: la proiezione UTM (serie di Krüger) e le
regole delle lettere MGRS, in 160 righe.

**Verificato contro il mondo reale**: il test chiede a sei città (Roma, Milano,
Torino, Napoli, Palermo, Cagliari) il loro quadrato, e i valori attesi **non sono
calcolati dal codice stesso** — sono i prefissi sotto cui il bucket tiene davvero
le scene di quelle città.

### Cosa si scarica

Per ogni quadrato, si leggono i metadati di tutte le scene del periodo richiesto
(estate di default, meno nuvole) e si sceglie **la meno nuvolosa**. Una scena per
quadrato, non tutte: due scene dello stesso quadrato coprono lo stesso territorio
in giorni diversi, e mosaicarle darebbe metà immagine con le ombre di giugno e
metà con quelle di agosto.

Si scarica il file `TCI.tif`: le tre bande visibili già combinate, 10 m, circa
230 MB per scena.

> 💡 **Per provare senza scaricare.** `--stream` non scarica niente: restituisce
> indirizzi `/vsicurl/...` che GDAL legge dalla rete, prendendo solo i pezzi che
> servono. I file sono *Cloud Optimized GeoTIFF*, fatti apposta per questo.

---

## 7.6 Quando l'immagine giusta non c'è

Nel caso normale — stesso livello, tile presente — non c'è niente da calcolare.
Ma ci sono due casi in cui l'immagine al livello del terreno non è disponibile:

* **la piramide delle immagini è più bassa**: succede sempre, il terreno arriva
  al 14 e Sentinel-2 si ferma al 13;
* **la tile non è ancora arrivata**: lo streaming è asincrono.

**Soluzione: si usa l'antenato.** Si risale la piramide fino alla prima immagine
disponibile e se ne **ritaglia il pezzo giusto**. È lo stesso principio del
capitolo 5.6: meglio grossolano subito che perfetto fra tre frame.

### La matematica

Terreno al livello `Lt`, posizione `(X, Y)`. Immagine disponibile al livello
`La`. Detta `d = Lt − La`:

```
antenato  = (La, X >> d, Y >> d)
scala     = 1 / 2^d
offsetU   = (X & (2^d − 1)) × scala
offsetV   = (Y & (2^d − 1)) × scala

UV_texture = offset + UV_mesh × scala
```

(`>>` è lo spostamento di bit, cioè la divisione per 2^d; `&` tiene il resto.)

> 💡 **Esempio.** Terreno al livello 12, tile `(4333, 1014)`; immagine solo al
> livello 10, quindi `d = 2`:
> ```
> antenato = (10, 1083, 253)      scala = 0,25
> offsetU  = (4333 & 3) × 0,25 = 1 × 0,25 = 0,25
> offsetV  = (1014 & 3) × 0,25 = 2 × 0,25 = 0,50
> ```
> La tile di terreno è un sedicesimo dell'immagine: seconda colonna da ovest,
> terza riga da nord.

Con `d = 0` la formula dà scala 1 e offset 0, cioè il caso normale: **non è un
ramo separato, è la stessa formula che degenera.**

### Perché in aritmetica intera

Gli offset sono sempre multipli esatti di 1/2^d, cioè numeri che un `float`
rappresenta **senza errore**. Calcolarli dai rettangoli geografici in gradi
darebbe valori "quasi" giusti — e "quasi", al bordo di una texture, significa
mezza riga di pixel presi dalla tile sbagliata.

I test verificano che il ritaglio calcolato con gli interi coincida con quello
misurato sulla geografia vera, entro 10⁻⁹. È il controllo che lega la formula al
mondo.

### Chiedere sempre quella giusta

Mentre si usa un antenato grossolano, si **chiede comunque** la tile giusta allo
streaming. È ciò che fa migliorare il drappeggio man mano che arrivano le tile,
invece di lasciarlo sfocato per sempre.

File: `ImageryMapping.h` (C++) e `imagecut.py` (Python), con gli stessi esempi
nei test di entrambi.

---

## 7.7 La texture, e le due trappole

Una tile decodificata diventa una texture:

```cpp
UTexture2D* Texture = UTexture2D::CreateTransient(256, 256, PF_B8G8R8A8);
Texture->SRGB = true;
Texture->AddressX = TextureAddress::TA_Clamp;
Texture->AddressY = TextureAddress::TA_Clamp;
// ...si copiano i pixel nel mip 0, poi:
Texture->UpdateResource();
```

*(in `GeoRender/Private/Imagery/GeoImagerySubsystem.cpp`)*

**Nota Unreal.** `CreateTransient` crea una texture che non esiste su disco e non
finisce nei pacchetti. Deve avvenire **sul game thread**, e ha un budget di 4 per
frame, come le mesh.

**Trappola 1: l'indirizzamento.** Deve essere `TA_Clamp`, non `TA_Wrap`. Con Wrap,
il filtro che ammorbidisce i pixel, sul bordo destro, andrebbe a prendere i pixel
del bordo **sinistro della stessa texture**: sul confine fra due tile
comparirebbe una riga di colori presi dall'altra parte del rettangolo.

**Trappola 2: sRGB.** Un JPEG contiene colori in spazio sRGB. Con `SRGB = false` il
terreno verrebbe slavato, e la tentazione sarebbe correggerlo nel materiale invece
che qui, che è il posto giusto.

### La decodifica sta sul worker

Decomprimere un JPEG 256 × 256 costa uno o due millisecondi. Sembrano pochi
finché non si moltiplicano per le decine di tile che arrivano quando ci si muove.
La decodifica avviene quindi **sul thread di caricamento**, con `IImageWrapper`
(il decoder JPEG di Unreal), e il game thread riceve pixel già pronti.

**Nota Unreal.** `IImageWrapper` si può usare da qualunque thread, purché ogni
thread abbia la **propria istanza**. Ma il modulo va **caricato sul game thread**:
è per questo che `Initialize()` lo carica esplicitamente, invece di lasciarlo al
primo worker che passa.

I pixel si convertono direttamente in **BGRA**, l'ordine che `PF_B8G8R8A8` si
aspetta: la conversione avviene una volta, sul worker, e non a ogni riga durante
la copia. Dopo la decodifica i byte compressi si buttano, per non occupare due
volte la cache.

### Una texture per immagine, non per tile di terreno

Quattro tile di terreno adiacenti possono usare lo **stesso antenato**. Le texture
sono quindi indicizzate per chiave dell'**immagine**, non del terreno: crearne una
per tile di terreno sprecherebbe memoria video in proporzione al numero di figli.
Quelle non più usate si buttano a ogni frame.

---

## 7.8 Il materiale: il pezzo che non si poteva scrivere

Il terreno ha bisogno di uno shader che campioni la texture della propria tile.
In Unreal uno shader è un **asset binario** (capitolo 1.15), e chi ha scritto
questo codice non aveva il motore.

### Il materiale necessario

```
TextureCoordinate ──► Multiply ──► Add ──► TextureSampleParameter2D("BaseColor")
                         ▲          ▲                    │
VectorParameter ─────────┴──────────┘                    ▼
("UvOffsetScale")     .BA        .RG                Base Color
```

Due parametri, con nomi **esatti** perché il codice li cerca per nome:

* **`BaseColor`**: la texture della tile;
* **`UvOffsetScale`**: il ritaglio della sezione 7.6, come
  `(offsetU, offsetV, scala, scala)`.

### Una revisione fatta scrivendo il codice

Il design iniziale diceva di scrivere le UV ritagliate **direttamente nei vertici
della mesh**, per tenere il materiale banale. Era sbagliato: quando arriva
un'immagine più fine il ritaglio cambia, e bisognerebbe riscrivere 17.157
coordinate per ogni tile che si affina — proprio mentre ci si sta muovendo.

Con un parametro del materiale, aggiornare il ritaglio costa **quattro numeri**, e
la mesh non viene toccata mai. Il prezzo sono tre nodi in più nello shader.

### Lit, non Unlit

*Unlit* mostrerebbe i colori esatti dell'ortofoto senza dipendere dalle luci della
scena, il che è comodo. Ma toglierebbe ogni ombreggiatura, e il rilievo appena
costruito sparirebbe visivamente. Il materiale è **Lit**: serve una luce
direzionale nella scena.

### Due strade per crearlo

1. **`geo.Imagery.CreateMaterial`** — un comando nel modulo `GeoWorldEditor` che
   costruisce il materiale da codice con `UMaterialEditingLibrary` e lo salva in
   `/GeoWorld/Materials/M_GeoTerrain`.
2. **A mano**, in un minuto: le istruzioni sono in `docs/fase6-verifica.md`. È la
   rete di sicurezza: il punto 1 usa API di editor che non si sono potute
   compilare.

Se il materiale manca, il terreno resta grigio e il log lo dice all'avvio, con le
istruzioni.

**Nota Unreal: un materiale, tante istanze.** Per ogni tile si crea una
`UMaterialInstanceDynamic` dal materiale base, e le si assegnano la propria
texture e il proprio ritaglio. L'istanza ha come *outer* il **componente** della
tile: quando il componente viene distrutto, l'istanza lo segue, senza doverla
liberare a mano.

---

## 7.9 La scacchiera: vedere le UV

```
geo.Imagery.Checker 1
```

Sostituisce le ortofoto con una **scacchiera** generata sul momento, con il
**bordo rosso** su ogni tile. Su una foto di un bosco un disallineamento di mezzo
pixel non lo nota nessuno; su una scacchiera salta all'occhio.

Cosa deve succedere:

* i bordi rossi di due tile vicine **dello stesso livello** formano una riga sola;
* dove una tile usa un antenato, i quadri sono **più grandi** (è un ritaglio
  ingrandito), e il bordo rosso compare **solo sui lati che coincidono con il
  bordo dell'antenato**: le quattro tile figlie, insieme, ricompongono un solo
  riquadro rosso grande.

Se vedi la scacchiera ripetuta più volte in una tile, il ritaglio è sbagliato. Se
vedi una riga di colori presi dall'altro lato, è `TA_Wrap` al posto di `TA_Clamp`.

La scacchiera usa il filtro `TF_Nearest` invece di quello bilineare: bordi netti,
che è il punto.

---

## 7.10 Il refactoring che la fase ha richiesto

Circa metà del lavoro della fase non è stata funzionalità nuova, ma
**ristrutturazione**:

| Pezzo | Prima | Dopo |
|---|---|---|
| Cache | solo tile di quota | **template** `TTileCache<Payload>` |
| Pool di thread | dentro lo streaming delle quote | **`FGeoLoaderPool`**, classe a sé |
| Dataset | quote | quote + **gemello** `FGeoImageryDataset` |
| Impacchettamento delle chiavi | copiato in sei file | **`FTileKey::Pack()`**, uno solo |

**Perché non uno streaming unico e generico.** I subsystem di Unreal sono
`UObject`, e gli `UObject` **non possono essere template**. Si potrebbe aggirare
con un'interfaccia virtuale, pagando una chiamata indiretta nel percorso caldo per
unificare due casi che sono e resteranno due. La regola seguita:

* **template** dove il codice è puro (la cache);
* **classe estratta** dove è infrastruttura (il pool);
* **duplicazione consapevole** dove la logica diverge davvero (dataset e
  subsystem).

**Stato attuale:** come detto nel capitolo 4.7, `FGeoLoaderPool` lo usa per ora
solo lo streaming delle ortofoto.

---

## 7.11 Cosa questa fase non fa

Elencato apposta, per non scoprirlo dopo:

* **niente mipmap**: a viste radenti il terreno sfarfalla. Il LOD tiene il
  rapporto texel/pixel vicino a 1:1, quindi il problema è contenuto, ma esiste;
* **niente trasparenza** sulle tile parziali: si vede il grigio di riempimento;
* **niente BC1**: più memoria video e una decodifica per tile;
* **niente dissolvenza** fra livelli: il passaggio è uno stacco netto;
* **una texture e un'istanza di materiale per tile**: un draw call per tile, lo
  stesso limite del provider del capitolo 6;
* **`geo.Imagery.ShowLevels`**, previsto nel documento di design (colorare le tile
  per livello di immagine), **non è stato implementato**. L'overlay mostra
  comunque l'intervallo di livelli in uso e quante tile usano un antenato.

---

## Dove sta nel codice

| File | Cosa contiene |
|---|---|
| `Pipeline/geoworld/imageformat.py` | il formato `.gim`, la riduzione 2 × 2 |
| `Pipeline/geoworld/imagecut.py` | taglio, riduzione, e la matematica del ritaglio |
| `Pipeline/geoworld/imagerymanifest.py` | indice e manifest delle immagini |
| `Pipeline/geoworld/imagerybuild.py` | l'orchestrazione dei tre passi |
| `Pipeline/geoworld/fetchimagery.py` | il download di Sentinel-2 |
| `Pipeline/geoworld/mgrs.py` | UTM e quadrati MGRS |
| `Pipeline/geoworld/raster.py` | `build_reprojected_vrt`, `vrt_source_files` |
| `GeoTiles/Public/Tiles/ImageTileFormat.h` | la lettura del formato `.gim` (C++) |
| `GeoTiles/Public/Streaming/GeoImageryDataset.h` + `.cpp` | il dataset di immagini |
| `GeoTiles/Public/Streaming/GeoImageryStreamingSubsystem.h` + `.cpp` | streaming e decodifica JPEG |
| `GeoTiles/Public/Streaming/GeoLoaderPool.h` | il pool estratto |
| `GeoRender/Public/Imagery/ImageryMapping.h` | il ritaglio (C++ puro) |
| `GeoRender/Public/Imagery/GeoImagerySubsystem.h` + `.cpp` | texture, drappeggio, scacchiera |
| `GeoWorldEditor/Private/GeoTerrainMaterialFactory.cpp` | `geo.Imagery.CreateMaterial` |
| `Tools/StandaloneTests/geoimagery_main.cpp` | 44 test |
| `docs/fase6-design.md`, `docs/fase6-verifica.md` | i documenti originali |

### Comandi

```bat
:: pipeline
python run.py fetch-imagery --area roma -o dati/sentinel [--stream]
python run.py build-imagery -i "dati/sentinel/*_TCI.tif" -o dataset/ortofoto
python run.py verify-imagery -o dataset/ortofoto
python run.py inspect-imagery dataset/ortofoto/13/.../....gim
```

```
geo.Imagery.Demo <quote> <ortofoto>   tutto insieme, a 6 km, wireframe spento
geo.Imagery.Open <cartella>           solo il dataset di immagini
geo.Imagery.Enable <0|1>              drappeggio
geo.Imagery.Checker <0|1>             la scacchiera
geo.Imagery.Debug <0|1>               overlay
geo.Imagery.Budget <N>                texture create per frame
geo.Imagery.Stats                     statistiche
geo.Imagery.CreateMaterial            crea M_GeoTerrain (solo editor)
```

---

## Riepilogo

* **Due piramidi, uno schema**: separate perché diverse in tutto, stesso tiling
  perché così la tile `(L, X, Y)` è lo stesso rettangolo in entrambe.
* **Pixel sulle aree, post sui nodi**: 256 × 256 senza sovrapposizione. Qui la
  media 2 × 2 è giusta.
* 256 = 2 × 128: l'immagine di livello L vale il terreno di livello L+1;
  **Sentinel-2 è nativo al 13**.
* **JPEG** oggi, formato pronto per **BC1** domani.
* Sentinel-2 dal **bucket S3**, con il quadrato **MGRS** calcolato e verificato
  contro il bucket vero.
* **Proiezioni diverse** fra scene sono il caso normale: si riproietta e si
  **contano i file distinti** usati dal mosaico.
* Senza l'immagine giusta si usa l'**antenato**, con un ritaglio in **aritmetica
  intera**, e si chiede comunque quella giusta.
* Texture con **`TA_Clamp`** e **sRGB**; decodifica **sul worker**.
* Il ritaglio sta in un **parametro del materiale**, non nei vertici.
* Il materiale si crea con un **comando**, o **a mano** in cinque nodi.

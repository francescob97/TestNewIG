# Fase 8 — Strade, ferrovie e piste: decisioni e motivazioni

> Prima fase del "mondo sopra il terreno". Le strade arrivano da OpenStreetMap,
> passano per la pipeline come **tile vettoriali**, e il runtime le **disegna**
> sopra l'ortofoto, tile per tile, alla risoluzione che serve in quel momento.
> Questo documento spiega perché così e non in uno dei modi più ovvi.
> Come provarla: `docs/fase8-verifica.md`. Il capitolo del manuale è il 12.

---

## 0. Il programma dalla Fase 8 in poi

La tua proposta era: 8 OpenStreetMap, 9 case e monumenti, 10 foreste,
11 acqua, 12 cielo e nuvole, 13 illuminazione. L'ho riordinata così:

| Fase | Cosa | Perché in questo punto |
|---|---|---|
| **8** | **Strade, ferrovie, piste** (questa) | Costruisce la catena dei **dati vettoriali** (OSM → tile → runtime) che le tre fasi successive riusano |
| **9** | **Acqua**: mare, laghi, fiumi | Usa la stessa catena, con i poligoni al posto delle linee. Da quota si vede ovunque: l'Italia ha 8.000 km di costa, e oggi il mare è terreno a quota zero con la foto sopra |
| **10** | **Edifici e monumenti** | Impronte e altezze da OSM, estruse; i monumenti come modelli veri (glTF, o i `.flt` del tuo database). È la fase più pesante per il rendering: decine di milioni di edifici in Italia |
| **11** | **Vegetazione** | Boschi da OSM e dalla copertura arborea Copernicus; alberi istanziati, con impostor da lontano. Stesso problema di numeri degli edifici, risolto una volta sola |
| **12** | **Atmosfera, cielo, nuvole e luce** | In Unreal sono **un sistema solo**: lo stesso sole illumina cielo, nuvole e terreno, e la foschia dipende dall'atmosfera. Separarli voleva dire fare due volte metà del lavoro |

Due note sull'ordine.

- **Acqua prima degli edifici**: arriva dalla stessa pipeline appena fatta
  (poligoni invece di linee), e si vede da qualunque quota; gli edifici si vedono
  solo da vicino.
- **L'atmosfera è indipendente da tutto il resto.** Un "minimo" (sole all'ora
  giusta, atmosfera centrata sulla Terra vera anche dopo il rebasing, foschia
  in lontananza) costa poco e cambia molto l'aspetto: se vuoi, si può anticipare
  fra una fase e l'altra.

La Fase 7 (entità e protocolli: CIGI, DIS, HLA) resta dov'è, progettata e non
implementata.

---

## 1. L'idea in breve

```
 Geofabrik             pipeline Python                      runtime Unreal
 ---------             ---------------                      --------------
 italy.osm.pbf  --->  [estrai]  way -> classe, larghezza   tile vettoriale (.gvt)
                      [taglia]  livelli 10..13, ritaglio       |
                      [indici]  index.bin, manifest.json        v
                                                           disegno su CPU (worker)
                                                           per OGNI tile di terreno
                                                               |
                                                               v
                                                     texture trasparente sopra
                                                     l'ortofoto, nel materiale
```

Tre pezzi nuovi nel codice:

- **pipeline**: `fetch-osm`, `build-roads`, `verify-roads`, `inspect-roads`;
- **strato puro**: il formato `.gvt` (`VectorTileFormat.h`), le classi
  (`VectorClasses.h`), il rasterizzatore (`Roads/RoadRasterizer.h`);
- **strato Unreal**: lo streaming delle tile di linee
  (`UGeoVectorStreamingSubsystem`), il subsystem delle strade
  (`UGeoRoadsSubsystem`), un secondo strato di texture nel provider e nel
  materiale, i comandi `geo.Roads.*`.

---

## 2. Da dove vengono i dati

**OpenStreetMap, dagli estratti di Geofabrik.** È l'unica fonte che ha tutte
le strade d'Italia, aggiornata, con la classe (`highway=primary`), le corsie,
i ponti, le gallerie, lo sterrato; e le ferrovie e le piste degli aeroporti
nello stesso file. Licenza ODbL: libera, con l'obbligo di citarla (il comando
scrive `ATTRIBUZIONE.txt`).

Geofabrik ha l'Italia intera (~2 GB) e cinque macro-regioni; per provare
conviene `nord-ovest`, che contiene Torino.

**Si legge con GDAL**, che ha un driver OSM per il `.pbf` e per l'XML `.osm`.
Nessuna libreria nuova da installare, e lo stesso principio del resto della
pipeline: tutto passa da GDAL. I tag che GDAL mette in colonne proprie cambiano
fra una versione e l'altra; si leggono sia le colonne sia la colonna
`other_tags`, e si fondono (`osmextract.py`).

**Il tuo database ha una cartella `SourceData` di shapefile.** Se dentro ci sono
strade, la pipeline potrà leggerle con poco lavoro (GDAL legge gli shapefile
come legge OSM): serve solo sapere come sono classificate. Mandami l'elenco dei
campi di un file (`ogrinfo -so file.shp layer`) e lo aggiungo.

---

## 3. La classificazione

Ogni way diventa una **classe** (17 in tutto: 12 di strade, 3 di ferrovie,
2 di aeroporto), una **larghezza in metri**, dei **flag** (ponte, galleria,
sterrato, rampa; dalla classificazione 2 anche i marciapiedi, per le strade
3D: `docs/strade-3d.md`) e un **layer**. Tutto in `roadclasses.py`, in un punto solo.

| Domanda | Risposta |
|---|---|
| Quanto è larga? | Il tag `width` se c'è; altrimenti `lanes` × 3,25 m (più la banchina); altrimenti un valore tipico della classe |
| L'autostrada è larga 25 m? | No: in OSM i due sensi di marcia sono **due** way. Una carreggiata è ~11 m |
| Gallerie? | **Scartate**: dal cielo non si vedono. Anche `building_passage` e `covered` |
| Piazze pedonali? | Sono aree, non linee: arriveranno con i poligoni (Fase 9) |
| Strade in progetto, cantieri, ferrovie smantellate? | Scartate |
| Sterrato? | Dal tag `surface`; senza, i `track` e i `path` sono sterrati, il resto è asfaltato |

**Il numero della classe è un contratto** fra Python (che lo scrive) e C++
(che lo usa per lo stile). Un test Python legge l'header C++ e confronta ogni
numero: cambiarne uno da una parte sola fa fallire i test, invece di colorare
in silenzio le ferrovie come autostrade.

---

## 4. Tile vettoriali, non immagini già disegnate

L'alternativa ovvia era disegnare le strade nella pipeline e salvarle come
immagini, accanto alle ortofoto. È stata scartata per numeri.

| | Immagini già disegnate | Tile vettoriali |
|---|---|---|
| Risoluzione per volare a 300 m | livello 17–18 (pixel di mezzo metro) | quella che serve, calcolata al momento |
| File per l'Italia | **decine di milioni** (solo le tile che toccano una strada) | ~100.000 (livelli 10–13) |
| Cambiare colore o larghezza | rifare tutto | un comando a runtime |
| Riuso per edifici, acqua, boschi | no | sì: è lo stesso formato |

Il prezzo è che il disegno si fa a runtime, su CPU: qualche millisecondo per
tile su un thread di lavoro (sezione 6).

### 4.1 I livelli

Le tile vettoriali esistono solo ai livelli **10, 11, 12, 13** (di default).
Ogni classe ha un **livello minimo**:

| Livello | Lato della tile (a 45°) | Cosa contiene |
|---:|---|---|
| 10 | ~20 km | autostrade, superstrade, ferrovie, piste |
| 11 | ~10 km | + statali e provinciali (primary, secondary), |
| 12 | ~5 km | + strade locali e residenziali, tram |
| 13 | ~2,4 km | + tutto: vicoli, sterrati, sentieri, binari di scalo |

Una tile di terreno usa la tile vettoriale **più profonda che non superi il
suo livello**: una di livello 9 non ha strade (sarebbero larghe un centesimo di
pixel), una di livello 12 usa la tile vettoriale del 12, una di livello 15 usa
quella del 13 e ne disegna **un sedicesimo**. Avvicinandosi, le strade minori
compaiono: è il LOD del terreno applicato alle linee.

### 4.2 Il formato `.gvt`

Header di 32 byte (`"GWVT"`, livello, x, y, extent, buffer, conteggi), un
record di 12 byte per linea (classe, flag, layer, larghezza in decimetri,
numero di punti), poi i punti in **int16 locali alla tile**.

- **Extent 16384**: una tile del 13 ha un passo di ~15 cm, sotto la precisione
  di OSM. Potenza di due: il pezzo di tile che serve a una tile di terreno più
  profonda cade su numeri interi.
- **Buffer di un ottavo**: le linee escono dalla tile fino a 2048 unità. Una
  strada larga 20 m che corre lungo il bordo deve comparire anche nella tile
  accanto, altrimenti metà della sua larghezza sparirebbe sul confine.
- **Nord in alto** (y = 0 sul bordo nord), come le righe di quote e immagini.
- **Ordine di disegno già deciso**: la pipeline ordina le linee per layer,
  ponte, importanza. Il runtime le disegna nell'ordine del file.

Nessuna compressione: un file `.gvt` è già piccolo, e così il lettore C++ non
ha bisogno di zlib e si prova nei test standalone.

### 4.3 La pipeline

Tre stadi, riavviabili come quelli delle quote e delle foto:

1. **estrai** — si legge l'estratto OSM **una volta**, si classifica ogni way e
   la si scrive nei **blocchi** che tocca (un blocco = una tile del livello 10).
   È lo stadio lento: il driver OSM ricompone milioni di way dai loro nodi.
2. **taglia** — ogni blocco produce tutte le sue tile, a tutti i livelli. I
   blocchi sono indipendenti: vanno **in parallelo** (`--jobs`) e un blocco
   finito non si rifà dopo un'interruzione. Ritaglio e semplificazione con GEOS
   (la libreria geometrica dentro GDAL), ma solo per le linee che escono dalla
   tile: la maggioranza ci sta dentro e si quantizza e basta.
3. **indici** — `index.bin` per livello e `manifest.json`.

Senza i blocchi ogni tile dovrebbe chiedere al file OSM "cosa passa di qui?",
e il driver OSM non ha un indice spaziale: ogni domanda rileggerebbe il file.

Misurato qui su un estratto sintetico di 60.000 way (420.000 nodi): **9
secondi** in tutto con 4 processi, 1.643 tile, verifica pulita. L'Italia ha
qualche milione di way da disegnare: la stima è **decine di minuti**, quasi
tutti nello stadio 1. Il numero vero lo vedrai tu: da qui Geofabrik non è
raggiungibile.

---

## 5. Il disegno: `RoadRasterizer.h`

Prende una tile vettoriale, il pezzo che serve e una dimensione in pixel, e
produce un'immagine BGRA trasparente. È C++ puro, header-only: gira nei test
standalone, dove la larghezza di una strada si **misura**.

### 5.1 In metri, non in pixel

Un pixel di una tile non è quadrato sul terreno: a 45° di latitudine un grado
di longitudine è lungo il 71% di uno di latitudine. A Torino, a 512 pixel per
una tile del 13, un pixel è **3,4 m × 4,8 m**. Una strada da 7 m deve essere
larga 7 m in ogni direzione: distanze e larghezze si misurano in metri.

### 5.2 L'antialias, e il primo errore

Il bordo è sfumato con un filtro "a scatola": la copertura di un pixel è la
frazione della sua larghezza che cade dentro la strada. Una strada più sottile
di un pixel diventa una linea tenue, come in una foto, invece che seghettata.

La prima versione usava per il filtro la dimensione **media** del pixel. I test
l'hanno presa subito: una strada orizzontale da 10 m ne misurava **9,5**, e una
larga un quarto di pixel copriva **0,11** pixel invece di 0,25 (a tratti
spariva). Il filtro deve essere largo quanto il passo dei pixel **lungo la
sezione della strada**: per una strada orizzontale è l'altezza del pixel, per
una verticale la larghezza, in diagonale una combinazione delle due. Corretto,
la sezione misura 9,99 m, 10,00 m e 9,97 m in orizzontale, in verticale e a
45°, e la linea sottile 0,251 pixel.

### 5.3 Giunzioni e composizione

- Dove due segmenti della stessa strada si incontrano il pixel è coperto **una
  volta**: per ogni linea si prende il **massimo** delle coperture dei suoi
  segmenti, non la somma. Con la somma ogni curva avrebbe un puntino più scuro.
- Le linee si compongono una sull'altra nell'ordine del file ("sopra").

### 5.4 Alfa premoltiplicato

Il colore è salvato **già moltiplicato per la copertura**. Sembra un dettaglio,
ma le mipmap e il filtro bilineare fanno medie fra pixel vicini: in alfa
normale, la media fra un pixel di strada e uno trasparente (colore nero, alfa
zero) scurisce i bordi a ogni livello di mip, e da lontano ogni strada avrebbe
un alone nero. In premoltiplicato la media è giusta per costruzione, e il
materiale compone con una moltiplicazione e una somma:

```
colore = foto * (1 - alfa * forza) + strade * forza
```

### 5.5 Quanto costa

Misurato nei test, su una tile "di città" di 3.000 linee: **2,6 ms** a 256
pixel, **6,1 ms** a 512, **26 ms** a 1024. Su un thread di lavoro, una volta per
tile: il game thread riceve pixel già pronti, come per le foto.

### 5.6 Gli stili

- **realistico** (default): grigio asfalto, massicciata, cemento delle piste,
  marrone dello sterrato; opacità non piena, perché un po' della foto sotto
  resti visibile e la strada non sembri un adesivo;
- **mappa**: colori da carta stradale e larghezza minima di un pixel e mezzo.
  Serve a **controllare l'allineamento**: da 5 km si vede subito se le strade
  disegnate stanno sopra quelle della foto.

---

## 6. Il runtime: `UGeoRoadsSubsystem`

È il gemello del subsystem delle ortofoto, con una differenza: le foto si
**caricano**, le strade si **disegnano**, una volta per ogni tile di terreno.

### 6.1 Per ogni tile di terreno costruita, a ogni frame

1. **Niente da disegnare?** (livello sotto il 10, o nessuna linea in quel pezzo
   di mondo secondo l'indice) → nessuna texture, tile pronta.
2. **Il suo disegno è finito?** → diventa texture (un numero limitato per
   frame), si mette sulla tile.
3. **Non c'è ancora** → si chiede la tile di linee al disco; quando c'è, si
   lancia il disegno su un worker (al massimo 4 insieme).
4. **Nel frattempo** → le strade di un **antenato**, ritagliate: più sfocate,
   ma subito.

### 6.2 Le stesse regole che hanno tolto i flash alle ortofoto

- **Una tile compare solo con le sue strade** (o con quelle di un antenato).
  Il terreno ora accetta **più vestitori**: le ortofoto e le strade registrano
  ciascuna un predicato con un nome, e una tile è pronta quando lo dicono
  tutti. Con un predicato solo, il secondo si sarebbe mangiato il primo.
- **Prima ciò che si vede**: le tile a schermo, poi quelle che il quadtree
  vorrebbe mostrare e sta aspettando, poi quelle tenute pronte dal piano di
  residenza.
- **Un antenato senza strade non è un ripiego**: le strade minori compaiono ai
  livelli più fini, e mostrarlo vorrebbe dire farle spuntare un attimo dopo.

### 6.3 Risoluzione e memoria video

Una texture di strade è **per tile di terreno**: segue il LOD da sola, e il
materiale la usa con le UV della mesh, senza ritagli.

| Profilo | Pixel per tile | Al livello più profondo del terreno | Budget video |
|---|---:|---:|---:|
| portatile (≤ 16 GB di RAM) | 256 | 512 | 384 MB |
| workstation | 512 | 1024 | 1.536 MB |

Una texture con le mipmap pesa 0,33 MB a 256 pixel, 1,33 MB a 512, 5,3 MB a
1024. Le tile al livello più profondo del terreno hanno più pixel perché sono
quelle vicine quando si vola bassi, e il terreno lì non si affina oltre.

Il **budget** limita solo il lavoro in anticipo: le tile a schermo e quelle che
il quadtree sta aspettando hanno sempre le loro strade, anche oltre il budget.
Le tile nascoste oltre il budget restano senza, e si ridisegnano se servono.

### 6.4 Cambiare stile o risoluzione

Si ridisegna tutto, ma le strade vecchie **restano addosso** finché non
arrivano le nuove: nessun buco, nessun lampo. Ogni disegno porta una
"generazione"; quelli di una generazione vecchia, se arrivano tardi, si buttano.

---

## 7. Il materiale

`M_GeoTerrain` guadagna un secondo campionatore e tre parametri:

| Parametro | Cosa |
|---|---|
| `Overlay` | la texture delle strade (sRGB, premoltiplicata) |
| `OverlayUv` | il ritaglio, come `DrapeUv`: identità, o il pezzo di un antenato |
| `OverlayStrength` | 0 di default: una tile senza strade mostra solo la foto |

**Va rifatto**: `geo.Imagery.CreateMaterial` lo ricostruisce sul posto. Un
materiale vecchio non ha `Overlay`; il provider se ne accorge all'avvio e
l'overlay delle strade lo dice in rosso, invece di non disegnare niente in
silenzio. In quel caso le strade non bloccano il terreno: aspettarle non
servirebbe.

---

## 8. Cosa NON fa, e cosa viene dopo

- **Da lontano le strade sono dipinte SUL terreno**, e un ponte è una striscia
  sul fondovalle. Da vicino, dopo la prima prova, sono diventate **3D**:
  `docs/strade-3d.md`.
- **Niente segnaletica orizzontale** (strisce, mezzeria): a 1 m per pixel non si
  vedrebbe. Arriverà con le strade 3D o con un dettaglio procedurale nel
  materiale.
- **Il dettaglio da vicino è limitato dal terreno.** La texture delle strade è
  per tile di terreno, e il terreno non va oltre il livello più profondo del
  DEM (13 o 14): da vicino una tile copre 1–2 km, e a 1024 pixel le strade sono
  a 1–2 m per pixel. Volando a 300 m restano un po' morbide. È lo stesso limite
  che hanno le ortofoto ECW più fini del DEM (oggi il drappeggio le ignora oltre
  il livello del terreno): la soluzione comune è **suddividere il terreno oltre
  il DEM** (tile "virtuali" interpolate), ed è il prossimo lavoro tecnico che
  propongo, indipendente dalle fasi.
- **Gallerie e sottopassi** si scartano: dal cielo non si vedono.

---

## 9. Dove sta nel codice

| File | Cosa |
|---|---|
| `Pipeline/geoworld/roadclasses.py` | classi, larghezze, flag: da tag OSM a linea |
| `Pipeline/geoworld/osmextract.py` | lettura OSM con GDAL, tag da colonne e `other_tags` |
| `Pipeline/geoworld/vectorformat.py` | formato `.gvt`, indice, manifest, fixture per i test C++ |
| `Pipeline/geoworld/vectorbuild.py` | i tre stadi, la verifica |
| `Pipeline/geoworld/fetchosm.py` | download da Geofabrik, ripresa, md5 |
| `Pipeline/tests/test_roads.py` | 30 test, con un estratto OSM sintetico |
| `GeoTiles/Public/Tiles/VectorClasses.h` | le classi (contratto con la pipeline) |
| `GeoTiles/Public/Tiles/VectorTileFormat.h` | lettura di `.gvt` e indici |
| `GeoTiles/.../Streaming/GeoVectorDataset.*` | il dataset su disco |
| `GeoTiles/.../Streaming/GeoVectorStreamingSubsystem.*` | caricamento asincrono e cache |
| `GeoRender/Public/Roads/RoadRasterizer.h` | il disegno: stili, finestre, antialias |
| `GeoRender/.../Roads/GeoRoadsSubsystem.*` | il runtime: ordine, ripiego, budget, texture |
| `GeoRender/.../Imagery/GeoRuntimeTexture.*` | creazione delle texture, ora condivisa con le foto |
| `GeoRender/.../Terrain/*Provider*` | `SetTileOverlay`, parametri `Overlay*` |
| `GeoRender/.../Terrain/GeoTerrainSubsystem.h` | vestitori multipli (`SetDressPredicate` con nome) |
| `GeoWorldEditor/Private/GeoTerrainMaterialFactory.cpp` | il materiale con le strade |
| `Tools/StandaloneTests/georoads_main.cpp` | 42 test: formato, larghezze misurate, ritagli |

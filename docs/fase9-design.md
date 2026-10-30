# Fase 9 — L'acqua: mare, laghi, fiumi

> Richiesta: "l'acqua, bella, realistica. Reagisce pure al vento (che non avevo
> incluso, ci sarà tutta una sezione di weather)". E, a margine, le stampe di
> latitudine, longitudine e quota sulla destra dello schermo (sezione 10).
>
> Come provarla: `docs/fase9-verifica.md`. Il racconto per chi impara:
> `docs/manuale/13-acqua.md`.

---

## 1. L'idea in breve

```
OSM (.osm.pbf)  ──┐  laghi, fiumi, canali, lagune
poligoni del mare ─┤  (osmdata.openstreetmap.de)
terreno (.ght)  ──┤  quota dei laghi, profilo dei fiumi
geoide (PROJ)   ──┘  il livello del mare in quota ellissoidica
        │
        ▼  python run.py build-water            (una volta, offline)
tile d'acqua .gwa: una per tile di terreno, allineata ai suoi post
   ├─ superficie  129×129  quota dell'acqua su ogni post
   ├─ fondale     129×129  quanto scavare il terreno sotto l'acqua
   └─ raster      256×256  distanza dalla riva (SDF), onde, corrente
        │
        ▼  a runtime, per ogni tile di terreno
   1. il terreno si costruisce SCAVATO sotto l'acqua (fondale)
   2. una mesh d'acqua alla quota della superficie, tagliata sulla riva
   3. materiale Single Layer Water: profondità, colore, riflessi,
      onde di Gerstner dal vento, schiuma sulla riva e sulle creste,
      corrente nei fiumi
```

Tre idee portano tutto il resto.

**L'acqua ha bisogno di profondità.** Oggi il mare è terreno a quota zero con
la foto sopra, e i laghi del DEM sono piani alla quota del lago. Una superficie
d'acqua posata lì sopra coinciderebbe con il terreno (z-fighting) e non avrebbe
niente sotto: niente trasparenza, niente colore che cambia con la profondità.
Quindi **il terreno si scava** sotto l'acqua: la pipeline calcola un fondale
(dalla batimetria vera se c'è, altrimenti una rampa dalla riva), e il runtime
abbassa i post del terreno fino a lì prima di costruirne la mesh.

**La riva si disegna con una distanza, non con un bordo.** Il raster della tile
non dice "acqua sì/no" ma **quanto si è lontani dalla riva** (*signed distance
field*: positivo in acqua, negativo a terra). Filtrata dalla scheda video, una
distanza dà una riva nitida a qualunque ingrandimento, la schiuma che segue la
costa per tanti metri quanti si vuole, e le onde che si spengono in secca.

**Il vento è un dato del mondo, non dell'acqua.** Nasce un `UGeoWeatherSubsystem`
minimo (vento: velocità, direzione, raffiche) che la futura fase del meteo
allargherà; l'acqua lo legge e ne ricava le onde con uno spettro fisico.

---

## 2. I dati

| Cosa | Da dove | Note |
|---|---|---|
| Laghi, bacini, lagune, fiumi larghi (aree) | OSM, layer `multipolygons` del `.osm.pbf` già scaricato per le strade | `natural=water`, `water=*`, `landuse=reservoir`, `waterway=riverbank` |
| Fiumi e canali (linee) | OSM, layer `lines` | `waterway=river|canal`; i torrenti (`stream`) solo se `width` ≥ 4 m |
| Il mare | **poligoni dell'acqua** di osmdata.openstreetmap.de (`water-polygons-split-4326.zip`, ~850 MB, il mondo) | In un estratto regionale di OSM la costa è fatta di **linee** aperte; chiuderle in poligoni è il problema classico, già risolto lì ogni giorno |
| Quota dei laghi e dei fiumi | il **dataset del terreno** (`--terrain`) | Coerente per costruzione con il terreno che si vede |
| Livello del mare | il **geoide** (PROJ, la stessa griglia della pipeline del terreno) | Le nostre quote sono ellissoidiche: in Italia il mare sta ~40–50 m sopra l'ellissoide |
| Batimetria (facoltativa) | EMODnet (~115 m, Europa) o GEBCO (~450 m, mondo), GeoTIFF | `--bathymetry`; senza, il fondale è una rampa dalla riva |

**Si scartano**: `intermittent=yes` e `seasonal=yes` (le fiumare asciutte del
Sud sono la maggioranza dei "fiumi" in OSM), `tunnel=*`, `covered=yes`,
`location=underground`, le piscine, i bacini industriali (`water=wastewater`).

---

## 3. I corpi d'acqua e le loro regole

| Tipo | Superficie | Fondale (senza batimetria) | Onde | Corrente |
|---|---|---|---|---|
| **Mare** | geoide | 0,5 m + 5% della distanza dalla riva, fino a 60 m | 1 (piene) | no |
| **Laguna** (`water=lagoon`) | geoide | 0,3 m + 5%, fino a 2 m | 0,35 | no |
| **Lago**, bacino | 20° percentile del terreno dentro il poligono | 0,5 m + 12%, fino a 5 + 6·√km² m (max 120) | da 0,12 a 0,8 secondo la superficie (*fetch*) | no |
| **Fiume** (linea, area) | profilo lungo l'asse, sempre in discesa nel verso della linea; le **aree** fluviali prendono la quota dagli assi che le attraversano | 0,3 m + 25%, fino a larghezza/15 (1–5 m) | 0,06 | 1 m/s nel verso della linea |
| **Canale** | come il fiume | 0,3 m + 50%, fino a 2,5 m | 0,03 | 0,35 m/s |

Tre note.

- **La quota di un lago è un percentile, non una media.** Il DEM sopra un lago è
  di solito piano alla quota dell'acqua, ma ai bordi prende la riva e a volte ha
  buchi. Il 20° percentile dei post dentro il poligono è robusto a entrambi.
- **Il profilo di un fiume scende sempre.** Le linee dei fiumi in OSM sono
  orientate a valle (è una regola del tag `waterway`). Si campiona il terreno
  lungo l'asse ogni 20 m e si prende il **minimo progressivo**: dove il DEM
  risale (un ponte, un argine, rumore) l'acqua non risale con lui. Prima del
  minimo il terreno si **pulisce** (sezione 12): solo campioni con dati veri,
  mediana su 180 m, via i crolli più bassi di 15 m della mediana del km. Le
  way dello stesso fiume si **cuciono**: una way comincia al massimo dalla quota
  a cui finiscono quelle che entrano nel suo primo punto (mai più di 5 m sotto
  il suo terreno). Una way che sale di molto nel suo verso si gira.
- **Onde e corrente vanno nel raster**, pixel per pixel: un fiume che entra in
  un lago passa da "scorre, niente onde" a "fermo, onde da lago" esattamente
  sulla riva del lago.

---

## 4. Il formato: tile `.gwa`

Una tile d'acqua per ogni tile di terreno che ha acqua, **allineata ai suoi
post**: lo scavo è esatto post per post. Le tile senza acqua non esistono
(l'indice lo dice, e il runtime non aspetta niente).

```
<root>/manifest.json
<root>/geoid.bin                 il geoide sull'area (per "s.l.m." nelle stampe)
<root>/<level>/index.bin         chi c'è, con i flag
<root>/<level>/<x>/<y>.gwa
```

**Header (32 byte, little-endian)**

| off | campo | |
|---|---|---|
| 0 | magic `GWWA` | |
| 4 | version uint16 = 1 | |
| 6 | flags uint16 | bit0 mare, bit1 lago, bit2 fiume/canale, bit3 c'è la corrente, bit4 tutta acqua |
| 8 | level, x, y uint32 | |
| 20 | posts uint16 = 129, raster uint16 = 256 | |
| 24 | payload uint32 | byte compressi (zlib) |
| 28 | riservato | |

**Payload (zlib)**, piano per piano (un piano costante si comprime in pochi byte):

| Piano | Tipo | Significato |
|---|---|---|
| superficie | int16 × 129² | quota ellissoidica in **decimetri** (−32768 = niente acqua qui) |
| fondale | int16 × 129² | quota a cui scavare il terreno, in decimetri (−32768 = non scavare) |
| SDF | uint8 × 256² | 128 + 8 × distanza dalla riva **in pixel** (positivo in acqua), saturata a ±16 px |
| onde | uint8 × 256² | esposizione alle onde 0..255 |
| corrente u, v | int8 × 256² × 2 | solo se il flag bit3: direzione × velocità, verso est e verso sud |

**Perché i decimetri senza offset per tile.** Con lo stesso zero per tutte le
tile, due tile vicine scrivono lo stesso numero sul post che condividono: niente
fessure fra le superfici. ±3.276 m bastano a tutto tranne ai fondali oltre i
3,2 km (il Tirreno), che si saturano: da sopra non si vede.

**Perché la distanza in pixel e non in metri.** Così il formato non dipende dal
livello. Il runtime conosce i metri per pixel della tile e li converte.

---

## 5. La pipeline: `build-water`

```bat
python run.py fetch-water -o dati\acqua                       :: i poligoni del mare (~850 MB, una volta)
python run.py build-water --osm dati\osm\nord-ovest-latest.osm.pbf ^
    --sea dati\acqua\water-polygons-split-4326.zip ^
    --terrain dataset\terreno -o dataset\acqua [--bbox ...] [--bathymetry emodnet.tif]
python run.py verify-water -o dataset\acqua
```

Tre stadi, riavviabili come quelli delle strade:

1. **Estrazione.** Si leggono i poligoni e le linee, si classificano, si calcola
   la quota di ogni lago e il profilo di ogni fiume (leggendo il terreno), si
   bufferizzano i fiumi alla loro larghezza. Il risultato va in **blocchi** (le
   tile del livello 9, con un margine), più una versione **semplificata** dei
   corpi grandi per i livelli bassi.
2. **Taglio.** Per ogni tile di terreno che tocca dell'acqua, a tutti i livelli
   del terreno: rasterizzazione a 512² con 32 pixel di margine, distanza dalla
   riva con `gdal.ComputeProximity` (dentro e fuori), riduzione a 256²;
   superficie e fondale campionati sui post. In parallelo (`--jobs`).
3. **Indici e manifest**, e il geoide sull'area.

Il **margine** di 16 pixel serve alla distanza: un pixel sul bordo della tile
deve sapere che a 10 pixel, nella tile accanto, c'è la riva. Senza, la schiuma
si interromperebbe sul confine.

---

## 6. Il runtime

### 6.1 Lo scavo del terreno

Prima di costruire la mesh di una tile, il terreno chiede all'acqua: "c'è
qualcosa da scavare qui?". Tre risposte:

- **no** (la tile non ha acqua, o l'acqua è spenta): si costruisce come prima;
- **non ancora** (la tile d'acqua si sta caricando): si aspetta, come per le
  quote;
- **sì**: ogni post diventa `min(terreno, fondale)`, poi la mesh.

Il "fondale" del file è anche un **tetto** per il terreno appena fuori
dall'acqua (la sponda, sezione 12): lì sta sopra la superficie, e il `min`
abbassa solo un terreno più alto.

Lo scavo vale anche per le quote che usano le **strade 3D** (stesso terreno
sotto, stessa origine della mesh) e per il **padre** nel geomorphing.

Se il dataset dell'acqua si apre o si chiude, il terreno si ricostruisce.

### 6.2 La mesh dell'acqua

Una griglia alla quota della superficie, con lo stesso passo del terreno,
**solo nelle celle vicine all'acqua**, più una gonna di 2 m sui bordi della
tile (contro le fessure fra livelli diversi, come per il terreno). La riva
vera la disegna il materiale con la distanza dal raster: la cella può
sporgere, il pixel no.

Dove il file **non porta la superficie** (un'acqua troppo stretta per la mesh
a quel livello, sezione 12) il vertice si **appoggia** sul terreno scavato,
0,3 m sopra: stessi vertici e stessa diagonale del terreno, quindi l'acqua sta
sopra il terreno di 0,3 m in ogni punto della cella.

È un quarto componente della tile nel provider (terreno, strade 3D, acqua):
stessa trasformazione, stessa visibilità, stesso rebase.

### 6.3 Il materiale: `M_GeoWater`

**Single Layer Water** è il modello d'ombreggiatura di Unreal fatto per
l'acqua (lo usa il plugin Water): la luce attraversa l'acqua e viene
**assorbita e diffusa** in funzione della profondità vera, letta dal depth
buffer. Il colore turchese in secca e blu scuro al largo non si dipinge: viene
da lì, dato il terreno scavato sotto.

| Ingresso | Da dove |
|---|---|
| Riva (opacity mask) | SDF del raster: la soglia 0,5 è esattamente la riva |
| Onde (World Position Offset) | 6 onde di Gerstner (sezione 7), × esposizione, × spegnimento in secca e con la distanza |
| Normale | le stesse onde, analitiche, + due campionamenti scorrevoli di una normale di dettaglio |
| Corrente | nei fiumi la normale di dettaglio scorre lungo la corrente (*flow map*, due fasi sfalsate) |
| Schiuma | sulla riva (dalla distanza) e sulle creste quando il vento supera ~6 m/s |
| Assorbimento, diffusione | mare limpido, acque interne più verdi e torbide: si sceglie con l'esposizione alle onde del raster (1 il mare, 0,8 un lago grande, ~0 un canale) |

Le onde e la normale si calcolano in due nodi **Custom** (HLSL scritto a mano,
nel factory dell'editor): è lo stesso conto di `WaveSpectrum.h`, che si prova
nei test. Due dettagli che contano:

- la posizione nel mondo arriva ai nodi Custom attraverso un nodo
  **TruncateLWC** (da *Large World Coordinates* a float normale): il rebase
  tiene il mondo entro ~10 km dall'origine, dove il float sbaglia di millimetri;
- le onde troppo piccole per lo schermo (meno di ~6 pixel) si spengono nella
  normale, stimando quanti metri copre un pixel da profondità e angolo di vista
  (non con `ddx`, che non esiste in tutti gli shader che il motore compila).

### 6.4 Le texture, generate dal codice

Come l'atlante delle strade: niente file binari da mantenere.

- **Normale di dettaglio** 256²: somma di onde con frequenze **intere**, quindi
  ripetibile senza cuciture, con uno spettro da mare mosso.
- **Schiuma** 256²: rumore cellulare ripetibile.
- **Raster d'acqua** di ogni tile (256², non sRGB: sono dati, non colori).

---

## 7. Le onde e il vento

### 7.1 Il vento

`UGeoWeatherSubsystem` tiene un **vento**: velocità (m/s), direzione **da cui
soffia** (gradi, come nei bollettini), raffiche. Cambia con dolcezza (costante
di tempo di qualche secondo: un cambio di vento è un cambio di mare, non uno
scatto). `geo.Weather.Wind 8 270` = 8 m/s da ovest.

**Le onde seguono il vento medio, le raffiche no.** Un'onda di 50 m ci mette
ore a formarsi: non segue un soffio di dieci secondi. Le raffiche muovono
l'increspatura (le normali di dettaglio, che scorrono più in fretta e si
fanno più marcate) e la schiuma sulle creste. È il seme della fase del
meteo: la pioggia, la neve e la nebbia che portano si sono aggiunte poi
(`docs/meteo-design.md`); le nuvole arriveranno con l'atmosfera.

### 7.2 Dallo spettro alle onde

Un mare "completamente sviluppato" (Pierson-Moskowitz) con vento U a 10 m ha:

- altezza significativa **Hs ≈ 0,21·U²/g** (8 m/s → 1,4 m; 15 m/s → 4,8 m);
- lunghezza d'onda di picco **λp ≈ 2π·U²/(0,877²·g)** (8 m/s → 53 m).

Da lì sei onde di Gerstner: lunghezze da 0,5·λp a 2·λp, direzioni sparse ±40°
attorno a quella **verso cui** soffia il vento, ampiezze che si dividono
l'energia di Hs, ripidità tale che la somma non si ripieghi su se stessa (le
creste a cappio di Gerstner). Velocità dalle acque profonde: ω = √(g·k).

Le onde più corte di 8 celle della mesh si spengono nella geometria (da 8 a
4 celle sfumano, sotto le 4 non ci sono): si vedrebbero a scalini. Restano
nella normale, che le disegna comunque.

### 7.3 Niente salti

- **Le fasi si integrano** (fase −= ω·dt, in double), non si calcolano come ω·t.
- **Un'onda non cambia mai lunghezza né direzione mentre c'è.** Ogni onda è
  sin(k·D·X + fase): se k cambia anche dell'1%, a 5 km dall'origine la fase
  cambia di decine di radianti, e il mare lontano "scorre" a velocità assurde.
  Quando il vento cambia, ogni onda confronta se stessa con quella che il
  vento nuovo vorrebbe: se è quasi la stessa (lunghezza entro il 3%, direzione
  entro 2°) cambia solo l'ampiezza, con dolcezza; altrimenti **sfuma a zero in
  2 s, e solo a zero** viene sostituita dalla nuova, che cresce da zero. Due
  onde alla volta: il mare non si spiana mai, e un cambio grosso (8 → 14 m/s)
  si completa in una decina di secondi. Il test lo misura in un punto vicino e
  in uno a 7 km: nessun salto oltre i 3 cm per frame.
- **Il rebase.** Le onde dipendono dalla posizione nel mondo, e un rebase la
  sposta di 10 km: senza correzione, il mare intero salterebbe. A ogni rebase
  si somma alla fase di ogni onda k·(D·Δ), dove Δ è lo spostamento dell'origine;
  in double, sulla CPU, e il materiale riceve fasi già ridotte a [0, 2π).
- Tutti i parametri delle onde viaggiano in una **Material Parameter
  Collection** (`MPC_GeoWater`): una scrittura per frame per tutte le tile,
  invece di una per istanza di materiale.

---

## 8. Cosa non fa (ancora)

- **Il mare aperto oltre il riquadro.** Dalla sezione 13 il mare aperto c'è,
  ma solo dentro il riquadro di `build-water` (il terreno, o `--bbox`): fuori
  dal dataset del terreno restano i buchi, in mare come in terra.
- **Il fondale vero al largo**: senza `--bathymetry` è la rampa della riva,
  fino a 60 m. Con la batimetria (EMODnet, GEBCO) è quella.
- **Le onde che frangono** sulla riva come geometria: qui si spengono in secca e
  la schiuma le racconta.
- **Sotto l'acqua** con la camera: nessun effetto di immersione.
- **Torrenti stretti** (< 4 m): restano la foto.
- **Gli affluenti** si cuciono al fiume in cui sboccano solo se la loro way
  finisce in un estremo di una way del fiume; se sboccano a metà di una way, il
  loro profilo è indipendente (al massimo un gradino di qualche decimetro alla
  foce).
- **Le cascate e le dighe** come oggetti: una diga è un salto nel profilo,
  smussato dalla mediana su un centinaio di metri (la traversa del Michelotti a
  Torino è una rampa di ~3 m).
- **Gli argini alti a ridosso dell'acqua** si abbassano per una cella e mezza
  della mesh (sezione 12): è il prezzo di una riva pulita a passo 2.

---

## 9. Alternative considerate

| Alternativa | Perché no |
|---|---|
| Il plugin **Water** di Unreal | È costruito attorno al Landscape e a un mondo locale: niente rebase, niente pianeta |
| Un piano d'acqua a quota zero ovunque | Il Po, il Garda e il mare non stanno alla stessa quota; e i terreni sotto il livello del mare (il delta del Po) finirebbero allagati |
| Maschera acqua sì/no al posto della distanza | Riva a scalini da vicino, niente schiuma che segue la costa |
| Scavare il terreno nella pipeline del terreno | Obbliga a rifare il terreno (ore) per ogni cambio all'acqua |
| FFT dell'oceano (Tessendorf) | Più bello da vicino, ma vuole compute shader e render target: non si prova senza il motore. Gerstner + normale di dettaglio è il passo prima |
| Fiumi come linee dipinte (come le strade) | Niente profondità, niente riflessi: è proprio ciò che non è "acqua realistica" |

---

## 10. Le stampe della posizione

`UGeoPositionHudSubsystem` disegna sulla destra dello schermo latitudine e
longitudine (decimali e gradi-primi-secondi), quota ellissoidica, quota **sul
livello del mare** (quando il dataset dell'acqua porta il geoide), altezza
**dal suolo** (dalla superficie disegnata del terreno) e rotta. Usa
`UDebugDrawService`, quindi funziona anche nel viewport dell'editor senza Play.
`geo.Hud 0|1`. La formattazione (virgola decimale, secondi che non arrivano mai
a 60) è nello strato puro e provata nei test (`Geo/GeoFormat.h`).

---

## 11. Dove sta nel codice

| File | Cosa |
|---|---|
| `Pipeline/geoworld/waterclasses.py` | da tag OSM a corpo d'acqua, con le regole della sezione 3 |
| `Pipeline/geoworld/waterformat.py` | formato `.gwa`, indici, manifest, geoide |
| `Pipeline/geoworld/waterbuild.py` | estrazione, taglio, indici |
| `GeoTiles/Public/Tiles/WaterTileFormat.h` | lettura del formato (puro) |
| `GeoTiles/.../Streaming/GeoWaterDataset.*`, `GeoWaterStreamingSubsystem.*` | dataset e caricamento |
| `GeoRender/Public/Water/WaterMesh.h` | scavo, mesh dell'acqua (puro) |
| `GeoRender/Public/Water/WaveSpectrum.h` | dal vento alle onde (puro) |
| `GeoRender/Public/Water/WaterTextures.h` | normale di dettaglio e schiuma (puro) |
| `GeoRender/.../Water/GeoWaterSubsystem.*` | il runtime |
| `GeoRender/.../Weather/GeoWeatherSubsystem.*` | il vento |
| `GeoWorldEditor/Private/GeoTerrainMaterialFactory.cpp` | `M_GeoWater` e `MPC_GeoWater` |

---

## 12. La prima prova sul Po, e cosa è cambiato

> "Il fiume è particolarmente brutto. Ha tipo scavato il buco ma solo alcuni
> vertici sono in alto, altri sono in basso." — la prima prova, su Torino.

Riprodotto con dati veri (Copernicus DEM a 30 m sull'area di Torino, l'acqua di
OSM attraverso Overture) e un simulatore del runtime: scavo, mesh a passo 2,
maschera della riva. Tre cause, in ordine di gravità.

**1. Quote finte fuori dal DEM: il buco di 200 m.** Il campionatore del
terreno leggeva le tile dei livelli bassi anche fuori dall'area del DEM, e i
post riempiti col livello del mare (`tilecut.py` scrive il geoide dove il
sorgente non ha dato). Il Po entra a Torino da sud, fuori dal DEM: il "minimo
progressivo" partiva da ~47 m e restava lì per tutto il fiume, e l'area del Po
(26 km², un'unica multipolygon) aveva per quota il 20° percentile di campioni in
gran parte finti: 44,8 m. Il fiume era scavato fino a ~45 m, tranne una striscia
attorno all'asse con la quota giusta: i "vertici in alto e in basso".
Ora `TerrainSampler.heights` legge solo il livello nativo, dentro il riquadro
del sorgente, e scarta i post che valgono il geoide; il profilo usa solo i
campioni validi (e non quelli a ridosso di un buco, che mescolano dato e
riempimento).

**2. Gradini fra un pezzo e l'altro.** Profili calcolati way per way, aree
fluviali con una quota sola per tutto il poligono, l'asse che riempiva l'area
solo per ~8 pixel, un canale vicino che "prestava" la sua quota al Po. Ora: way
cucite, aree che prendono la quota in tutta la larghezza dai soli assi che le
attraversano, aree sopra le linee nell'ordine di rasterizzazione (il Po è
un'area larga 128 m con dentro la sua linea "larga 12 m": la linea sopra dava
al centro del fiume le regole di un fosso).

**3. La mesh è più grossa del fiume.** A passo 2 una cella del livello 13 è
larga 38 m, e il Po ~130 m: fra un vertice in acqua e uno a terra c'è un
triangolo, e l'acqua si vede solo dove il triangolo sta sotto la superficie.
Con lo scavo di prima (0,3 m sulla riva, una rampa dolce) il vertice a terra,
alto, tirava su il triangolo: il terreno copriva il 20% dell'acqua del Po,
lungo tutte le rive, e dentro il fiume la profondità a zig-zag diventava uno
zig-zag di colore. Ora, due regimi:

- **acqua "risolta"** (larga almeno 4 post al livello della tile, il passo
  nominale del livello): scavata, con una sponda a **V continua**: la stessa
  funzione della distanza dalla riva fa la profondità dentro e il tetto del
  terreno fuori, zero sulla riva. Lungo un lato di un triangolo il terreno è
  allora lineare nella distanza, e incrocia l'acqua sulla riva. Fuori, il tetto
  segue la V per un post e mezzo e poi sale a 45°: un argine alto resta alto;
- **acqua troppo stretta**: niente scavo e niente superficie nel file; il
  runtime la appoggia sul terreno (sezione 6.2).

| Il Po a Torino, livello 13, passo 2 | prima | dopo |
|---|---|---|
| post d'acqua sotto il DEM di oltre 10 m | sì (fino a 242 m) | nessuno |
| acqua coperta dal terreno (pixel dentro la riva) | 19–23% | 2,2% |
| idem, livello 12 (Po "appoggiato") | 33% | 0% |
| idem, livello 13 a passo 1 | 2,8% | 0,3% |
| gradino medio della superficie fra post vicini | 27 m | 2 cm |

Il test `geowater_tests`, sezione 8, rifà queste misure **con il codice del
runtime** su due coppie di tile vere (`TestData/po_torino_*`), e fallisce sui
dati di prima.

### 12.1 La seconda prova: un dataset vecchio aperto

La seconda prova mostrava ancora un canyon, sulla Dora. Era il dato, non il
codice: rifatto con la pipeline di prima, lo scavo della Dora a ovest di
Torino arriva a 250–330 m esattamente dove guardava la camera; con quella
nuova la Dora non è scavata (è più stretta di 4 post) e il massimo dell'area è
16 m. Abbiamo controllato anche gli altri sospetti: un terreno con il livello
14 (la Dora diventa "risolta" e si scava: massimo 16 m), i processi lanciati
come su Windows (`spawn`: identico), e la ricostruzione sopra una cartella con
i dati vecchi e il loro `_work` (le regole nuove rifanno tutto).

Il formato non cambia quando cambiano le regole, quindi un dataset vecchio si
legge benissimo ed è sbagliato in silenzio. Adesso no: il manifest porta
`rulesVersion`, il motore lo confronta con `WaterRulesVersion`
(`WaterTileFormat.h`) e, se è vecchio, lo dice con una riga rossa fissa che
nomina la cartella e il comando; `verify-water` lo conta come problema. Un
test Python tiene allineati i due numeri. Il dataset vecchio si apre lo
stesso: nasconderlo farebbe sparire il sintomo, non la causa.

---

## 13. Il mare aperto

**Il buco.** Il dataset del terreno non scrive le tile senza nemmeno un post
di dato (`tilecut.py`: per l'Italia il riquadro è cinque volte la terra
emersa). Al largo, oltre i primi livelli, le tile non ci sono, e il quadtree,
raffinando la tile costiera, disegna solo le figlie che esistono: al posto di
quelle in mare, il vuoto. Da quota alta si vedeva il mare (le tile grossolane
lo coprono, piatto al geoide); scendendo, si apriva un buco grande come il
Tirreno.

**I dati: `opensea.bin`.** `build-water`, quando ha i poligoni del mare
(`--sea`), scende nel quadtree del terreno: una tile che c'è si attraversa,
una che manca si guarda in una maschera del mare (celle di 1/16 del lato di
una tile del livello più fine, ~200–300 m, una cella di margine per lato).
Tutta mare: si segna, al livello **più grossolano** possibile (vale per tutte
le discendenti); in parte: si scende; per niente: è terra fuori dal DEM, si
lascia stare. Con le tile, una griglia della **profondità del fondale**
(~430 m): la rampa della riva delle regole del mare (0,5 m + 5% della
distanza, fino a 60 m), o la batimetria se c'è. Su disco è piccolo (la
griglia compressa: decine o centinaia di kB, quasi tutta "60 m" o "terra");
in memoria la griglia dell'Italia intera pesa ~18 MB.

**Il runtime: tile virtuali.** Il dataset d'acqua carica `opensea.bin` e,
quando l'acqua è accesa, lo streaming del terreno riceve una **sorgente di tile
virtuali** (`FGeoVirtualTerrain`): per una tile che il dataset non ha e che il
mare aperto copre, dice che esiste, dà il suo intervallo di quote (25 punti,
senza costruirla) e la costruisce sul pool di thread come una lettura:
il **fondale** al geoide meno la profondità della griglia. L'acqua fa lo stesso
con la sua tile: superficie al geoide, niente scavo, tutta lontana dalla riva,
onde piene. Il resto del runtime (quadtree, mesh, geomorphing, materiale) non
sa che sono virtuali. Codice: `GeoTiles/Public/Tiles/OpenSea.h`.

Tre dettagli:

- **Si accende e si spegne con lo scavo.** Senza acqua (o senza il suo
  materiale) il terreno torna quello del DEM, buchi compresi: un fondale a −60 m
  senza acqua sopra sarebbe peggio del buco.
- **I dati condivisi sono immutabili** (`TSharedPtr` a dati costanti): i
  thread che costruiscono le tile possono finire dopo una riapertura del
  dataset, e una tile della sorgente di prima si butta (contatore di
  generazione).
- **"Dal suolo", al largo, è dal mare**: le stampe e il vento usano il geoide,
  non il fondale.

**Fra la tile costiera e quella virtuale** il fondale non è identico: la prima
ha la rampa calcolata sulla sua riva a 256 pixel, la seconda la griglia da
~430 m. La differenza è di qualche metro sotto qualche metro d'acqua; più al
largo, sotto 60 m d'acqua, non si vede.

| | Prima | Adesso |
|---|---|---|
| Al largo, scendendo di quota | buco | mare, con le onde |
| Dati in più | — | `opensea.bin`: piccolo su disco, ~18 MB in memoria per l'Italia |
| Costo a runtime | — | una tile di terreno e una d'acqua costruite in memoria: ~0,8 ms la coppia, sui thread del pool (mai sul game thread) |

Prove: Python (`OpenSeaOnSyntheticData`: le tile elencate non hanno terreno,
stanno tutte in mare, al livello più grossolano, coprono ogni buco; il fondale
scende allontanandosi dalla riva) e C++ (`geowater_tests`, sezione 9: il file
scritto dalla pipeline, le discendenti, la griglia, le tile virtuali, tutta
l'acqua visibile sopra il fondale).


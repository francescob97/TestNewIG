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
| **Fiume** (linea, area) | profilo lungo l'asse, sempre in discesa nel verso della linea | 0,3 m + 25%, fino a larghezza/15 (1–5 m) | 0,06 | 1 m/s nel verso della linea |
| **Canale** | come il fiume | 0,3 m + 50%, fino a 2,5 m | 0,03 | 0,35 m/s |

Tre note.

- **La quota di un lago è un percentile, non una media.** Il DEM sopra un lago è
  di solito piano alla quota dell'acqua, ma ai bordi prende la riva e a volte ha
  buchi. Il 20° percentile dei post dentro il poligono è robusto a entrambi.
- **Il profilo di un fiume scende sempre.** Le linee dei fiumi in OSM sono
  orientate a valle (è una regola del tag `waterway`). Si campiona il terreno
  lungo l'asse ogni 20 m e si prende il **minimo progressivo**: dove il DEM
  risale (un ponte, un argine, rumore) l'acqua non risale con lui.
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

Lo scavo vale anche per le quote che usano le **strade 3D** (stesso terreno
sotto, stessa origine della mesh) e per il **padre** nel geomorphing.

Se il dataset dell'acqua si apre o si chiude, il terreno si ricostruisce.

### 6.2 La mesh dell'acqua

Una griglia alla quota della superficie, con lo stesso passo del terreno,
**solo nelle celle vicine all'acqua**, più una gonna di 2 m sui bordi della
tile (contro le fessure fra livelli diversi, come per il terreno). La riva
vera la disegna il materiale con la distanza dal raster: la cella può
sporgere, il pixel no.

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
meteo: nuvole, pioggia, nebbia si aggiungeranno qui.

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

- **Il mare aperto oltre il terreno.** L'acqua sta sulle tile di terreno; dove
  il dataset del terreno non ha tile (mare aperto lontano dalla costa) non c'è
  niente da scavare né da coprire. Rimedio: rifare il terreno con la batimetria
  sotto (GEBCO), così le tile esistono anche al largo.
- **Le onde che frangono** sulla riva come geometria: qui si spengono in secca e
  la schiuma le racconta.
- **Sotto l'acqua** con la camera: nessun effetto di immersione.
- **Torrenti stretti** (< 4 m): restano la foto.
- **I fiumi spezzati in più linee** non sono ricuciti: il profilo scende dentro
  ogni linea, e fra una linea e l'altra può esserci un gradino di qualche
  decimetro.
- **Le cascate e le dighe** come oggetti: una diga è un salto nel profilo.

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

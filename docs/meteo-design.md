# Il meteo: pioggia, neve, grandine

> Il vento c'era già (`docs/vento-design.md`). Adesso il meteo ha anche la
> **precipitazione**: dove cade, cosa cade (pioggia, neve, grandine o una
> miscela, secondo la quota), quanto. E le sue conseguenze: le particelle
> attorno alla camera, il suolo bagnato e la neve posata, la visibilità che
> cala.
>
> Come provarlo: `docs/meteo-verifica.md`. Per chi impara: `docs/manuale/15-meteo.md`.

---

## 1. L'idea in breve

Il **dato** è un tempo impostato a mano (un comando, o domani un bollettino):

```
intensità      mm/h d'acqua nel cuore della precipitazione (0 = sereno)
copertura      quanta parte del suolo è sotto: 1 dappertutto, 0,3 rovesci sparsi
rovesci        quanto sono grandi, km
tipo           secondo la temperatura (Auto), solo pioggia, solo neve, con grandine
temperatura    °C al livello del mare, e quanto cala salendo (°C/km)
```

Da qui, cinque cose calcolate:

```
1. la MAPPA       dove cade e quanto: rovesci che viaggiano con il vento
                  in quota, nascono e muoiono in ~40 minuti            dipende dal punto
2. il TIPO        dalla temperatura con la quota: neve sopra lo zero
                  termico, pioggia sotto, mista in mezzo; grandine
                  nei nuclei dei temporali                             dipende dalla quota
3. la VISIBILITÀ  quanto lontano si vede: la neve nasconde dieci
                  volte la pioggia a parità d'acqua                    dipende da cosa cade
4. il SUOLO       si bagna, si asciuga; la neve si posa sopra una
                  quota che scende mentre nevica e sale mentre fonde   uno stato per il mondo
5. le PARTICELLE  gocce, fiocchi, chicchi attorno alla camera, che
                  cadono con il vento e con la loro velocità           attorno alla camera
```

Tutto il calcolo sta in `Plugins/GeoWorld/Source/GeoRender/Public/Weather/Precipitation.h`
(C++ puro, provato senza Unreal da `geoweather_tests`, sezioni 6–10).
Unreal riceve i risultati e li disegna (sezione 3).

**Deterministico**, come il vento: con lo stesso seme, nello stesso punto e allo
stesso istante, il rovescio è lo stesso per tutti.

---

## 2. I modelli, con i numeri

### 2.1 La mappa: dove cade

Un **rumore di gradiente** in quattro ottave (lo stesso della turbolenza del
vento, deviazione ~1) disegna la mappa, con le celle grandi quanto i rovesci
(default 8 km). Due differenze rispetto alle raffiche:

- **il tempo non accelera con l'ottava.** Nelle raffiche le scale piccole
  cambiano più in fretta (è la turbolenza); qui tutto il disegno cambia
  insieme, in ~40 minuti (la vita di un rovescio). Con il tempo che
  accelerava, i dettagli di un rovescio cambiavano otto volte più in fretta del
  rovescio stesso, e il rovescio non si vedeva viaggiare: il test 7 lo prova.
- **il trasporto** è con il vento a **3000 m s.l.m.** (~700 hPa, il "vento
  guida" dei meteorologi), il vento medio senza raffiche, integrato frame per
  frame come le raffiche e le onde (niente salti se il vento cambia).

La **copertura** decide la soglia: si cerca il valore del rumore sopra cui sta
la frazione voluta del suolo (l'inversa della normale, algoritmo di Acklam).
Sopra la soglia l'intensità parte dal 15% del massimo e sale al 100% in 1,2
deviazioni: i **nuclei** dei rovesci. Il test 7 misura la copertura su una
griglia: 20%, 50%, 80% chiesti, gli stessi trovati (entro 8 punti percentuali).

Con copertura 1 (pioggia "stratiforme", un fronte) cade dappertutto, con
un'ondulazione del 15%: non è mai uniforme.

**La cima delle nuvole.** Sopra non cade niente: **6 km** per una pioggia estesa
(i nembostrati), **10 km** per i rovesci e la grandine (i cumulonembi). Nel
chilometro più alto la precipitazione sfuma. Senza, un aereo a 10 km vedrebbe
nevicare.

### 2.2 Cosa cade: lo zero termico e la quota neve

```
temperatura(quota) = T al mare − 6,5 °C/km · quota
zero termico       = dove fa 0 °C
quota neve         = zero termico − 300 m      (i fiocchi ci mettono qualche
                                                centinaio di metri a fondere)
neve               0 sotto quota neve − 150 m, 1 sopra quota neve + 150 m
```

Con il default (15 °C al mare) lo zero termico è a 2.308 m e la quota neve a
2.008 m: in pianura piove, sulle Alpi nevica, **con lo stesso tempo**.
Con 2 °C al mare la quota neve scende a 8 m: nevica quasi ovunque.

I tipi imposti scavalcano la temperatura: `Snow` è neve a ogni quota, `Rain`
pioggia a ogni quota (il test 6 prova entrambi, anche a 25 °C e a −30 °C).

### 2.3 La grandine

Cade nei **nuclei** dei temporali: la frazione di grandine è
`0,6 · smoothstep(0,55, 0,9, livello)`, cioè niente ai margini del rovescio e
fino al 60% dove l'intensità è massima (il resto è pioggia, o neve in quota).
I chicchi crescono con l'intensità: 5 mm con poca grandine, 2 cm da 30 mm/h.

### 2.4 La visibilità

Koschmieder: si vede fin dove resta il 2% del contrasto,
`visibilità = 3,912 / estinzione`. L'estinzione di ogni precipitazione, in
1/km, dalle misure classiche:

| Precipitazione | Estinzione | 1 mm/h | 10 mm/h | 50 mm/h |
|---|---|---|---|---|
| Pioggia (e grandine) | 0,29 · R^0,67 | 13,5 km | 2,9 km | 1 km |
| Neve (mm/h d'acqua) | 3,9 · S^0,75 | 1 km | — (4 mm/h: 350 m) | — |

Le estinzioni si **sommano** (la neve mista a pioggia nasconde come le due
insieme). La neve nasconde dieci volte la pioggia a parità d'acqua: fiocchi
grandi, lenti, tanti.

### 2.5 La caduta

| Particella | Velocità | Da dove |
|---|---|---|
| Gocce | 4 m/s (1 mm/h) … 7 m/s (50 mm/h) | diametro mediano di Marshall-Palmer `0,89 R^0,21` mm, velocità terminale di Atlas `9,65 − 10,3 e^(−0,6 D)` |
| Fiocchi | 0,9 m/s, 1,5 m/s vicino a zero (neve bagnata) | misure |
| Chicchi | 14,5 m/s per 1 cm, come la radice del diametro (2 cm: 20,5 m/s) | velocità terminale di una sfera di ghiaccio |

Il vento le porta: i fiocchi del tutto, le gocce al 95%, i chicchi al 70%
(pesano). Con la raffica di adesso, sotto la camera.

### 2.6 Il suolo

Uno stato per il mondo, non per punto, guidato dalla precipitazione **al suolo
sotto la camera** (quella che si vede):

- **bagnato** 0..1: si bagna in 4 minuti con 2 mm/h (2 minuti con la pioggia
  forte, 16 con una pioggerella), si asciuga in 40 minuti, cinque volte più
  piano sotto zero;
- **neve**: una quota sopra cui il suolo è bianco. Mentre nevica scende verso
  la quota neve (con 1 mm/h metà della strada in ~10 minuti; da nessuna neve,
  si comincia 3 km sopra la quota neve); quando smette, sale di **20 m all'ora
  per ogni grado** sopra zero alla sua quota. Sotto zero non fonde.

Il materiale del terreno e delle strade guarda questo stato (sezione 3.3).

### 2.7 Le particelle

Una **scatola** di 50 × 50 × 30 m attorno alla camera, che si ripete nello
spazio. Ogni particella ha un posto fisso nella scatola; sulla scheda video sta
a

```
posizione = (posto + spostamento − camera), ridotta alla scatola
```

- lo **spostamento** di ogni tipo (gocce, fiocchi, chicchi) si **somma** frame
  per frame con la velocità di quel momento: niente salti quando il vento
  cambia;
- **meno la camera**: le particelle restano ferme nel **mondo** mentre la
  camera si muove (la parallasse è giusta: passando in auto la pioggia ti
  scorre addosso), ma la scatola segue la camera;
- la **densità** (quante si vedono, 0..1) cresce con l'intensità: le gocce
  `√(R/20)` (piena da 20 mm/h), i fiocchi `(S/2)^0,6` (pieni da 2 mm/h), i
  chicchi `√(H/10)`;
- le **dimensioni**: la scia di una goccia è quanto cade in 1/30 di secondo (fra
  5 e 40 cm: è così che la vedono l'occhio e le camere), un fiocco 1,2 cm, un
  chicco il suo diametro.

**Il rebase** sposta il mondo sotto la camera: lo spostamento si corregge della
stessa quantità (come le onde), e le particelle non saltano.

---

## 3. In Unreal

### 3.1 Il giro di un frame

`UGeoWeatherSubsystem::Tick`, dopo il vento:

```
1. vento a 3000 m s.l.m. sopra la camera  -> trasporta la mappa
2. precipitazione alla quota della camera e al suolo sotto (quote sul MARE: si toglie il geoide)
3. suolo: avanza con quella al suolo
4. visibilità: si smussa l'ESTINZIONE in 3 s (la nebbia non scatta; le estinzioni si sommano, le distanze no)
5. particelle: avanzano con quella alla camera e il vento di adesso alla camera
6. FGeoPrecipitationRenderer: particelle, MPC_GeoWeather, nebbia
```

### 3.2 Le particelle: una mesh, nessuna simulazione sulla CPU

`FGeoPrecipitationRenderer` crea, alla prima goccia, un attore transient con
un `UDynamicMeshComponent`: **32.768 quadrati degeneri** (4 vertici nello stesso
punto, 65.536 triangoli), una particella ogni ~2,3 m³ della scatola. Ogni
vertice porta l'angolo del quadrato (UV 0) e due numeri a caso (UV 1).

Il materiale `M_GeoPrecipitation` (Unlit, Translucent, due facce) fa tutto sulla
scheda video, nel **World Position Offset**:

- sceglie il tipo con il primo numero a caso e la miscela (`PrecipMix`): se il
  30% delle particelle visibili sono fiocchi, il 30% dei posti fa il fiocco;
- tiene la particella se il secondo numero è sotto la densità del suo tipo;
- la sposta (fase del suo tipo, ridotta alla scatola); i fiocchi ondeggiano di
  25 cm, ognuno con il suo passo;
- **apre** il quadrato: le gocce in una scia lungo la velocità, i fiocchi e i
  chicchi rivolti alla camera; mai più sottili di circa un pixel;
- le particelle che non servono collassano in un punto: nessun pixel.

Il pixel disegna la forma (scia, disco morbido, disco netto), le fa svanire
vicinissimo alla camera (sotto 30 cm: non accecano) e verso il bordo della
scatola (dove una particella esce da una parte e rientra dall'altra).

La CPU scrive **dieci vettori** per frame nella collezione `MPC_GeoWeather`:

| Parametro | Contenuto |
|---|---|
| `GroundWeather` | bagnato, quota della neve, transizione (150 m), neve sì/no |
| `GroundFrame` | geoide all'origine, raggio della Terra |
| `PrecipRain`, `PrecipSnow`, `PrecipHail` | fase (m, nord-est-su) e densità di ogni tipo |
| `PrecipRainVel`, `PrecipSnowVel`, `PrecipHailVel` | velocità (m/s) e dimensione (m) |
| `PrecipBox` | la scatola (m) e la luminosità |
| `PrecipMix` | la miscela dei tipi |

### 3.3 Il suolo bagnato e la neve

`geo.Imagery.CreateMaterial` mette un nodo "Meteo sul suolo" fra il colore e le
uscite di `M_GeoTerrain` e `M_GeoRoad`:

- **la quota** del pixel sul mare, dalla sua posizione nel mondo:
  `Z + d²/2R − geoide all'origine` (all'origine Z = 0 è l'ellissoide; il
  secondo termine è la curvatura della Terra, che a 30 km fa 70 m);
- **la neve**: sopra la quota della neve, con 150 m di transizione; resta sui
  pendii dolci e scivola via da quelli ripidi (normale); sulla foto i chiari
  (prati, campi) si imbiancano prima dei boschi scuri;
- **il bagnato**: colore al 55% (l'acqua riempie i pori) e ruvidità fino a 0,18
  (lucido).

Senza `MPC_GeoWeather` i materiali si costruiscono lo stesso, asciutti (lo dice
in giallo).

### 3.4 La nebbia

La precipitazione toglie visibilità con la `ExponentialHeightFog`:

```
densità = 56,4 / visibilità (m)
```

perché la nebbia di Unreal trasmette `2^(−densità/1000 · distanza in cm)`, e
la visibilità è dove ne resta il 2% (`1000 · log2(50) / 100 = 56,4`). Con la
pioggia la nebbia è quasi uniforme fino alle nuvole: la caduta con la quota va a
0,01 (dimezza ogni chilometro).

- Si usa la nebbia **del livello**, se c'è: si alza la densità finché piove, e
  quando smette si rimettono i valori di prima. Mai meno nebbia di quella che
  il livello aveva. La nebbia del livello sta alla sua quota: se il suolo è più
  in alto, la densità si corregge (fino a 16 volte) perché sul suolo sia quella
  voluta.
- Se non c'è, se ne crea una **transient**, che si sposta sul suolo sotto la
  camera.
- **Nell'editor la nebbia del livello non si tocca**: con il livello salvato
  mentre piove, finirebbero nel file i valori della pioggia. Lì la visibilità
  cala solo in Play (o se il livello non ha una nebbia).

`geo.Weather.Fog 0` spegne tutto questo.

### 3.5 Chi lo vede

- **Le stampe** a destra: `Meteo` (es. `pioggia 4,2 mm/h`), `Temperatura` (con
  lo zero termico), `Visibilita'` (solo se sotto i 50 km), `Suolo` (solo se
  bagnato o innevato).
- **L'overlay** (`geo.Weather.Debug 1`): sotto quello del vento, il tempo
  impostato, la temperatura (zero termico, quota neve, cima delle nuvole), il
  suolo, cosa cade qui e al suolo, le particelle, la visibilità e quale
  nebbia si usa; in giallo cosa manca.
- **I Blueprint**: `GetPrecipitationAt(WorldLocation)` restituisce un
  `FGeoPrecipitationInfo` (mm/h, frazioni, temperatura, visibilità).

### 3.6 I comandi

```
geo.Weather.Rain [mm/h] [copertura] [km]       pioggia a ogni quota (default 4 mm/h, dappertutto)
geo.Weather.Snow [mm/h] [copertura] [km]       neve a ogni quota (default 1 mm/h d'acqua)
geo.Weather.Hail [mm/h] [copertura] [km]       temporali con grandine (default 30 mm/h, 40%, celle da 6 km)
geo.Weather.Precipitation [mm/h] [cop.] [km]   pioggia o neve secondo la temperatura (default 3 mm/h)
geo.Weather.Clear                              sereno (il suolo si asciuga da solo)
geo.Weather.Temperature <°C al mare> [°C/km]   dove la pioggia diventa neve (default 15 e 6,5)
geo.Weather.Wetness <0..1>                     il suolo bagnato, adesso
geo.Weather.SnowCover <quota> | off            la neve sul suolo sopra una quota, adesso
geo.Weather.Fog <0|1>                          la nebbia della precipitazione
```

Intensità indicative: 1 mm/h pioggia debole, 4 moderata, 15 forte, 50 a
dirotto; 1 mm/h di neve (d'acqua) sono ~1 cm di neve all'ora.

---

## 4. Cosa non fa (ancora)

- **Le nuvole e il cielo.** Piove da un cielo sereno, con il sole: la luce non
  cala. Arriveranno con l'atmosfera (Fasi 10–12).
- **Il suolo è uno per il mondo**: bagnato o no dappertutto, guidato da sotto
  la camera. Lontano, sotto un rovescio che non ti tocca, il suolo resta
  asciutto (e viceversa). La neve invece segue la quota, che è la cosa che
  conta di più.
- **Niente pozzanghere, cerchi sull'acqua, schizzi**, né neve accumulata in
  spessore (è un colore, non una geometria).
- **Piove dappertutto attorno alla camera**: anche sotto un ponte o in una
  galleria (non c'è un test di occlusione).
- **Le particelle sono solo attorno alla camera** (50 m): oltre, la
  precipitazione è la nebbia. Da 2 km di quota non si vedono le colonne di
  pioggia sotto i temporali.
- **Niente fulmini, tuoni, suono.**
- **Il tempo è impostato a mano**: domani da un bollettino (METAR, GRIB).

---

## 5. Alternative considerate

- **Niagara** (le particelle di Unreal). È lo strumento giusto per un effetto
  disegnato a mano, ma un sistema Niagara è un asset complicato da creare da
  codice, e la simulazione per particella non serve: qui le particelle non
  interagiscono con niente, e la loro posizione è una formula (posto +
  spostamento, ridotti alla scatola). Una mesh e un materiale, creati dallo
  stesso comando dei materiali dell'acqua, fanno lo stesso con niente sulla
  CPU, e il comportamento si prova nei test (la parte numerica è in
  `FPrecipitationParticles`).
- **Particelle sulla CPU.** 32.768 posizioni da aggiornare e ricaricare sulla
  scheda video a ogni frame: millisecondi spesi per una formula.
- **Pioggia a schermo** (un effetto di post-processo con le strisce). Costa
  poco, ma non ha profondità: girando la camera la pioggia gira con lei.
- **Uno stato del suolo per punto** (una texture di bagnato e di neve attorno
  alla camera, aggiornata con la mappa). Più vero, ma serve una texture che
  segue la camera, con il suo rebase. Il bagnato globale e la neve per quota
  coprono il caso comune: il fronte che arriva e copre tutto.

---

## 6. Dove sta nel codice

| File | Cosa |
|---|---|
| `GeoRender/Public/Weather/Precipitation.h` | il modello (puro): mappa, tipo, visibilità, caduta, suolo, particelle, la mesh |
| `GeoRender/.../Weather/GeoPrecipitationRenderer.*` | in Unreal: l'attore delle particelle, `MPC_GeoWeather`, la nebbia |
| `GeoRender/.../Weather/GeoWeatherSubsystem.*` | il giro di un frame, il rebase, l'overlay, `GetPrecipitationAt` |
| `GeoRender/Private/GeoPositionHudSubsystem.cpp` | le righe `Meteo`, `Temperatura`, `Visibilita'`, `Suolo` |
| `GeoRender/Private/GeoRenderModule.cpp` | i comandi `geo.Weather.*` |
| `GeoWorldEditor/Private/GeoTerrainMaterialFactory.cpp` | `MPC_GeoWeather`, `M_GeoPrecipitation`, il suolo in `M_GeoTerrain` e `M_GeoRoad` |
| `GeoCore/Public/Geo/GeoFormat.h` | "pioggia 4,2 mm/h", "1,2 km" |
| `Tools/StandaloneTests/geoweather_main.cpp` | i test (sezioni 6–10) |

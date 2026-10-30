# Il meteo: pioggia, neve, grandine

> Il vento c'era già (`docs/vento-design.md`). Adesso il meteo ha anche la
> **precipitazione**: dove cade, cosa cade (pioggia, neve, grandine o una
> miscela, secondo la quota), quanto. E le sue conseguenze: le particelle
> attorno alla camera, il suolo bagnato e la neve posata, la visibilità che
> cala. E i **fulmini**, con il loro tuono (sezioni 2.8, 2.9 e 3.7). E le
> **nuvole**: i cumuli di bel tempo e le nubi di pioggia sopra i rovesci
> (sezioni 2.10 e 3.8).
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

### 2.8 I fulmini

Codice: `Plugins/GeoWorld/Source/GeoRender/Public/Weather/Lightning.h`
(puro, provato da `geoweather_tests`, sezioni 11–13).

**Dove e quanti.** Nei **nuclei** dei temporali, gli stessi della mappa della
precipitazione: la densità è di **0,25 fulmini per km² al minuto** dove il
nucleo è pieno (con attività 1), cioè ~5 al minuto per un temporale con un
nucleo di 20 km². Il "quanto è nucleo" di un punto è
`smoothstep(0,55; 0,9; livello)` nei rovesci; in una pioggia estesa (un
fronte) i temporali sono rari e deboli, un decimo dei picchi più alti.
`Attività` moltiplica tutto: 3 un temporale forte, 5 una supercella.

**Come si estraggono: un processo di Poisson diradato.** A ogni frame si
tirano punti a caso nel cerchio di **30 km** attorno alla camera, quanti ne
darebbe la densità massima ovunque; ognuno si tiene con la probabilità "quanto
è nucleo" del suo posto. Il risultato è esattamente un processo di Poisson
con la densità vera, senza integrare la mappa. Il test conta un'ora di
fulmini: 5.806 contro i 5.951 attesi.

**Di che tipo.** Circa **uno su tre** arriva al suolo (nube-suolo); gli altri
restano nella nube (si vede il cielo che si illumina).

**Il lampo: i colpi di ritorno.** Un nube-suolo non è un colpo solo: dopo il
primo, sullo stesso canale ne passano altri, in media **3** in tutto (fino a
8), a **30–90 ms** l'uno dall'altro, un po' più deboli. Ogni colpo è un
picco che si spegne in ~25 ms; fra un colpo e l'altro resta una luce debole
(la corrente continua). È per questo che il lampo **sfarfalla**. Tutto dura
~0,3 s.

**La forma.** Un canale dalla nube al punto colpito. Parte dalla **base della
nube di pioggia** (sezione 2.10), qualche centinaio di metri dentro, spostato
di lato fino a un quinto dell'altezza; un lampo nella nube sta nel suo cuore,
a metà fra la base e la cima. Il canale si costruisce con lo **spostamento del
punto medio**: il segmento si spezza a metà, il punto medio si sposta di lato
(fino a un quarto della lunghezza), e si ripete sulle due metà, sette volte:
128 pezzi, zig-zag sempre più piccoli. Poi fino a 9 **rami**, più deboli
(0,35), che scendono di lato e si fermano a mezz'aria. Nell'ultimo 15% in alto
la luce sfuma: lì il canale entra nella nube.

### 2.9 Il tuono

**Il ritardo.** Il suono fa 340 m/s a 15 °C (`331,3 + 0,606 · T`): **tre
secondi per chilometro**. Il tuono di un fulmine a 3,4 km arriva dopo 10 s.

**Il rombo.** Tutto il canale fa il botto nello stesso istante, ma ogni pezzo
sta a una distanza diversa da chi ascolta: il botto arriva **spalmato** su
`(distanza del pezzo più lontano − del più vicino) / velocità del suono`. Un
fulmine a 300 m con il canale alto 3 km brontola per 8 s; uno a 10 km, dove
tutti i pezzi sono quasi alla stessa distanza, molto meno.

**Lo schiocco e il brontolio.** L'aria assorbe gli acuti con la distanza.
Ogni pezzo di canale è un colpo di rumore filtrato con un passa-basso che si
chiude con la distanza (`120 + 5000 · e^(−d/1200)` Hz: 4,7 kHz a 100 m, 2,3 kHz
a 1 km, 120 Hz a 10 km): da vicino uno schiocco, da lontano un brontolio. Sotto,
un rumore molto basso (~90 Hz) che si spegne in 1–2 s: le eco del suolo e
delle nubi.

**Il volume** cala con la distanza come l'ampiezza di un'onda sferica
(`1000 m / distanza`, fra 0,06 e 1): −6 dB ogni volta che la distanza
raddoppia. Oltre **25 km** il tuono non si sente.

Il tuono si **sintetizza**: niente file audio. 22 kHz mono; per un fulmine
vicino (il rombo più lungo) sono 8 ms di calcolo in media, 16 nel caso
peggiore: un frame intero. Per questo lo fa un **thread di lavoro**; sul game
thread si calcola solo quando arriva (le distanze del canale) e si costruisce
il canale (8 microsecondi).

### 2.10 Le nuvole

Codice: `Plugins/GeoWorld/Source/GeoRender/Public/Weather/Clouds.h` (puro) e
`Weather/CloudShader.inl` (lo shader: vedi 3.8). Provati da `geoweather_tests`,
sezioni 14–15.

**Due tipi di nuvole, due origini.**

```
nubi di PIOGGIA   dalla stessa mappa dei rovesci (2.1): dove piove c'e' sempre
                  la nuvola sopra, e si allarga un po' oltre la pioggia
cumuli di BEL     chiazze di ~1,5 km, base piatta, cima a cupola; quanti, lo dice
TEMPO             la copertura impostata (geo.Weather.Clouds)
```

**Le nubi di pioggia.** La mappa della precipitazione è un rumore con
deviazione ~1, e piove dove supera la soglia `T` che lascia sopra di sé la
copertura chiesta. La nuvola usa **lo stesso rumore**: la sua copertura è
`smoothstep(T − 0,9; T − 0,1; rumore)`, quindi è piena dove piove (il rumore
è sopra `T`) e sfuma fino a 0,9 deviazioni sotto. Il test lo conta: dove
piove la nuvola c'è sempre, piena; il cielo nuvoloso è più di 1,3 volte
quello che piove. Le **cime**: nei rovesci `0,35 + 0,65 · smoothstep(0,15; 1;
livello)` fra la base e la cima della precipitazione (10 km nei temporali),
cioè torri alte nei nuclei e basse ai margini; attorno alla pioggia, un quarto
della copertura. Una pioggia estesa (un fronte) è **cielo coperto**, con la
cima fra il 55% e il 100% dei 6 km. La **base** è più bassa di quella dei
cumuli: il 65%, mai sotto i 300 m (con la base dei cumuli a 1.500 m, a 975 m).

**I cumuli di bel tempo.** Due ottave di rumore di valore (celle di 1,5 km e
di ~520 m) fanno le **chiazze**; dove una chiazza supera la soglia, c'è un
cumulo, tanto più alto quanto più la supera (la **cupola**, fino allo
spessore impostato, 1.200 m di default). La **base è piatta**: lì l'aria che
sale arriva a condensare, alla stessa quota per tutti. Un rumore più fine
(~280 m) **mangia i bordi** dove la nuvola è sottile: sfrangiati, non tagliati
con l'accetta. Le chiazze cambiano in mezz'ora e viaggiano con il vento in
quota, come i rovesci.

**La copertura giusta: una tabella misurata.** "Il 25% del cielo" vuol dire:
il 25% delle colonne di cielo ha dentro un po' di nuvola, a qualunque quota
(quello che si vede guardando in su). Il rumore non è uniforme e i bordi
sfrangiati mangiano un po' di nuvola, quindi la soglia che dà il 25% non è una
formula: si è **misurata** sullo shader stesso, compilato come C++, su 40.000
colonne, per 18 soglie da 0 a 0,85 (`FairCloudThreshold`). Il test la riprova
in un'altra zona: chiesti 10, 25, 50 e 75%, misurati 9, 24, 48 e 75%.

**Niente salti.** Il disegno dei cumuli sta in metri verso nord e verso est,
**ancorato a un punto fisso** (il primo posto visto). Con un'ancora mobile (il
coseno della latitudine dell'origine) un rebase di 10 km verso nord spostava i
cumuli di quasi un chilometro verso est. Perché i numeri restino piccoli per un
float, l'origine del disegno si riporta dentro **1.500 km** e l'orologio dentro
**un giorno**: il rumore dello shader è **periodico** con gli stessi passi
(ogni ottava avvolge le sue celle: 1000, 2900, 5360), quindi il riporto non si
vede. I test lo provano: un rebase cambia la densità di 2 decimillesimi; il
disegno spostato di un periodo o di un giorno, di 6.

**La mappa meteo.** Lo shader non può rifare il rumore della pioggia (hash a
64 bit, decine di volte per pixel): gliela si dà come **texture**, come fanno
i sistemi di nuvole dei giochi. **256 × 256** texel (625 m l'uno) su **160 km**
attorno alla camera: copertura (R), cima (G), pioggia (B). Fra un ricalcolo e
l'altro lo shader la **sposta di quanto ha soffiato il vento in quota** da
quando è stata fatta: il test sposta il vento per 10 minuti a 15 m/s e trova
la stessa nuvola 9 km più a nord.

**Densità ed estinzione.** Una nuvola piena ha un'**estinzione** di 0,03 al
metro (dentro si vede a ~130 m); le nubi di pioggia fino a 2,5 volte tanto
(più acqua: più scure). `geo.Weather.CloudDensity` la scala.

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
7. i fulmini nati in questo frame (FGeoLightningRenderer)
8. FGeoCloudRenderer: la mappa meteo (su un thread di lavoro), i parametri, la VolumetricCloud
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

I fulmini:

```
geo.Weather.Storm [mm/h] [copertura] [km]      temporali: rovesci con i fulmini (25 mm/h, 35%, celle da 6 km)
geo.Weather.Lightning <0..5>                   quanti fulmini: 0 nessuno, 1 normale, 3 forte, 5 supercella
geo.Weather.Strike [distanza m] [nube]         un fulmine adesso, davanti alla camera (default 1.500 m)
geo.Weather.Thunder <0..2>                     il volume del tuono
```

`geo.Weather.Hail` accende anche i fulmini (la grandine viene dai temporali);
`geo.Weather.Clear` li spegne.

Le nuvole:

```
geo.Weather.Clouds [copertura] [base] [spessore]   i cumuli (default 0,25, base 1.500 m s.l.m., alti 1.200 m)
geo.Weather.Clouds off | on                        niente nuvole GeoWorld (quella del livello torna) / di nuovo
geo.Weather.CloudDensity <x>                       quanto sono fitte: 1 normale, 2 il doppio
```

Le nubi di pioggia non hanno un comando: vengono dalla precipitazione.
`geo.Weather.Clear` non tocca i cumuli (il sereno ha i suoi);
`geo.Weather.Clouds 0` li toglie.

### 3.7 I fulmini a schermo e il tuono

`FGeoLightningRenderer` (`GeoRender/.../Weather/GeoLightningRenderer.*`). Ogni
fulmine è un **attore transient** che vive meno di un secondo:

- **il canale**: una mesh di **nastri**, uno per pezzo, girati verso la camera
  nel momento in cui il fulmine nasce (in 0,3 s la camera non si sposta
  abbastanza da accorgersene). Il filo è largo almeno 1,5 m e da lontano
  almeno ~1 pixel (1,2 mm per metro di distanza); il nastro è 8 volte tanto,
  per l'alone. Materiale `M_GeoLightning`: Unlit, **Additive**, un filo
  accecante (emissivo 80) dentro un alone (4). La luce di ogni istante arriva
  nei **Custom Primitive Data**: ogni fulmine sfarfalla per conto suo;
- **la luce**: una **point light** senza ombre, a un terzo dell'altezza del
  canale. Al suolo sotto di lei vale **una volta e mezza il sole del livello**
  (di notte, almeno qualche lux): si vede di giorno e acceca di notte. Un
  lampo nella nube è solo la luce, nel cuore della nube di pioggia e più larga (25 km di raggio, 0,6
  volte il sole). L'emissivo del canale si misura anche lui sul sole: con un
  sole fisico (100.000 lux) l'esposizione è molto più bassa;
- **il tuono**: sintetizzato su un thread di lavoro quando il fulmine nasce, e
  suonato quando arriva (`USoundWaveProcedural`, 2D);
- **il rebase**: il fulmine ricorda dove è caduto in coordinate geografiche e
  si rimette a posto a ogni frame.

Al massimo 12 fulmini a schermo e 8 tuoni in coda o che suonano (quelli entro
5 km si sentono sempre). In una supercella (attività 5, una decina di fulmini
al secondo nel cerchio) il resto si vede solo come luce.

**Per i Blueprint**: l'evento `OnLightningStrike` del `GeoWeatherSubsystem`
(posizione, nube-suolo o no, distanza) a ogni fulmine.

**Nelle stampe**: la riga `Fulmine` (a che distanza, quanti secondi fa, fra
quanto il tuono); nell'overlay quanti nell'ultimo minuto e l'attività.

### 3.8 Le nuvole a schermo

`FGeoCloudRenderer` (`GeoRender/.../Weather/GeoCloudRenderer.*`).

**La VolumetricCloud di Unreal.** È un guscio sferico attorno al pianeta, fra
due quote; la scheda video lo attraversa a passi e a ogni passo chiede al
**materiale** com'è l'aria lì: quanto è densa (l'*estinzione*), di che colore
diffonde la luce (l'*albedo*), se fa luce da sé (l'*emissivo*). La luce del
sole, le ombre dentro la nuvola, il cielo che la illumina da sotto li fa il
motore. Il nostro lavoro è il materiale, `M_GeoClouds`: dominio **Volume**,
**Additive**, **Default Lit**; l'estinzione va nel pin che l'editor, in
questo dominio, chiama *Extinction* (è il `SubsurfaceColor`).

**Lo shader è provato.** La densità la calcola un nodo Custom, e il suo testo
sta in `Weather/CloudShader.inl`, scritto in una sintassi che è **HLSL e C++
insieme**: dentro `GEO_CLOUD_SHADER( ... )`. La factory dei materiali lo
trasforma in una stringa con il preprocessore (`#__VA_ARGS__`) e la mette
nel nodo; i test lo compilano come corpo di una funzione C++, con dei tipi
che imitano quelli di HLSL (`Tools/StandaloneTests/HlslShim.h`: `float3`,
`lerp`, `saturate`, la lettura bilineare di una texture). Quello che i test
provano è **esattamente** quello che gira sulla scheda video. Le regole per
scriverci: solo gli swizzle `.x .y .z .w`, nessun `#`, le funzioni d'aiuto in
una `struct` (in un nodo Custom si scrive solo un corpo di funzione: è il
trucco classico), da float a uint sempre passando per int.

**La mappa sulla scheda video.** Ogni **8 secondi** (o subito se la camera si
è spostata di 20 km, o il tempo è cambiato con un comando) un thread di
lavoro rifà la mappa (`BuildCloudMap`); quando è pronta diventa una texture
lineare (dati, non colori) e va all'istanza del materiale, parametro
`CloudWeatherMap`. Le collezioni di parametri non portano texture; il resto
sì, sei vettori in `MPC_GeoWeather`:

| Parametro | Contenuto |
|---|---|
| `CloudMap` | il centro della mappa negli assi di Unreal (m: cambia con il rebase), il suo lato |
| `CloudShift` | di quanto il vento in quota l'ha spostata da quando è stata fatta (m) |
| `CloudLayer` | base dei cumuli, cima delle nubi di pioggia, soglia dei cumuli, loro spessore |
| `CloudLook` | estinzione, geoide all'origine, base delle nubi di pioggia, orologio |
| `CloudFlash` | il lampo dentro la nuvola: dove (m) e quanto |
| `CloudFair` | dove sta l'origine nel disegno dei cumuli (m) |

**Lo strato.** Il motore attraversa solo il guscio fra base e cima: lo si
imposta (in km) da `CloudLayerBounds` — dalla base più bassa alla cima più
alta, più il geoide all'origine (il guscio è sull'ellissoide, le nostre quote
sul mare) e 100 m di margine. Si ridà solo quando cambia: ogni volta il
motore rifà le nuvole nella scena. Quando non c'è niente da disegnare (cumuli
a zero e nessuna pioggia) il componente si **nasconde**: non costa niente.

**Quale VolumetricCloud.** Se ne vede una sola per volta, quindi se ne usa una
**nostra**, transient. Se il livello ne ha già una, **in Play** la si nasconde
finché le nuvole sono di GeoWorld (`geo.Weather.Clouds off` la fa tornare);
**nell'editor non si tocca** — nascosta, si salverebbe nascosta — e
l'overlay lo dice. Senza una SkyAtmosphere il guscio usa il raggio della
Terra (6.371 km, lo stesso della curvatura dello shader) con la cima del
pianeta nell'origine del mondo: lì dove il rebase tiene l'ellissoide.

**La luce.** Le nuvole le illumina la luce direzionale con **Atmosphere Sun
Light**; le ombre sul terreno le fa se ha **Cast Cloud Shadows**. Non si
toccano le luci del livello: l'overlay e `geo.Weather.Clouds` dicono in giallo
cosa manca.

**Il lampo nella nuvola.** Il fulmine più forte del momento (il punto della
sua luce e quanto è acceso, `FlashBrightness`) va in `CloudFlash`: la nuvola
si accende in un raggio di qualche chilometro (`e^(−d/2,5 km)`). Quanto,
è una **stima** (`CloudGlowPerLux` in `GeoCloudRenderer.cpp`): il cuore del
lampo luminoso quanto la nuvola al sole. Di giorno si vede appena, di notte
acceca, come quello vero.

**Nelle stampe**: la riga `Nuvole` (i cumuli; la nube di pioggia sopra di te,
dalla base alla cima). Nell'overlay: cumuli e nubi di pioggia, cosa c'è sopra
di te, quale VolumetricCloud si usa, lo strato, quanti secondi ha la mappa; in
giallo cosa manca.

---

## 4. Cosa non fa (ancora)

- **Il cielo non si scurisce da solo.** Sotto un cielo coperto la luce cala
  solo se la luce del sole fa le ombre delle nuvole (*Cast Cloud Shadows*), e
  la luce del cielo (la SkyLight) resta quella del sereno se non si ricattura.
  Niente nubi alte (cirri, altostrati): solo cumuli e nubi di pioggia.
- **Le nuvole non vivono**: un cumulo non cresce fino a diventare un
  temporale; i temporali sono dove li mette la mappa della precipitazione.
- **Oltre 80 km dalla camera** le nubi di pioggia non ci sono (la mappa è di
  160 km); i cumuli sì, dappertutto.
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
- **Il fulmine non sceglie i punti alti** (campanili, creste): cade dove
  capita nel nucleo.
- **Il tuono è 2D e non conosce il terreno**: niente eco delle valli, niente
  ombra acustica dietro una montagna. E si sente solo il tuono: la pioggia non
  ha ancora un suono.
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
- **Il tuono da file audio** (qualche registrazione, scelta a caso). Suona
  meglio di qualunque sintesi, ma un file non sa a che distanza è il fulmine
  né com'è fatto il canale: il rombo lungo di un fulmine vicino e lo schiocco
  che diventa brontolio con la distanza vengono gratis solo sintetizzando
  dalla geometria. E non servono asset da distribuire.
- **Il fulmine come effetto Niagara (ribbon).** Stessa obiezione delle
  particelle: un asset da costruire nell'editor, per una geometria che il
  codice sa già calcolare (e i test provare).
- **Il rumore della pioggia rifatto nello shader**, invece della mappa meteo.
  Sarebbe esatto ovunque, senza texture né thread di lavoro; ma sono hash a
  64 bit e quattro ottave, chiamati a ogni passo del motore dentro lo strato
  (decine per pixel). La mappa costa una lettura di texture, e 625 m di
  risoluzione bastano a nuvole larghe chilometri.
- **Le nuvole di Unreal con le texture di rumore 3D** (come il materiale di
  esempio del motore). Più belle da vicino, ma sono asset da costruire
  nell'editor, e il risultato non si può provare nei test. Il rumore
  procedurale nel nodo Custom costa di più alla scheda video, ma sta tutto nel
  codice e i test lo compilano.
- **Nuvole come sprite o impostori** (quadrati con un'immagine di nuvola).
  Costano pochissimo, ma da un aereo ci si vola dentro: un volume è l'unica
  cosa che regge l'attraversamento.
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
| `GeoRender/Public/Weather/Lightning.h` | i fulmini (puro): dove e quanti, i colpi di ritorno, il canale, il tuono |
| `GeoRender/.../Weather/GeoLightningRenderer.*` | in Unreal: il canale, la luce, il tuono che suona |
| `GeoRender/Public/Weather/Clouds.h` | le nuvole (puro): la colonna di cielo, la mappa meteo, i parametri dello shader, la tabella della copertura |
| `GeoRender/Public/Weather/CloudShader.inl` | lo shader della densità, HLSL e C++ insieme |
| `GeoRender/.../Weather/GeoCloudRenderer.*` | in Unreal: la VolumetricCloud, la mappa su un thread di lavoro, i parametri |
| `GeoRender/.../Weather/GeoWeatherSubsystem.*` | il giro di un frame, il rebase, l'overlay, `GetPrecipitationAt`, `OnLightningStrike` |
| `GeoRender/Private/GeoPositionHudSubsystem.cpp` | le righe `Meteo`, `Temperatura`, `Visibilita'`, `Suolo`, `Nuvole`, `Fulmine` |
| `GeoRender/Private/GeoRenderModule.cpp` | i comandi `geo.Weather.*` |
| `GeoWorldEditor/Private/GeoTerrainMaterialFactory.cpp` | `MPC_GeoWeather`, `M_GeoPrecipitation`, `M_GeoLightning`, `M_GeoClouds` e `T_GeoCloudDefault`, il suolo in `M_GeoTerrain` e `M_GeoRoad` |
| `GeoCore/Public/Geo/GeoFormat.h` | "pioggia 4,2 mm/h", "1,2 km" |
| `Tools/StandaloneTests/geoweather_main.cpp` | i test (sezioni 6–10; i fulmini 11–13; le nuvole 14–15) |
| `Tools/StandaloneTests/HlslShim.h` | quel tanto di HLSL che serve a compilare lo shader delle nuvole come C++ |

# Capitolo 13 — Fase 9: l'acqua, il vento, e dove sono

> Mare, laghi e fiumi: dove stanno (pipeline), come scavano il terreno, come si
> disegnano (materiale Single Layer Water) e come si muovono (onde di Gerstner
> dal vento). In più le stampe della posizione sulla destra dello schermo. Il
> documento di progetto con tutti i numeri è `docs/fase9-design.md`; come
> provarla è in `docs/fase9-verifica.md`.

---

## L'idea in breve

L'acqua usa la stessa catena delle strade (capitolo 12): una pipeline che
prepara delle tile, uno streaming che le carica, un subsystem che le usa per
ogni tile di terreno. Cambia **cosa c'è nelle tile**:

1. per ogni post del terreno, la **quota della superficie** dell'acqua e quella
   del **fondale**;
2. un'immagine di 256 × 256 pixel con, per ogni pixel, **quanto si è lontani
   dalla riva**, quanto il punto è **esposto alle onde** e la **corrente**.

E cambia chi le usa: il terreno (che si **scava**), una mesh nuova (la
**superficie**), un materiale nuovo (l'**acqua**), e un mondo che ora ha un
**vento**.

---

## 13.1 L'acqua ha bisogno di profondità

Il DEM vede l'acqua come terreno: il mare è piatto a quota zero, un lago è
piatto alla quota del lago. Una superficie d'acqua posata lì sopra coinciderebbe
con il terreno: i due triangoli alla stessa quota si contendono il pixel
(*z-fighting*, uno sfarfallio a strisce), e sotto l'acqua non ci sarebbe niente
da vedere.

Quindi il terreno si **scava**: la pipeline calcola un fondale e il runtime
abbassa ogni post del terreno al minimo fra la sua quota e il fondale, **prima**
di costruirne la mesh. Senza batimetria vera il fondale è una **rampa** dalla
riva:

    profondità = profondità a riva + pendenza × distanza dalla riva   (fino a un massimo)

con numeri diversi per tipo d'acqua (mare 0,5 m + 5%, fino a 60 m; fiume
0,3 m + 25%, fino a un quindicesimo della larghezza...).

> 💡 **Esempio.** Un lago di 1 km²: a 10 m dalla riva è profondo 0,5 + 0,12 × 10
> = 1,7 m; a 50 m sarebbe 6,5 m, ma il massimo per un lago di 1 km² è
> 5 + 6 × √1 = 11 m, quindi ci arriva a ~88 m dalla riva.

Il "fondale" del file fa anche da **tetto** per il terreno appena fuori
dall'acqua, per una cella della mesh: è la sponda a V della sezione 13.12.

Lo scavo vive nel **terreno**, non nella pipeline del terreno: il subsystem
dell'acqua consegna al terreno una funzione ("dato questo blocco di quote,
modificalo") con `SetHeightEditor`. Così cambiare l'acqua non obbliga a rifare
ore di terreno, e spegnerla (`geo.Water.Enable 0`) rimette il DEM com'era.

> ⚠️ **Trappola.** Le quote scavate servono **ovunque** si usino le quote di
> quella tile: la mesh del terreno, il **padre** nel geomorphing (capitolo 6:
> una tile nasce con la forma del padre; se il padre non fosse scavato, il
> fondale emergerebbe per mezzo secondo) e le **strade 3D**, che devono avere
> la stessa origine del frame locale della tile su cui stanno.

## 13.2 Una distanza invece di un bordo

Il modo ovvio di dire dov'è l'acqua è una maschera: 1 = acqua, 0 = terra. Da
vicino una maschera di 256 pixel per una tile di 2 km è fatta di quadratini di
8 m: la riva sarebbe a scalini.

Si memorizza invece la **distanza dalla riva**, con il segno: positiva in acqua,
negativa a terra (*signed distance field*, SDF). La scheda video, filtrando,
**interpola** le distanze, e la riva è dove la distanza interpolata passa per
zero: una curva liscia a qualunque ingrandimento.

> 💡 **Esempio.** Due pixel vicini valgono +3 e −1 metri. Una maschera direbbe
> "acqua" e "terra", con il confine sul bordo fra i due pixel. La distanza dice
> che la riva è a tre quarti della strada dal primo al secondo: il confine cade
> lì, anche se lì non c'è nessun pixel.

La stessa distanza serve per altre due cose, gratis: la **schiuma** che segue la
costa per qualche metro, e le **onde** che si spengono negli ultimi 15 m prima
della riva (in secca le onde non ci sono).

Nel file la distanza è un byte: 128 + 8 × pixel, quindi ±16 pixel con un ottavo
di pixel di precisione. Oltre i 16 pixel interessa solo sapere "molto in acqua"
o "molto a terra".

## 13.3 Dove sta l'acqua, e a che quota

**I poligoni.** Laghi, bacini, lagune e i fiumi larghi sono aree in OSM
(`natural=water`, `waterway=riverbank`...). Il mare invece in un estratto
regionale è fatto di **linee di costa** aperte: chiuderle in poligoni è un
problema classico e difficile, che osmdata.openstreetmap.de risolve ogni giorno
per tutto il mondo. Si scaricano i loro poligoni (`fetch-water`).

**Le linee.** I fiumi e i canali stretti sono linee: si "ingrassano" della loro
larghezza (`width` di OSM, o una larghezza tipica per classe). I torrenti sotto
i 4 m restano la foto.

**Cosa si scarta.** I corsi `intermittent` e `seasonal` (le fiumare asciutte del
Sud sono la maggioranza dei "fiumi" in OSM), i tratti in galleria o coperti, le
piscine, i bacini dei depuratori.

**La quota della superficie** è il punto più delicato:

| Acqua | Quota |
|---|---|
| Mare, laguna | il **geoide**: le quote del progetto sono ellissoidiche (capitolo 2), e in Italia il mare sta 40–50 m sopra l'ellissoide |
| Lago | il 20° percentile del terreno dentro il lago: il DEM ha il lago piatto, ma ai bordi pesca un po' di sponda; il percentile basso la ignora |
| Fiume | un **profilo** lungo la linea: si campiona il terreno ogni 20 m, lo si pulisce (solo dati veri, mediana) e si prende il **minimo progressivo** nel verso della corrente, così l'acqua non risale mai. Le way dello stesso fiume si **cuciono**; le **aree** fluviali prendono la quota dagli assi che le attraversano |

> ⚠️ **Trappola (presa dai test).** Tagliare il poligono del mare sul riquadro
> di lavoro crea una "costa" finta sul bordo del riquadro: la distanza dalla
> riva lì è zero, e il fondale vicino al bordo veniva profondo 28 m invece di
> 60. Si taglia con un margine di 0,1°.

## 13.4 La pipeline e il formato

`build-water` lavora in tre stadi, come quella delle strade: estrae i corpi
d'acqua in blocchi, poi per ogni tile di terreno che tocca dell'acqua
**rasterizza** i corpi (con GDAL) al doppio della risoluzione con un margine,
calcola la distanza dalla riva (`gdal.ComputeProximity`, dentro e fuori),
campiona superficie e fondale sui post, e scrive la tile `.gwa`: un header di
32 byte e i piani compressi con zlib. Poi indici, manifest e una griglia del
geoide per l'area (le stampe della quota s.l.m. la usano).

Il **margine** serve alla distanza: un pixel sul bordo deve sapere che 10 pixel
più in là, nella tile accanto, c'è la riva. Senza, la schiuma si fermerebbe sul
confine della tile.

## 13.5 Il runtime, per ogni tile di terreno

1. Il terreno, prima di costruire la mesh, chiede: "c'è da scavare?". Se la
   tile d'acqua si sta ancora caricando la risposta è "non ancora", e la tile
   aspetta come aspetta le sue quote.
2. Su un thread di lavoro: le stesse quote scavate → la **mesh della
   superficie** (solo nelle celle vicine all'acqua, con una gonna sui bordi
   della tile) e i pixel del **raster** per la scheda video.
3. Sul game thread: la texture, l'istanza del materiale, il componente.

Come per le strade, una tile compare **solo con la sua acqua** (il predicato
"Acqua" del terreno): un lago scavato senza superficie sarebbe una buca.

## 13.6 Il materiale: Single Layer Water

Unreal ha un modello d'ombreggiatura fatto apposta per l'acqua: **Single Layer
Water**. Invece di un colore, l'acqua ha per ogni canale (rosso, verde, blu) un
coefficiente di **assorbimento** e uno di **diffusione**, per metro. Il motore
guarda quanta acqua c'è fra la superficie e ciò che sta dietro (il fondale
scavato, letto dal depth buffer) e ne ricava il colore.

> 💡 **Esempio.** L'acqua di mare assorbe il rosso (0,35 per metro) molto più
> del blu (0,025). In 30 cm d'acqua il rosso perde il 10%: si vede il fondale
> quasi com'è. In 10 m ne perde il 97% e il blu solo il 22%: il fondale sparisce
> e resta il blu. Il turchese in secca e il blu al largo non si dipingono:
> vengono da lì.

Il resto del materiale:

- **Masked** con la distanza come maschera: la riva la decide il raster;
- la **normale** e lo **spostamento** delle onde (sezione 13.7) li calcolano due
  nodi **Custom**, cioè codice HLSL scritto a mano dentro il grafo. È lo stesso
  conto di `WaveSpectrum.h`, che si prova nei test;
- l'ottica cambia con l'**esposizione** del raster: 1 il mare (limpido), ~0 i
  canali (più verdi e torbidi), i laghi in mezzo secondo la grandezza;
- i parametri delle onde arrivano da una **Material Parameter Collection**
  (`MPC_GeoWater`): una scrittura per frame per tutto il mondo, invece di una
  per ogni tile.

## 13.7 Le onde

**Gerstner.** Un'onda di Gerstner è una sinusoide in cui i punti della
superficie non vanno solo su e giù ma girano in piccoli cerchi: avanzano sulla
cresta e arretrano nel cavo. Le creste vengono strette e i cavi larghi, come
nel mare vero. Sei onde sommate, con direzioni e lunghezze diverse, fanno un
mare credibile.

**Quali onde, dal vento.** Non a occhio: dallo spettro di **Pierson-Moskowitz**,
che descrive un mare "completamente sviluppato" (il vento soffia da abbastanza
tempo e su abbastanza mare):

- altezza significativa Hs = 0,21 · U² / g (8 m/s → 1,4 m; 15 m/s → 4,8 m);
- lunghezza d'onda di picco λp = 2π · U² / (0,877² · g) (8 m/s → 53 m).

Le sei onde stanno fra 0,5 e 2 volte λp, si dividono l'energia secondo lo
spettro, vanno **verso cui** soffia il vento con ±40°, e corrono alla velocità
delle acque profonde, ω = √(g·k): le onde lunghe sono più veloci.

**Geometria e luce.** La mesh dell'acqua ha celle di 10–40 m: un'onda di 20 m
non ci sta. Le onde più corte di 8 celle si spengono nella **geometria** ma
restano nella **normale**, calcolata per ogni pixel: la luce le disegna anche
dove i vertici non possono. Due normali di dettaglio (texture di 9 e 31 m che
scorrono col vento) aggiungono l'increspatura di pochi centimetri che fa
luccicare il sole.

## 13.8 Niente salti

Ogni onda è `sin(k · D·X + fase)`, dove `X` è la posizione nel mondo. Tre modi
di sbagliare, tutti evitati:

1. **La fase come ω·t.** Quando ω cambia (cambia il vento) tutte le creste
   saltano. La fase invece si **integra**: fase −= ω · dt a ogni frame.
2. **Il mare che scorre.** È il più subdolo. Se `k` cambia anche poco, in un
   punto lontano `X` la fase cambia di `Δk · |X|`.

   > 💡 **Esempio.** Onda di 60 m (k ≈ 0,1), il vento la allunga dell'1% in un
   > secondo. A 5 km dall'origine la fase cambia di 0,001 × 5.000 = 5 radianti
   > al secondo: le creste lontane corrono a quasi una lunghezza d'onda al
   > secondo, avanti o indietro, solo perché è cambiato il vento.

   La regola: **un'onda che c'è non cambia mai lunghezza né direzione**. Se il
   vento nuovo ne vuole una diversa, quella vecchia sfuma a zero in due secondi
   e solo a zero viene sostituita dalla nuova, che cresce da zero; due onde alla
   volta, così il mare non si spiana. Il test segue un punto a 7 km durante un
   cambio da 8 a 14 m/s: il peggior salto è di 2,5 cm in un frame.
3. **Il rebase.** Il rebase sposta il mondo di 10 km sotto le onde. Si somma a
   ogni fase `k · D·Δ` (Δ è lo spostamento dell'origine), e il mare resta fermo
   rispetto alla Terra.

## 13.9 Il vento: il primo pezzo del meteo

`UGeoWeatherSubsystem` tiene un vento medio (velocità, direzione **da cui**
soffia, come nei bollettini) e lo modula con delle **raffiche** lente. È il seme
della futura fase del meteo.

Le onde lunghe seguono il vento **medio**; le raffiche muovono solo
l'increspatura e la schiuma. Non è solo una scelta tecnica (sezione 13.8): è la
fisica. Un'onda di 50 m ci mette ore a formarsi e non segue un soffio di dieci
secondi; le increspature di pochi centimetri invece nascono e muoiono in un
attimo, e sono proprio le "zampe di gatto" che si vedono correre sull'acqua
quando arriva una raffica.

## 13.10 La corrente

Nei fiumi la normale di dettaglio scorre lungo la corrente (dal raster: verso
est e verso sud, in centesimi di m/s). Non si può farla scorrere per sempre: dopo
un po' la texture sarebbe stirata in modo assurdo dove la corrente cambia. Il
trucco classico (*flow map*) usa **due fasi sfalsate di mezzo ciclo**: ognuna
scorre per 4 secondi e poi ricomincia, e si mescolano in modo che quella che
ricomincia abbia peso zero in quell'istante. L'occhio vede un flusso continuo.

## 13.11 Le stampe della posizione

`UGeoPositionHudSubsystem` disegna sulla destra latitudine, longitudine, quota
sull'ellissoide, quota sul livello del mare (con il geoide del dataset
d'acqua), altezza dal suolo e rotta. Due cose da Unreal:

- disegna con **`UDebugDrawService`**, che chiama una funzione per ogni
  viewport che ha acceso un "canale" (`Game` per il gioco, `Editor` per
  l'editor): così le stampe ci sono anche nell'editor senza Play;
- con il Play aperto esistono **due mondi** (quello dell'editor e quello del
  gioco) e ognuno ha il suo subsystem: ognuno disegna solo nei viewport del
  proprio mondo, o le stampe comparirebbero doppie.

La formattazione all'italiana (virgola decimale, punto delle migliaia, secondi
che arrotondati non arrivano mai a 60) sta nello strato puro, in
`Geo/GeoFormat.h`, ed è provata nei test.

> ⚠️ **Trappola.** 45° 4' 59,96" arrotondato al decimo fa 45° 4' 60,0": va
> scritto 45° 5' 0,0". Si arrotonda **prima** il numero totale di decimi di
> secondo, poi lo si spezza in gradi, primi e secondi.

## 13.12 La prima prova: il fiume "a vertici alti e bassi"

La prima prova sul Po ha mostrato un fiume scavato in una fossa, con alcuni
vertici in alto e altri in basso. Riprodurla con dati veri (il DEM di
Copernicus, l'acqua di OSM) e un simulatore del runtime ha trovato tre cause,
ognuna con una lezione.

**"Non lo so" non è un numero.** Le tile dei livelli bassi del terreno coprono
ben oltre l'area del DEM, e dove il sorgente non ha dati la pipeline del
terreno scrive il livello del mare. Per chi le legge sono numeri validi, e
sbagliati di centinaia di metri. Il Po entra a Torino da sud, fuori dal DEM:
il minimo progressivo partiva dal livello del mare e se lo portava dietro per
tutto il fiume. Ora il campionatore restituisce NaN fuori dal livello nativo e
sui post riempiti, e ogni calcolo usa solo i campioni validi.

> 💡 **Esempio.** Il primo campione valido del Po, sul bordo del DEM, valeva
> 194,6 m: un post a metà fra dato vero (262 m) e riempimento. Esteso a tutta la
> parte scoperta della linea, bastava lui a rovinare il fiume. Per questo si
> scartano anche i campioni **a ridosso** di un buco.

**Un numero solo per un poligono grande è sbagliato quasi ovunque.** L'area del
Po è una multipolygon di 26 km² che scende per chilometri: un solo percentile
non può andare bene dappertutto. La quota delle aree fluviali viene dall'asse che
le attraversa, punto per punto.

**La mesh decide cosa si può disegnare.** A passo 2, al livello 13, una cella è
larga 38 m. Fra un vertice nell'acqua e uno sulla riva c'è un triangolo: se il
vertice sulla riva è alto, il triangolo sale e copre l'acqua per mezza cella.
Due rimedi, secondo quanto l'acqua è larga rispetto alla mesh:

- **larga** (almeno 4 post): si scava con una **sponda a V**. La stessa
  funzione della distanza fa la profondità dentro e il "tetto" del terreno
  fuori, zero sulla riva. Lungo un lato del triangolo il terreno è allora
  lineare nella distanza dalla riva, e attraversa l'acqua proprio sulla riva;
- **stretta**: non si scava (sarebbe un pettine di buchi, uno per vertice che
  cade nel fiume). La superficie si **appoggia** sul terreno, 0,3 m sopra, con gli
  stessi vertici: l'acqua sta sopra il terreno in ogni punto, e la forma la
  disegna la maschera della riva.

> ⚠️ **Trappola (presa dai test).** La prima V aveva un piccolo salto sulla
> riva: 0,3 m di profondità subito dentro, 0,3 m di argine subito fuori. Sembrava
> innocuo: un vertice che cade proprio sulla riva prendeva +0,3 o −0,3 a seconda
> del lato, e questi 60 cm bastavano a far spuntare il terreno. La V giusta è
> **continua**: zero sulla riva.

E una lezione di metodo: il simulatore riproduce il runtime (scavo, mesh,
diagonale, maschera) e conta i pixel d'acqua coperti dal terreno. Ogni
correzione si misurava in pochi secondi, invece di un giro di compilazione su
Windows. Lo stesso conto, fatto con il codice C++ del runtime su due tile vere
del Po, ora è un test (`geowater_tests`, sezione 8).

### La seconda prova: il dato vecchio

Alla seconda prova il canyon c'era ancora, sulla Dora. Prima di toccare il
codice abbiamo messo a confronto la posizione della camera (dalle stampe a
destra) con i due dataset rifatti qui. Con la pipeline vecchia, la Dora era
scavata di 250–330 m proprio lì. Con quella nuova non era scavata per niente,
perché è stretta e quindi si appoggia. Il codice era a posto: Unreal stava
aprendo un dataset d'acqua fatto prima della correzione.

È un errore facile, perché un dataset vecchio **non è rotto**. Il formato non
è cambiato, i file si leggono, `verify` era contento: solo i numeri dentro
erano sbagliati. La cura è la stessa del materiale vecchio: il dato porta la
**versione delle regole** che l'ha fatto (`rulesVersion` nel manifest), il
motore conosce la sua (`WaterRulesVersion`), e se il dato è più vecchio lo
dice con una **riga rossa fissa**, con la cartella e il comando per rifarlo.

> ⚠️ **Trappola (fatta in questo progetto).** Quando una correzione cambia i
> **dati** e non il formato, versiona le regole e falle controllare a chi
> legge. Altrimenti il dato vecchio continua a girare e sembra un bug del
> codice nuovo.

---

## Nota Unreal

### Nodi Custom e Large World Coordinates

In Unreal 5 la posizione nel mondo nei materiali è in **Large World
Coordinates** (due float per componente, per i mondi grandi). Il codice HLSL di
un nodo Custom si aspetta float normali, e a seconda della versione del motore
riceverebbe un tipo LWC su cui `P.xy` non compila. Il factory mette in mezzo un
nodo **TruncateLWC**, cercato per nome a runtime (se una versione del motore
non lo avesse, il file compila lo stesso e lo dice nel log). Il float basta
perché il rebase tiene il mondo entro ~10 km dall'origine: millimetri di errore.

### Niente `ddx` nel nodo dei pixel

Per spegnere le onde troppo piccole per lo schermo serve sapere quanti metri
copre un pixel. Il modo solito è `ddx`/`ddy` (le derivate fra pixel vicini), ma
il motore compila lo stesso materiale anche per shader dove le derivate non
esistono (ray tracing, cattura delle superfici di Lumen). Si stima invece dalla
**profondità** del pixel e dall'**angolo** di vista.

### Texture di dati e campionatori "Linear Color"

Il raster dell'acqua, la normale di dettaglio e la schiuma sono **dati**, non
colori: si creano senza sRGB, e nel materiale si leggono con campionatori
*Linear Color*. Un campionatore deve avere una texture di default dello stesso
tipo, e quella del motore è sRGB: per questo il factory crea
`T_GeoWaterDefault`, 4 × 4 pixel lineari che servono solo a far compilare il
materiale.

### Material Parameter Collection

Una MPC è un piccolo insieme di parametri **globali**, condiviso da tutti i
materiali che lo usano. Si scrive con
`World->GetParameterCollectionInstance(Collection)->SetVectorParameterValue(...)`.
È la scelta giusta per ciò che è uguale in tutto il mondo (le onde, il vento);
ciò che cambia per tile (il raster) resta nelle istanze di materiale.

---

## Dove sta nel codice

| File | Cosa |
|---|---|
| `Pipeline/geoworld/waterclasses.py` | da tag OSM a corpo d'acqua, le regole della sezione 13.3 |
| `Pipeline/geoworld/waterformat.py` | formato `.gwa`, indici, manifest, geoide |
| `Pipeline/geoworld/waterbuild.py` | estrazione, taglio, verifica |
| `GeoTiles/Public/Tiles/WaterTileFormat.h` | lettura del formato (puro) |
| `GeoTiles/.../Streaming/GeoWater*` | dataset e caricamento |
| `GeoRender/Public/Water/WaterMesh.h` | scavo, mesh, raster (puro) |
| `GeoRender/Public/Water/WaveSpectrum.h` | dal vento alle onde, niente salti (puro) |
| `GeoRender/Public/Water/WaterTextures.h` | normale di dettaglio e schiuma (puro) |
| `GeoRender/Public/Weather/Wind.h`, `.../Weather/GeoWeatherSubsystem.*` | il vento |
| `GeoRender/.../Water/GeoWaterSubsystem.*` | il runtime |
| `GeoRender/.../GeoPositionHudSubsystem.*`, `GeoCore/Public/Geo/GeoFormat.h` | le stampe |
| `GeoWorldEditor/Private/GeoTerrainMaterialFactory.cpp` | `M_GeoWater`, `MPC_GeoWater` |
| `Tools/StandaloneTests/geowater_main.cpp` | i test |

---

## Riepilogo

- L'acqua ha bisogno di **profondità**: il terreno si **scava** sotto di lei,
  a runtime, con una funzione che il terreno applica alle quote.
- La riva è una **distanza** (SDF), non un bordo: nitida a ogni ingrandimento,
  e regala schiuma e onde che si spengono in secca.
- La quota dell'acqua: il **geoide** per il mare, un percentile del terreno per i
  laghi, un profilo sempre in discesa per i fiumi.
- Il colore lo fa la **profondità** (Single Layer Water), non un pennello.
- Le onde vengono dal **vento** attraverso uno spettro fisico; un'onda cambia
  solo ad ampiezza zero, e il mare non salta né scorre.
- Le raffiche muovono l'**increspatura**, il vento medio le **onde**.
- Dopo l'aggiornamento: `geo.Imagery.CreateMaterial`.
- "Non lo so" non è un numero: fuori dal DEM il campionatore dice NaN, non il
  livello del mare. E la mesh decide cosa si può disegnare: l'acqua larga si
  scava con una sponda a V continua, quella stretta si appoggia sul terreno.
- Un dato fatto con **regole vecchie** si legge benissimo ed è sbagliato in
  silenzio: il manifest porta `rulesVersion`, e il motore lo dice in rosso.

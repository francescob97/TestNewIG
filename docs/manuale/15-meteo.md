# Capitolo 15 — Il meteo: pioggia, neve, grandine

> Come si simula la precipitazione e come la si mette a schermo: dove cade,
> cosa cade a ogni quota, quanto toglie la vista, il suolo che si bagna e
> s'imbianca, e le particelle che cadono attorno alla camera. Poi i fulmini e
> il tuono (sezione 15.7). Il documento di
> progetto con tutti i numeri è `docs/meteo-design.md`; come provarlo è in
> `docs/meteo-verifica.md`.

---

## L'idea in breve

Un bollettino dice "pioggia moderata, 4 mm/h, quota neve 1.200 m". Come il
vento del capitolo 14, sono **numeri soli**; la simulazione ne fa un mondo:

1. **dove** cade: rovesci sparsi o un fronte che copre tutto, che viaggiano con
   il vento in quota;
2. **cosa** cade: dipende dalla temperatura, quindi dalla **quota**: in valle
   piove, in montagna nevica, con lo stesso tempo; nei temporali, grandine;
3. **cosa si vede**: le particelle attorno alla camera, la nebbia che chiude
   l'orizzonte, il suolo bagnato e la neve posata.

Il modello è tutto in C++ puro (`Weather/Precipitation.h`), provato senza
Unreal. Unreal riceve una manciata di numeri per frame e li disegna.

---

## 15.1 Dove cade: una mappa che viaggia

Un rovescio è una **macchia** di qualche chilometro, che nasce, cresce, viaggia
con il vento e muore in una quarantina di minuti. Per disegnare le macchie si
usa lo stesso **rumore di gradiente** delle raffiche (capitolo 14): un campo
liscio di valori attorno a zero, con le macchie grandi quanto si vuole.

La **copertura** dice quanta parte del suolo è sotto la precipitazione. Si
taglia il rumore con una soglia: dove sta sopra, piove. Quale soglia lascia
sopra di sé il 30% del suolo? Il rumore ha una distribuzione quasi normale,
quindi la risposta è l'**inversa della normale**: per il 30%, 0,52 deviazioni.
Sopra la soglia l'intensità cresce verso il centro della macchia: i **nuclei**.

> 💡 **Esempio.** `geo.Weather.Hail` chiede temporali sul 40% del suolo, celle
> da 6 km, 30 mm/h nei nuclei. Il test chiede il 20, il 50 e l'80% e misura la
> frazione bagnata su una griglia di 14.400 punti: la ritrova, con il massimo
> esattamente all'intensità chiesta.

Il rumore cambia piano nel tempo (i rovesci nascono e muoiono) e la mappa
intera **si sposta** con il vento a 3000 m (il "vento guida" che porta i
temporali). Lo spostamento si **somma** frame per frame, come per le raffiche
e le onde: se il vento cambia, la mappa non salta.

> ⚠️ **Trappola.** La prima versione faceva cambiare le scale piccole del
> rumore più in fretta di quelle grandi, come nella turbolenza. Risultato: i
> dettagli di un rovescio cambiavano otto volte più in fretta del rovescio, e
> nel test "i rovesci viaggiano con il vento" la mappa spostata non somigliava
> più a quella di prima. In un rovescio tutto cambia **insieme**.

## 15.2 Cosa cade: la quota decide

L'aria si raffredda salendo, circa **6,5 °C ogni chilometro**. Con 15 °C al
mare, a 2.308 m fa zero: è lo **zero termico**. I fiocchi nascono in alto,
sopra lo zero termico, e scendendo fondono; ci mettono qualche centinaio di
metri. Così:

```
sopra la quota neve + 150 m      neve
attorno alla quota neve          pioggia e neve insieme
sotto la quota neve − 150 m      pioggia
quota neve = zero termico − 300 m
```

> 💡 **Esempio.** Con 8 °C al mare: zero termico a 1.231 m, quota neve a 931 m.
> A Torino (240 m) piove; a Superga (670 m) ancora pioggia; a 900 m pioggia e
> neve insieme; sul Musiné (1.150 m) e sul Rocciamelone (3.538 m) neve. **Un
> comando, tre tempi.**

La **grandine** non dipende dalla quota ma dalla **forza** del temporale: si
forma nelle correnti ascendenti violente, quindi cade nei nuclei. Il modello la
mette solo dove l'intensità supera metà del massimo, fino al 60% di quello che
cade.

## 15.3 Quanto si vede

Una goccia è piccola, ma sono tante: ognuna toglie un po' di luce. Quanta,
lo dice il **coefficiente di estinzione**, la frazione di luce persa per
chilometro. E la **visibilità** è dove il contrasto di un oggetto scuro sul
cielo scende al 2% (la regola di Koschmieder):

```
visibilità = 3,912 / estinzione
```

Dalle misure: la pioggia ha un'estinzione di `0,29 · R^0,67` per km (R in mm/h),
la neve `3,9 · S^0,75`. A parità d'acqua, **la neve nasconde dieci volte la
pioggia**: un fiocco è grande, leggero, lento; per far cadere la stessa acqua ne
servono moltissimi.

> 💡 **Esempio.** Pioggerella (1 mm/h): 13,5 km. Acquazzone (50 mm/h): 1 km.
> Nevicata normale (1 mm/h d'acqua, ~1 cm di neve all'ora): 1 km.

## 15.4 Il suolo

Il suolo **si bagna** sotto la pioggia (qualche minuto) e **si asciuga** dopo
(quaranta, molto di più sotto zero). La **neve** si posa sopra una quota: mentre
nevica la quota scende verso la quota neve, tanto più in fretta quanto più
nevica; quando smette **fonde dal basso**, risalendo di 20 m all'ora per ogni
grado sopra zero. Sui pendii ripidi la neve non resta.

Lo stato del suolo è **uno per il mondo**, guidato da quello che cade sotto la
camera. È una semplificazione (sezione "Alternative"), ma copre il caso
comune: il fronte che arriva e bagna tutto, la nevicata che imbianca le
montagne fino a una quota.

## 15.5 Le particelle: una scatola che si ripete

Disegnare ogni goccia del mondo è impossibile. Ma si vedono solo quelle
**vicine**: oltre qualche decina di metri una goccia è più piccola di un pixel,
e la pioggia lontana è solo la nebbia della sezione 15.3.

Si mettono quindi 32.768 particelle in una **scatola** di 50 × 50 × 30 m
attorno alla camera, e si fa finta che la scatola si **ripeta** in tutto lo
spazio, come le piastrelle di un pavimento. Una particella sta a

```
(il suo posto + lo spostamento − la camera), riportato dentro la scatola
```

- **lo spostamento** cresce con la velocità della particella (caduta più vento):
  le particelle cadono;
- **meno la camera**: se la camera avanza di un metro, la particella si
  ritrova un metro più indietro, cioè **ferma nel mondo**. Girando la testa la
  pioggia non gira con te; camminando ci passi in mezzo;
- **riportato dentro la scatola**: la particella che esce da sotto rientra da
  sopra. Non finiscono mai.

> 💡 **Esempio.** Con 4 mm/h una goccia cade a circa 4,5 m/s: attraversa i 30 m
> della scatola in meno di 7 secondi, poi ricomincia dall'alto. Nessuno se ne accorge: le
> gocce sono tutte uguali, e al bordo della scatola svaniscono.

Tre tipi di particelle, con la loro velocità e la loro forma: le **gocce**
(strisce lunghe quanto cadono in un trentesimo di secondo: così le vede
l'occhio), i **fiocchi** (dischi morbidi che ondeggiano), i **chicchi** (dischi
netti e veloci).

## 15.6 Niente salti, ancora

Il rebase (capitolo 2) sposta il mondo sotto la camera: un punto che stava a
20 km dall'origine ora sta a zero. Se lo spostamento delle particelle non lo
sapesse, tutte le particelle salterebbero di 20 km (riportati nella scatola:
un salto a caso di qualche metro, ma visibile). Come le onde, al rebase lo
spostamento si corregge della stessa quantità.

## 15.7 I fulmini e il tuono

Un fulmine nasce dove l'aria sale più forte: nei **nuclei** dei temporali. Il
modello li prende dalla stessa mappa della precipitazione, con una densità:
0,25 fulmini per km² al minuto dove il nucleo è pieno, cioè una manciata al
minuto per temporale.

Per estrarli si usa un trucco elegante, il **processo di Poisson diradato**:
si tirano punti a caso nel cerchio attorno alla camera, quanti ne darebbe la
densità massima ovunque, e ognuno si tiene con la probabilità "quanto è
nucleo" del suo posto. Quelli che restano sono distribuiti esattamente come
se la densità fosse quella vera, punto per punto, senza aver mai integrato la
mappa.

**Il lampo sfarfalla** perché un fulmine non è un colpo solo. Il primo
"colpo di ritorno" apre il canale nell'aria; poi sullo stesso canale ne
passano altri, in media tre, a qualche decina di millisecondi l'uno
dall'altro. A 60 fps sono due o tre frame di luce piena separati da frame
quasi bui.

**La forma** viene dallo spostamento del punto medio: il segmento dalla nube
al suolo si spezza a metà, il punto medio si sposta di lato, e si ripete sulle
due metà, sette volte. Zig-zag dentro zig-zag: la stessa struttura frattale di
una costa.

> 💡 **Esempio: il tuono.** Il suono fa 340 m/s: **tre secondi per
> chilometro**. Conti i secondi fra il lampo e il tuono, dividi per tre, e sai
> a che distanza è caduto. Le stampe lo fanno per te: "tuono fra 9 s".

**Perché il tuono brontola.** Tutto il canale fa il botto nello stesso
istante. Ma il pezzo più basso può essere a 300 m da te e il più alto a 3 km:
il primo arriva dopo un secondo, l'ultimo dopo nove. In mezzo, tutti gli
altri: è il rombo. Più il fulmine è vicino, più le distanze dei pezzi sono
diverse, e più il rombo è lungo. Da lontano invece l'aria si è mangiata gli
acuti, e resta un brontolio basso.

Il tuono del progetto è **sintetizzato** da queste regole: ogni pezzo di
canale è un colpo di rumore filtrato (più è lontano, più il filtro taglia gli
acuti), messo nel tempo al suo istante d'arrivo. Nessun file audio.

> ⚠️ **Trappola.** Sintetizzare il tuono di un fulmine vicino costa 8–16 ms:
> fatto sul game thread, ogni fulmine vicino era un frame perso, cioè un
> piccolo scatto. Lo fa un thread di lavoro; sul game thread resta solo il
> conto di **quando** arriva.

---

## Alternative considerate

- **Niagara**, il sistema di particelle di Unreal. Lo strumento naturale, ma
  è un asset da costruire nell'editor, difficile da creare da codice, e la sua
  simulazione per particella qui non serve: la posizione di una goccia è una
  formula. Una mesh e un materiale fanno lo stesso, e la formula si prova nei
  test.
- **La pioggia disegnata sullo schermo** (strisce in post-processo). Costa
  poco, ma non ha profondità: girando la camera la pioggia gira con lei.
- **Il suolo bagnato punto per punto** (una texture attorno alla camera).
  Più vero (il suolo asciutto fuori dal rovescio), ma una texture che segue la
  camera ha il suo rebase, la sua risoluzione, il suo streaming. Un passo
  successivo.

---

## Nota Unreal

### Particelle senza particelle: il World Position Offset

Un materiale può **spostare i vertici** di una mesh: è l'uscita *World Position
Offset*, calcolata dalla scheda video per ogni vertice, a ogni frame. La mesh
delle particelle è fatta di quadrati **degeneri**: i quattro vertici di ogni
quadrato stanno nello stesso punto, quindi non si vede niente. Il materiale li
**apre**: sa da quale angolo viene ogni vertice (UV 0) e sposta ognuno di
mezzo quadrato nella direzione giusta (verso la camera per i fiocchi, lungo la
caduta per le gocce). Le particelle che non servono le lascia chiuse: zero
pixel.

La CPU non tocca mai le 32.768 particelle: scrive una decina di numeri per frame.

### La Material Parameter Collection

`MPC_GeoWeather` è un insieme di parametri **globali**: una scrittura dal C++,
e tutti i materiali che la leggono la vedono. Le particelle ci leggono la fase e
la velocità, il terreno e le strade il bagnato e la quota della neve. È lo
stesso meccanismo delle onde (`MPC_GeoWater`, capitolo 13).

### Translucent e Unlit

Le particelle sono **traslucide** (si vede attraverso) e **unlit** (non
calcolano la luce: il colore è quello che il materiale emette). La luce di una
goccia conta poco, e i materiali traslucidi con la luce costano molto: per
migliaia di particelle che coprono lo schermo, unlit è la scelta di tutti i
giochi.

### La nebbia: la ExponentialHeightFog

La nebbia di Unreal è un attore (`AExponentialHeightFog`) con una densità che
cala con la quota. La precipitazione ne alza la densità e la rimette com'era
quando smette.

> ⚠️ **Trappola.** Nell'**editor** il livello è quello che si salva. Se la
> pioggia alzasse la densità della nebbia del livello e tu salvassi mentre
> piove, il livello resterebbe nebbioso per sempre. Per questo nell'editor la
> nebbia del livello non si tocca: si usa solo in Play (mondo copiato, che si
> butta) o se il livello non ne ha una (e allora se ne crea una **transient**,
> che non si salva).

### Il suono fatto dal codice: USoundWaveProcedural

Un `USoundWave` di solito viene da un file. `USoundWaveProcedural` è un suono i
cui campioni li dà il codice: si crea, si dice la frequenza (22.050 al secondo)
e i canali (uno), si mettono in coda i campioni a 16 bit (`QueueAudio`) e lo
si suona come qualunque altro suono (`UGameplayStatics::PlaySound2D`). Non è
una `UPROPERTY` di nessuno: il garbage collector potrebbe portarselo via
mentre suona, e il renderer lo tiene in vita finché dura
(`TStrongObjectPtr`).

### La luce del fulmine: una point light enorme, per un attimo

Un fulmine illumina il paesaggio: è una `UPointLightComponent` con un raggio di
15 km, senza ombre (costerebbero troppo), accesa per un terzo di secondo.
L'intensità è in **candele**: a un metro una candela dà un lux, a mille metri
un milionesimo. Per avere al suolo una volta e mezza il sole del livello, a
900 m di altezza, servono 1,5 × sole × 900² candele.

### Additive: la luce che si somma

Il materiale del canale è **Additive**: il suo colore si somma a quello che
c'è dietro, come la luce vera (il nero non cambia niente, il bianco schiarisce).
Con un emissivo molto alto, il **bloom** di Unreal fa l'alone attorno al filo.

### Un attore transient

`RF_Transient` è un'etichetta che dice a Unreal "questo oggetto non va
salvato". L'attore delle particelle e la nebbia creata da noi la portano:
esistono finché il mondo è aperto, e non finiscono mai in un file.

---

## Dove sta nel codice

| File | Cosa |
|---|---|
| `GeoRender/Public/Weather/Precipitation.h` | il modello (puro): mappa, tipo, visibilità, caduta, suolo, particelle |
| `GeoRender/.../Weather/GeoPrecipitationRenderer.*` | l'attore delle particelle, `MPC_GeoWeather`, la nebbia |
| `GeoRender/Public/Weather/Lightning.h` | i fulmini (puro): dove, il lampo, il canale, il tuono |
| `GeoRender/.../Weather/GeoLightningRenderer.*` | il canale, la luce, il tuono che suona |
| `GeoRender/.../Weather/GeoWeatherSubsystem.*` | il giro di un frame, il rebase, l'overlay, `GetPrecipitationAt`, `OnLightningStrike` |
| `GeoRender/Private/GeoRenderModule.cpp` | i comandi `geo.Weather.Rain`, `Snow`, `Hail`, `Storm`, `Strike`, ... |
| `GeoWorldEditor/Private/GeoTerrainMaterialFactory.cpp` | `M_GeoPrecipitation`, `M_GeoLightning` e il suolo bagnato/innevato |
| `Tools/StandaloneTests/geoweather_main.cpp` | i test (sezioni 6–13) |

---

## Riepilogo

- La precipitazione è una **mappa** di rovesci (rumore con una soglia data
  dalla copertura) che **viaggia** con il vento in quota.
- **Cosa** cade lo decide la **quota**: zero termico, quota neve 300 m sotto,
  una fascia mista; la grandine nei nuclei dei temporali.
- La **visibilità** viene dall'estinzione (Koschmieder): la neve nasconde dieci
  volte la pioggia.
- Il **suolo** si bagna e s'imbianca sopra una quota; si asciuga e fonde da
  solo.
- Le **particelle** sono una scatola che si ripete attorno alla camera, ferme
  nel mondo; la scheda video le apre e le muove, la CPU scrive dieci numeri.
- Lo spostamento **si somma** e si corregge al rebase: niente salti.
- I **fulmini** nascono nei nuclei dei temporali (Poisson diradato),
  sfarfallano (i colpi di ritorno), hanno un canale frattale; il **tuono**
  arriva dopo tre secondi per chilometro e brontola perché i pezzi del canale
  sono a distanze diverse. Sintetizzato, su un thread di lavoro.

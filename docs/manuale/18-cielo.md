# Capitolo 18 — Il cielo: il giorno e la notte

> Come si mette il sole al posto giusto per un luogo, un giorno e un'ora; come
> si calcola la luna con la sua fase; come si fa la notte in un motore che
> l'esposizione automatica vorrebbe sempre giorno. Il documento di progetto è
> `docs/cielo-design.md`; come provarlo è in `docs/cielo-verifica.md`.

---

## L'idea in breve

Fino a qui il sole era quello del livello: fermo, a un'ora che nessuno aveva
scelto. Le ombre degli edifici e delle montagne non dicevano niente.

Il cielo aggiunge un **orologio**. A ogni frame calcola dove sono il sole e la
luna **visti dalla camera**, e da lì:

1. gira la **luce del sole** del livello verso il sole vero;
2. di notte accende una **luce della luna**, con la luce della sua fase;
3. abbassa l'**esposizione** di notte, perché la notte resti notte;
4. disegna **stelle, Via Lattea e luna** con le fasi;
5. accende le **finestre** degli edifici, tante la sera e poche all'alba.

L'astronomia sta nello strato puro e si prova con gli esempi di un libro;
Unreal fa il resto (la SkyAtmosphere colora il cielo da sola).

---

## 18.1 Il tempo degli astronomi

Un calendario è scomodo per i calcoli: mesi di lunghezza diversa, anni
bisestili. Gli astronomi contano i **giorni giuliani**: un numero che cresce
di 1 al giorno da un giorno del 4713 a.C., e comincia a **mezzogiorno**. Il
1 gennaio 2000 alle 12 è 2.451.545,0. La differenza fra due istanti è una
sottrazione.

Due "tempi" diversi:

- il **Tempo Universale** (UT) segue la rotazione della Terra: è quello degli
  orologi (più il fuso);
- il **Tempo Terrestre** (TT) è uniforme, quello degli orologi atomici. Il
  moto del sole e della luna si calcola in TT.

La Terra rallenta: oggi TT − UT ≈ 69 secondi (si chiama ΔT). Per la luna,
che si sposta di mezzo grado l'ora, 69 secondi sono un centesimo di grado.

**Il tempo siderale.** In un giorno solare la Terra fa un giro e un po' (deve
"riprendere" il sole, che si è spostato di un grado sull'eclittica); rispetto
alle stelle un giro dura 23 h 56 min. Il tempo siderale dice quanto è girata
la Terra rispetto alle stelle: serve per sapere dove sono gli astri, e per
far girare il cielo stellato.

> 💡 **Esempio.** Il test confronta il tempo siderale del 10 aprile 1987 alle
> 0 UT con il libro di Meeus: 197,693195°. Il codice dà lo stesso numero alla
> sesta cifra.

---

## 18.2 Dove è il sole

Il sole sembra girare attorno alla Terra su un cerchio inclinato di 23,44°
sull'equatore: l'**eclittica**. La sua posizione su quel cerchio
(la **longitudine eclittica**) avanza di circa un grado al giorno, un po' più
in fretta a gennaio (la Terra è più vicina al sole) che a luglio.

Il calcolo, in quattro passi:

1. la longitudine **media** del sole (un moto uniforme) e l'**anomalia media**
   (dove è la Terra sulla sua ellisse);
2. l'**equazione del centro**: la correzione per l'ellisse, fino a 1,9°;
3. le piccole correzioni che fanno la posizione **apparente**: l'aberrazione
   (la luce viaggia, la Terra si muove: 20″) e la nutazione (l'asse della
   Terra oscilla: fino a 17″);
4. dall'eclittica all'**equatore**: ascensione retta (la "longitudine del
   cielo") e declinazione (la "latitudine del cielo").

Poi **dal cielo all'orizzonte**: con il tempo siderale locale e la latitudine
del posto, l'**azimut** (dal nord verso est) e l'**altezza** sull'orizzonte.
Infine la **rifrazione**: l'aria piega la luce, e un astro all'orizzonte
sembra mezzo grado più alto di dove è. Al tramonto, quando il sole "tocca"
l'orizzonte, è già sotto.

> 💡 **Esempio.** Torino, 21 giugno: il mezzogiorno vero (il sole a sud) è
> alle 11:31 UT, cioè le 13:31 con l'ora legale, e il sole è alto 68,4°
> (90° − 45,07° di latitudine + 23,44° di declinazione). Il sole sorge alle
> 5:42 e tramonta alle 21:20: 15 ore e 38 minuti di giorno.

---

## 18.3 Dove è la luna, e che faccia ha

La luna è più difficile: il sole la tira, la Terra la tira, e la sua orbita
cambia di continuo. Meeus dà la sua posizione come una somma di **seni**:
120 termini, ognuno un multiplo di quattro angoli che girano (l'elongazione
dal sole, le anomalie della Terra e della luna, la distanza dal nodo). Il
primo vale 6,3°; l'ultimo un decimillesimo.

> 💡 **Esempio.** Nel test, per il 12 aprile 1992, la somma dei 60 termini
> della longitudine fa −1.127.527 milionesimi di grado: esattamente il numero
> del libro. Un solo coefficiente sbagliato si vedrebbe.

**La parallasse.** La luna è vicina (384.000 km): vista dalla superficie è
più in basso che vista dal centro della Terra, fino a un grado all'orizzonte.
Il codice lo fa con i vettori: la direzione della luna per la sua distanza,
meno il raggio della Terra verso lo zenit.

> 💡 **Esempio.** L'eclisse totale dell'8 aprile 2024, alle 18:17 UT in
> Messico: il codice mette sole e luna a 0,007° l'uno dall'altra. Dal centro
> della Terra sarebbero a 0,35°: senza la parallasse l'eclisse non ci sarebbe.

**La fase.** Quanto disco è illuminato dipende dall'angolo fra il sole e la
luna visti dalla Terra (l'**elongazione**): 0 alla luna nuova, 180° alla
piena. La luce che arriva cala molto più della parte illuminata: al primo
quarto è un decimo della piena, non la metà (le ombre delle montagne lunari,
la **legge di Allen**).

---

## 18.4 Levate e tramonti: cercare invece di risolvere

Per la levata del sole c'è una formula chiusa (l'angolo orario per cui
l'altezza vale −0,833°). Per la luna no: in un giorno si sposta di 13°.

Il codice fa una cosa più semplice e generale: **campiona** l'altezza ogni
10 minuti, e dove cambia segno **stringe** con la bisezione fino al secondo.
Funziona uguale per il sole, i crepuscoli (−6°, −12°, −18°) e la luna, e
quando non c'è il tramonto (Tromsø d'estate) semplicemente non trova niente.

---

## 18.5 Dal cielo al mondo di Unreal

Gli assi del mondo sono il **nord, l'est e l'alto dell'origine** (capitolo
2), e l'origine si sposta con il rebasing. Il sole si calcola nel cielo
locale **della camera** (azimut e altezza → un versore nord-est-alto), poi lo
si porta nel mondo con la rotazione della georeferenziazione in quel punto.
A 10 km dall'origine l'"alto" della camera e quello dell'origine differiscono
di un decimo di grado: senza questo passaggio, il sole si sposterebbe a ogni
rebase.

**Le stelle** girano attorno al polo celeste. Il cielo passa al materiale la
rotazione dal mondo al **cielo equatoriale** (tre righe, in
`MPC_GeoWeather`): il materiale disegna le stelle sull'equatoriale, e girano
da sole un giro ogni giorno siderale.

---

## 18.6 La notte in un motore che vuole il giorno

Fra il sole a mezzogiorno e la luna piena ci sono 18 **stop** (un fattore
400.000). L'**esposizione automatica** di Unreal fa come l'occhio: apre il
diaframma quando c'è poca luce. Ma ne recupera una decina: con la luna vera
l'immagine sarebbe nera.

La soluzione dei giochi, qui come scelta dichiarata:

- la luna piena vale **1/1000** del sole (10 stop): l'esposizione si adatta;
- di notte un **Post Process Volume** del cielo, senza confini, abbassa
  l'esposizione di **2 stop**, pesato da quanto è notte: la scena resta
  scura e blu, si vede quanto basta;
- quello che ha una luce sua (le **finestre**, le **stelle**, la **luna**)
  si misura sul sole del livello e spicca.

> ⚠️ **Trappola.** Quello che il motore non illumina non sa che è notte: la
> pioggia (particelle unlit) sarebbe una cascata di strisce bianche, la
> nebbia del livello un muro luminoso. Il cielo dice a tutti e due quanta
> luce c'è.

---

## Alternative considerate

| Idea | Perché no |
|---|---|
| Il plugin Sun Position Calculator del motore | niente luna né stelle, non sa del rebasing |
| Ruotare il sole di 15° all'ora | sbagliato fuori dall'equatore e dagli equinozi: d'estate a Torino il sole sorge a nord-est |
| Spegnere il sole al tramonto | il crepuscolo sparirebbe di colpo |
| La luna fisica | 18 stop: notte nera |
| Le stelle di un catalogo | un passo futuro, sopra questo: la griglia e la rotazione ci sono già |

---

## Nota Unreal

### La SkyAtmosphere e le "luci dell'atmosfera"

La **SkyAtmosphere** simula l'aria: la luce del sole che si diffonde (il blu
del cielo), che si attenua attraversandola (il rosso al tramonto), e la Terra
che la copre quando il sole va sotto. Lo fa per le luci direzionali marcate
**Atmosphere Sun Light**, fino a due: indice 0 il sole, indice 1 (qui) la
luna. Per questo il cielo **non tocca l'intensità** del sole: basta girarlo.

### Girare una luce costa

Le **Virtual Shadow Maps** tengono in memoria le ombre di quello che non
cambia. Girare la luce le invalida tutte. Il cielo gira il sole solo quando
la sua direzione cambia di più di un centesimo di grado: a velocità normale
ogni due secondi circa, a tempo accelerato ogni frame (e allora costa).

### Il Post Process Volume senza confini

Un **Post Process Volume** cambia le impostazioni della camera (esposizione,
colore, bloom) dentro il suo volume; con **Unbound** vale ovunque. Ogni
impostazione ha un "override": solo quelle accese contano, e il **Blend
Weight** mescola il suo valore con quello sotto. Qui: solo la correzione
dell'esposizione, con il peso di quanto è notte.

### Un trasparente additivo, lontano

La sfera delle stelle è una **static mesh** (la sfera del motore, scalata a
100 km) con un materiale **Additive**: aggiunge luce a quello che c'è dietro
(il cielo dell'atmosfera) invece di coprirlo. Due facce, perché la camera sta
dentro; senza nebbia ("Apply Fogging" spento), se no a 100 km le stelle
sparirebbero; "Responsive AA" perché l'antialiasing temporale non le
cancelli (sono grandi come un pixel). Si sposta con la camera a ogni frame.

### Una luce creata da codice

La luna è un `ADirectionalLight` creato con `SpawnActor` e la bandiera
**RF_Transient**: esiste nel mondo, ma non viene mai salvata nel livello né
copiata nel Play. Lo stesso per la sfera e il volume dell'esposizione.

---

## Dove sta nel codice

| File | |
|---|---|
| `GeoRender/Public/Sky/Astronomy.h` | giorni giuliani, tempo siderale, nutazione, sole, luna, orizzonte, parallasse, fase, levate e tramonti (puro) |
| `GeoRender/Public/Sky/DayLight.h` | l'ora locale (ora legale), la luce del giorno e della notte, le finestre (puro) |
| `GeoRender/Public/Sky/NightSkyShader.inl` | stelle, Via Lattea, luna: HLSL e C++ |
| `GeoRender/Public/Buildings/BuildingShader.inl` | `GEO_BUILDING_LIGHTS`: le finestre accese |
| `GeoRender/Public/Sky/GeoSkySubsystem.h` | Unreal: l'orologio, il sole, la luna, l'esposizione, la nebbia, la sfera delle stelle |
| `GeoRender/Public/Sky/GeoSkySettings.h` | Project Settings → Plugins → GeoWorld - Cielo |
| `GeoWorldEditor/Private/GeoTerrainMaterialFactory.cpp` | i parametri `Sky*` di `MPC_GeoWeather`, `M_GeoNightSky`, le luci di `M_GeoBuilding` |
| `Tools/StandaloneTests/geosky_main.cpp` | i test (gli esempi di Meeus, l'eclisse del 2024) |

---

## Riepilogo

- Il tempo degli astronomi è il **giorno giuliano**; i pianeti si muovono nel
  **TT**, la Terra gira nell'**UT** (ΔT ≈ 69 s); le stelle col **tempo
  siderale**.
- Il **sole** dall'eclittica all'equatore all'orizzonte, con aberrazione,
  nutazione e **rifrazione**; la **luna** con 120 termini, la **parallasse**
  e la **fase** (la legge di Allen).
- **Levate e tramonti** si cercano campionando e stringendo: funziona per
  tutto, anche dove il sole non tramonta.
- Il sole va nel cielo locale **della camera** e poi nel mondo: niente salti
  al rebase.
- La **notte**: luna a 1/1000 del sole, esposizione −2 stop pesata dal buio,
  e quello che non è illuminato (pioggia, nebbia) scalato a mano.
- **Stelle, Via Lattea, luna con le fasi**: un trasparente additivo su una
  sfera di 100 km; **finestre accese** dall'ora e dal buio.

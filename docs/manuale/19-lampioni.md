# Capitolo 19 — I lampioni: le luci delle strade

> Come si mettono decine di migliaia di lampioni lungo le strade senza un
> dataset che dica dove sono; come si decide quando si accendono; come si fa
> vedere una città di notte da otto chilometri senza accendere ottomila luci.
> Il documento di progetto è `docs/lampioni-design.md`; come provarli è in
> `docs/lampioni-verifica.md`.

---

## L'idea in breve

Con il cielo (capitolo 18) è arrivata la notte, e con la notte le finestre
accese. Le strade però restavano buie: una città di notte, vista dall'alto, è
soprattutto **file di punti** lungo le vie, e da vicino **pozze di luce**
sull'asfalto.

Nessuno pubblica dove sono i lampioni d'Italia. Ma le strade le abbiamo già
(capitolo 12), e i lampioni stanno lungo le strade, con regole precise: a che
distanza l'uno dall'altro, da che lato, quanto alti. I lampioni si
**ricostruiscono** dalle strade:

1. **quali strade**: il tag `lit` di OSM se c'è; se no la classe, e quanto è
   abitato attorno;
2. **dove**: a passo fisso lungo la strada, sul marciapiede, mai in mezzo a
   un'altra strada;
3. **quando**: una fotocellula per circuito, che accende dopo il tramonto;
4. **come si vedono**: un puntino per lampione fino a 8 km, i pali fino a
   700 m, qualche decina di luci vere sui più vicini.

---

## 19.1 Quali strade

OpenStreetMap ha il tag `lit` (`yes`, `no`, o un orario). La pipeline lo
mette in un byte della tile delle strade che prima era riservato e sempre
zero: le tile vecchie si leggono come "non si sa", e funzionano lo stesso.

Il tag però c'è su poche strade. Per le altre servono **regole**:

- le **autostrade** sono buie (in Italia solo gli svincoli in città, le
  barriere, le gallerie hanno luce);
- le **residenziali** hanno i lampioni quasi sempre: anche una frazione di
  dieci case ha i suoi;
- le **strade principali** li hanno dentro il paese, non in campagna;
- **sentieri e sterrati** solo se il tag lo dice.

"Dentro il paese" va misurato. Un'idea semplice: dove si abita ci sono le
**strade di casa** (residenziali, pedonali, "living street"), tante e fitte.
Si conta la loro lunghezza attorno a ogni punto (in un quadrato di 500 m) e si
divide per l'area: chilometri di strada per chilometro quadrato.

> 💡 **Esempio.** Il paese di prova dei test ha cinque vie da ovest a est e
> quattro da nord a sud, lunghe 500–600 m: 15,5 km/km². In campagna, a un
> chilometro dal paese: 0. La soglia per le strade principali è 3, per le
> residenziali 1.

La densità si guarda **lampione per lampione**, non strada per strada. Una
provinciale è spesso una sola linea lunga chilometri: con il suo punto di
mezzo in campagna sarebbe tutta buia, anche dove attraversa il paese.

---

## 19.2 Dove: camminare lungo la strada

Il lampione nasce camminando lungo la linea: il primo a **mezzo passo**
dall'inizio, poi uno ogni passo (26 m in una via di quartiere, 34 su una
provinciale: circa 3,5 volte l'altezza del palo, come nei progetti veri).

> ⚠️ **Trappola: le strade a pezzi.** In OSM una via è spezzata a ogni
> incrocio, a ogni cambio di nome o di senso unico. Camminando pezzo per pezzo
> ogni pezzo avrebbe il suo lampione a mezzo passo: una via spezzata in dieci
> pezzi da 15 m ne avrebbe dieci, uno ogni 15 m. Prima si **ricuciono** le
> linee che si toccano solo fra loro due (una "catena"), poi si cammina. Nei
> test: cinque lampioni a 26 m.

**Il confine fra due tile.** Una strada attraversa il confine: le linee escono
dalla tile di un buffer, e tutte e due le tile la vedono. Se tutte e due
mettessero i lampioni sul tratto in comune, sul confine ce ne sarebbero due
attaccati. Ogni tile cammina solo i tratti **dentro di sé**, ricominciando a
mezzo passo dal bordo: fra l'ultimo di una e il primo dell'altra c'è sempre
fra mezzo passo e un passo e mezzo.

**I lati.** Le strade strette hanno i lampioni da un lato solo, le medie
alternati, le larghe a coppie. Il palo sta sul marciapiede, a mezzo metro dal
cordolo; lo **sbraccio** guarda la strada.

> ⚠️ **Trappola: gli angoli.** Due vie che si incontrano ad angolo, e solo
> loro, diventano una catena: la camminata gira di 90 gradi. L'ultimo palo
> prima dell'angolo, spostato di lato, finiva **sulla carreggiata dell'altro
> braccio della stessa via**. Il controllo "mai su un'altra strada" saltava la
> propria catena, e non lo vedeva. Il test che verifica che nessun palo stia
> su una carreggiata l'ha trovato al primo giro: ora la catena si guarda
> anche lei, tranne il pezzo attorno al lampione.

---

## 19.3 Quando si accendono

Un lampione vero non sa che ore sono: ha una **fotocellula** che misura la
luce e accende sotto una soglia, fra 15 e 70 lux. I lampioni di una via sono
sullo stesso **circuito**, e si accendono insieme; un'altra via, con un'altra
fotocellula, qualche minuto dopo.

Qui la luce che vede una fotocellula viene dalla stessa formula del cielo
(`SunlightShare`, capitolo 18): 440 lux al tramonto, ~50 con il sole a 3
gradi sotto l'orizzonte. Ogni circuito (una cella di ~300 m) ha la sua soglia
fra 15 e 60 lux.

> 💡 **Esempio.** Torino, 21 giugno 2026: il sole tramonta alle 21:33. Il
> circuito più sensibile (60 lux) si accende alle 21:47, il meno sensibile (15
> lux) alle 22:00. Con il cielo coperto la luce è un quarto, e si accendono
> subito.

**L'isteresi.** La mattina una fotocellula spegne a una luce più alta di
quella a cui ha acceso (qui 1,6 volte): altrimenti all'alba, con la luce che
oscilla attorno alla soglia, il lampione lampeggerebbe.

**La riduzione del flusso.** Molti comuni, nel cuore della notte, abbassano
la luce di un terzo: qui del 30% fra le 00:30 e le 04:30.

---

## 19.4 Quanta luce

Come per la luna, **non quella vera**. Sotto un lampione ci sono 15 lux, la
luna piena ne dà 0,25: sessanta volte tanto. Ma la luna di GeoWorld è già 400
volte quella vera (capitolo 18: l'esposizione automatica non regge 18 stop);
con il rapporto vero le strade sarebbero bianche. Qui al piede di un palo c'è
**1/50 del sole del livello**, venti volte la luna piena di GeoWorld.

Il colore: le lampade al **sodio** (arancioni) o i **LED** (bianchi). Un
comune è passato ai LED o no: tre su quattro sì. I LED delle strade principali
sono neutri (4000 K), quelli dei quartieri caldi (3000 K).

---

## 19.5 Tre cose a schermo

**I punti luce.** Fino a 8 km di lampioni sono decine di migliaia. Una luce
vera per ognuno è impossibile. Ma da lontano un lampione è **un puntino**: un
quadratino che il materiale gira verso la camera e tiene largo almeno due
pixel e mezzo. Si disegna **additivo** (aggiunge luce a quello che c'è
dietro), con un nucleo e un alone, e più debole per pixel quando è lontano.

> ⚠️ **Trappola: il terreno lontano.** Il terreno a qualche chilometro è
> disegnato con pochi triangoli: un dosso può stare qualche metro più in alto
> di dove sarebbe. Un lampione alto 8 m ci finirebbe dentro, e sparirebbe. Il
> materiale avvicina il punto alla camera di un paio di "punti": di più quando
> è lontano (e il terreno più grossolano), niente da vicino.

**I pali.** Fino a 700 m, tre forme fatte da codice (sbraccio, palo,
lanterna), come gli alberi: un componente di istanze per forma e per cella. Il
vetro della lampada si accende con la stessa formula del punto luce.

**Le luci vere.** Le pozze di luce sull'asfalto le fanno le luci di Unreal:
48 spot verso il basso, sui lampioni accesi **più vicini**. Quando la camera si
muove la lista cambia: per non far scattare la luce, chi sta in fondo alla
lista pesa poco, e ogni luce sfuma in quattro decimi di secondo.

---

## Alternative considerate

| Idea | Perché no |
|---|---|
| Una luce vera per lampione | decine di migliaia di luci |
| Lampioni solo dove OSM ha `lit=yes` | quasi tutta l'Italia sarebbe buia |
| "Dove si abita" dalla copertura del suolo (ESA WorldCover, capitolo 16) | un secondo dataset da aprire; la rete delle strade dice già dove si abita |
| Le strade illuminate dipinte nella texture delle strade | una seconda texture per ogni tile di terreno, e la luce cambierebbe con il dettaglio |
| Le pozze di luce come decal | un componente per lampione, migliaia sul game thread |
| I punti luce con Niagara | un sistema in più; un componente di istanze con un materiale basta, e lo shader si prova come C++ |

---

## Nota Unreal

### Un quadratino che si apre verso la camera

Il World Position Offset sposta i vertici nello shader dei vertici. Il
quadratino è quasi un punto (5 cm): i suoi quattro angoli hanno lo stesso
posto, e un colore diverso (0 o 1 in rosso e verde). Lo shader legge l'angolo
dal colore e sposta ogni vertice lungo la **destra** e l'**alto** della
camera: il quadrato è sempre di fronte a chi guarda (un *billboard*). Il
motore però decide se un oggetto è a schermo dai suoi **bounds**: quelli di un
quadrato di 5 cm, e il punto sparirebbe quando il centro esce dallo schermo.
I bounds della mesh si allargano a mano (`SetPositiveBoundsExtension`).

### I dati per istanza

Un componente di istanze (HISM) disegna migliaia di copie della stessa mesh
con un solo comando. Ogni copia può portare qualche numero suo (*Per Instance
Custom Data*): qui il colore della lampada e la soglia della sua fotocellula.
Il materiale li legge, e ogni lampione si accende quando vuole lui, senza che
il C++ tocchi niente al tramonto.

### Le candele

Le luci di Unreal accettano diverse unità. In **candele** il conto è quello del
libro di fisica: un punto da I candele illumina con I / d² lux una superficie
a d metri. Sotto un palo alto H, per avere E lux, I = E × H².

### Le MegaLights

Ogni luce di Unreal costa: illumina ogni pixel nel suo raggio, e con l'ombra
ridisegna la scena dal suo punto di vista. Le **MegaLights** (da Unreal 5.5)
cambiano il conto: il costo dipende poco dal numero di luci. Con le MegaLights
accese nel progetto si possono dare luci vere a centinaia di lampioni
(`geo.Lights.Count 300`).

---

## Dove sta nel codice

| File | |
|---|---|
| `GeoRender/Public/Roads/StreetLights.h` | quali strade, la densità, le catene, gli ostacoli, i lampioni di una tile e di una cella, l'accensione, la luce, le luci vere, le forme (puro) |
| `GeoRender/Public/Roads/StreetLightShader.inl` | il punto luce (che si apre verso la camera, il bagliore) e il vetro dei pali: HLSL e C++ |
| `GeoRender/Public/Roads/GeoStreetLightsSubsystem.h` | Unreal: le celle, i componenti, le luci vere, `MPC_GeoWeather`, l'overlay |
| `GeoWorldEditor/Private/GeoTerrainMaterialFactory.cpp` | `M_GeoStreetLight`, `M_GeoStreetLamp`, `StreetLights` e `StreetLightView` in `MPC_GeoWeather` |
| `GeoTiles/Public/Tiles/VectorClasses.h` | `RoadLitUnknown`, `RoadLitYes`, `RoadLitNo` |
| `Pipeline/geoworld/roadclasses.py` | `lit_state`: il tag `lit` |
| `Tools/StandaloneTests/georoads_main.cpp` | i test (sezioni 13 e 14) |

---

## Riepilogo

- I lampioni si **ricostruiscono dalle strade**: il tag `lit` se c'è, se no la
  classe e la **rete di strade di casa** attorno (km per km²), lampione per
  lampione.
- Si cammina lungo la strada **ricucita**: il primo a mezzo passo, poi uno ogni
  passo; ogni tile solo i suoi tratti; mai su un'altra carreggiata, nemmeno
  agli angoli della propria.
- Si accendono con una **fotocellula** per circuito (15–60 lux), un quarto
  d'ora dopo il tramonto; spengono la mattina a una luce più alta (isteresi);
  di notte il flusso scende del 30%.
- La luce è in proporzione al **sole del livello** (1/50 al piede di un palo),
  come la luna; sodio o LED per comune.
- A schermo: **punti luce** fino a 8 km (un quadratino aperto verso la camera,
  additivo), **pali** fino a 700 m, **48 luci vere** sui più vicini, che
  sfumano entrando e uscendo.

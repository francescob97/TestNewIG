# Il programma: cosa fa ogni fase, e dove siamo

## In una riga

Stiamo costruendo un motore che disegna la superficie terrestre in Unreal
caricando da disco solo i pezzi che servono, alla risoluzione che serve.

Le sei fasi si dividono in due gruppi: **quattro costruiscono la macchina** e
**due preparano e vestono i dati**.

---

## Le fasi

| Fase | Cosa fa | Input | Output | Stato |
|---|---|---|---|---|
| **1** | Geodesia e georeferenziazione: dove sta un punto sul pianeta e dove finisce nello spazio di Unreal | — | codice C++ | **fatta** *(issue #1 aperta)* |
| **2** | Pipeline dati **DEM**: da GeoTIFF sparsi a piramide di tile di **quote** | TINITALY 1.1 | file su disco | **fatta** |
| **3** | Loader asincrono e cache LRU: leggere quei file senza bloccare il game thread | tile di Fase 2 | codice C++ | da fare |
| **4** | Quadtree e LOD: decidere *quali* tile servono in questo istante | — | codice C++ | da fare |
| **5** | Mesh: trasformare le quote in triangoli, con le skirt contro le crepe | tile + quadtree | **terreno visibile** | da fare |
| **6** | Imagery: drappeggiare le **ortofoto** sopra la geometria | ortofoto | **terreno fotografico** | da fare |

**Il primo momento in cui vedi del terreno e' la fine della Fase 5.** Prima di
allora vedi solo cubi di prova e numeri sull'HUD. E' voluto: ogni fase produce
qualcosa di verificabile, ma solo la quinta produce qualcosa di *guardabile*.

---

## Perche' le quote prima e le foto dopo

Sono due dati completamente diversi e uno dipende dall'altro.

**Il DEM (Digital Elevation Model)** e' la **forma** del terreno: un numero per
punto, l'altezza. E' la struttura portante. Determina la geometria, cioe' dove
stanno i triangoli.

**L'ortofoto** e' il **colore**: una fotografia aerea raddrizzata. E' una texture
che si appiccica sopra la geometria.

Senza geometria non c'e' dove appiccicare la texture. Ma c'e' una ragione piu'
sostanziale per quest'ordine: **il DEM e' la parte difficile**. Le crepe fra
livelli di dettaglio diversi, le skirt, la selezione LOD, il rebasing — sono
tutti problemi della geometria. L'imagery, una volta che il quadtree esiste,
riusa la stessa identica struttura: stesso schema di tiling, stesso loader,
stessa cache. Se avessimo fatto le foto per prime, avremmo dovuto rifarle dopo
aver capito come funziona la geometria.

---

## Cosa ha prodotto concretamente la Fase 2

**Solo quote. Nessuna immagine.**

Sei preso TINITALY 1.1: decine di GeoTIFF in proiezione UTM 32N, ognuno grande
quanto capita, con le quote riferite al livello del mare. Un raster monolitico
che un motore 3D non puo' usare cosi' com'e', per tre motivi.

1. **Troppo grande.** Non ci sta in RAM e non lo si puo' caricare tutto.
2. **Una risoluzione sola.** Una montagna a 300 km di distanza non ha bisogno
   di un punto ogni 10 metri: leggerli tutti sarebbe spreco puro.
3. **Quote sbagliate per noi.** TINITALY da' quote *ortometriche* (sopra il
   livello del mare); il motore lavora su un ellissoide e ha bisogno di quote
   *ellissoidiche*. In Italia differiscono di circa 48 metri.

La pipeline risolve tutti e tre:

```
TINITALY                          Piramide di tile
(N GeoTIFF, UTM32N,               <root>/manifest.json
 quote ortometriche)              <root>/8/273/68.ght      <- il mondo, grossolano
        |                         <root>/9/547/136.ght
        v                         ...
   [6 stadi]        ------>       <root>/14/17525/4389.ght <- 9.5 m per punto
                                  <root>/14/index.bin
```

Ogni file `.ght` e' **66.596 byte**: 129x129 quote in float32 piu' un header di
32 byte. Sempre la stessa dimensione, a qualunque livello. Il runtime chiedera'
"dammi il tile (14, 17525, 4389)" e ricevera' 66 KB in una lettura sola.

Livello 8 = tutta l'area con pochi punti. Livello 14 = risoluzione nativa del
dato. Ogni livello ha quattro volte i tile del precedente e quattro volte il
dettaglio.

---

## Cosa succedera' nella Fase 6 (le ortofoto)

Stessa identica struttura, dato diverso:

* stesso schema di tiling (livello 0 = 2x1, quadtree geografico WGS84);
* tile di colore invece che di quota — probabilmente JPEG o PNG invece di
  float32 grezzo, perche' le immagini si comprimono bene e le quote no;
* la pipeline Python guadagna un secondo percorso, che condivide riproiezione,
  piramide e manifest con quello delle quote;
* a runtime, il quadtree carica **due** tile per nodo: quota e colore. Possono
  anche essere a livelli diversi — e' normale avere l'imagery piu' dettagliata
  della geometria, o viceversa.

Le sorgenti candidate per l'Italia (da verificare licenze e disponibilita'
quando ci arriviamo, non ho modo di controllarle da qui): ortofoto nazionali via
i servizi del Geoportale Nazionale, oppure Sentinel-2 del programma Copernicus
a 10 m, che ha il vantaggio di essere libero e di corrispondere bene alla
risoluzione di TINITALY.

---

## Riepilogo dei formati fissati finora

| | |
|---|---|
| Ellissoide | WGS84 |
| Tiling | geografico WGS84, livello 0 = 2x1, livello L = 2^(L+1) x 2^L tile |
| Tile di quota | 129x129 post float32, registrati sui nodi (1 post di bordo condiviso) |
| Percorso | `<root>/<level>/<x>/<y>.ght` |
| Indice | `<root>/<level>/index.bin` — quali tile esistono, con min/max quota |
| Manifest | `<root>/manifest.json` — metadati globali |
| Quote | ellissoidiche WGS84 |
| Assi in Unreal | North -> +X, East -> +Y, Up -> +Z |
| Unita' | 1 unita' Unreal = 1 cm |

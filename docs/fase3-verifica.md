# Fase 3 — Come verificare che funzioni

## A. Senza Unreal — 1 secondo

```bat
cmake -S Plugins/GeoWorld/Tools/StandaloneTests -B build
cmake --build build
build\Debug\geotiles_tests.exe dataset\test
```

L'argomento e' la cartella di un dataset prodotto dalla Fase 2. Senza, i test
che ne hanno bisogno vengono saltati invece di fallire.

Attesi **23 test verdi**:

| Gruppo | Cosa dimostra |
|---|---|
| Schema contro i vettori Python | l'implementazione C++ e quella della pipeline concordano su livelli, estremi delle tile e ricerca da lon/lat |
| Invarianti dello schema | il post 128 di una tile e' il post 0 della successiva; padre e figlio condividono l'angolo; 1600 chiavi adiacenti senza collisioni di hash |
| Cache LRU | sfratta la meno usata; `Find` protegge dalla prossima evizione; una tile pinnata sopravvive a sette inserimenti con budget per due; un riferimento gia' preso resta valido dopo lo sfratto |
| Dataset reale | indici ordinati, tile decodificabili, min/max dell'header uguali all'indice, e **giunzioni verificate leggendo i file** |

L'ultima riga e' quella che vale: le giunzioni erano gia' verificate dalla Fase 2,
ma **dal lato di chi le scrive**. Qui le verifica il codice C++ che le legge
davvero, che e' quello che poi girera' nel motore.

---

## B. In Unreal — la parte che si vede

Prerequisito: un dataset generato dalla Fase 2, per esempio

```bat
cd Pipeline
python run.py fetch --area test -o dati\copernicus
python run.py build -i "dati\copernicus\*.tif" -o D:\geoworld\test
```

Poi, in un livello qualunque, apri la console con **`** e digita:

```
geo.Tiles.Demo D:\geoworld\test
```

Un comando solo. Apre il dataset, ti porta sopra il centro a una quota da cui si
vede l'insieme, chiede un riquadro di tile e accende overlay e disegno.

### Cosa devi vedere

**1. L'overlay in alto a sinistra.**

```
--- GeoWorld | Fase 3: streaming delle tile ---
Dataset    : Copernicus DEM GLO-30   livelli 0..13
Tile in cache : 81   (pinnate 0)
Memoria cache : 5.2 / 256 MB   (2%)
Tasso di hit  : 0.0%   evizioni 0
In caricamento: 0
Tempo medio   : 0.43 ms per tile
```

**2. Scatole colorate sospese nel vuoto.** Sono i volumi delle tile residenti:
un colore per livello, con etichetta `L10 1094/273` e l'intervallo di quote. Non
e' terreno — il terreno arriva in Fase 5 — sono **i bounding volume** che la
Fase 4 usera' per il culling. Vederli adesso serve proprio a poterli verificare
prima di dipenderci.

I lati seguono la curvatura terrestre: su una tile grande la differenza fra un
lato curvo e uno rettilineo e' di chilometri.

**3. Le scatole devono stare dove ti aspetti.** Se hai i marker della Fase 1
attivi (`geo.SpawnMarkers`), le scatole devono contenerli. Se sono altrove, il
problema e' nella georeferenziazione, non nello streaming.

### Prove da fare

**Caricare a risoluzione piena.**
```
geo.Tiles.LoadAround 13 4
geo.Tiles.Stats
```
Un riquadro 9x9, cioe' fino a 81 tile. Guarda `Tempo medio`: sono i millisecondi
di lettura e decodifica per tile. **Il gioco non deve scattare** mentre
succede: e' l'intera ragione per cui il caricamento sta su un thread pool.

**Far lavorare la cache.**
```
geo.Tiles.Budget 8
geo.Tiles.LoadAround 13 6
geo.Tiles.Stats
```
Con 8 MB ci stanno ~126 tile, ma ne stai chiedendo 169. Devi vedere `evizioni`
salire e la memoria fermarsi al budget. Poi rialza:
```
geo.Tiles.Budget 256
```

**Vedere il tasso di hit.**
```
geo.Tiles.Clear
geo.Tiles.LoadAround 12 2
geo.Tiles.LoadAround 12 2
geo.Tiles.Stats
```
La seconda volta le tile ci sono gia': il tasso di hit deve salire e nessun
caricamento deve partire.

**Chiedere qualcosa che non esiste.**
```
geo.Tiles.Load 13 45.0 9.0
```
Fuori dal dataset di prova. Deve rispondere `non esiste nel dataset` — **senza**
toccare il disco, perche' la risposta viene dall'indice.

### Comandi

```
geo.Tiles.Demo <cartella> [livello]   fa tutto: apri, vai, carica, mostra
geo.Tiles.Open <cartella>             apre un dataset
geo.Tiles.Info                        livelli, quote, indici in memoria
geo.Tiles.Stats                       cache, hit rate, tempi
geo.Tiles.LoadAround [liv] [raggio]   carica un riquadro attorno alla camera
geo.Tiles.Load [liv] [lat] [lon]      carica una tile sola
geo.Tiles.Debug <0|1>                 overlay statistiche
geo.Tiles.Draw <0|1>                  volumi delle tile nel mondo
geo.Tiles.Budget <MB>                 budget della cache a caldo
geo.Tiles.Clear                       svuota cache e statistiche
```

---

## C. Cosa NON e' verificato

* **Il C++ non e' mai stato compilato dentro Unreal.** L'ambiente di sviluppo e'
  Linux senza il motore. Matematica e formati sono verificati numericamente, le
  convenzioni UE staticamente, ma al primo build aspettati errori di
  compilazione da sistemare.
* **Nessuna misura di prestazioni reale.** I tempi di caricamento che vedrai
  nell'overlay saranno i primi numeri veri. Su Windows, con l'antivirus attivo
  su centinaia di migliaia di file piccoli, possono essere molto peggiori che
  altrove: vale la pena escludere la cartella del dataset dalla scansione.
* **Nessuno chiede tile automaticamente.** `LoadAround` e' un sostituto
  provvisorio del quadtree, non la logica definitiva.
* **La priorita' delle richieste non e' mai stata messa sotto stress**: con
  poche decine di tile la coda si svuota comunque.

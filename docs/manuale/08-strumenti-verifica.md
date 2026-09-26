# Capitolo 8 — Test e controlli automatici

> Come si verifica un motore per Unreal senza avere Unreal. È il capitolo che
> spiega perché la maggior parte degli errori è stata trovata prima di arrivare
> sulla tua macchina — e perché alcuni ci sono arrivati lo stesso.

---

## L'idea in breve

Il codice di questo progetto è stato scritto in un ambiente **senza Unreal
Engine**: Linux, senza il motore, senza Visual Studio. Non si poteva compilare il
ponte Unreal, né vedere niente a schermo.

Questo ha imposto una disciplina precisa, in tre livelli:

1. **Tutto ciò che si può separare da Unreal, si separa** — lo strato puro — e si
   testa per davvero, numericamente.
2. **Ciò che resta in Unreal si controlla staticamente**: script che leggono il
   sorgente e cercano le classi di errore già incontrate.
3. **Ciò che si vede solo a schermo** ha comandi di diagnosi dedicati, e un
   documento di verifica che dice cosa guardare.

---

## 8.1 I test C++ senza Unreal

### Come si lanciano

```bat
cmake -S Plugins/GeoWorld/Tools/StandaloneTests -B build
cmake --build build
build\Debug\geocore_tests.exe
build\Debug\geotiles_tests.exe  [cartella di un dataset, facoltativa]
build\Debug\geoquadtree_tests.exe
build\Debug\geomesh_tests.exe
build\Debug\geoimagery_tests.exe
```

Durano meno di un secondo in tutto. Non serve Unreal: sono programmi C++ normali,
che includono gli header dello strato puro.

### Cosa verificano

| Eseguibile | Test | Cosa dimostra |
|---|---:|---|
| `geocore_tests` | 37 | conversioni geodetiche, Bowring contro iterativo, basi NEU, 1.000 rebase senza deriva, precisione dei `float` |
| `geotiles_tests` | 18 (+1) | formato `.ght`, schema di tiling, cache LRU; con un dataset vero, ne rilegge indici, header e giunzioni |
| `geoquadtree_tests` | 42 | volume, frustum, margine, orizzonte, errore su schermo, selezione, niente buchi |
| `geomesh_tests` | 28 | conteggi, precisione del frame locale, normali, gonne, **giunzioni in ECEF** |
| `geoimagery_tests` | 44 | ritaglio contro la geografia, scelta dell'antenato, formato `.gim`, cache template |
| **Totale** | **169** | |

### Un test che conta più degli altri

Il test più istruttivo è quello delle **giunzioni** in `geomesh_tests`. Non
verifica una formula: costruisce due tile vicine **in frame locali diversi**, le
porta entrambe in ECEF, e controlla che i 129 vertici del bordo comune coincidano.
Collega quattro pezzi diversi — il formato, il tiling, la geodesia, la mesh — e
verifica che **insieme** facciano la cosa giusta.

### Il principio: verificare contro qualcosa di indipendente

I test migliori del progetto non confrontano il codice con se stesso. Lo
confrontano con qualcosa che **non dipende da quel codice**:

| Test | Confronto indipendente |
|---|---|
| Bowring | contro un algoritmo iterativo diverso |
| Orizzonte | contro la formula fisica √(2Rh) |
| Ritaglio delle immagini | contro i rettangoli geografici misurati in gradi |
| Quadrati MGRS | contro le cartelle **vere** del bucket Sentinel-2 |
| Schema di tiling | vettori generati dalla pipeline Python e riletti dal C++ |

Un test che confronta una funzione con una sua copia trova solo gli errori di
battitura. Uno che la confronta con la fisica trova gli errori di ragionamento.

---

## 8.2 I test Python

```bat
cd Pipeline
python -m unittest discover -s tests
```

**77 test.** Lavorano su un **sorgente sintetico** generato al volo
(`make_synthetic_source.py`), così non serve scaricare niente.

Tre scelte da notare:

* **Saltano invece di fallire** quando manca una libreria opzionale. Senza la
  griglia geoidica i test del geoide saltano; senza Pillow saltano quelli che
  comprimono JPEG; senza GDAL quelli che riproiettano. Un test rosso deve voler
  dire *"il codice è sbagliato"*, non *"ti manca una libreria"*.
* **`test_resolution.py` non passa mai `--max-level`.** È stato scritto dopo il
  bug del livello 24 (capitolo 3.9), che era rimasto nascosto proprio perché tutti
  gli altri test lo passavano.
* **Alcuni test controllano la struttura del codice**, non il suo comportamento:
  vedi la sezione 8.4.

---

## 8.3 I controlli statici sul C++

Il ponte Unreal non si può compilare qui. Ma molti errori di compilazione
seguono schemi riconoscibili leggendo il sorgente. Ogni volta che un errore è
arrivato fino alla tua macchina, è nato un controllo che lo intercetta in locale.

### `CheckSourceDiscipline.sh` — cinque regole

| Regola | Cosa controlla | Nata da |
|---|---|---|
| 1 | lo strato puro non include niente di Unreal | il principio del progetto |
| 2 | nessun parametro ha lo stesso nome di un campo | errori di *shadowing* sulla tua macchina |
| 3 | nessuna funzione dello strato puro è invisibile agli altri moduli | `unresolved external symbol` |
| 4 | il numero 100 (metri → cm) sta solo in `GeoUnits.h` | quattro `* 100` sparsi |
| 5 | i tipi `FAutoConsoleCommand*` esistono davvero | `FAutoConsoleCommandWithArgs` |

Le regole 2 e 3 sono implementate da due script Python a parte:
`CheckShadowedParameters.py` e `CheckModuleExports.py`.

### `CheckUnityCollisions.py`

Cerca funzioni con lo stesso nome in namespace anonimi di `.cpp` diversi dello
stesso modulo: la build unity le farebbe scontrare (capitolo 1.11). Nato da
`C2084: PackKey already has a body`.

### `CheckOwnApiCalls.py`

Cerca chiamate a metodi che **non esistono** sulle classi del progetto, e propone
il nome giusto:

```
'SetBudget' non e' un metodo di TTileCache (variabile 'Cache')
forse intendevi: SetBudgetBytes
```

Nato da `C2039: 'SetBudget' is not a member of 'TTileCache'`.

### Come si scrive un buon controllo statico

`CheckOwnApiCalls.py` ha richiesto tre giri, e ognuno insegna qualcosa:

1. La prima versione cercava di riconoscere le firme dei metodi con precisione,
   non gestiva quelle scritte su più righe, e dava **48 falsi positivi**.
   Inutilizzabile. Ora raccoglie ogni nome seguito da una parentesi nel corpo
   della classe: un insieme più grande dei metodi veri, che basta per rispondere a
   *"questo nome esiste?"*.
2. Poi segnalava variabili che nello stesso file hanno tipi diversi in funzioni
   diverse. Ora un nome ambiguo si ignora.
3. La terza versione passava sul codice pulito ma **non trovava l'errore per cui
   era stata scritta**, perché i campi sono dichiarati nell'header e usati nel
   `.cpp`. Ora legge anche l'header corrispondente.

Le lezioni:

> **Un controllo che urla al lupo viene spento dopo tre giorni.** Meglio lasciar
> passare un errore che segnalarne dieci inesistenti.

> **Un controllo va provato reintroducendo l'errore vero.** Che passi sul codice
> pulito non dimostra niente: anche uno script vuoto passa sul codice pulito.

Ogni controllo di questo capitolo è stato verificato così: si rimette nel codice
l'errore da cui è nato, e si controlla che lo trovi.

---

## 8.4 I controlli sulla struttura del Python

Due file di test non verificano cosa fa il codice, ma **come è scritto**.

**`test_imports.py`** verifica, leggendo l'albero sintattico, che nessun modulo
importi Pillow, GDAL o pyproj **in cima al file**. Nato da un errore preciso: un
`from PIL import Image` in testa a un modulo si propagava a catena fino al comando
`check-env` — cioè lo strumento di **diagnosi** dell'ambiente moriva proprio per
la cosa che doveva diagnosticare.

**`test_pipeline_contracts.py`** verifica che le chiavi lette dai dizionari
restituiti da `raster.py` esistano davvero fra quelle che `raster.py` produce.
Nato da `KeyError: 'x_metres'`: nessuno dei due file era sbagliato da solo, era
sbagliato il **contratto** fra i due, e Python se ne accorge solo quando la riga
viene eseguita — dopo aver scaricato un gigabyte di dati.

---

## 8.5 La verifica che si fa solo a schermo

Alcune cose non si possono verificare senza guardare: l'orientamento dei
triangoli, la nebbia, l'aspetto di un'ortofoto. Per quelle, ogni fase ha:

* un **documento di verifica** (`docs/faseN-verifica.md`) che dice cosa aspettarsi,
  con i numeri, e cosa significa se non torna;
* un **comando demo** che fa tutto in un colpo (`geo.Terrain.Demo`,
  `geo.Imagery.Demo`...);
* comandi di **diagnosi** che misurano il risultato vero (`geo.Terrain.Diag`,
  `geo.Diag`, `geo.Imagery.Stats`);
* comandi che **spengono un pezzo** per mostrare a cosa serve (`geo.Terrain.Skirt 0`,
  `geo.Lod.Margin 1.0`, `geo.Imagery.Checker 1`).

L'ultimo tipo è il più istruttivo: **per capire a cosa serve una cosa, la si
spegne e si guarda cosa si rompe.**

---

## 8.6 Cosa i test non possono vedere

Onestamente:

* **nessuna riga del ponte Unreal è mai stata compilata** da chi l'ha scritta;
* i controlli statici trovano **le classi di errore già viste**, non quelle nuove;
* il taglio delle ortofoto dalla sorgente richiede GDAL, e **non è mai girato**
  prima del tuo primo `build-imagery`;
* niente di ciò che riguarda l'aspetto visivo è verificabile senza il motore.

Per questo ogni errore che è arrivato sulla tua macchina è stato trattato allo
stesso modo: **correggerlo, e poi aggiungere il controllo che lo avrebbe
trovato.** Il capitolo 9 li racconta tutti.

---

## Dove sta nel codice

| File | Cosa contiene |
|---|---|
| `Tools/StandaloneTests/CMakeLists.txt` | i cinque eseguibili di test |
| `Tools/StandaloneTests/main.cpp` | test di GeoCore |
| `Tools/StandaloneTests/geotiles_main.cpp` | test di GeoTiles |
| `Tools/StandaloneTests/geoquadtree_main.cpp` | test del quadtree |
| `Tools/StandaloneTests/geomesh_main.cpp` | test della mesh |
| `Tools/StandaloneTests/geoimagery_main.cpp` | test delle ortofoto |
| `Tools/CheckSourceDiscipline.sh` | le cinque regole |
| `Tools/CheckShadowedParameters.py` | regola 2 |
| `Tools/CheckModuleExports.py` | regola 3 |
| `Tools/CheckUnityCollisions.py` | collisioni da build unity |
| `Tools/CheckOwnApiCalls.py` | metodi inesistenti |
| `GeoCore/Private/Tests/GeoCoreTests.cpp` | test di GeoCore come *Automation Test* di Unreal |
| `Pipeline/tests/` | i test Python |

**Nota Unreal.** `GeoCoreTests.cpp` contiene test analoghi a quelli di
`main.cpp`, scritti nel sistema di test di Unreal (*Automation*), con la macro
`IMPLEMENT_SIMPLE_AUTOMATION_TEST`. Si lanciano dall'editor, dal pannello
*Automation* (in *Tools → Session Frontend*, o *Tools → Test Automation* nelle
versioni recenti), filtrando per "GeoWorld". Servono a verificare che lo strato
puro si comporti allo stesso modo anche **compilato da Unreal**, con le sue
opzioni di compilazione.

### Tutti i controlli in un colpo

```bat
Plugins\GeoWorld\Tools\CheckSourceDiscipline.sh
python Plugins\GeoWorld\Tools\CheckUnityCollisions.py
python Plugins\GeoWorld\Tools\CheckOwnApiCalls.py
```

(Lo script `.sh` serve bash: su Windows, Git Bash.)

---

## Riepilogo

* **169 test C++** senza Unreal, **77 test Python**, **sette controlli statici**.
* I test migliori confrontano con **qualcosa di indipendente**: la fisica, un
  algoritmo diverso, il mondo reale.
* I test **saltano** quando manca una libreria opzionale, non falliscono.
* Ogni errore arrivato sulla tua macchina ha generato **un controllo** che lo
  intercetta in locale.
* Un controllo statico deve essere **affidabile prima che preciso**, e va provato
  **reintroducendo l'errore vero**.
* Ciò che si vede solo a schermo ha **documenti di verifica**, **demo**, comandi di
  **diagnosi**, e comandi che **spengono un pezzo** per mostrarne lo scopo.

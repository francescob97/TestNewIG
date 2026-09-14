# Fase 1 — Come verificare che funzioni

Due livelli: la matematica si verifica **senza Unreal** (veloce, gira ovunque),
il ponte verso il motore si verifica **dentro Unreal**.

---

## A. Senza Unreal — 2 secondi, nessun prerequisito

```bash
cmake -S Plugins/GeoWorld/Tools/StandaloneTests -B build
cmake --build build
./build/geocore_tests
```

Attesi **37 test verdi**. Cosa verificano, e con che risultati misurati:

| # | Test | Soglia | Misurato |
|---|---|---|---|
| 1 | Round-trip geodetiche→ECEF→geodetiche su 175 208 punti globali | < 1e-6 m | **3.3e-9 m** |
| 2 | Bowring vs iterativo di riferimento, 200 000 punti casuali | < 1e-13 rad | **2.2e-16 rad** |
| 3 | Valori analitici noti (equatore, poli, costanti WGS84) | esatti | ok |
| 4 | Caduta per curvatura terrestre a 1 km | ±2 mm | **−0.0785 m** (atteso −0.0785) |
| 5 | Ortonormalita' e handedness delle basi | det ±1 | **ENU +1, NEU −1** |
| 6 | Georeferenziazione: unita', assi, round-trip a 1173 km | < 1e-6 m | **1.0e-9 m** |
| 7 | Invarianza dopo 1000 rebase | < 1e-7 m | **1.2e-10 m** |
| 8 | Precisione float vs double (la motivazione del rebasing) | — | float 64 cm / 0.625 mm |
| 9 | La base locale e' una rotazione propria (det +1) | ±1e-14 | **+1.000000000000000** |

Il test 4 e' quello che vale la pena capire: non e' tautologico. Un punto
1000 m a Nord sulla superficie deve trovarsi **78.5 mm sotto** il piano
tangente, perche' `d²/(2R)` vale esattamente quello. Se quel numero cambia,
abbiamo sbagliato la normale o il frame.

Poi i controlli strutturali:

```bash
./Plugins/GeoWorld/Tools/CheckSourceDiscipline.sh
```

Verifica che lo strato puro non abbia acquisito include di Unreal e che il
fattore metri→unita' non sia sparpagliato nel codice.

---

## B. In Unreal, su Windows

### B.1 Compilare

1. Tasto destro su `TestNewIG.uproject` → **Generate Visual Studio project files**.
2. Aprire `TestNewIG.sln`, configurazione **Development Editor**, target
   **Win64**, e compilare.
3. Aprire il `.uproject`.

Al primo avvio l'Output Log deve contenere:

```
LogGeoWorld: GeoCore avviato. Console: geo.Help
LogTemp: [GeoWorld] Georeferenziazione inizializzata. Origine: 41.890210N, 12.492231E, 40.0 m, soglia rebase: 10.0 km
```

### B.2 Test di automazione

**Window → Test Automation**, filtrare `GeoWorld`, **Start Tests**.
Otto test, tutti verdi.

Headless:

```
UnrealEditor-Cmd.exe <percorso>\TestNewIG.uproject ^
  -ExecCmds="Automation RunTests GeoWorld; Quit" ^
  -unattended -nopause -nullrhi -log
```

I test `GeoWorld.GeoCore.UnrealBridge` e `.RebasingInvariance` sono quelli che
l'harness standalone **non puo'** coprire: verificano `FVector`, `FQuat` e il
comportamento di `FMatrix::ToQuat()` sulla nostra base.

### B.3 Verifica visiva — e' qui che si vede se funziona davvero

Aprire un livello qualunque (anche vuoto), premere **`** per la console:

```
geo.Debug 1
geo.SpawnMarkers
geo.Goto 41.8902 12.4922 200
```

Cosa deve succedere, in ordine di importanza:

**1. Nessun jitter.** Avvicinarsi al cubo del Colosseo e muovere la camera:
gli spigoli devono essere fermi. Poi:

```
geo.Goto 45.4641 9.1919 300
```

(Milano, ~480 km). Di nuovo: nessun tremolio. L'HUD deve segnare
`Rebase: 1` o piu'.

**2. I cubi non sono paralleli fra loro.** Questa e' la verifica del frame ENU
e la piu' facile da sbagliare. Ogni marker ha la terna locale disegnata
(rosso = Nord, verde = Est, blu = Alto). Il cubo di Roma e quello di Milano
sono inclinati **di ~4.3 gradi** l'uno rispetto all'altro, perche' la verticale
locale segue la curvatura terrestre. **Se fossero paralleli, il frame ENU e'
sbagliato.**

**3. Le coordinate corrispondono alla realta'.** Mettere la camera sul cubo del
Colosseo e:

```
geo.Where
```

Le lat/lon stampate, incollate in una qualunque mappa, devono cadere sul
Colosseo entro pochi metri.

**4. Il rebasing e' invisibile.** Con `geo.Debug 1` attivo, volare in linea
retta e guardare il contatore `Rebase` e il campo `Dist origine`: la distanza
cresce fino a 10 km, poi il contatore sale e la distanza torna a ~0.
**Non deve esserci nessuno scatto visivo.**

Per *vedere* che un rebase e' avvenuto, si puo' disattivare temporaneamente il
riposizionamento della camera in Project Settings → Plugins → GeoWorld →
`bMoveViewTargetOnRebase`. A quel punto la camera salta: e' la conferma che
quel passo sta facendo qualcosa.

**5. Gli assi sono quelli dichiarati.** All'origine e' disegnata la terna:
**X rosso = Nord, Y verde = Est, Z blu = Alto**. Puntando la camera lungo +X,
il compasso dell'editor deve indicare Nord.

### B.4 Se vedi jitter

**Non ragionare per impressioni visive**: "il cubo balla" ha tre cause diverse,
visivamente quasi identiche e con rimedi opposti. Lancia

```
geo.Diag
```

che stampa i tre numeri che le distinguono e propone un verdetto:

| Verdetto | Significato | Rimedio |
|---|---|---|
| `Tick eseguiti: 0` | Il tick del subsystem non gira: il rebasing non parte mai e la camera resta a coordinate enormi. | Bug nel plugin — segnalalo. |
| Camera oltre la soglia | Il tick gira ma il rebase non scatta. | `geo.Rebase`, poi capire perche' non e' scattato da solo. |
| `ULP float` > 1 uu | Le coordinate sono troppo grandi per un float. | `geo.RebaseThreshold 2` |
| Coordinate piccole + TSR/TAA | La geodesia e' a posto: a ballare e' l'antialiasing temporale. | `r.AntiAliasingMethod 0` per confermare. |

L'ultima riga e' la piu' insidiosa: l'antialiasing temporale (TSR, il default di
UE5) ricostruisce l'immagine accumulando frame con la proiezione spostata di un
sotto-pixel. Su uno spigolo netto e privo di texture contro il cielo vuoto —
cioe' esattamente i nostri cubi di verifica — produce un tremolio orizzontale
indistinguibile a occhio da un problema di precisione. `r.AntiAliasingMethod 0`
lo spegne: se il tremolio sparisce, la geodesia non c'entra nulla.

### B.5 Comandi disponibili

```
geo.Help                      elenco dei comandi
geo.Where                     posizione geografica della camera
geo.Origin                    origine corrente, ECEF, contatore rebase
geo.Goto <lat> <lon> [quota]  teletrasporto
geo.Rebase                    forza un rebase adesso
geo.AutoRebase <0|1>          rebasing automatico on/off
geo.RebaseThreshold <km>      cambia la soglia a caldo
geo.Debug <0|1>               overlay di debug
geo.SpawnMarkers              piazza i cubi di verifica sull'Italia
geo.ClearMarkers              li rimuove
geo.Diag                      diagnostica del jitter (vedi B.4)
```

---

## C. Cosa NON e' ancora verificato

* Il progetto **non e' mai stato compilato con Unreal** in questa sessione:
  l'ambiente di sviluppo e' Linux e senza il motore. La matematica e' verificata
  numericamente e le convenzioni UE sono controllate staticamente (bilanciamento,
  posizione dei `.generated.h`, guardie `WITH_EDITOR`), ma eventuali errori di
  compilazione veri vanno risolti al primo build su Windows.
* Nessun terreno: i marker fluttuano nel vuoto. Arriva in Fase 5.
* La quota dei marker e' **ellissoidica**. Confrontandola con l'altitudine di
  Google Maps (che e' ortometrica) c'e' uno scarto di ~45-50 m in Italia:
  e' corretto, non e' un bug, ed e' esattamente cio' che la Fase 2 dovra'
  gestire con EGM2008.

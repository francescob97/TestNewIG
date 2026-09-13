# Fase 1 — Geodesia, georeferenziazione, origin rebasing

Documento di design della fase, con le motivazioni delle scelte.
Per la procedura di verifica vedi `fase1-verifica.md`.

---

## 1. Perche' due strati in GeoCore

`GeoCore` e' diviso in due parti che non si mescolano:

| Strato | Cartelle | Include ammessi |
|---|---|---|
| Matematica pura | `Public/Geo`, `Private/Geo` | solo `<cmath>`, `<cstdint>` |
| Ponte Unreal | `Public/Unreal`, `Private/Unreal` | `CoreMinimal.h`, `Engine`, … |

Non e' purismo. Lo strato puro viene compilato **anche** da
`Tools/StandaloneTests` con un normale `g++`/`clang`, senza Unreal: la
geodesia si verifica con numeri veri in un secondo, invece che aprendo
l'editor. In Fase 2 gli stessi valori si confronteranno con `pyproj`.

La regola e' verificata automaticamente da `Tools/CheckSourceDiscipline.sh`.

---

## 2. Conversioni

### Geodetiche → ECEF

Forma chiusa esatta, nessuna iterazione:

```
N(φ) = a / sqrt(1 − e²·sin²φ)
X = (N + h)·cosφ·cosλ
Y = (N + h)·cosφ·sinλ
Z = (N·(1 − e²) + h)·sinφ
```

### ECEF → geodetiche: **Bowring 1976 + una raffinazione**

Scelto contro l'alternativa iterativa per tre ragioni:

1. **Costo deterministico.** Gira ogni frame (posizione camera per HUD e
   rebasing) e, dalla Fase 4, su migliaia di tile. Un metodo iterativo ha un
   numero di iterazioni variabile: varianza nel frame time e risultati non
   bit-identici fra thread diversi.
2. **Accuratezza sovrabbondante.** Bowring sta sotto il decimo di millimetro
   fra −10 km e +30 km di quota. Il dominio reale e' [−500 m, +9000 m] su un
   DTM con passo 10 m.
3. **E' una funzione sola.** Sostituibile con Vermeille in un punto solo se
   un giorno servissero quote orbitali.

Misurato: con la raffinazione a punto fisso, Bowring e l'implementazione
iterativa di riferimento concordano entro **2.2e-16 rad**, cioe' l'epsilon
di macchina.

Due dettagli implementativi che evitano bug classici:

* La quota si calcola con `h = p·cosφ + Z·sinφ − a·sqrt(1 − e²sin²φ)`, che
  **non divide per `cos φ`** e quindi resta stabile ai poli, a differenza
  della piu' nota `h = p/cosφ − N`.
* Il caso "punto sull'asse polare" ha un ramo dedicato: li' la longitudine
  e' matematicamente indefinita.

### Perche' esiste anche l'implementazione iterativa

Serve **solo ai test**. Scrivere due volte la stessa cosa con algoritmi
diversi, partendo da inizializzazioni diverse, e' il modo piu' economico di
scoprire che una delle due ha un bug.

---

## 3. Assi: North → X, East → Y, Up → Z

ENU e' **destrorso** (`E × N = U`); Unreal e' **sinistrorso** (X avanti, Y a
destra, Z su). Copiare le componenti 1:1 produce un mondo **specchiato**:
chiralita' sbagliata dei modelli, rotazioni al contrario.

Serve una riflessione, cioe' una matrice con determinante −1. Scambiare North
ed East e' il modo piu' utile di ottenerla, perche' in piu' fa coincidere lo
**yaw di Unreal con l'azimuth della bussola**: yaw 0 = Nord (+X), yaw 90 =
Est (+Y), orario visto dall'alto. Un attore con rotazione identita' guarda a
Nord.

Conseguenza non ovvia, verificata dal test 9 dell'harness: la base di
**orientamento locale** in un punto qualunque, espressa in assi Unreal, ha
determinante **+1**. La base NEU locale in ECEF ha det −1 e la matrice
ECEF→Unreal ha det −1: il loro prodotto e' una rotazione propria. Questo e'
cio' che rende lecito passarla a `FMatrix::ToQuat()`, che su una matrice con
det −1 non fallisce ma restituisce silenziosamente spazzatura.

---

## 4. Unita'

`1 unita' Unreal = 1 cm`. La costante vive in `Public/Geo/GeoUnits.h` e viene
applicata **solo** dentro `FGeoreference`. Il resto del motore parla metri in
`double`.

La regola e' imposta da due cose, non dalla disciplina:

* I tipi. `FEcef` (metri) e `FVector` (centimetri) sono tipi diversi: il
  compilatore rifiuta di scambiarli.
* `Tools/CheckSourceDiscipline.sh`, che cerca il fattore 100 fuori dai due
  file autorizzati. Ha gia' trovato quattro violazioni nel primo giro.

All'avvio il subsystem verifica anche `AWorldSettings::WorldToMeters` (la
convenzione del *motore*, letta da audio, VR e fisica) con un `ensureMsgf`.

---

## 5. Origin rebasing

### Cosa fanno davvero le Large World Coordinates

* In **UE4** le coordinate mondo erano `float` e il mondo era limitato a
  `WORLD_MAX = 2 097 152` unita', cioe' **~21 km**. Un pianeta era
  irrappresentabile.
* In **UE5** `FVector` e' `FVector3d`, cioe' `double`, e con esso `FMatrix`,
  `FTransform`, `FRotator`. Posizionare un attore a coordinate da raggio
  terrestre e' lecito e preciso.
* Ma **float sopravvive dove conta**: vertex buffer, uniform dei shader, parti
  di Chaos.

Numeri misurati dall'harness (test 8), che sono la giustificazione
quantitativa dell'intera architettura:

| | ULP a distanza del geocentro | ULP entro 10 km dall'origine |
|---|---|---|
| `float` (in cm) | **64 cm** | **0.625 mm** |
| `double` (in cm) | 1.2e-9 m | — |

### Meccanismo scelto: floating origin nostro, non quello del motore

Unreal ha un suo *World Origin Rebasing*
(`UWorld::RequestNewWorldOrigin` / `AActor::ApplyWorldOffset`). **Non lo
usiamo**: e' legato al vecchio World Composition, di fatto incompatibile con
World Partition, sposta tutti gli attori del mondo con effetti collaterali che
non controlliamo, e lavora su `FIntVector` con quantizzazione.

Il nostro rebasing vive dentro la trasformazione georeferenziata:

```
P_unreal[cm] = M · (P_ecef[m] − O_ecef[m]) · 100
```

### L'invariante che elimina la deriva

**La posizione ECEF e' l'unica autorita'. La posizione in unita' Unreal e'
sempre un valore derivato, mai stato primario.**

Percio' non si compone mai una trasformazione con la precedente: si ricalcola
sempre da capo dall'ECEF. Misurato: dopo **1000 rebase consecutivi** su origini
casuali, un punto geografico fisso si sposta di **1.2e-10 m**, e tornando
all'origine iniziale ritorna esattamente a (0,0,0).

### Sequenza di un rebase

1. Congela in ECEF la posizione della camera (che non e' georeferenziata).
2. Cambia origine, incrementa `Generation`.
3. Rimette la camera dove stava geograficamente, con
   `SetActorLocation(..., bSweep=false, nullptr, ETeleportType::TeleportPhysics)`.
   * `bSweep = false` perche' non e' un movimento ma un cambio di sistema di
     riferimento: uno sweep testerebbe le collisioni lungo 10 km.
   * `TeleportPhysics` perche' senza, Chaos deduce una velocita' di 10 km/frame
     e spara il pawn nello spazio.
4. Notifica i componenti registrati, che si **ricalcolano dalla propria
   coordinata geodetica**.

### Limite noto e voluto

Un attore piazzato a mano e privo di `UGeoTransformComponent` **apparira'
spostato** dopo un rebase: la sua posizione e' in unita' Unreal, e quelle
cambiano significato. In Fase 1 il mondo e' vuoto a parte i marker; dalla
Fase 4 il terreno e' tutto georeferenziato. La soluzione, se servisse, e'
agganciare il contenuto a un attore-ancora georeferenziato.

---

## 6. Le classi Unreal e il perche'

| Classe | Base | Perche' |
|---|---|---|
| `UGeoreferenceSubsystem` | `UWorldSubsystem` + `FTickableGameObject` | Singleton con ciclo di vita gestito dal motore, **uno per `UWorld`**: editor, PIE e preview hanno origini indipendenti. `FTickableGameObject` da' un tick senza creare un attore fittizio. |
| `UGeoTransformComponent` | `USceneComponent` | Si attacca a qualunque attore, eredita il gizmo dell'editor, e ha la coppia garantita `OnRegister`/`OnUnregister` — il posto corretto per iscriversi al subsystem (non il costruttore: gli oggetti UE vengono costruiti come CDO, duplicati per il PIE, distrutti dal GC). |
| `UGeoWorldSettings` | `UDeveloperSettings` | Compare da sola in Project Settings e si serializza nel `.ini`, senza scrivere UI. |
| `FGeoreferenceSnapshot` | struct C++ | **Non** una `USTRUCT`: vive solo in C++, la riflessione sarebbe costo senza beneficio. E' la copia per valore che i worker thread si portano dietro. |
| `AGeoMarkerActor` | `AActor` | Verifica visiva. |

### Dettagli che sono trappole per chi arriva da altri motori

* `Mobility = EComponentMobility::Movable` sui componenti georeferenziati: un
  componente `Static` **non puo'** essere spostato a runtime.
* `ShouldCreateSubsystem` filtra i mondi: Unreal ne crea in continuazione per
  le miniature degli asset e le preview dei materiali.
* `IsTickableInEditor() = true`, altrimenti nell'editor nulla si aggiorna
  finche' non premi Play.
* `PostEditChangeProperty`, altrimenti digitare una latitudine nel pannello
  Details non muove nulla e sembra che il plugin sia rotto.
* Dipendenza da `UnrealEd` **condizionale** (`if (Target.bBuildEditor)`) piu'
  `#if WITH_EDITOR`: serve per leggere la camera del viewport, che non e' un
  attore. Senza questa accortezza il progetto compila nell'editor e si rompe al
  packaging.
* `GEOCORE_API` davanti a `DECLARE_LOG_CATEGORY_EXTERN`: senza, il linker non
  trova la categoria dagli altri moduli.

---

## 7. Thread safety, decisa adesso e non in Fase 3

Un worker **non chiama mai** il subsystem: riceve una copia di
`FGeoreferenceSnapshot` all'avvio del task e la usa per tutta la durata. Se nel
frattempo avviene un rebase, il `Generation` non corrisponde piu' e il game
thread ricalcola la sola trasformazione finale all'attach.

Nessun lock nel percorso caldo. `GetSnapshot()` prende un `FRWLock` in lettura,
che si contende solo con il rebase (raro, sul game thread).

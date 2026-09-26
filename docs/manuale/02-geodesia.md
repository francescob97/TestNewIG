# Capitolo 2 — La Terra, le coordinate, il rebasing (Fase 1)

> Il capitolo su cui poggia tutto il resto. Se lo capisci, capisci perché ogni
> altro pezzo del motore è fatto com'è.

---

## L'idea in breve

Per disegnare la Terra bisogna rispondere a due domande.

1. **Come si descrive un punto della Terra?** Con latitudine e longitudine, ma
   per fare calcoli servono coordinate cartesiane (X, Y, Z). Servono formule
   per passare dall'una all'altra, e servono giuste al millimetro.
2. **Come si mette un punto della Terra dentro Unreal?** Unreal lavora in
   centimetri con numeri che, sulla scheda video, hanno poca precisione. Se
   metti il Colosseo alle sue coordinate vere, a 6.000 km dal centro della
   Terra, trema.

La risposta alla seconda domanda si chiama **rebasing**, ed è la decisione più
importante di tutto il progetto.

---

## 2.1 La forma della Terra

La Terra non è una sfera: è schiacciata ai poli, perché ruota. Il modello
matematico usato quasi ovunque è un **ellissoide di rotazione**: un'ellisse
fatta girare attorno all'asse polare.

Un ellissoide è definito da **due numeri**. Quello che usiamo, **WGS84**, è lo
stesso del GPS:

| | Valore | Significato |
|---|---|---|
| `a` | 6.378.137 m | raggio equatoriale — **definito**, non misurato |
| `1/f` | 298,257223563 | inverso dello schiacciamento — **definito** |
| `b` | 6.356.752,3142 m | raggio polare, **derivato**: `b = a·(1 − f)` |

La differenza fra i due raggi è di **21,4 km**. Sembra poco su 6.378 km, ma è
più del doppio dell'altezza dell'Everest: ignorarla sposterebbe le cose di
chilometri.

Nel codice:

```cpp
inline constexpr FEllipsoid WGS84{ 6378137.0, 298.257223563 };
```

*(in `GeoCore/Public/Geo/Ellipsoid.h`)*

`FEllipsoid` precalcola nel costruttore tutte le grandezze derivate
(eccentricità, raggio polare), perché compaiono in funzioni chiamate milioni di
volte e non vale la pena ricalcolarle ogni volta.

---

## 2.2 Tre modi di dire "dove"

Questo è il punto da capire bene. Lo stesso luogo si può descrivere in tre modi,
e ognuno serve a qualcosa.

### 1. Coordinate geodetiche: latitudine, longitudine, quota

Il modo umano. "Il Colosseo è a 41,8902° N, 12,4922° E, quota 69 m."

Nel codice è **`FGeodetic`** (in `GeoCore/Public/Geo/GeoTypes.h`), con gli angoli
in **radianti** e la quota in metri **sull'ellissoide**.

### 2. Coordinate ECEF: X, Y, Z dal centro della Terra

ECEF significa *Earth-Centered, Earth-Fixed*: un sistema cartesiano con
l'origine nel centro della Terra, che ruota con lei.

* **X** punta verso l'intersezione fra equatore e meridiano di Greenwich;
* **Y** verso l'equatore a 90° Est;
* **Z** verso il Polo Nord.

> 💡 **Esempio.** Il Colosseo in ECEF:
> ```
> X = 4.642.623,7 m
> Y = 1.028.584,5 m
> Z = 4.236.579,7 m
> ```
> Distanza dal centro della Terra: 6.368.716 m. Sono numeri grandi, ed è
> proprio questo il problema della sezione 2.7.

Nel codice è **`FEcef`**. È il sistema in cui si fanno i calcoli, perché è
cartesiano: le distanze sono distanze, i vettori si sommano. **Ed è l'unica
autorità del progetto**: ogni posizione "vera" è in ECEF, tutto il resto ne è
derivato.

### 3. Coordinate locali: Nord, Est, Alto

Stai in piedi al Colosseo. "Cento metri a nord, cinquanta a est, dieci più in
alto." È un sistema cartesiano **appoggiato sulla superficie** in un punto
preciso, con un asse verso nord, uno verso est, uno verso l'alto.

Si chiama **ENU** (East-North-Up) o **NEU** (North-East-Up) a seconda dell'ordine
degli assi, e la sezione 2.6 spiega perché l'ordine conta molto più di quanto
sembri.

### Perché servono tutti e tre

| Sistema | Buono per | Pessimo per |
|---|---|---|
| Geodetiche | parlare con le persone, leggere i dati GIS | fare calcoli: le distanze non sono lineari |
| ECEF | fare calcoli, essere la verità | disegnare: numeri enormi, nessun "su" |
| Locale | disegnare, ragionare sul posto | vale solo vicino al suo punto |

---

## 2.3 Da geodetiche a ECEF

Questa direzione è facile: esiste una **formula chiusa esatta**.

```
N(φ) = a / √(1 − e²·sin²φ)          raggio di curvatura

X = (N + h) · cos φ · cos λ
Y = (N + h) · cos φ · sin λ
Z = (N·(1 − e²) + h) · sin φ
```

dove φ è la latitudine, λ la longitudine, h la quota, e² l'eccentricità.

Il dettaglio da notare è `(1 − e²)` sulla **Z**: è l'intero schiacciamento
terrestre. È il motivo per cui "verso l'alto" in un punto **non punta al centro
della Terra**.

Funzione: `GeodeticToEcef()` in `GeoCore/Public/Geo/Ellipsoid.h`.

---

## 2.4 Da ECEF a geodetiche: la scelta di Bowring

La direzione inversa è difficile: **non esiste una formula chiusa esatta**. Ci
sono due famiglie di soluzioni.

| Approccio | Come funziona | Pro | Contro |
|---|---|---|---|
| **Iterativo** | si parte da una stima e si ripete finché non converge | preciso quanto vuoi | numero di passi **variabile** |
| **Bowring (1976)** | formula approssimata in un colpo solo | costo **fisso** | approssimato |

**Scelta: Bowring, più un passo di raffinazione.**

Le ragioni, in ordine di importanza:

1. **Costo prevedibile.** Questa funzione gira a ogni frame per la camera, e
   dalla Fase 4 su migliaia di tile. Un metodo iterativo fa a volte 2 passi, a
   volte 5: il tempo del frame varia, e peggio, **due thread diversi possono
   ottenere risultati non identici bit per bit**, il che rende i bug
   irriproducibili.
2. **Precisione più che sufficiente.** Con un passo di raffinazione, Bowring e
   l'iterativo di riferimento concordano entro **2,2 × 10⁻¹⁶ radianti** — che è
   l'epsilon della macchina, cioè il limite fisico del `double`. Non si può fare
   meglio.
3. **Sostituibile.** È una funzione sola: se un giorno servissero satelliti in
   orbita alta, dove Bowring perde precisione, si cambia in un punto.

### Perché esiste anche la versione iterativa

`EcefToGeodeticIterative()` è nel codice ma **non la usa nessuno a runtime**.
Serve ai test: scrivere la stessa funzione due volte con algoritmi diversi e
confrontarle è il modo più economico di scoprire che una delle due ha un bug. Se
concordano all'epsilon di macchina su migliaia di punti, sono giuste entrambe.

### Due dettagli che evitano bug classici

**La formula della quota.** La più nota è `h = p / cos φ − N`. Ai poli cos φ
vale zero, e si divide per zero. Il codice usa invece:

```
h = p·cos φ + Z·sin φ − a·√(1 − e²·sin²φ)
```

che **non divide per niente** ed è stabile ovunque. Funzione di supporto:
`Detail::HeightFromLatitude()`.

**Il punto sull'asse polare.** Se X e Y sono entrambi zero, la longitudine è
matematicamente indefinita (al Polo Nord tutti i meridiani si incontrano). C'è
un ramo dedicato, con la soglia `PolarAxisEpsilonM`.

### Alternative considerate

* **Vermeille (2002)**: esatto in forma chiusa, ma con radici cubiche e più
  costoso. Utile per le orbite, sprecato per il terreno.
* **Iterazione di Heiskanen-Moritz**: la classica, tenuta proprio come
  riferimento per i test.
* **Usare PROJ a runtime**: una dipendenza enorme per una funzione di trenta
  righe, e PROJ non è pensato per essere chiamato milioni di volte per frame.

---

## 2.5 La verticale non punta al centro della Terra

Una sottigliezza che sembra accademica e non lo è.

"Verso l'alto" in un punto si può intendere in due modi:

* **geocentrico**: la direzione dal centro della Terra al punto;
* **geodetico**: la perpendicolare alla superficie dell'ellissoide in quel punto.

Su una sfera coincidono. Sull'ellissoide **no**: a 45° di latitudine
differiscono di **11,5 primi d'arco**. Sembra niente, ma su una tile di terreno
da 5 km significa un'inclinazione di circa 17 metri da un bordo all'altro.

Il codice usa sempre la normale **geodetica**: `GeodeticSurfaceNormal()`. È la
verticale vera, quella che segue un filo a piombo (a meno dell'ondulazione del
geoide, capitolo 3).

---

## 2.6 Il frame locale, e la trappola della mano

Qui c'è il primo errore che in questo progetto si è rischiato davvero, ed è
istruttivo.

### ENU è destrorso, Unreal è sinistrorso

Un sistema di assi è **destrorso** se, puntando il pollice della mano destra
lungo X e l'indice lungo Y, il medio indica Z. ENU (Est, Nord, Alto) è destrorso.

Unreal è **sinistrorso**: X avanti, Y a destra, Z in alto. Se provi con la mano
destra, il medio punta in basso.

La tentazione è copiare ENU in Unreal così com'è: Est → X, Nord → Y, Alto → Z.
**Il risultato è un mondo allo specchio.** Non si nota subito — le montagne sono
montagne — ma tutto è riflesso: l'Italia ha il tacco dalla parte sbagliata, i
modelli 3D hanno la chiralità invertita, le rotazioni girano al contrario.

### La soluzione: NEU

Serve una **riflessione**, cioè scambiare due assi. Il progetto scambia Nord ed
Est:

| Asse Unreal | Direzione geografica |
|---|---|
| **X** (avanti) | **Nord** |
| **Y** (destra) | **Est** |
| **Z** (alto) | **Alto** |

Perché proprio questi due? Perché in più **fa coincidere lo yaw di Unreal con
l'azimut della bussola**: yaw 0° = Nord, yaw 90° = Est, in senso orario visto
dall'alto. Un attore con rotazione zero guarda a nord, esattamente come un
navigatore si aspetta.

> 💡 **Esempio.** Metti un attore con rotazione (0, 0, 0) in qualunque punto
> d'Italia: guarda a nord. Con yaw 90 guarda a est, con 180 a sud. È quello che
> farebbe una bussola. Con ENU ingenuo, lo stesso attore guarderebbe a est, e
> l'Italia sarebbe allo specchio.

Funzioni: `MakeEnuBasis()` e `MakeNeuBasis()` in `Ellipsoid.h`. La base NEU ha
**determinante −1** (è una riflessione), ed è l'unico punto del progetto dove
questo è voluto.

> ⚠️ **Trappola.** Dentro Unreal, invece, tutte le matrici di rotazione devono
> avere determinante **+1**. `FMatrix::ToQuat()` su una matrice con determinante
> −1 **non dà errore**: restituisce silenziosamente un quaternione sbagliato. La
> riflessione quindi avviene **una sola volta**, al confine fra geografia e
> Unreal, e dopo quel confine si lavora solo con rotazioni proprie. Il test 9
> della suite di Fase 1 lo verifica.

---

## 2.7 Il problema della precisione

Ora il problema che ha dato forma a tutto il progetto.

Un `float` (32 bit) ha **24 bit di mantissa**: circa 7 cifre significative. Il
passo fra due `float` consecutivi — si chiama **ULP**, *unit in the last place* —
cresce con la grandezza del numero:

| Distanza dall'origine | ULP di un `float` in centimetri |
|---|---|
| 10 km | **0,625 mm** |
| 6.371 km (raggio terrestre) | **64 cm** |

Se il Colosseo sta a 6.371 km dall'origine di Unreal, **i suoi vertici possono
stare solo su una griglia con passo 64 cm**. Muovendo la camera, i vertici
saltano da un gradino all'altro: il modello trema, i bordi ballano.

> 💡 **Esempio.** Un palo della luce alto 8 metri, a 6.371 km dall'origine: la
> sua cima può stare a 7,68 m o a 8,32 m, niente in mezzo. A 10 km
> dall'origine, invece, lo sbaglio massimo è di 0,6 millimetri.

### Ma Unreal 5 non usa i double?

Sì, e qui c'è l'equivoco più diffuso. In Unreal 5 `FVector` è `double`: la
**posizione degli attori** sulla CPU è precisa anche a scala planetaria. Ma:

* i **vertici** delle mesh vanno alla scheda video in `float`;
* gli **shader** lavorano in `float`;
* parti del **motore fisico** lavorano in `float`.

Il double ti salva la posizione dell'attore, non la sua geometria.

---

## 2.8 La soluzione: il rebasing

**Idea.** Tenere l'origine di Unreal **sempre vicina alla camera**. Se tutto ciò
che si vede sta entro qualche chilometro dall'origine, tutti i `float` hanno una
precisione sotto il millimetro.

Quando la camera si allontana troppo dall'origine, si **sposta l'origine** sotto
la camera, e si ricalcola la posizione Unreal di tutto. È il *rebasing*, o
*floating origin*.

### La formula

Tutto il progetto passa da questa formula, in `GeoCore/Public/Geo/Georeference.h`:

```
P_unreal[cm] = M · (P_ecef − O_ecef) · 100
```

* `O_ecef` è l'origine corrente, in ECEF;
* `M` è la base NEU nel punto dell'origine;
* `100` converte metri in centimetri.

Leggila da destra: prendi la posizione vera (ECEF), sottrai l'origine (ora è un
vettore piccolo, relativo), ruotala negli assi locali (NEU), convertila in
centimetri.

### L'invariante che rende il rebasing senza errori

> **La posizione ECEF è l'unica autorità. La posizione Unreal è sempre
> ricalcolata da capo, mai modificata.**

Sembra un dettaglio. È tutto. L'alternativa ingenua sarebbe: "al rebase,
sottrai lo spostamento dell'origine da ogni posizione Unreal". Funziona una
volta. Dopo mille rebase, ogni sottrazione ha aggiunto un piccolo errore di
arrotondamento, e gli oggetti sono **derivati** dalla loro posizione vera.

Con l'invariante, invece, ogni rebase ricalcola dall'ECEF, che non è mai stato
toccato. Il test di Fase 1 lo misura: **dopo 1.000 rebase, l'errore è di
1,2 × 10⁻¹⁰ metri**. Zero, in pratica.

### Alternative considerate

| Alternativa | Perché no |
|---|---|
| **Nessun rebasing**, fidarsi dei double di UE5 | i vertici sono `float`: trema tutto (sezione 2.7) |
| **Il rebasing di Unreal** (`UWorld::RequestNewWorldOrigin`) | legato al vecchio World Composition, incompatibile con World Partition, sposta **tutti** gli attori del mondo con effetti che non controlli, e lavora su interi con quantizzazione |
| **Sottrarre lo spostamento** a ogni rebase | accumula errore (vedi sopra) |
| **Origine fissa, mondo diviso in celle** | complica tutto il resto per un risultato identico |

---

## 2.9 Il rebase, passo per passo

Il metodo è `UGeoreferenceSubsystem::ApplyRebase()` in
`GeoCore/Private/Georeference/GeoreferenceSubsystem.cpp`, ed è scritto in
quattro passi commentati.

**Passo 1 — Fotografare la camera in ECEF.** La camera non è
georeferenziata: la sua posizione è in centimetri Unreal, e quelli stanno per
cambiare significato. Prima di tutto la si converte in ECEF, cioè in qualcosa
che non cambierà.

**Passo 2 — Cambiare l'origine.** Nuova `O_ecef`, nuova base `M`.

**Passo 3 — Rimettere la camera dove stava geograficamente.** Dalla sua
posizione ECEF (passo 1) si calcola la nuova posizione Unreal. Sullo schermo non
cambia niente: la camera è nello stesso punto del mondo, sono cambiati solo i
numeri che lo descrivono.

> ⚠️ **Due parametri che sembrano dettagli e non lo sono.**
> ```cpp
> ViewTarget->SetActorLocation(NewLocation, /*bSweep=*/false,
>                              nullptr, ETeleportType::TeleportPhysics);
> ```
> **`bSweep = false`**: questo non è un movimento, è un cambio di sistema di
> riferimento. Con lo sweep, Unreal controllerebbe le collisioni lungo un
> segmento di 10 km e genererebbe urti fantasma.
>
> **`TeleportPhysics`**: dice al motore fisico (Chaos) di **non dedurre una
> velocità** dallo spostamento. Senza, Chaos vede 10 km percorsi in un frame, ne
> deduce una velocità di 600 km/s, e spara il pawn nello spazio.

**Passo 4 — Avvisare chi deve ricalcolarsi.** Ogni `UGeoTransformComponent`
ricalcola la propria posizione dall'ECEF, e il delegato `OnGeoreferenceRebased`
avvisa tutti gli altri — dalla Fase 5 il terreno, che ricalcola solo le
trasformazioni dei componenti senza toccare un vertice.

### Nell'editor la camera non è un attore

C'è un caso a parte: fuori dal Play, la camera è uno stato del *viewport
client* dell'editor, non un attore. Il subsystem la salva e la ripristina
separatamente, con codice chiuso in `#if WITH_EDITOR`. Senza quel ramo, nel
viewport dell'editor il mondo "scapperebbe" via al primo rebase.

### Quando scatta

Le regole stanno in `UGeoWorldSettings` (*Project Settings → Plugins →
GeoWorld*):

| Impostazione | Valore | Perché |
|---|---|---|
| `RebaseThresholdKm` | 10 km | entro 10 km un `float` è sotto il millimetro |
| `MinFramesBetweenRebases` | 10 | evita rebase a raffica se si vola molto veloce |
| `bAutoRebase` | vero | disattivabile per il debug (`geo.AutoRebase 0`) |

---

## 2.10 Lo snapshot: come i thread leggono l'origine

Dalla Fase 3 in poi, parti del motore girano su altri thread. Anche loro devono
convertire coordinate — ma l'origine può cambiare in qualunque momento.

**Regola fissata in Fase 1, prima che servisse:** *un thread di lavoro non
chiama mai il subsystem.* Riceve una **copia** della georeferenziazione,
`FGeoreferenceSnapshot`: una manciata di `double`, copiarla non costa niente.

La copia porta un **numero di generazione**, che cresce a ogni rebase. Se un
thread finisce un lavoro calcolato con un'origine ormai vecchia, il numero non
corrisponde e il game thread sa che deve ricalcolare la trasformazione finale.
Nessun lucchetto nel percorso caldo, nessuno stato condiviso modificabile.

File: `GeoCore/Public/Georeference/GeoreferenceSnapshot.h`.

> **Nota Unreal.** `FGeoreferenceSnapshot` **non** è una `USTRUCT`. Una USTRUCT
> impone che ogni campo sia riflettibile, e la matrice `FMat3` non lo è. Siccome
> questa struttura vive solo in C++, la riflessione sarebbe un costo senza
> beneficio. Per i Blueprint esiste `FGeoCoordinate` (in `GeoWorldTypes.h`), che
> è una USTRUCT con latitudine, longitudine e quota in gradi.

---

## 2.11 Due regole di igiene

### Le unità si convertono in un punto solo

Il numero 100 (metri → centimetri) compare **in un solo file**:
`GeoCore/Public/Geo/GeoUnits.h`, come `GeoWorld::Units::MetersToUu`. Lo script
`Tools/CheckSourceDiscipline.sh` fallisce se trova `* 100` in qualunque altro
posto.

Sembra pedante. Il motivo è che l'errore di unità è **il più silenzioso che
esista**: una quota in metri scambiata per centimetri produce una montagna alta
48 metri invece di 4.800, e niente si rompe.

> 💡 **Esempio.** In una delle prime scritture c'erano quattro `* 100` sparsi
> nel codice. Li ha trovati lo script, non un test: i test controllavano i
> risultati, e i risultati erano giusti — per ora. Il problema di una
> conversione duplicata non è che sia sbagliata oggi, è che domani qualcuno ne
> cambia una sola.

### Latitudine e longitudine non si possono scambiare

`FGeodetic` **non ha un costruttore con tre numeri**. Si crea solo così:

```cpp
FGeodetic::FromDegrees(41.89, 12.49, 69.0);   // latitudine, longitudine, quota
FGeodetic::FromRadians(0.7311, 0.2180, 69.0);
```

Perché? Perché `FGeodetic(41.89, 12.49, 69.0)` non dice se il primo numero è la
latitudine o la longitudine, e scambiarle è **l'errore più comune del settore**:
GeoJSON usa (longitudine, latitudine), quasi tutto il resto (latitudine,
longitudine). Con le funzioni nominate, il nome porta l'informazione e il
compilatore impedisce di dimenticarla.

---

## 2.12 Quello che resta aperto: la issue #1

Nella Fase 1 hai notato che gli spigoli dei cubi di verifica **tremolavano**
muovendo la camera. È stato trovato e corretto un bug vero (il tick del
subsystem, capitolo 1.6), **ma il sintomo è rimasto**.

Le cause possibili sono tre, e il comando `geo.Diag` è fatto apposta per
distinguerle:

1. il tick non gira (il contatore resta a zero);
2. il rebase non scatta (la distanza dall'origine supera la soglia);
3. le coordinate sono giuste, e a tremolare è **l'antialiasing temporale** (TSR)
   di Unreal, che su uno spigolo netto contro il cielo vuoto produce esattamente
   quel sintomo. Si verifica in cinque secondi con `r.AntiAliasingMethod 0`.

L'ipotesi più probabile è la terza. È descritta in `docs/issues-aperte.md`.

---

## Dove sta nel codice

| File | Cosa contiene |
|---|---|
| `GeoCore/Public/Geo/GeoTypes.h` | `FGeodetic`, `FEcef`, `FMat3` — i tipi base |
| `GeoCore/Public/Geo/Ellipsoid.h` | WGS84, conversioni, basi ENU e NEU |
| `GeoCore/Public/Geo/EnuFrame.h` | frame locale ENU |
| `GeoCore/Public/Geo/GeoUnits.h` | **l'unico** posto dove sta il numero 100 |
| `GeoCore/Public/Geo/Georeference.h` | la formula `P_unreal = M·(P − O)·100` |
| `GeoCore/Public/Georeference/GeoreferenceSnapshot.h` | la fotografia per i thread |
| `GeoCore/Public/Georeference/GeoreferenceSubsystem.h` + `.cpp` | il rebasing |
| `GeoCore/Public/Georeference/GeoTransformComponent.h` + `.cpp` | "tieni questo attore a questa coordinata" |
| `GeoCore/Public/Georeference/GeoWorldSettings.h` | le impostazioni in Project Settings |
| `GeoCore/Public/Georeference/GeoPlaces.h` | i 28 luoghi di `geo.Goto` |
| `GeoCore/Private/GeoCoreModule.cpp` | i comandi `geo.*` di base |
| `Tools/StandaloneTests/main.cpp` | 37 test, senza Unreal |
| `docs/fase1-design.md`, `docs/fase1-verifica.md` | i documenti originali della fase |

### Comandi

```
geo.Help                        elenco dei comandi
geo.Where                       dove sono, in coordinate geografiche
geo.Origin                      l'origine corrente e le statistiche di rebasing
geo.Goto <lat> <lon> [quota]    teletrasporto (quota sull'ellissoide)
geo.Goto <nome> [quota]         teletrasporto su un luogo noto (quota sul suolo)
geo.Places                      i luoghi noti
geo.Rebase                      forza un rebase adesso
geo.AutoRebase <0|1>            rebasing automatico
geo.RebaseThreshold <km>        soglia
geo.Debug <0|1>                 overlay
geo.Diag                        diagnosi del jitter
geo.SpawnMarkers                cubi di verifica su cinque luoghi noti
```

---

## Riepilogo

* La Terra è un **ellissoide WGS84**: due numeri definiti, raggi che
  differiscono di 21 km.
* Tre sistemi di coordinate: **geodetiche** (per le persone), **ECEF** (per i
  calcoli, ed è la verità), **locali** (per disegnare).
* Geodetiche → ECEF ha una formula esatta; ECEF → geodetiche usa **Bowring più
  una raffinazione**: costo fisso, precisione all'epsilon di macchina.
* **NEU, non ENU**: Unreal è sinistrorso, serve una riflessione, e scambiare
  Nord ed Est fa coincidere lo yaw con la bussola.
* Un `float` a distanza del raggio terrestre ha una precisione di **64 cm**; a
  10 km, di **0,6 mm**. I vertici sulla GPU sono `float` anche in UE5.
* Il **rebasing** tiene l'origine vicina alla camera. **L'ECEF è l'unica
  autorità**: la posizione Unreal si ricalcola sempre da capo, e dopo 1.000
  rebase l'errore è di 10⁻¹⁰ m.
* I thread lavorano su una **copia** della georeferenziazione, mai sul subsystem.
* Il numero 100 sta **in un file solo**; latitudine e longitudine **non si possono
  scambiare** per costruzione.

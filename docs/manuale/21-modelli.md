# Capitolo 21 — I modelli: dai file del database alle parti che si muovono

> Come un aereo smette di essere una scatola: leggere un modello da una
> cartella qualsiasi (OpenFlight, OBJ), le sue texture, la sua gerarchia, le
> parti che si muovono (DOF) e quelle che si accendono e si spengono (switch,
> LOD), e farne attori di Unreal senza fermare il frame. Il capitolo di
> progetto è `docs/modelli-design.md`; come provarlo è in
> `docs/modelli-verifica.md`.

---

## L'idea in breve

Nel capitolo 20 le entità arrivano, si muovono, ma sono scatole. Un IG vero
disegna i **modelli del database** del cliente: centinaia di file `.flt`
(OpenFlight, il formato della simulazione da trent'anni), con le loro
texture `.rgb`, fatti con Creator o con altri strumenti, spesso su un'altra
macchina e con i percorsi di quella macchina.

Il lavoro si divide come in tutto GeoWorld:

1. lo **strato puro** (`GeoEntities/Public/Models/`): leggere i file, capire
   la gerarchia, portare tutto nelle convenzioni dell'attore. Nessun Unreal,
   48 test;
2. **Unreal** (`GeoEntities/Private/ModelLibrary/` e il subsystem delle
   entità): mesh, texture, materiali, un attore con le sue parti.

E come nella Fase 7 c'è un'osservazione che regge tutto: **i formati sono
tanti, il contenuto è uno.** Un albero di nodi; ogni nodo con una
trasformazione, forse della geometria (triangoli con materiale e texture),
forse un comportamento (si muove, si accende, cambia con la distanza).

---

## 21.1 Un modello fedele al file, e un rig da disegnare

Ci sono **due** rappresentazioni, e la differenza è il cuore del capitolo.

**`FModel`** (`Models/ModelData.h`) è **fedele al file**: l'albero dei nodi
com'è, la geometria nelle coordinate del formato (ma già in metri). Ogni
lettore riempie un `FModel` e basta: non sa niente di Unreal né
dell'attore.

**`FModelRig`** (`Models/ModelRig.h`) è **quello che si disegna**: tutto negli
assi dell'attore, in metri, con i triangoli girati dove serve, e con
pochissime **parti**.

```
.flt  ─┐
.obj  ─┼─> FModel (convenzioni del formato) ──BuildRig──> FModelRig (attore) ──> Unreal
(.gltf)┘                                       │
                                     l'UNICO punto di conversione
```

Perché due e non uno? Perché le conversioni (assi, mano, UV) sono **le**
trappole di questa fase, e una trappola si disinnesca solo se sta in un punto
solo, con il suo test. Se ogni lettore convertisse a modo suo, avremmo tre
posti dove sbagliare, e il terzo formato rifarebbe gli errori del primo.

---

## 21.2 Le trappole

| | OpenFlight | OBJ | glTF | Attore |
|---|---|---|---|---|
| avanti | +Y | −Z | +Z | +X |
| alto | +Z | +Y | +Y | +Z |
| mano | destra | destra | destra | **sinistra** |
| unità | nell'intestazione | metri (di solito) | metri | metri |
| origine delle UV | in basso | in basso | in alto | in alto |

### Trappola 1 — la mano

Un sistema destrorso e uno sinistrorso differiscono per una **riflessione**:
la matrice di passaggio ha determinante −1. Una riflessione inverte il verso
in cui si percorrono i vertici di un triangolo: quello che era antiorario
visto da fuori diventa orario. Se non si girano i triangoli, il motore pensa
che la faccia davanti sia quella di dietro, e si vede il modello **da
dentro**.

La regola di GeoWorld (la stessa degli alberi, capitolo 16): **nell'attore,
(B−A)×(C−A) punta fuori**. `BuildRig` controlla il determinante di ogni
trasformazione che applica (la conversione, ma anche le matrici dei modelli
con una scala negativa: le ali "a specchio") e gira i triangoli quando è
negativo.

Il test non si fida delle normali (potrebbero sbagliare nello stesso modo dei
triangoli): calcola (B−A)×(C−A) **dalle coordinate** e controlla che esca dal
centro del cubo.

### Trappola 2 — l'avanti non è garantito

"OpenFlight guarda verso +Y" è un'abitudine, non una legge. Molti modelli
guardano verso +X, alcuni verso −Y. Per questo ogni regola della tabella ha
le sue **correzioni**:

```
1.2.105.1     F16/f16.flt yaw 90
1.1.105.81    "auto rossa/auto.obj" scale 0.01 up z forward x
```

`yaw` è **positivo a destra**, come la bussola e come lo pensa chi guarda il
modello. Si applicano **dopo** la conversione degli assi: prima il formato
diventa attore, poi si corregge.

### Trappola 3 — gli angoli di OpenFlight sono matematici

Un DOF di OpenFlight con imbardata +90 gira **a sinistra** (in senso
antiorario visto dall'alto): è l'angolo della matematica, dall'asse x verso
l'asse y in un sistema destrorso. La bussola, DIS e CIGI girano a destra.

Il lettore e il rig **non** cambiano il segno: un DOF ha i valori del file, con
i limiti del file, e `geo.Entities.Dof` li prende così. Il passaggio fra i
due mondi sta nella tabella delle articolazioni (M3), dove si dirà "la
torretta di questo carro è il DOF `turret`, e l'angolo di DIS va cambiato di
segno". Un test lo fissa: imbardata +90, la canna va a sinistra.

### Le unità e le UV

Le **unità** di OpenFlight sono nell'intestazione (metri, chilometri, piedi,
pollici, miglia nautiche) e si convertono subito, nel lettore: sono un fatto
del file, non una convenzione. Un modello "in piedi" con l'intestazione
sbagliata (succede) si corregge con `scale 0.3048`.

Le **UV**: OpenFlight e OBJ mettono (0, 0) in basso a sinistra
dell'immagine, Unreal in alto a sinistra. Si gira la V: `v' = 1 − v`. Le
immagini, intanto, si decodificano dall'alto in basso (il decodificatore SGI
le gira: il formato le scrive dal basso).

---

## 21.3 OpenFlight in dieci minuti

Un file OpenFlight è una sequenza di **record**, in **big-endian**:

```
[opcode: 2 byte][lunghezza: 2 byte][contenuto...]
```

L'albero non è scritto con i figli dentro i genitori, ma con due record
speciali, **Push** (scendo di un livello) e **Pop** (risalgo):

```
Header
  Push
    Group "carro"
    Push
      Object "scafo"
      Push
        Face, Vertex List      <- un poligono
        Face, Vertex List
      Pop
      DOF "torretta"
      Push
        Object "torretta" ...
      Pop
    Pop
  Pop
```

Il lettore (`Models/OpenFlightReader.h`) tiene una **pila**: Push mette
l'ultimo nodo letto in cima, Pop lo toglie. Un nodo nuovo è figlio di quello
in cima. Un errore classico (l'abbiamo fatto e il test l'ha trovato): dopo un
Pop, "l'ultimo nodo letto" è di nuovo quello in cima alla pila, non quello
che si è appena chiuso, altrimenti il Push successivo attacca i figli al
posto sbagliato.

Prima della gerarchia ci sono le **palette**: colori, texture, materiali. Le
facce non scrivono "rosso" o "carro.rgb", ma un **indice** nella palette.
Anche i vertici stanno in una palette (il record Vertex Palette, con i suoi
offset) e le facce li nominano per posizione.

Le cose che la specifica dice in fretta e che contano:

- **i bit dei flag si contano dal più significativo**: "bit 0" è `0x80000000`;
- un record più lungo di 65535 byte continua in record di **continuazione**,
  che vanno attaccati al precedente;
- il **record di matrice** è a vettore riga (la traslazione nell'ultima
  riga): va trasposto per le nostre matrici a vettore colonna;
- le **istanze** (un pezzo definito una volta e usato in più posti, come le
  ruote) si **clonano** dove sono usate;
- i **riferimenti esterni** sono altri `.flt` (un pilota, un carico, un intero
  edificio): si leggono ricorsivamente, fino a 8 livelli, e si attaccano sotto
  il loro nodo;
- le facce **nascoste** e i **punti luce** non sono geometria; i punti luce
  restano da parte per le luci (M7), i **codici IR** delle facce e dei
  materiali per la termica (M4);
- un record che non conosciamo, seguito da un Push, diventa un **gruppo**:
  meglio un nodo senza significato che perdere i figli.

---

## 21.4 Il rig: parti solo dove servono

Un modello di OpenFlight ha spesso **centinaia** di gruppi e oggetti:
l'organizzazione di chi l'ha fatto ("ala sinistra", "carrello", "insegne").
Se ognuno diventasse un componente di Unreal, ognuno avrebbe il suo costo di
disegno, la sua trasformazione da aggiornare, il suo posto nella scena. Con
400 entità a schermo sarebbero decine di migliaia di componenti.

Ma un gruppo che **non si muove e non si spegne** non ha bisogno di esistere
nel disegno: la sua trasformazione si può **cuocere nei vertici**. Il rig tiene
una parte solo per:

- la **radice**;
- ogni **DOF** (si muove);
- ogni figlio di **switch**, ogni **LOD**, ogni fotogramma di **sequenza** (si
  accende e si spegne).

Tutto il resto finisce nella parte più vicina sopra di lui. Il carro dei test
(gruppi, oggetti, matrici, una torretta, uno switch, un LOD, una sequenza)
diventa 8 parti. Ogni parte ha le sue **sezioni**, una per materiale, e le
**condizioni** per essere visibile: quale figlio di quale switch, fra quali
distanze.

---

## 21.5 Un DOF: Prima · Moto · Dopo

Un DOF di OpenFlight è un nodo che si muove, con un **suo** sistema di
riferimento: un'origine (il perno), un asse x, un piano xy. La torretta di
un carro ha l'origine al centro della ralla e l'asse z verso l'alto; il
tettuccio di un aereo ha l'origine sulla cerniera e l'asse x lungo la
cerniera.

Il moto avviene **negli assi del DOF**:

```
Moto(valori) = T(x, y, z) · Rz(imbardata) · Rx(beccheggio) · Ry(rollio) · S
```

ma la geometria della parte è scritta nel sistema della **madre**. Quindi:

```
trasformazione della parte = Prima · Moto(valori) · Dopo

Prima = (trasformazioni sopra) · Frame       dal DOF alla madre
Dopo  = Frame⁻¹ · (trasformazioni sopra)⁻¹  dalla madre al DOF
```

Leggila da destra: porta il punto nel sistema del DOF, muovilo lì, riportalo
indietro. Con i valori a zero `Moto` è l'identità, `Prima · Dopo` si
semplifica e la parte resta dov'era: **un modello a riposo si disegna com'è
stato fatto**. Il test della torretta lo controlla, e poi controlla che il
perno non si muova quando la torretta gira (il centro della ralla resta a
1,524 m di altezza).

I limiti del file (`imbardata −180..180`, `beccheggio −10..20`) valgono
sempre: `ClampDof` ferma il valore al limite.

---

## 21.6 Switch, LOD, sequenze

**Lo switch** ha N figli e alcune **maschere**: ogni maschera dice quali figli
sono accesi. Lo switch del danno ha di solito due maschere, intatto e
distrutto; quello del carrello, su e giù. Il rig tiene le maschere del file
e quella di partenza; lo **stato del rig** di ogni entità dice quale maschera
è accesa adesso (`geo.Entities.Switch`).

**Il LOD** (livello di dettaglio) ha due distanze, *switch in* e *switch out*,
e un **centro**: è visibile se la camera è fra le due distanze **da quel
centro**. Un modello ha di solito tre o quattro LOD, dal più ricco (vicino) al
più povero (lontano). La distanza si misura nel sistema del modello: la
camera si porta nel sistema dell'attore, poi in metri.

**La sequenza** è un'animazione a fotogrammi: un figlio per fotogramma (le
eliche in tre posizioni, la fiamma che tremola). Nel rig è uno switch con una
maschera per fotogramma; per ora si vede il primo, farla girare è M3.

---

## 21.7 Le texture: dove sono, e come si leggono

Un `.flt` nomina le sue texture con il percorso della macchina dove è stato
fatto: `D:\Progetti\db\tex\carro.rgb`. Sulla nostra macchina quel percorso
non esiste. `ResolveReferencedFile` (`Models/ModelFiles.h`) le cerca in
ordine:

1. com'è scritto (a volte funziona);
2. relativo al modello;
3. **il nome** accanto al modello;
4. nelle cartelle `textures`, `Textures`, `tex` accanto al modello;
5. nella cartella sopra;
6. **per nome in tutta la cartella dei modelli** (un indice fatto una volta).

Le maiuscole non contano (Windows non le distingue, e i database nati su
Windows le mescolano). Una texture che non si trova si conta e si dice; la
faccia resta del suo colore.

**Il formato SGI** (`.rgb`, `.rgba`, `.int`, `.inta`) è quello di OpenFlight,
nato sulle stazioni Silicon Graphics. È semplice: un'intestazione di 512 byte,
poi i canali **uno dopo l'altro** (tutto il rosso, poi tutto il verde...),
riga per riga **dal basso**, grezzi o compressi a corse (RLE). Unreal non lo
conosce: lo decodifica `Models/SgiImage.h`. Gli altri (PNG, JPEG, BMP, TGA...)
li decodifica l'`ImageWrapper` di Unreal.

---

## 21.8 In Unreal

### Due tempi

Leggere un modello vero vuol dire leggere un file da decine di megabyte, i
suoi riferimenti esterni, decine di texture. Sul game thread fermerebbe il
frame per secondi. Il lavoro si divide (`FGeoModelLibrary`):

```
thread di lavoro (UE::Tasks)               game thread (un modello per frame)
─────────────────────────────              ──────────────────────────────────
file, riferimenti esterni                  UTexture2D (con le mip già fatte)
FModel -> BuildRig                         istanze dei materiali
texture decodificate in BGRA, mip          una UStaticMesh per parte
                   └──── coda MPSC ────────┘
```

Il thread di lavoro non tocca niente di Unreal che viva sul game thread (gli
`UObject` si creano solo lì); il game thread non legge file. Fra i due, la
stessa coda MPSC delle entità.

La mesh si costruisce come quella degli alberi: una `FMeshDescription` per
parte, `BuildFromMeshDescriptions` con `bFastBuild`, che funziona anche nel
gioco impacchettato (senza l'editor).

### Uno per tutti

Un modello si legge **una volta**. La chiave è il file **più il testo della
regola**: lo stesso file con due correzioni sono due rig. Mesh, texture e
materiali sono condivisi fra tutte le entità con quella regola; ogni entità ha
solo il suo **stato del rig** (i valori dei DOF, le maschere degli switch).
Mille auto uguali: un modello, mille stati.

Mentre il modello arriva, l'entità ha le scatole del suo dominio; quando è
pronto, l'attore si rifà. Un modello che non si legge lascia le scatole in
magenta, e il nome dice "(modello non letto)".

### I materiali

Tre materiali, creati da `geo.Imagery.CreateMaterial`:

| | quando | |
|---|---|---|
| `M_GeoEntity` | il caso normale | opaco, una faccia |
| `M_GeoEntityMasked` | a due facce, o la texture ha l'alfa | ritagliato (alberi, reti, griglie) |
| `M_GeoEntityTranslucent` | il materiale è trasparente | vetri, cupole |

Il colore è **colore del vertice × texture × tinta**. Il colore del vertice
viene dal file (OpenFlight lo mette sulla faccia, OBJ sul materiale); è in
sRGB, e il materiale lo eleva al quadrato per tornare lineare (una buona
approssimazione della curva sRGB, e costa una moltiplicazione).

### L'attore

```
Root (posizione e assetto della Fase 7)
└─ "Modello" (scala 100: il rig è in metri, Unreal in centimetri)
   ├─ parte 0, radice     (UStaticMeshComponent)
   ├─ parte 1, DOF        (la sua trasformazione cambia con i valori)
   │  └─ parte 2 ...
   └─ ...
```

Le parti sono in metri sotto un componente scalato di 100: la conversione
delle unità sta in un componente, non in ogni vertice né in ogni
trasformazione. I LOD si scelgono quattro volte al secondo, con la scelta
degli attori. Nessuna collisione: arriva con M5.

### I comandi

| Comando | |
|---|---|
| `geo.Entities.ModelFolder [cartella]` | la cartella dei modelli, anche fuori dal progetto |
| `geo.Entities.ModelInfo <file>` | cosa c'è in un modello: misure, DOF, switch, texture trovate e no |
| `geo.Entities.ReloadModels` | rilegge tutto |
| `geo.Entities.Dof <entità> [dof] [imb] [becc] [rollio] [x y z]` | muove una parte (senza dof: l'elenco) |
| `geo.Entities.Switch <entità> <switch> <maschera>` | sceglie una maschera |
| `geo.Entities.Models` | elenca anche i modelli letti, con i tempi e gli avvisi |

Fuori da Unreal, `geomodel_info <modello> [cartella]` (in
`Tools/StandaloneTests`) dice le stesse cose: è il primo controllo su un
modello nuovo.

---

## 21.9 Che cosa non si legge, e perché

**FBX** è un formato chiuso: l'SDK di Autodesk non si può ridistribuire in un
plugin, e il formato binario non è documentato. Si converte (FBX2glTF,
Blender) verso glTF o OBJ, oppure si importa nell'editor come asset e la
regola usa il percorso dell'asset.

**IVE** (e `.osgb`) è il formato binario **interno** di OpenSceneGraph:
cambia con la versione della libreria, nessuno lo legge fuori da OSG. Si
converte con lo strumento di OSG: `osgconv modello.ive modello.flt` (o
`.obj`).

Il caricatore li riconosce e dice il comando, invece di "formato
sconosciuto". glTF arriva con M2, insieme a `geoworld convert-models`.

---

## 21.10 Che cosa manca

La scaletta è nella sezione 11 di `modelli-design.md`:

- **M2**: glTF 2.0 e la conversione automatica dei formati chiusi;
- **M3**: articolazioni e animazione (DIS, CIGI, curve, regole procedurali
  come i rotori, cavi a catenaria, carichi che si staccano);
- **M4**: la **termica**, per i sensori IR: zone a parametri concentrati,
  sole, cielo, vento, motori;
- **M5**: collisioni e interrogazioni (HAT/HOT, linea di vista);
- **M6**: il danno;
- **M7**: luci (i punti luce di OpenFlight), assetto su terreno e onde,
  effetti.

---

## Riassunto

- Due rappresentazioni: **`FModel`** fedele al file, **`FModelRig`** pronto da
  disegnare. Le conversioni stanno in **un punto solo**, `BuildRig`.
- Le trappole: la **mano** (riflessione, triangoli da girare), l'**avanti** non
  garantito (le correzioni della regola), gli **angoli di OpenFlight** che
  girano a sinistra, le **unità** e le **UV**.
- OpenFlight è una sequenza di record con **Push/Pop** e **palette**; i
  dettagli che contano (bit dal più significativo, matrici a vettore riga,
  istanze, riferimenti esterni) hanno ognuno un test.
- Il rig tiene **parti solo dove servono**: radice, DOF, switch, LOD,
  sequenze. Il resto si cuoce nei vertici.
- Un DOF è **Prima · Moto · Dopo**: a riposo il modello è com'è stato fatto.
- Le texture si **cercano** (il percorso del file è di un'altra macchina);
  SGI la decodifichiamo noi.
- In Unreal: il file su un **thread di lavoro**, gli oggetti sul **game
  thread**, un modello per frame; un modello per tutti, uno **stato del rig**
  per entità.

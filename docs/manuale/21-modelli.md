# Capitolo 21 — I modelli: dai file del database alle parti che si muovono

> Come un aereo smette di essere una scatola: leggere un modello da una
> cartella qualsiasi (OpenFlight, OBJ, glTF), le sue texture, la sua
> gerarchia, le parti che si muovono (DOF, nodi animati) e quelle che si
> accendono e si spengono (switch, LOD), le animazioni dei file, e farne
> attori di Unreal senza fermare il frame; e convertire i formati chiusi.
> Il capitolo di progetto è `docs/modelli-design.md`; come provarlo è in
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
.gltf ─┘                                       │
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
| `geo.Entities.Animation <entità> [animazione] [loop\|once] [velocità]` | fa girare un'animazione del file (21.11) |
| `geo.Entities.Models` | elenca anche i modelli letti, con i tempi e gli avvisi |

Fuori da Unreal, `geomodel_info <modello> [cartella]` (in
`Tools/StandaloneTests`) dice le stesse cose: è il primo controllo su un
modello nuovo.

---

## 21.9 Che cosa non si legge, e perché

**FBX** è un formato chiuso: l'SDK di Autodesk non si può ridistribuire in un
plugin, e il formato binario non è documentato. **IVE** (e `.osgb`) è il
formato binario **interno** di OpenSceneGraph: cambia con la versione della
libreria, nessuno lo legge fuori da OSG.

Scrivere un lettore "a tentativi" per formati così è un lavoro enorme che si
rompe alla versione dopo. Si convertono invece **una volta**, fuori da Unreal,
con gli strumenti di chi li ha fatti: `geoworld convert-models` (21.12). Il
caricatore li riconosce e dice il comando, invece di "formato sconosciuto".
Per un FBX resta anche la strada dell'editor: importarlo come asset e usare
il suo percorso `/Game/...` nella tabella.

---

## 21.10 glTF: il "JPEG del 3D"

glTF 2.0 è il formato aperto dei modelli del Khronos Group (quelli di OpenGL
e Vulkan). Due forme: `.gltf`, un testo JSON con accanto i dati binari
(`.bin`) e le immagini; `.glb`, tutto in un file (un pezzo JSON e un pezzo
binario).

### Come è fatto

Il JSON non contiene i numeri dei vertici: dice **dove trovarli**. È una
catena di quattro passi:

```
buffers       i byte (un .bin, un "data:" in base64, il pezzo binario del .glb)
  └ bufferViews     una fetta di un buffer (da dove, quanto, con che passo)
      └ accessors       come leggere la fetta: quanti elementi, di che tipo
                        (SCALAR, VEC2, VEC3, VEC4, MAT4), con che numeri
          └ meshes          le primitive: POSITION -> accessor 3, NORMAL -> accessor 4...
```

Un **accessor** può leggere float, ma anche interi (un byte, due byte) e
"normalizzati": un byte 255 vale 1,0, uno short 32767 vale 1,0. Così si
risparmia memoria (le UV in un byte ciascuna). Può anche essere **sparse**:
una base, più pochi elementi che cambiano.

Le **primitive** sono triangoli, o strisce (ogni vertice nuovo fa un
triangolo con i due prima, girando il verso a quelli dispari), o ventagli
(tutti attorno al primo vertice). Il lettore le porta tutte a triangoli.

Sopra c'è l'albero dei **nodi**: ognuno con una matrice, oppure con
traslazione, rotazione (un **quaternione**) e scala, e una mesh, e dei figli.

### Le convenzioni

| | glTF |
|---|---|
| avanti | +Z |
| alto | +Y |
| destra | **−X** (sì: +X è la sinistra) |
| unità | metri |
| UV | origine in **alto**, come Unreal |
| colori | **lineari** |

Le prime quattro righe le gestisce `BuildRig` come per gli altri formati.
Le UV non si girano: un test lo fissa, perché girarle "per abitudine" è
proprio l'errore da non fare. I colori lineari si scrivono nei vertici con la
**radice quadrata**, perché il materiale li eleva al quadrato (21.8): 0,25
lineare diventa 0,5 nel vertice, e torna 0,25.

### I materiali PBR

glTF descrive i materiali come li disegna un motore moderno (*physically
based rendering*): colore di base, **metallo** (0 plastica, 1 metallo),
**rugosità** (0 specchio, 1 opaco), luce propria, e come usare l'**alfa**:

- `OPAQUE`: l'alfa non conta;
- `MASK`: si ritaglia sotto una soglia (foglie, reti);
- `BLEND`: trasparente (vetri).

Metallo e rugosità diventano parametri dei materiali `M_GeoEntity*`. La
soglia del ritaglio, in Unreal, è una proprietà fissa del materiale (0,5),
non un parametro: la soglia del file si ottiene **spostando l'alfa**, alfa +
0,5 − soglia. Un trucco piccolo che evita un materiale per ogni soglia.

Attenzione a un default: un materiale glTF senza fattori ha metallo **1** (la
specifica dice così). Un modello che sembra troppo scuro e metallico spesso è
scritto così davvero.

### Quello che non disegniamo ancora

Gli **scheletri** (persone, animali): la mesh si deforma una volta, alla
lettura, con le ossa nella loro posa del file, e resta così. I **morph
target** (le facce che cambiano espressione): ignorati. Le texture normali e
di occlusione: ignorate (i fattori no). Le compressioni (Draco, meshopt): un
errore che dice come riesportare.

---

## 21.11 Le animazioni dei file

Un'animazione di glTF è un insieme di **canali**: ognuno muove una cosa
(traslazione, rotazione o scala) di un nodo, con una **curva**: dei tempi e
dei valori.

### Le curve

Fra due chiavi il valore si **interpola** in tre modi:

- **a gradino**: il valore resta quello della chiave prima, poi salta;
- **lineare**: in linea retta. Per le rotazioni non si può fare una media dei
  quaternioni: si usa lo **slerp** (*spherical linear interpolation*), che va
  da una rotazione all'altra a velocità costante, per la via più corta;
- **cubica** (Hermite): ogni chiave ha anche due **tangenti** (la velocità
  in entrata e in uscita), e la curva passa per i valori con quelle
  pendenze. Con le tangenti a zero è una "S" morbida.

Prima della prima chiave vale la prima, dopo l'ultima l'ultima. Il giro
(*loop*) lo decide chi fa girare l'animazione, non la curva.

### Un nodo animato è una parte

Un nodo che si muove non si può cuocere nei vertici (21.4): diventa una
**parte**, come un DOF. Anche la formula è la stessa:

```
trasformazione della parte = Prima · Posa(t) · Dopo
```

con Prima = le trasformazioni sopra il nodo, Dopo = l'inverso della
conversione degli assi, e la Posa (traslazione, quaternione, scala) **nelle
coordinate del file**, come i valori dei DOF di OpenFlight. A riposo la posa
è quella del file, e il modello è com'è stato fatto.

Anche qui gli angoli sono **matematici**: una rotazione positiva attorno
all'alto gira a **sinistra**. È così in glTF, in Blender, in OpenFlight. Un
test con un carro fatto davvero in Blender lo fissa: +90 gradi, la canna
finisce a sinistra.

### In Unreal

Ogni entità ha la sua animazione che gira (quale, da quando, a che velocità,
in loop o una volta) e la sua **posa** nello stato del rig. A ogni frame,
dopo la posizione dell'attore, si calcola la posa di adesso e si aggiornano
le parti animate. Un modello, cento entità, cento pose.

- `geo.Entities.Animation <entità>`: l'elenco delle animazioni;
- `geo.Entities.Animation <entità> <nome|numero> [loop|once] [velocità]`:
  la fa girare (velocità negativa: all'indietro);
- `stop` la ferma dov'è, `reset` torna a riposo;
- nella tabella, `anim <nome>` la fa girare sempre (un radar, un rotore),
  ogni entità in un punto diverso del giro: cento radar non girano in fase.

---

## 21.12 La conversione: `geoworld convert-models`

```bat
geoworld convert-models D:\db\Models
```

Cammina nella cartella (anche tutto il database) e converte ogni modello che
GeoWorld non legge, con lo strumento giusto:

| Da | A | Con |
|---|---|---|
| `.ive`, `.osgb`, `.osgt`, `.osg` | `.flt` | `osgconv` (OpenSceneGraph): `.flt` è l'unico formato aperto che conserva DOF, switch e LOD |
| `.fbx` | `.glb` | FBX2glTF, o Blender |
| `.dae`, `.3ds`, `.blend`, `.stl`, `.ply`... | `.glb` | Blender, in background (senza finestra), con uno script nostro |

Come `make`: un modello già convertito, più recente dell'originale, si salta.
Senza `-o` il file nuovo va **accanto** all'originale: la cartella dei
modelli resta la stessa e le texture si trovano dove erano. Uno strumento che
manca non ferma il resto: il comando elenca i file rimasti e dice come
procurarselo.

I test usano strumenti **finti** (tre script che si comportano come
`osgconv`, FBX2glTF e Blender): provano tutto quello che è nostro. Lo script
di Blender è stato provato con Blender vero.

---

## 21.13 Che cosa manca

La scaletta è nella sezione 11 di `modelli-design.md`:

- **M3**: le articolazioni: i parametri articolati di DIS e CIGI legati ai
  DOF e ai nodi (con il segno degli angoli), le curve nostre, le regole
  procedurali (rotori, ruote, eliche), le sequenze di OpenFlight che girano,
  i cavi a catenaria, i carichi che si staccano;
- **M4**: la **termica**, per i sensori IR: zone a parametri concentrati,
  sole, cielo, vento, motori;
- **M5**: collisioni e interrogazioni (HAT/HOT, linea di vista);
- **M6**: il danno;
- **M7**: luci (i punti luce di OpenFlight e di glTF), assetto su terreno e
  onde, effetti.

---

## Riassunto

- Due rappresentazioni: **`FModel`** fedele al file, **`FModelRig`** pronto da
  disegnare. Le conversioni stanno in **un punto solo**, `BuildRig`.
- Le trappole: la **mano** (riflessione, triangoli da girare), l'**avanti** non
  garantito (le correzioni della regola), gli **angoli** matematici che
  girano a sinistra (OpenFlight, glTF, Blender), le **unità**, le **UV**
  (girate per OpenFlight e OBJ, non per glTF), i **colori lineari** di glTF.
- OpenFlight è una sequenza di record con **Push/Pop** e **palette**; glTF è
  un JSON che dice dove sono i numeri (buffer, fette, accessor).
- Il rig tiene **parti solo dove servono**: radice, DOF, nodi animati,
  switch, LOD, sequenze. Il resto si cuoce nei vertici.
- Un DOF è **Prima · Moto · Dopo**, un nodo animato **Prima · Posa · Dopo**:
  a riposo il modello è com'è stato fatto.
- Le curve: a gradino, lineari (slerp per le rotazioni), cubiche di Hermite.
- Le texture si **cercano** (il percorso del file è di un'altra macchina), o
  sono **dentro** il file (glTF); SGI la decodifichiamo noi.
- In Unreal: il file su un **thread di lavoro**, gli oggetti sul **game
  thread**, un modello per frame; un modello per tutti, uno **stato del rig**
  (DOF, pose, switch) per entità.
- I formati chiusi si **convertono** una volta, con gli strumenti di chi li
  ha fatti.

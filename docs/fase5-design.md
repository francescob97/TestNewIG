# Fase 5 — Mesh e gonne: decisioni e motivazioni

Codice in `Plugins/GeoWorld/Source/GeoRender/Public/Mesh` e
`Public/Terrain` + `Private/Terrain`.
Per la verifica vedi `docs/fase5-verifica.md`.

---

## 1. Cosa fa questa fase

La Fase 4 decideva *quali* tile disegnare e disegnava soltanto la propria
decisione: rettangoli colorati. Questa fase prende quella lista, legge le quote
dalla cache della Fase 3 e produce **geometria vera**. Da qui in avanti sullo
schermo c'e' terreno.

Tre pezzi, in tre punti diversi della piramide di astrazione:

| Pezzo | Dove | Strato |
|---|---|---|
| Da 129x129 quote a vertici, normali, UV, indici | `Mesh/TileMesh.h` | C++ puro, header-only |
| Da mesh astratta a qualcosa che Unreal disegna | `Terrain/IGeoTerrainMeshProvider` | interfaccia |
| Chi chiama chi, con che budget, e chi pulisce | `Terrain/UGeoTerrainSubsystem` | Unreal |

---

## 2. In che sistema stanno i vertici (e perche' non ENU)

I vertici sono in un frame **locale alla tile**, in **metri**, con assi
`X = Nord`, `Y = Est`, `Z = Alto`: NEU, non ENU.

Non e' una preferenza estetica, e' la stessa trappola della Fase 1. ENU e'
destrorso, lo spazio di Unreal e' sinistrorso. Una mesh costruita in ENU
richiederebbe, sul componente, una trasformazione con determinante -1, cioe' una
riflessione. `FMatrix::ToQuat()` su una matrice del genere **non fallisce e non
avverte: restituisce silenziosamente spazzatura**, e il terreno comparirebbe
specchiato — un bug che si manifesta come "la valle e' dalla parte sbagliata",
il tipo di cosa che si insegue per giorni.

In NEU la trasformazione del componente e' esattamente quella che
`FGeoreferenceSnapshot::GetLocalNeuTransform()` gia' produce, ed e' una
rotazione propria (determinante +1, verificato dal test 9 della Fase 1).

### Perche' locale alla tile e non in spazio mondo

Questo e' il punto in cui il lavoro della Fase 1 viene incassato.

I vertici sono `float`: e' il formato dei vertex buffer, non e' negoziabile. Un
`float` alla distanza del raggio terrestre ha un ULP di **64 cm**: una mesh in
coordinate mondo assolute si spappolerebbe visibilmente. Relativi al centro
della propria tile i vertici restano entro pochi chilometri, dove il `float` e'
millimetrico. I test misurano l'ULP effettivo: **0.0039 m al livello 8, 0.0001 m
al livello 14** — sotto il millimetro dove il dettaglio conta.

Conseguenza operativa, ed e' la piu' importante di tutta la fase:

> **Un rebase non rigenera un solo vertice.** Cambia soltanto la trasformazione
> dei componenti.

E' esattamente cio' che fa `RefreshTransforms()`, ed e' il motivo per cui
l'interfaccia del provider la espone come metodo di primo livello invece di
nasconderla: se qualcuno in futuro implementasse un provider che ricostruisce la
geometria a ogni rebase, l'interfaccia deve renderglielo evidente come errore.

I valori restano in **metri** dentro lo strato puro. La conversione in unita'
Unreal avviene in un punto solo, nel provider, come scala del componente:

```cpp
Transform.SetScale3D(FVector(GeoWorld::Units::MetersToUu));
```

La regola del punto unico di conversione (Fase 1) vale anche qui: il numero 100
non compare da nessun'altra parte.

---

## 3. La griglia: 129x129 post, 128x128 celle

I post sono registrati sui nodi (Fase 2), quindi il bordo di una tile e'
condiviso con la vicina: l'ultima colonna di una e' la prima dell'altra, con gli
**stessi valori sorgente**. Non e' un dettaglio del formato, e' cio' che rende le
giunzioni fra tile dello **stesso livello** esatte per costruzione: non serve
nessun cucito, nessuna media di bordo.

I test lo verificano nello spazio, non sulla carta: due tile adiacenti costruite
in **frame locali diversi**, riportate entrambe in ECEF, hanno i 129 vertici di
bordo coincidenti a **0.0000 m in est-ovest e 0.0001 m in nord-sud**. Il residuo
e' la precisione del `float`, non un disallineamento.

Da qui segue il corollario che guida tutto il resto:

> Le crepe non esistono fra tile dello stesso livello. Esistono **solo** fra
> livelli diversi. Le gonne servono a quello, e a nient'altro.

**Normali** per differenze centrali sulle posizioni (non sulle quote: le
posizioni tengono conto della curvatura), `Nord x Est`. Sul bordo della tile la
differenza e' unilaterale perche' non si guarda la tile vicina: la geometria
resta continua, l'illuminazione puo' mostrare una lievissima cucitura. E' una
scelta consapevole — leggere la tile vicina significherebbe accoppiare la
generazione della mesh alla disponibilita' dei vicini, cioe' rendere non
deterministico l'unico pezzo puro della fase. Se la cucitura si vedra', si
risolve nello shader.

**UV** `(I/128, J/128)`: preparate per la Fase 6, dove l'ortofoto verra'
drappeggiata su questa stessa griglia.

---

## 4. Le gonne: da dove viene il numero

La crepa fra due livelli nasce cosi': il bordo della tile grossolana e' un
segmento fra due suoi post; la tile fine ha post intermedi che seguono il
terreno. Lo scarto verticale massimo fra i due e' il dislivello del terreno su
mezzo intervallo del livello grossolano — pendenza x spacing.

La gonna e' una striscia verticale che scende dal bordo e riempie quella
fessura. Con una pendenza fino a 4 (76 gradi, piu' ripido di qualunque versante
reale) e l'intervallo grossolano pari al doppio del nostro:

```
profondita' = spacing_del_livello x 111132 m/grado x 8
```

Al livello 13 sono **152.6 m**, e raddoppia salendo di livello, esattamente come
l'errore geometrico della Fase 4 — il che e' una buona conferma incrociata:
gonna ed errore geometrico misurano la stessa grandezza fisica.

Abbondante quanto basta perche' la gonna non si veda mai spuntare, non tanto da
produrre pareti visibili. La si spegne con `geo.Terrain.Skirt 0`, ed e' la prova
piu' istruttiva della fase (vedi la verifica).

Le quattro strisce sono percorse in modo che l'interno resti a sinistra, cosi'
l'orientamento e' coerente con la superficie e la gonna non appare trasparente
da fuori.

---

## 5. `bFlipWinding`, ovvero l'onesta' sulle cose che non si deducono

La convenzione di faccia frontale di Unreal, combinata con la scala del
componente e con la mano del frame, e' una di quelle cose che **si verificano
guardando lo schermo**, non ragionando. Nessuna riga di questa sessione e' mai
stata compilata dentro il motore.

Invece di indovinare e lasciare all'utente un terreno invisibile senza
spiegazione, la direzione e' un parametro (`bFlipWinding`, default `true`)
commutabile a caldo con `geo.Terrain.FlipWinding`. Se il terreno si vede da
sotto e non da sopra, e' un comando, non una ricompilazione.

---

## 6. L'interfaccia del provider: la richiesta esplicita

Era nel brief iniziale: partire da `UDynamicMeshComponent` per semplicita', **ma
isolato dietro un'interfaccia** per poterlo sostituire con un
`FPrimitiveSceneProxy` senza riscrivere il resto.

```cpp
class GEORENDER_API IGeoTerrainMeshProvider
{
public:
    virtual FString GetName() const = 0;
    virtual void Initialize(UWorld* World) = 0;
    virtual void Shutdown() = 0;
    virtual bool CreateOrUpdateTile(const FTileKey&, const FTileMeshData&, const FTransform&) = 0;
    virtual void RemoveTile(const FTileKey&) = 0;
    virtual void RemoveAllTiles() = 0;
    virtual void RefreshTransforms(const FGeoreferenceSnapshot&) = 0;
    virtual int32 GetTileCount() const = 0;
    virtual void SetWireframe(bool bInWireframe) = 0;
    virtual bool IsWireframe() const = 0;
};
```

Quello che l'interfaccia **non** contiene e' informativo quanto quello che
contiene: niente `UDynamicMeshComponent`, niente `FDynamicMesh3`, niente
`UMaterialInterface`. Un provider basato su scene proxy custom implementa gli
stessi dieci metodi e il subsystem non se ne accorge.

`FDynamicMeshTerrainProvider` e' l'implementazione di oggi: un
`UDynamicMeshComponent` per tile, sotto un attore contenitore transient.
Costruisce la mesh con `Component->EditMesh(...)` popolando vertici, overlay
delle normali e overlay delle UV, e chiude con `NotifyMeshUpdated()`.

E' la scelta lenta ma corta: `UDynamicMeshComponent` fa tutto (collisione,
materiali, editor) al prezzo di una conversione e di un ricalcolo interno per
tile. La Fase 4 misura il costo della selezione, questa fase espone il costo
della costruzione nell'overlay: quando quel numero diventera' il collo di
bottiglia, si sostituisce il provider, non il resto.

---

## 7. Il subsystem: budget, pin, pulizia

`UGeoTerrainSubsystem` e' un `UTickableWorldSubsystem` (la stessa lezione della
Fase 1: `UWorldSubsystem` + `FTickableGameObject` a mano fallisce in silenzio se
si dimentica `GetTickableGameObjectWorld()`).

Ogni frame:

1. prende la selezione della Fase 4;
2. **pinna** nella cache le tile selezionate — chiude esplicitamente la
   limitazione lasciata aperta dalla Fase 4, dove una tile in uso poteva essere
   sfrattata e ricaricata subito dopo;
3. costruisce la geometria di al massimo `MaxTilesPerFrame` tile (default 4);
4. rimuove le tile non piu' selezionate e **le spinna**.

Il budget per frame esiste perche' `BuildTileMesh` gira **sul game thread**.
Trentamila triangoli per tile sono pochi, ma quaranta tile nuove nello stesso
frame sarebbero uno scatto visibile. Quattro per frame significa che una
tassellatura completamente nuova si riempie in una decina di frame, che a video
si legge come un caricamento progressivo, non come un blocco.

E' una scelta, non un limite: `BuildTileMesh` e' **puro e senza stato**, quindi
spostarlo sul pool di thread della Fase 3 e' una modifica localizzata a una
funzione. Non e' stato fatto ora perche' non c'e' misura che dica che serve, e
introdurre concorrenza prima di avere un numero e' il modo classico di pagare
complessita' per niente.

### Il bug che ho trovato scrivendo questa parte

La prima versione teneva `BuiltTiles` come mappa da chiave impacchettata
(`uint64`) a conteggio triangoli. Ma quella chiave **non e' invertibile**: al
momento di rimuovere una tile non c'era piu' modo di ricostruire la `FTileKey`
da passare a `Provider->RemoveTile()`. Risultato: la geometria non veniva mai
rimossa e il pin non veniva mai sciolto — cioe' esattamente i due problemi che
questa fase doveva risolvere, reintrodotti dalla struttura dati.

`FBuiltTile` ora contiene la `FTileKey` intera. Costa 16 byte per tile
residente.

---

## 8. Debug

Il brief chiedeva debug fin dall'inizio e non come extra. Qui sono sette comandi:

```
geo.Terrain.Demo <cartella>    apre un dataset, si posiziona, accende tutto
geo.Terrain.Enable <0|1>       costruzione della geometria
geo.Terrain.Wireframe <0|1>    reticolo dei triangoli
geo.Terrain.Skirt <0|1>        gonne — spegnile per VEDERE le crepe
geo.Terrain.FlipWinding <0|1>  orientamento delle facce
geo.Terrain.Budget <N>         tile costruite per frame
geo.Terrain.Stats              statistiche dell'ultimo frame
```

`geo.Terrain.Skirt 0` merita una riga a parte: non e' un interruttore di
comodo, e' **lo strumento che dimostra la tesi di questa fase**. Spegnendo le
gonne le crepe compaiono, e compaiono *solo* dove due livelli si toccano. E'
la verifica visiva del ragionamento della sezione 3.

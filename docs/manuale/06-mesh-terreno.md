# Capitolo 6 — Dalle quote ai triangoli (Fase 5)

> Il capitolo in cui il terreno diventa visibile. Contiene anche la storia di un
> terreno che "non si vedeva", che è la lezione di debug più utile di tutto il
> progetto.

---

## L'idea in breve

Il quadtree ha deciso quali tile disegnare. Ora bisogna trasformare ogni tile —
una griglia di 129 × 129 quote — in **triangoli** che Unreal sa disegnare.

Il lavoro si divide in tre pezzi, ognuno in uno strato diverso:

| Pezzo | Dove | Strato |
|---|---|---|
| Da quote a vertici, normali, UV, triangoli | `Mesh/TileMesh.h` | C++ puro |
| Da triangoli a qualcosa che Unreal disegna | `IGeoTerrainMeshProvider` | interfaccia |
| Chi costruisce cosa, quando, con che budget | `UGeoTerrainSubsystem` | Unreal |

---

## 6.1 Da griglia a triangoli

Una griglia di 129 × 129 punti ha 128 × 128 **celle** quadrate. Ogni cella si
divide in **due triangoli**:

```
 TL ──── TR          TL ──── TR
  │       │           │ ╲     │
  │       │    →      │   ╲   │
  │       │           │     ╲ │
 BL ──── BR          BL ──── BR
```

128 × 128 × 2 = **32.768 triangoli** per la superficie. Più le gonne (sezione
6.5): 1.024 triangoli e 516 vertici in più. In totale **17.157 vertici e 33.792
triangoli** per tile, circa **0,91 MB** in memoria. Con 200 tile sullo schermo,
182 MB di geometria.

---

## 6.2 In che sistema stanno i vertici

Questa è la decisione centrale della fase, ed è la conseguenza diretta del
capitolo 2.

**I vertici sono in un frame locale alla tile, in metri, con assi Nord-Est-Alto.**

Ogni tile ha un'**origine** propria: il suo centro, a metà fra la quota minima e
la massima. I vertici sono espressi **relativi a quel punto**. Poi il componente
Unreal della tile riceve una trasformazione che porta il frame locale nel mondo.

### Perché locale e non coordinate mondo

I vertici sono `float` (è il formato dei vertex buffer, non si discute). In
coordinate mondo assolute, a 6.371 km dal centro della Terra, avrebbero un passo
di 64 cm. Relativi al centro della propria tile, restano entro pochi chilometri.

I test misurano la precisione effettiva:

| Livello | Distanza massima dall'origine della tile | Passo del `float` |
|---:|---:|---:|
| 8 | 39 km | 3,9 mm |
| 12 | 2,4 km | 0,2 mm |
| 14 | 612 m | 0,1 mm |

Sotto il millimetro proprio dove il dettaglio conta.

### La conseguenza più importante

> **Un rebase non ricostruisce un solo vertice.** Cambia soltanto la
> trasformazione dei componenti.

I vertici sono relativi al centro della loro tile, e quel centro non cambia con
il rebase: cambia solo **dove sta il centro in coordinate Unreal**, cioè una
manciata di numeri per tile. È il metodo `RefreshTransforms()`, ed è il vero
guadagno di tutto il lavoro della Fase 1.

### Perché NEU e non ENU

Stessa trappola del capitolo 2.6. ENU è destrorso, Unreal sinistrorso. Una mesh
in ENU richiederebbe una trasformazione del componente con determinante −1, e
`FMatrix::ToQuat()` su una riflessione **restituisce silenziosamente spazzatura**:
il terreno comparirebbe specchiato. In NEU la trasformazione è una rotazione
propria, esattamente quella che `FGeoreferenceSnapshot::GetLocalNeuTransform()`
già produce.

### I metri e il numero 100

I vertici restano **in metri**. La conversione in centimetri Unreal avviene nella
**scala** della trasformazione del componente:

```cpp
FTransform Transform = Snapshot.GetLocalNeuTransform(MeshData.Origin);
Transform.SetScale3D(FVector(GeoWorld::Units::MetersToUu));   // × 100
```

*(in `GeoRender/Private/Terrain/GeoTerrainSubsystem.cpp`)*

La regola del capitolo 2.11 vale anche qui: il 100 viene da `GeoUnits.h`.

---

## 6.3 Normali e coordinate di texture

**Le normali** (la direzione "fuori" della superficie in ogni punto, che serve
all'illuminazione) si calcolano con le **differenze centrali**: per ogni vertice,
si guarda il vicino a nord e quello a sud, quello a est e quello a ovest, e si fa
il prodotto vettoriale delle due direzioni.

Si calcolano sulle **posizioni**, non sulle quote: così tengono conto della
curvatura della Terra, che su una tile grande non è trascurabile.

> ⚠️ **Limite dichiarato.** Sul bordo della tile il vicino esterno sta nella tile
> accanto, che non si guarda. Lì si usa una differenza da un lato solo. La
> geometria resta perfettamente continua, ma l'illuminazione può mostrare una
> cucitura sottile lungo i bordi. È una scelta: leggere la tile vicina
> significherebbe far dipendere la costruzione della mesh da quali tile sono in
> memoria, cioè rendere non deterministico l'unico pezzo puro della fase. Se la
> cucitura si vedrà, si risolve nello shader.

**Le coordinate di texture** (UV) sono semplicemente `(I/128, J/128)`: 0 al bordo
nord-ovest, 1 al bordo sud-est. Erano già pronte per la Fase 6.

---

## 6.4 Le giunzioni fra tile dello stesso livello

Grazie ai 129 post con bordo condiviso (capitolo 3.3), due tile vicine dello
stesso livello hanno i vertici di bordo **negli stessi punti**. Ma sono costruite
in **frame locali diversi**, con origini diverse: coincidono ancora, una volta
portate nel mondo?

I test lo verificano nello spazio vero: costruiscono due tile adiacenti, portano
i 129 vertici di bordo di entrambe in ECEF, e li confrontano.

| Giunzione | Scarto massimo |
|---|---|
| Est-Ovest | **0,0000 m** |
| Nord-Sud | **0,0001 m** |

Il residuo è la precisione del `float`, non un disallineamento. Quindi:

> **Fra tile dello stesso livello le crepe non esistono, per costruzione.**

---

## 6.5 Le gonne: contro le crepe fra livelli diversi

Le crepe esistono invece dove si incontrano **due livelli diversi**.

### Perché si formano

Una tile di livello 12 accanto a una di livello 13. Il bordo della tile
grossolana è un segmento dritto fra due suoi post. La tile fine, sullo stesso
bordo, ha un post **in mezzo**, che segue il terreno vero. Se in quel punto c'è
una collina, il post fine è più alto del segmento grossolano:

```
         post fine (livello 13)
              ●
             ╱ ╲
            ╱   ╲         ← la tile fine segue la collina
    ●──────────────────●  ← la tile grossolana passa dritta
               ↑
         la CREPA: da qui si vede il cielo
```

Da certe angolazioni, attraverso quella fessura, si vede il vuoto.

### Le soluzioni possibili

| Soluzione | Pro | Contro |
|---|---|---|
| **Gonne** ✓ | semplice, nessuna dipendenza fra tile | geometria in più |
| Cucitura ("stitching") | geometricamente perfetta | ogni tile dipende dal livello delle vicine: si ricostruisce quando cambiano |
| Vincolo di livello (vicine differiscono al massimo di 1) | limita il problema | non lo elimina, e complica il quadtree |

### Come sono fatte

Una **gonna** è una striscia verticale che scende dal bordo della tile, come la
gonna di un tavolo. Copre la fessura dall'esterno: non chiude la crepa, ma ci
mette qualcosa dietro, e da nessuna angolazione si vede più il cielo.

### Quanto profonde

Lo scarto massimo fra i due bordi è il dislivello del terreno su mezzo intervallo
del livello grossolano: **pendenza × passo**. Con una pendenza fino a 4 (cioè
76°, più ripida di qualunque versante reale) e l'intervallo grossolano pari al
doppio del nostro:

```
profondità = passo_del_livello × 111.132 m/grado × 8
```

> 💡 **Numeri.** Al livello 13 sono **152,6 m**. Raddoppiano salendo di livello,
> esattamente come l'errore geometrico del capitolo 5: gonna ed errore misurano
> la stessa grandezza fisica.

File: `ComputeSkirtDepth()` in `GeoRender/Public/Mesh/TileMesh.h`.

> 💡 **Per vederlo.** `geo.Terrain.Skirt 0` spegne le gonne. A quota media,
> guardando di taglio, compaiono fessure di cielo **solo** dove due livelli si
> toccano, mai fra tile dello stesso livello. È la prova visiva di tutto il
> ragionamento.

---

## 6.6 L'orientamento dei triangoli

Ogni triangolo ha una faccia "davanti" e una "dietro", decisa dall'ordine dei suoi
tre vertici (orario o antiorario). Il renderer, per risparmiare, di solito
**disegna solo la faccia davanti**.

Quale verso sia "davanti" in Unreal, combinato con la scala del componente e con
la mano del frame, **è una di quelle cose che si verificano guardando lo schermo,
non ragionando**. Il codice non è mai stato compilato in Unreal da chi l'ha
scritto.

Invece di indovinare e lasciare un terreno invisibile senza spiegazione,
l'orientamento è un parametro (`bFlipWinding`, default `true`), invertibile a
caldo:

```
geo.Terrain.FlipWinding 0      (o 1)
```

Un test verifica che tutti i 32.768 triangoli abbiano lo stesso verso, e che il
parametro lo inverta davvero.

---

## 6.7 L'interfaccia del provider

Il tuo brief iniziale chiedeva: *partire da `UDynamicMeshComponent` per
semplicità, ma isolato dietro un'interfaccia, per poterlo sostituire con un
`FPrimitiveSceneProxy` senza riscrivere il resto.*

L'interfaccia è `IGeoTerrainMeshProvider`:

```cpp
class IGeoTerrainMeshProvider
{
public:
    virtual void Initialize(UWorld* World) = 0;
    virtual void Shutdown() = 0;
    virtual bool CreateOrUpdateTile(const FTileKey&, const FTileMeshData&,
                                    const FTransform&) = 0;
    virtual void RemoveTile(const FTileKey&) = 0;
    virtual void RemoveAllTiles() = 0;
    virtual void RefreshTransforms(const FGeoreferenceSnapshot&) = 0;
    virtual void SetTileDrape(const FTileKey&, UTexture2D*,
                              const FDrapeTransform&) = 0;     // Fase 6
    virtual int32 GetRealizedTriangleCount() const = 0;        // diagnosi
    // ... e qualche altro metodo di stato
};
```

*(in `GeoRender/Public/Terrain/GeoTerrainMeshProvider.h`)*

Quello che l'interfaccia **non** contiene conta quanto quello che contiene:
nessun `UDynamicMeshComponent`, nessuna `FDynamicMesh3`, nessun materiale
concreto. Un provider nuovo implementa gli stessi metodi, e il subsystem non se
ne accorge.

`RefreshTransforms()` è un metodo di primo livello, e non per caso: rende
esplicito che al rebase **si ricalcolano le trasformazioni e basta**. Se qualcuno
un giorno scrivesse un provider che ricostruisce la geometria a ogni rebase,
l'interfaccia deve farglielo sembrare un errore.

### L'implementazione di oggi: `UDynamicMeshComponent`

`FDynamicMeshTerrainProvider` crea un `UDynamicMeshComponent` per ogni tile,
tutti figli di un attore contenitore.

**Nota Unreal: come si riempie una mesh dinamica.**

```cpp
Component->EditMesh([&MeshData](UE::Geometry::FDynamicMesh3& Mesh)
{
    Mesh.Clear();
    Mesh.EnableAttributes();                     // normali e UV

    auto* Normals = Mesh.Attributes()->PrimaryNormals();
    auto* UVs     = Mesh.Attributes()->PrimaryUV();

    for (/* ogni vertice */)
    {
        Mesh.AppendVertex(Position);
        Normals->AppendElement(Normal);
        UVs->AppendElement(UV);
    }
    for (/* ogni triangolo */)
    {
        const int32 Id = Mesh.AppendTriangle(A, B, C);
        Normals->SetTriangle(Id, FIndex3i(A, B, C));
        UVs->SetTriangle(Id, FIndex3i(A, B, C));
    }
});
Component->NotifyMeshUpdated();
```

*(in `GeoRender/Private/Terrain/DynamicMeshTerrainProvider.cpp`)*

Due cose da notare. **`EditMesh` riceve una funzione** (una *lambda*): è il modo
corretto di modificare la mesh, perché così il componente sa quando deve
invalidare i dati del renderer. Modificarla fuori lascerebbe il renderer con la
versione vecchia. E le **normali e le UV** non stanno nei vertici ma in
**overlay** separati, con i propri indici: permette a un vertice di avere normali
diverse sui triangoli che lo toccano (per gli spigoli vivi). Qui ogni post ha una
normale sola, quindi gli indici coincidono con quelli dei vertici.

### Perché non è la soluzione finale

Un componente per tile significa un `UObject`, un proxy di scena e un *draw call*
per tile. Con qualche centinaio di tile diventa il collo di bottiglia. La
soluzione è un `FPrimitiveSceneProxy` scritto da zero, che disegna tutte le tile
in pochi draw call. Si scrive quando quel numero diventerà il problema — ed è per
questo che l'interfaccia esiste.

---

## 6.8 Il subsystem del terreno

`UGeoTerrainSubsystem` coordina tutto. A ogni frame:

1. prende la selezione del quadtree;
2. **pinna** nella cache le tile selezionate, perché non vengano sfrattate mentre
   si disegnano;
3. **rimuove** la geometria delle tile non più selezionate, e le spinna;
4. **costruisce** la geometria di quelle nuove, al massimo **4 per frame**.

### Il budget per frame

`BuildTileMesh()` gira **sul game thread**. Trentamila triangoli per tile sono
pochi, ma quaranta tile nuove nello stesso frame sarebbero uno scatto visibile.
Quattro per frame significano che una vista completamente nuova si riempie in una
decina di frame: a video sembra un caricamento progressivo, non un blocco.

Si potrebbe spostare la costruzione sui thread di caricamento: `BuildTileMesh()`
è pura e senza stato, quindi la modifica sarebbe piccola. Non è stato fatto
perché nessuna misura dice che serve, e aggiungere concorrenza prima di avere un
numero è il modo classico di pagare complessità per niente.

### Un bug trovato scrivendo

La prima versione teneva le tile costruite in una mappa da **chiave impacchettata**
(un intero a 64 bit) a numero di triangoli. Ma l'impacchettamento non è
invertibile: al momento di rimuovere una tile non si sapeva più **quale** tile
fosse, e non si poteva chiamare `RemoveTile()`. La geometria non veniva mai
rimossa e il pin mai sciolto — cioè esattamente i due problemi che la fase doveva
risolvere. Ora la mappa tiene la chiave intera.

---

## 6.9 La storia del terreno invisibile

Questa sezione racconta come è andato il primo avvio della Fase 5, perché
insegna come si fa debug in Unreal — e soprattutto come **non** lo si fa.

### Il sintomo

L'overlay diceva: **108 tile disegnate, 3.538.944 triangoli.** Sullo schermo:
niente. Un azzurro uniforme.

### Primo errore: il contatore misurava l'intenzione

Quel "3.538.944 triangoli" **non veniva dal renderer**. Era la somma dei triangoli
che il codice **credeva di aver costruito**. Se la mesh non fosse mai arrivata al
componente Unreal, l'overlay avrebbe mostrato esattamente lo stesso numero.

> **Lezione.** Un contatore di debug deve misurare **il risultato**, non
> l'intenzione. Altrimenti non è debug: è rumore che nasconde il problema.

Correzione: `GetRealizedTriangleCount()` legge i triangoli **dalla mesh dentro il
componente**. L'overlay mostra entrambi i numeri e diventa rosso se non
coincidono.

### Secondo errore: il comando che mentiva

Il primo tentativo è stato `geo.Terrain.FlipWinding 1`. Risposta: *"Orientamento
invertito: ON"*. Ma il default **era già** `true`: il comando non aveva cambiato
niente, e aveva risposto come se l'avesse fatto.

> **Lezione.** Un comando che non ha cambiato niente deve **dirlo**. Ora risponde
> *"era GIÀ ON, nessun cambiamento (per invertirlo: 0)"*.

### Il comando che ha risolto: `geo.Terrain.Diag`

Stampa, per le prime tile, i numeri **dal lato del renderer**:

```
L12 (4332,1012): vertici 17157  triangoli 33792  raggio 3.0 km
      distanza 19.8 km  davanti 0.82  registrato  visibile  materiale BasicShapeMaterial
```

Ogni numero si confronta con la teoria:

* **raggio 3,0 km** per una tile di livello 12 a 45° di latitudine: la
  semidiagonale di 3,4 × 4,9 km è proprio 3,0 km. ✓
* **distanza 19,8 km** da una camera a 15 km di quota che guarda in giù: ✓
* **davanti 0,82**: la tile sta a 35° dall'asse della camera, dentro il campo
  visivo. ✓
* registrata, visibile, con un materiale. ✓

**Tutto giusto.** La geometria c'era, era al posto giusto, davanti alla camera.
Quindi il problema non era **dove** stava il terreno, ma **perché non si vedeva**.
Sono domande diverse, con strumenti diversi.

### La causa vera

Due cose insieme:

* il materiale di base è **grigio senza texture**: illuminato solo dalla luce del
  cielo, riempie lo schermo di un azzurrino uniforme, **indistinguibile dal cielo
  vuoto**. Il terreno c'era, ma non si riconosceva;
* la **nebbia di default** di Unreal (`ExponentialHeightFog` + `SkyAtmosphere`),
  a 20 km di distanza, sostituisce quasi interamente il terreno con il colore del
  cielo.

### Cosa è cambiato

* `geo.Terrain.Demo` ora parte con il **wireframe acceso**: un reticolo di
  triangoli non si confonde con niente.
* La demo si posiziona a **6 km** invece di 15.
* `geo.Terrain.Boxes 1` disegna i bounds dei componenti come **linee di debug**,
  che non passano per il materiale, non hanno faccia davanti o dietro e non
  vengono nebbiate: un canale di disegno indipendente dalla mesh.
* La scaletta di debug è stampata dal comando stesso.

### La scaletta, da usare quando "non si vede niente"

```
1. geo.Terrain.Diag       i numeri sono sani? (triangoli, raggio, distanza, davanti)
2. viewmode wireframe     comando di UNREAL: ignora materiali, luci, facce e nebbia
3. geo.Terrain.Boxes 1    linee di debug: la posizione è giusta?
4. r.Fog 0                la nebbia se l'è mangiato?
5. r.SkyAtmosphere 0      idem
6. geo.Terrain.FlipWinding 0 / 1     le facce sono girate?
```

**Nota Unreal: `viewmode wireframe`** è il comando più utile che esista quando
qualcosa non si vede. Mostra il reticolo di **tutte** le primitive, ignorando
materiali, illuminazione, nebbia e orientamento delle facce. Se lì la geometria
c'è, il problema è nell'ombreggiatura. Se non c'è nemmeno lì, il problema è a
monte. Per tornare normale: `viewmode lit`.

---

## 6.10 Il wireframe e la lezione sul `MarkRenderStateDirty`

**Nota Unreal.** Accendere il wireframe su un `UDynamicMeshComponent` non si fa
con un metodo `SetWireframe()` (che non esiste: è stato un errore di
compilazione), ma con una proprietà pubblica:

```cpp
Component->bExplicitShowWireframe = true;
Component->MarkRenderStateDirty();        // ← non opzionale
```

Il secondo rigo è la lezione. Il proxy di scena (capitolo 1.16) viene costruito
**una volta**, e non rilegge da solo i campi del componente. Senza
`MarkRenderStateDirty()`, il cambiamento si vedrebbe solo al prossimo
aggiornamento della mesh — e il comando sembrerebbe non fare niente finché non ti
muovi.

In generale: **se cambi a runtime una proprietà che riguarda il disegno, dillo
al renderer.**

---

## 6.11 Muoversi nel mondo

Due strumenti aggiunti dopo la Fase 5, per poter esplorare il terreno.

### `geo.Goto` per nome

```
geo.Goto Torino                  2.500 m sopra il suolo
geo.Goto Monte Bianco 500        500 m sopra la vetta
geo.Goto 45.07 7.69 3000         coordinate: quota sull'ellissoide
```

La quota **cambia significato** fra le due forme, di proposito: con un nome è
sopra il suolo, perché `geo.Goto MonteBianco 2000` inteso sull'ellissoide
metterebbe la camera 2.800 m **dentro** la montagna.

I 28 luoghi stanno in `GeoCore/Public/Georeference/GeoPlaces.h`, una tabella
sola condivisa con i cubi di verifica. Le quote sono **sul livello del mare**,
come si leggono su una mappa, così restano controllabili a mano; la conversione
all'ellissoide somma 48 m di ondulazione (giusta entro circa 5 m in Italia).

### `geo.Fly`: la camera che segue la quota

Il pawn di default di Unreal vola a 1.200 unità al secondo, cioè **12 m/s**: per
attraversare una tile di livello 9 servono tre minuti. `AGeoFlyPawn` vola a una
velocità **proporzionale alla quota**: mezza quota al secondo. A 200 m sono 360
km/h, a 15 km sono 27.000 km/h.

Non è un vezzo: quello che conta per chi guarda non è la velocità in metri al
secondo, ma quanto in fretta cambia l'inquadratura. A 100 m inquadri un isolato,
a 100 km una regione, e per percorrerli "alla stessa andatura percepita" la
velocità deve crescere con la quota. È la stessa scelta di Google Earth.

**Nota Unreal.** `AGeoFlyPawn` deriva da `ADefaultPawn`, che registra già da solo
WASD e il mouse. Derivare da `APawn` avrebbe richiesto di configurare l'*Enhanced
Input* (il sistema di input moderno di Unreal), sproporzionato per una camera di
debug. E la quota si chiede alla georeferenziazione, **non** si legge da
`Location.Z`: Z è l'altezza sopra l'origine corrente, che il rebasing sposta di
continuo.

`geo.Fly` funziona solo nel Play. Nel viewport dell'editor la camera non è un
attore (capitolo 1.8), e lì si usa `geo.ViewSpeed`.

---

## Dove sta nel codice

| File | Cosa contiene |
|---|---|
| `GeoRender/Public/Mesh/TileMesh.h` | da quote a mesh: vertici, normali, UV, gonne |
| `GeoRender/Public/Terrain/GeoTerrainMeshProvider.h` | l'interfaccia, e `FGeoTerrainTileDiagnostic` |
| `GeoRender/Public/Terrain/DynamicMeshTerrainProvider.h` + `.cpp` | l'implementazione con `UDynamicMeshComponent` |
| `GeoRender/Public/Terrain/GeoTerrainSubsystem.h` + `.cpp` | budget, pin, rimozione, overlay, scatole |
| `GeoRender/Public/GeoFlyPawn.h` + `.cpp` | la camera di volo |
| `GeoCore/Public/Georeference/GeoPlaces.h` | i luoghi noti |
| `GeoRender/Private/GeoRenderModule.cpp` | i comandi `geo.Terrain.*`, `geo.Fly*` |
| `Tools/StandaloneTests/geomesh_main.cpp` | 28 test, giunzioni comprese |
| `docs/fase5-design.md`, `docs/fase5-verifica.md` | i documenti originali |

### Comandi

```
geo.Terrain.Demo <cartella>      apre, si posiziona a 6 km, wireframe acceso
geo.Terrain.Enable <0|1>         costruzione della geometria
geo.Terrain.Wireframe <0|1>      reticolo dei triangoli
geo.Terrain.Skirt <0|1>          gonne: spegnile per VEDERE le crepe
geo.Terrain.FlipWinding <0|1>    orientamento delle facce
geo.Terrain.Budget <N>           tile costruite per frame
geo.Terrain.Stats                statistiche
geo.Terrain.Diag                 i numeri del renderer
geo.Terrain.Boxes <0|1>          bounds come linee di debug
geo.Fly / geo.Fly.Speed <x>      camera di volo (nel Play)
geo.ViewSpeed <1..8> [x]         velocità del viewport (nell'editor)
```

---

## Riepilogo

* Ogni tile diventa **17.157 vertici e 33.792 triangoli**, circa 0,9 MB.
* I vertici stanno in un **frame NEU locale alla tile, in metri**: precisione
  sotto il millimetro, e **un rebase non ricostruisce niente**.
* La conversione in centimetri è nella **scala** del componente.
* Normali per **differenze centrali** sulle posizioni; possibile cucitura di luce
  ai bordi, geometria sempre continua.
* **Nessuna crepa fra tile dello stesso livello** (misurato: 0,0001 m); le
  **gonne** coprono quelle fra livelli diversi.
* Profondità della gonna = passo × 111.132 × 8; **152,6 m** al livello 13.
* L'orientamento dei triangoli si verifica **a schermo**: per questo è un
  parametro a caldo.
* `IGeoTerrainMeshProvider` isola `UDynamicMeshComponent`, che si potrà
  sostituire con un proxy scritto da zero.
* **4 tile per frame**, pin delle tile in uso, chiave intera per poter rimuovere.
* Debug: **misura il risultato, non l'intenzione**; un comando che non cambia
  niente deve dirlo; **`viewmode wireframe`** prima di tutto.
* Cambiando a runtime una proprietà di disegno: **`MarkRenderStateDirty()`**.

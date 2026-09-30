# Capitolo 11 — Residenza: cosa tenere pronto

> Il capitolo 5 decideva cosa **disegnare** guardando dove guarda la camera.
> Questo capitolo cambia la domanda: cosa tenere **pronto**, guardando dove la
> camera **è** e dove **sta andando**. Il documento di progetto completo, con
> tutte le motivazioni e i numeri, è `docs/residenza-design.md`; qui c'è quello
> che serve per capirlo e per imparare le parti di Unreal che usa.

---

## L'idea in breve

Tre cambiamenti, che insieme fanno sì che girare la camera non costi niente e
volare dritto non faccia aspettare:

1. **La selezione non guarda più il frustum.** Si scelgono le tile tutto
   attorno alla camera; a non disegnare quelle dietro ci pensa il renderer di
   Unreal, che lo fa già per conto suo.
2. **Un piano dice cosa tenere pronto**: le tile che servono qui, quelle che
   serviranno dove la camera sarà fra 4, 8 e 12 secondi, e un anello di
   sicurezza tutto attorno.
3. **Le mesh si costruiscono in anticipo e si tengono**, nascoste, finché c'è
   posto. È qui il guadagno vero, perché il costo vero è costruirle.

L'idea di partenza è tua ("carica in base alla posizione e alla velocità, non
alla direzione"). Il capitolo spiega come è diventata codice.

---

## 11.1 Dove va davvero il tempo

Prima di progettare il precarico bisogna sapere **cosa** conviene fare prima.
Per una tile, i passi sono:

| Passo | Dove gira | Quanto costa | Si vede nel frame? |
|---|---|---|---|
| leggere le quote dal disco | thread di lavoro | frazione di ms | no |
| costruire la mesh (geodesia + topologia) | **game thread** (fino alla prima prova; ora thread di lavoro) | qualche ms | **sì** (ora no) |
| decodificare il JPEG dell'ortofoto | thread di lavoro | qualche ms | no |
| creare la texture | **game thread** | poco, ma per ogni tile | sì, se sono tante |

Precaricare solo le **quote** servirebbe quindi a poco: la lettura sta già su
un altro thread. Quello che fa scattare il frame è la costruzione della mesh.
Ecco perché il piano non si limita a leggere in anticipo: **costruisce** in
anticipo.

> 💡 **La lezione generale.** Prima di ottimizzare, chiediti *dove* va il
> tempo, e *su quale thread*. Un lavoro lento su un thread di lavoro non si
> vede; uno veloce ma ripetuto sul game thread sì.

---

## 11.2 Tre domande, tre insiemi

| Domanda | Insieme | Chi decide | Budget |
|---|---|---|---|
| Cosa **disegno** adesso? | la selezione | quadtree, a ogni frame | — |
| Cosa ho **pronto**? | mesh costruite, visibili + nascoste | terreno | 2.000 mesh |
| Cosa ho in **RAM**? | quote del piano | streaming | 1 GB |

Ogni insieme contiene il precedente. Il disegno pesca solo fra le mesh pronte;
le mesh si costruiscono solo da quote in RAM.

---

## 11.3 La selezione senza frustum

Nel capitolo 5 i controlli erano: esiste → frustum → orizzonte → errore su
schermo. Il frustum era **l'unico** che dipendeva dalla direzione dello sguardo:
l'errore su schermo dipende dalla distanza, l'orizzonte dalla posizione. Tolto
quello (`FViewParameters::bFrustumCulling = false`), la selezione dipende solo
da **dove** è la camera.

> 💡 **Misurato.** Sopra Roma a 3 km: con il frustum ~230 tile, senza ~630.
> Girando la camera verso nord, verso sud o in basso, l'insieme è identico tile
> per tile.

### Nota Unreal: il renderer fa il suo frustum culling

Ogni componente visibile (`UPrimitiveComponent`) ha dei **bounds**, il volume
che lo contiene. A ogni frame il renderer confronta i bounds di ogni primitiva
con il frustum della camera, e scarta quelle fuori **prima** di mandarle alla
scheda video. Poi, per quelle dentro, può anche scartare quelle nascoste dietro
altre (*occlusion culling*).

Per questo possiamo selezionare tile anche dietro la camera: costano un test
sui bounds, non un disegno. È la stessa ragione per cui nel capitolo 6
`geo.Terrain.Boxes` disegna proprio i bounds letti dal componente: sono quelli
che il renderer usa per decidere.

---

## 11.4 Il piano di residenza

Il piano è una lista di tile in ordine di urgenza, divisa in **fasce**:

| Fascia | Contenuto | Mesh? |
|---|---|---|
| 0 | l'insieme ideale qui | sì |
| 1, 2, 3 | l'insieme ideale a 4, 8, 12 s | sì, nascoste |
| sicurezza | l'insieme ideale qui con soglia d'errore dimezzata | no, solo quote |

**Insieme ideale** vuol dire "quello che disegnerei se avessi già tutto". Si
calcola con lo **stesso** selettore del capitolo 5, passandogli una
disponibilità finta che risponde sempre "sì, è caricata"
(`FAssumeEverythingLoaded`). È un bell'esempio del perché la selezione parla con
un'interfaccia (capitolo 5, sezione 5.8): per cambiarle il comportamento non si
tocca una riga dell'attraversamento.

> 💡 **Dai test.** Volo a 2.500 m verso nord a 250 m/s: fascia 0 = 443 tile,
> previste = 78 nuove, sicurezza = 993. Delle previste, 62 sono davanti.

Il piano si rifà ogni **0,25 s** (costa circa 1 ms), subito dopo un salto.

### "Scaricare dietro", pigramente

Non si butta via niente di proposito. A ogni aggiornamento del piano, le sue
tile vengono **toccate** nella cache (`TTileCache::Touch`, capitolo 4): vanno in
testa alla coda LRU. Quando serve spazio, si butta per primo quello che il
piano non tocca da più tempo. Se torni indietro prima che la memoria serva, è
ancora lì.

---

## 11.5 La previsione del moto

`FMotionPredictor` stima la velocità dalle **posizioni** della camera, con un
filtro esponenziale di mezzo secondo, e la proietta avanti su una retta.
Dalle posizioni e non da una velocità dichiarata, perché così funziona con
qualunque cosa muova la camera: pawn, editor, `geo.Goto`, domani l'host via
CIGI o DIS.

> 💡 **Il filtro esponenziale, in una riga.** A ogni campione:
> `v = v + (v_istantanea − v) · α`, con `α = 1 − e^(−Δt/τ)`. Con τ = 0,5 s, un
> campione nuovo pesa poco se è passato poco tempo, e molto se è passato
> tanto. Scritto così non dipende dal frame rate.

### Il teletrasporto

Uno spostamento oltre **metà della quota** in un solo aggiornamento (e almeno
500 m) è un salto, non un moto. Relativo alla quota perché l'insieme di tile
scala con la quota: 5 km per frame sono un salto a 2 km di quota e un volo
normale a 600 km.

Dopo un salto: code di lettura svuotate, velocità azzerata, piano rifatto,
terreno in **riscaldamento** (11.7).

---

## 11.6 Le mesh nascoste

Una mesh costruita in anticipo, o non più selezionata, non va distrutta: va
**nascosta**. Sovrapposta al padre o ai figli farebbe z-fighting; nascosta non
costa niente, e rimostrarla costa molto meno che ricostruirla.

Budget di default: 2.000 mesh in tutto. Oltre, si buttano le nascoste che il
piano non vuole più, dalla meno recente. Quelle visibili non si toccano mai.

### Nota Unreal: `SetVisibility`, `SetHiddenInGame` e il proxy di scena

Unreal ha **due thread** che contano per il disegno: il **game thread**, dove
vivono gli attori e i componenti, e il **render thread**, che prepara il lavoro
per la scheda video. Il render thread non guarda mai i componenti direttamente:
ne usa una copia, il **proxy di scena** (`FPrimitiveSceneProxy`), creata dal
componente quando viene registrato o quando il suo stato di rendering è
"sporco" (`MarkRenderStateDirty`, capitolo 6, sezione 6.10).

Quando un componente diventa invisibile, Unreal lo **toglie dalla scena**: il
proxy si distrugge, ma i dati sul game thread restano. Quando torna visibile, il
proxy si ricrea copiando i vertici nei buffer della scheda video. Questa copia
avviene alla fine del frame, non dentro `SetVisibility`: per questo non si può
cronometrare dal codice, e va guardata con `stat unit`.

Perché `SetVisibility` e non `SetHiddenInGame`: il secondo vale solo in gioco,
e nel viewport dell'editor la tile resterebbe visibile, sovrapposta al padre.

> ⚠️ **Trappola.** Il provider crea i componenti **visibili**. Una mesh
> costruita in anticipo va nascosta **nello stesso frame** in cui la si crea:
> altrimenti per un frame compare sopra quella giusta. Lo fa
> `UGeoTerrainSubsystem::BuildTile`.

---

## 11.7 La regola anti-buchi, rivista, e il riscaldamento

La regola del capitolo 5 ("scendi nei figli solo quando ci sono tutti") usava
come "ci sono" le **quote in RAM**. Ma a schermo va la **mesh**, che arriva
qualche frame dopo: per quei frame, tolto il padre, non c'era niente. Adesso il
terreno dice al quadtree cosa vuol dire "pronta": **ha la mesh**.

### Nota Unreal: una funzione registrata da un altro oggetto

Il terreno registra la sua risposta nel quadtree con una lambda:

```cpp
Quadtree->SetRenderReadiness([this](const FTileKey& Key)
{
    return BuiltTiles.Contains(Key.Pack());
});
```

`TFunction` è l'equivalente Unreal di `std::function`: un oggetto che contiene
"qualcosa da chiamare". La lambda cattura `this`, cioè un puntatore al
subsystem del terreno. Se il terreno sparisse prima del quadtree, il quadtree
chiamerebbe un oggetto morto: per questo il terreno toglie la funzione in
`Deinitialize()` e quando lo si spegne.

Un'alternativa più robusta sarebbe catturare un `TWeakObjectPtr` (capitolo 1)
e controllarlo a ogni chiamata. Qui non serve: i due subsystem vivono e muoiono
con lo stesso mondo, e la pulizia esplicita basta. Ma se un giorno la funzione
la registrasse un attore, che può essere distrutto in qualunque momento, il
puntatore debole diventerebbe obbligatorio.

### Il riscaldamento

Dopo un salto manca quasi tutto. Si entra in riscaldamento quando le mesh pronte
della fascia 0 scendono sotto il **60%** (o dopo un salto), si esce sopra il
**95%**; nel frattempo 16 mesh (erano 24) e 32 texture per frame invece di 4. Un attimo di
frame lento, poi tutto pronto: è la tua scelta "meglio un'attesa lunga una
volta sola".

Due soglie diverse si chiamano **isteresi**: con una sola, un frame all'89% e
uno al 91% farebbero entrare e uscire di continuo.

---

## 11.8 L'orizzonte, rifatto

Togliere il frustum ha scoperto un difetto del capitolo 5: il test d'orizzonte
sulla **sfera** rinunciava per tutte le tile grandi, perché la loro sfera scende
sotto la superficie. Su un dataset mondiale si caricavano tile sull'America
guardando da Roma.

Il nuovo test (`IsTileRectBeyondHorizon`) ragiona sul **rettangolo** vero:

1. si passa nello **spazio scalato** (x/a, y/a, z/b), dove l'ellissoide è
   esattamente la sfera unitaria;
2. lì la latitudine di un punto in superficie è la **latitudine parametrica**,
   tan β = (b/a)·tan φ, monotona: il rettangolo resta un rettangolo;
3. si calcola γ, l'angolo minimo fra la camera e il rettangolo: se la
   longitudine della camera cade nel rettangolo è una differenza di latitudini,
   altrimenti il punto più vicino sta sul lato meridiano più vicino, e lungo un
   meridiano il coseno della distanza è una sinusoide in latitudine, con il
   massimo in una formula chiusa;
4. la tile è oltre l'orizzonte se γ > αC + αT, con αC = acos(1/|camera|) e
   αT = acos(1/(1 + quota_max/b)).

> 💡 **Risultato.** Dataset mondiale, 3 km sopra Roma: da 983 a 543 tile. E il
> controllo nel verso pericoloso: 534 punti visibili fino a 800 km, nessuno in
> una tile scartata.

---

## 11.9 Nota C++: perché `inline` in un header

Rileggendo `TileSelector.h` è venuta fuori una funzione **definita** nell'header
senza `inline`:

```cpp
namespace Detail
{
    int32_t PriorityFromError(double ScreenSpaceError, double MaxError) { ... }
}
```

In C++ c'è la **regola della definizione unica** (*ODR*): una funzione con
collegamento esterno può essere definita una volta sola in tutto il programma.
Un header incluso da due `.cpp` produce due definizioni, e il linker si ferma:
`LNK2005: already defined`. `inline` dice al linker "ce ne saranno più copie,
identiche: tienine una".

Non era mai esploso per caso: la unity build di Unreal (capitolo 1) fondeva i
pochi `.cpp` che lo includevano in un file solo. Corretto, e sorvegliato da una
regola nuova di `CheckModuleExports.py`: negli header dello strato puro ogni
funzione definita fuori da una classe deve essere `inline` (le funzioni definite
**dentro** una classe lo sono già implicitamente).

> ⚠️ **Il tipo di errore.** È lo schema 4 del capitolo 9 in un'altra forma: un
> difetto che la build di oggi non vede, e che esplode quando qualcuno cambia
> qualcosa di lontano. La difesa è la stessa: un controllo automatico, provato
> sul caso che doveva trovare.

---

## 11.10 Dopo la prima prova: la costruzione sui thread di lavoro

Alla prima prova su un portatile il gioco laggava appena ci si muoveva. Il
motivo era la tabella della sezione 11.1: costruire una mesh costava diversi
millisecondi **sul game thread**, e la residenza, costruendo in anticipo, lo
teneva occupato anche in volo dritto. La costruzione ora è divisa in due:

```
game thread                        thread di lavoro (UE::Tasks)
-----------                        ----------------------------
DispatchBuild(tile) ───────────►   BuildTileMesh (geodesia)
                                   PrepareTileMesh (FDynamicMesh3)
CommitCompletedBuilds  ◄──coda───  risultato
  SetMesh + trasformazione
```

### Nota Unreal: `UE::Tasks` e chi può toccare cosa

`UE::Tasks::Launch(nome, lambda, priorità)` affida la lambda al **task graph**
del motore, che la esegue su uno dei suoi thread. Per le letture da disco
(capitolo 4) avevamo scelto un pool nostro, perché una lettura **aspetta** il
disco; costruire una mesh invece **calcola**, ed è proprio il lavoro per cui il
task graph esiste. Priorità `BackgroundNormal`: non deve rubare i thread a chi
prepara il frame.

Le regole che rendono sicura la cosa:

* **niente UObject sul worker.** La `FDynamicMesh3` è una struttura di
  GeometryCore, non un UObject: si può costruire ovunque. Il componente invece
  si tocca solo alla consegna, sul game thread (`SetMesh`, che *sposta* la mesh
  senza copiarla);
* **niente `this` nella lambda.** Il lavoro riceve copie di tutto quello che
  usa: le quote (un `shared_ptr` a una tile immutabile), i parametri, la
  funzione di preparazione (una funzione libera, senza stato), la coda;
* **la coda è condivisa** (`TSharedPtr`): se il subsystem sparisse con un lavoro
  in corso, il lavoro scriverebbe comunque in una coda viva;
* **le generazioni.** Se cambiano i parametri (gonne, orientamento), i lavori
  in volo sono stati lanciati con quelli vecchi: il loro risultato porta un
  numero di generazione, e se non è quello corrente si butta;
* **alla chiusura si aspetta.** Un lavoro in volo esegue codice del modulo
  `GeoRender`; chiudere l'editor mentre gira vorrebbe dire eseguire codice di
  una DLL già scaricata. `Deinitialize` aspetta che finiscano.

La **trasformazione** si calcola alla consegna, non alla partenza: se nel
frattempo c'è stato un rebase, l'origine è cambiata. I vertici no, perché sono
nel frame locale della tile (capitolo 6) — ed è di nuovo quella scelta a
rendere facile una cosa che altrimenti non lo sarebbe.

---

## Alternative considerate

| Alternativa | Perché no |
|---|---|
| Griglia fissa di tile attorno alla camera | ignora il LOD: o manca il lontano o si caricano migliaia di tile fini inutili |
| Caricare tutta l'Italia all'avvio | le quote ci starebbero (35 GB), ma il disco non era il problema; mesh e immagini non ci stanno |
| Frustum più largo | sposta il problema, non lo toglie: una rotazione veloce lo supera |
| Previsione su una curva | la stima della curvatura è rumorosa; l'anello di sicurezza copre le virate normali |
| Scaricare esplicitamente ciò che sta dietro | si perde quello che si rivedrà presto, senza guadagno finché la memoria basta |
| Costruire le mesh su un thread di lavoro | è il **prossimo passo**, non un'alternativa: il precarico nasconde il costo ma non lo toglie |

---

## Dove sta nel codice

| File | Cosa contiene |
|---|---|
| `GeoRender/Public/Quadtree/Residency.h` | `FAssumeEverythingLoaded`, `SelectIdealTiles`, `FMotionPredictor`, `BuildResidencyPlan` |
| `GeoRender/Public/Quadtree/Culling.h` | `IsTileRectBeyondHorizon` |
| `GeoRender/Public/Quadtree/QuadtreeTypes.h` | `bFrustumCulling` |
| `GeoRender/Private/Lod/GeoQuadtreeSubsystem.cpp` | moto, piano, precarico con tetto, readiness |
| `GeoRender/Private/Terrain/GeoTerrainSubsystem.cpp` | mesh nascoste, costruzione in anticipo, sfratto, riscaldamento |
| `GeoRender/Private/Terrain/DynamicMeshTerrainProvider.cpp` | `SetTileVisible` |
| `GeoRender/Private/Imagery/GeoImagerySubsystem.cpp` | prima le tile a schermo, coda svuotata al salto |
| `GeoTiles/Public/Tiles/TileCache.h` | `Touch` |
| `Tools/StandaloneTests/geoquadtree_main.cpp` | sezioni 3b, 7, 8, 9 |

### Comandi

```
geo.Lod.ViewIndependent <0|1>   1 = tutto attorno (default), 0 = modo classico con frustum
geo.Lod.Prefetch <0|1>          piano e precarico
geo.Lod.Lookahead <s>           orizzonte della previsione (12)
geo.Lod.Safety <fattore>        anello di sicurezza (0.5; 0 = spento)
geo.Terrain.MeshBudget <N>      mesh costruite in tutto (2000)
geo.Terrain.Warmup <N>          mesh per frame in riscaldamento (24; 0 = spento)
```

La verifica passo passo è in `docs/residenza-verifica.md`.

---

## Riepilogo

* Il costo vero di una tile è **costruire la mesh**, non leggerla: per questo
  si precostruisce, non solo si precarica. Dopo la prima prova la costruzione
  è passata sui **thread di lavoro** (sezione 11.10).
* La selezione è **vista-indipendente**: il frustum lo applica il renderer, che
  scarta le primitive fuori vista usando i loro **bounds**.
* Il **piano di residenza** ha fasce: qui, dove sarò a 4-8-12 s, e un anello di
  sicurezza solo in RAM. Si rifà ogni 0,25 s.
* La velocità si **stima dalle posizioni**; un salto oltre metà della quota è un
  **teletrasporto**.
* Le mesh si **nascondono** invece di distruggerle: il proxy di scena si ricrea,
  la geometria no. `SetVisibility`, non `SetHiddenInGame`.
* "Pronta per il disegno" ora vuol dire **"ha la mesh"**: niente più buchi di
  qualche frame.
* Dopo un salto, **riscaldamento** con isteresi 60% / 95%.
* L'orizzonte si testa sul **rettangolo** nello **spazio scalato**.
* Negli header, fuori dalle classi, **`inline`**: altrimenti prima o poi
  `LNK2005`.

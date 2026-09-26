# Capitolo 4 — Caricare senza bloccare (Fase 3)

> Come il motore legge le tile dal disco mentre il gioco continua a girare, e
> come decide quali tenere in memoria.

---

## L'idea in breve

Il motore deve leggere migliaia di file piccoli, di continuo, mentre ci si
muove. Due problemi:

1. **Leggere un file può bloccare.** Se lo si fa sul game thread, il gioco
   scatta.
2. **La memoria non basta per tutto.** Bisogna scegliere cosa tenere e cosa
   buttare.

Le soluzioni: un **gruppo di thread dedicati** che leggono in background, e una
**cache** che tiene le tile usate di recente entro un budget di memoria.

---

## 4.1 Perché le letture non stanno sul game thread

Ricorda la regola del capitolo 1: sul game thread hai 16,6 ms per frame, per
tutto. Una lettura da disco di 66 KB dura di solito **meno di mezzo
millisecondo**. Allora perché preoccuparsi?

Perché "di solito" non basta. Il tempo di una lettura dipende da cose che non
controlli: l'antivirus che sta scansionando, il disco che sta scrivendo altro, la
cache del sistema operativo che è stata svuotata. Una lettura su cento può durare
50 ms. E se ti muovi in fretta, ne chiedi decine al secondo: **quella lenta
arriva sempre**, e ogni volta è uno scatto.

> 💡 **Esempio.** 40 letture al secondo, una su cento lenta: in media **uno
> scatto ogni due secondi e mezzo**. Nessuno direbbe che il gioco "va bene".

---

## 4.2 Il pool di thread

### Come funziona

```
 GAME THREAD                          THREAD DI CARICAMENTO (4)
 ───────────                          ────────────────────────
 "mi serve la tile 14/17521/4379"
        │
        │  AddQueuedWork(lavoro)
        └──────────────────────────►  un thread libero lo prende:
                                        legge il file
 ... il gioco continua ...              controlla l'header
 ... frame dopo frame ...               decodifica le quote
                                        │
        ┌───────────────────────────────┘
        │  mette il risultato in una CODA
        ▼
 al Tick successivo:
 svuota la coda, mette le tile in cache
```

**Il game thread non aspetta mai.** Chiede, e continua. La tile arriva qualche
frame dopo, e nel frattempo si disegna quello che c'è (capitolo 5 spiega come
evitare i buchi).

### Le scelte, una per una

**Un pool dedicato, non il task graph del motore.** Unreal ha un sistema per
distribuire lavoro su tutti i core (il *task graph*). Non va usato per l'I/O:
è dimensionato sul numero di core, e un lavoro che si blocca sul disco occupa un
core senza usarlo. Un pool separato con pochi thread può stare fermo in attesa
senza togliere niente a nessuno.

**Quattro thread.** Non uno per core: il collo di bottiglia è il disco, non la
CPU. Quattro bastano a tenere il disco occupato; di più litigherebbero per lui.

**Priorità bassa** (`TPri_BelowNormal`). Il caricamento non deve mai rubare CPU
al game thread. Meglio una tile che arriva un frame dopo, che un frame che salta.

### Nota Unreal: `IQueuedWork` e i due metodi obbligatori

Un lavoro per il pool è una classe che implementa `IQueuedWork`:

```cpp
class FGeoTileLoadWork : public IQueuedWork
{
public:
    virtual void DoThreadedWork() override;   // il lavoro vero
    virtual void Abandon() override;          // "non ti eseguirò più"
};
```

*(in `GeoTiles/Private/Streaming/GeoTileStreamingSubsystem.cpp`)*

Quando lo aggiungi al pool con `AddQueuedWork(new FGeoTileLoadWork(...))`, il
pool **ne prende la proprietà**: non devi cancellarlo tu. Si cancella da solo
alla fine di `DoThreadedWork()` **oppure** di `Abandon()`.

> ⚠️ **Trappola.** `Abandon()` viene chiamata quando il pool si chiude con dei
> lavori ancora in coda. **Dimenticarla è la causa classica delle perdite di
> memoria alla chiusura**: il lavoro non viene mai eseguito, quindi non si
> cancella mai.

### Nota Unreal: `EQueuedWorkPriority` ha una trappola

La priorità dei lavori è un'enumerazione che va da `Blocking` (0) a `Lowest`
(5). Ma l'enumerazione ha anche un settimo valore, `Count` (6), che **non è una
priorità**: è il numero di valori, usato internamente. Il compilatore te lo
lascia passare senza dire niente, perché è pur sempre un valore dell'enum.

La prima versione calcolava la priorità con una formula che poteva dare 6. Ora
si mappa esplicitamente su `High`…`Lowest`, evitando anche `Blocking` (che
bloccherebbe il pool finché il lavoro non finisce — l'ultima cosa da fare con
una lettura da disco).

---

## 4.3 La coda fra thread

I thread di caricamento devono consegnare i risultati al game thread. Il modo
sbagliato è che scrivano direttamente nella cache: servirebbe un lucchetto, e il
game thread lo dovrebbe prendere a ogni accesso.

Il modo giusto è una **coda**:

```cpp
TQueue<FLoadResult, EQueueMode::Mpsc> CompletedLoads;
```

**Mpsc** significa *multiple producers, single consumer*: molti thread possono
aggiungere, **uno solo** (il game thread) toglie. È esattamente la nostra
situazione, ed è il tipo di coda che si può realizzare senza lucchetti, quindi
molto veloce.

Al `Tick`, il game thread svuota la coda e mette tutto in cache:

```cpp
FLoadResult Result;
while (CompletedLoads.Dequeue(Result))
{
    // unico punto in cui la cache viene scritta, sul game thread
}
```

**La cache vive sul game thread e basta.** Nessun thread di caricamento la tocca
mai. È per questo che la cache non ha bisogno di nessun lucchetto: il percorso
caldo — la selezione LOD la interroga migliaia di volte per frame — non paga
niente per una concorrenza che non esiste.

---

## 4.4 La cache LRU

### Cosa fa

La cache tiene le tile già caricate, entro un **budget in byte** (256 MB di
default). Quando si supera il budget, butta la tile usata **meno di recente**:
è una cache *LRU*, *Least Recently Used*.

```
più recente ─────────────────────────────────────── meno recente
[ 14/17521/4379 ][ 14/17522/4379 ][ ... ][ 12/4380/1094 ] ← si butta questa
```

Ogni volta che una tile viene usata, si sposta in testa. Quelle in fondo sono
quelle che non si guardano da più tempo.

### Budget in byte, non in numero di tile

Le tile di quota sono tutte grandi uguali, quindi contarle sarebbe equivalente.
Ma la scelta è stata fatta pensando alla Fase 6: le tile di **immagine** hanno
dimensioni diverse. E comunque, la cosa che l'utente vuole configurare è "usa al
massimo 512 MB", non "tieni al massimo 3.000 tile", che non significa niente.

### Il pin: "non buttare questa"

Una tile che si sta **disegnando** non va sfrattata, anche se è la meno recente:
la si ricaricherebbe subito dopo. Per questo esiste il **pin**:

```cpp
Cache.SetPinned(Key, true);    // non sfrattabile
Cache.SetPinned(Key, false);   // torna normale
```

Il terreno (capitolo 6) pinna le tile che sta disegnando e le spinna quando le
toglie.

### Consegnare le tile in modo sicuro

La cache consegna le tile come `std::shared_ptr<const FHeightTile>`. Se la mesh
sta usando una tile mentre la cache la sfratta, il puntatore condiviso la tiene
viva finché l'ultimo utente non la rilascia. Senza, uno sfratto nel momento
sbagliato lascerebbe un puntatore a memoria già liberata — il tipo di bug che si
manifesta una volta ogni tanto, a caso, e non si riproduce mai.

### Osservare senza disturbare

La cache ha un metodo `ForEachResident()` per l'overlay di debug. Il dettaglio:
**non modifica l'ordine LRU**. Se lo facesse, accendere l'overlay cambierebbe
quali tile vengono sfrattate, e si osserverebbe un sistema diverso da quello che
si voleva osservare.

### Dalla Fase 6: un template

Fino alla Fase 5 la cache conteneva solo tile di quota. Con le ortofoto serviva
la stessa cache per un contenuto diverso. È diventata un **template**:

```cpp
template <typename PayloadType>
class TTileCache { ... };

using FTileCache = TTileCache<FHeightTile>;   // il nome di sempre
```

Il template è gratis a runtime, e qui è gratis anche in leggibilità, perché la
cache **non guarda mai dentro** il contenuto: gli chiede solo `GetByteSize()`.
L'alias `FTileCache` fa sì che il codice delle Fasi 3-5 compili senza modifiche.

File: `GeoTiles/Public/Tiles/TileCache.h` (strato puro).

---

## 4.5 Il dataset: sapere cosa esiste prima di chiederlo

`FGeoTileDataset` rappresenta un dataset su disco. All'apertura legge **solo il
manifest**; gli indici dei livelli li carica quando servono.

Serve a rispondere, **senza toccare il disco**, a due domande che il quadtree fa
di continuo:

* `TileExists(Key)` — questa tile esiste, o è mare?
* `GetTileHeightRange(Key, Min, Max)` — quanto è alta?

### Nota Unreal: `FFileHelper`, non `std::ifstream`

Per leggere i file il codice usa `FFileHelper::LoadFileToArray()`, non la
libreria standard. Il motivo: Unreal ha un **file system virtuale**. In un gioco
impacchettato, i file stanno dentro archivi `.pak`, e un percorso "normale" non
esiste come file sul disco. `std::ifstream` funzionerebbe per tutto lo sviluppo e
fallirebbe nel gioco finito.

File: `GeoTiles/Public/Streaming/GeoTileDataset.h` + `.cpp`.

---

## 4.6 Lo stato di una tile

Dal punto di vista di chi la chiede, una tile è sempre in uno di questi stati
(`EGeoTileState`):

| Stato | Significato |
|---|---|
| `Assente` | non esiste nel dataset (per esempio, è mare) |
| `NonCaricata` | esiste, ma nessuno l'ha ancora chiesta |
| `InCaricamento` | chiesta, un thread ci sta lavorando |
| `Pronta` | in cache, usabile |
| `Errore` | la lettura è fallita |

Chiedere una tile già `InCaricamento` non fa niente: un insieme `InFlight` evita
di accodare due volte la stessa lettura.

---

## 4.7 L'estrazione del pool (Fase 6), e dove è arrivata

Nella Fase 6 è servito un secondo streaming, per le ortofoto. La logica del pool
(creazione, chiusura, priorità) è stata estratta in una classe a sé,
**`FGeoLoaderPool`** (`GeoTiles/Public/Streaming/GeoLoaderPool.h`), secondo una
regola precisa:

> **Il pool si condivide, il lavoro no.** Come si legge e si interpreta un file
> dipende dal contenuto; una classe base che provasse a condividerlo avrebbe un
> metodo virtuale per ogni differenza.

**Stato attuale, detto con onestà:** `FGeoLoaderPool` lo usa **solo lo streaming
delle ortofoto**. Quello delle quote ha ancora il suo pool interno
(`FQueuedThreadPool* LoadPool`), come nella Fase 3. Non è stato migrato per non
toccare codice funzionante che in questo ambiente non si poteva compilare. La
migrazione è meccanica — quattro punti da sostituire — ed è il primo lavoro di
pulizia da fare quando il codice sarà stato provato in Unreal.

---

## Alternative considerate

| Alternativa | Perché no |
|---|---|
| Leggere sul game thread | scatti (sezione 4.1) |
| Il task graph di Unreal | occupa core con lavoro che aspetta il disco |
| Un thread per richiesta | creare un thread costa più di leggere il file |
| Cache con lucchetto, scritta dai worker | un lucchetto su ogni accesso del percorso caldo |
| `std::async` | nessun controllo sul numero di thread né sulla priorità |
| Budget in numero di tile | non significa niente quando le tile hanno dimensioni diverse |
| Streaming di Unreal (`FStreamableManager`) | pensato per gli asset `.uasset`, non per file binari nostri |

---

## Dove sta nel codice

| File | Cosa contiene |
|---|---|
| `GeoTiles/Public/Tiles/TileKey.h` | la chiave di una tile (livello, X, Y) e il suo impacchettamento |
| `GeoTiles/Public/Tiles/TilingScheme.h` | lo schema di tiling, gemello di `tiling.py` |
| `GeoTiles/Public/Tiles/TileFormat.h` | la lettura del formato `.ght` |
| `GeoTiles/Public/Tiles/TileCache.h` | la cache LRU template |
| `GeoTiles/Public/Streaming/GeoTileDataset.h` + `.cpp` | manifest e indici |
| `GeoTiles/Public/Streaming/GeoTileStreamingSubsystem.h` + `.cpp` | il subsystem, il pool, la coda |
| `GeoTiles/Public/Streaming/GeoLoaderPool.h` | il pool estratto (Fase 6) |
| `GeoTiles/Private/GeoTilesModule.cpp` | i comandi `geo.Tiles.*` |
| `Tools/StandaloneTests/geotiles_main.cpp` | test senza Unreal, anche su un dataset vero |
| `docs/fase3-design.md`, `docs/fase3-verifica.md` | i documenti originali |

### Comandi

```
geo.Tiles.Demo <cartella>        apri, vai, carica, mostra
geo.Tiles.Open <cartella>        apri un dataset
geo.Tiles.Info                   livelli, quote, indici
geo.Tiles.Stats                  cache, hit rate, tempi di caricamento
geo.Tiles.LoadAround [liv] [r]   carica un riquadro attorno alla camera
geo.Tiles.Load <livello>         carica la tile sotto la camera
geo.Tiles.Debug <0|1>            overlay
geo.Tiles.Draw <0|1>             volumi delle tile nel mondo
geo.Tiles.Budget <MB>            budget della cache
geo.Tiles.Clear                  svuota
```

---

## Riepilogo

* **Mai leggere dal disco sul game thread**: una lettura su cento è lenta, e
  quella arriva sempre.
* Un **pool dedicato** di 4 thread a priorità bassa legge le tile; il task graph
  del motore non va usato per l'I/O.
* `IQueuedWork` richiede **due** metodi: `DoThreadedWork()` e `Abandon()`.
  Dimenticare il secondo significa perdite di memoria.
* `EQueuedWorkPriority::Count` **non è una priorità**.
* I worker consegnano via **coda MPSC**; la cache vive **solo** sul game thread e
  non ha lucchetti.
* Cache **LRU** con budget **in byte**, **pin** per le tile in uso, tile
  consegnate come `shared_ptr`.
* L'**indice** risponde a "esiste?" e "quanto è alta?" senza toccare il disco.
* `FFileHelper`, non `std::ifstream`: in un gioco impacchettato i file stanno
  nei `.pak`.
* `FGeoLoaderPool` è condiviso solo dalle ortofoto per ora: la migrazione delle
  quote è da fare.

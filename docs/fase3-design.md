# Fase 3 — Loader asincrono e cache: decisioni e motivazioni

Codice in `Plugins/GeoWorld/Source/GeoTiles/`.
Per la verifica vedi `docs/fase3-verifica.md`.

---

## 1. Cosa fa questa fase, e cosa non fa

**Fa:** apre un dataset prodotto dalla Fase 2, sa quali tile esistono senza
toccare il disco, e carica su richiesta quelle che servono senza mai bloccare il
game thread, tenendole in una cache a budget.

**Non fa:** decidere *quali* tile servono. Quella e' la Fase 4. Senza il
quadtree, nessuno chiede tile — e' il motivo per cui questa fase, da sola, non
produrrebbe niente da guardare, e per cui esistono i comandi di debug descritti
piu' avanti.

---

## 2. I due strati, di nuovo

Come `GeoCore`, anche `GeoTiles` e' diviso in due parti che non si mescolano:

| Strato | Cartelle | Include ammessi |
|---|---|---|
| Puro | `Public/Tiles`, `Private/Tiles` | solo standard library |
| Ponte Unreal | `Public/Georeference`, `Private/Georeference` | `CoreMinimal.h`, `Engine`, … |

Non e' purismo ripetuto per abitudine: e' il motivo per cui questa fase, pur non
essendo **mai stata compilata dentro Unreal**, ha 23 test eseguiti — fra cui la
lettura di un dataset vero con verifica delle giunzioni. `CheckSourceDiscipline.sh`
copre ora anche questo strato.

---

## 3. Il formato letto, non riscritto a memoria

`TilingScheme.h` e' il gemello C++ di `Pipeline/geoworld/tiling.py`. Le due
implementazioni **devono** concordare: la pipeline decide dove sta ogni post e
il runtime ricostruisce la stessa geometria.

Per questo la pipeline emette `TestData/tiling_vectors.txt` — livelli, estremi
di tile, ricerche da lon/lat — e i test C++ ci si confrontano. Riscrivere la
matematica a memoria e' esattamente il modo in cui due implementazioni divergono
di mezzo pixel e nessuno capisce perche' il terreno e' spostato.

### Letture byte per byte, non struct mappate sul buffer

La tentazione e' `memcpy` della struct dell'header sul buffer del file. Non si
fa: il compilatore inserisce **padding** fra i campi secondo regole sue, e quel
layout cambia con compilatore e architettura. Funziona finche' non cambia
qualcosa, e poi smette di funzionare in modo difficilissimo da diagnosticare.
Le letture little-endian esplicite costano nulla e sono definite.

### La validazione piu' importante

`DecodeTile()` verifica che la chiave **dichiarata dentro il file** corrisponda
a quella attesa. Intercetta il caso che capita davvero: una piramide rigenerata
con parametri diversi che lascia sul disco i file vecchi. Senza questo controllo
il runtime carica dati vecchi credendoli nuovi, e il terreno risulta
*sottilmente* sbagliato — il tipo di bug che costa giorni.

Anche i NaN vengono rifiutati: la pipeline garantisce che ogni post abbia una
quota, quindi un NaN qui significa un bug a monte. Lasciarlo passare produrrebbe
triangoli degeneri in Fase 5, molto piu' difficili da ricondurre alla causa.

---

## 4. L'indice: sapere senza leggere

Il quadtree dovra' decidere se raffinare **prima** di aver caricato alcunche'.
Senza indice dovrebbe tentare una lettura per ogni tile candidata e interpretare
il fallimento: un accesso al filesystem per scoprire un'assenza, sul percorso
critico, migliaia di volte per frame.

`FGeoTileDataset` carica `<level>/index.bin` in una `TMap` e risponde a costo
costante a due domande:

* **esiste?** → `TileExists()`
* **che quote ha?** → `GetTileHeightRange()`, che e' cio' che serve per costruire
  il bounding volume e fare culling **senza caricare la tile**.

Costo: 16 byte per tile piu' overhead, cioe' ~20 MB per il livello piu' fine
dell'Italia intera. Si caricano solo i livelli effettivamente usati, e i primi
quattro all'apertura perche' costano pochissimo e servono comunque.

---

## 5. Nessun I/O sul game thread

Una lettura da disco costa fra frazioni di millisecondo (cache del sistema
operativo) e decine di millisecondi (disco meccanico, antivirus, rete). Un frame
a 60 fps dura **16.6 ms**: una sola lettura sfortunata lo fa saltare. Con
centinaia di tile da caricare mentre la camera si muove, farlo sul game thread
rende l'esperienza inutilizzabile.

Il ciclo e':

```
game thread    RequestTile()  -> mette in coda e ritorna subito
worker thread  legge il file, lo decodifica
worker thread  deposita il risultato in una coda MPSC
game thread    Tick() svuota la coda e riempie la cache
```

### Thread pool dedicato, non il task graph

Il task graph di Unreal e' pensato per lavoro che **calcola**, dimensionato sui
core. Le nostre attivita' invece si **bloccano** su I/O: un thread fermo ad
aspettare il disco non consuma CPU ma occupa uno slot. Mescolarle al task graph
significa togliere worker a chi deve calcolare — animazione, fisica, particelle —
per tenerli fermi.

Con un pool dedicato il numero di thread si dimensiona sul parallelismo del
disco (4, non sui core) e una raffica di caricamenti non puo' affamare il resto
del motore. La priorita' e' `TPri_BelowNormal`: meglio una tile che arriva un
frame dopo che un frame che salta.

### `IQueuedWork` ha due uscite, e vanno gestite entrambe

`DoThreadedWork()` quando il lavoro viene eseguito, `Abandon()` quando il pool
chiude o il lavoro viene annullato prima di partire. Dimenticare `Abandon()` e'
la causa classica delle perdite di memoria alla chiusura: gli oggetti in coda
non vengono mai distrutti.

### Cosa un worker non fa MAI

Non tocca la cache, non chiama delegate, non guarda nessun `UObject`. Sono tutti
di competenza esclusiva del game thread, e il garbage collector puo' muoverli in
qualunque momento. L'unico punto di contatto e' una coda `TQueue<T, Mpsc>`
— multi-produttore, singolo-consumatore — che e' senza lock.

---

## 6. La cache

### Budget in byte, non in numero di tile

Oggi le tile di quota sono tutte da 66.596 byte, quindi contarle sarebbe
equivalente. Ma dalla Fase 6 arrivano le tile di imagery, che sono compresse e
di dimensione variabile: un limite a numero di elementi diventerebbe un limite a
memoria imprevedibile. Meglio fissare adesso la semantica giusta, che e' anche
quella che si vuole configurare ("usa al massimo 512 MB").

### Deliberatamente non thread-safe

La cache vive sul game thread e basta. Renderla thread-safe significherebbe
pagare un lock su ogni accesso del percorso caldo — la selezione LOD la
interroghera' migliaia di volte per frame — per una concorrenza che non esiste.

### `shared_ptr` e non puntatori nudi

Chi usa una tile (il quadtree, la generazione mesh) puo' tenerne un riferimento
e continuare a lavorarci anche se nel frattempo la cache la sfratta. Senza, uno
sfratto durante la generazione di una mesh lascerebbe un puntatore pendente.
Verificato da un test apposta.

### Le tile pinnate non si sfrattano

Servono per quelle in uso **adesso** e per le radici della piramide, che se
sfrattate costringerebbero a ricaricare da disco proprio l'unica cosa che serve
sempre. Senza pin, una scorribanda della camera che riempie la cache potrebbe
buttare fuori le tile che si stanno disegnando.

Se fossero tutte pinnate, il budget viene sforato invece di buttare via una tile
in uso: e' voluto.

---

## 7. La priorita' delle richieste

`RequestTile(Key, Priority)`: piu' alto viene servito prima. Oggi lo usa
`RequestTilesAround`, che da' priorita' decrescente con la distanza dal centro.

In Fase 4 la priorita' verra' derivata dall'**errore su schermo**, cosi' le tile
che l'utente sta guardando arrivano prima di quelle ai bordi della vista. Il
meccanismo e' gia' in piedi: cambia solo chi calcola il numero.

---

## 8. Debug: rendere osservabile una fase che non si vede

Senza il quadtree nessuno chiede tile, quindi la Fase 3 da sola non produrrebbe
niente da guardare. Due strumenti la rendono osservabile prima che esista chi la
usa davvero:

* **`geo.Tiles.LoadAround <livello> <raggio>`** — sostituto provvisorio del
  quadtree: chiede un riquadro di tile attorno alla camera, con priorita'
  decrescente dal centro. Serve anche a mettere sotto pressione la cache;
* **`geo.Tiles.Draw 1`** — disegna nel mondo il volume di ogni tile residente,
  un colore per livello, spesse se pinnate, con etichetta e intervallo di quote.
  E' letteralmente il bounding volume che la Fase 4 usera' per il culling: vederlo
  adesso significa poterlo verificare prima di dipenderci.

I lati dei volumi sono suddivisi in otto segmenti e non disegnati come rette:
una tile di livello 8 e' larga centinaia di km, e un segmento rettilineo
attraverserebbe la superficie invece di seguirla, di chilometri.

L'overlay non aggiorna l'ordine LRU quando ispeziona la cache: accendere il
debug non deve cambiare quali tile vengono sfrattate, altrimenti si finirebbe
per osservare un sistema diverso da quello che si voleva osservare.

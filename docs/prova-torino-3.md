# Terza prova: flash quando ci si muove, e le nuvole

> Dopo `prova-torino-2.md` le prestazioni andavano "molto meglio", ma restavano
> due cose: **flash fastidiosissimi** quando ti muovi o giri, "come se il
> terreno si ricalcolasse", e le ortofoto **piene di nuvole**. Ecco cosa le
> causava e cosa è cambiato.

---

## 1. In una pagina

| Sintomo | Causa | Cosa è cambiato |
|---|---|---|
| Lampi chiari quando compaiono tile nuove | ogni tile nuova nasceva con il **materiale grigio di base** di Unreal e riceveva la foto solo dopo, un frame o più | una tile **compare solo vestita**; se la sua foto non è pronta, prende subito quella di un antenato |
| "Il terreno si ricalcola" girando | dietro di te il dettaglio è ridotto (dalla seconda prova): girando, le tile si affinavano **davanti agli occhi** | il dettaglio pieno si estende **oltre i bordi** dello schermo (margine 1,6) |
| Tile che cambiano e ricambiano con piccoli movimenti | nessuna **isteresi**: una tile sulla soglia si raffinava e si ricomponeva continuamente | si raffina alla soglia, si ricompone solo all'80% |
| Nuvole sulle ortofoto | si sceglieva la scena **meno** nuvolosa, ma le nuvole restavano | nuvole, ombre e cirri si **tolgono** pixel per pixel e i buchi si riempiono con altre date |

Per le nuvole devi **riscaricare** le scene (sezione 4). Il resto si applica
ricompilando.

---

## 2. I flash

### 2.1 Cosa succedeva

Il terreno costruisce una tile e la passa al renderer. Il componente nasce con
il materiale di base del motore, `BasicShapeMaterial`, **grigio chiaro**. La
foto gliela mette il subsystem delle ortofoto, che passa **dopo**, nello stesso
frame o in quello successivo a seconda dell'ordine di aggiornamento. E crea al
massimo 4 texture per frame: se in quel momento ne servono di più, le tile in
coda restano grigie per altri frame.

Girandoti entrano nella vista decine di tile nuove alla volta, e ognuna
lampeggia grigio chiaro in mezzo alle foto. Sono i tuoi flash.

### 2.2 Cosa è cambiato

**Una tile compare solo vestita.** Il quadtree sceglie cosa disegnare fra le
tile "pronte", e fino a ieri pronta voleva dire "ha la mesh". Ora, con le
ortofoto accese, vuol dire "ha la mesh **e la foto**". Finché la foto non c'è,
a schermo resta il padre, che la foto ce l'ha. È la stessa regola anti-buchi
della Fase 4, estesa alla texture.

Una tile che **nessuna** immagine potrà mai vestire (fuori dalla copertura
delle ortofoto) non aspetta: si mostra grigia, altrimenti il padre resterebbe a
schermo per sempre. Gli indici delle immagini sono tutti in memoria, quindi la
domanda "un'immagine può arrivare?" non tocca il disco.

**Se la foto giusta non è pronta, si usa quella del nonno.** Quando il budget di
texture del frame è finito, invece di lasciare la tile com'è si cerca un
antenato la cui texture **esiste già** e si usa quella, con il ritaglio
giusto: costa zero e la tile ha subito una foto, solo più sfocata. Quella
giusta arriva nei frame successivi. L'overlay delle ortofoto lo conta:
`con la foto di un antenato N`.

**Niente lavoro inutile sui materiali.** Le ortofoto riassegnavano texture e
parametri a tutte le tile a ogni frame, anche quando non cambiava niente. Ogni
assegnazione aggiorna la copia del materiale che usa il renderer: centinaia di
aggiornamenti per frame, per niente. Ora, se texture e ritaglio sono gli
stessi, non si tocca niente.

---

## 3. Il terreno che si ricalcola

### 3.1 Il dettaglio fuori vista

Dalla seconda prova, fuori dalla vista si tollera un errore 4 volte più
grande: è ciò che ha portato la memoria da 18 a 2,5 milioni di triangoli. Il
prezzo, scritto nel documento, era che girando la testa ciò che entra nella
vista si affina. Si vedeva troppo: l'affinamento avveniva **sul bordo dello
schermo**, cioè davanti agli occhi.

Ora "nella vista", ai fini del dettaglio, vuol dire dentro un frustum
**allargato di 1,6 volte** (`OutOfViewMarginFactor`). Girando, le tile si
affinano quando sono ancora fuori dallo schermo. Costa poco: da 2,52 a 2,63
milioni di triangoli (misurato sopra Torino).

### 3.2 L'isteresi

Una tile con l'errore proprio sulla soglia, muovendo appena la camera, passava
avanti e indietro: raffinata, ricomposta, raffinata. Ogni cambio è un salto
della geometria e un cambio di foto.

Ora c'è **isteresi**: una tile si raffina quando supera la soglia, ma si
ricompone solo quando scende sotto l'**80%** della soglia. Fra i due valori la
decisione non cambia. Il quadtree ricorda quali tile ha raffinato al frame
prima (`FSelectionResult::Refined`) e applica a quelle la soglia più severa.

I test (`geoquadtree_tests`, sezione 11) lo misurano: con un passo del 10%
nella soglia, senza isteresi le tile cambiano (da 146 a 131), con l'isteresi
non ne cambia **nessuna**. Oltre la banda (40%) il dettaglio cala davvero.

### 3.3 Cosa resta: il salto di livello

Quando una tile si raffina, le quattro figlie sostituiscono il padre da un
frame all'altro. È un piccolo salto della geometria (*popping*), più visibile
sui rilievi. Il rimedio vero è il **geomorphing**: per qualche decimo di
secondo i vertici delle figlie scivolano dalla forma del padre alla loro. Va
fatto nel materiale (un vertex shader) ed è un lavoro a sé. Con isteresi e
margine i salti dovrebbero essere rari e lontani dal centro dello schermo:
dimmi se lo sono abbastanza.

---

## 4. Le nuvole

### 4.1 Perché c'erano

`fetch-imagery` sceglieva la scena **meno nuvolosa** del periodo, sotto il 10%.
Il 6% di nuvole su un quadrato di 100 × 100 km sono 600 km² di macchie bianche,
con le loro ombre scure accanto: sopra Torino, esattamente quello che hai visto.

### 4.2 Come si tolgono

Ogni scena Sentinel-2 L2A ha un file **SCL** (*Scene Classification Layer*):
per ogni pixel da 20 m dice cos'è. Pesa 2-4 MB contro i 300 del TCI. Il nuovo
modulo `Pipeline/geoworld/cloudmask.py`:

1. legge l'SCL e segna i pixel di classe **nuvola** (probabilità media e alta),
   **cirro**, **ombra di nuvola**, **saturato** e **nessun dato**;
2. **allarga** la maschera di 2 pixel (40 m): i bordi delle nuvole sono sfumati
   e l'SCL li chiama terreno, e senza allargare resterebbe un alone biancastro;
3. **non tocca la neve** (classe 11): sulle Alpi d'estate la neve è vera;
4. mette a zero ("nessun dato") quei pixel del TCI e scrive
   `<scena>_TCI_senza_nuvole.tif`.

Poi i buchi si riempiono. Per ogni quadrato si scaricano **2 scene in più**
(`--fillers`), di giorni diversi, scelte fra le meno nuvolose e a parità le più
vicine nel tempo alla principale (stessa stagione, stessa luce). Vanno **sotto**
la principale nel mosaico: dove lei ha tolto una nuvola, si vede la scena di
sotto; se anche quella aveva una nuvola lì, quella ancora sotto.

Due dettagli tecnici:

- il file senza nuvole è compresso **DEFLATE**, non JPEG: il JPEG sporcherebbe
  lo zero dei pixel tolti (diventerebbero 1, 2, 3…) e attorno a ogni nuvola
  resterebbe una cornice quasi nera non riconosciuta come "nessun dato";
- un pixel **nero vero** della foto, che sarebbe scambiato per "nessun dato",
  diventa (1, 1, 1): invisibile a occhio, ma distinto.

### 4.3 La prova vera, su Torino

Prima di consegnartelo l'ho fatto girare per davvero, con la rete, sull'area
di Torino (7,55-7,80 E, 45,00-45,15 N), estate 2024.

**`fetch-imagery`** ha scelto 6 scene, 3 per quadrato:

| Quadrato | Ruolo | Scena | Nuvole (metadati) | Tolto dalla maschera |
|---|---|---|---|---|
| 32TLQ | principale | 21/08/2024 | 6,0% | 8,3% |
| 32TLQ | riempimento | 01/08/2024 | 6,5% | 11,1% |
| 32TLQ | riempimento | 27/06/2024 | 9,7% | 13,0% |
| 32TMQ | principale | 29/07/2024 | 0,3% | 0,9% |
| 32TMQ | riempimento | 03/08/2024 | 0,4% | 0,9% |
| 32TMQ | riempimento | 13/08/2024 | 1,3% | 3,4% |

La maschera toglie un po' più di quanto dicono i metadati: sono le **ombre**
delle nuvole e l'allargamento di 40 m attorno ai bordi, che i metadati non
contano. Tempo totale **1 minuto e 25 secondi** (circa 1,9 GB scaricati,
4 scene alla volta); ogni file senza nuvole pesa ~230 MB.

**`build-imagery`** è stata anche la prima volta che il taglio girava con GDAL
vero su dati veri, non sui file di prova dei test. Risoluzione rilevata 8,7 m,
livello massimo 13, ~18 tile al secondo: le 5904 tile del livello 13 in circa
5 minuti e mezzo. L'ho interrotta a metà di proposito e rilanciata: ha ripreso
da dove era rimasta, senza rifare le tile già scritte. `verify-imagery` alla
fine dice **TUTTO A POSTO**: 7562 tile, in media 15 KB l'una.

**A occhio**, ricomponendo le tile del livello 12 attorno a Torino: i buchi
lasciati dalle nuvole della scena principale sono riempiti dalle altre date e
non si vedono macchie bianche né ombre. Restano qualche puntino e qualche bordo
appena più chiaro dove le nuvole erano molto sottili, e la fascia grigia in
alto è semplicemente fuori dall'area scaricata.

### 4.4 Cosa fare

```bat
python run.py fetch-imagery --area torino -o dati/sentinel_torino_v2
python run.py build-imagery -i @dati/sentinel_torino_v2/ordine_scene.txt -o dataset/ortofoto_torino_v2 --name "Sentinel-2 Torino, senza nuvole"
```

Opzioni nuove di `fetch-imagery`:

| Opzione | Default | Cosa fa |
|---|---|---|
| `--fillers N` | 2 | scene in più per quadrato, per riempire i buchi |
| `--no-cloud-mask` | — | torna al comportamento di prima |
| `--keep-originals` | — | tiene anche i TCI con le nuvole (di default si cancellano) |

Su tutta l'Italia, con 2 riempimenti, le scene diventano circa il triplo: ~75 GB
da scaricare, e i file senza nuvole occupano quanto gli originali (che poi si
cancellano). Il `--dry-run` te lo dice prima.

### 4.5 Cosa resta

- **Foschia e veli sottili** che l'SCL non classifica come nuvole restano.
- Dove una zona è nuvolosa in **tutte** e tre le date resta un buco (il grigio
  di riempimento). Con `--fillers 4` si scende ancora.
- Il cambio di data **dentro** un quadrato si può vedere come una macchia di
  colore leggermente diverso: meglio di una nuvola, ma non invisibile.
- **10 metri restano 10 metri.** Per volare bassi servono le tue ECW
  (`prova-torino.md`, sezione 4.2).

---

## 5. Dove sta nel codice

| File | Cosa è cambiato |
|---|---|
| `GeoRender/.../Terrain/GeoTerrainSubsystem.*` | `SetDressPredicate`: la readiness diventa "costruita e vestita" |
| `GeoRender/.../Imagery/GeoImagerySubsystem.*` | registra il predicato, `IsDressedOrUndressable`, ripiego sulla texture di un antenato |
| `GeoRender/.../Terrain/*Provider*` | `IsTileDraped`; non riassegna il materiale se non è cambiato |
| `GeoRender/Public/Quadtree/QuadtreeTypes.h` | `OutOfViewMarginFactor`, `PreviouslyRefined`, `RefineHysteresis`, `Refined` |
| `GeoRender/Public/Quadtree/TileSelector.h` | frustum largo per il fuori vista, isteresi |
| `GeoRender/.../Lod/GeoQuadtreeSubsystem.*` | ricorda i nodi raffinati fra un frame e l'altro |
| `Pipeline/geoworld/cloudmask.py` | **nuovo**: maschera SCL, dilatazione, applicazione |
| `Pipeline/geoworld/fetchimagery.py` | scarica anche l'SCL, scene di riempimento per le nuvole, `--fillers` |
| `Pipeline/tests/test_cloudmask.py` | **nuovo**, 9 test |
| `Pipeline/tests/test_fetchimagery.py` | 2 test sui riempimenti |
| `Tools/StandaloneTests/geoquadtree_main.cpp` | sezione 11: isteresi e margine (5 test) |

---

## Riepilogo

- I flash erano tile **grigie** per qualche frame: ora una tile compare solo
  **vestita**, e se la sua foto non è pronta usa quella di un antenato.
- Il "ricalcolo" girando era il dettaglio che si affinava **sul bordo** dello
  schermo: ora si affina prima, fuori.
- Le tile non oscillano più sulla soglia: **isteresi** dell'80%.
- Le nuvole si **tolgono** con l'SCL e i buchi si riempiono con altre date:
  **riscarica** con `fetch-imagery`.
- Resta il piccolo salto quando una tile si raffina: il rimedio (geomorphing) è
  il prossimo passo, se dopo queste correzioni si vede ancora.

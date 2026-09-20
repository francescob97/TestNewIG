# Fase 6 — Ortofoto sul terreno: decisioni e motivazioni

> Questo documento spiega **cosa** costruiamo e **perché** in quel modo.
> Per provare che funziona: `docs/fase6-verifica.md`.

---

## 1. In una pagina

Il terreno della Fase 5 è grigio. Questa fase gli mette sopra delle immagini
vere: ortofoto, cioè fotografie aeree o satellitari già raddrizzate, in cui ogni
pixel sta alla sua coordinata geografica.

Il lavoro si divide in tre parti:

| Dove | Cosa |
|---|---|
| Pipeline Python | una seconda piramide di tile, fatta di immagini invece che di quote |
| Runtime C++ | un secondo streaming, una texture per tile, il calcolo delle UV |
| Materiale | uno shader che prenda quella texture e la disegni sul terreno |

Le UV della mesh **esistono già** e sono corrette: le ha generate la Fase 5.
Questo è il motivo per cui la fase è fattibile senza toccare la generazione
della geometria.

---

## 2. La decisione centrale: due piramidi, uno schema solo

**Decisione.** Quote e immagini stanno in due dataset separati, ma usano lo
**stesso schema di tiling** (geografico WGS84, livello 0 = 2×1, livello L =
2^(L+1) × 2^L).

**Perché separati.** Le due sorgenti non hanno niente in comune se non il
territorio che coprono:

- hanno risoluzioni native diverse, e diverse in modo imprevedibile: oggi 10 m
  per entrambe, domani ortofoto a 20 cm su quote a 30 m;
- si aggiornano con cadenze diverse: un DEM dura anni, un'ortofoto invecchia;
- hanno payload incompatibili: `float32` contro pixel compressi.

Legarle significherebbe costringere per sempre la più fine a scendere al livello
della più grossolana. Ogni volta che si aggiorna una sola delle due bisognerebbe
rigenerare entrambe.

**Perché lo stesso schema di tiling.** È qui che si guadagna. Se i due dataset
tassellano il mondo allo stesso modo, la tile di terreno `(L, X, Y)` e la tile di
immagine `(L, X, Y)` coprono **esattamente lo stesso rettangolo**. Nel caso
normale il drappeggio è quindi banale: la texture copre la tile, le UV `[0,1]`
della mesh vanno bene così come sono, non serve nessun calcolo.

Il caso complicato — quando l'immagine giusta non c'è — è trattato nella
sezione 5.

---

## 3. Registrazione: le immagini non si comportano come le quote

Questa è la differenza che, se la si sbaglia, produce artefatti che poi nessuno
riesce più a spiegare. Vale la pena essere espliciti.

**Le quote sono registrate sui nodi.** Una tile ha 129×129 *post*, e il post 128
di una tile **è** il post 0 della tile accanto: stesso punto, stesso valore. È
questa condivisione che rende le giunzioni fra mesh esatte per costruzione, ed è
verificata dai test della Fase 5 (i bordi coincidono a 0,0001 m in ECEF).

**Le immagini sono registrate sulle aree.** Un pixel non è un punto: è un
quadratino di territorio. Una tile ha 256×256 pixel che coprono il rettangolo
per intero, **senza nessuna sovrapposizione** con le vicine.

Duplicare una colonna di pixel sul bordo, per analogia con le quote, sarebbe un
errore: quella striscia verrebbe disegnata due volte, una per tile, e alla
minima differenza di campionamento si vedrebbe sfarfallare.

### Perché proprio 256

Tre ragioni, in ordine di importanza:

1. **È una potenza di due.** Serve per i mipmap e perché la riduzione da un
   livello al successivo sia una media 2×2 esatta, senza resti.
2. **Dà esattamente due pixel per cella di terreno.** La mesh ha 128 celle per
   lato, la texture 256 pixel: il rapporto è 2:1 su entrambi gli assi. È la
   densità giusta — meno si vedrebbe la sfocatura, molto di più si sprecherebbe
   memoria su dettaglio che lo schermo non mostra.
3. **Fa coincidere le due piramidi.** Poiché 256 = 2 × 128, l'immagine di
   livello L ha la stessa risoluzione al suolo del terreno di livello L+1:

   | Livello | Pixel immagine | Passo terreno |
   |---:|---:|---:|
   | 12 | 19,1 m | 38,2 m |
   | 13 | **9,6 m** | 19,1 m |
   | 14 | 4,8 m | **9,6 m** |
   | 18 | 0,30 m | 0,60 m |

   Da leggere così: Sentinel-2 (10 m) è nativo al **livello 13**; TINITALY
   (10 m) è nativo al livello 14; un'ortofoto a 30 cm arriva al livello 18.

---

## 4. Il formato del file

**Decisione.** JPEG, 256×256, qualità 85, un file per tile, stessa struttura di
cartelle del terreno.

```
<root>/<level>/<x>/<y>.gim     header di 32 byte + JPEG
<root>/<level>/index.bin       una voce per tile
<root>/manifest.json           metadati globali
```

**Perché JPEG e non pixel grezzi.** 256×256 in RGB non compresso sono 196 KB per
tile. Su una piramide che copre l'Italia fino al livello 16 sarebbero centinaia
di gigabyte. In JPEG la stessa tile sta in 15–25 KB, cioè un fattore dieci.

**Perché JPEG e non BC1 (la compressione della GPU).** BC1 sarebbe tecnicamente
migliore: 32 KB per tile, e soprattutto **nessuna decompressione** — i byte si
caricano sulla scheda video così come sono, e lì restano compressi, occupando
otto volte meno memoria video.

Non lo scegliamo adesso per una ragione pratica: comprimere in BC1 richiede un
encoder che né GDAL né Pillow forniscono, quindi una dipendenza esterna in più
nella pipeline. Il vincolo dichiarato all'inizio del progetto era «Python +
GDAL», e vale la pena rispettarlo finché non c'è una misura che dica il
contrario.

**Il formato del file è però già predisposto**: l'header contiene un byte che
dichiara il tipo di payload. Aggiungere BC1 domani significa scrivere un
encoder e un ramo nel lettore, non cambiare il formato.

**Il costo che accettiamo.** Il JPEG va decompresso a ogni caricamento. Circa
1–2 ms per tile, che però avvengono **sul thread di lavoro** e non sul game
thread: è esattamente per questo che la Fase 3 ha costruito il pool.

### L'indice, e le zone senza dati

Ogni voce di indice contiene la chiave della tile e un byte di **copertura**: la
percentuale del rettangolo effettivamente coperta dall'ortofoto.

Serve perché le ortofoto, a differenza dei DEM, hanno bordi frastagliati: finisce
il volo, finisce la regione, c'è il mare. Le tile completamente scoperte non
vengono scritte affatto, come già succede per il terreno. Quelle parzialmente
coperte vengono riempite con un colore neutro.

**Limite dichiarato:** in questa fase una tile parziale non porta con sé una
maschera di trasparenza, quindi il colore di riempimento si vede. Risolverlo
richiede un canale alfa, che il JPEG non ha: è una delle ragioni per cui prima o
poi si passerà a BC3 o a una coppia JPEG + maschera.

---

## 5. Il drappeggio quando l'immagine giusta non c'è

Nel caso normale — stesso livello, tile presente — non c'è niente da calcolare.
Ma capitano due situazioni in cui l'immagine al livello del terreno non è
disponibile:

- **la piramide delle immagini è più bassa di quella del terreno.** Succede
  sempre: il terreno arriva al livello 14, un'ortofoto gratuita si ferma al 13;
- **la tile non è ancora arrivata.** Lo streaming è asincrono. Nel frattempo
  bisogna disegnare qualcosa, e l'alternativa — aspettare — produrrebbe buchi
  grigi lampeggianti.

**Soluzione: si usa l'antenato.** Si risale la piramide fino alla prima immagine
disponibile e se ne ritaglia il pezzo giusto. È lo stesso principio della regola
anti-buchi della Fase 4: meglio grossolano subito che perfetto fra tre frame.

### La matematica

Il terreno sta al livello `Lt`, posizione `(X, Y)`. L'immagine disponibile sta
al livello `La ≤ Lt`. Detta `d = Lt − La` la differenza di livelli:

```
antenato   = ( La , X >> d , Y >> d )
scala      = 1 / 2^d
offsetX    = ( X & (2^d − 1) ) · scala
offsetY    = ( Y & (2^d − 1) ) · scala

UV finale  = offset + UV_mesh · scala
```

Tutto in aritmetica intera, nessun arrotondamento, nessun caso particolare. Con
`d = 0` si ottiene scala 1 e offset 0, cioè il caso normale: **non è un ramo
separato, è lo stesso codice**.

Un esempio concreto. Terreno al livello 12, tile `(4333, 1014)`; immagine
disponibile solo al livello 10, quindi `d = 2`:

```
antenato = (10, 1083, 253)     scala = 0,25
offsetX  = (4333 & 3) · 0,25 = 1 · 0,25 = 0,25
offsetY  = (1014 & 3) · 0,25 = 2 · 0,25 = 0,50
```

La tile di terreno è un sedicesimo di quella di immagine: la seconda colonna da
ovest, la terza riga da nord. La mesh campiona quel riquadro e basta.

### Una coincidenza che non è una coincidenza

`X` cresce verso est e `Y` verso sud; nel file di una tile la riga 0 è quella
più a nord. Le UV della mesh, generate in Fase 5, sono `(I/128, J/128)` con `I`
verso est e `J` verso sud.

I tre versi coincidono, quindi fra UV della mesh e coordinate della texture non
c'è nessuna inversione da fare. Non è fortuna: è la stessa convenzione scelta in
Fase 2 e tenuta ferma da allora, proprio per non dover ricordare dove va messo
un `1 −`.

Questa è la parte della fase che si può **verificare senza Unreal**, e va
verificata: sta nello strato puro, in `Imagery/ImageryMapping.h`.

---

## 6. Cosa va cambiato nel codice esistente

È la parte meno vistosa e più sostanziosa della fase. Vale la pena dirlo prima:
**circa metà del lavoro è ristrutturazione, non funzionalità nuove.**

Oggi tre pezzi sono legati alle quote e non dovrebbero esserlo:

| Pezzo | Com'è | Come diventa |
|---|---|---|
| `FTileCache` | cache di `FHeightTile` | `TTileCache<Payload>`, template, resta header-only |
| pool di caricamento | dentro `UGeoTileStreamingSubsystem` | `FGeoTileLoaderPool`, classe a sé, usata da entrambi |
| `FGeoTileDataset` | manifest e indice delle quote | resta; nasce `FGeoImageryDataset`, gemello |

**Perché non un unico streaming generico.** I subsystem di Unreal sono
`UObject` e gli `UObject` non possono essere template. Si potrebbe aggirare
l'ostacolo con un'interfaccia virtuale sul payload, ma si pagherebbe una
chiamata indiretta nel percorso caldo per unificare due casi che sono e
resteranno due.

La scelta è quindi: **template dove il codice è puro** (cache), **estrazione di
una classe condivisa dove il codice è infrastruttura** (pool di thread),
**duplicazione consapevole dove la logica diverge davvero** (dataset e
subsystem). Il risultato è un `UGeoImageryStreamingSubsystem` di poche decine di
righe, perché tutto ciò che è comune sta altrove.

---

## 7. La texture, e le due trappole

Una tile decodificata diventa una `UTexture2D` transiente:

```cpp
UTexture2D* Texture = UTexture2D::CreateTransient(256, 256, PF_B8G8R8A8);
// copia dei pixel nel mip 0, poi UpdateResource()
```

Deve avvenire **sul game thread**, come la mesh, e come la mesh avrà un budget
per frame. Il thread di lavoro si ferma un passo prima: decodifica il JPEG e
consegna i byte.

**Trappola numero uno: l'indirizzamento.** Va messo a `TA_Clamp`, non `TA_Wrap`.
Con `Wrap`, il filtro bilineare sul bordo destro andrebbe a prendere i pixel del
bordo sinistro della **stessa** texture: sul confine fra due tile comparirebbe
una riga di colori presi dall'altra parte del rettangolo. È un artefatto
classico e difficile da riconoscere se non si sa che esiste.

**Trappola numero due: i mipmap.** Una texture senza mipmap sfarfalla appena la
si guarda di sbieco. In questa fase non ne generiamo, per una ragione difendibile
— il LOD della Fase 4 tiene il rapporto texel/pixel vicino a 1:1, quindi il
problema è contenuto — ma è un limite reale, e la soluzione è nota: scrivere i
mip già nel file e caricarli tutti.

---

## 8. Il materiale: il pezzo che non posso scrivere io

Il terreno ha bisogno di uno shader che campioni la texture della propria tile.
In Unreal uno shader è un **asset binario** (`.uasset`), e questo ambiente non
ha il motore: non posso generarlo.

Il materiale serve semplicissimo:

```
TextureSampleParameter2D("BaseColor")  ->  Base Color
Constant 1.0                           ->  Roughness
Constant 0.0                           ->  Specular
```

Una `UMaterialInstanceDynamic` per tile imposta il parametro `BaseColor` con la
texture di quella tile.

> **Revisione, fatta scrivendo il codice.** L'idea iniziale era scrivere le UV
> ritagliate direttamente nei vertici, per tenere il materiale banale. È
> sbagliata: quando arriva un'immagine più fine il ritaglio cambia, e con le UV
> nei vertici bisognerebbe riscrivere 17.157 coordinate per ogni tile che si
> affina — proprio mentre ci si sta muovendo, cioè nel momento peggiore.
>
> Il materiale ha quindi un secondo parametro, un vettore `UvOffsetScale` che
> vale `(offsetU, offsetV, scala, scala)`. Aggiornarlo costa quattro float e la
> mesh non viene toccata mai. Il prezzo sono tre nodi in più nello shader:

```
TextureCoordinate --> Multiply --> Add --> TextureSampleParameter2D("BaseColor")
                         ^          ^                    |
VectorParameter ---------+----------+                    v
("UvOffsetScale")     .BA        .RG                Base Color
```

Due strade, e le prepariamo entrambe:

1. **`geo.Imagery.CreateMaterial`** — un comando nel modulo `GeoWorldEditor`
   (finora vuoto) che costruisce il materiale con `UMaterialEditingLibrary` e lo
   salva. Un solo comando, una volta sola.
2. **Istruzioni per farlo a mano** in `fase6-verifica.md`, cinque nodi. È la
   rete di sicurezza: il punto 1 usa API di editor che non posso compilare qui, e
   se non compila non voglio che la fase si blocchi.

Sul modello di illuminazione scegliamo **Lit**, non Unlit. Unlit mostrerebbe i
colori esatti dell'ortofoto senza dipendere dalle luci della scena, il che è
comodo; ma toglierebbe ogni ombreggiatura, e il rilievo — che è la cosa che
abbiamo appena finito di costruire — sparirebbe visivamente. Il demo verificherà
che nella scena esista una luce direzionale, e lo dirà se manca.

---

## 9. Da dove vengono le ortofoto

La pipeline è **indifferente alla sorgente**: prende qualunque raster RGB che
GDAL sappia leggere, georeferenziato, e produce la piramide. Esattamente come per
il DEM. La sorgente cambia solo il comando `fetch`.

| Sorgente | Risoluzione | Livello | Note |
|---|---|---|---|
| **Sentinel-2** (bucket `sentinel-cogs`, AWS) | 10 m | 13 | Libera, nessuna registrazione, scaricabile in automatico. Bisogna scegliere scene senza nuvole |
| **Le tue ECW** (i 200 GB) | probabilmente < 1 m | 16–18 | La migliore che hai. Ma vedi sotto |
| **Ortofoto AGEA / IGM via WMS** | 20–50 cm | 17–18 | Alta qualità su tutta l'Italia, ma scaricare per WMS è lento e vanno lette le condizioni d'uso |

**Sulle ECW, una precisazione che va fatta subito.** Il driver ECW di GDAL
richiede l'SDK proprietario ERDAS, che non è incluso nei pacchetti GDAL di
conda-forge. In pratica: `gdalinfo` sulle tue ECW molto probabilmente fallirà.
La strada è convertirle una volta sola in GeoTIFF con uno strumento che l'ECW lo
legge (QGIS con i driver completi, o il visualizzatore fornito col database), e
poi darle in pasto alla pipeline.

La scelta predefinita è **Sentinel-2**, perché è l'unica automatizzabile
dall'inizio alla fine. Le altre due funzionano con lo stesso comando
`build-imagery`, a partire da GeoTIFF.

### L'indifferenza alla sorgente è una proprietà, non una speranza

Vale la pena dirlo esplicitamente, perché è la stessa cosa che già succede per
le quote e non è un caso: la pipeline dei DEM accetta i GeoTIFF di TINITALY
**e** i DTED `.dt2`, senza un ramo di codice per ciascuno, perché tutto passa da
`gdal.Open`. `build-imagery` è costruito allo stesso modo.

In pratica, tutti questi comandi sono lo stesso comando:

```bat
python run.py build          -i "tinitaly/*.tif"       -o dataset/quote
python run.py build          -i "elevation/*.dt2"      -o dataset/quote-dted
python run.py build-imagery  -i "sentinel/*_TCI.tif"   -o dataset/ortofoto
python run.py build-imagery  -i "imagery/*.ecw"        -o dataset/ortofoto-ecw
```

L'ultimo funziona **se e solo se** la tua installazione di GDAL ha il driver
ECW. Non è una cosa da scoprire a metà di una build su 200 GB, quindi
`check-env` adesso stampa la tabella dei driver disponibili e, quando l'ECW
manca, dice cosa fare.

---

## 10. Debug

Lo stesso criterio delle fasi precedenti: gli strumenti nascono insieme alla
funzionalità, non dopo.

```
geo.Imagery.Demo <cartella>    apre, si posiziona, accende tutto
geo.Imagery.Open <cartella>    apre un dataset di immagini
geo.Imagery.Enable <0|1>       drappeggio acceso o spento
geo.Imagery.Stats              tile con texture, memoria video, tempi
geo.Imagery.Diag               cosa il renderer ha davvero, per le prime tile
geo.Imagery.ShowLevels <0|1>   colora le tile per LIVELLO DELL'IMMAGINE
geo.Imagery.Checker <0|1>      sostituisce le ortofoto con una scacchiera
```

Gli ultimi due meritano una riga a testa, perché sono quelli che dimostrano le
decisioni di questo documento.

**`ShowLevels`** colora ogni tile in base al livello dell'immagine che sta
usando, non a quello del terreno. Se il ritaglio della sezione 5 funziona, salendo
di quota si vedono comparire zone di colore diverso dove la piramide delle
immagini si è fermata prima di quella del terreno.

**`Checker`** sostituisce le ortofoto con una scacchiera generata sul momento.
È il modo per vedere le UV: su una scacchiera un disallineamento di mezzo pixel
al bordo è evidente, su una foto di un bosco non lo nota nessuno.

---

## 11. Cosa questa fase non farà

Elencato prima di cominciare, non dopo:

- **niente mipmap**, quindi sfarfallio agli angoli radenti (sezione 7);
- **niente trasparenza** sulle tile parzialmente coperte (sezione 4);
- **niente BC1**, quindi più memoria video e una decodifica per tile (sezione 4);
- **niente fusione fra livelli**: il passaggio da un livello di immagine al
  successivo sarà uno stacco netto, non una dissolvenza;
- **niente atlante di texture**: una texture e una material instance per tile,
  quindi un draw call per tile. È lo stesso limite del provider della Fase 5, e
  si risolverà insieme al suo, quando si scriverà il secondo provider.

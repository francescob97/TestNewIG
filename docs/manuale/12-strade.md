# Capitolo 12 — Fase 8: strade, ferrovie e piste

> Il primo capitolo del "mondo sopra il terreno". Le strade arrivano da
> OpenStreetMap come **dati vettoriali** (linee, non pixel), la pipeline le
> taglia in tile, e il runtime le **disegna** sopra l'ortofoto. Il documento di
> progetto con tutti i numeri è `docs/fase8-design.md`; come provarla è in
> `docs/fase8-verifica.md`.

---

## L'idea in breve

Finora i dati erano tutti **raster**: griglie di numeri (quote) o di colori
(foto). Una strada invece è una **linea**: una sequenza di punti con delle
proprietà (classe, larghezza, ponte sì o no). Questo capitolo introduce il terzo
tipo di dataset, accanto a quote e ortofoto:

1. la **pipeline** legge OSM, decide cosa diventa ogni linea, e la taglia in
   tile **vettoriali** ai livelli 10–13;
2. il **runtime** carica quelle tile e, per ogni tile di terreno, **disegna**
   le linee in un'immagine trasparente;
3. il **materiale** mette quell'immagine sopra la foto.

È la catena che riuseranno l'acqua (Fase 9), gli edifici (10) e i boschi (11):
cambieranno le geometrie (poligoni invece di linee) e chi le usa, non il
percorso.

---

## 12.1 Vettoriale e raster

> 💡 **Esempio.** Una strada di 1 km descritta come linea sono una ventina di
> punti: 80 byte. La stessa strada disegnata a mezzo metro per pixel è
> un'immagine di 2.000 × 20 pixel, quasi tutti uguali: e se la vuoi più larga,
> o di un altro colore, va ridisegnata da capo.

I dati vettoriali si **disegnano al momento**, alla risoluzione e nello stile
che servono. È lo stesso motivo per cui un font sullo schermo è nitido a
qualunque dimensione mentre un'immagine ingrandita si sgrana.

Il prezzo è che qualcuno deve disegnarli. Nei programmi di mappe lo fa la scheda
video; qui lo fa la CPU, su un thread di lavoro, perché così il codice si prova
nei test standalone (sezione 12.4).

---

## 12.2 Da OSM a linea classificata

Una "way" di OpenStreetMap è una sequenza di nodi più dei tag:

```
highway=primary  lanes=2  bridge=yes  layer=1  surface=asphalt
```

`roadclasses.py` la trasforma in quattro numeri:

| Campo | Da dove | Esempio |
|---|---|---|
| classe | `highway`, `railway`, `aeroway` | `primary` → 3 |
| larghezza | `width`, poi `lanes` × 3,25 m, poi un valore tipico | 2 corsie → 7,0 m |
| flag | ponte, galleria, sterrato, rampa | ponte |
| layer | `layer` | 1 |

Alcune way si **scartano**: le gallerie (dal cielo non si vedono), i progetti e
i cantieri, le ferrovie smantellate, le piazze disegnate come area.

> ⚠️ **Trappola.** In OSM un'autostrada sono **due** linee, una per senso di
> marcia. Se la si disegnasse larga quanto l'autostrada intera (25 m), ogni
> carreggiata coprirebbe anche l'altra e lo spartitraffico: la larghezza tipica
> è quella di una carreggiata, 11 m.

> ⚠️ **Trappola.** `round()` di Python arrotonda "al pari": `round(112.5)` fa
> 112, non 113. Una corsia da 11,25 m diventava 112 decimetri. Un test l'ha
> preso; ora si arrotonda con `floor(x + 0.5)`.

---

## 12.3 Le tile vettoriali

### Perché a livelli

Una tile del livello 10 copre ~20 km: metterci i sentieri vorrebbe dire milioni
di punti per disegnare linee larghe un centesimo di pixel. Ogni classe ha un
**livello minimo** (autostrade dal 10, sentieri dal 13), e una tile di terreno
usa la tile vettoriale **più profonda che non superi il suo livello**.

> 💡 **Esempio.** Una tile di terreno del livello 15 sopra Torino usa la tile
> vettoriale del 13 che la contiene, e ne disegna solo un sedicesimo: la
> "finestra" in alto a destra, seconda riga. La stessa aritmetica intera del
> ritaglio delle ortofoto (capitolo 7).

### Il formato

Un header di 32 byte, un record di 12 byte per linea, i punti in `int16`.
Coordinate **locali alla tile**, da 0 a 16384, con il nord in alto; le linee
possono uscire dalla tile di un ottavo del lato (il **buffer**), perché una
strada larga che corre lungo il bordo deve comparire anche nella tile accanto.

### La pipeline in tre stadi

1. **estrai**: si legge OSM una volta, e ogni linea finisce nei **blocchi**
   (tile del livello 10) che tocca;
2. **taglia**: ogni blocco produce le sue tile, in parallelo agli altri;
3. **indici**: `index.bin` e `manifest.json`.

Il trucco dei blocchi: il driver OSM di GDAL non ha un indice spaziale, quindi
chiedergli "cosa passa per questa tile?" vorrebbe dire rileggere tutto il file
per ogni tile. Con i blocchi lo si legge una volta.

---

## 12.4 Il disegno

`RoadRasterizer.h` è C++ puro. Per ogni linea, per ogni segmento, per ogni pixel
vicino: quanto di quel pixel cade dentro la strada?

### In metri

Un pixel di una tile non è quadrato sul terreno: a Torino, a 512 pixel per una
tile del 13, è 3,4 m × 4,8 m. Distanze e larghezze si misurano in metri, così
una strada da 7 m è larga 7 m in ogni direzione.

### L'antialias

La copertura di un pixel è la frazione della sua **impronta** che cade nella
strada. L'impronta va presa **lungo la sezione** della strada: per una strada
orizzontale è l'altezza del pixel, per una verticale la larghezza.

> ⚠️ **Trappola (presa dai test).** La prima versione usava la dimensione media
> del pixel. Una strada da 10 m ne misurava 9,5 in orizzontale, e una sottile
> spariva a tratti: il filtro era più stretto del passo dei pixel, e la somma
> delle coperture dipendeva da dove cadeva la strada rispetto alla griglia. Il
> test che l'ha trovata **misura**: somma la copertura lungo una sezione e la
> confronta con la larghezza scritta nella tile.

### Massimo, non somma

Dove due segmenti della stessa strada si incontrano, un pixel è vicino a tutti e
due. Si prende il **massimo** delle due coperture: con la somma, ogni curva
avrebbe un puntino più scuro.

### Alfa premoltiplicato

> 💡 **Esempio.** Un pixel di strada grigia (100, 100, 100, opaco) e uno
> trasparente vicino (0, 0, 0, alfa zero). La mipmap ne fa la media: in alfa
> normale viene (50, 50, 50) con alfa a metà, cioè un grigio **più scuro**
> semitrasparente. In premoltiplicato il primo è (100, 100, 100, 1) e il
> secondo (0, 0, 0, 0): la media è (50, 50, 50, 0,5), che "ripremoltiplicato"
> torna il grigio 100 a metà copertura. Giusto.

Il materiale compone con:

```
colore = foto * (1 - alfa) + strade
```

---

## 12.5 Il runtime

`UGeoRoadsSubsystem` fa per le strade quello che il subsystem delle ortofoto fa
per le foto, con una differenza: le foto si caricano, le strade si **disegnano**,
una volta per tile di terreno. Per ogni tile, a ogni frame:

1. niente da disegnare qui? (livello troppo alto, o nessuna linea) → pronta;
2. il disegno è finito? → diventa texture (poche per frame);
3. non c'è ancora? → si carica la tile di linee, poi si lancia il disegno;
4. intanto → le strade del padre, ritagliate.

E le regole imparate con le ortofoto: una tile compare **solo con le sue
strade** (o con quelle del padre), e si lavora **prima per ciò che si vede**.

### Più vestitori

Fino alla Fase 6 il terreno aveva un solo "vestitore", le ortofoto, che gli
diceva quali tile erano pronte. Ora sono due. Il predicato è diventato una
**mappa con un nome per vestitore** (`"Ortofoto"`, `"Strade"`), e una tile è
pronta quando lo dicono tutti.

> ⚠️ **Trappola.** Con un predicato solo, accendere le strade avrebbe
> **sostituito** quello delle ortofoto: le tile sarebbero tornate a comparire
> senza foto per un frame, cioè i flash della terza prova.

### Memoria video

Una texture di strade pesa 0,33 MB a 256 pixel e 5,3 MB a 1024. Il budget
(`geo.Roads.Budget`) limita le texture delle tile **nascoste**; quelle a
schermo le hanno sempre.

---

## 12.6 Le strade 3D

Da vicino le strade diventano **geometria** (`Roads/RoadMesh.h`): sulle tile di
terreno al livello più fine, cioè attorno alla camera quando si vola bassi.
Salendo, il quadtree smette di scegliere quelle tile e le strade tornano
dipinte da sole. Il racconto completo, con i numeri, è in `docs/strade-3d.md`.

Il problema è **stare sul terreno**: non sul DEM, ma sulla mesh che si vede,
con un post ogni due e le celle divise lungo la diagonale.

1. La quota si prende dallo **stesso triangolo** che il terreno disegna
   (`FSurfaceSampler`).
2. La strada si **spezza** dove attraversa uno spigolo della mesh: fra due
   tagli il terreno è un piano, e il nastro ci sta sopra.
3. La si **solleva** di 20 cm, più 2 cm per gradino di importanza: agli
   incroci la strada più importante sta sopra, e le due superfici non
   lampeggiano l'una nell'altra (*z-fighting*).

> 💡 **Esempio.** I ponti non seguono il terreno: l'impalcato va dritto fra le
> due spalle e ha fianchi e fondo. Su una valle di 60 m il test misura la
> strada normale a 340 m e l'impalcato a 400 m.

> ⚠️ **Trappola (presa dai test).** Nelle curve strette i due bordi interni di
> due sezioni consecutive si incrociano e il quadrilatero diventa un
> "papillon": i suoi due triangoli guardano in direzioni opposte. Decidere il
> verso una volta per quadrilatero ne lasciava uno su 1.740 rovesciato, cioè
> invisibile dall'alto. Ora si decide triangolo per triangolo.

La mesh della strada vive **nel provider, dentro la tile**: stessa
trasformazione, stessa visibilità, stesso rebase. Non c'è un momento in cui il
terreno è a schermo e la sua strada no, o il contrario.

---

## Alternative considerate

| Alternativa | Perché no |
|---|---|
| Disegnare le strade nella pipeline, come immagini | Decine di milioni di file per avere mezzo metro per pixel; stile fisso |
| Strade 3D OVUNQUE, anche da lontano | Milioni di triangoli per linee larghe meno di un pixel. Da lontano dipinte, da vicino 3D (sezione 12.6) |
| Decal di Unreal, una per segmento | Milioni di decal; ognuna costa nel renderer |
| Runtime Virtual Texture di Unreal | Pensata per un Landscape locale, non per un pianeta con il rebasing |
| Disegno sulla GPU (render target) | Più veloce, ma non si prova senza il motore. Il disegno su CPU costa 3–25 ms su un worker, accettabile |
| Libreria OSM dedicata (osmium) | Una dipendenza in più; GDAL legge già il `.pbf` |

---

## Nota Unreal

### Parametri di un'istanza di materiale

Il materiale `M_GeoTerrain` dichiara dei **parametri** (`BaseColor`, `DrapeUv`,
`Overlay`, `OverlayUv`, `OverlayStrength`). Ogni tile ha la sua **istanza
dinamica** (`UMaterialInstanceDynamic`) e ci scrive i propri valori con
`SetTextureParameterValue`, `SetVectorParameterValue`,
`SetScalarParameterValue`. Lo shader è uno solo; cambiano i valori.

Due cose che servono sapere:

- un parametro che il materiale **non ha** si può scrivere senza errori: non
  succede niente. Per questo il provider controlla all'avvio
  (`GetTextureParameterValue`) che `Overlay` esista, e se manca lo dice;
- `ClearParameterValues()` riporta un'istanza ai valori di default del
  materiale: lo si usa quando si toglie la foto a una tile che ha ancora le
  strade.

### sRGB e texture premoltiplicate

Una texture con `SRGB = true` contiene valori codificati con la curva sRGB; la
scheda video li riporta in luce lineare **prima** di filtrarli e prima che il
materiale li veda. Il rasterizzatore scrive quindi il colore premoltiplicato
**in luce lineare, poi codificato sRGB**: dopo la decodifica il materiale
riceve esattamente il premoltiplicato lineare, e le medie delle mipmap sono
giuste.

### `FName` come chiave

I vestitori del terreno sono registrati con un `FName` (`TEXT("Strade")`). Un
`FName` è un nome "internato": il motore tiene una tabella di stringhe e il nome
è un indice. Confrontarne due costa quanto confrontare due interi, ed è per
questo che Unreal lo usa per tutto ciò che è un identificatore.

### `UE::Tasks` e la coda condivisa

Il disegno gira con `UE::Tasks::Launch`, come la costruzione delle mesh
(capitolo 11). La lambda **copia** tutto ciò che usa (la tile in uno
`std::shared_ptr`, lo stile, la richiesta, una "generazione") e consegna il
risultato in una `TQueue` tenuta viva da un `TSharedPtr`: se il subsystem
chiude mentre un disegno è in corso, la coda sopravvive al subsystem e il
lavoro non scrive in memoria già liberata.

---

## Dove sta nel codice

| File | Cosa |
|---|---|
| `Pipeline/geoworld/roadclasses.py` | da tag OSM a classe, larghezza, flag |
| `Pipeline/geoworld/osmextract.py` | lettura con GDAL |
| `Pipeline/geoworld/vectorformat.py` | formato `.gvt` |
| `Pipeline/geoworld/vectorbuild.py` | estrai, taglia, indici, verifica |
| `Pipeline/geoworld/fetchosm.py` | download da Geofabrik |
| `GeoTiles/Public/Tiles/VectorClasses.h` | le classi, contratto con la pipeline |
| `GeoTiles/Public/Tiles/VectorTileFormat.h` | lettura di `.gvt` |
| `GeoTiles/.../Streaming/GeoVector*` | dataset e caricamento |
| `GeoRender/Public/Roads/RoadRasterizer.h` | il disegno |
| `GeoRender/Public/Roads/RoadMesh.h` | le strade 3D: superficie, nastri, ponti, atlante |
| `GeoRender/.../Roads/GeoRoadsSubsystem.*` | il runtime |
| `GeoRender/.../Imagery/GeoRuntimeTexture.*` | texture da pixel, condivisa con le foto |
| `Tools/StandaloneTests/georoads_main.cpp` | i test che misurano |

---

## Riepilogo

- Le strade sono **linee**, non pixel: si tagliano in tile vettoriali e si
  disegnano al momento, alla risoluzione che serve.
- Ogni classe compare da un **livello**: avvicinandosi arrivano le strade minori.
- Il disegno è in **metri**, con un antialias che rispetta la forma del pixel,
  e in **alfa premoltiplicato** per avere mipmap giuste.
- Una tile compare **solo con le sue strade** o con quelle del padre: il terreno
  ora accetta più vestitori, per nome.
- Dopo l'aggiornamento il materiale va **rifatto** (`geo.Imagery.CreateMaterial`).
- Da vicino le strade sono **3D**: posate sui triangoli del terreno, sollevate
  di qualche centimetro per classe, con ponti dritti fra le spalle.

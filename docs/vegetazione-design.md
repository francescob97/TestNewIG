# La vegetazione — le piante

> Come provarla: `docs/vegetazione-verifica.md`. Il racconto per chi impara:
> `docs/manuale/16-vegetazione.md`. Gli animali verranno dopo, su questa base.

## 1. L'idea in breve

Gli alberi crescono **dove crescono davvero**, cioè dove la mappa europea
della copertura del suolo dice che c'è bosco, macchia o prato. La specie
dipende dalla quota e un po' dalla latitudine. Si fermano al limite del bosco
e sulle pareti troppo ripide.

Si vedono solo **attorno alla camera**, entro ~2,5 km; più in là c'è
l'ortofoto, che il bosco lo mostra già. Le forme degli alberi sono fatte
**da codice**, senza asset da scaricare.

```
 ESA WorldCover (10 m)  --geoworld build-vegetation-->  tile .gvg (livello 13)
                                                             |
                     UGeoVegetationStreamingSubsystem  <-----+
                                   |
   terreno DISEGNATO  -->  UGeoVegetationSubsystem: celle di ~600 m attorno alla camera
   (tile visibili)            |  worker: chi cresce dove (TreePlacement.h, VegetationCells.h)
                              |  game thread: un HISM per forma, con gli alberi della banda
                              v
                     M_GeoVegetation: verde, luce, neve (MPC_GeoWeather), vento
```

| | |
|---|---|
| Dati | ESA WorldCover 2021 v200, 10 m, CC BY 4.0, letti a pezzi dalla rete |
| Tile | `.gvg`, livello 13 (2,4 km), 256x256 classi, zlib; solo dove cresce qualcosa |
| Griglia | un albero possibile ogni 8 x 8 m, fissa nel mondo |
| Specie | latifoglie, conifere, pini domestici, arbusti; 2 forme ciascuna |
| Forme | 2 LOD: vicino fino a 332 triangoli, lontano 20–28 |
| Celle | livello 15 (~610 x 430 m a 45°), entro 2,5 km dalla camera |
| Bande | vicino 700 m (tutti, con l'ombra), medio 1.500 m (metà), lontano 2.500 m (un quarto) |
| Bosco | ~14.500 alberi per km², ~3.800 per cella |

## 2. I dati: la copertura del suolo

### 2.1 Perché ESA WorldCover

Per mettere gli alberi al posto giusto serve sapere, punto per punto, se lì
c'è un bosco. Le possibilità:

| Sorgente | Risoluzione | Pro | Contro |
|---|---|---|---|
| **ESA WorldCover 2021** | 10 m | tutto il mondo, gratis, **senza account**, file COG su un bucket pubblico | 11 classi, nessuna specie |
| Corine Land Cover | 100 m | 44 classi, distingue conifere e latifoglie | 100 m: un parco di città sparisce, i bordi del bosco sono a scalini da 100 m |
| Copernicus HRL Tree Cover Density | 10 m | la densità degli alberi in % | serve un account, e scaricare a mano |
| OpenStreetMap (`landuse=forest`, `natural=wood`) | vettoriale | lo usiamo già per strade e acqua | copertura disuguale: ottima in città, a buchi in montagna |
| NDVI da Sentinel-2 | 10 m | lo calcoleremmo dalle scene che abbiamo | verde non vuol dire albero: un prato irrigato è più verde di un bosco |

WorldCover ha la stessa risoluzione dell'ortofoto Sentinel: il bosco degli
alberi coincide con il bosco della foto. La specie non la dà nessuna sorgente
gratuita a 10 m, quindi la deduciamo dalla quota (sezione 3.3).

**Il trucco che lo rende comodo.** I file sono di 3 x 3 gradi, ~70 MB
l'uno, in formato **COG** (GeoTIFF "cloud optimized": i pixel sono in
blocchi, con un indice all'inizio). GDAL li legge **a pezzi** via HTTP
(`/vsicurl/`): per Torino si scaricano pochi MB, non il file intero. Niente
account, niente download a mano. Chi lavora senza rete scarica i file una volta
e li passa con `--worldcover`.

### 2.2 Le classi

Nelle tile scriviamo i **nostri** numeri, piccoli e consecutivi. Il contratto
fra Python (`vegclasses.py`) e C++ (`VegetationClasses.h`) lo controlla un test
che legge l'header C++.

| Nostra | WorldCover | Cosa | Alberi / arbusti per punto |
|---|---:|---|---|
| 0 NoData | 0 | fuori dalla mappa | — |
| 1 Trees | 10 | bosco, parchi, filari fitti | 0,85 / 0,08 |
| 2 Shrubs | 20 | arbusteti, macchia | 0,05 / 0,55 |
| 3 Grass | 30 | prati, pascoli | 0,012 / 0,04 |
| 4 Crops | 40 | campi | 0,01 / 0,005 |
| 5 Built | 50 | costruito | 0,03 / 0,01 |
| 6 Bare | 60 | roccia, sabbia | — |
| 7 Snow | 70 | neve e ghiacciai | — |
| 8 Water | 80 | acqua | — |
| 9 Wetland | 90 | zone umide | 0,02 / 0,12 |
| 10 Mangrove | 95 | mangrovie | 0,85 / 0 |
| 11 Moss | 100 | muschi, licheni | — |

Le probabilità sono "per punto della griglia" (uno ogni 64 m²). Nel bosco
0,85 vuol dire un albero ogni ~75 m²: le chiome (6–15 m di diametro) si
toccano, come in un bosco vero visto dall'alto. In città 0,03 sono un albero
ogni ~2.000 m²: i viali e i cortili. I parchi, invece, WorldCover li segna
come bosco.

### 2.3 La pipeline: `geoworld build-vegetation`

```bat
geoworld build-vegetation --terrain dataset/terreno -o dataset/vegetazione
geoworld verify-vegetation -o dataset/vegetazione
geoworld inspect-vegetation dataset/vegetazione/13/8541/2044.gvg
```

1. **L'area**: quella del dataset del terreno (`--terrain`), o `--bbox`.
2. **I file WorldCover** che la coprono (`N45E006`...). Quelli che non
   esistono (il mare aperto non ha file) si saltano.
3. **A blocchi** di livello 9 (~40 km): una sola `gdal.Warp` per blocco, al
   passo esatto delle tile del livello 13 (4096 x 4096 pixel), con il
   **campione più vicino**. Sono classi, non quote: la media di "bosco" e
   "acqua" non è niente. Poi si traducono i codici e si taglia in tile.
4. Le tile dove **non cresce niente** non si scrivono: mare, ghiacciai,
   deserti di roccia. Per il runtime una tile che manca vuol dire "niente
   piante", senza aprire un file.
5. I blocchi fatti si segnano in `_work/blocks_13.json`: se il comando si
   ferma, rilanciandolo riparte da dove era (`--force` rifà tutto).
6. **Indice e manifest** dalle tile su disco, con la copertura misurata
   ("bosco 28%") e l'attribuzione che la licenza chiede.

Su Torino (0,06° x 0,05°, 16 tile) ci vogliono 2 secondi; il centro è 85%
costruito e 13% alberi, cioè i parchi e i viali.

### 2.4 Il formato `.gvg`

```
header   32 byte: "GWVG", versione, flag, livello, x, y, raster (256),
         riservato, byte del payload, riservato
payload  zlib di 256 x 256 byte: la classe; righe dal NORD, colonne dall'OVEST
indice   <livello>/index.bin: "GWVI", livello, numero; record di 16 byte
         (x, y, flag, byte del file), ordinati per (y, x)
```

Un pixel è ~10 m, come WorldCover. Tutte le tile stanno a **un solo
livello**: gli alberi servono solo da vicino, quindi non c'è una piramide. Le
classi sono lunghe file di numeri uguali: la zlib le riduce di dieci volte
(un bosco intero pesa ~300 byte). Il flag dice se la tile ha alberi o
arbusti.

## 3. Chi cresce dove (`TreePlacement.h`)

### 3.1 La griglia fissa

Il punto di partenza è una **griglia geografica uguale per tutto il
pianeta**: righe ogni 8 m verso nord, e in ogni riga colonne ogni 8 m verso
est (il passo in longitudine lo decide il coseno della riga). Ogni punto ha un
**hash** della riga e della colonna, e da quello i suoi numeri a caso:

- se ci cresce qualcosa, e cosa;
- dove sta dentro la sua casella (fra il 10 e il 90%: niente file regolari, mai
  due tronchi a meno di ~2 m);
- quanto è alto, quanto è larga la chioma, come è girato, di che verde;
- `Thin`, il numero che decide se resta da lontano (sezione 5.2).

Da qui vengono due proprietà, provate dai test:

- **lo stesso albero sta sempre nello stesso posto.** Tornando indietro, o
  ricostruendo una cella perché il terreno sotto si è affinato, si ritrova
  identico;
- **niente cuciture fra le celle.** Un punto sta in una cella sola (bordi mezzi
  aperti), e la griglia non dipende dalle celle: due celle vicine hanno insieme
  esattamente i punti del rettangolo intero.

### 3.2 Albero o arbusto

La classe nel punto (dalla tile `.gvg`) dà le due probabilità della tabella
2.2. Il numero a caso del punto le confronta: sotto la prima un albero, fra la
prima e la somma un arbusto, sopra niente. `geo.Vegetation.Density` moltiplica
entrambe (0,5: metà alberi, sempre gli stessi punti).

### 3.3 Quale albero

Nessuna sorgente gratuita dice la specie a 10 m. Le regole, semplici e
italiane:

- **conifere** (abeti, larici, pini silvestri): dal 10% in pianura al 95%
  sopra ~1.500 m sulle Alpi, con un passaggio morbido fra 700 e 1.500 m.
  Scendendo verso sud la fascia sale di ~180 m per grado sotto il 46°
  parallelo: in Calabria a 1.500 m ci sono ancora le faggete;
- **pini domestici**: nelle pianure e colline del centro-sud (sotto il 44,5°
  parallelo, sotto i ~300 m), uno su sei. Al nord nessuno;
- **latifoglie** (querce, faggi, castagni): il resto.

La quota è quella **sul mare**: la quota ellissoidica del suolo meno il
geoide. Il geoide viene dal dataset dell'acqua. Senza, si usa l'ellissoide, e
in Italia tutto sta ~45 m più in alto di quanto è.

### 3.4 Limite del bosco e pendenza

- sopra **2.100 m** niente alberi: solo arbusti (pino mugo, rododendri);
- sopra **2.700 m** niente;
- negli ultimi 600 m prima di ogni limite tutto si fa più piccolo, fino alla
  metà;
- oltre **40°** di pendenza niente alberi; gli arbusti resistono fino a 50°.
  La pendenza è quella del terreno **disegnato**.

### 3.5 Grandezze e colori

| Specie | Altezza | Chioma / altezza | Verde (lineare) |
|---|---|---|---|
| latifoglie | 9–21 m | 0,60–0,85 | 0,075 0,125 0,035 |
| conifere | 14–30 m | 0,30–0,42 | 0,035 0,072 0,040 (scuro, bluastro) |
| pini domestici | 10–17 m | 0,95–1,20 | 0,060 0,100 0,040 |
| arbusti | 1–3,2 m | 1,00–1,60 | 0,090 0,115 0,050 (oliva) |

Ogni albero ha il verde della sua specie, più chiaro o più scuro (±25%) e
un po' più giallo o più blu.

## 4. Le forme (`TreeMesh.h`)

Otto forme: quattro specie per due varianti. Ognuna sta nel **cubo unitario**
(larga 1, alta 1, la base al centro della faccia di sotto); l'istanza la
scala alla larghezza e all'altezza dell'albero. Pochi solidi:

| Specie | Vicino | Lontano |
|---|---|---|
| conifera | un tronco e sei coni sovrapposti, sempre più stretti | un cono |
| latifoglia | un tronco e una chioma di quattro "bolle" bitorzolute | una bolla |
| pino domestico | un tronco alto e nudo, una chioma piatta a ombrello | una bolla piatta |
| arbusto | tre bolle schiacciate a terra | una bolla |

Le due varianti hanno bitorzoli e coni diversi.

| Specie | Vicino | Lontano |
|---|---:|---:|
| latifoglie | 332 triangoli | 20 |
| conifere | 120 | 24 |
| pini domestici | 252 | 28 |
| arbusti | 240 | 20 |

**Il colore dei vertici porta i dati del materiale**, non un colore:

- R = 255 sulla chioma, 0 sul tronco;
- G = la luce del cielo, meno dentro la chioma e in basso (una occlusione
  ambientale fatta a mano);
- B = l'altezza nell'albero, 0 alla base e 255 in cima: il vento la usa per
  piegare la cima e lasciare ferma la base.

Le facce girano come quelle del terreno: un test lo controlla con la normale
geometrica. Se in Unreal gli alberi si vedono "dal di dentro",
`geo.Vegetation.FlipWinding 1` lo dice subito.

## 5. Le celle (`VegetationCells.h`)

### 5.1 Quali celle

Una **cella** è una tile del livello 15: ~610 m nord-sud e ~430 m est-ovest a
45°. Si costruiscono le celle che hanno almeno un punto entro il raggio (2,5
km), dalla più vicina. Nella distanza conta anche **la quota della camera sopra
il suolo**: da 1.500 m di quota ne servono meno; da 3.000 m nessuna (gli alberi
sarebbero di un pixel, e la foto basta).

Con la camera al suolo sono ~94 celle.

### 5.2 Le bande, e l'isteresi

| Banda | Fino a | Alberi | Ombre |
|---|---:|---:|---|
| vicino | 700 m | tutti | sì |
| medio | 1.500 m | metà | no |
| lontano | 2.500 m | un quarto | no |

Da lontano un albero è qualche pixel e la foto sotto fa già il bosco: un
quarto basta a dare il rilievo delle chiome. Quali alberi restano lo decide
`Thin`, fisso per albero: avvicinandosi **se ne aggiungono**, nessuno cambia
posto.

**L'isteresi.** Per cambiare banda bisogna superare il confine di 75 m, nei
due versi. Senza, una cella proprio sul confine cambierebbe a ogni passo della
camera e metà degli alberi comparirebbero e sparirebbero a scatti. Un test fa
oscillare la camera di 40 m sul confine: zero cambi.

`geo.Vegetation.Radius` allarga le tre bande insieme (28%, 60%, 100% del
raggio).

### 5.3 Sul terreno disegnato, e la firma

Gli alberi stanno sul terreno **che si vede**, non sul DEM. Il terreno
disegnato è fatto di triangoli piani fra i post, a passo 2, e a livelli
diversi secondo la distanza. Messo sul DEM, un albero in un avvallamento
lontano galleggerebbe di metri sopra la mesh grossolana, o ci sparirebbe
dentro. La quota la dà `FSurfaceSampler`, la stessa funzione delle strade 3D
e delle stampe "dal suolo": stessa griglia, stessa diagonale.

Per ogni cella si guardano le **tile di terreno visibili** che la toccano (con
un pezzo d'area, non solo un bordo). La loro **firma** (un hash delle chiavi,
in qualunque ordine, e del passo della mesh) dice su che terreno è stata fatta
la cella. Quando il terreno sotto si affina la firma cambia, e la cella **si
rifà**. Intanto la vecchia resta a schermo: niente buchi nel bosco.

### 5.4 Quanto pesa

In un bosco continuo (il caso peggiore):

| | |
|---|---|
| alberi in una cella | ~3.800 (il 93% dei punti: alberi e arbusti) |
| alberi a schermo | ~110.000 (22.000 vicini, 40.000 medi, 45.000 lontani) |
| triangoli | ~5 milioni: ~2,8 nei ~9.000 alberi entro ~450 m (forma dettagliata), ~2,3 nel resto |
| memoria | ~52 byte per albero: ~20 MB per tutte le celle |
| una cella sul worker | pochi millisecondi |

Le manopole, in ordine di efficacia: `geo.Vegetation.Density 0.5` (metà di
tutto), `geo.Vegetation.Radius 1500`, `geo.Vegetation.Shadows 0`.

## 6. In Unreal

### 6.1 Il giro

Ogni quarto di secondo (`UGeoVegetationSubsystem::Synchronise`):

1. le tile di terreno visibili (`GetBuiltTileKeys`) e la camera;
2. le celle attorno, con la loro banda; quelle uscite dal raggio si tolgono;
3. per ogni cella, dalla più vicina:
   - la tile `.gvg` che la contiene (chiesta allo streaming: assente vuol dire
     cella vuota);
   - la firma del terreno sotto;
   - se è cambiata, un lavoro sul **worker** (al massimo 4 in volo), che fa
     `BuildCellInstances`: chi cresce dove e le istanze nel frame della cella;
   - se è cambiata la banda, si rifanno i componenti con le istanze che ci
     sono già.

Ogni frame si consegnano al massimo 2 celle (`DrainFinished`): creare un
componente con 3.000 istanze costa, e il frame non deve sentirlo. Un lavoro
resta "in volo" finché non è consegnato. Così il giro delle celle non lo
rilancia mentre aspetta il suo turno. I lavori di prima di un cambio
(`Density`, apertura di un dataset) portano la generazione vecchia e si
buttano.

### 6.2 Le forme come UStaticMesh a runtime

Le otto forme si costruiscono all'avvio, da codice:
`FTreeMesh` → `FMeshDescription` → `UStaticMesh::BuildFromMeshDescriptions`,
con **due LOD**, la forma dettagliata e quella semplice.

- `bFastBuild` costruisce i dati di disegno direttamente, senza la pipeline
  dell'editor: funziona anche nel gioco impacchettato.
- Le soglie dei LOD si mettono a mano dopo, nei dati di disegno
  (`ScreenSize[1] = 0,02`: un albero di 15 m passa alla forma semplice oltre
  ~450 m). Senza, varrebbe sempre il LOD 0.
- Il colore dei vertici passa da `FLinearColor::FromSRGBColor`: la costruzione
  lo ricodifica sRGB, e così al materiale arrivano esattamente i byte di
  `TreeMesh.h`.

### 6.3 HISM e i dati per istanza

Un **`UHierarchicalInstancedStaticMeshComponent`** per forma per cella: una
mesh, migliaia di copie, un draw call. Il componente divide le istanze in
gruppi vicini (un albero gerarchico, da cui il nome) e per ogni gruppo sceglie
il LOD e se è visibile.

Ogni istanza ha la sua trasformazione **nel frame della cella** (metri: X
nord, Y est, Z alto) e cinque numeri per il materiale (**Per Instance Custom
Data**): il verde (3), l'altezza in metri, un numero a caso per la fase del
vento.

La trasformazione del componente è `GetLocalNeuTransform(centro della cella)`,
con la scala metri → centimetri, come le tile del terreno. **Il rebase cambia
solo quella**: le istanze non si toccano.

### 6.4 Il materiale `M_GeoVegetation`

Due nodi Custom, dal testo di `VegetationShader.inl`, che i test compilano come
C++ (doppia sintassi, come le nuvole e il suolo):

- **colore**: il verde dell'istanza sulla chioma e il marrone sul tronco,
  scuriti dove la luce del cielo è poca (G). La neve copre la chioma che guarda
  il cielo, sopra la quota della neve al suolo (`GroundWeather`, la stessa del
  terreno); i tronchi restano scuri;
- **vento** (World Position Offset): l'albero si piega sottovento tanto più
  quanto più è in alto (B al quadrato: la base non si muove mai) e quanto più
  il vento è forte (al quadrato). Oscilla con un periodo suo di 2–4 secondi,
  sfasato da un albero all'altro. A 10 m/s la cima di un albero di 20 m va
  avanti e indietro fra 33 e 67 cm.

Il vento arriva da **`MPC_GeoWeather`**, parametro `VegetationWind`: nord, est
(m/s, il vento medio a 10 m dal suolo sotto la camera), la raffica di adesso
(0..1) e un orologio (s, che ricomincia ogni giorno: un float resta preciso).
Lo scrive `UGeoVegetationSubsystem` a ogni frame, leggendo il vento del meteo.

`bUsedWithInstancedStaticMeshes` è obbligatorio: senza, sui componenti di
istanze il motore usa il materiale di default e lo dice solo nell'Output Log.
`DitheredLODTransition` fa sfumare il passaggio fra le due forme.

### 6.5 I comandi

| Comando | |
|---|---|
| `geo.Vegetation.Open <cartella>` / `Close` | apre e chiude il dataset |
| `geo.Vegetation.Demo <terreno> <ortofoto\|-> <vegetazione> [acqua\|-]` | tutto in un colpo, 250 m sopra il centro |
| `geo.Vegetation.Enable <0\|1>` | accende e spegne |
| `geo.Vegetation.Density <0..4>` | quanti alberi |
| `geo.Vegetation.Radius <m>` | fin dove |
| `geo.Vegetation.Shadows <0\|1>` | le ombre dei vicini |
| `geo.Vegetation.FlipWinding <0\|1>` | se si vedono dal di dentro |
| `geo.Vegetation.Debug <0\|1>` / `Stats` | overlay (celle colorate per banda) e numeri |

Nelle stampe della posizione (`geo.Hud`) c'è la riga "Vegetazione": alberi a
schermo, celle, celle in attesa.

## 7. Cosa non fa (ancora)

- **Le specie sono dedotte**, non lette: un castagneto e una faggeta alla
  stessa quota sono lo stesso bosco. Corine (100 m) distingue conifere e
  latifoglie; si potrebbe usarlo per correggere la quota.
- **Le forme sono semplici** (low poly, tinta unita per specie). Il punto dove
  sostituirle con modelli tuoi è uno solo, `BuildShapeMesh` in
  `GeoVegetationSubsystem.cpp`. Un modello va bene se sta nel cubo unitario
  e ha il colore dei vertici come sopra.
- **Niente erba né sottobosco**, niente fiori, niente colori d'autunno.
- **Gli alberi non seguono il geomorphing**: quando il terreno si affina, la
  cella si rifà sul terreno nuovo con un salto (di solito di qualche
  decimetro), mentre il terreno ci scivola in mezzo secondo.
- **Lo scavo dei fiumi** non è nella quota degli alberi: sulle sponde scavate
  un albero può stare un po' sopra il suolo (l'acqua stessa non ha alberi).
- **WorldCover è del 2021**: i tagli e gli incendi dopo non ci sono.
- **Niente collisione**: si vola attraverso le chiome.
- **La scala non uniforme** (larghezza diversa dall'altezza) storce un po' le
  normali delle istanze. Sulle chiome tonde non si vede.
- **Oltre 2,5 km niente alberi**: un impostor (un'immagine dell'albero) per
  le distanze grandi è il passo successivo, se servirà.

## 8. Alternative considerate

- **Il Foliage Tool di Unreal.** Si dipinge a mano nell'editor e si salva nel
  livello. Per un mondo grande quanto l'Italia, che si carica a tile da un
  dataset, non funziona: serve generare a runtime.
- **PCG** (Procedural Content Generation, il framework di Unreal). Fa proprio
  questo, con un grafo nell'editor. Ma è un plugin che cambia spesso fra le
  versioni e non si prova senza il motore. Qui le regole sono C++ puro,
  provato dai test, e il pezzo Unreal è sottile.
- **Nanite foliage.** È il futuro per le chiome dettagliate. Le nostre forme
  sono di poche centinaia di triangoli, e Nanite non accetta il World Position
  Offset senza costi (il vento).
- **ISM invece di HISM.** Più semplice; ma il LOD lo sceglierebbe per tutto il
  componente insieme, non per gruppi di alberi.
- **Un componente per cella invece che per forma.** Un HISM ha una sola mesh:
  servono otto componenti (al più) per cella.
- **Mettere gli alberi sul DEM.** Più semplice e senza ricostruzioni. Ma gli
  alberi lontani galleggiano sulla mesh grossolana, o ci affondano.

## 9. Dove sta nel codice

| File | |
|---|---|
| `Pipeline/geoworld/vegclasses.py` | le classi, il contratto con il C++ |
| `Pipeline/geoworld/vegformat.py` | il formato `.gvg`, indice, manifest, la tile di prova |
| `Pipeline/geoworld/vegbuild.py` | `build-vegetation`, `verify-vegetation` |
| `Pipeline/tests/test_vegetation.py` | 20 test, con un WorldCover finto |
| `GeoTiles/Public/Tiles/VegetationClasses.h` | le classi (C++ puro) |
| `GeoTiles/Public/Tiles/VegetationTileFormat.h` | lettura di tile e indice (C++ puro) |
| `GeoTiles/Public/Streaming/GeoVegetationDataset.h` | il dataset su disco |
| `GeoTiles/Public/Streaming/GeoVegetationStreamingSubsystem.h` | il caricamento asincrono |
| `GeoRender/Public/Vegetation/TreePlacement.h` | chi cresce dove (C++ puro) |
| `GeoRender/Public/Vegetation/TreeMesh.h` | le forme (C++ puro) |
| `GeoRender/Public/Vegetation/VegetationCells.h` | celle, bande, firma, terreno disegnato (C++ puro) |
| `GeoRender/Public/Vegetation/VegetationShader.inl` | il materiale (HLSL e C++) |
| `GeoRender/Public/Vegetation/GeoVegetationSubsystem.h` | tutto il resto, in Unreal |
| `GeoWorldEditor/Private/GeoTerrainMaterialFactory.cpp` | `M_GeoVegetation`, `VegetationWind` |
| `Tools/StandaloneTests/geovegetation_main.cpp` | 33 test C++ |

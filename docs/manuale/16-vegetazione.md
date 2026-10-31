# Capitolo 16 — La vegetazione: le piante

> Come si mettono gli alberi in un mondo grande quanto l'Italia: da dove si sa
> dov'è un bosco, chi decide che albero cresce in un punto, come si fanno le
> forme senza modelli, e come se ne disegnano centomila senza fermare il
> frame. Il documento di progetto con tutti i numeri è
> `docs/vegetazione-design.md`; come provarla è in `docs/vegetazione-verifica.md`.
> Gli animali verranno dopo, sopra queste fondamenta.

---

## L'idea in breve

L'ortofoto mostra già i boschi, ma piatti: da vicino sono macchie verdi stese
sul terreno. Per avere alberi veri servono quattro cose:

1. **dove**: una mappa che dica, ogni 10 m, se lì c'è bosco, prato, città;
2. **quale**: per ogni punto, se cresce un albero, di che specie, quanto alto;
3. **come**: la forma di un albero, in triangoli;
4. **quanti a schermo**: solo attorno alla camera, sempre meno con la
   distanza, senza mai fermare il frame.

I primi tre sono C++ puro e Python, provati senza Unreal. Il quarto è il
pezzo Unreal, ed è sottile: componenti di istanze e un materiale.

---

## 16.1 Dove: la copertura del suolo

Una **mappa di copertura del suolo** (land cover) divide il territorio in
classi: bosco, arbusti, prato, campi, costruito, acqua, roccia, neve. Quella che
usiamo è **ESA WorldCover**, fatta dall'Agenzia Spaziale Europea con i
satelliti Sentinel-1 e Sentinel-2: tutto il mondo, a 10 m, gratuita.

Il trucco che la rende comoda è il formato. I file sono **COG** (Cloud
Optimized GeoTIFF): i pixel sono in blocchi, e all'inizio del file c'è un
indice che dice dove sta ogni blocco. GDAL legge l'indice, poi chiede al server
**solo i blocchi che servono** (una "richiesta di intervallo" HTTP). Un file
copre 3 x 3 gradi e pesa 70 MB, ma per Torino se ne scaricano pochi MB. Non
c'è niente da scaricare a mano e non serve un account.

> 💡 **Esempio.** `geoworld build-vegetation --bbox 7.55 45.00 7.80 45.15`
> legge due file (N42E006 e N45E006) in 6 secondi e scrive 96 tile: bosco
> 33%, prato 15%. Il resto è soprattutto città.

La pipeline taglia la mappa in **tile `.gvg`**, con lo stesso tiling del
terreno ma a un livello solo (13: 2,4 km di lato, 256 pixel da ~10 m). In
ogni pixel c'è un byte, la classe, compresso con zlib. Un bosco intero pesa
~300 byte. Le tile dove non cresce niente (mare, ghiacciai) non si scrivono:
per il runtime una tile che manca vuol dire "qui niente piante".

> ⚠️ **Trappola.** Le classi si **ricampionano** con il campione più vicino,
> mai con la media. La media di "bosco" (10) e "acqua" (80) è 45, cioè un
> numero che non vuol dire niente (o peggio, "campi").

> ⚠️ **Trappola (incontrata).** Le tile del bordo sporgono dall'area chiesta. Un'area
> che comincia esattamente a 45° N ha una riga di tile appena sotto, nel file
> WorldCover del 42°. La prima versione sceglieva i file sull'area e lasciava
> quella riga vuota. Ora li sceglie sulle tile.

---

## 16.2 Quale: una griglia fissa nel mondo

Il modo più ovvio di mettere gli alberi è "prendi N punti a caso nella cella".
Ha due difetti gravi:

- **cambia ogni volta**: se la cella si ricostruisce (e succede, sezione
  16.4), gli alberi cambiano posto davanti ai tuoi occhi;
- **le celle hanno le cuciture**: lungo il bordo fra due celle i punti si
  addensano o si diradano, e si vede una riga.

La soluzione è una **griglia fissa nel mondo**: un punto ogni 8 m verso nord e
verso est, numerato (riga, colonna) a partire dall'equatore e da Greenwich.
Ogni punto ha un **hash** della sua riga e colonna: un numero che sembra a
caso, ma è sempre lo stesso per quel punto. Dall'hash, con dei "sali"
diversi, escono tutti i suoi numeri: se ci cresce qualcosa, dove sta nella sua
casella, l'altezza, la larghezza, la rotazione, il verde.

Per esempio (numeri inventati, solo per vedere il meccanismo):

```
punto (riga 562.104, colonna 76.391)  --hash-->  0x9c41...  --sale 2-->  0,31: un albero (bosco: < 0,85)
                                                            --sale 7-->  0,62: latifoglia (conifere al 10%)
                                                            --sale 8-->  0,44: alto 15,2 m
```

Così l'albero in quel punto è **sempre lo stesso**, da qualunque cella lo si
guardi e quante volte si vuole. Le celle diventano solo un modo di raggruppare:
un punto sta in una cella sola (i bordi sono "mezzi aperti": il bordo sud e
ovest dentro, nord ed est fuori), e due celle vicine hanno insieme esattamente
i punti del rettangolo intero.

> 💡 **Esempio.** Il test conta i punti di due celle vicine e del rettangolo
> che le contiene: 8.748 in tutto, nessuno due volte, contro gli 8.737 attesi
> da 8 x 8 m.

**Chi cresce.** La classe nel punto dà due probabilità: un albero (bosco
0,85), un arbusto (bosco 0,08). In un prato sono 0,012 e 0,04: qualche
cespuglio e un albero isolato ogni tanto. In città 0,03: i viali.

**Quale specie.** Nessuna mappa gratuita dice la specie a 10 m. Si deduce
dalla **quota sul mare**, con regole che un forestale italiano riconoscerebbe:

- le **conifere** vanno dal 10% in pianura al 95% oltre i 1.500 m sulle Alpi;
  scendendo verso sud la fascia sale di 180 m per grado di latitudine;
- i **pini domestici**, uno su sei, nelle pianure del centro-sud;
- sopra i **2.100 m** (il limite del bosco) solo arbusti; sopra i 2.700 niente;
- oltre i **40°** di pendenza niente alberi (gli arbusti resistono fino a 50°).

---

## 16.3 Come: forme fatte da codice

Ogni specie ha due forme ("varianti") e ogni forma due dettagli: **vicino**
(fino a 332 triangoli) e **lontano** (20–28: un cono, una bolla). Sono fatte di
pochi solidi: coni impilati per le conifere, quattro bolle bitorzolute per le
latifoglie, un ombrello piatto su un tronco nudo per il pino domestico.

Tutte stanno nel **cubo unitario**: alte 1, larghe 1. Ogni albero le scala alla
sua altezza e larghezza. Otto forme bastano per un bosco intero: è la
dimensione, la rotazione e il colore a renderlo vario.

**Il colore dei vertici porta i dati, non un colore.** Il canale R dice se
il vertice è chioma (255) o tronco (0). G dice quanta luce del cielo arriva lì:
meno sotto la chioma e dentro, un'occlusione ambientale fatta a mano. B dice a
che altezza dell'albero sta, da 0 alla base a 255 in cima. Il materiale li usa
per il colore e per il vento.

---

## 16.4 Quanti: celle, bande, il terreno disegnato

Un bosco ha ~14.500 alberi per km². Entro 2,5 km dalla camera ci sono 20 km²:
290.000 alberi. Troppi, e soprattutto inutili: a 2 km un albero è di due
pixel, e la foto sotto fa già il bosco.

**Le celle.** Gli alberi si costruiscono a **celle**, le tile del livello 15
(~610 x 430 m). Si tengono solo le celle entro 2,5 km dalla camera, contando
anche la quota: da 3.000 m sopra il suolo nessuna.

**Le bande.** Più una cella è lontana, meno alberi tiene:

| | fino a | alberi | ombre |
|---|---:|---:|---|
| vicino | 700 m | tutti | sì |
| medio | 1.500 m | metà | no |
| lontano | 2.500 m | un quarto | no |

Quali restano lo decide un numero dell'albero, `Thin`, fisso come tutto il
resto. Avvicinandosi gli alberi **si aggiungono**, e nessuno cambia posto.

**L'isteresi.** Per cambiare banda una cella deve superare il confine di 75 m.
Senza, la camera ferma proprio sul confine farebbe comparire e sparire metà
degli alberi a ogni piccolo movimento. È la stessa idea del termostato: si
accende a 19 °C e si spegne a 21, non accende e spegne a 20.

**Il terreno disegnato.** Gli alberi devono stare sul terreno **che si vede**.
Lontano dalla camera il terreno è una mesh grossolana, con triangoli di
centinaia di metri; il DEM sotto ha i dettagli. Un albero messo sul DEM in un
avvallamento lontano starebbe metri **sotto** la mesh: sparirebbe. Messo su un
dosso, galleggerebbe. Si usa allora `FSurfaceSampler`, che dà la quota della
superficie disegnata (stessa griglia, stessa diagonale dei triangoli), come
per le strade 3D.

Ma il terreno disegnato **cambia**: avvicinandosi, le tile si affinano. Per ogni
cella si tiene una **firma**: un hash delle tile di terreno visibili sotto di
lei. Quando cambia, la cella si rifà sul terreno nuovo, sul worker. Intanto la
vecchia resta a schermo, e non si vede mai un buco.

---

## 16.5 A schermo: istanze e un materiale

Le **istanze**: una mesh, migliaia di copie, ognuna con la sua posizione,
rotazione e scala, disegnate con una sola chiamata alla GPU. Per ogni cella
c'è un componente di istanze per forma: al più otto.

Il **materiale** fa due cose, in due nodi Custom (HLSL scritto a mano, e
provato come C++ dai test, come le nuvole):

- **il colore**: il verde dell'albero sulla chioma, il marrone sul tronco,
  più scuri dove la luce del cielo è poca. Con la neve al suolo, sopra la quota
  della neve, la chioma che guarda il cielo diventa bianca;
- **il vento**: ogni vertice si sposta sottovento, di più in cima (l'altezza
  nel canale B, al quadrato) e di più con il vento forte (al quadrato). E
  oscilla, con un periodo diverso per ogni albero: un bosco che ondeggia
  all'unisono sembrerebbe finto.

> 💡 **Esempio.** Con 10 m/s di vento la cima di un albero di 20 m va
> avanti e indietro fra 33 e 67 cm sottovento; con 5 m/s, 16 cm. In una
> burrasca di 40 m/s un abete di 30 m piega la cima di 4,5 m, e non di più:
> lo shader la ferma al 15% dell'altezza.

---

## 16.6 La libreria dei modelli, e Nanite

Le forme fatte dal codice vanno bene per cominciare, ma il piano è un altro:
**una libreria di modelli preparata nell'editor**, istanziata dai dati. È lo
stesso schema che useranno le case.

**Nanite** è il sistema di Unreal 5 per la geometria densa. Una mesh Nanite è
divisa in gruppi di ~128 triangoli, organizzati in una gerarchia da fine a
grossolano; a ogni frame la GPU sceglie, gruppo per gruppo, il dettaglio che
basta per i pixel che occupa. Niente LOD fatti a mano, e milioni di istanze
reggono. In cambio, i dati Nanite si costruiscono **nell'editor**: una mesh
fatta mentre il gioco gira non li ha.

Per questo le forme si "cuociono" una volta, come i materiali:

1. `geo.Vegetation.CreateModels` costruisce le otto forme al dettaglio
   **pieno** (fino a 3.220 triangoli: troppi per una mesh normale ripetuta
   centomila volte, niente per Nanite) e le salva come asset Nanite in
   `/GeoWorld/Vegetation`;
2. in **Project Settings → Plugins → GeoWorld - Vegetazione** otto caselle
   puntano a quei modelli. Si possono cambiare con modelli propri: un abete
   comprato, una quercia scansionata;
3. a runtime ogni cella crea istanze di quei modelli. Un modello si adatta a
   ogni albero **dai suoi bounds**: largo quanto la chioma, alto quanto
   l'albero, con la base sul suolo. Se una casella è vuota, si torna alla
   forma fatta dal codice.

> ⚠️ **Trappola.** Il vento sposta i vertici; Nanite, che decide in anticipo
> quali gruppi si vedono, deve sapere **di quanto al massimo**, altrimenti
> taglia via un albero piegato fuori dai suoi bounds. Lo shader limita la
> cima al 15% dell'altezza, e il materiale lo dichiara: 500 cm.

> ⚠️ **Trappola.** Un'ombra che si muove costringe le Virtual Shadow Maps a
> ridisegnarsi a ogni frame. Per questo il vento c'è solo entro ~700 m: oltre,
> la cima si sposta di meno di un pixel e non lo vede nessuno.

Le posizioni non sono salvate albero per albero: nei tile c'è la copertura
del suolo, e la posizione di ogni albero se ne ricava al caricamento,
sempre uguale (16.2). Salvarle tutte vorrebbe dire una ventina di GB per i
boschi italiani, contro 64 KB per tile.

---

## Alternative considerate

- **Il Foliage Tool di Unreal**: si dipinge a mano e si salva nel livello. Per
  un mondo che arriva a tile da un dataset serve generare a runtime.
- **PCG** (Procedural Content Generation): fa proprio questo, ma con un grafo
  nell'editor, che cambia fra le versioni e non si prova senza il motore.
- **Corine Land Cover** (100 m, distingue conifere e latifoglie): troppo
  grossolana per i parchi di città e i bordi del bosco.
- **Punti a caso per cella**: cuciture e alberi che cambiano posto (16.2).
- **Alberi sul DEM**: galleggiano o affondano nel terreno lontano (16.4).

---

## Nota Unreal

### Una UStaticMesh fatta a runtime

Di solito una static mesh è un asset, importato da un file FBX. Qui nasce da
codice all'avvio: si riempie una **`FMeshDescription`**, il formato
"modificabile" delle mesh in Unreal (vertici, "istanze di vertice" con normale,
colore e UV, triangoli, gruppi di materiale), e la si dà a
**`UStaticMesh::BuildFromMeshDescriptions`**, una per LOD. Con `bFastBuild` la
mesh si costruisce senza la pipeline dell'editor, quindi anche nel gioco
impacchettato. I moduli da aggiungere al `Build.cs` sono `MeshDescription` e
`StaticMeshDescription`.

> ⚠️ **Trappola.** Il colore dei vertici, nella costruzione, viene codificato
> **sRGB**: un 0,5 lineare diventa 188, non 128. Per far arrivare al materiale
> i byte esatti si passa il colore che, codificato, li ridà:
> `FLinearColor::FromSRGBColor(FColor(...))`.

### I LOD e lo ScreenSize

Un **LOD** (Level Of Detail) è una versione più semplice della stessa mesh. Il
motore sceglie quale disegnare dallo **ScreenSize**: quanto è grande l'oggetto
sullo schermo, circa il suo raggio diviso l'altezza dello schermo. Il LOD 1
degli alberi ha ScreenSize 0,02: un albero di 15 m ci arriva a ~450 m. Con
una mesh fatta a runtime le soglie vanno messe a mano nei dati di disegno.

### ISM e HISM

Un **`UInstancedStaticMeshComponent`** (ISM) disegna tante copie di una mesh
con una chiamata. Lo **`UHierarchicalInstancedStaticMeshComponent`** (HISM) in
più divide le copie in gruppi vicini, in un albero gerarchico. Così per ogni
gruppo decide se è visibile e quale LOD usare: gli alberi vicini dettagliati,
quelli in fondo alla stessa cella semplici. È quello che usa il Foliage Tool
del motore.

### Per Instance Custom Data

Ogni istanza può portare qualche numero suo, che il materiale legge con il nodo
**PerInstanceCustomData** (indice 0, 1, 2...). Qui sono cinque: il verde
(tre), l'altezza in metri, un numero a caso per la fase del vento. Si dichiara
quanti sono (`SetNumCustomDataFloats`) **prima** di aggiungere le istanze.

### Le "usage" di un materiale

Un materiale viene compilato in tante versioni, una per tipo di geometria. Per
le istanze serve la casella **Used with Instanced Static Meshes**
(`bUsedWithInstancedStaticMeshes`). Se manca, il motore disegna il materiale di
default a scacchi grigi, e lo dice solo nell'Output Log.

### UDeveloperSettings: le impostazioni in Project Settings

Una classe derivata da **`UDeveloperSettings`** compare da sola in Project
Settings, con un campo per ogni `UPROPERTY(Config, EditAnywhere)`. `Config =
Game, DefaultConfig` la salva in `Config/DefaultGame.ini`: nel progetto, e
quindi nel repository, non nelle preferenze di chi lo apre. Dal codice si
legge con `GetDefault<UGeoVegetationSettings>()`.

Le caselle sono **`TSoftObjectPtr<UStaticMesh>`**: un riferimento "morbido",
che ricorda il percorso dell'asset senza caricarlo. Si carica quando serve
(`LoadSynchronous`). Un riferimento normale caricherebbe tutti i modelli
all'avvio del motore, anche senza vegetazione.

### Una static mesh fatta da codice, nell'editor

Nell'editor la strada è diversa da quella del runtime: la `FMeshDescription`
si consegna al **source model** del LOD 0 (`CreateMeshDescription`,
`CommitMeshDescription`), si accende `NaniteSettings.bEnabled`, e `Build()` fa
tutto, dati Nanite compresi. Poi si salva il package, come per i materiali.

### Il World Position Offset, ancora

Lo stesso meccanismo delle particelle della pioggia (capitolo 15): il vertex
shader sposta ogni vertice. Il vento non costa niente alla CPU: un vettore per
frame nella Material Parameter Collection (`VegetationWind`), e la GPU piega
centomila alberi.

---

## Dove sta nel codice

| File | |
|---|---|
| `Pipeline/geoworld/vegclasses.py`, `vegformat.py`, `vegbuild.py` | la pipeline: classi, formato `.gvg`, `build-vegetation` |
| `GeoTiles/Public/Tiles/VegetationClasses.h`, `VegetationTileFormat.h` | classi e lettura delle tile (C++ puro) |
| `GeoTiles/Public/Streaming/GeoVegetationDataset.h`, `GeoVegetationStreamingSubsystem.h` | il dataset e il caricamento |
| `GeoRender/Public/Vegetation/TreePlacement.h` | la griglia, chi cresce dove, specie, grandezze |
| `GeoRender/Public/Vegetation/TreeMesh.h` | le otto forme |
| `GeoRender/Public/Vegetation/VegetationCells.h` | celle, bande, isteresi, firma, terreno disegnato |
| `GeoRender/Public/Vegetation/VegetationShader.inl` | il materiale, HLSL e C++ |
| `GeoRender/Public/Vegetation/GeoVegetationSubsystem.h` | Unreal: mesh a runtime, HISM, giro, vento, overlay |
| `GeoRender/Public/Vegetation/GeoVegetationSettings.h` | la libreria dei modelli, in Project Settings |
| `GeoWorldEditor/Private/GeoVegetationModelFactory.cpp` | `geo.Vegetation.CreateModels`: i modelli Nanite |
| `GeoWorldEditor/Private/GeoTerrainMaterialFactory.cpp` | `M_GeoVegetation` |

---

## Riepilogo

- La **copertura del suolo** (ESA WorldCover, 10 m) dice dove c'è bosco; si
  legge dalla rete a pezzi (COG) e diventa tile `.gvg`.
- Gli alberi stanno su una **griglia fissa nel mondo** con un hash per punto:
  sempre gli stessi, niente cuciture fra le celle.
- La **specie** viene dalla quota sul mare e dalla latitudine; il **limite del
  bosco** e la **pendenza** tolgono gli alberi dove non crescono.
- Le **forme** sono fatte da codice, nel cubo unitario, con i dati per il
  materiale nel colore dei vertici; due LOD ciascuna.
- Le **celle** (livello 15) attorno alla camera hanno tre **bande** con
  l'**isteresi**; stanno sul **terreno disegnato** e si rifanno quando la
  sua **firma** cambia.
- In Unreal: **HISM** con i **Per Instance Custom Data**, un materiale con il
  vento e la neve.
- I modelli vengono da una **libreria** in Project Settings: di default otto
  modelli **Nanite** cotti nell'editor (`geo.Vegetation.CreateModels`), o
  modelli propri adattati dai bounds; senza, forme fatte dal codice a
  runtime.

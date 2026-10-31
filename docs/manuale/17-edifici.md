# Capitolo 17 — Gli edifici: le città da OpenStreetMap

> Come si alza una città intera dalle sagome di una mappa: da dove vengono le
> sagome e le altezze, come un poligono con i cortili diventa un solido con
> il tetto, perché le finestre le disegna il materiale, e come i monumenti
> veri prendono il posto delle loro sagome. Il documento di progetto con
> tutti i numeri è `docs/edifici-design.md`; come provarli è in
> `docs/edifici-verifica.md`.

---

## L'idea in breve

Vista dall'alto, una città è un insieme di **sagome**: il contorno di ogni
edificio. OpenStreetMap le ha quasi tutte, e per molte sa anche quanto sono
alte, quanti piani hanno, che tetto. Da una sagoma e un'altezza si fa un
**solido estruso**: i muri sono rettangoli alzati sui lati, il tetto chiude
sopra.

Quattro pezzi, come per gli alberi:

1. **la pipeline** legge un estratto OSM e scrive le sagome in tile `.gbd`;
2. **il C++ puro** trasforma le sagome in triangoli: muri, tetti, cortili;
3. **il materiale** colora, disegna le finestre, mette neve e bagnato;
4. **Unreal** costruisce le celle attorno alla camera, e al posto di pochi
   edifici scelti mette un **modello** (un monumento, di solito Nanite).

I primi tre si provano senza Unreal; il quarto è sottile.

---

## 17.1 Le sagome e le altezze

OpenStreetMap descrive un edificio come una **way chiusa** (un anello di
punti) con il tag `building=*`. Un edificio con un cortile è un
**multipoligono**: una relazione che mette insieme un anello esterno
(`outer`) e uno o più anelli interni (`inner`), i buchi. Il driver OSM di
GDAL li consegna tutti e due come poligoni, cortili compresi.

L'altezza è il pezzo difficile, perché c'è solo per una parte degli edifici.
La pipeline la cerca in ordine di fiducia:

| Tag | Cosa dice | Esempio |
|---|---|---|
| `height` | la cima, tetto compreso | `height=18` |
| `building:levels` | quanti piani sopra terra | 5 piani → 3 m a piano + 0,5 m = 15,5 m |
| la classe | un default ragionevole | una casa 2 piani, un box 1, una torre 25 m |

> 💡 **Esempio.** `building=house` senza altri tag diventa una casa di 2
> piani: 6,5 m ai muri, più un tetto a falde deciso dalla sagoma. Con
> `building:levels=3` diventa 9,5 m. Con `height=12` è alta 12 m **in tutto**:
> il tetto sta dentro e i muri si abbassano.

La **classe** (casa, condominio, chiesa, capannone, box...) viene dal valore
di `building`, e quando è un generico `yes` da altri tag: `amenity=place_of_worship`
fa una chiesa, `man_made=tower` una torre. Serve a tre cose: l'altezza di
default, la forma del tetto, la tavolozza dei colori.

### Le parti

Una chiesa è bassa, il suo campanile alto. Lo schema **Simple 3D Buildings**
di OSM lo dice con le **parti**: dentro il contorno dell'edificio, altre
sagome con `building:part=*`, ognuna con la sua altezza e il suo tetto. La
regola dello schema: se un edificio ha delle parti, **si disegnano le parti**
e non il contorno.

La pipeline lo riconosce così: un contorno nel quale cade il centro di
qualche parte. Non lo butta, però: lo marca "solo contorno" e gli mette
accanto, nella stessa tile, le sue parti. Il motivo è nella sezione 17.6.

---

## 17.2 Le tile `.gbd`

Come le altre tile del progetto: un header, un payload compresso con zlib,
un file per tile, un indice. Due scelte meritano una parola.

**I vertici in interi di 16 bit.** Una tile del livello 15 è ~600 m; le
coordinate sono frazioni della tile moltiplicate per 16384, cioè un passo di
circa 4 cm. Più fine di quanto le sagome di OSM siano precise, e un vertice
costa 4 byte invece di 16.

**Un edificio sta nella tile del suo centro**, anche se i suoi muri
sporgono nella tile accanto. Così una cella costruisce i suoi edifici
**interi**: tagliarli al bordo della tile vorrebbe dire muri finti lungo la
linea del taglio.

> ⚠️ **Trappola.** `FCompression::UncompressMemory` di Unreal vuole sapere
> **esattamente** quanti byte usciranno. Se gli dai un numero più grande
> fallisce. Per questo l'header porta anche la dimensione decompressa: con le
> altre tile si sapeva dal formato, qui no (ogni edificio ha un numero di
> vertici diverso).

---

## 17.3 Dalla sagoma al solido

### La triangolazione

La GPU disegna solo triangoli: il tetto piano di un edificio a L va diviso
in triangoli che coprano esattamente la sagoma. Il metodo è l'**ear
clipping** ("tagliare le orecchie"): in un poligono semplice c'è sempre un
vertice il cui triangolo con i due vicini sta tutto dentro (un'"orecchia").
Lo si taglia via, il poligono ha un vertice in meno, e si ricomincia.

```
   D-----C           D-----C           D
   |     |           |    /            |\
   |  F--E    -->    |   /     -->     | \     ...
   |  |              |  F--E           |  F
   A--B              A--B              A--B
```

I cortili complicano le cose: un poligono con un buco non è "semplice".
Il trucco (di David Eberly) è un **ponte**: si collega il buco al contorno con
un taglio sottilissimo, andata e ritorno. Il poligono diventa uno solo, con
un corridoio di larghezza zero, e l'ear clipping lo digerisce. Con due
cortili, due ponti.

> 💡 **Esempio.** Il test del palazzo a corte: 40 x 40 m con un cortile di
> 16 x 16. Area attesa 1.600 − 256 = 1.344 m², e tanta ne copre la
> triangolazione, con 8 triangoli; il cortile resta vuoto.

### Muri, base, tetti

- **La base** è la quota **più bassa** del terreno disegnato sotto il
  contorno, e i muri scendono un altro metro e mezzo. Su un pendio il lato
  a monte resta sepolto, quello a valle tocca terra. Meglio un muro che entra
  nel terreno di uno che galleggia.
- **I muri** sono un rettangolo per lato, girato verso fuori. Per i cortili
  "fuori" vuol dire verso il cortile: per questo il contorno gira in senso
  antiorario e i cortili in senso orario, e un muro guarda sempre alla
  destra del verso dell'anello.
- **I tetti.** Piano, a due falde, a padiglione, a piramide, a cupola. Le
  falde si fanno solo dove la geometria è semplice: due falde e padiglione
  su una sagoma di 4 lati, piramide e cupola su una sagoma convessa. Su una
  L il tetto resta piano: per le falde servirebbe lo *scheletro* del
  poligono (lo *straight skeleton*), che è un lavoro a sé.

### Cosa porta ogni vertice

Il materiale deve sapere, per ogni punto, se è muro o tetto, a che altezza
sta sulla facciata, quanto è alto un piano. Lo porta il vertice:

| Canale | Cosa |
|---|---|
| colore RGB | il colore dell'edificio |
| colore A | 1 muro, 0 tetto |
| UV0 | metri lungo il muro, metri sopra la base |
| UV1 | altezza di un piano (0 = niente finestre), un numero a caso |

Le "UV" di solito servono a incollare una texture; qui sono solo due numeri
per vertice che il materiale legge interpolati, punto per punto.

---

## 17.4 Le finestre le disegna il materiale

Un condominio di 6 piani con 10 finestre per piano su ogni lato ne ha 240.
Fatte di triangoli, sarebbero un paio di migliaia di triangoli per un
edificio che altrimenti ne ha 24. Moltiplicato per una città, troppo.

Il materiale invece le **calcola**: con UV0 sa dov'è il punto sulla facciata
in metri. Divide la facciata in **campate** (3,2–4 m) e in **piani**; dentro
ogni casella, se il punto è nel rettangolo centrale, è vetro (scuro e
lucido), altrimenti intonaco. Una riga più scura alla base di ogni piano fa
il marcapiano.

```
   +------+------+------+       una casella = una campata x un piano
   |  ##  |  ##  |  ##  |       ## = vetro (44% x 48% della casella)
   +------+------+------+  <--  marcapiano
   |  ##  |  ##  |  ##  |
   +------+------+------+  <--  suolo: sotto, niente finestre
```

Da lontano le finestre diventerebbero più piccole di un pixel e farebbero
solo scintillare: nella banda lontana UV1.x è 0 e il materiale le spegne.

È lo stesso testo compilato due volte, come per le nuvole e gli alberi: in
Unreal è il nodo Custom, nei test è C++ con lo shim HLSL. Così si può
controllare che al centro della campata ci sia vetro e fra due finestre
intonaco senza aprire l'editor.

---

## 17.5 Le celle e i componenti

Le celle sono le tile del dataset, e la logica è quella degli alberi
(`Mesh/GroundCells.h`, ora condivisa): bande per distanza con l'isteresi, il
terreno **disegnato** sotto, la **firma** che dice quando rifare una cella.

Cambia il modo di mettere a schermo. Gli alberi sono poche forme ripetute
centomila volte: istanze. Gli edifici sono tutti diversi: una cella è **una
mesh sola**, diversa da tutte le altre. Per questo:

1. sul **worker** si fanno muri e tetti, e la mesh finale per Unreal (una
   `FDynamicMesh3`, nota Unreal più sotto);
2. sul **game thread**, due celle per frame, un `UDynamicMeshComponent`
   riceve quella mesh **spostandola**, senza copiarla, e prende il posto della
   cella vecchia.

Le mesh sono in metri nel frame della loro cella; la scala in centimetri è
nella trasformazione del componente. Al **rebase** si sposta solo il frame.

---

## 17.6 I monumenti

Un estruso va bene per un condominio, non per il Duomo. Per i pochi edifici
che contano c'è una **libreria**: Project Settings → Plugins → GeoWorld -
Edifici. Ogni voce dice "al posto di questo edificio, questo modello".

Come si dice "questo edificio" senza coordinate? Con un **id**:

- **Wikidata** (`Q…`): l'archivio di Wikipedia per i fatti; ogni monumento
  ha il suo numero, e su OSM l'edificio lo porta nel tag `wikidata=*`;
- l'**id OSM** (`way/123456`, `relation/987`): si legge su openstreetmap.org
  cliccando l'edificio.

Il worker, quando trova un edificio della libreria, non lo estrude: dice
dove va il modello (il baricentro della sagoma, la quota della base, la
direzione del lato lungo). Sul game thread un `UStaticMeshComponent` lo
mette lì. Il modello è un asset normale, e può essere Nanite: qui Nanite
funziona perché il modello è preparato nell'editor.

> ⚠️ **Trappola.** Le chiese su OSM sono spesso fatte a **parti**, e il tag
> `wikidata` sta sul **contorno**, che non si disegna. Se la pipeline lo
> buttasse, la libreria non troverebbe mai la chiesa; se si disegnasse il
> modello lasciando le parti, navata e campanile estrusi spunterebbero
> attraverso il modello. Per questo il contorno resta "solo contorno", con
> le sue parti nella stessa tile: se diventa un monumento, le parti che gli
> cadono dentro spariscono.

---

## Alternative considerate

| Idea | Perché no (per ora) |
|---|---|
| Static mesh Nanite fatte a runtime | i dati Nanite si costruiscono solo nell'editor |
| Le mesh già pronte nella pipeline | la base dipende dal terreno disegnato, che cambia con il LOD |
| Un componente per edificio | decine di migliaia di componenti: il game thread non regge |
| Finestre fatte di triangoli | cento volte i triangoli, e scintillano da lontano |
| Il PCG di Unreal | la logica starebbe nei grafi dell'editor, non provabile fuori |
| Facciate a pezzi (un "kit" modulare) | serve una libreria di pezzi: un passo futuro, sopra questo |

---

## Nota Unreal

### UDynamicMeshComponent e FDynamicMesh3

`FDynamicMesh3` è la mesh "da lavoro" di Unreal (modulo GeometryCore): vertici
e triangoli in array, più degli **overlay** per gli attributi (normali, UV,
colori). Un overlay ha i suoi **elementi**, e ogni triangolo dice quali tre
elementi usa: due triangoli che condividono un vertice possono avere normali
diverse (uno spigolo vivo). Qui ogni vertice ha i suoi attributi, quindi gli
elementi hanno gli stessi indici dei vertici.

`UDynamicMeshComponent` (modulo GeometryFramework) disegna una
`FDynamicMesh3`. Ha due vantaggi su una static mesh fatta a runtime: la mesh
si prepara tutta su un altro thread, e la si consegna con
`SetMesh(MoveTemp(Mesh))`, uno spostamento.

> ⚠️ **Trappola.** `FDynamicMesh3::AppendTriangle` rifiuta un triangolo che
> farebbe un lato condiviso da **tre** triangoli (un lato "non-manifold"), e
> torna `NonManifoldID`. Con le sagome vere succede: un cortile che tocca il
> contorno, due edifici sovrapposti. Il triangolo si aggiunge allora con
> vertici suoi: a schermo è identico.

### Il colore dei vertici

L'overlay dei colori ha valori float; il componente li porta alla GPU come
byte senza conversioni di spazio colore (il suo default). Il materiale
riceve il byte / 255 e lo tratta come sRGB, portandolo in lineare con un
quadrato. È il contrario della static mesh degli alberi, dove la costruzione
codifica il colore in sRGB (capitolo 16).

### Una lista di struct in Project Settings

`UGeoBuildingSettings` è un `UDeveloperSettings` (come la libreria degli
alberi) con una `TArray<FGeoLandmark>`: una lista di `USTRUCT`. L'editor la
mostra con i bottoni + e −, e ogni voce con i suoi campi. Un
`TSoftObjectPtr<UStaticMesh>` è un riferimento "morbido": il percorso
dell'asset, che si carica solo quando serve (`LoadSynchronous`).

### Due uscite in un float4: l'AppendVector

Il nodo VertexColor ha un'uscita RGB e una A separate. Il Custom vuole un
`float4`: un **AppendVector** le unisce (float3 + float1). Le uscite si
cercano per **maschera** dei canali, non per nome (capitolo 16).

---

## Dove sta nel codice

| File | |
|---|---|
| `Pipeline/geoworld/bldclasses.py`, `bldformat.py`, `bldbuild.py` | la pipeline: classi e altezze, formato `.gbd`, `build-buildings` |
| `Pipeline/costruisci-edifici.bat` | il doppio clic: terreno, estratto OSM (lo scarica se manca), build, verifica |
| `GeoTiles/Public/Tiles/BuildingClasses.h`, `BuildingTileFormat.h` | classi, tetti, flag e lettura delle tile (C++ puro) |
| `GeoTiles/Public/Streaming/GeoBuildingDataset.h`, `GeoBuildingStreamingSubsystem.h` | il dataset e il caricamento |
| `GeoRender/Public/Mesh/GroundCells.h` | celle, bande, isteresi, firma, terreno disegnato (con gli alberi) |
| `GeoRender/Public/Buildings/BuildingMesh.h` | pulizia, triangolazione, muri, tetti, monumenti |
| `GeoRender/Public/Buildings/BuildingShader.inl` | il materiale, HLSL e C++ |
| `GeoRender/Public/Buildings/GeoBuildingsSubsystem.h` | Unreal: celle, worker, componenti, monumenti, rebase, overlay |
| `GeoRender/Public/Buildings/GeoBuildingSettings.h` | la libreria dei monumenti, in Project Settings |
| `GeoWorldEditor/Private/GeoTerrainMaterialFactory.cpp` | `M_GeoBuilding` |
| `Tools/StandaloneTests/geobuildings_main.cpp` | i test C++ |

---

## Riepilogo

- Le **sagome** vengono da OpenStreetMap, con altezze, piani, tetti e colori
  dai tag; quando mancano, dalla **classe** dell'edificio.
- Le **parti** (`building:part`) si disegnano al posto del loro contorno; il
  contorno resta, "solo contorno", per i monumenti.
- Le tile `.gbd` tengono ogni edificio intero nella tile del suo centro, con
  i vertici in int16 (~4 cm).
- Dalla sagoma al solido: **ear clipping** con i **ponti** per i cortili, muri
  verso fuori dalla quota più bassa, tetti a falde solo su sagome semplici.
- Le **finestre** le disegna il materiale, campata per campata e piano per
  piano; da lontano si spengono.
- In Unreal: una `FDynamicMesh3` per cella preparata sul worker, un
  `UDynamicMeshComponent` che la riceve con uno spostamento.
- I **monumenti**: una libreria in Project Settings, per id Wikidata o OSM;
  il modello (anche Nanite) prende il posto della sagoma, e delle sue parti.

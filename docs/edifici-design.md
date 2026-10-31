# Gli edifici — le città da OpenStreetMap

> Come provarli: `docs/edifici-verifica.md`. Il racconto per chi impara:
> `docs/manuale/17-edifici.md`. Dopo gli edifici viene il ciclo giorno e
> notte, per vedere le loro ombre.

## 1. L'idea in breve

Ogni edificio di OpenStreetMap diventa un **solido estruso**: la sua sagoma
(il contorno visto dall'alto) alzata fino alla sua altezza, con un tetto
sopra. Si costruiscono **a runtime**, attorno alla camera, come mesh normali.
Le finestre non sono triangoli: le **disegna il materiale**.

I pochi edifici che meritano di più (un duomo, una torre famosa) si
sostituiscono con un **modello** (di solito Nanite) preso da una **libreria**
in Project Settings: la sagoma estrusa sparisce e al suo posto compare il
modello, sul baricentro e girato come il lato lungo.

```
 estratto OSM (.osm.pbf) --geoworld build-buildings-->  tile .gbd (livello 15)
                                                              |
                       UGeoBuildingStreamingSubsystem  <------+
                                    |
   terreno DISEGNATO  -->  UGeoBuildingsSubsystem: celle (= tile) attorno alla camera
   (tile visibili)            |  worker: sagome -> muri, tetti (BuildingMesh.h) -> FDynamicMesh3
                              |  game thread: un UDynamicMeshComponent per cella
                              |               + i monumenti della libreria (UStaticMeshComponent)
                              v
                     M_GeoBuilding: colore, finestre, marcapiano, neve, bagnato
```

| | |
|---|---|
| Dati | OpenStreetMap: `building=*` (way chiuse e multipoligoni), `building:part=*`; ODbL |
| Tile | `.gbd`, livello 15 (~610 x 430 m a 45°), zlib; solo dove c'è almeno un edificio |
| Vertici | int16, frazioni della tile x 16384: un passo di ~4 cm |
| Altezze | `height`, `building:levels`, `min_height`, `roof:*` dai tag; altrimenti dalla classe (3 m a piano + 0,5 m) |
| Classi | 12: generico, casa, condominio, commerciale, industriale, religioso, pubblico, agricolo, box, pensilina, torre, storico |
| Tetti | piano, due falde, padiglione, piramide, cupola (falde solo su sagome semplici) |
| Celle | le tile del dataset, entro 6 km dalla camera |
| Bande | vicino 1.500 m (tutto, finestre, ombre), medio 3.500 m (via i box sotto 40 m², contorni semplificati 0,5 m), lontano 6.000 m (solo sopra 150 m², 2 m, senza finestre) |
| Costo | 1.000 edifici in ~3,5 ms su un worker, ≤ 24 triangoli l'uno (13.600 in tutto) |
| Monumenti | Project Settings → Plugins → **GeoWorld - Edifici**: id Wikidata o OSM → static mesh |

## 2. I dati: perché OpenStreetMap

| Sorgente | Pro | Contro |
|---|---|---|
| **OpenStreetMap** | lo stesso estratto di strade e acqua; in Italia quasi completo (molte regioni hanno importato la loro cartografia); tag per altezza, piani, tetto, colore; ODbL | le altezze ci sono per una parte degli edifici: il resto si stima |
| Cartografie regionali (DBTRE, CTR) | sagome precise, spesso con la quota di gronda | un formato e una licenza per regione; da scaricare a mano |
| Microsoft / Google Open Buildings | tutto il mondo, dall'IA sulle foto satellitari | niente Italia (Microsoft) o solo Africa/Asia/America Latina (Google); nessuna altezza |
| Modelli 3D delle città (CityGML) | edifici veri, con i tetti | pochissime città, formati pesanti |
| Fotogrammetria (Google 3D Tiles) | realistico | licenza: non si può usare offline né ridistribuire |

OSM ha tre vantaggi in più: è **già nella pipeline** (fetch-osm, il lettore
GDAL), ha lo schema **Simple 3D Buildings** (le parti di un edificio con
altezze diverse: il campanile sopra la chiesa) e i tag `wikidata=*`, che
permettono di riconoscere un monumento senza coordinate.

### 2.1 Cosa si legge

- way chiuse e multipoligoni con `building=*` (il driver OSM di GDAL li
  mette nel layer `multipolygons`, con i **cortili** come buchi);
- le **parti** (`building:part=*`). Le way chiuse con solo `building:part`
  il driver le mette fra le `lines`: la pipeline le richiude in poligoni.
  Un edificio che ha delle parti **non si disegna**: si disegnano le parti,
  come vuole lo schema. Si riconosce perché il centro di una parte gli cade
  dentro. Resta però nella tile, marcato "solo contorno", per i monumenti
  (sezione 7.1);
- `building=no` e `building:part=no` si saltano; `amenity=place_of_worship`
  e `man_made=tower` aiutano a classificare un `building=yes`.

### 2.2 Altezze e piani (bldclasses.py)

In ordine di fiducia:

1. `height` (o `building:height`), con le unità (`12 m`, `40'`): è la
   **cima**, tetto compreso;
2. `building:levels` (+ `roof:levels`): 3 m a piano, più 0,5 m per il piano
   terra;
3. la classe: una casa 2 piani, un condominio 5, un box 1, una torre 25 m.

`min_height` / `building:min_level` alzano la base (un corpo sospeso su un
portico). Una pensilina (`building=roof`) è un tetto spesso 60 cm in cima a
pilastri invisibili. Il manifest dice per quanti edifici l'altezza viene dai
tag ("misurata").

### 2.3 Colori

`building:colour` / `colour` e `roof:colour`, nomi CSS o `#rrggbb`. Senza,
una **tavolozza per classe** (intonaci italiani: ocra, crema, salmone, grigio;
terracotta per i tetti a falde, grigio per quelli piani), scelta con un
caso che dipende dall'id OSM: lo stesso edificio ha sempre lo stesso colore.

## 3. Il formato `.gbd`

Una tile per file, `<livello>/<x>/<y>.gbd`, solo se ha almeno un edificio.

```
header 32 byte  "GWBD", versione 1, riservato, livello, x, y,
                quanti edifici, byte del payload, byte decompressi
payload zlib    per ogni edificio un record di 30 byte:
                  classe, tetto, flag (colore dai tag, parte, solo
                  contorno, altezza o piani misurati), piani,
                  altezza, min_height, altezza del tetto (dm, u16),
                  colore muri RGB, colore tetto RGB,
                  id OSM (u64: il tipo nei 2 bit alti, 1 way / 2 relazione),
                  Wikidata (u32, il numero dopo la Q), quanti anelli
                e i suoi anelli: quanti vertici (u16), poi x, y in int16
                (frazioni della tile x 16384 dall'angolo sud-ovest).
                Il primo anello è il contorno (antiorario), gli altri
                i cortili (orari).
```

- **Perché i byte decompressi nell'header.** `FCompression::UncompressMemory`
  di Unreal vuole la dimensione **esatta** del risultato: senza, bisognerebbe
  indovinarla e riprovare.
- **Perché int16.** Una tile è ~600 m: 16384 passi sono ~4 cm, più fini dei
  rilievi catastali. Un edificio di 8 vertici pesa 30 + 2 + 32 = 64 byte
  prima di zlib.
- **Chi sta in una tile.** Ogni edificio sta nella tile del suo **centro**:
  i vertici possono uscire (fino a mezza tile: gli int16 lo permettono), e
  una cella costruisce tutti i suoi edifici interi, senza tagliarli.
- **L'indice** (`<livello>/index.bin`, "GWBI"): le tile che esistono, con
  quanti edifici. Una cella senza tile non costa niente.

## 4. Dalle sagome ai triangoli (BuildingMesh.h, C++ puro)

Per ogni edificio, nel frame locale della cella (metri: X nord, Y est, Z alto):

1. **La pulizia.** Via i vertici doppi e i lati sotto i 30 cm; da lontano,
   Douglas–Peucker (0,5 m in banda media, 2 m in quella lontana).
2. **La base.** La quota **più bassa** del terreno disegnato sotto il
   contorno. I muri partono 1,5 m più giù: su un pendio il lato a monte
   resta sepolto, quello a valle non galleggia.
3. **I muri.** Un rettangolo per lato, del contorno e dei cortili, girato
   verso fuori (verso il cortile per i cortili).
4. **Il tetto.** La forma dai tag, oppure dalla classe (case a padiglione
   o a due falde, chiese e fienili a due falde, condomini e palazzi storici a
   padiglione, torri a piramide, il resto piano). Le falde si fanno **solo
   dove la geometria è semplice**:

   | Forma | Su che sagome | Com'è fatta |
   |---|---|---|
   | due falde | 4 lati, convessa, senza cortili | colmo lungo il lato lungo, timpani color muro |
   | padiglione | idem | colmo accorciato, quattro falde |
   | piramide | convessa, senza cortili | punta sul baricentro (torri e campanili ripidi) |
   | cupola | convessa, senza cortili | 5 anelli che si stringono come una sfera |
   | piano | tutte | il contorno triangolato, cortili compresi |

   Su una L, una T o un palazzo a corte il tetto è **piano**: per le falde
   servirebbe lo *straight skeleton* del poligono. È il limite più visibile
   (sezione 9).
5. **L'altezza del tetto** se i tag non la dicono: pendenza 0,45 (0,3 per i
   condomini, 0,9 per chiese e torri) sulla metà del lato corto, al massimo
   8 m (30 per chiese e torri). Se l'altezza è misurata il tetto sta
   **dentro**: i muri si abbassano.

La **triangolazione** è un ear clipping con i cortili collegati al contorno
da un ponte (il metodo di Eberly): regge le L, le stelle, i palazzi a corte
con più cortili. Tutte le facce girano come quelle del terreno: la normale
`(B − A) × (C − A)` punta fuori, e i test lo controllano su mille edifici.

### 4.1 Cosa porta ogni vertice al materiale

| Canale | Cosa |
|---|---|
| colore RGB | il colore dell'edificio, in sRGB (byte) |
| colore A | 1 muro, 0 tetto |
| UV0 | la facciata: metri lungo il muro, metri sopra la base |
| UV1 | lo stile: altezza di un piano (0 = niente finestre), un caso 0..1 |

## 5. Il materiale (BuildingShader.inl)

Un nodo Custom, **lo stesso testo** compilato come C++ dai test (come le
nuvole e gli alberi).

- **Le finestre.** La facciata si divide in **campate** (3,2–4 m, secondo il
  caso dell'edificio) e in piani (UV1.x). In ogni casella una finestra larga
  il 44% e alta il 48%, vetro scuro e lucido (ruvidità 0,15). Sotto il suolo
  (il primo "piano" negativo) niente. Una riga più scura alla base di ogni
  piano: il marcapiano.
- **La neve** sulle superfici che guardano il cielo, sopra la quota della
  copertura del suolo (MPC_GeoWeather, come terreno e alberi).
- **Il bagnato** scurisce i tetti più dei muri: la pioggia batte dall'alto.
- **Il colore** arriva in sRGB: al quadrato è (quasi) lineare.
- **Di notte le finestre si accendono** (un secondo blocco dello stesso
  shader, `GEO_BUILDING_LIGHTS`, nell'Emissive): quante dipende dall'ora e dal
  buio, vedi `docs/cielo-design.md`, sezione 5.4.

Perché le finestre nel materiale: un condominio di 6 piani con 10 campate
per lato avrebbe 240 finestre. Come triangoli sarebbero ~2.000 triangoli
invece di 24; nel materiale costano qualche istruzione per pixel, e da
lontano (banda lontana) si spengono perché farebbero solo scintillare.

## 6. Le celle (GroundCells.h, condiviso con gli alberi)

Le celle **sono le tile del dataset** (livello 15): ogni edificio sta in una
cella sola e la cella li costruisce tutti. Ogni quarto di secondo:

1. le celle entro il raggio, dalla più vicina, con la loro banda
   (distanza orizzontale e quota della camera dal suolo, con 100 m di
   **isteresi** per non far ballare una cella sul confine);
2. per ogni cella che **esiste** nell'indice: la tile (streaming), le tile di
   terreno **visibili** che la toccano (con un margine del 25%: gli edifici
   sporgono), e la loro **firma**;
3. una cella si rifà quando cambia la firma del terreno sotto (un altro
   livello di dettaglio) o la banda;
4. il lavoro va su un **worker**: muri e tetti (`BuildBuildingCell`), poi la
   `FDynamicMesh3` pronta per il componente;
5. sul game thread, **due celle per frame**: il componente si crea, riceve
   la mesh con uno spostamento (`SetMesh(MoveTemp(...))`), si mette al posto
   della cella vecchia. La cella vecchia resta a schermo fino ad allora.

Le mesh stanno nel frame della loro cella, in metri, con la scala in
centimetri nella trasformazione: al **rebase** si sposta solo il frame.

### 6.1 Perché UDynamicMeshComponent e non UStaticMesh

| | UStaticMesh a runtime | UDynamicMeshComponent |
|---|---|---|
| Costruzione | `BuildFromMeshDescriptions` sul game thread: decine di ms per una cella di città | la `FDynamicMesh3` si prepara tutta sul worker |
| Consegna | — | uno spostamento, poi il proxy di render |
| Nanite | no (a runtime) | no |
| Usato per | le 8 forme degli alberi (piccole, una volta sola) | terreno, strade, acqua, edifici |

Una cella di città ha 10.000–20.000 triangoli ed è diversa per ogni cella:
la strada del terreno è quella giusta. Nanite resta per i **monumenti**,
che sono asset preparati nell'editor.

### 6.2 Ombre

`geo.Buildings.Shadows`: 0 nessuno, 1 le celle vicine (default), 2 tutte.
Senza il ciclo giorno e notte il sole è fermo: il prossimo passo lo muove.

## 7. I monumenti (la libreria)

Project Settings → Plugins → **GeoWorld - Edifici** (`UGeoBuildingSettings`,
salvato in `Config/DefaultGame.ini`). Ogni voce:

| Campo | |
|---|---|
| Id | `Q12345` (il tag `wikidata=*` dell'edificio, o la pagina su wikidata.org), oppure `way/123456`, `relation/987` (l'id OSM: si legge su openstreetmap.org cliccando l'edificio); anche `w123`, `r123` |
| Model | la static mesh (Nanite, fatta o comprata a parte) |
| YawDegrees | rotazione in più rispetto al lato lungo della sagoma |
| HeightOffsetM | quota in più: per un modello con la base sopra o sotto il suo zero |
| bFitToFootprint | adatta il modello alla sagoma (lato lungo su X, lato corto su Y) invece della sua grandezza |

Il worker, quando incontra un edificio con quell'id (OSM o Wikidata), non lo
estrude: restituisce **dove** va il modello (baricentro, quota della base,
direzione del lato lungo, lato lungo e corto). Sul game thread un
`UStaticMeshComponent` per monumento, con la trasformazione locale
moltiplicata per quella della cella (anche al rebase).

### 7.1 I monumenti fatti a parti

Su OSM le chiese e i palazzi importanti sono spesso disegnati **a parti**
(`building:part`: la navata, il campanile, la cupola), e il tag `wikidata`
sta sul **contorno**, che però non si disegna. Per questo la pipeline non
butta il contorno: lo tiene nella tile marcato **`FLAG_OUTLINE`** ("solo
contorno") e mette **le sue parti nella sua stessa tile**. A runtime:

| La voce della libreria punta a | Risultato |
|---|---|
| nessuna voce | si disegnano le parti; il contorno no |
| il contorno (`Q…` o `way/…`) | al suo posto il modello, e **tutte le parti che ci cadono dentro spariscono** |
| una parte sola (`way/…` del campanile) | il modello al posto di quella parte, le altre restano |

Il caso che non regge: una parte il cui contorno sta in un altro **blocco**
della pipeline (20 km), cioè quasi mai.

## 8. Cosa costa

Misurati nei test standalone (Linux, un core):

| | |
|---|---|
| Worker | ~3,5 ms per una tile di 1.000 edifici |
| Triangoli | ≤ 24 per edificio: 13.600 per 1.000 edifici vicini, 9.000 per la stessa tile da lontano (661 edifici) |
| Game thread | due celle per frame, un componente per cella (più uno per monumento) |

Quanti edifici ci sono davvero in una tile di città, e quanto pesa, lo dirà
il primo dataset vero: `geoworld inspect-buildings` per una tile,
`geo.Buildings.Stats` per le celle a schermo. La regola: a parità di area,
una città costa meno di un bosco (un albero vicino ha da ~300 triangoli, forma del codice, a ~3.000, modello Nanite).

## 9. Limiti conosciuti

- **Tetti a falde solo su sagome semplici** (4 lati o convesse): su L, T e
  corti il tetto è piano. Servirebbe lo straight skeleton.
- **Le finestre sono uguali per tutti**: niente balconi, portoni, vetrine.
  Il materiale le mette anche sui capannoni industriali alti (dove non ci
  sono), mentre le salta per box, pensiline e capannoni bassi.
- **I colori stimati** sono plausibili, non veri: dove serve si mette il tag
  `building:colour` su OSM (e lo vedono tutti).
- **Le altezze stimate** sbagliano i palazzi con i piani alti (un palazzo
  storico con 4 piani da 5 m risulta 12,5 m invece di 20).
- **Nessuna collisione**: si vola attraverso gli edifici.
- **Monumenti e blocchi**: una parte che sta in un altro blocco della
  pipeline rispetto al suo contorno non sparisce sotto il modello (7.1).

## 10. Alternative considerate

| Idea | Perché no (per ora) |
|---|---|
| Static mesh Nanite per cella, fatte a runtime | i dati Nanite si costruiscono solo nell'editor |
| Precalcolare le mesh nella pipeline | dipendono dal terreno disegnato (la base), che cambia con il LOD |
| Una mesh per edificio | decine di migliaia di componenti: il game thread non regge |
| Finestre come triangoli | 100 volte i triangoli, e scintillano da lontano |
| Procedural Content Generation (PCG) di Unreal | ottimo per le regole artistiche, ma la logica sarebbe nei grafi e non testabile fuori dall'editor |
| Instanced "kit" di facciate | bellissimo, ma serve una libreria di pezzi: un passo futuro, sopra questo |

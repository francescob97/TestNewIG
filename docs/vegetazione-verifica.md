# La vegetazione — come provarla

> Le decisioni sono in `docs/vegetazione-design.md`; il racconto per chi
> impara in `docs/manuale/16-vegetazione.md`.

## 0. Cosa è già provato qui, senza Unreal

**Pipeline** (`Pipeline/tests/test_vegetation.py`, 20 test):

- i numeri delle classi in Python e nell'header C++ sono gli stessi; le
  costanti del formato e i flag anche; le classi su cui "cresce qualcosa"
  sono quelle che `PlantsFor` (C++) fa crescere;
- il formato: andata e ritorno, un bosco pesa pochi byte, file troncati o
  sbagliati rifiutati, indice ordinato e controllato;
- i nomi dei file WorldCover per un'area (anche a cavallo di 9° E e 45° N,
  e nell'emisfero sud);
- il taglio vero su un WorldCover **finto** (bosco a ovest, prato a est, un
  lago a nord-est): 102 tile su 108, le tutte-lago non scritte; le classi al
  posto giusto (riga 0 = nord, colonna 0 = ovest); manifest e copertura;
  rilanciato riparte senza rifare niente; `verify` trova un file rotto.

E due prove vere sulla rete: Torino centro, 16 tile in 2 secondi, 85%
costruito e 13% alberi; tutta Torino con la collina (sezione 2), 96 tile in 6
secondi, 33% bosco.

**C++** (`geovegetation_tests`, 33 test):

1. il formato `.gvg`, anche un file scritto dalla pipeline vera;
2. la griglia: due celle vicine hanno insieme esattamente i punti del
   rettangolo intero (8.748 contro 8.737 attesi), i due alberi più vicini a 2,4 m;
3. chi cresce dove: bosco 85% dei punti; conifere al 10% a 400 m e al 95% a
   1.800 m; sopra 2.300 m solo arbusti, a 2.900 niente; pini domestici a Roma
   (15%) e non a Torino; oltre 40° solo arbusti;
4. grandezze (15,6 m a 1.800 m, 10,7 m vicino al limite), bande (50% e 25%,
   sempre gli stessi alberi), il frame della cella;
5. le forme: nel cubo unitario, facce verso fuori, triangoli per dettaglio;
6. il materiale compilato come C++: verde, tronco, neve sulle chiome che
   guardano il cielo, vento (16 cm in cima a 5 m/s, 33–67 cm a 10 m/s);
7. le celle: 94 entro 2,5 km, meno da 1.500 m di quota, nessuna da 3.000;
   bande e isteresi (zero cambi con la camera che oscilla di 40 m sul
   confine); la firma; la quota e la pendenza del terreno disegnato (26,56°
   su una rampa di 26,57°), la tile più fine vince; una cella di bosco: 3.799
   alberi su 3.831 attesi, tutti appoggiati al suolo entro 10 cm.

Quello che **non** è provato: tutto quello che passa per Unreal (la mesh a
runtime, gli HISM, il materiale vero, il vento letto dalla collezione). È il
lavoro di questa pagina.

## 1. Compilare, rifare i materiali, cuocere i modelli

1. Compila il progetto. I punti più a rischio sono in fondo (sezione 6).
2. Apri l'editor e lancia **`geo.Imagery.CreateMaterial`**. Crea
   `M_GeoVegetation` e aggiunge `VegetationWind` a `MPC_GeoWeather`. Atteso,
   fra i messaggi: "MPC_GeoWeather: aggiunti 1 parametri" e "Materiali pronti".
3. Lancia **`geo.Vegetation.CreateModels`**. Salva in `/GeoWorld/Vegetation`
   gli otto modelli Nanite (`SM_GeoTree_Broadleaf_0` ...). Atteso: "Modelli
   Nanite: 8 su 8 in /GeoWorld/Vegetation (triangoli: latifoglie 3220,
   conifere 500, pini domestici 1940, arbusti 1600)". Ci mette qualche
   secondo: costruisce i dati Nanite.
4. Apri **Project Settings → Plugins → GeoWorld - Vegetazione**: le otto
   caselle puntano a quei modelli. Aprine uno nel Content Browser: nello
   Static Mesh Editor, "Nanite Settings → Enable Nanite Support" acceso, e i
   triangoli come sopra.

Il passo 3 si può saltare: senza modelli il runtime costruisce le forme da
codice (non Nanite), e gli alberi ci sono lo stesso.

## 2. Il dataset

Con la pipeline (`Pipeline\geoworld.bat` su Windows):

```bat
geoworld build-vegetation --terrain dataset/terreno -o dataset/vegetazione
geoworld verify-vegetation -o dataset/vegetazione
```

Atteso, per un terreno su Torino:

```
[1/4] area: ovest 7.5500, sud 45.0000, est 7.8000, nord 45.1500
[2/4] ESA WorldCover 10 m dal bucket pubblico (lettura a pezzi)
      N42E006: ok
      N45E006: ok
[3/4] 4 blocchi da ~39 km, 96 tile del livello 13
      blocco 1/4: 63 tile scritte, 0 senza piante (2s)
      ...
      blocco 4/4: 96 tile scritte, 0 senza piante (6s)
[4/4] indice e manifest
      96 tile; bosco 32.6%, arbusti 0.0%, prato 15.2%
```

(Provato davvero, su questo bbox. Il file N42E006 serve alla riga di tile
appena sotto il 45° parallelo: le tile del bordo sporgono dal bbox.)

- **"nessun file WorldCover raggiungibile"**: la rete non arriva al bucket di
  Amazon (proxy aziendale?). Scarica a mano il file (`ESA_WorldCover_10m_2021_v200_N45E006_Map.tif`,
  da https://esa-worldcover.org o dal bucket) e passa la cartella con
  `--worldcover`.
- Un'area grande (tutta una regione) richiede minuti: i blocchi fatti si
  segnano, e se lo interrompi riparte da dove era.

Per guardare una tile: `geoworld inspect-vegetation dataset/vegetazione/13/<x>/<y>.gvg`.

## 3. La prima occhiata

```
geo.Vegetation.Demo <terreno> <ortofoto> <vegetazione>
```

(con l'acqua: un quarto argomento). Apre tutto e si mette 250 m sopra il
centro, con l'overlay acceso. Atteso:

- in alto a sinistra le righe "GeoWorld | vegetazione": celle per banda, alberi
  per specie, "in attesa" che scende a zero in qualche secondo;
- sul terreno i **contorni delle celle**: verdi le vicine, gialle le medie,
  arancio le lontane;
- **alberi sui boschi della foto**, radi sui prati, qualche albero in città;
- nelle stampe a destra la riga "Vegetazione: N alberi in M celle".

## 4. Le prove, una per una

| # | Prova | Atteso | Se no |
|---|---|---|---|
| a | Scendi a 20 m da un bosco e guarda i piedi degli alberi | i tronchi toccano il suolo | sezione 5, "galleggiano" |
| b | Gira attorno a un albero | la chioma piena da ogni lato | se vedi l'interno, o la chioma sparisce dal lato verso di te: `geo.Vegetation.FlipWinding 1` |
| c | I colori | chiome verdi diverse da albero ad albero, tronchi marroni, più scuro sotto | grigio uniforme: manca `M_GeoVegetation` (riga rossa); grigio a scacchi: il materiale non ha "Used with Instanced Static Meshes" |
| d | Allontanati lentamente da un albero | oltre ~450 m diventa una bolla (o un cono), con una sfumatura | se resta dettagliato fino a 2,5 km, le soglie dei LOD non sono state prese: `stat unit` dirà molti triangoli (sezione 6) |
| e | Vola via dal bosco | le celle cambiano colore; da gialle ci sono metà alberi, da arancio un quarto; nessuno cambia posto | |
| f | Fermati con una cella sul confine fra due bande e muoviti di poco | gli alberi non compaiono e spariscono | isteresi rotta: segnalalo |
| g | `geo.Weather.Wind 15 270` | le cime si piegano verso est e oscillano, ognuna a modo suo; la base ferma | fermi: riga rossa "manca VegetationWind" → `geo.Imagery.CreateMaterial` |
| h | `geo.Weather.SnowCover 0` | le chiome imbiancate sopra, i fianchi e i tronchi scuri | |
| i | Sali a 3.000 m sopra il suolo | gli alberi spariscono (celle a zero): la foto basta | |
| j | Vola dritto per 30 km (rebase) | nessun salto degli alberi | se saltano al rebase: segnalalo |
| k | In montagna, verso i 2.000 m | conifere, poi solo arbusti, poi niente sopra i 2.700 m | se ci sono latifoglie in alto: manca il geoide (apri anche l'acqua) o la quota è sbagliata |
| l | `geo.Vegetation.Density 0.3`, poi `1` | le celle si rifanno (le vecchie restano finché le nuove sono pronte) | |
| m | `geo.Vegetation.Stats` | "Modelli: 8 modelli dalla libreria (8 Nanite), 0 forme fatte dal codice" | "forme fatte dal codice": i modelli non ci sono (passo 1.3) o il percorso nelle caselle è sbagliato; "(0 Nanite)": il modello non ha i dati Nanite (Enable Nanite Support) |
| n | Nel viewport: View Mode → Nanite Visualization → Triangles | gli alberi vicini colorati a triangoli fitti, quelli lontani a pochi grandi triangoli | se gli alberi non si colorano, non sono Nanite (vedi m) |
| o | `geo.Vegetation.Shadows 2`, con le Virtual Shadow Maps accese (`r.Shadow.Virtual.Enable 1`) | tutti gli alberi fanno ombra; `stat unit` non deve crescere molto | se la GPU sale tanto: `geo.Vegetation.Shadows 1` |
| p | Un modello tuo: mettilo in una casella della libreria, poi `geo.Vegetation.ReloadModels` | gli alberi di quella forma diventano il tuo modello, alti e larghi come prima, con la base sul suolo | se galleggiano o affondano: il modello non ha la base in basso (il pivot non conta, contano i bounds); se sono sdraiati, il modello non ha Z in alto |

## 5. Se qualcosa non va

**Nessun albero.**

- `geo.Vegetation.Stats`: se dice "Nessun dataset", apri il dataset; se le
  celle sono tutte "vuote", il dataset non copre questa zona (controlla l'area
  con `geo.Vegetation.Open`, che la stampa);
- celle "in attesa" che non scendono: il terreno sotto non è a schermo
  (`geo.Terrain.Stats`), o le quote non sono in memoria;
- `geo.Vegetation.Enable 1`; sei più in alto del raggio (`geo.Hud`: "dal suolo").

**Galleggiano o sono sepolti.** Gli alberi stanno sulla superficie
**disegnata**. Se il terreno ha appena cambiato livello, la cella si rifà in
un quarto di secondo più il lavoro: un attimo di sfasamento è normale. Se
restano sfasati, guarda se `geo.Terrain.MeshStep` è cambiato. Con l'acqua accesa, sulle
sponde scavate un albero può stare sopra il suolo (sezione 7 del design).

**Lento.** In un bosco continuo ci sono ~110.000 alberi e ~5 milioni di
triangoli. In ordine:

```
geo.Vegetation.Density 0.5
geo.Vegetation.Radius 1500
geo.Vegetation.Shadows 0
```

`stat unit` prima e dopo dice quale pesa. Se il collo è il game thread (i
picchi quando si muove la camera), è la creazione dei componenti: diminuire
il raggio aiuta più della densità.

## 6. Se non compila

Le API di Unreal usate qui per la prima volta, con il ripiego:

| Dove | API | Se non c'è |
|---|---|---|
| `GeoVegetationSubsystem.cpp`, `BuildShapeMesh` | campi di `UStaticMesh::FBuildMeshDescriptionsParams` (`bMarkPackageDirty`, `bBuildSimpleCollision`, `bCommitMeshDescription`, `bFastBuild`) | togli la riga del campo che manca |
| idem | `GetRenderData()->ScreenSize[1].Default` | togli le due righe: tutti useranno la forma dettagliata (più triangoli) |
| idem, `FillShapeDescription` | `FStaticMeshAttributes`, `GetVertexInstanceColors` & co. | il modulo `StaticMeshDescription` deve essere nel Build.cs (c'è) |
| `ApplyCell` | `SetNumCustomDataFloats`, `SetCustomData(int32, TArrayView<const float>, bool)` | nelle versioni vecchie `SetCustomData` voleva un `TArray<float>`: costruiscilo dai 5 numeri |
| `ApplyCell` | `AddInstances(Transforms, false, false)` | idem con due argomenti |
| `GeoTerrainMaterialFactory.cpp` | `UMaterialExpressionPerInstanceCustomData::DataIndex` | il nodo è "PerInstanceCustomData" nell'editor: si può rifare a mano |
| idem | `Material->DitheredLODTransition` | togli la riga (niente sfumatura fra i LOD) |
| idem | `Material->bUsedWithNanite`, `Material->MaxWorldPositionOffsetDisplacement` | nell'editor sono "Used with Nanite" (Usage) e "Max World Position Offset Displacement" (Nanite): accendili a mano su `M_GeoVegetation`, 500 |
| `GeoVegetationModelFactory.cpp` | `SetNumSourceModels`, `GetSourceModel(0).BuildSettings`, `CreateMeshDescription`, `CommitMeshDescription`, `Build(true)`, `NaniteSettings.bEnabled` | la strada dell'editor per una static mesh da codice: se una manca, dimmi quale; intanto gli alberi usano le forme del codice |
| `GeoVegetationSubsystem.cpp`, `EnsureShapes` | `HasValidNaniteData()` | metti `false`: serve solo alla statistica |
| `ApplyCell` | `SetEvaluateWorldPositionOffset` | togli la riga: il vento resterà anche lontano (costa un po' di più) |
| `GeoVegetationSettings.h` | `UDeveloperSettings` (modulo `DeveloperSettings`, nel Build.cs) | se il modulo non si trova, è in "Engine" nelle versioni vecchie: togli la riga dal Build.cs |

Se il materiale non si costruisce, si può fare a mano: due nodi Custom con il
testo di `VegetationShader.inl` (un blocco ciascuno), gli ingressi elencati in
cima al file, e "Used with Instanced Static Meshes" acceso.

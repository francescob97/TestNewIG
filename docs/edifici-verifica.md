# Gli edifici — come provarli

> Le decisioni sono in `docs/edifici-design.md`; il racconto per chi impara
> in `docs/manuale/17-edifici.md`.

## 0. Cosa è già provato qui, senza Unreal

**Pipeline** (`Pipeline/tests/test_buildings.py`, 17 test):

- classi, forme dei tetti, flag e costanti del formato uguali in Python e
  negli header C++; la tile di prova (`buildings_fixture.gbd`) è aggiornata
  ed è su Torino;
- la classificazione (`building=yes` + `amenity=place_of_worship` → chiesa),
  le altezze dai tag con le unità, i piani, i colori (nomi CSS e `#rrggbb`);
- il formato: andata e ritorno, la quantizzazione, file troncati o
  sbagliati rifiutati, l'indice;
- il build vero su un OSM **finto** (letto dal driver OSM di GDAL): una casa,
  un condominio a L, una chiesa con navata e campanile (`building:part`), un
  palazzo a corte (multipoligono), una pensilina, un edificio colorato con
  `wikidata`, un `building=no` e un edificio fuori area. Risultato: 7
  edifici disegnabili, la chiesa tenuta come "solo contorno" con il suo
  wikidata e le parti nella sua tile, il cortile che gira al contrario del
  contorno, la casa 20 x 10 m al decimetro; rilanciato non riscrive niente.

**C++** (`geobuildings_tests`, 24 test):

1. il formato `.gbd`, anche la tile scritta dalla pipeline vera;
2. la triangolazione: una L, un palazzo a corte (l'area torna, il cortile
   resta vuoto), due cortili, una sagoma con 42 vertici quasi allineati
   (torna un rettangolo), una stella di 24 punte;
3. gli edifici: la casa (muri da 1,5 m sotto il suolo, tetto a due falde,
   tutte le facce verso fuori), l'altezza misurata che comprende il tetto, il
   palazzo a corte, cupola e guglia, i muri su un pendio, i colori, la
   pensilina;
4. una tile di **1.000 edifici in ~3,5 ms**, al più 24 triangoli l'uno; il
   monumento al suo posto (a terra, girato come il lato lungo); da lontano
   661 edifici invece di 999 e niente finestre; la chiesa a parti con e senza
   monumento; la tile della pipeline;
5. il materiale compilato come C++: vetro scuro e lucido al centro della
   campata, intonaco fra una finestra e l'altra, niente finestre sotto il
   suolo; neve sui tetti e non sui muri; il bagnato scurisce i tetti più dei
   muri.

Quello che **non** è provato: tutto quello che passa per Unreal (il
componente, il materiale vero, i monumenti, le ombre). È il lavoro di questa
pagina.

## 1. Compilare e rifare i materiali

1. Compila il progetto. I punti più a rischio sono in fondo (sezione 6).
2. Apri l'editor e lancia **`geo.Imagery.CreateMaterial`**. Crea, fra gli
   altri, `M_GeoBuilding`. Atteso: "Materiali pronti". Se c'è una riga rossa
   "M_GeoBuilding non fatto", guarda l'Output Log (categoria LogGeoWorld).

## 2. Il dataset

Il modo comodo: doppio clic su **`Pipeline\costruisci-edifici.bat`**.

1. chiede il dataset del terreno (il nome di una cartella in
   `Pipeline\dataset`, o la cartella trascinata dentro);
2. cerca un estratto OpenStreetMap (`.osm.pbf`) in `Pipeline\dati\osm`. Se
   non ce n'è nessuno, chiede la regione e lo scarica da Geofabrik
   (Piemonte, Lombardia, Liguria e Valle d'Aosta sono `nord-ovest`). Se ce
   n'è più d'uno, chiede quale;
3. costruisce `dataset\edifici_<terreno>`, lo verifica e mette negli appunti
   `geo.Buildings.Open "..."`.

A mano, con la pipeline (`Pipeline\geoworld.bat` su Windows):

```bat
geoworld fetch-osm --area nord-ovest -o dati/osm
geoworld build-buildings --osm dati/osm/nord-ovest-latest.osm.pbf --terrain dataset/terreno -o dataset/edifici
geoworld verify-buildings -o dataset/edifici
```

Atteso (questo è l'OSM finto dei test; su una città i numeri sono migliaia):

```
[1/4] area: ovest 7.6700, sud 45.0600, est 7.7000, nord 45.0800
[2/4] sagome da OpenStreetMap (una passata sul file: per una regione qualche minuto)
      .../prova.osm
      6 edifici e 2 parti (0s)
[3/4] 1 blocchi da ~20 km
      blocco 1/1: 1 tile, 8 edifici
      1 contorni con le parti (building:part): non si disegnano, restano per i monumenti
[4/4] indice e manifest
      1 tile, 7 edifici; altezza o piani da OSM per il 71%, il resto stimato dalla classe
1 tile al livello 15: 0 problemi
```

- La passata sul file `.osm.pbf` di una regione intera richiede qualche
  minuto (il file si legge tutto, si tiene solo l'area). Se lo interrompi,
  riparte: la passata finita e i blocchi fatti si segnano in `_work`.
- `--force` rifà tutto, anche la lettura del file.
- Per guardare una tile: `geoworld inspect-buildings dataset/edifici/15/<x>/<y>.gbd --show 20`.
  Per ogni edificio: classe, altezza, piani, tetto, vertici, cortili, id OSM,
  `Q…` se ha il wikidata, "(parte)" o "(contorno con le parti)".

## 3. La prima occhiata

```
geo.Buildings.Demo <terreno> <ortofoto|-> <edifici> [vegetazione|-]
```

Apre tutto, si mette **300 m sopra la tile con più edifici** (il centro
della città, di solito) e accende l'overlay. Atteso:

- in alto a sinistra "GeoWorld | edifici": dataset, bande, celle per banda,
  edifici, triangoli, monumenti; "in attesa" che scende a zero in qualche
  secondo;
- sul terreno i **contorni delle celle**: verdi le vicine, gialle le medie,
  arancio le lontane;
- gli **edifici sulle sagome della foto**, con i tetti a falde sulle case e
  piani sui grandi palazzi; le finestre sulle facciate;
- nelle stampe a destra la riga "Edifici: N in M celle".

## 4. Le prove, una per una

| # | Prova | Atteso | Se no |
|---|---|---|---|
| a | Scendi a 30 m da una via | gli edifici combaciano con l'ortofoto (una differenza di 1–2 m è l'ortofoto stessa) | spostati di decine di metri: segnalalo, con le coordinate (`geo.Hud`) |
| b | Guarda la base di un edificio in pendenza | il muro a valle tocca il suolo; a monte entra nel terreno | se galleggia: sezione 5 |
| c | Gira attorno a un edificio | muri pieni da tutti i lati, tetto chiuso | se vedi l'interno (muri trasparenti da fuori): segnalalo, è la direzione delle facce |
| d | Le finestre | file regolari, una per campata e per piano; nessuna sotto il suolo; i box e le pensiline senza | niente finestre e tutto grigio: manca `M_GeoBuilding` (riga rossa) |
| e | I colori | intonaci diversi da edificio a edificio, tetti a falde in coppi, piani grigi; sempre gli stessi colori tornando | tutto bianco o tutto nero: segnalalo (lo spazio colore del vertice) |
| f | Un palazzo a corte (centro storico) | il cortile vuoto, con i suoi muri verso dentro | |
| g | Una chiesa con il campanile | navata e campanile di altezze diverse (se su OSM sono disegnati a parti) | |
| h | Allontanati | da gialle (1,5 km) spariscono box e baracche; da arancio (3,5 km) restano solo i grandi, senza finestre | se le finestre scintillano da lontano: segnalalo |
| i | Fermati con una cella sul confine fra due bande e muoviti di poco | nessun edificio compare e sparisce | isteresi rotta: segnalalo |
| j | `geo.Weather.SnowCover 0` | tetti imbiancati, muri no | |
| k | `geo.Weather.Rain 15` (pioggia forte) | tetti più scuri, muri un po' | |
| l | Vola dritto per 30 km (rebase) | nessun salto degli edifici | se saltano al rebase: segnalalo |
| m | `geo.Buildings.Shadows 2`, `geo.Buildings.Shadows 0` | ombre di tutti gli edifici, poi di nessuno | |
| n | `geo.Buildings.Radius 3000` | la città finisce a 3 km, le bande si stringono | |
| o | `geo.Buildings.Stats` | edifici, triangoli, celle, tile in memoria, nessun errore | errori di caricamento: `geoworld verify-buildings` |

### 4.1 Un monumento

1. Scegli un edificio: su [openstreetmap.org](https://www.openstreetmap.org)
   cliccalo (strumento "?" o "Interroga elementi"). In alto c'è "Via 123456"
   (o "Relazione 987"): l'id è `way/123456` (o `relation/987`). Se fra i tag
   c'è `wikidata=Q…`, puoi usare quello. Oppure:
   `geoworld inspect-buildings <tile> --show 200` e cerca il `Q…`.
2. Un modello: qualunque static mesh, anche un cubo (`/Engine/BasicShapes/Cube`)
   per la prima prova. Il modello vero va preparato con Z in alto, la base
   a Z = 0, il lato lungo lungo X, in centimetri.
3. **Project Settings → Plugins → GeoWorld - Edifici → Monumenti**: aggiungi
   una voce, scrivi l'Id, scegli il Model. Per il cubo: **Fit To Footprint**
   acceso (altrimenti è un cubo di un metro).
4. `geo.Buildings.ReloadLandmarks`. Atteso: "Monumenti nella libreria: 1".

| # | Prova | Atteso | Se no |
|---|---|---|---|
| p | Vai all'edificio | al posto della sagoma estrusa il modello, sul baricentro, girato come il lato lungo, a terra | "monumenti: 0 a schermo": l'id non corrisponde (Output Log, `[GeoBuildings]`: "id non valido" o "il modello non c'e'") |
| q | `YawDegrees` 90, poi `ReloadLandmarks` | il modello ruota di 90° | |
| r | Una chiesa fatta a parti, con il wikidata del contorno | il modello, e nessuna parte estrusa sotto | parti rimaste: la chiesa sta a cavallo di due blocchi della pipeline (raro: segnalalo) |
| s | Vola via e torna (o fai un rebase) | il modello resta al suo posto | |
| t | Nel viewport: View Mode → Nanite Visualization → Triangles, con un modello Nanite | il modello colorato a triangoli | se non si colora, il modello non ha "Enable Nanite Support" |

## 5. Se qualcosa non va

**Nessun edificio.**

- `geo.Buildings.Stats`: se dice "Nessun dataset", aprilo; se tutto è a zero,
  il dataset non copre questa zona (`geo.Buildings.Open` stampa l'area);
- celle "in attesa" che non scendono: il terreno sotto non è a schermo
  (`geo.Terrain.Stats`);
- `geo.Buildings.Enable 1`; sei più in alto del raggio.

**Galleggiano o affondano.** Gli edifici stanno sulla superficie
**disegnata**, alla sua quota più bassa sotto il contorno. Quando il terreno
cambia livello la cella si rifà: un attimo di sfasamento è normale. Se
restano sfasati, guarda se `geo.Terrain.MeshStep` è cambiato.

**Grigi, senza finestre.** Manca `M_GeoBuilding`: `geo.Imagery.CreateMaterial`.
L'overlay e `geo.Buildings.Stats` lo dicono in rosso.

**Lento.** In ordine:

```
geo.Buildings.Radius 3000
geo.Buildings.Shadows 0
```

`stat unit` prima e dopo. Se il collo è il game thread (picchi quando la
camera si muove), è la creazione dei componenti: il raggio aiuta più delle
ombre.

## 6. Se non compila

Le API di Unreal usate qui:

| Dove | API | Se non c'è |
|---|---|---|
| `GeoBuildingsSubsystem.cpp`, `PrepareBuildingMesh` | `FDynamicMesh3::NonManifoldID` | metti `-2` |
| idem | `Attributes()->EnablePrimaryColors()`, `PrimaryColors()` | la strada è la stessa del terreno: se manca, dimmelo |
| `ApplyResult` | `SetEnableRaytracing(false)` | togli la riga (come per il terreno) |
| `ApplyResult` | `SetMesh(FDynamicMesh3&&)` | è la stessa chiamata del terreno |
| `LoadLandmarks` | `TSoftObjectPtr::LoadSynchronous`, `FPackageName::DoesPackageExist` | come la libreria degli alberi |
| `GeoTerrainMaterialFactory.cpp`, `BuildBuildingGraph` | `UMaterialExpressionTextureCoordinate::CoordinateIndex`, `FExpressionInput::Connect` sull'`AppendVector` | si può rifare a mano: un VertexColor RGB + A in un Append, TexCoord 0 e 1, e un Custom con il testo di `BuildingShader.inl` |
| `GeoBuildingSettings.h` | `UDeveloperSettings` | come la libreria degli alberi (modulo `DeveloperSettings`) |

Se il materiale non si costruisce, a mano: un nodo Custom (uscita Float 4)
con il testo di `BuildingShader.inl` fra le parentesi della macro, gli
ingressi `Paint` (VertexColor RGB e A uniti da un Append), `Facade`
(TexCoord 0), `Style` (TexCoord 1), `Normal` (VertexNormalWS), `Pos`
(World Position, passata da un TruncateLWC), `Ground` e `Frame` (MPC_GeoWeather,
GroundWeather e GroundFrame), `UuPerMetre` (100); RGB al Base Color, A alla
Roughness.

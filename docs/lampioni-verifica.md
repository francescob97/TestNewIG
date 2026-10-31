# I lampioni — come provarli

> Le decisioni sono in `docs/lampioni-design.md`; il racconto per chi impara
> in `docs/manuale/19-lampioni.md`.

## 0. Cosa è già provato qui, senza Unreal

**C++** (`georoads_tests`, sezioni 13 e 14: 55 controlli):

1. il byte del tag `lit` nel record (sì, no, non si sa; un valore strano è
   "non si sa");
2. un paese di prova (nove vie, una provinciale che prosegue in campagna, una
   terziaria, sentieri, un ponte, autostrade): la rete di casa fitta in paese
   (15,5 km/km²) e nulla in campagna; una via di 600 m con 21 pali da 7,5 m a
   26 m, sul marciapiede (3,25 m dal centro), lo sbraccio verso la strada,
   tutti dallo stesso lato; la provinciale larga a coppie in paese e buia in
   campagna (40 e 0); la terziaria media alternata; nessun palo su una
   carreggiata (18 saltati agli incroci e agli angoli); la via spezzata in
   dieci pezzi ricucita (5 lampioni a 26 m); niente con `lit=no`, sullo
   sterrato, sul ponte, sui sentieri senza tag, sull'autostrada senza tag, su
   una stradina isolata; lanterne sul sentiero con `lit=yes`; pali da 12 m ogni
   42 m sull'autostrada con `lit=yes`;
3. le fotocellule fra 15 e 60 lux, lo stesso circuito insieme; LED neutro sulle
   principali e caldo nei quartieri; tre comuni su quattro ai LED; due volte gli
   stessi lampioni; due tile vicine senza doppioni sul confine;
4. la luce che vede una fotocellula (440 lux al tramonto, ~50 a −3°, un quarto
   con il cielo coperto); l'accensione e l'isteresi della mattina; Torino il 21
   giugno: 14 e 27 minuti dopo il tramonto, prima con il cielo coperto; la
   riduzione del flusso; le luci vere (le candele, i più vicini, il peso);
5. le quattro celle di una tile si dividono i lampioni, sul terreno, ognuno con
   il suo numero; le tre forme dei pali; il quadratino dei punti luce;
6. lo shader compilato come C++: il punto si apre verso la camera (2,5 pixel da
   lontano, 80 cm da vicino, anche visto da sopra), è avvicinato di due punti,
   brilla di notte e non di giorno, **con la stessa formula del C++** sera e
   mattina, più debole a 5 km, sparisce a 3 m; il vetro del palo.

**Python** (`tests/test_roads.py`): il tag `lit` (valori, orari, ferrovie), il
byte avanti e indietro, la tile di prima che si legge uguale, il tag che
arriva dalla OSM alla tile.

Quello che **non** è provato: tutto quello che passa per Unreal (i componenti,
il materiale vero, le luci, l'esposizione). È il lavoro di questa pagina.

## 1. Compilare e rifare i materiali

1. Compila il progetto (le API più a rischio sono nella sezione 5).
2. Apri l'editor e lancia **`geo.Imagery.CreateMaterial`**. Aggiunge a
   `MPC_GeoWeather` due parametri (`StreetLights`, `StreetLightView`) e crea
   `M_GeoStreetLight` (i punti luce) e `M_GeoStreetLamp` (i pali). Atteso:
   "MPC_GeoWeather: aggiunti 2 parametri" e "Materiali pronti".
3. Facoltativo: rifai il dataset delle strade (`geoworld build-roads`, lo stesso
   comando di prima) per avere anche il tag `lit` di OSM. Senza, i lampioni ci
   sono lo stesso: decidono la classe e la rete di casa.

## 2. La prima occhiata

Con le strade aperte (per esempio `geo.Roads.Demo ...` su Torino, o
`geo.Roads.Open <cartella>` con il terreno già a schermo):

```
geo.Lights.Demo
```

Porta il cielo alle 22:00, ferma l'orologio, accende l'overlay. Atteso:

- in alto a sinistra "GeoWorld | lampioni": la luce (pochi lux, "sera"), le
  celle (verdi con i pali, arancio solo punti luce), i lampioni, le luci vere;
- da 300–500 m di quota: le vie della città come **file di puntini** arancioni
  e bianchi, le principali più luminose, la campagna buia, le autostrade buie;
- scendendo a 30–50 m: i **pali** con lo sbraccio verso la strada, il vetro
  acceso, e sotto i più vicini le **pozze di luce** sull'asfalto e sui muri
  (sfere gialle nell'overlay);
- nelle stampe a destra la riga "Lampioni: accesi, N lampioni, 48 luci vere".

## 3. Le prove, una per una

| # | Prova | Atteso | Se no |
|---|---|---|---|
| a | `geo.Lights.Stats` | "accesi, N lampioni, 48 luci vere"; celle vicine e lontane | "serve il dataset delle strade": `geo.Roads.Open` |
| b | Vola a 400 m sopra la città | file di punti lungo le vie, più fitti in centro; le strade di campagna buie | nessun punto: sezione 4, "nessun punto luce" |
| c | Allontanati fino a 6–8 km | i punti restano puntini (non spariscono), più deboli, nella foschia | spariscono a qualche km: `geo.Lights.Radius 12000` (e leggi la 4) |
| d | Scendi a 40 m in una via residenziale | pali da 7–8 m ogni ~26 m, sul marciapiede, lo sbraccio verso la strada | pali in mezzo alla strada: segnalalo con l'overlay acceso |
| e | Sotto i pali più vicini | pozze di luce sull'asfalto, sui muri, sugli alberi; nessuna luce che "scatta" camminando | nessuna pozza: sezione 4, "niente pozze di luce" |
| f | Un incrocio | nessun palo sulla carreggiata dell'altra strada | segnalalo (posizione e classe delle due strade) |
| g | Una strada principale larga in centro | pali a coppie, uno di fronte all'altro | |
| h | Una zona pedonale | lanterne basse (4 m), più fitte | |
| i | `geo.Sky.Time 20:30 2026-06-21`, poi `geo.Sky.Speed 60` | dopo il tramonto (21:33) le vie si accendono a gruppi fra le 21:45 e le 22:00, non tutte insieme | tutte insieme: le soglie dei circuiti non arrivano (segnalalo) |
| j | `geo.Weather.Clouds 1` all'imbrunire | si accendono prima | |
| k | `geo.Sky.Time 02:00` | un po' meno luce (−30%); la riga dice "flusso ridotto" | |
| l | `geo.Sky.Time 05:15` (giugno), `geo.Sky.Speed 60` | si spengono prima dell'alba, dalla luce più alta | |
| m | `geo.Sky.Time 12:00` | spenti, i punti nascosti; i pali restano (di giorno si vedono) | |
| n | `geo.Lights.Force on` di giorno | accesi per guardarli; `geo.Lights.Force auto` torna alla fotocellula | |
| o | Vola per 30 km (rebase) | i lampioni e le luci vere non saltano | |
| p | `geo.Lights.Enable 0` | spariscono tutti, anche le luci vere | |
| q | `stat unit` con e senza lampioni (`geo.Lights.Enable 0`/`1`) | qualche decimo di ms in più sulla GPU | molto di più: `geo.Lights.Count 16`, `geo.Lights.Radius 5000` |

## 4. Se qualcosa non va

**Nessun punto luce.** Nell'overlay: se manca `M_GeoStreetLight` c'è una riga
rossa (`geo.Imagery.CreateMaterial`). Se le celle sono "in attesa", manca il
terreno sotto (i lampioni stanno sul terreno disegnato). Se le celle sono
"vuote", in quella zona il dataset non ha strade di casa: guarda la densità
(`geo.Lights.Density 1 0.5` accende di più). Se la luce è alta (centinaia di
lux), è giorno per la fotocellula: `geo.Lights.Force on`.

**I punti luce spariscono muovendo la camera, ai bordi dello schermo.** I
bounds del quadratino non sono stati allargati (sezione 5, la prima riga).

**I punti luce sono quadrati, o enormi.** Il materiale non riceve
`StreetLightView` (MPC vecchia: riga rossa, `geo.Imagery.CreateMaterial`).

**Niente pozze di luce.** `geo.Lights.Count` (0 = nessuna); le luci vere stanno
solo sui lampioni accesi delle celle vicine (verdi nell'overlay): scendi sotto
i 700 m. Se l'overlay mostra le sfere gialle ma l'asfalto resta buio,
l'esposizione della notte è troppo bassa: `geo.Lights.Brightness 3`.

**Le strade illuminate sono bianche, bruciate.** `geo.Lights.Brightness 0.4`,
o `geo.Sky.NightExposure -3` (`docs/cielo-verifica.md`, sezione 4).

**Costa troppo.** Le luci vere sono la parte più cara: `geo.Lights.Count 16`.
Con le MegaLights del progetto accese (Project Settings → Rendering) se ne
possono tenere centinaia.

## 5. Se non compila

| Dove | API | Se non c'è |
|---|---|---|
| `GeoStreetLightsSubsystem.cpp` | `UStaticMesh::SetPositiveBoundsExtension`, `SetNegativeBoundsExtension`, `CalculateExtendedBounds` | nella tua versione i campi sono pubblici: `Mesh->PositiveBoundsExtension = ...`; oppure togli le righe e metti `Sprites->SetBoundsScale(2000.0f)` sul componente |
| idem | `ULocalLightComponent::SetIntensityUnits`, `ELightUnits::Candelas` | togli la riga e moltiplica l'intensità per 4π (le unità di default sono lumen per le luci nuove) |
| idem | `UInstancedStaticMeshComponent::SetCullDistances` | togli la riga: i pali si disegnano fino al raggio della cella |
| idem | `APlayerCameraManager::GetFOVAngle`, `UGameViewportClient::GetViewportSize` | togli: restano 90° e 1920 pixel |
| `GeoTerrainMaterialFactory.cpp` | `UMaterialExpressionCameraPositionWS` | nell'editor è "Camera Position": cercalo a mano |
| idem | `bEnableResponsiveAA` (per nome) | se non c'è la factory lo dice in giallo: cercalo in Details |

Se `M_GeoStreetLight` non si costruisce, a mano: Unlit, Additive, Two Sided,
"Used with Instanced Static Meshes"; un nodo Custom (uscita Float 3) con il
testo di `GEO_STREETLIGHT_SPRITE` in `StreetLightShader.inl` (ingressi `Pos` =
Absolute World Position senza offset, `Cam` = Camera Position, `Corner` =
Vertex Color, `View` = MPC `StreetLightView`, `UuPerMetre` = 100) al World
Position Offset; un secondo con `GEO_STREETLIGHT_GLOW` (in più `Tint` = i dati
per istanza 0, 1, 2 uniti, `Switch` = il dato 3, `Lights` = MPC
`StreetLights`) all'Emissive.

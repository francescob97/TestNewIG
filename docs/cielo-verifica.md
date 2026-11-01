# Il cielo — come provarlo

> Le decisioni sono in `docs/cielo-design.md`; il racconto per chi impara in
> `docs/manuale/18-cielo.md`.

## 0. Cosa è già provato qui, senza Unreal

**C++** (`geosky_tests`, 33 test):

1. il tempo: giorni giuliani e calendario (andata e ritorno), giorno della
   settimana, tempo siderale (Meeus 12.a e 12.b), ΔT, l'ora legale 2024 (31
   marzo – 27 ottobre alle 01:00 UT), mezzogiorno italiano d'estate e
   d'inverno, la mezzanotte locale, le ore e le date scritte in console;
2. il sole: Meeus 25.a, l'equazione del tempo (−14 minuti a febbraio, +16 a
   novembre), il solstizio, Torino il 21 giugno (mezzogiorno vero alle 13:31
   a 68,4°, levata 5:42 e tramonto 21:20 come la formula chiusa, azimut della
   levata), il giorno all'equatore (12 h 7 min), Tromsø (né tramonto né
   levata), il crepuscolo civile;
3. la luna: i 120 termini (le somme dell'esempio 47.a identiche), posizione,
   l'eclisse dell'8 aprile 2024 (0,007° fra sole e luna), la luna piena del 23
   aprile, crescente e calante, levate e tramonti;
4. la rotazione dal cielo locale all'equatoriale (le stelle);
5. la luce: sole, crepuscolo, luna (legge di Allen), notte, finestre;
6. lo shader del cielo notturno compilato come C++: stelle di notte e non di
   giorno, che girano con il cielo; la Via Lattea; la luna piena, nuova, al
   primo quarto (il lato verso il sole), di giorno pallida, dietro le nuvole
   no.

E le stelle vere e i pianeti (sezioni 7, 8, 9 dello stesso `geosky_tests`):
il file della pipeline (`stars_fixture.gst`) e i file rovinati; la
precessione di Meeus 21.b identica al libro; il polo celeste spostato di
0,15° e il punto vernale di 0,37° dal 2000; il cielo specchiato è una
rotazione vera e dà lo stesso cielo della formula di sempre (18 stelle, meno
di un milionesimo di grado); la Polare a 45° verso nord a ogni ora; Sirio
che passa a 28°, Vega a 84°; i colori (Vega azzurrina, Betelgeuse arancione);
la magnitudine limite; Venere di Meeus 33.a, la grande congiunzione del 2020,
le opposizioni di Marte, Giove e Saturno del 2020 (posizione e magnitudine),
la massima elongazione di Venere, gli anelli di Saturno di taglio nel 2025;
lo shader delle stelle (la grandezza del punto, 2,512 volte per magnitudine,
l'orizzonte, la scintillazione che i pianeti non hanno). In Python
(`tests/test_stars.py`): il catalogo letto a colonne fisse, il file `.gst`,
il comando senza rete.

Più due test negli edifici (`geobuildings_tests`): di notte metà delle
finestre accese, mai fra una e l'altra, né sui tetti, né sotto il suolo; a
notte fonda poche, tutte fra quelle della sera.

Quello che **non** è provato: tutto quello che passa per Unreal (la luce che
gira, la SkyAtmosphere, l'esposizione, la sfera delle stelle). È il lavoro di
questa pagina.

## 1. Compilare e rifare i materiali

1. Compila il progetto (le API più a rischio sono nella sezione 5).
2. Apri l'editor e lancia **`geo.Imagery.CreateMaterial`**. Aggiunge a
   `MPC_GeoWeather` i sei parametri del cielo (`SkySun`, `SkyMoon`,
   `SkyEquatorX/Y/Z`, `SkyNight`), crea `M_GeoNightSky` e rifà
   `M_GeoBuilding` con le finestre accese. Atteso: "MPC_GeoWeather: aggiunti
   6 parametri" e "Materiali pronti".

## 2. La prima occhiata

Con un terreno a schermo (per esempio `geo.Buildings.Demo ...` su Torino):

```
geo.Sky.Demo
```

Parte 40 minuti prima dell'alba di oggi e fa correre il tempo 300 volte (un
giorno in poco meno di 5 minuti), con l'overlay. Atteso:

- in alto a sinistra "GeoWorld | cielo": ora locale, sole (azimut, altezza,
  alba, tramonto), luna (fase, altezza), luce, la scena;
- il cielo che si schiarisce a est, il sole che sorge **a est-nord-est
  d'estate e a sud-est d'inverno**, le ombre che si accorciano e girano;
- il tramonto rosso a ovest, il crepuscolo, poi la notte: il cielo scuro, le
  stelle, la luna se c'è, le **finestre accese**;
- nelle stampe a destra le righe "Ora" e "Sole" (e "Luna" di notte).

`geo.Sky.Speed 0` ferma il tempo; `geo.Sky.Time 18:30` porta a un'ora precisa.

## 3. Le prove, una per una

| # | Prova | Atteso | Se no |
|---|---|---|---|
| a | `geo.Sky.Time 12:00 2026-06-21`, poi `geo.Sky.Stats` | a Torino il sole a sud-est (azimut ~132°), alto ~61°; a sud alle 13:31 (mezzogiorno vero, con l'ora legale), alto 68° | se è basso o a nord: la direzione è girata, segnalalo con l'overlay acceso |
| b | `geo.Sky.Time 13:31` e guarda le ombre degli edifici | corte, verso nord | ombre verso sud: la luce è girata al contrario (dimmelo) |
| c | `geo.Sky.Time 20:00` | sole basso (~12°) a ovest-nord-ovest, ombre lunghe cinque volte gli edifici verso est-sud-est, luce calda | |
| d | `geo.Sky.Time 21:45` | crepuscolo: il cielo blu scuro a ovest, finestre che si accendono | tutto nero di colpo: manca la SkyAtmosphere (l'overlay lo dice) |
| e | `geo.Sky.Time 23:30` | notte: stelle, finestre accese, la scena scura e blu ma leggibile | nera: sezione 4, "la notte è nera"; grigia come di giorno: sezione 4, "la notte è chiara" |
| f | Una notte di luna piena (per esempio `geo.Sky.Time 23:00 2026-03-03`) | la luna in cielo, piena, e la scena più chiara (luce argento, ombre della luna) | luna senza fase o niente luna: manca `M_GeoNightSky` (riga rossa) |
| g | `geo.Sky.Time 21:00 2026-03-21` (falce crescente a ovest) | una falce, con la parte accesa **verso il punto dove il sole è tramontato** | falce dalla parte sbagliata: segnalalo |
| h | `geo.Sky.Time 03:00` | poche finestre accese (sempre le stesse di prima, meno) | |
| i | `geo.Sky.Date 2026-12-21`, `geo.Sky.Time 12:30` | sole basso a sud (~21°), ombre lunghe anche a mezzogiorno | |
| j | `geo.Weather.Rain 4` di notte | la pioggia si vede appena, non come strisce bianche | strisce chiare: il materiale della pioggia è vecchio (`geo.Imagery.CreateMaterial`) |
| k | `geo.Weather.Strike 1500` di notte | il lampo illumina tutto, acceca | |
| l | `geo.Weather.Clouds 0.8` di notte | le stelle quasi tutte spente | |
| m | Vola per 30 km (rebase) | il sole e le stelle non saltano | |
| n | `geo.Sky.Enable 0` | il sole del livello torna dov'era, la luna e le stelle spariscono | |
| o | `geo.Sky.Now` | l'ora vera, il tempo che corre come l'orologio | |

## 3b. Le stelle vere e i pianeti

1. Il catalogo, una volta (serve la rete: lo scarica dal CDS di Strasburgo,
   0,4 MB):
   ```
   geoworld build-stars -o D:/GeoData/stars.gst
   ```
   Atteso: "Stelle: 9096 fino alla magnitudine 6.5" (più o meno: qualche voce
   del catalogo non ha la posizione), "la più luminosa: magnitudine -1.46".
2. In Unreal: `geo.Imagery.CreateMaterial` (crea `M_GeoStars`, aggiunge
   `SkyStars` a `MPC_GeoWeather`, rifà `M_GeoNightSky`), poi
   `geo.Sky.Catalog D:/GeoData/stars.gst`. Per averlo a ogni avvio: Project
   Settings → Plugins → GeoWorld - Cielo → Star Catalog.

| # | Prova | Atteso | Se no |
|---|---|---|---|
| a | `geo.Sky.Time 22:00 2026-01-15`, guarda a sud | Orione: le tre stelle della cintura in fila, Betelgeuse arancione in alto a sinistra, Rigel azzurrina in basso a destra; Sirio sotto, la più luminosa | stelle a caso: il catalogo non è caricato (`geo.Sky.Stats` dice "finte") |
| b | `geo.Sky.LookAt polare` | la Polare a 45° (a Torino) verso nord; il Grande Carro attorno | la Polare altrove: segnalalo con l'overlay acceso |
| c | `geo.Sky.Speed 600` | il cielo gira attorno alla Polare, che resta ferma | |
| d | `geo.Sky.Time 19:00 2026-03-01` (o una sera in cui Venere è della sera), `geo.Sky.Planets` | la tabella dei pianeti; quelli alti si vedono, Venere la più luminosa dopo la luna | |
| e | `geo.Sky.LookAt venere` all'imbrunire | Venere compare prima delle stelle (a −1° di sole già si vede) | |
| f | `geo.Sky.Time 23:00 2020-12-21` e `geo.Sky.LookAt giove` | Giove e Saturno quasi attaccati (un decimo di grado): la grande congiunzione | |
| g | Una notte di luna piena | meno stelle (la magnitudine limite scende a ~4,8) | |
| h | Guarda verso l'orizzonte | le stelle basse più deboli e tremolanti; i pianeti fermi | |
| i | `geo.Sky.Catalog off` | tornano le stelle finte; i pianeti restano | |
| j | `geo.Sky.StarBrightness 3` | stelle più luminose | |

## 4. Se qualcosa non va

**La notte è nera.** L'esposizione automatica del progetto non scende
abbastanza. In ordine: `geo.Sky.NightExposure 0` (non abbassarla),
`geo.Sky.MoonLight 0.003` (più luce di luna); oppure, nel Post Process Volume
del livello, Exposure → Min EV100 più basso (−4).

**La notte è chiara come il giorno.** L'esposizione si adatta troppo:
`geo.Sky.NightExposure -3` o `geo.Sky.MoonLight 0.0003`.

**Le ombre spariscono a qualche centinaio di metri.** Le Virtual Shadow Maps
sono spente (`r.Shadow.Virtual.Enable 0`): il cielo porta le ombre a cascata
a 3 km (Project Settings → GeoWorld - Cielo → Shadow Distance M); oppure
accendile.

**Le montagne non fanno ombra.** È voluto: `geo.Terrain.Shadows 1` (costa).

**Le stelle non si vedono, o sono velate.** Sono sulla sfera "GeoWorld Cielo
notturno" (nell'Outliner). In `M_GeoNightSky`, Details → Translucency:
"Apply Fogging" spento. `geo.Sky.Stars 1`; niente nuvole o pioggia sopra di
te.

**Le stelle tremolano o "strisciano" muovendo la camera.** Sono grandi come
un pixel: l'antialiasing temporale le confonde. In `M_GeoNightSky`
"Responsive AA" acceso (la factory lo fa).

**Le stelle vere non si vedono.** `geo.Sky.Stats`: se dice "finte", il
catalogo non è aperto (`geo.Sky.Catalog`). Se l'overlay (`geo.Sky.Debug 1`)
ha una riga rossa "mancano M_GeoStars o SkyStars", rilancia
`geo.Imagery.CreateMaterial`. Se la magnitudine limite è sotto zero è ancora
giorno per le stelle (o c'è la luna piena alta).

**Le stelle sono quadrati, o grosse come la luna.** Il materiale non riceve
`SkyStars` (MPC vecchia): `geo.Imagery.CreateMaterial`.

**Le stelle spariscono quando giri la camera.** I bounds del quadratino non
sono stati allargati (sezione 5, `SetPositiveBoundsExtension`).

**Il sole del livello resta fermo.** `geo.Sky.Stats`: se dice "in attesa
della camera", il cielo non sa dove sei; se il sole è "--" nell'overlay, non
c'è una luce direzionale e il cielo ne ha creata una ("GeoWorld Sole").

## 5. Se non compila

| Dove | API | Se non c'è |
|---|---|---|
| `GeoSkySubsystem.cpp` | `UDirectionalLightComponent::SetAtmosphereSunLightIndex`, `SetDynamicShadowDistanceMovableLight`, `AtmosphereSunDiskColorScale` | togli la riga: la luna userà l'indice 0 (niente luce della luna sul cielo) |
| idem | `UExponentialHeightFogComponent::SetFogInscatteringColor`, `SetDirectionalInscatteringColor` | togli `ApplyFog` (la nebbia resterà chiara di notte) |
| idem | `USkyLightComponent::bRealTimeCapture`, `RecaptureSky` | togli `ApplySkyLight` |
| idem | `APostProcessVolume::Settings.bOverride_AutoExposureBias` | togli `ApplyExposure` e regola l'esposizione nel livello |
| idem | `UPrimitiveComponent::SetTranslucentSortPriority` | togli la riga |
| idem | `FDateTime::GetJulianDay` | `FDateTime::UtcNow().GetTicks()`: (ticks / 864e9) + 1721425,5 |
| `GeoTerrainMaterialFactory.cpp` | `UMaterialExpressionCameraVectorWS` | nell'editor è "CameraVector" |
| idem | i flag per nome `bUseTranslucencyVertexFog`, `bApplyCloudFogging`, `bEnableResponsiveAA` | se il nome non c'è la factory lo dice in giallo: cercali a mano in Details |
| `GeoSkyStars.cpp` | `UStaticMesh::SetPositiveBoundsExtension`, `SetNegativeBoundsExtension`, `CalculateExtendedBounds` | i campi pubblici `PositiveBoundsExtension`/`NegativeBoundsExtension`; o `Field->SetBoundsScale(20000.0f)` |
| idem | `UInstancedStaticMeshComponent::UpdateInstanceTransform`, `SetCustomDataValue` | i pianeti restano dove erano all'avvio del cielo: togli le righe |
| `GeoSkySettings.h` | `FFilePath` con `FilePathFilter` | togli il `meta` |

Se `M_GeoNightSky` non si costruisce, a mano: Unlit, Additive, Two Sided,
"Apply Fogging" spento; un nodo Custom (uscita Float 3) con il testo di
`NightSkyShader.inl` fra le parentesi della macro, gli ingressi `View`
(CameraVector), `EqX`, `EqY`, `EqZ`, `Moon`, `Sun`, `Night` (i parametri
`SkyEquatorX/Y/Z`, `SkyMoon`, `SkySun`, `SkyNight` di MPC_GeoWeather; `Catalog`
= `SkyStars`), uscita
all'Emissive.

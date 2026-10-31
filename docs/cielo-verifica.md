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

Se `M_GeoNightSky` non si costruisce, a mano: Unlit, Additive, Two Sided,
"Apply Fogging" spento; un nodo Custom (uscita Float 3) con il testo di
`NightSkyShader.inl` fra le parentesi della macro, gli ingressi `View`
(CameraVector), `EqX`, `EqY`, `EqZ`, `Moon`, `Sun`, `Night` (i parametri
`SkyEquatorX/Y/Z`, `SkyMoon`, `SkySun`, `SkyNight` di MPC_GeoWeather), uscita
all'Emissive.

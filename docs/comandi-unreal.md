# I comandi di GeoWorld nella console di Unreal

> **Generato dal codice**: `python3 Plugins/GeoWorld/Tools/ListConsoleCommands.py`.
> Non modificarlo a mano: i controlli statici (`CheckSourceDiscipline.sh`, regola 7)
> dicono quando e' indietro. In Unreal lo stesso elenco lo da' `geo.Help`.

**172 comandi** in 14 gruppi.

## Come si usano

- **La console**: il tasto della console (di solito quello sotto Esc); nell'editor anche il
  campo **Cmd** in basso a sinistra. **Tab** completa i nomi.
- `<...>` va scritto, `[...]` si puo' omettere, `a|b` vuol dire uno dei due.
- **I percorsi con spazi vanno fra virgolette**:
  `geo.Terrain.Demo "C:\Unreal Projects\TestNewIG\Pipeline\dataset\dted"`.
- Quasi tutti funzionano sia nell'editor sia in Play. `geo.Fly` solo in Play;
  `geo.Imagery.CreateMaterial` solo nell'editor.
- `<0|1>` senza argomento, di solito, inverte lo stato.
- Dentro Unreal: `geo.Help` elenca i gruppi (e tutto nell'Output Log),
  `geo.Help Weather` i comandi di un gruppo, `geo.Help Open` quelli che
  contengono una parola.

## Per cominciare

| Voglio... | Comando |
|---|---|
| vedere un dataset del terreno | `geo.Terrain.Demo "D:\GeoWorld\Pipeline\dataset\dted"` |
| terreno e ortofoto | `geo.Imagery.Demo "<terreno>" "<ortofoto>"` |
| terreno, ortofoto e strade | `geo.Roads.Demo "<terreno>" "<ortofoto>" "<strade>"` |
| terreno, ortofoto, acqua (e strade) | `geo.Water.Demo "<terreno>" "<ortofoto>" "<acqua>" "<strade>"` |
| terreno, ortofoto e alberi | `geo.Vegetation.Demo "<terreno>" "<ortofoto>" "<vegetazione>"` |
| terreno, ortofoto ed edifici | `geo.Buildings.Demo "<terreno>" "<ortofoto>" "<edifici>"` |
| andare in un posto | `geo.Goto 45.07 7.69 2000   oppure   geo.Goto Torino` |
| sapere dove sono | `geo.Where   (e le stampe a destra: geo.Hud 1)` |
| volare (in Play) | `geo.Fly` |
| rifare i materiali dopo un aggiornamento | `geo.Imagery.CreateMaterial` |
| pioggia, neve, sereno | `geo.Weather.Rain 4   geo.Weather.Snow 1   geo.Weather.Clear` |
| un temporale, o un fulmine subito | `geo.Weather.Storm   oppure   geo.Weather.Strike 1500` |
| vento | `geo.Weather.Wind 10 270` |
| il giorno e la notte | `geo.Sky.Demo   oppure   geo.Sky.Time 18:30` |
| capire perche' non vedo niente | `geo.Terrain.Diag   (poi viewmode wireframe, r.Fog 0)` |
| i comandi di un gruppo, dentro Unreal | `geo.Help Weather` |

## I gruppi

- [Generale](#generale) — `geo.*`, 15 comandi
- [Le tile delle quote](#le-tile-delle-quote) — `geo.Tiles.*`, 10 comandi
- [Cosa disegnare](#cosa-disegnare) — `geo.Lod.*`, 13 comandi
- [Il terreno](#il-terreno) — `geo.Terrain.*`, 16 comandi
- [Le ortofoto](#le-ortofoto) — `geo.Imagery.*`, 8 comandi
- [Strade, ferrovie, piste](#strade-ferrovie-piste) — `geo.Roads.*`, 11 comandi
- [L'acqua](#lacqua) — `geo.Water.*`, 5 comandi
- [Il meteo](#il-meteo) — `geo.Weather.*`, 26 comandi
- [La vegetazione](#la-vegetazione) — `geo.Vegetation.*`, 12 comandi
- [Gli edifici](#gli-edifici) — `geo.Buildings.*`, 9 comandi
- [Il cielo](#il-cielo) — `geo.Sky.*`, 18 comandi
- [La camera di volo](#la-camera-di-volo) — `geo.Fly.*`, 2 comandi
- [Entities](#entities) — `geo.Entities.*`, 16 comandi
- [Lights](#lights) — `geo.Lights.*`, 11 comandi

## Generale

Dove sei, andare in un posto, l'origine del mondo, la diagnosi.

| Comando | Cosa fa |
|---|---|
| `geo.Help [gruppo o parola]` | i comandi di GeoWorld: senza argomenti i gruppi (e tutti i comandi nell'Output Log), con un gruppo (Weather) o una parola (Open) quelli che corrispondono. |
| `geo.Where` | Stampa la posizione geografica della camera attiva. |
| `geo.Origin` | Stampa l'origine corrente e le statistiche di rebasing. |
| `geo.Goto <lat> <lon> [quota] \| <nome> [quota_sul_suolo]` | porta la camera su un punto. |
| `geo.Places` | Elenca i luoghi noti utilizzabili con geo.Goto. |
| `geo.ViewSpeed <1..8> [moltiplicatore]` | velocita' della camera del viewport dell'editor. |
| `geo.Rebase` | Forza un rebase dell'origine sulla posizione attuale della camera. |
| `geo.AutoRebase <0\|1>` | attiva o disattiva il rebasing automatico. |
| `geo.RebaseThreshold <km>` | cambia a caldo la soglia di rebasing. |
| `geo.Debug <0\|1>` | mostra o nasconde l'overlay di debug. |
| `geo.SpawnMarkers` | Piazza i cubi di verifica sui punti noti dell'Italia. |
| `geo.ClearMarkers` | Rimuove tutti i marker di verifica. |
| `geo.Diag` | Diagnostica del jitter: stampa i numeri che distinguono precisione, rebasing e antialiasing. |
| `geo.Quality <portatile\|workstation>` | imposta insieme soglia, dettaglio fuori vista, passo, budget. |
| `geo.Hud <0\|1>` | latitudine, longitudine, quota e rotta sulla destra dello schermo (default 1). |

## Le tile delle quote

Aprire un dataset del terreno, streaming e cache (Fase 3).

| Comando | Cosa fa |
|---|---|
| `geo.Tiles.Open <cartella>` | apre un dataset prodotto dalla pipeline. |
| `geo.Tiles.Info` | Descrive il dataset aperto, livello per livello. |
| `geo.Tiles.Stats` | Statistiche di streaming e cache. |
| `geo.Tiles.Load <livello> [lat] [lon]` | carica la tile sotto la camera o sul punto dato. |
| `geo.Tiles.Budget <MB>` | cambia a caldo il budget della cache. |
| `geo.Tiles.Clear` | Svuota la cache e azzera le statistiche. |
| `geo.Tiles.Debug <0\|1>` | overlay con statistiche di streaming e cache. |
| `geo.Tiles.Draw <0\|1>` | disegna nel mondo il volume delle tile in cache. |
| `geo.Tiles.LoadAround [livello] [raggio]` | carica un riquadro di tile attorno alla camera. |
| `geo.Tiles.Demo <cartella> [livello]` | apre un dataset, ci va sopra e mostra tutto. |

## Cosa disegnare

Livello di dettaglio, residenza, precarico (Fase 4 e dopo).

| Comando | Cosa fa |
|---|---|
| `geo.Lod.Enable <0\|1>` | attiva la selezione LOD per frame. |
| `geo.Lod.Error <pixel>` | errore su schermo tollerato. Piu' basso = piu' dettaglio. |
| `geo.Lod.Freeze <0\|1>` | congela la vista usata dal LOD e lascia muovere la camera. |
| `geo.Lod.Debug <0\|1>` | overlay con le statistiche di selezione. |
| `geo.Lod.Draw <0\|1>` | disegna la tassellatura scelta, un colore per livello. |
| `geo.Lod.Stats` | Statistiche dell'ultima selezione. |
| `geo.Lod.Demo <cartella>` | apre un dataset, ci si posiziona sopra e accende tutto. |
| `geo.Lod.Margin <fattore>` | allarga il frustum per caricare PRIMA le tile ai bordi. |
| `geo.Lod.ViewIndependent <0\|1>` | 1: si disegna tutto attorno, il frustum lo fa Unreal. 0: modo classico. |
| `geo.Lod.Prefetch <0\|1>` | precarica le tile dove la camera sta andando, e un anello di sicurezza. |
| `geo.Lod.Lookahead <secondi>` | quanto avanti prevedere la posizione (default 12). |
| `geo.Lod.Safety <fattore>` | anello di sicurezza in RAM: 0.5 = un livello piu' fine tutto attorno, 0 = spento. |
| `geo.Lod.OutOfView <fattore>` | meno dettaglio fuori dalla vista: 1 = uguale ovunque, 4 = default. |

## Il terreno

Geometria, gonne, ombre, budget, la demo del terreno (Fase 5).

| Comando | Cosa fa |
|---|---|
| `geo.Terrain.Enable <0\|1>` | costruisce la geometria delle tile selezionate. |
| `geo.Terrain.Wireframe <0\|1>` | mostra il reticolo dei triangoli. Costa: lascialo spento per misurare. |
| `geo.Terrain.Skirt <0\|1>` | gonne ai bordi. Spegnile per VEDERE le crepe. |
| `geo.Terrain.FlipWinding <0\|1>` | inverte l'orientamento dei triangoli. |
| `geo.Terrain.Budget <N>` | mesh CONSEGNATE al renderer per frame (la costruzione e' su thread). |
| `geo.Terrain.Stats` | Statistiche della geometria. |
| `geo.Terrain.Boxes <0\|1>` | scatole di debug sui bounds delle tile. |
| `geo.Terrain.MeshBudget <N>` | mesh da tenere costruite in tutto, visibili + nascoste (default in base alla RAM). |
| `geo.Terrain.Shadows <0\|1>` | ombre proiettate dal terreno (default 0: le foto le hanno gia', e costano molto). |
| `geo.Terrain.Debug <0\|1>` | overlay del terreno: mesh, triangoli, costruzione, transizioni. |
| `geo.Terrain.Morph <secondi>` | quando una tile si affina, quanto ci mette a passare dalla forma e dalla foto del padre alle proprie (default 0.6, 0 = di colpo). |
| `geo.Terrain.Threads <N>` | mesh in costruzione contemporanea sui thread di lavoro (default 6). |
| `geo.Terrain.MeshStep <1\|2\|4\|8>` | un post ogni N nella mesh di una tile (default 2). |
| `geo.Terrain.Warmup <N>` | mesh CONSEGNATE per frame durante il riscaldamento (dopo un salto, default 16). 0 = spento. |
| `geo.Terrain.Diag` | Diagnosi: cosa il renderer ha davvero, e dove sta rispetto alla camera. |
| `geo.Terrain.Demo <cartella>` | apre un dataset e disegna il terreno. |

## Le ortofoto

Le foto sul terreno, e i materiali di tutto il progetto (Fase 6).

| Comando | Cosa fa |
|---|---|
| `geo.Imagery.Open <cartella>` | apre un dataset di ortofoto. |
| `geo.Imagery.Enable <0\|1>` | veste il terreno con le ortofoto. |
| `geo.Imagery.Checker <0\|1>` | scacchiera al posto delle foto: mostra le UV. |
| `geo.Imagery.Debug <0\|1>` | overlay con le statistiche del drappeggio. |
| `geo.Imagery.Budget <N>` | texture create per frame (default 4). |
| `geo.Imagery.Stats` | Statistiche del drappeggio e dello streaming delle ortofoto. |
| `geo.Imagery.Demo <cartella_terreno> <cartella_ortofoto>` | apre tutto e accende. |
| `geo.Imagery.CreateMaterial` | Costruisce (o RIFA') in /GeoWorld/Materials: M_GeoTerrain (foto e strade dipinte), M_GeoRoad (strade 3D), M_GeoWater e MPC_GeoWater (acqua), MPC_GeoWeather, M_GeoPrecipitation, M_GeoLightning e M_GeoClouds (meteo), M_GeoVegetation (alberi), M_GeoBuilding (edifici), M_GeoNightSky (stelle e luna), M_GeoStars (stelle vere e pianeti), M_GeoStreetLight e M_GeoStreetLamp (lampioni), M_GeoEntity, M_GeoEntityMasked e M_GeoEntityTranslucent (i modelli delle entita'). *(solo nell'editor)* |

## Strade, ferrovie, piste

Dipinte sul terreno e 3D da vicino (Fase 8).

| Comando | Cosa fa |
|---|---|
| `geo.Roads.Open <cartella>` | apre un dataset di strade (geoworld build-roads). |
| `geo.Roads.Enable <0\|1>` | disegna strade, ferrovie e piste sul terreno. |
| `geo.Roads.Style <realistico\|mappa>` | colori veri, o da carta stradale per controllare l'allineamento. |
| `geo.Roads.Resolution <base> [livello piu' fine]` | pixel per tile (potenze di 2, 64..2048). |
| `geo.Roads.Budget <MB>` | memoria video massima per le strade (le tile a schermo l'hanno sempre). |
| `geo.Roads.Strength <0..1>` | quanto si vedono le strade sopra la foto (1 = come disegnate). |
| `geo.Roads.3D <0\|1>` | strade 3D (asfalto, segnaletica, binari, ponti) vicino alla camera. |
| `geo.Roads.3DLevels <1..3>` | quanti livelli di terreno, dal piu' fine, hanno le strade 3D (piu' livelli = piu' lontano, piu' triangoli). |
| `geo.Roads.Debug <0\|1>` | overlay con le statistiche delle strade. |
| `geo.Roads.Stats` | Statistiche delle strade e del caricamento delle tile di linee. |
| `geo.Roads.Demo <terreno> <ortofoto\|-> <strade>` | apre tutto, accende e si mette sopra le strade. |

## L'acqua

Mare, laghi, fiumi (Fase 9).

| Comando | Cosa fa |
|---|---|
| `geo.Water.Open <cartella>` | apre un dataset d'acqua (geoworld build-water) e rifa' il terreno scavato. |
| `geo.Water.Enable <0\|1>` | acqua e scavo del terreno (spegnendola il terreno torna quello del DEM). |
| `geo.Water.Debug <0\|1>` | overlay dell'acqua: superfici, attese, vento, mare. |
| `geo.Water.Stats` | Statistiche dell'acqua. |
| `geo.Water.Demo <terreno> <ortofoto\|-> <acqua> [strade\|-]` | apre tutto, accende e si mette sopra l'acqua. |

## Il meteo

Vento, pioggia, neve, grandine, fulmini e tuono.

| Comando | Cosa fa |
|---|---|
| `geo.Weather.Wind <m/s> [gradi da cui soffia] [turbolenza 0..2]` | il vento di riferimento: 10 m su un prato, come un METAR. Senza argomenti dice quello attuale. |
| `geo.Weather.Turbulence <0..2>` | le raffiche: 0 aria liscia, 1 l'atmosfera vera, 2 il doppio. |
| `geo.Weather.Breezes <0\|1>` | le brezze che fa il sole: di mare (di giorno) e di terra (di notte) sulla costa, di pendio e di valle in montagna. Seguono l'ora del cielo (geo.Sky.Time). |
| `geo.Weather.BreezeStrength <0..3>` | quanto sono forti le brezze (1 = come in natura: 3-6 m/s quella di mare, 1-3 quelle di pendio). |
| `geo.Weather.Variability <0..1>` | il tempo che cambia da solo: velocita' e direzione derivano in decine di minuti. |
| `geo.Weather.WindAloft <quota s.l.m. m> <m/s> <gradi> \| clear` | un vento in quota (sopra lo strato limite il vento lo segue). |
| `geo.Weather.Seed <n>` | il seme delle raffiche: stesso seme, stesse raffiche negli stessi punti. |
| `geo.Weather.Debug <0\|1>` | overlay del meteo: il vento (riferimento, mare, sotto di te, a 10 m, alla tua quota) e la precipitazione. |
| `geo.Weather.Arrows <quota sul suolo m>` | frecce del vento attorno a te (lunghe quanto il vento fa in 20 s); 0 le spegne. |
| `geo.Weather.Rain [mm/h] [copertura 0..1] [km]` | pioggia (default 4 mm/h, dappertutto). 1 debole, 4 moderata, 15 forte, 50 a dirotto. |
| `geo.Weather.Snow [mm/h d'acqua] [copertura 0..1] [km]` | neve a ogni quota (default 1 mm/h: ~1 cm di neve all'ora). |
| `geo.Weather.Hail [mm/h] [copertura 0..1] [km]` | temporali con grandine nei nuclei (default 30 mm/h, 40% del suolo, celle da 6 km). |
| `geo.Weather.Precipitation [mm/h] [copertura 0..1] [km]` | pioggia o neve secondo la temperatura e la quota (default 3 mm/h). Senza argomenti con il sereno, dice com'e'. |
| `geo.Weather.Storm [mm/h] [copertura 0..1] [km]` | temporali: rovesci (pioggia o neve secondo la quota) con i fulmini (default 25 mm/h, 35% del suolo, celle da 6 km). |
| `geo.Weather.Lightning <0..5>` | quanti fulmini nei temporali: 0 nessuno, 1 un temporale normale (~5 al minuto per cella), 3 forte, 5 una supercella. |
| `geo.Weather.Strike [distanza m] [nube]` | un fulmine adesso, davanti alla camera (default 1500 m; "nube": un lampo dentro la nube). Il tuono arriva dopo 3 s per km. |
| `geo.Weather.Thunder <0..2>` | il volume del tuono (0 = muto, 1 = normale). |
| `geo.Weather.Clear` | torna il sereno, e niente fulmini. Il suolo si asciuga e la neve fonde da soli (in decine di minuti). |
| `geo.Weather.Clouds [copertura 0..1 \| on \| off] [base m s.l.m.] [spessore m]` | i cumuli di bel tempo (default 0,25, base 1500 m, alti 1200 m). Le nubi di pioggia vengono dalla precipitazione. "off": niente nuvole. |
| `geo.Weather.CloudDensity <x>` | quanto sono fitte le nuvole: 1 normale (dentro si vede a ~130 m), 2 il doppio, 0,5 la meta'. |
| `geo.Weather.Temperature <gradi C al livello del mare> [gradi C per km]` | decide dove la pioggia diventa neve (default 15 e 6,5). |
| `geo.Weather.Wetness <0..1>` | quanto e' bagnato il suolo, adesso (poi segue la pioggia da solo). |
| `geo.Weather.SnowCover <quota s.l.m. m> \| off` | neve sul suolo sopra quella quota, adesso (poi scende con le nevicate e risale con il caldo). |
| `geo.Weather.Ice <0..1> [quota s.l.m. m] \| off` | il suolo ghiacciato adesso, sopra quella quota (default: lo zero termico); poi gela o sgela da solo. |
| `geo.Weather.HailGround [mm] [raggio km] \| off` | grandine posata adesso attorno a te (default 8 mm in 3 km); poi fonde da sola. |
| `geo.Weather.Fog <0\|1>` | la precipitazione toglie visibilita' con la nebbia del livello (default 1). |

## La vegetazione

Alberi e arbusti dalla copertura del suolo, la libreria dei modelli (Fase 11).

| Comando | Cosa fa |
|---|---|
| `geo.Vegetation.Open <cartella>` | apre un dataset di vegetazione (geoworld build-vegetation): gli alberi crescono attorno alla camera. |
| `geo.Vegetation.Close` | chiude il dataset di vegetazione: spariscono tutti gli alberi. |
| `geo.Vegetation.Enable <0\|1>` | accende e spegne gli alberi (il dataset resta aperto). |
| `geo.Vegetation.Density <0..4>` | quanti alberi rispetto alla natura (1 = come la copertura del suolo; 0,5 = la meta'). Default 1. |
| `geo.Vegetation.Radius <m>` | fin dove ci sono alberi (default 2500). Le bande vicina e media si allargano insieme. |
| `geo.Vegetation.Shadows <0\|1\|2>` | chi fa ombra: 0 nessuno, 1 gli alberi vicini (default), 2 tutti (con i modelli Nanite e le Virtual Shadow Maps). |
| `geo.Vegetation.ReloadModels` | rilegge la libreria dei modelli (Project Settings &gt; Plugins &gt; GeoWorld - Vegetazione) e rifa' gli alberi: dopo geo.Vegetation.CreateModels o dopo aver cambiato un modello. |
| `geo.Vegetation.FlipWinding <0\|1>` | inverte le facce degli alberi: se li vedi "dal di dentro" (la chioma sparisce dal lato verso di te). |
| `geo.Vegetation.Debug <0\|1>` | overlay della vegetazione: celle per banda (contorni colorati), alberi per specie, attese. |
| `geo.Vegetation.Stats` | Statistiche della vegetazione. |
| `geo.Vegetation.Demo <terreno> <ortofoto\|-> <vegetazione> [acqua\|-]` | apre tutto, accende e si mette 250 m sopra il centro della vegetazione. |
| `geo.Vegetation.CreateModels [1]` | salva in /GeoWorld/Vegetation le otto forme degli alberi come static mesh Nanite, la libreria di default. Con 1: facce invertite (se servi geo.Vegetation.FlipWinding 1). Prima: geo.Imagery.CreateMaterial. *(solo nell'editor)* |

## Gli edifici

Le sagome di OpenStreetMap estruse, i monumenti dalla libreria (Fase 10).

| Comando | Cosa fa |
|---|---|
| `geo.Buildings.Open <cartella>` | apre un dataset di edifici (geoworld build-buildings): le citta' si alzano attorno alla camera. |
| `geo.Buildings.Close` | chiude il dataset di edifici: spariscono tutti gli edifici. |
| `geo.Buildings.Enable <0\|1>` | accende e spegne gli edifici (il dataset resta aperto). |
| `geo.Buildings.Radius <m>` | fin dove ci sono edifici (default 6000). Le bande vicina e media si allargano insieme. |
| `geo.Buildings.Shadows <0\|1\|2>` | chi fa ombra: 0 nessuno, 1 gli edifici vicini (default), 2 tutti. |
| `geo.Buildings.ReloadLandmarks` | rilegge la libreria dei monumenti (Project Settings &gt; Plugins &gt; GeoWorld - Edifici) e rifa' le celle. |
| `geo.Buildings.Debug <0\|1>` | overlay degli edifici: celle per banda (contorni colorati), edifici, triangoli, monumenti. |
| `geo.Buildings.Stats` | Statistiche degli edifici. |
| `geo.Buildings.Demo <terreno> <ortofoto\|-> <edifici> [vegetazione\|-]` | apre tutto, accende e si mette 300 m sopra la zona con piu' edifici. |

## Il cielo

Il giorno e la notte: il sole all'ora giusta, la luna con le fasi, le stelle (Fase 12).

| Comando | Cosa fa |
|---|---|
| `geo.Sky.Time <HH:MM> [AAAA-MM-GG]` | l'ora locale (e la data): geo.Sky.Time 18:30, geo.Sky.Time 21:00 2026-06-21. Senza argomenti dice dove sono sole e luna. |
| `geo.Sky.Date <AAAA-MM-GG>` | il giorno, alla stessa ora locale: geo.Sky.Date 2026-12-21 (il solstizio d'inverno). |
| `geo.Sky.Speed <x>` | quanto corre il tempo: 0 fermo, 1 come l'orologio, 60 un minuto al secondo, 600 un giorno in 2,4 minuti; negativo all'indietro. |
| `geo.Sky.Now` | l'ora vera, adesso, e il tempo che corre come l'orologio. |
| `geo.Sky.Enable <0\|1>` | il cielo muove il sole del livello (0: il sole, la nebbia e l'esposizione tornano come erano). |
| `geo.Sky.Zone <cet\|ore>` | il fuso: cet l'ora italiana (con l'ora legale d'estate), oppure lo scostamento fisso da UT (geo.Sky.Zone 0 = UT, 5.5 = India). |
| `geo.Sky.MoonLight <rapporto>` | la luce della luna piena rispetto al sole (default 0.001; dal vero 0.0000025, troppo buio per un gioco). |
| `geo.Sky.NightExposure <stop>` | di notte l'esposizione scende di tanto (default -2; 0 = lascia fare all'esposizione automatica). |
| `geo.Sky.SunLux <lux>` | la luce del sole (0 = quella che aveva la luce del livello). Tutto il resto, luna compresa, si misura su questa. |
| `geo.Sky.Stars <0\|1>` | stelle, Via Lattea e luna con le fasi (M_GeoNightSky). |
| `geo.Sky.CityLights <0\|1>` | le finestre degli edifici accese di notte. |
| `geo.Sky.Debug <0\|1>` | overlay del cielo: ora, sole e luna (altezza, levate, tramonti, fase), luce, la scena. |
| `geo.Sky.Stats` | Dove sono il sole e la luna, e che ora e'. |
| `geo.Sky.Demo [velocita']` | un giorno intero di corsa: da prima dell'alba, 300 volte piu' veloce (un giorno in poco meno di 5 minuti), con l'overlay. |
| `geo.Sky.Catalog <file .gst\|off>` | le stelle vere del catalogo di Yale (geoworld build-stars) al posto di quelle finte; off: si torna a quelle finte. |
| `geo.Sky.Planets [0\|1]` | i pianeti a occhio nudo (Mercurio, Venere, Marte, Giove, Saturno); senza argomenti dice dove sono e quanto brillano. |
| `geo.Sky.StarBrightness <x>` | quanta luce fanno le stelle vere e i pianeti (1: una stella di magnitudine 0 come 1/50 del sole del livello). |
| `geo.Sky.LookAt <astro>` | gira la camera verso un astro: sole, luna, polare, mercurio, venere, marte, giove, saturno (nel Play). |

## La camera di volo

Solo in Play: velocita' proporzionale alla quota.

| Comando | Cosa fa |
|---|---|
| `geo.Fly` | Sostituisce la camera del Play con una che vola a velocita' proporzionale alla quota. |
| `geo.Fly.Speed <moltiplicatore>` | scala la velocita' calcolata dalla quota. |

## Entities

| Comando | Cosa fa |
|---|---|
| `geo.Entities.Listen [nome]` | in ascolto della memoria condivisa (default GeoWorldEntities): aerei, veicoli, navi da un simulatore, o da 'geoworld demo-entities'. |
| `geo.Entities.Stop` | Ferma tutte le sorgenti di entita' e le toglie dalla scena. |
| `geo.Entities.List [quante]` | le sorgenti e le entita' piu' vicine: tipo, modello, posizione, velocita', estrapolazione (default 20). |
| `geo.Entities.Blend <s>` | quanto dura il rientro morbido quando un aggiornamento non coincide con l'estrapolazione (0: salta subito; default 0,4). |
| `geo.Entities.Timeout <s>` | un'entita' che tace da tanto se ne va (default 5). |
| `geo.Entities.Radius <m> [max]` | fin dove un'entita' ha un attore, e quanti attori al massimo (i piu' vicini). Le altre restano nel registro. |
| `geo.Entities.Models [file\|default]` | la tabella dei modelli (tipo DIS o indice CIGI -&gt; forma o asset), ricaricata a caldo; senza argomenti la elenca. |
| `geo.Entities.Follow <nome\|numero\|off>` | la camera del Play segue un'entita', da dietro e da sopra (es. geo.Entities.Follow AEREO-1). |
| `geo.Entities.Labels [0\|1]` | il nome sopra ogni entita' (magenta, con il tipo, se la forma e' di ripiego). |
| `geo.Entities.Debug [0\|1]` | la velocita' (verde: dove sara' fra un secondo; rossa: notizie ferme), l'errore che il rientro sta smaltendo (giallo), la distanza nei nomi. |
| `geo.Entities.ModelFolder [cartella]` | la cartella dei modelli (anche fuori dal progetto): le regole 'file' della tabella partono da qui, le texture si cercano qui per nome. I modelli si rileggono. |
| `geo.Entities.ModelInfo <file>` | legge un modello (.flt, .obj, .gltf, .glb) e dice cosa c'e': misure, DOF e loro limiti, switch, animazioni, texture trovate e no, avvisi. Senza argomenti, i modelli gia' letti. |
| `geo.Entities.ReloadModels` | Rilegge dal disco tutti i modelli da file (dopo averli cambiati), e rifa' gli attori. |
| `geo.Entities.Dof <entita'> [dof] [imbardata] [beccheggio] [rollio] [x y z]` | muove una parte del modello (gradi di OpenFlight: imbardata positiva a SINISTRA; metri), nei limiti del modello. Senza dof: l'elenco di DOF, switch e animazioni. |
| `geo.Entities.Switch <entita'> <switch> <maschera>` | sceglie la maschera di uno switch del modello (es. il danno: 0 intatto, 1 distrutto; dipende dal modello). |
| `geo.Entities.Animation <entita'> [animazione\|stop\|reset] [loop\|once] [velocita']` | fa girare un'animazione del modello (glTF: per nome o numero; default in loop, velocita' 1; negativa: all'indietro). Senza animazione: l'elenco. |

## Lights

| Comando | Cosa fa |
|---|---|
| `geo.Lights.Enable <0\|1>` | accende e spegne i lampioni lungo le strade (il dataset delle strade resta aperto). |
| `geo.Lights.Radius <m> [pali m]` | fin dove si vedono i punti luce (default 8000) e i pali (default 700). |
| `geo.Lights.Count <n>` | quante luci vere (spot) sui lampioni accesi piu' vicini: fanno le pozze di luce (default 48, 0 nessuna). |
| `geo.Lights.Brightness <x>` | quanta luce fanno (1: al piede del palo 1/50 del sole del livello, venti lune piene). |
| `geo.Lights.Dimming <0..1>` | la riduzione del flusso nel cuore della notte, dalle 00:30 alle 04:30 (default 0,3: -30%). |
| `geo.Lights.Shadows <0\|1>` | le luci vere fanno ombra (costa: una mappa d'ombra per luce; default no). |
| `geo.Lights.Force <auto\|on\|off>` | auto: la fotocellula (dopo il tramonto); on: sempre accesi (per guardarli di giorno); off: sempre spenti. |
| `geo.Lights.Density <km/km2> [residenziali]` | quanto deve essere fitta la rete di strade di casa perche' una strada abbia i lampioni (default 3 le principali, 1 le residenziali). |
| `geo.Lights.Debug <0\|1>` | overlay dei lampioni: la luce, le celle (verdi con i pali, arancio solo punti luce), le luci vere (sfere gialle). |
| `geo.Lights.Stats` | I lampioni: accesi o spenti, quanti, la luce che c'e'. |
| `geo.Lights.Demo [HH:MM]` | porta il cielo alla sera (default 22:00), ferma l'orologio, accende i lampioni e l'overlay. Serve il dataset delle strade. |

## Comandi del motore utili

Non sono di GeoWorld, ma la documentazione li usa spesso.

| Comando | Cosa fa |
|---|---|
| `viewmode wireframe` | solo i triangoli, senza materiali; `viewmode lit` torna normale |
| `r.Fog 0` | spegne la nebbia (il terreno lontano sparisce nella nebbia); `r.Fog 1` la riaccende |
| `stat fps` | i fotogrammi al secondo |
| `stat unit` | quanto costano game thread, render thread e GPU, in millisecondi |
| `stat gpu` | il costo della GPU, voce per voce |
| `t.MaxFPS 60` | limita i fotogrammi al secondo (0 = nessun limite) |
| `r.ScreenPercentage 50` | disegna a meta' risoluzione: per capire se il limite e' la GPU |

## Dove stanno nel codice

| File | Comandi |
|---|---|
| `Plugins/GeoWorld/Source/GeoCore/Private/GeoCoreModule.cpp` | 10 |
| `Plugins/GeoWorld/Source/GeoEntities/Private/GeoEntitiesModule.cpp` | 16 |
| `Plugins/GeoWorld/Source/GeoRender/Private/GeoRenderModule.cpp` | 134 |
| `Plugins/GeoWorld/Source/GeoTiles/Private/GeoTilesModule.cpp` | 10 |
| `Plugins/GeoWorld/Source/GeoWorldEditor/Private/GeoTerrainMaterialFactory.cpp` | 1 |
| `Plugins/GeoWorld/Source/GeoWorldEditor/Private/GeoVegetationModelFactory.cpp` | 1 |

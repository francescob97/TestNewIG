# Il cielo — il giorno e la notte

> Come provarlo: `docs/cielo-verifica.md`. Il racconto per chi impara:
> `docs/manuale/18-cielo.md`. È il primo pezzo della Fase 12 (atmosfera,
> cielo e luce): il sole all'ora giusta, per vedere le ombre.

## 1. L'idea in breve

Un **orologio** (data e ora, che corre alla velocità scelta) e, a ogni frame,
**il sole e la luna visti dalla camera**, calcolati con gli algoritmi
dell'astronomia (Jean Meeus, *Astronomical Algorithms*). Con quelli:

```
 data, ora UT ──► Astronomy.h: sole, luna, fase, tempo siderale
                       │
       luogo della ────┤      DayLight.h: quanta luce c'è, notte, stelle, finestre
       camera          │
                       ▼
            UGeoSkySubsystem
              ├─ il SOLE del livello girato verso il sole vero (SkyAtmosphere fa il resto)
              ├─ la LUNA: una seconda luce direzionale, di notte, con la luce della sua fase
              ├─ l'ESPOSIZIONE: di notte qualche stop in meno (Post Process Volume)
              ├─ la NEBBIA: il suo colore scende con la luce
              ├─ MPC_GeoWeather: SkySun, SkyMoon, SkyEquatorX/Y/Z, SkyNight
              │     ├─ M_GeoBuilding: le finestre accese di notte
              │     └─ M_GeoNightSky: stelle, Via Lattea, luna con le fasi
              └─ la pioggia (unlit) e i fulmini si misurano sulla luce che c'è
```

| | |
|---|---|
| Sole | Meeus cap. 25, ~0,01°; con aberrazione, nutazione e rifrazione |
| Luna | Meeus cap. 47, i 120 termini più grandi (~10″); con la parallasse (fino a 1°) e la fase |
| Tempo | giorno giuliano, ΔT (TT − UT, ~69 s), tempo siderale apparente |
| Ora locale | l'ora dell'Europa centrale con l'ora legale (regola UE), o uno scostamento fisso |
| Levate e tramonti | ricerca numerica (campioni ogni 10 minuti e bisezione): sole, crepuscoli, luna; dice da sola quando non c'è tramonto |
| Luce | relativa al sole allo zenit: diretta + diffusa (massa d'aria), crepuscolo, luna (legge di Allen), fondo del cielo |
| Luna piena | 1/1000 del sole (dal vero 1/400.000: troppo buio per l'esposizione di un gioco) |
| Default | oggi alle 10:30 ora locale, tempo fermo |

## 2. Perché calcolare il sole (e non usare il plugin del motore)

Unreal ha un plugin, **Sun Position Calculator**, che dato luogo e ora gira
la luce del sole. Non lo usiamo, per tre ragioni:

1. il nostro mondo è **georeferenziato con il rebasing**: gli assi di Unreal
   sono il nord, l'est e l'alto *dell'origine*, che si sposta. Il sole va
   messo nel frame locale **della camera** e poi portato nel mondo con la
   rotazione della georeferenziazione;
2. serve anche la **luna** con la sua fase, il **tempo siderale** per le
   stelle, le levate e i tramonti per le stampe: tutto quello che il plugin
   non dà;
3. come il resto del progetto, il calcolo sta nello **strato puro** e si
   prova senza Unreal, con i numeri del libro.

### 2.1 Quanto è preciso, e come lo sappiamo

I test (`geosky_tests`) confrontano con gli esempi di Meeus:

| Prova | Atteso | Ottenuto |
|---|---|---|
| tempo siderale, 10 aprile 1987 (12.a, 12.b) | 197,693195° e 128,7378734° | uguali al 10⁻⁶ |
| sole, 13 ottobre 1992 (25.a) | AR 198,38083°, dec −7,78507° | 198,38051°, −7,78499° (1″) |
| argomenti e somme della luna, 12 aprile 1992 (47.a) | Σl −1127527, Σb −3229127, Σr −16590875 | identiche: i 120 termini sono giusti |
| luna (47.a) | AR 134,688470°, dec 13,768368°, 368.409,7 km | entro 0,00004° e 0,1 km |
| eclisse totale dell'8 aprile 2024, 18:17 UT, Messico | sole e luna sovrapposti | 0,007° di distanza (dal centro della Terra 0,35°: la parallasse) |
| luna piena del 23 aprile 2024 | illuminata 100% | 99,98% |
| Torino, 21 giugno 2024 | mezzogiorno vero 11:31 UT a 68,4°; giorno 15 h 38 min | uguale, levata e tramonto entro 20 s dalla formula chiusa |
| equatore all'equinozio | 12 h 7 min (rifrazione e raggio del sole) | 727 minuti |
| Tromsø | d'estate niente tramonto, d'inverno niente levata | così |

## 3. L'ora

- Gli astri si calcolano in **UT**; la rotazione della Terra nel UT, il moto
  di sole e luna nel **TT** (uniforme). ΔT = TT − UT: 63,8 s nel 2000, 69,2 s
  nel 2024, una retta fra i due.
- **L'ora locale**: per l'Italia l'ora dell'Europa centrale (UT+1) e l'ora
  legale (UT+2) dall'ultima domenica di marzo all'ultima di ottobre, alle
  01:00 UT. `geo.Sky.Zone 5.5` per uno scostamento fisso (qui niente ora
  legale: ogni paese ha la sua regola).
- **Il tempo che corre**: `geo.Sky.Speed`. 0 fermo (default: si lavora a
  un'ora fissa), 1 come l'orologio, 60 un minuto al secondo, 300 un giorno
  in meno di 5 minuti. Anche negativo.

## 4. La luce del sole: chi fa cosa

Con la **SkyAtmosphere** nel livello (c'è nel template OpenWorld), Unreal
calcola da solo la luce del sole attraverso l'aria: più rossa e debole al
tramonto, coperta dalla Terra quando va sotto, e il cielo del crepuscolo.
Quindi il cielo:

- **gira** la luce verso il sole vero, dove lo si vede (con la rifrazione:
  al tramonto mezzo grado più in alto di dove è);
- **non tocca l'intensità** finché il sole è sopra i −4°; fra −4° e −10° la
  spegne (per sicurezza, e perché la notte sia notte);
- **spegne le ombre del sole** sotto i −4° (con isteresi);
- la gira solo quando la direzione cambia di più di 0,01°: girare la luce
  rifà le mappe d'ombra (le Virtual Shadow Maps tengono in cache quello che
  non cambia). A velocità 1 è ogni ~2 secondi; di corsa, ogni frame.

Senza SkyAtmosphere l'intensità si spegne a mano sull'orizzonte, e il cielo
non si colora: l'overlay lo consiglia.

**L'intensità di giorno** è quella che la luce del livello aveva quando il
cielo l'ha presa (il livello è tarato su quella), oppure
`geo.Sky.SunLux`. Tutto il resto si misura su questa: la luna, le stelle, le
finestre, la pioggia.

**Le ombre lontane.** Con le Virtual Shadow Maps (default di UE5) le ombre ci
sono ovunque. Senza, una luce mobile ha le ombre a cascata fino a 200 m: il
cielo le porta a 3 km (`ShadowDistanceM` nelle impostazioni). Le ombre del
**terreno** restano una scelta (`geo.Terrain.Shadows 1`): costano, e le
ortofoto hanno già le loro; con il sole basso l'overlay lo ricorda.

## 5. La notte

### 5.1 Quanta luce c'è

`DayLight.h`, relativa al sole allo zenit:

| Momento | Luce | Dal vero |
|---|---|---|
| sole allo zenit | 1 | ~110.000 lux |
| sole a 30° | 0,47 | |
| tramonto (0°) | 0,004 | ~400 lux |
| −6° (fine del crepuscolo civile) | 5·10⁻⁵ | ~3 lux |
| luna piena alta | 10⁻³ (scelta) | 0,25 lux = 2,5·10⁻⁶ |
| primo quarto | 9·10⁻⁵ | (un decimo della piena: la legge di Allen) |
| senza luna | 3·10⁻⁵ | 0,002 lux |

**Perché la luna "sbagliata".** Dal vero fra il giorno e una notte di luna ci
sono 18 stop; l'esposizione automatica di un gioco ne recupera una decina,
poi l'immagine è nera. Con la luna a 1/1000 (10 stop) l'esposizione si
adatta, e un **Post Process Volume** del cielo, senza confini, abbassa
l'esposizione di 2 stop quando è notte (`geo.Sky.NightExposure`): la notte
resta scura e blu, si vede quanto basta.

### 5.2 La luna

Una seconda luce direzionale, creata dal cielo (transient: non finisce nel
livello salvato), **luce atmosferica indice 1**: illumina anche il cielo e
le nuvole. Accesa solo col sole sotto i −6° e la luna sopra l'orizzonte
(mai due luci con le ombre insieme), colore argento-blu, intensità secondo
la fase. Il suo disco dell'atmosfera è spento: la luna la disegna il cielo
notturno, con la fase.

### 5.3 Le stelle e la luna (M_GeoNightSky)

Una sfera di 100 km attorno alla camera, con un materiale **unlit,
additivo, a due facce**, senza nebbia (`NightSkyShader.inl`, a doppia
sintassi):

- **le stelle** sul cielo **equatoriale**: la rotazione dal cielo locale
  all'equatoriale (`HorizonToEquatorial`, girata nel mondo) arriva nella
  collezione, e il cielo gira attorno alla stella polare un giro al giorno
  siderale. Una griglia di celle di ~0,36°, una stella in una cella su 28,
  le luminosità con la statistica del cielo vero (tre volte più stelle a ogni
  magnitudine in più): ~5.000 stelle, non quelle del catalogo;
- **la Via Lattea**: una fascia sul piano della galassia (il polo
  galattico a AR 192,86°, dec +27,13°), più chiara verso il centro (il
  Sagittario), sbiadita dalla luna;
- **la luna**: una sfera illuminata dal sole. Per ogni punto del disco la
  normale della sfera e il prodotto scalare con la direzione del sole: le
  fasi vengono da sole, falce compresa, girata dalla parte giusta; un po' di
  luce della Terra sulla parte scura; i "mari" più scuri; un alone.
  Di giorno si vede lo stesso, pallida, come quella vera.

Le stelle compaiono col sole a −5° e ci sono tutte a −16°; le nuvole e la
pioggia le tolgono (la sfera è dietro le nuvole, ma il materiale additivo
non lo sa: si spengono con la copertura).

### 5.4 Le finestre accese

`BuildingShader.inl`, `GEO_BUILDING_LIGHTS`: lo stesso disegno delle
finestre del colore; ognuna ha un numero a caso (campata, piano, muro,
edificio) ed è accesa se il numero sta sotto la quota delle accese. La quota
viene dall'**ora locale** (tante la sera, poche alle quattro: `LitWindowShare`)
per quanto è buio (`CityLightsOn`). Quando la quota scende si spengono
sempre le stesse, una alla volta. Luce calda di lampadina, qualche finestra
fredda; ~1/250 del sole del livello.

### 5.5 Quello che Unreal non illumina

- **la pioggia, la neve, la grandine** sono particelle unlit: la loro
  luminosità segue la luce che c'è (`GetUnlitBrightness`, 1 col sole a 45°);
- **la nebbia** del livello ha un colore fisso: il cielo lo scala con la luce
  (se no, di notte, la nebbia "brilla");
- **i fulmini** si misuravano sulla luce del sole del livello: ora su quella
  che c'è (mai meno di 1/50 del giorno: di notte accecano).

### 5.6 Le stelle vere e i pianeti

Le stelle finte di 5.3 hanno la statistica giusta ma non le costellazioni.
Quelle **vere** vengono dal **catalogo di Yale** (Bright Star Catalogue, 5a
edizione: 9.110 stelle fino alla magnitudine 6,5, tutte quelle a occhio nudo;
dominio pubblico, distribuito dal CDS di Strasburgo). Il catalogo non sta nel
progetto: lo scarica la pipeline, una volta,

```
geoworld build-stars -o D:/GeoData/stars.gst
```

e lo riduce a un file di 109 KB (`.gst`: posizione J2000, magnitudine, B-V;
dalla più luminosa, `Pipeline/geoworld/starbuild.py`). In Unreal
`geo.Sky.Catalog D:/GeoData/stars.gst`, o Project Settings → GeoWorld - Cielo
→ Star Catalog per averlo a ogni avvio. Senza, restano le stelle finte.

| | |
|---|---|
| Dove sono | il catalogo è del 2000: la **precessione** (Meeus 21, rigorosa) le porta alla data. In 26 anni il punto vernale è scivolato di 0,37°, il polo di 0,15° |
| Come girano | le istanze stanno ferme in un componente attorno alla camera, a 150 km; si gira il componente (`SkyFieldRotation`: precessione, tempo siderale, latitudine, poi il mondo) |
| La trappola | il cielo equatoriale è destrorso, quello di Unreal (nord, est, alto) sinistrorso: il passaggio è una **riflessione**, che un quaternione non sa fare. Le stelle stanno in un cielo **specchiato** (y cambiata di segno): da lì al mondo è una rotazione vera (determinante +1, provato) |
| Quanto brillano | 2,512 volte per magnitudine; un punto di 1,5–7 pixel, più grande per le più luminose; la magnitudine 0 come 1/50 del sole del livello (`geo.Sky.StarBrightness`) |
| Quante | la **magnitudine limite**: 6 di notte, 4,5 a −12°, 1,5 alla fine del crepuscolo civile, −4,5 al tramonto (Venere); la luna piena alta toglie 1,2 |
| Il colore | dal B-V la temperatura (Ballesteros: Vega 10.100 K, il sole 5.800, Betelgeuse 3.300), poi il corpo nero, smorzato a metà verso il bianco |
| L'aria | 0,25 magnitudini per massa d'aria: a 2° sull'orizzonte quasi spente; la **scintillazione** vicino all'orizzonte (i pianeti no) |

**I pianeti** (Mercurio, Venere, Marte, Giove, Saturno) non hanno bisogno di
dati: si calcolano dagli **elementi orbitali approssimati del JPL**
(Standish, validi dal 1800 al 2050: pochi primi d'arco), con l'equazione di
Keplero, il tempo della luce, la magnitudine dell'Astronomical Almanac (Meeus
41) e, per Saturno, l'apertura degli anelli (Meeus 45). Provati su Venere di
Meeus 33.a (a 0,0003°), la grande congiunzione di Giove e Saturno del 21 dicembre
2020 (0,11°, vera 0,10), l'opposizione di Marte del 2020 (−2,6), Saturno con
gli anelli di taglio il 23 marzo 2025. Si rifanno ogni ora di cielo; ci sono
anche senza il catalogo (`geo.Sky.Planets`).

`MPC_GeoWeather` **SkyStars**: x 2 stelle vere e pianeti (le finte si
spengono), 1 solo i pianeti, 0 niente; y la magnitudine limite; z la luce;
w l'angolo di un pixel. `geo.Sky.LookAt venere` gira la camera verso un
astro.

## 6. Cosa tocca del livello, e come lo rimette

| Cosa | Come | Al ritorno (`geo.Sky.Enable 0`, fine del Play) |
|---|---|---|
| la luce del sole | rotazione, intensità, ombre, "Atmosphere Sun Light", distanza delle ombre, mobilità | tutto com'era |
| la nebbia | il colore | com'era |
| la luna, la sfera delle stelle, le stelle vere e i pianeti, il volume dell'esposizione | creati (transient) | distrutti |
| MPC_GeoWeather | SkyNight, SkyStars | niente stelle né finestre |

Nell'**editor** (fuori dal Play) il cielo lavora lo stesso: il sole del
livello si muove. Se si salva il livello, si salva la rotazione del momento
(l'intensità e il resto il cielo li rimette quando si chiude).

## 7. Limiti

- Le stelle vere ci sono con il catalogo (5.6); senza, restano quelle finte.
  Niente nutazione né aberrazione per le stelle e i pianeti (meno di 40″),
  niente moti propri (al più 2′ in 26 anni, per poche stelle).
- Niente Urano e Nettuno (al limite dell'occhio), niente comete né asteroidi;
  la luna non copre le stelle (le occultazioni).
- La luna è una sfera con dei "mari" finti, non la sua faccia.
- La sfera delle stelle sta davanti alle nuvole (un trasparente additivo):
  le stelle si spengono con la copertura delle nuvole, non nuvola per nuvola.
- Niente eclissi a schermo (la geometria c'è: i test la provano).
- Le luci delle strade sono arrivate (`docs/lampioni-design.md`); quelle delle
  auto no.
- Le brezze (di mare e di terra, di pendio, di valle) sono arrivate "con il
  sole": il vento legge l'orologio di questo cielo (`docs/vento-design.md`,
  sezione 2.6).

## 8. Alternative considerate

| Idea | Perché no |
|---|---|
| Plugin Sun Position Calculator | niente luna, niente stelle, non sa del rebasing (sezione 2) |
| Ruotare la luce a velocità costante (15° all'ora) | il sole non sorge a est ovunque e sempre: l'altezza a mezzogiorno cambia con le stagioni e la latitudine |
| Spegnere il sole al tramonto | il cielo del crepuscolo sparirebbe di colpo |
| Luna fisica (1/400.000) | 18 stop: l'esposizione automatica non ci arriva, notte nera |
| Le stelle del catalogo dipinte in una texture del cielo | a una texture manca la risoluzione: un punto di un pixel diventa sfocato o sfarfalla; un quadratino per stella (come i punti luce dei lampioni) resta nitido |
| Le stelle del catalogo nel progetto | 109 KB di dati in più da tenere in pari; la pipeline lo scarica in un secondo |
| I pianeti con VSOP87 (secondi d'arco) | migliaia di termini; gli elementi del JPL sbagliano di qualche primo, meno di un pixel di un pianeta visto a occhio |
| Stelle e luna con "Is Sky" sulla SkyAtmosphere | il comportamento cambia fra le versioni del motore; un trasparente additivo è prevedibile |

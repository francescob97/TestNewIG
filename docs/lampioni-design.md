# I lampioni — le luci delle strade

> Come provarli: `docs/lampioni-verifica.md`. Il racconto per chi impara:
> `docs/manuale/19-lampioni.md`. È il terzo pezzo della Fase 12 (atmosfera,
> cielo e luce), dopo il giorno e la notte (`docs/cielo-design.md`) e le
> brezze (`docs/vento-design.md`, 2.6).

## 1. L'idea in breve

Di notte una città vista dall'alto è fatta di **file di punti** arancioni e
bianchi lungo le strade; da vicino, di **pozze di luce** sull'asfalto. I
lampioni stanno lungo le strade che GeoWorld ha già (il dataset di
`geo.Roads.Open`): nessun dataset nuovo, nessun file da scaricare.

```
 tile delle strade (.gvt) ──► StreetLights.h: quali strade, dove i pali, che lampada
        │                                  │
 terreno disegnato ───────────► BuildCellLamps: i lampioni della cella, sul suolo
                                           │
 il cielo (ora, sole) ──► la luce che vede una fotocellula ──► MPC_GeoWeather StreetLights
 le nuvole                                 │
                                           ▼
                           UGeoStreetLightsSubsystem
                             ├─ PUNTI LUCE: un quadratino per lampione, aperto verso la
                             │   camera (M_GeoStreetLight), fino a 8 km
                             ├─ PALI: tre forme con il vetro acceso (M_GeoStreetLamp), fino a 700 m
                             └─ LUCI VERE: 48 spot sui lampioni accesi più vicini
```

| | |
|---|---|
| Da dove | le strade del dataset vettoriale; il tag `lit` di OSM se c'è (nel record della linea) |
| Quali strade | il tag, o la classe e quanto è abitato attorno (la rete di strade di casa) |
| Dove | lungo la strada, a passo fisso (3,5 volte l'altezza), linee ricucite, mai su un'altra carreggiata |
| Quando | una fotocellula per circuito (~300 m), soglia 15–60 lux: 14–27 minuti dopo il tramonto |
| Quanta luce | al piede di un palo 1/50 del sole del livello: venti volte la luna piena |
| Colore | sodio o LED per comune (tre su quattro LED): neutro sulle principali, caldo nei quartieri |
| Di notte | il flusso scende del 30% fra le 00:30 e le 04:30 |

## 2. Da dove vengono: il tag `lit`

OpenStreetMap ha un tag per l'illuminazione: `lit=yes`, `lit=no`, e anche
orari (`sunset-sunrise`, `18:00-01:00`) o `automatic` (con il sensore). La
pipeline (`roadclasses.py`, `lit_state`) lo riduce a tre valori e lo scrive in
un byte del record della linea (`.gvt`):

| Byte | Vuol dire | Da |
|---|---|---|
| 0 | non si sa | il tag manca (quasi sempre) |
| 1 | lampioni | `yes`, gli orari, `automatic`, `limited`, `interval`, … |
| 2 | buio | `no`, `disused` |

Il byte era **riservato** e sempre zero: le tile fatte prima si leggono uguali,
con "non si sa", e non serve rifare il dataset. Rifarlo (la classificazione
passa alla versione 3) porta il tag dove c'è. In Italia il tag c'è su una
parte delle strade di città; quasi mai in campagna.

## 3. Quali strade

Dove il tag manca (quasi sempre) decide la **classe**, insieme a **quanto è
abitato attorno**: la lunghezza delle strade residenziali, pedonali e "living
street" entro ~250 m, divisa per l'area, in km per km². In un paese sono
5–20 (il paese di prova dei test: 15,5), in campagna meno di 1.

| Classe | Tipo | Altezza | Passo | Lampioni se |
|---|---|---|---|---|
| autostrada, superstrada | sbraccio | 12 m | 42 m | il tag; le rampe in città |
| principale, secondaria | sbraccio | 10 m | 34 m | in città (≥ 3 km/km²) |
| terziaria, non classificata | sbraccio | 9 m | 30 m | in città |
| residenziale | palo | 7,5 m | 26 m | appena c'è una frazione (≥ 1 km/km²) |
| di servizio | palo | 7 m | 28 m | in città |
| living street, pedonale | lanterna | 4,25 m | 18 m | appena c'è una frazione |
| sentiero, tratturo | lanterna, palo | 4, 6 m | 22, 30 m | solo con il tag |
| ferrovie, piste | — | — | — | mai |

- **La densità si guarda lampione per lampione**, non per strada: una
  provinciale lunga chilometri è illuminata nel paese e buia in campagna (nei
  test: 40 lampioni in paese, 0 in campagna).
- **Sterrato senza tag**: buio. **Ponti**: niente (vedi i limiti).
- Una stradina residenziale isolata di 100 m in campagna (0,4 km/km²) resta
  buia; una frazione con le sue vie no.

## 4. Dove

**Le linee ricucite.** In OSM una via è spesso a pezzi: un incrocio, un cambio
di nome, un tratto a senso unico. Camminare pezzo per pezzo metterebbe un
lampione all'inizio di ognuno. Prima si **ricuciono** le linee che si toccano
solo fra loro due, con lo stesso stile (classe, larghezza, disposizione, tag):
`FChain`. Si parte dai capi liberi, nel verso della prima linea, così i sensi
unici e le carreggiate delle autostrade restano nel loro verso (il lato destro
è quello esterno). Le rotonde (catene chiuse) dopo. Nei test una via spezzata
in 10 pezzi da 15 m ha 5 lampioni a 26 m, non 10 a 15 m.

**Il passo.** Il primo lampione a **mezzo passo** dall'inizio, poi uno ogni
passo. Fra due tratti (o due tile) ci sono sempre fra mezzo passo e un passo e
mezzo: mai due pali attaccati, mai un buco più grande. Ogni tile mette solo i
lampioni dei **tratti dentro di sé** (le linee escono di un buffer di un
ottavo): sul confine nessun doppione (provato con due tile vicine: passo fra
25,2 e 26 m).

**I lati.** Sulle strade strette (≤ 9 m) tutti a destra nel verso della linea;
sulle medie (≤ 15 m) alternati; sulle larghe a coppie, uno di fronte
all'altro. Le autostrade un lato (sono una linea per carreggiata).

**Il palo** sul marciapiede, a mezzo metro dal cordolo (le residenziali lo
hanno di default, come le strade 3D di `RoadMesh.h`), a 0,8 m dal ciglio
senza; a 1,5 m sulle autostrade. Le lanterne sul bordo della zona pedonale, o
accanto al sentiero. Lo **sbraccio guarda la strada**.

**Mai su un'altra carreggiata.** Agli incroci il palo di una via cadrebbe
sull'altra: una griglia delle strade della tile (`FObstacleGrid`, celle da
40 m) lo dice in fretta, e il lampione si salta. I sentieri non contano: un
marciapiede mappato a parte è proprio dove va il palo. **Gli angoli**: due vie
ricucite girano di 90 gradi, e il palo dell'ultimo lampione starebbe
sull'altro braccio della stessa catena. Si guarda anche la catena, tranne il
pezzo attorno al lampione (`OwnBlocked`): su una curva dolce la distanza resta
quella del palo, su un angolo scende.

## 5. Quando si accendono

Una **fotocellula** misura la luce e accende sotto una soglia. Le fotocellule
vere stanno fra 15 e 70 lux; qui fra 15 e 60, una per **circuito** (una cella
di ~300 m: una via si accende tutta insieme), distribuite in logaritmo.

| | |
|---|---|
| La luce che vede | `110.000 lux × SunlightShare(altezza del sole)` (`DayLight.h`, la stessa del cielo): 440 lux al tramonto, ~50 a −3°; con il cielo coperto un quarto |
| Si accende | sotto la soglia; spento sopra il 15% in più; in mezzo una rampa (la lampada che si scalda) |
| Torino, 21 giugno | tramonto alle 21:33; il circuito da 60 lux si accende 14 minuti dopo, quello da 15 lux 27 minuti dopo; con il cielo coperto subito |
| La mattina | si spegne a una luce 1,6 volte più alta: l'isteresi delle fotocellule (i lampioni non lampeggiano all'alba) |
| Nel cuore della notte | il flusso scende del 30% fra le 00:30 e le 04:30, con un'ora di rampa ai due capi (`geo.Lights.Dimming`) |

La formula dell'accensione è scritta **due volte, uguale**: in C++
(`LampOnLevel`, per le luci vere) e nel materiale (`StreetLightShader.inl`,
per i punti luce e il vetro). Un test compila lo shader come C++ e controlla
che diano lo stesso numero, la sera e la mattina.

## 6. Quanta luce, che colore

**Non la luce vera.** Come la luna (`docs/cielo-design.md`, sezione 4) anche i
lampioni sono in proporzione al **sole del livello**: al piede di un palo di
riferimento 1/50 del sole del livello, **venti volte la luna piena** di
GeoWorld. Dal vero sarebbero ~60 volte (15 lux contro 0,25), ma la luna di
GeoWorld è già 400 volte quella vera (per l'esposizione automatica): con il
rapporto vero le strade sarebbero bianche. `geo.Lights.Brightness` lo cambia.

| Lampada | Colore (lineare) | Dove |
|---|---|---|
| sodio ad alta pressione | (1, 0,52, 0,13): arancione | nei comuni non ancora passati ai LED |
| LED caldo, 3000 K | (1, 0,71, 0,42) | quartieri, zone pedonali |
| LED neutro, 4000 K | (1, 0,82, 0,64) | strade principali |

Tutti normalizzati alla stessa luminanza: il colore cambia la tinta, non
quanta luce c'è. Un **comune** (una cella di ~2 km) è passato ai LED o no: tre
su quattro sì (nei test, su 3600 celle: 74,6%). Quanta luce fa ogni classe:
1,2 le autostrade, 1 le principali, 0,7 le residenziali, 0,5 le pedonali.

## 7. In Unreal

**Le celle.** Le tile del livello 14 (~1,2 km × 0,9 km a 45°) attorno alla
camera, come gli alberi e gli edifici (`Mesh/GroundCells.h`): due bande, i
pali fino a 700 m, i punti luce fino a 8 km. Il lavoro di una cella
(`PlaceTileLamps` sulla sua tile delle strade, poi `BuildCellLamps` sul
terreno disegnato) gira su un worker: ~1,6 ms per una tile di città fitta
(4.500 lampioni). Una cella lontana si rifà per il terreno al più ogni 10 s:
i suoi punti luce sono avvicinati alla camera, l'errore non si vede.

**I punti luce** (`M_GeoStreetLight`). Un quadratino di 5 cm per lampione,
nella luce, in un componente di istanze per cella. Il materiale lo **apre
verso la camera** (World Position Offset), largo 80 cm da vicino e mai meno di
2,5 pixel da lontano: un lampione a 8 km è ancora un puntino. Lo avvicina alla
camera di due "punti" (2,5 pixel di distanza): il terreno lontano è disegnato
grossolano, e senza la luce ci finirebbe dentro. Additivo: aggiunge luce, non
copre. Un nucleo e un alone; da lontano più debole per pixel (la luce di un
lampione si sparge su un puntino più grande di lui); a pochi metri sparisce,
perché c'è il vetro del palo. Di giorno i componenti si **nascondono** (neri e
additivi non si vedrebbero, ma si disegnerebbero).

**I pali** (`M_GeoStreetLamp`). Le tre forme di `BuildLampMesh` (54–60
triangoli, un LOD da lontano), scalate all'altezza del lampione, girate verso
la strada; il vetro (il colore del vertice, alfa 1) si accende con la stessa
formula. Spariscono oltre il raggio dei pali (le distanze di taglio del
componente).

**Le luci vere.** 48 spot di Unreal, verso il basso, sui lampioni **accesi**
più vicini alla camera (`SelectLights`): fanno le pozze di luce sull'asfalto,
sui muri, sugli alberi. Il peso è pieno per i primi tre quarti della lista e
scende a zero in fondo, e ogni luce sfuma in 0,4 s: un lampione entra ed esce
dalla lista dove pesa quasi niente, e la luce non scatta mentre la camera si
muove. L'intensità in **candele**: al piede del palo, H metri sotto,
`candele / H²` lux. Senza ombre (ognuna costa una mappa d'ombra:
`geo.Lights.Shadows 1`).

**MPC_GeoWeather.** Due parametri nuovi:

| Parametro | x | y | z | w |
|---|---|---|---|---|
| `StreetLights` | la luce che c'è (lux veri) | 1 la sera, 1,6 la mattina | la luce al piede (unità della scena) | la riduzione del flusso; 0 = spenti |
| `StreetLightView` | un pixel, radianti | pixel minimi (2,5) | lato da vicino (0,8 m) | avvicinamento (2 punti) |

**L'ora e la luce** vengono dal cielo (`UGeoSkySubsystem`); senza, dall'orologio
vero sul punto della camera. Le nuvole da `UGeoWeatherSubsystem`.

## 8. Comandi

| Comando | Cosa fa |
|---|---|
| `geo.Lights.Demo [HH:MM]` | il cielo alla sera (22:00), orologio fermo, lampioni e overlay accesi |
| `geo.Lights.Enable <0\|1>` | accende e spegne i lampioni |
| `geo.Lights.Force <auto\|on\|off>` | la fotocellula, o sempre accesi (per guardarli di giorno), o sempre spenti |
| `geo.Lights.Radius <m> [pali m]` | fin dove i punti luce (8000) e i pali (700) |
| `geo.Lights.Count <n>` | quante luci vere (48; 0 nessuna) |
| `geo.Lights.Brightness <x>` | quanta luce (1) |
| `geo.Lights.Dimming <0..1>` | la riduzione del flusso di notte (0,3) |
| `geo.Lights.Shadows <0\|1>` | le luci vere fanno ombra |
| `geo.Lights.Density <km/km²> [residenziali]` | da quanta rete di casa una strada ha i lampioni (3 e 1) |
| `geo.Lights.Debug <0\|1>` | l'overlay: la luce, le celle, le luci vere |
| `geo.Lights.Stats` | accesi o spenti, quanti |

Nelle stampe a destra, la riga **"Lampioni"** quando sono accesi.

## 9. Limiti

- **Sui ponti niente.** Il terreno non sa che c'è un ponte: la luce starebbe
  in fondo alla valle. Lo stesso per le linee sopraelevate (layer > 0).
- **Le gallerie** non ci sono proprio (la pipeline le scarta).
- **Le luci delle auto, dei semafori, delle insegne, delle piste** degli
  aeroporti: non ci sono.
- **Le pozze di luce solo vicino**: oltre le 48 luci vere l'asfalto non è
  illuminato, si vedono solo i punti. Con le **MegaLights** di Unreal (5.5 e
  poi) se ne possono accendere centinaia: `geo.Lights.Count 300`.
- **I lampioni non sanno degli edifici**: in una via stretta di centro storico
  il palo può finire dentro il muro di una casa (le lanterne vere, lì, stanno
  appese ai muri).
- **Le rotonde e le piazze** disegnate come aree (non come linee) non hanno
  lampioni: la pipeline non le porta.
- **La densità si misura sulla rete di strade**, non sugli edifici: un
  quartiere industriale senza strade residenziali ha le principali buie.

## 10. Alternative considerate

| Idea | Perché no |
|---|---|
| Una luce vera per ogni lampione | migliaia di luci: anche con le MegaLights troppe per una città vista dall'alto |
| Le pozze di luce come decal | un componente per lampione: migliaia, sul game thread |
| Le strade illuminate dipinte nella texture delle strade | una seconda texture per tile di terreno, e la luce cambierebbe con il livello di dettaglio |
| Lampioni dal tag `lit` soltanto | quasi tutte le strade di campagna e metà di quelle di città non lo hanno: una città quasi buia |
| Lampioni dalla copertura del suolo (ESA "costruito") | un secondo dataset da aprire; la rete delle strade dice già dove si abita |
| Punti luce fatti con Niagara | un sistema in più da configurare; un componente di istanze con un materiale basta, e lo shader si prova come C++ |
| Il passo dal punto più vicino di una griglia globale | i lampioni non sarebbero equidistanti lungo una strada obliqua |

## 11. Il codice

| File | Cosa c'è |
|---|---|
| `Source/GeoRender/Public/Roads/StreetLights.h` | tutto il puro: quali strade (`LampStyleFor`, `FUrbanDensity`), le catene (`BuildChains`), gli ostacoli (`FObstacleGrid`, `OwnBlocked`), i lampioni di una tile (`PlaceTileLamps`) e di una cella (`BuildCellLamps`), l'accensione (`AmbientLux`, `LampOnLevel`, `IsMorning`, `NightDimming`), la luce (`FootLux`, `SpotFor`), le luci vere (`SelectLights`), le forme (`BuildLampMesh`, `BuildSpriteQuad`) |
| `Source/GeoRender/Public/Roads/StreetLightShader.inl` | lo shader dei punti luce e del vetro, HLSL e C++ insieme |
| `Source/GeoRender/Public/Roads/GeoStreetLightsSubsystem.h`, `Private/Roads/GeoStreetLightsSubsystem.cpp` | Unreal: le celle, i componenti, le luci vere, MPC, l'overlay |
| `Source/GeoWorldEditor/Private/GeoTerrainMaterialFactory.cpp` | `M_GeoStreetLight`, `M_GeoStreetLamp`, i due parametri di `MPC_GeoWeather` |
| `Source/GeoTiles/Public/Tiles/VectorClasses.h`, `VectorTileFormat.h` | `RoadLit*`, `FVectorFeature::Lit` |
| `Pipeline/geoworld/roadclasses.py`, `vectorformat.py`, `vectorbuild.py` | il tag `lit` fino alla tile |
| `Tools/StandaloneTests/georoads_main.cpp` (sezioni 13, 14) | 55 controlli: quali strade, il passo, i lati, gli incroci, le catene, il confine fra tile, l'accensione a Torino, le celle, le forme, lo shader |
| `Pipeline/tests/test_roads.py` | il tag `lit`: i valori, il byte, il viaggio dalla OSM alla tile |

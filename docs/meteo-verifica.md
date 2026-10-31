# Il meteo — come provarlo

> Le decisioni sono in `docs/meteo-design.md`; il racconto per chi impara in
> `docs/manuale/15-meteo.md`.

**Cosa è già provato qui, senza Unreal** (`geoweather_tests`, sezioni 6–10,
24 test sulla precipitazione):

- cosa cade per quota: con 9,75 °C al mare lo zero termico è a 1500 m e la
  quota neve a 1200 m; pioggia in valle, metà e metà alla quota neve, neve in
  cima; neve e pioggia imposte valgono a ogni temperatura; sopra la cima delle
  nuvole (6 km, 10 per i rovesci) non cade niente;
- i rovesci: chiesta una copertura del 20, 50, 80%, piove su quella frazione di
  suolo, e nei nuclei si arriva al massimo; viaggiano con il vento in quota; la
  grandine cade nei nuclei (60%) e mai ai margini;
- visibilità e caduta: pioggia debole 13,5 km, a dirotto 1 km, 1 mm/h di neve
  1 km; gocce a 4–7 m/s, fiocchi a ~1, un chicco da 1 cm a 14,5 m/s, da 2 cm a 20,5;
- il suolo: bagnato dopo 5 minuti di pioggia, asciutto due ore dopo; la neve
  scende alla quota neve in un'ora e risale fondendo dove fa più di zero; sotto
  zero non fonde;
- le particelle: ogni tipo alla sua velocità, il vento porta di più i fiocchi,
  la camera che si muove non le trascina, nessun salto fra un frame e l'altro;
  la mesh (4 vertici nello stesso punto, tutti nella scatola).

**I fulmini** (sezioni 11–13, 19 test):

- dove e quanti: in un'ora, quanti la densità nei nuclei promette (5.806
  contro 5.951 attesi); tutti nei nuclei e entro 30 km; uno su tre al suolo;
  attività 3 il triplo; un fronte (pioggia estesa) fulmina poco; niente senza
  pioggia o con attività zero; stesso seme, stessi fulmini;
- il lampo: un picco per ogni colpo di ritorno, buio prima e dopo; in media 3
  colpi, a 30–90 ms l'uno dall'altro;
- il canale: un filo continuo dalla nube al punto colpito, rami più deboli che
  si fermano a mezz'aria, la cima che sfuma nella nube, stesso seme stessa
  forma;
- il tuono: arriva dopo distanza / velocità del suono (10,1 s a 3,45 km); da
  vicino acuto, da lontano basso (quattro volte meno passaggi per lo zero); il
  rombo dura quanto la differenza di distanza fra i pezzi del canale (8 s a
  300 m); il volume cala con la distanza, oltre 25 km silenzio.

**Le nuvole** (sezioni 14–15, 15 test). La sezione 15 compila come C++ **lo
stesso testo** dello shader che va nel materiale (`Weather/CloudShader.inl`):

- la mappa: dove piove la nuvola c'è sempre, piena; il cielo nuvoloso è più
  di 1,3 volte quello che piove; le cime alte nei nuclei, basse ai margini; un
  fronte è tutto coperto; righe verso nord e colonne verso est, R copertura, G
  cima, B pioggia;
- sopra un temporale: nuvola piena fra la base (975 m) e la cima, niente sotto
  la base e sopra i 10 km; le nubi di pioggia più scure; la cima che dicono le
  stampe è quella che disegna lo shader;
- il vento: 10 minuti a 15 m/s, la stessa nuvola 9 km più a nord;
- niente salti: un rebase di 10 km cambia la densità di 2 decimillesimi; il
  disegno riportato di 1.500 km o di un giorno, di 6;
- i cumuli: chiesti 10, 25, 50, 75% del cielo, misurati 9, 24, 48, 75%; base
  piatta (niente sotto), cima entro lo spessore;
- il lampo accende la nuvola attorno, non quella a 12 km.

**Il ghiaccio e la grandine al suolo** (sezioni 16–18, 13 test). La sezione 18
compila come C++ il testo del nodo "Meteo sul suolo" (`Weather/GroundShader.inl`):

- il ghiaccio: il suolo bagnato gela sopra lo zero termico (più di metà in 10
  minuti); il gelicidio (pioggia imposta con il gelo) vetra tutto in un quarto
  d'ora; senz'acqua, o quando nevica, niente ghiaccio; al disgelo la quota del
  ghiaccio segue lo zero termico in un quarto d'ora, e sopra il ghiaccio resta;
- la grandine posata: è quella che cade (la stessa mappa); venti minuti di
  temporale che viaggia verso nord lasciano bianco il 10% del suolo, in strisce
  allungate verso nord; la pioggia non lascia niente; a 20 °C fonde in meno di
  un'ora e mezza, sotto zero resta; ricentrare la griglia non sposta la
  grandine di un decimo di millimetro;
- il materiale: con il sereno colore e ruvidità restano identici; la neve sopra
  la copertura e non sui pendii ripidi; il ghiaccio lucidissimo (ruvidità
  0,05) sopra la sua quota e invisibile sotto la neve; la grandine bianca dove
  la mappa dice, da vicino a chicchi con la stessa copertura media, e al suo
  posto anche 300 km più a nord del primo centro.

Più i formati delle stampe (`geocore_tests`, sezione 10): "pioggia 4,2 mm/h",
"neve e pioggia 1,0 mm/h", "350 m", "1,2 km".

**Cosa non ho potuto provare:** la compilazione in Unreal, i materiali, le
particelle, la nebbia, i fulmini a schermo, il suono del tuono, le nuvole a
schermo (la VolumetricCloud, `M_GeoClouds`), il ghiaccio e la grandine sul
terreno (la texture aggiornata sul posto, il nodo "Meteo sul suolo"). I controlli statici (sezione 4 di
`docs/consegna.md`) sono verdi.

---

## 1. Aggiorna, compila, rifai i materiali

`git pull`, compila (*Development Editor*), apri l'editor e lancia **una volta**:

```
geo.Imagery.CreateMaterial
```

Crea `MPC_GeoWeather`, `T_GeoWeatherDefault`, `M_GeoPrecipitation`,
`M_GeoLightning` e `M_GeoClouds` in `/GeoWorld/Materials`, e
**rifà** `M_GeoTerrain` e `M_GeoRoad` con il nodo "Meteo sul suolo". Deve
dire in verde tutti i materiali; un giallo "manca MPC_GeoWeather" vuol dire
che la collezione non si è creata (mandami il log).

Poi salva gli asset (*File → Save All*): sono asset del plugin.

## 2. Il sereno, prima

Apri un terreno come sempre (con l'acqua, se ce l'hai: serve il geoide per la
quota sul mare). Nelle stampe a destra, sotto il vento, ci sono due righe
nuove:

| Riga | Esempio | Cosa vuol dire |
|---|---|---|
| Meteo | `niente` | cosa cade alla tua quota |
| Temperatura | `13,4 °C, zero termico 2.308 m` | l'aria alla tua quota, e dove fa 0 °C |

Con il sereno non cade niente e la nebbia non si tocca. **Le nuvole sì**: di
default ci sono i cumuli di bel tempo, un quarto del cielo, con la base a
1.500 m (sezione 7). `geo.Weather.Clouds 0` li toglie, `geo.Weather.Clouds off`
spegne del tutto le nuvole di GeoWorld.

## 3. La pioggia

```
geo.Weather.Rain 4
geo.Weather.Debug 1
```

Scendi a qualche decina di metri dal suolo.

1. **Le gocce**: strisce chiare e sottili che cadono, inclinate dal vento
   (`geo.Weather.Wind 15 270`: molto inclinate, verso est). Girando la camera
   restano ferme nel mondo; muovendoti ti scorrono addosso.
2. **Le stampe**: `Meteo  pioggia 3,4 mm/h` (fra 2 e 4: 4 è il cuore, e la
   precipitazione non è uniforme), poi `Visibilita'` intorno ai 6 km.
3. **Il suolo**: in un paio di minuti il terreno si **scurisce** e diventa
   **lucido** (riflessi del cielo e del sole sulle strade). La riga `Suolo`
   dice `bagnato 100%`.
4. **La nebbia** (in Play: nell'editor la nebbia del livello non si tocca,
   sezione 7): l'orizzonte si chiude un po'. Con `geo.Weather.Rain 50` (a
   dirotto) la visibilità scende a ~1 km e le gocce diventano fitte.
5. `geo.Weather.Clear`: le gocce spariscono, la nebbia torna com'era in pochi
   secondi; il suolo resta bagnato e si asciuga in 40 minuti
   (`geo.Weather.Wetness 0` per asciugarlo subito).

L'overlay (in alto a sinistra, sotto quello del vento) dice il tempo impostato,
la temperatura con zero termico, quota neve e cima delle nuvole, il suolo,
cosa cade qui e al suolo, le particelle (quante per tipo, a che velocità
cadono) e la visibilità con la nebbia usata.

## 4. La neve, e la quota

```
geo.Weather.Snow 1
```

Fiocchi bianchi, lenti, che ondeggiano; la visibilità scende a ~1 km. In una
decina di minuti il suolo **si imbianca**, prima i prati e i campi (chiari),
poi i boschi; i pendii ripidi restano scuri. La riga `Suolo` dice `neve sopra
...` con una quota che scende.

Adesso la cosa interessante: **pioggia in valle, neve in montagna**, con lo
stesso tempo.

```
geo.Weather.Clear
geo.Weather.SnowCover off
geo.Weather.Temperature 8
geo.Weather.Precipitation 3
```

Con 8 °C al mare lo zero termico è a 1.231 m e la quota neve a ~930 m.

- In pianura (Torino, 240 m): **pioggia**.
- Sali (o `geo.Goto` su una montagna): verso 800–1.100 m le gocce si mescolano
  ai fiocchi (`Meteo  pioggia e neve ...`, poi `neve e pioggia ...`), sopra
  solo **neve**.
- Col tempo, sul suolo la neve si posa da ~930 m in su: da lontano si vede la
  **linea della neve** sulle montagne, più bassa sui versanti dolci.
- `geo.Weather.Temperature 2`: la quota neve scende quasi al mare, nevica anche
  in pianura.
- `geo.Weather.Clear` con `geo.Weather.Temperature 15`: la neve fonde,
  la linea risale piano (20 m all'ora per grado).

`geo.Weather.SnowCover 1500` mette subito la neve sopra 1.500 m, senza
aspettare.

## 5. La grandine

```
geo.Weather.Hail
```

Temporali sparsi (40% del suolo, celle da 6 km) che **viaggiano** con il vento
in quota: guardando dall'alto (2–3 km) vedi la nebbia più fitta sotto i
temporali spostarsi. Sotto un temporale: pioggia forte e, **nei nuclei**,
chicchi bianchi che cadono molto più veloci delle gocce. Ai margini solo
pioggia. Se non sei sotto un temporale (`Meteo  niente`), aspetta o spostati:
i rovesci passano.

`geo.Weather.Hail 30 1` mette la grandine ovunque ci sia un nucleo, senza buchi
fra i temporali.

La grandine **si posa** (sezione 8): dopo una decina di minuti sotto un nucleo
il suolo diventa bianco, in strisce dove i nuclei sono passati.

## 6. I fulmini

Il modo più rapido per vederne uno, senza aspettare un temporale (in **Play**,
per sentire anche il tuono):

```
geo.Weather.Strike 1500
```

Un fulmine nube-suolo a 1,5 km davanti alla camera:

1. **il canale**: un filo bianco-azzurro, tortuoso, con qualche ramo che si
   ferma a mezz'aria; in alto entra nella base della nuvola (con
   `geo.Weather.Storm` la nube di pioggia c'è; con il sereno sfuma nel nulla);
2. **sfarfalla**: due, tre, quattro lampi in un terzo di secondo (i colpi di
   ritorno), poi buio;
3. **il paesaggio si illumina** attorno al punto colpito, per un attimo;
4. **il tuono**, dopo ~4,4 s (tre secondi per chilometro): uno schiocco secco
   e un brontolio. La riga `Fulmine` delle stampe conta i secondi che mancano.

Poi prova le distanze:

- `geo.Weather.Strike 200`: il tuono arriva quasi subito, fortissimo, ed è
  **lungo** (il canale sopra di te è a distanze molto diverse);
- `geo.Weather.Strike 8000`: un lampo lontano, il tuono dopo ~24 s, basso e
  piano;
- `geo.Weather.Strike 3000 nube`: niente canale, si illumina tutto il cielo
  (un lampo dentro la nube).

E un temporale vero:

```
geo.Weather.Storm
geo.Weather.Debug 1
```

Rovesci sparsi (35% del suolo) che viaggiano con il vento in quota, e i
fulmini nei loro nuclei: l'overlay dice quanti nell'ultimo minuto (qualche
decina entro 30 km, se ci sono dei nuclei vicini) e l'ultimo, a che distanza.
Sotto un nucleo, fulmini vicini e tuoni forti; ai margini, lampi lontani.
`geo.Weather.Lightning 3` per un temporale forte; `geo.Weather.Thunder 0.5`
per abbassare il tuono; `geo.Weather.Clear` per finire.

## 7. Le nuvole

Dopo `geo.Imagery.CreateMaterial`, in **Play**, con il sereno:

```
geo.Weather.Debug 1
```

1. **i cumuli**: un quarto del cielo, chiazze di un chilometro e mezzo con la
   **base piatta** (tutte alla stessa quota, 1.500 m) e la cima a cupola, i
   bordi sfrangiati. Si muovono piano con il vento in quota e in mezz'ora
   cambiano forma;
2. **l'overlay**, nella parte "le nuvole": i cumuli impostati, la
   VolumetricCloud usata ("nostra"), lo strato, e quanti secondi ha la mappa
   meteo (si rifà ogni 8 s);
3. **in giallo, se c'è**: cosa manca nella scena. "Atmosphere Sun Light"
   spenta sulla luce del sole vuol dire nuvole scure; "Cast Cloud Shadows"
   spenta vuol dire niente ombre sul terreno. Sono due caselle della
   *Directional Light* del livello, nella categoria *Atmosphere and Cloud*.

Poi:

- `geo.Weather.Clouds 0.6`: più cumuli; `geo.Weather.Clouds 0.6 800 2500`:
  bassi e alti (cumuli congesti);
- **volaci dentro**: sali a 1.800 m sopra un cumulo e scendi. Dentro si vede
  a ~130 m; sotto la base il cielo è libero;
- `geo.Weather.Rain`: un fronte, **cielo coperto** dalla base delle nubi di
  pioggia (975 m) fino a 3–6 km, più scuro dei cumuli;
- `geo.Weather.Storm`: le **torri** sopra i rovesci, alte fino a 10 km nei
  nuclei; dove piove la nuvola c'è sempre. Da 3 km di quota, guardando
  lontano, si vedono le torri isolate;
- `geo.Weather.Strike 3000 nube`, **di sera** (abbassa il sole): la nuvola si
  accende da dentro, per un attimo;
- `geo.Weather.Strike 1500`: il canale esce dalla **base** della nuvola;
- `geo.Weather.CloudDensity 2`: più fitte e più scure; `0.5`, più leggere;
- `geo.Weather.Clouds off`: niente nuvole (se il livello ha una sua
  VolumetricCloud, torna quella).

Nelle stampe a destra c'è la riga **Nuvole**: i cumuli e, se c'è, la nube di
pioggia sopra di te, dalla base alla cima.

## 8. Il ghiaccio e la grandine sul terreno

Servono le ortofoto (`geo.Imagery.Demo`): senza, il terreno ha il materiale
grigio del motore e niente meteo sul suolo.

**Il ghiaccio, subito:**

```
geo.Weather.Ice 1 0
geo.Weather.Debug 1
```

Tutto il terreno sopra 0 m diventa un velo **lucidissimo**, un po' più scuro e
appena azzurro: guarda contro il sole, che ci si specchia. Le stampe dicono
`Suolo  asciutto, ghiaccio sopra 0 m`. `geo.Weather.Ice 1 800`: solo sopra
800 m (in valle il terreno resta com'era, in montagna è vetrato).
`geo.Weather.Ice off` lo toglie.

**Il ghiaccio come viene davvero:**

1. `geo.Weather.Rain 3` per qualche minuto: il suolo si bagna;
2. `geo.Weather.Clear` e poi `geo.Weather.Temperature 3`: lo zero termico
   scende a ~460 m; in una decina di minuti, sopra quella quota, il bagnato
   diventa ghiaccio (l'overlay, riga Suolo, dice quanto e sopra che quota);
3. `geo.Weather.Temperature 15`: lo zero termico torna a 2.300 m, e la quota
   del ghiaccio lo segue in un quarto d'ora.

Il **gelicidio**: `geo.Weather.Temperature -2` e poi `geo.Weather.Rain 2`
(pioggia imposta: con il gelo, gela al contatto). In un quarto d'ora è tutto
vetrato. Con `geo.Weather.Precipitation 2` invece nevica, e il suolo si
imbianca senza ghiaccio.

**La grandine posata, subito:**

```
geo.Weather.HailGround 8 3
```

Un disco bianco di 3 km attorno a te. **Scendi a pochi metri dal suolo**: da
vicino si vedono i chicchi (celle di 3 cm), da lontano un bianco uniforme.
`geo.Weather.HailGround 2 5`: un disco più largo e a metà bianco. Fonde da
sola: a 15 °C 8 mm durano quasi un'ora; `geo.Weather.Temperature -1` la ferma,
`geo.Weather.HailGround off` la toglie.

**La grandine come viene davvero:** `geo.Weather.Hail`, poi sali a 2–3 km e
aspetta 10–20 minuti: sotto i nuclei che passano compaiono **strisce bianche**,
allungate dove va il temporale. Le stampe, riga Suolo, dicono quanto è bianco
sotto di te; l'overlay (`geo.Weather.Debug 1`), riga Grandine, quanto è spessa
al massimo.

## 9. Da Blueprint

Nel Level Blueprint: *Get GeoWeatherSubsystem* → *Get Precipitation At* con una
posizione (es. quella del pawn) → *Break GeoPrecipitationInfo*: mm/h, le tre
frazioni, la temperatura, la visibilità, e il suolo lì: **Ground Ice** e
**Ground Hail** (0..1), per l'aderenza di un veicolo o il suono dei passi.

I fulmini: *Get GeoWeatherSubsystem* → *Bind Event to On Lightning Strike*: a
ogni fulmine arrivano la posizione, se è arrivato al suolo e la distanza (m).
Per un effetto in più, un suono tuo, una camera che trema da vicino.

## 10. Se qualcosa non va

| Sintomo | Causa probabile | Cosa fare |
|---|---|---|
| Errore di compilazione in `GeoPrecipitationRenderer.cpp` o `GeoWeatherSubsystem.cpp` | codice mai compilato in Unreal | mandami le righe dell'Output Log |
| In giallo "manca MPC_GeoWeather" o "manca M_GeoPrecipitation" | i materiali non sono stati rifatti | `geo.Imagery.CreateMaterial`, poi aspetta 2 s (si cercano da soli) |
| Le stampe dicono `pioggia` ma non si vede niente | il materiale delle particelle non compila | apri `M_GeoPrecipitation`: se il nodo Custom è rosso, mandami l'errore |
| Le gocce sono puntini fermi, o tutte in un punto | il World Position Offset non arriva | in `M_GeoPrecipitation` controlla che "Particelle (WPO)" vada in *World Position Offset* |
| Il suolo non si bagna né s'imbianca | `M_GeoTerrain` vecchio (fatto prima di questo aggiornamento) | `geo.Imagery.CreateMaterial` |
| La neve si posa alla quota sbagliata di ~50 m | manca il geoide (dataset d'acqua non aperto): quote sull'ellissoide | apri l'acqua; senza, lo scarto è il geoide (~47 m a Torino) |
| Nell'editor la visibilità non cala | voluto: la nebbia del livello nell'editor non si tocca (si salverebbe) | prova in Play; l'overlay dice "quella del livello non si tocca nell'editor" |
| La nebbia resta fitta dopo `geo.Weather.Clear` | i valori originali non sono stati ripristinati | `geo.Weather.Fog 0` li rimette; dimmelo |
| Da 8 km di quota non piove | voluto: sopra la cima delle nuvole (6 km, 10 per i temporali) non cade niente | scendi |
| Il suolo è bagnato anche dove non piove | voluto: il bagnato è uno per il mondo, guidato da sotto la camera | `docs/meteo-design.md`, sezione 4 |
| Il fulmine illumina ma il canale non si vede | manca `M_GeoLightning` (l'overlay lo dice in giallo) | `geo.Imagery.CreateMaterial` |
| Il canale si vede, ma è un filo grigio spento | l'esposizione della scena è altissima (sole fisico molto forte) | dimmi l'intensità del sole del livello: il canale si misura su quella |
| Il fulmine c'è ma il tuono no | fuori dal Play l'audio del mondo dell'editor può essere spento; o il volume (`geo.Weather.Thunder`) | prova in Play; `geo.Weather.Thunder 1` |
| `geo.Weather.Storm` ma nessun fulmine per minuti | nessun nucleo entro 30 km (i temporali sono sparsi) | aspetta, `geo.Weather.Lightning 3`, o `geo.Weather.Strike` |
| Con `geo.Weather.Rain` e `geo.Weather.Lightning 1` quasi nessun fulmine | voluto: la pioggia estesa (un fronte) ha pochi temporali | `geo.Weather.Storm` |
| Errore di compilazione in `GeoCloudRenderer.cpp` | codice mai compilato in Unreal (le API della VolumetricCloud e della luce direzionale cambiano fra versioni) | mandami le righe dell'Output Log |
| In giallo "manca M_GeoClouds", o "MPC_GeoWeather non ha i parametri delle nuvole" | i materiali non sono stati rifatti dopo l'aggiornamento | `geo.Imagery.CreateMaterial` |
| `geo.Imagery.CreateMaterial` dice "M_GeoClouds non fatto" | il nodo Custom delle nuvole non compila | apri `M_GeoClouds`: se "Nuvole (densita')" è rosso, mandami l'errore |
| Nessuna nuvola, e l'overlay dice "quella del livello non si tocca nell'editor" | voluto: il livello ha una sua VolumetricCloud, e nell'editor non la si nasconde | prova in Play, o togli quella del livello |
| Le nuvole sono nere o grigio scuro | la luce del sole non ha *Atmosphere Sun Light* (l'overlay lo dice in giallo) | accendila sulla *Directional Light* |
| Le nuvole ci sono ma il terreno sotto non è in ombra | la luce del sole non ha *Cast Cloud Shadows* | accendila sulla *Directional Light* |
| Nuvole velate, quasi trasparenti, o blocchi compatti senza forma | l'estinzione per metro del motore diversa da quella che ho stimato | `geo.Weather.CloudDensity 2` o `0.5`, e dimmi quale va |
| Il lampo nella nuvola non si vede, o la nuvola diventa bianca accecante | la forza del bagliore è una stima (`CloudGlowPerLux` in `GeoCloudRenderer.cpp`) | dimmi com'è, di giorno e di notte |
| Le nubi di pioggia finiscono di colpo lontano | voluto: la mappa meteo copre 160 km attorno alla camera | oltre, i rovesci non hanno nuvole (sezione 4 del design) |
| Errore di compilazione in `GeoHailMap.cpp` (`FUpdateTextureRegion2D`, `UpdateTextureRegions`) | codice mai compilato in Unreal; il tipo della regione sta nel modulo RHI (aggiunto alle dipendenze di GeoRender) | mandami le righe dell'Output Log |
| In giallo "MPC_GeoWeather e' di prima del ghiaccio e della grandine" | la collezione non ha `GroundIce` e `GroundHailMap` | `geo.Imagery.CreateMaterial` |
| `geo.Weather.Ice 1 0` ma il terreno non cambia | `M_GeoTerrain` vecchio (senza ghiaccio), o niente ortofoto (materiale grigio del motore) | `geo.Imagery.CreateMaterial`; accendi le ortofoto |
| Il ghiaccio si vede ma "non sembra ghiaccio" | è un velo lucido: si nota solo dove il sole o il cielo ci si specchiano | guarda verso il sole, con il sole basso; dimmi com'è |
| `geo.Weather.HailGround` ma niente bianco | il nodo "Meteo sul suolo" non riceve la texture (materiale vecchio), o sei su un pendio ripidissimo | `geo.Imagery.CreateMaterial`; guarda un pianoro. Se resta, mandami il log |
| La grandine è spostata rispetto a dove l'hai messa | la posizione della mappa (`GroundHailMap`) non segue il rebase | dimmi di quanto e in che direzione |
| La grandine da vicino sfarfalla | i chicchi (3 cm) diventano più piccoli di un pixel prima dei 15 m previsti (schermo piccolo, campo visivo largo) | dimmelo: si accorcia la distanza dei chicchi |
| Il frame rate cala molto con le nuvole | la VolumetricCloud costa sulla scheda video, e il rumore è procedurale | `geo.Weather.Clouds off` per confrontare; dimmi quanti ms (`stat gpu`) |

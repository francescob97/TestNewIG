# Il vento: la simulazione

> Fino alla Fase 9 il vento era **un numero**: una velocità, una direzione, e le
> raffiche come due sinusoidi uguali in tutto il mondo. Adesso è un **campo**: in
> ogni punto e a ogni quota, a ogni istante, un vettore (nord, est, su) in m/s.
> Lo stesso campo per tutti: le onde, le stampe a schermo, le frecce del debug,
> e domani le nuvole, gli alberi e gli aerei della Fase 7.
>
> Come provarlo: `docs/vento-verifica.md`. Per chi impara: `docs/manuale/14-vento.md`.

---

## 1. L'idea in breve

Il vento in un punto nasce da quattro strati di fenomeni. Si calcolano dal più
grande al più piccolo:

```
1. il tempo che fa      il vento di una stazione meteo (10 m su un prato),
                        con una deriva lenta (decine di minuti)            uguale dappertutto
2. lo strato limite     l'attrito del suolo: profilo logaritmico con la
                        rugosità (mare, terra, città), e la rotazione
                        con la quota (spirale di Ekman)                    dipende dal suolo sotto
3. i rilievi            accelera sulle creste, rallenta nelle valli,
                        ridosso sottovento ai pendii ripidi, sale sui
                        pendii; tutto si spegne salendo                    dipende dal terreno attorno
4. la turbolenza        raffiche coerenti, con la scala dell'atmosfera
                        vera, trasportate dal vento (turbolenza
                        "congelata")                                       cambia da metro a metro
```

Il **dato** è uno solo: il vento di riferimento, come lo darebbe un METAR
(10 m di quota, su un prato con rugosità 3 cm). Tutto il resto lo calcola il
campo. Sul mare, più liscio, lo stesso tempo dà più vento; in città meno.

**Deterministico.** Un seme decide le raffiche: nello stesso punto e allo
stesso istante tutti vedono la stessa raffica. Le onde la vedono passare sul
lago, e un aereo che ci vola sopra la sentirebbe.

---

## 2. I modelli, con i numeri

Codice: `Plugins/GeoWorld/Source/GeoRender/Public/Weather/WindField.h`
(C++ puro, provato senza Unreal da `geoweather_tests`).

### 2.1 Lo strato limite

Sopra **600 m** dal suolo il vento non sente l'attrito: è il **vento di
gradiente**. Sotto segue il profilo logaritmico:

```
U(z) = U_gradiente · ln((z + z0) / z0) / ln((600 + z0) / z0)
```

con z la quota sul suolo e z0 la **rugosità**:

| Suolo | z0 | A 10 m, in rapporto al gradiente | Con 10 m/s di riferimento |
|---|---|---|---|
| Mare, laghi | 0,2 mm | 0,73 | **12,4 m/s** |
| Prato (la stazione) | 3 cm | 0,59 | **10,0 m/s** |
| Campagna (default sulla terra) | 10 cm | 0,53 | **9,0 m/s** |
| Città | 1 m | 0,37 | **6,4 m/s** |

Il vento di gradiente si ricava dal riferimento risalendo il profilo del prato:
10 m/s alla stazione = 17 m/s a 600 m.

**La rotazione.** Al suolo l'attrito ruota il vento verso la bassa pressione:
nell'emisfero nord, salendo, il vento gira in senso orario. La rotazione fra
10 m e il gradiente è di 10° sul mare, ~27° sul prato, ~40° in città
(`10 + 8·log10(z0 / 0,2 mm)`, fra 5° e 40°). Sotto i 10 m è tutta; sopra cala
con il logaritmo della quota fino a zero in cima. Nell'emisfero sud gira al
contrario.

**I venti in quota.** Facoltativi, come nei bollettini dei venti in quota (e
negli strati di vento del CIGI): quota s.l.m., velocità, direzione. Sopra lo
strato limite il vento va dal gradiente al primo strato, interpolando in
linea retta (come vettori), poi da uno strato all'altro; sopra l'ultimo resta
l'ultimo. Gli strati dentro lo strato limite si ignorano: lì comanda il suolo.

### 2.2 I rilievi

Calcolati da 25 quote attorno al punto, dal terreno in memoria a un livello
grossolano (livello 11, post da ~75 m: liscio, e quasi sempre caricato):

- **Accelerazione** (alla Jackson-Hunt): quanto il punto sta sopra la media di
  due anelli, a 500 m e a 2 km, diviso 1 km, dà una "pendenza" φ; il vento al
  suolo vale `1 + 1,6·φ` (fra 0,5 e 1,6). Una collina di 300 m larga un paio di
  km: **+27% in cima**. Una valle: meno vento.
- **Ridosso**: il pendio più ripido **sopravvento** entro 1,5 km. Oltre 14° il
  punto è riparato, fino al −60% di vento e **+150% di turbolenza** (i rotori
  sottovento a una cresta).
- **Vento verticale**: la pendenza del suolo nella direzione del vento, su
  ±150 m; l'aria segue il terreno, `w = u · pendenza`. Sopravvento a una collina
  sale (la "dinamica" dei piloti di aliante), sottovento scende.

Tutti e tre si spengono salendo, con `exp(−z / 400 m)`. Sull'acqua il vento
verticale di pendio è zero (l'acqua è piana); il ridosso di una scogliera
sopravvento resta.

### 2.3 La turbolenza

- **Intensità**: `σu / U = 1 / ln(z / z0)`, come nell'Eurocodice del vento
  (EN 1991-1-4), fra 5% e 35%: a 10 m il 9% sul mare, il 17% sul prato, il 33%
  in città. Sopra lo strato limite un fondo del 5%. Le altre componenti:
  `σv = 0,75 σu`, `σw = 0,5 σu`.
- **Scala**: `L = 300 · (z / 200)^α`, con `α = 0,67 + 0,05 ln z0` (stessa
  norma, appendice B): a 10 m ~70 m sul prato, ~145 m sul mare.
- **Il campo**: rumore di gradiente in 3D (Perlin "improved") in quattro
  ottave, ognuna metà lunghezza e ampiezza `2^(−1/3)` della precedente: la
  cascata di Kolmogorov (spettro −5/3) tagliata a L/8. Normalizzato a varianza
  1 su 400.000 punti misurati.
- **Congelata e trasportata** (ipotesi di Taylor): il campo si sposta con il
  vento. La raffica che ora è qui, fra due secondi è 24 m sottovento: si vede
  correre sul lago. E cambia anche mentre viaggia, in qualche `L / σ`.
- **La raffica di picco** che le stampe mostrano: `U + 3σu` (la raffica di ~3 s
  dei bollettini).

### 2.4 Il tempo che cambia

Con la **variabilità** (0..1) il vento di riferimento deriva da solo: due
rumori lenti nel tempo (scale di 25 e 35 minuti) muovono velocità e direzione,
tipicamente ±20% e ±15°, in un giorno anche il doppio. Uguale dappertutto: è
il tempo che fa, non il posto. Spenta (default) il vento resta quello
impostato.

### 2.5 Niente salti

Lo stesso problema delle onde (`docs/fase9-design.md`, sezione 7.3): il
trasporto della turbolenza **si integra** (`FWindField::Advance`), non si
calcola come velocità × tempo. Con la variabilità la velocità cambia, e dopo
un'ora di gioco un metro al secondo in più sposterebbe tutte le raffiche di
3,6 km in un colpo. Il test lo misura dopo un'ora, a 60 fps: salto massimo
0,07 m/s fra un frame e l'altro.

### 2.6 Le brezze: i venti del sole

`Weather/Breeze.h`. Il sole scalda la terra più dell'acqua e i pendii al sole
più delle valli in ombra; di notte la terra si raffredda prima. L'aria calda
sale, quella fredda scende, e nasce un vento locale che si **somma** a quello
del tempo che fa, con il suo profilo in quota (`FBreezeLayer` nel campo):

| Brezza | Quando | Quanto | Spessore |
|---|---|---|---|
| **di mare** | di giorno, dal mare verso terra | fino a 6 m/s (3,5 a 10 m sulla riva) | ~700 m, poi il **ritorno** in quota (30%) fino a 1,5 km |
| **di terra** | di notte, da terra verso il mare | fino a 2,5 m/s | ~250 m |
| **anabatica** (di pendio) | di giorno, su per i versanti al sole | fino a 2,5 m/s, più forte dove il sole batte | ~120 m, massimo a 25 m |
| **catabatica** (di pendio) | di notte, giù per tutti i pendii | fino a 2,5 m/s | ~40 m, massimo a 4 m |
| **di valle** | di giorno su per la valle, di notte giù | fino a 3 m/s | ~500 m di giorno, ~250 di notte |

**Il ritardo: lo stato termico.** Le brezze non seguono il sole istante per
istante. Lo stato (`FBreezeClimate`) insegue la **spinta del sole**
(l'altezza del sole, meno quanto la tolgono le nuvole e la pioggia, meno il
raffreddamento della notte) con un tempo proprio: 2,5 ore per il mare, 40
minuti per i pendii, un'ora e mezza per le valli. In un giorno d'estate
sereno sulla costa ligure (test): brezza di terra all'alba, quella di mare
parte verso le 9, è massima verso le 16 (ora legale), di terra di nuovo
dopo il tramonto. D'inverno la brezza di mare è un quarto di quella
d'estate; con il cielo coperto un terzo.

**L'orologio è quello del cielo** (`docs/cielo-design.md`): con `geo.Sky.Time
15:00` la brezza è quella delle 15. Lo stato si integra a passi di 10
minuti; se il tempo salta (più di tre ore avanti, o indietro) si riparte da
36 ore prima, e il risultato è lo stesso di averci passato la giornata (il
test lo controlla).

**Dove è l'acqua.** Una mappa di 96 × 96 km attorno alla camera (48 × 48
celle da 2 km, rifatta ogni 5 secondi o dopo 8 km), lisciata con una
gaussiana di 10 km. Il suo **gradiente** punta verso l'acqua; la sua
grandezza, normalizzata, dice **quanto si è sulla costa**: 1 sulla riva di
un mare, 0,14 a 20 km, quasi niente a 40 km. Un laghetto di 3 km non fa
quasi brezza; un lago grande sì (il Garda ha la sua "Ora").

**I pendii e le valli** vengono dalle quote: la pendenza su ±150 m (i
versanti) e su ±2,5 km (le valli). L'anabatica è più forte sui versanti che
**guardano il sole** (la normale del pendio contro la direzione del sole: a
mezzogiorno un versante a sud di 30° ha il 70% di brezza in più di quello a
nord); vicino al suolo l'aria segue il pendio, cioè sale (o scende).

**Il tempo che fa le spegne**: con 9 m/s di vento di riferimento le brezze
non ci sono più (a metà, a 4,5 m/s).

---

## 3. In Unreal

`UGeoWeatherSubsystem` (`GeoRender/Public/Weather/GeoWeatherSubsystem.h`) tiene
il campo, lo fa avanzare a ogni frame e gli dà il suolo:

| Serve al campo | Da dove |
|---|---|
| Quota del suolo sotto il punto | la tile di terreno più fine in memoria; al largo (tile virtuali, sezione 13 di `fase9-design.md`) il geoide |
| Rilievi attorno | le tile di livello 11 in memoria (o le più vicine) |
| Acqua o terra | le tile d'acqua in memoria (la riva dell'SDF) e il mare aperto |
| Quota s.l.m. (per i venti in quota) | il geoide del dataset d'acqua |
| Le brezze | lo stato termico con l'ora del cielo (`UGeoSkySubsystem`), la mappa dell'acqua (`UGeoWaterSubsystem::IsWaterAt`), le pendenze dalle quote |

Le quote si leggono **senza toccare la cache** (`PeekLoadedTile`): il vento ne
chiede centinaia per frame e non deve falsare le statistiche né l'ordine LRU.
Senza quote il suolo è piatto al livello del mare; senza acqua è terra.

Chi lo usa:

- **Le onde**: crescono con il vento **medio sul mare aperto** sotto la camera
  (prima era il vento impostato: ora, con 10 m/s alla stazione, il mare ne ha
  12,4, come in natura), **più la brezza di mare o di terra** vicino alla
  costa. L'increspatura e la schiuma seguono la **raffica che
  sta passando** in quel punto.
- **Le stampe a destra**: `Vento` (alla quota della vista), `Raffiche` (di
  picco, di adesso, verticale), `A 10 m` (sotto di te, come una stazione),
  `Brezza` (quando se ne sente una: "di mare 3,4 m/s da 270°").
- **`GetWindNorthEastUp(Posizione)`**, `BlueprintCallable`: (nord, est, su) in
  m/s in qualunque punto del mondo. È l'aggancio per la Fase 7 e per gli attori.
- **Il debug**: `geo.Weather.Debug 1` (overlay) e `geo.Weather.Arrows <quota>`
  (9 × 9 frecce attorno alla camera, lunghe quanto il vento fa in 20 s,
  colorate per velocità: blu calma, verde 10 m/s, giallo 20, rosso 30).

| Comando | Cosa fa |
|---|---|
| `geo.Weather.Wind <m/s> [gradi] [turbolenza]` | il vento di riferimento (10 m su un prato); senza argomenti lo dice |
| `geo.Weather.Turbulence <0..2>` | 0 aria liscia, 1 l'atmosfera vera (default), 2 il doppio |
| `geo.Weather.Variability <0..1>` | il tempo cambia da solo (default 0) |
| `geo.Weather.WindAloft <quota> <m/s> <gradi>` / `clear` | un vento in quota, s.l.m. |
| `geo.Weather.Seed <n>` | il seme delle raffiche |
| `geo.Weather.Breezes 0\|1` | le brezze (default accese) |
| `geo.Weather.BreezeStrength <0..3>` | quanto sono forti (1 come in natura) |
| `geo.Weather.Debug 0\|1` | overlay |
| `geo.Weather.Arrows <quota sul suolo>` | frecce; 0 le spegne |

**Cambia il terzo argomento di `geo.Weather.Wind`**: prima erano le "raffiche
0..1" (±30% di sinusoide), ora è la turbolenza (1 = quella vera).

---

## 4. Cosa non fa (ancora)

- **Gli altri venti locali**: foehn, bora, i venti di caduta. Le brezze (di
  mare e di terra, di pendio, di valle) invece ci sono (sezione 2.6).
- **Le brezze in dettaglio**: niente fronte della brezza di mare (la linea di
  cumuli dove converge, nell'entroterra), niente rotazione nel pomeriggio (la
  forza di Coriolis la gira di qualche decina di gradi), e la temperatura
  vera del mare non c'è (la stagione la dà solo l'altezza del sole).
- **L'incanalamento nelle valli**: la direzione al suolo non si piega lungo una
  valle stretta. Qui la valle rallenta il vento, non lo gira.
- **Lo strato limite interno**: sulla riva il vento passa da "mare" a "terra" di
  colpo; in natura si adatta in qualche centinaio di metri.
- **Le termiche**: niente correnti ascensionali da riscaldamento (il sole
  ora c'è: sono il passo dopo le brezze di pendio).
- **Dati veri**: il vento si imposta a mano. Il passo successivo naturale è
  leggerlo da un METAR o da un modello (ICON, GFS): il campo ha già il suo
  ingresso, il vento di riferimento più gli strati in quota.
- **Il vento di Unreal** (`WindDirectionalSource`, per SpeedTree e Niagara): si
  collegherà con la vegetazione (Fase 11), quando ci sarà qualcosa da muovere.

---

## 5. Alternative considerate

- **Un solver vero (CFD, o un modello a massa conservata come WindNinja).** Dà
  l'incanalamento e le scie giuste, ma costa secondi per chilometro quadrato e
  va rifatto a ogni cambio di vento: è un lavoro da pipeline offline su una
  griglia, non da campo interrogabile a ogni frame. I modelli scelti sono le
  stesse regole che usano le norme di progetto (Eurocodice) e i simulatori di
  volo: robusti, istantanei, senza dati in più.
- **Turbolenza di Dryden o von Kármán filtrata nel tempo**, come nei simulatori
  di volo (MIL-HDBK-1797). Dà la statistica giusta lungo la traiettoria di UN
  aereo, ma ogni osservatore ne ha una sua: due punti vicini non vedono la
  stessa raffica, e sul lago non si vedrebbe niente correre. Il campo congelato
  e trasportato dà la coerenza nello spazio, che è ciò che si vede.
- **Raffiche come sinusoidi** (quello di prima): uguali dappertutto, il lago
  "respira" tutto insieme. Sostituite.
- **Il vento di riferimento al gradiente** invece che a 10 m sul prato: più
  pulito per la fisica, ma nessuno lo conosce. Il vento a 10 m è quello di
  ogni bollettino.

---

## 6. Dove sta nel codice

| File | Cosa contiene |
|---|---|
| `GeoRender/Public/Weather/WindField.h` | il campo: profilo, rotazione, venti in quota, rilievi, turbolenza, variabilità (C++ puro) |
| `GeoRender/Public/Weather/Breeze.h` | le brezze: lo stato termico, la mappa dell'acqua, mare e terra, pendii, valli (C++ puro) |
| `GeoRender/Public/Weather/Wind.h` | `FWind`: velocità e direzione, il tipo che si scambiano meteo, onde e stampe |
| `GeoRender/Public/Weather/GeoWeatherSubsystem.h` / `.cpp` | il campo in Unreal: suolo, acqua, osservatore, overlay, frecce, Blueprint |
| `GeoRender/Private/GeoRenderModule.cpp` | i comandi `geo.Weather.*` |
| `GeoRender/Private/GeoPositionHudSubsystem.cpp` | le righe del vento nelle stampe a destra |
| `GeoRender/Private/Water/GeoWaterSubsystem.cpp` | le onde dal vento sul mare, l'increspatura dalla raffica |
| `GeoCore/Public/Geo/GeoFormat.h` | `Format::Wind`: "7,2 m/s (14 kt) da 245°" |
| `Tools/StandaloneTests/geoweather_main.cpp` | i test del vento (sezioni 1-5) e delle brezze (sezione 19), insieme al resto del meteo |

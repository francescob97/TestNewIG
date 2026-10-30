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

Le quote si leggono **senza toccare la cache** (`PeekLoadedTile`): il vento ne
chiede centinaia per frame e non deve falsare le statistiche né l'ordine LRU.
Senza quote il suolo è piatto al livello del mare; senza acqua è terra.

Chi lo usa:

- **Le onde**: crescono con il vento **medio sul mare aperto** sotto la camera
  (prima era il vento impostato: ora, con 10 m/s alla stazione, il mare ne ha
  12,4, come in natura). L'increspatura e la schiuma seguono la **raffica che
  sta passando** in quel punto.
- **Le stampe a destra**: `Vento` (alla quota della vista), `Raffiche` (di
  picco, di adesso, verticale), `A 10 m` (sotto di te, come una stazione).
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
| `geo.Weather.Debug 0\|1` | overlay |
| `geo.Weather.Arrows <quota sul suolo>` | frecce; 0 le spegne |

**Cambia il terzo argomento di `geo.Weather.Wind`**: prima erano le "raffiche
0..1" (±30% di sinusoide), ora è la turbolenza (1 = quella vera).

---

## 4. Cosa non fa (ancora)

- **Brezze e venti locali**: brezza di mare e di terra, di monte e di valle,
  foehn, bora. Sono il prossimo passo naturale: dipendono dall'ora e dalla
  temperatura, cioè dalla Fase 12 (sole, atmosfera).
- **L'incanalamento nelle valli**: la direzione al suolo non si piega lungo una
  valle stretta. Qui la valle rallenta il vento, non lo gira.
- **Lo strato limite interno**: sulla riva il vento passa da "mare" a "terra" di
  colpo; in natura si adatta in qualche centinaio di metri.
- **Le termiche**: niente correnti ascensionali da riscaldamento (serve il
  sole).
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
| `GeoRender/Public/Weather/Wind.h` | `FWind`: velocità e direzione, il tipo che si scambiano meteo, onde e stampe |
| `GeoRender/Public/Weather/GeoWeatherSubsystem.h` / `.cpp` | il campo in Unreal: suolo, acqua, osservatore, overlay, frecce, Blueprint |
| `GeoRender/Private/GeoRenderModule.cpp` | i comandi `geo.Weather.*` |
| `GeoRender/Private/GeoPositionHudSubsystem.cpp` | le righe del vento nelle stampe a destra |
| `GeoRender/Private/Water/GeoWaterSubsystem.cpp` | le onde dal vento sul mare, l'increspatura dalla raffica |
| `GeoCore/Public/Geo/GeoFormat.h` | `Format::Wind`: "7,2 m/s (14 kt) da 245°" |
| `Tools/StandaloneTests/geoweather_main.cpp` | 26 test |

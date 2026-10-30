# Il vento — come provarlo

> Le decisioni sono in `docs/vento-design.md`; il racconto per chi impara in
> `docs/manuale/14-vento.md`.

**Cosa è già provato qui, senza Unreal** (`geoweather_tests`, 26 test):

- profilo e rugosità: a 10 m sul prato esattamente il vento impostato; sul mare
  12,4 m/s, in campagna 9,0, in città 6,4 (con 10 m/s alla stazione); cresce
  fino al gradiente (17 m/s) e poi resta lì;
- la rotazione con la quota: +27° sulla terra, +10° sul mare, al contrario
  nell'emisfero sud; i venti in quota e la continuità in cima allo strato limite;
- i rilievi su un terreno sintetico: +27% in cima a una collina di 300 m,
  l'effetto spento a 2500 m, l'aria che sale sopravvento e scende sottovento
  (±2,4 m/s), il ridosso dietro una cresta ripida (−58% di vento, turbolenza
  dal 22% al 53%);
- la turbolenza: media zero, deviazione quella della norma (entro il 20%),
  raffiche coerenti (vicine si somigliano, a 5 scale no), trasportate dal vento,
  nessun salto fra un frame e l'altro dopo un'ora con la variabilità accesa;
- la variabilità (in un giorno da 4,7 a 15,3 m/s, ±40°, mai più di 0,9 m/s al
  minuto), il determinismo (stesso seme, stesso vento), le impostazioni fuori
  scala riportate dentro.

**Cosa non ho potuto provare:** la compilazione in Unreal, le frecce e le
stampe a schermo. I controlli statici (sezione 4 di `docs/consegna.md`) sono
verdi.

---

## 1. Aggiorna e compila

`git pull`, poi compila (*Development Editor*). Il materiale dell'acqua non è
cambiato: `geo.Imagery.CreateMaterial` non serve. Il vento non ha dati da
preparare: c'è sempre.

## 2. Le stampe a destra

Apri un terreno (con l'acqua, se ce l'hai: serve al geoide e a sapere dov'è
l'acqua). Sotto le righe di sempre ce ne sono tre nuove:

| Riga | Esempio | Cosa vuol dire |
|---|---|---|
| Vento | `7,2 m/s (14 kt) da 245°` | il vento medio alla quota della vista |
| Raffiche | `11,3 m/s, adesso 8,1, verticale +0,4 m/s` | la raffica di picco attesa, il vento di questo istante, la componente verticale |
| A 10 m | `4,1 m/s (8 kt) da 232°` | sotto di te, a 10 m dal suolo, come una stazione meteo |

Cose da vedere:

1. **Sali di quota.** `Vento` cresce fino a ~600 m dal suolo e poi si ferma,
   e la direzione gira di una trentina di gradi in senso orario (con il vento
   di default, da ~236° a ~267°). `A 10 m` non cambia.
2. **Mettiti sopra un lago o il mare** (`geo.Goto`): `A 10 m` cresce (l'acqua è
   liscia), le raffiche si calmano.
3. **Sopra una cresta, poi in una valle** (a Torino: la collina di Superga, poi
   la pianura): in cresta più vento e il verticale positivo sul versante
   sopravvento, negativo sottovento.
4. **`adesso` cambia di continuo, senza scatti**: è la raffica che passa.

## 3. Le frecce

```
geo.Weather.Arrows 50
geo.Weather.Debug 1
```

Una griglia di 9 × 9 frecce a 50 m dal suolo attorno a te, lunghe quanto il
vento fa in 20 s, colorate per velocità (blu calma, verde 10 m/s, giallo 20,
rosso 30). Il passo segue la tua quota (200 m da vicino, 1,2 km da 5 km).

- Con un vento forte (`geo.Weather.Wind 15 270`) **le frecce in cresta sono più
  lunghe**, quelle **dietro una cresta corte e storte** (ridosso e raffiche), e
  sui versanti sopravvento **puntano in su**.
- Le frecce **tremano** con le raffiche, che si vedono passare da una freccia
  alla vicina **nel verso del vento**.
- Sull'acqua sono più lunghe e più regolari.
- `geo.Weather.Turbulence 0`: le frecce si fermano (resta il vento medio);
  `geo.Weather.Turbulence 2`: il doppio delle raffiche.

L'overlay (`geo.Weather.Debug 1`) dice il vento di riferimento, il gradiente,
il mare (quello delle onde), il suolo sotto di te (acqua o terra, rugosità,
rilievi, ridosso, pendenza) e il vento a 10 m e alla tua quota.

## 4. Il mare e le onde

Sul mare il vento è più forte di quello impostato: con `geo.Weather.Wind 10`
le onde crescono con 12,4 m/s (lo dice anche la risposta del comando: Hs e
onda di picco). L'increspatura segue la raffica che sta passando sotto la
camera.

## 5. Il tempo che cambia, e il vento in quota

```
geo.Weather.Variability 1
geo.Weather.WindAloft 3000 25 290
geo.Weather.WindAloft 6000 40 300
```

- Con la variabilità il vento di riferimento (prima riga dell'overlay) deriva da
  solo in decine di minuti; `geo.Weather.Variability 0` lo ferma.
- Con i venti in quota, salendo sopra lo strato limite il `Vento` delle stampe
  va verso 25 m/s da 290° a 3000 m s.l.m., poi verso 40 m/s a 6000 m.
  `geo.Weather.WindAloft clear` li toglie.

## 6. Se qualcosa non va

| Sintomo | Causa probabile | Cosa fare |
|---|---|---|
| Errore di compilazione in `GeoWeatherSubsystem.cpp` | codice mai compilato in Unreal | mandami le righe dell'Output Log |
| Le righe del vento non ci sono | il subsystem del meteo non c'è (mondo di servizio) | normale fuori dall'editor e dal gioco |
| `A 10 m` uguale a `Vento` a qualunque quota | le quote del terreno non sono in memoria (manca il dataset) | apri il terreno: senza, il suolo è piatto al livello del mare |
| Sopra un lago `A 10 m` non cresce | il dataset d'acqua non è aperto, o la tile d'acqua non è ancora in memoria | `geo.Water.Open`; l'overlay del vento dice "ACQUA" o "terra" |
| Le frecce non si vedono | sono a 50 m dal suolo e tu sei a 10 km | `geo.Weather.Arrows 1000` |
| Il mare ha onde più grandi di prima con lo stesso comando | voluto: le onde seguono il vento **sul mare**, più forte di quello della stazione | `geo.Weather.Wind 8` per avere il mare di prima con 10 |
| Il terzo argomento di `geo.Weather.Wind` fa "troppo" | ora è la turbolenza (1 = normale), prima erano le raffiche 0..1 | `geo.Weather.Turbulence 1` |

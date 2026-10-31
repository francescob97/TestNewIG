# Il vento — come provarlo

> Le decisioni sono in `docs/vento-design.md`; il racconto per chi impara in
> `docs/manuale/14-vento.md`.

**Cosa è già provato qui, senza Unreal** (`geoweather_tests`, i test del
vento e, nella sezione 19, quelli delle brezze):

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
  scala riportate dentro;
- le brezze: un giorno d'estate sulla costa (di terra all'alba, di mare dalle
  9, massima verso le 16, di terra dopo il tramonto), coperto e inverno più
  deboli, il salto d'ora uguale alla giornata intera; la mappa dell'acqua (la
  costa si sente fino a 20-30 km, un laghetto quasi niente); alle 15 sulla riva
  3,4 m/s a 10 m, 4,4 a 300 m, il ritorno a 1100 m, niente a 2500 m; di notte
  1,6 m/s da terra, sottile; al largo sì, a 40 km dentro terra no, con 12 m/s
  di vento sparite; l'anabatica sul versante al sole (e su quello a nord meno),
  la catabatica di notte in pochi metri, la brezza di valle su e giù.

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

## 5b. Le brezze

Le brezze seguono l'**ora del cielo** (`docs/cielo-verifica.md`). Con il vento
calmo (`geo.Weather.Wind 1`) e il cielo sereno (`geo.Weather.Clear`, niente
nuvole: `geo.Weather.Clouds 0`):

| # | Prova | Atteso | Se no |
|---|---|---|---|
| a | Sulla costa ligure (terreno e acqua aperti), `geo.Sky.Time 15:00 2026-07-15`, `geo.Weather.Debug 1` | riga "Brezze": mare vicino a +0,9; "Brezze a 10 m: di mare 3-4 m/s" dal mare verso terra; nelle stampe a destra la riga "Brezza" | "senza il dataset dell'acqua": `geo.Water.Open`; mare a 0: guarda l'ora (il cielo è acceso?) |
| b | `geo.Weather.Arrows 300` | le frecce corrono dal mare verso l'entroterra, più forti sulla costa, deboli a 30 km | |
| c | `geo.Weather.Arrows 1100` | il ritorno: frecce deboli verso il mare | |
| d | `geo.Sky.Time 03:00` | "di terra 1-2 m/s", da terra verso il mare | |
| e | `geo.Sky.Time 09:00`, poi `geo.Sky.Speed 600` | la brezza di terra cala, poi gira e cresce verso il pomeriggio | |
| f | Su un versante al sole in montagna, `geo.Sky.Time 13:00`, `geo.Weather.Arrows 20` | frecce su per il pendio, più forti sui versanti a sud | |
| g | Stesso posto, `geo.Sky.Time 02:00`, `geo.Weather.Arrows 5` | frecce giù per tutti i pendii; a 100 m niente | |
| h | `geo.Weather.Wind 12` | le brezze spariscono: le frecce sono il vento del tempo | |
| i | Sul mare vicino alla costa, alle 15 | il mare un po' più mosso che a mezzanotte (le onde sentono la brezza) | |
| j | `geo.Weather.Breezes 0` | tutto come prima delle brezze | |

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
| Con il vento a 0 c'è comunque un vento di qualche m/s | sono le brezze (lo dice la riga "Brezza") | `geo.Weather.Breezes 0` per la calma piatta |
| La brezza di mare non c'è mai | manca il dataset dell'acqua, o l'acqua è a più di 40 km | l'overlay lo dice ("senza il dataset dell'acqua", "niente acqua entro 50 km") |

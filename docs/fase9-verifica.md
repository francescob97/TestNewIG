# Fase 9 — Come provarla

> L'acqua (mare, laghi, fiumi) dal dataset OSM e dal terreno fino allo schermo,
> le onde che seguono il vento, e le stampe della posizione a destra. Le
> decisioni sono in `docs/fase9-design.md`; il racconto per chi impara in
> `docs/manuale/13-acqua.md`.

**Cosa è già provato qui, senza Unreal:**

- la pipeline intera su dati sintetici (terreno a tre livelli, un lago, un
  fiume largo 20 m con la sua corrente, un mare, un fiume intermittente e una
  piscina che devono sparire): 23 test Python, 173 in tutto;
- il lettore C++ su una tile scritta dalla pipeline vera, lo scavo, la mesh
  della superficie, il raster, le onde dal vento (energia dello spettro, niente
  creste ripiegate, **niente salti né "mare che scorre" quando il vento
  cambia**, rebase) e le texture generate: 54 test standalone
  (`geowater_tests`); la formattazione delle stampe (virgola, gradi-primi-secondi):
  11 test nuovi in `geocore_tests`;
- i controlli statici (shadowing, simboli fra moduli, build unity, metodi
  inesistenti, formato di `Printf`, conversioni di unità).

**Cosa non ho potuto provare:** la compilazione in Unreal, il **materiale**
(il codice HLSL dei due nodi Custom si compila solo dentro il motore) e un
estratto OSM vero. Il punto più delicato è il materiale: sezione 2, passo 2.

---

## 1. I dati (5–20 minuti)

Dalla cartella `Pipeline`, con lo stesso estratto OSM delle strade:

```bat
python run.py build-water --osm dati\osm\nord-ovest-latest.osm.pbf --terrain dataset\terreno -o dataset\acqua_torino --bbox 7.55 45.00 7.80 45.15
python run.py verify-water -o dataset\acqua_torino
```

- Su Torino **non serve il mare**: ci sono il Po, la Dora, la Stura, il
  Sangone. `--terrain` è il dataset del terreno che usi in Unreal: la pipeline
  ne legge le quote (livello dei laghi, profilo dei fiumi) e crea una tile
  d'acqua per ogni tile di terreno che tocca dell'acqua.
- Alla fine `verify-water` deve dire **TUTTO A POSTO**.
- Per guardare dentro una tile: `python run.py inspect-water dataset\acqua_torino\14\<x>\<y>.gwa`
  (tipi d'acqua, quota della superficie, profondità, corrente).

### Il mare (facoltativo, ma è dove si vedono le onde vere)

Le onde lunghe stanno sul mare e sui laghi grandi; sui fiumi l'esposizione è
quasi zero e resta l'increspatura. Se hai il terreno di tutta Italia
(`dati\copernicus_italia`), prova Genova:

```bat
python run.py fetch-water -o dati\acqua            :: i poligoni del mare, ~850 MB, una volta
python run.py build-water --osm dati\osm\nord-ovest-latest.osm.pbf --sea dati\acqua\water-polygons-split-4326.zip --terrain dataset\italia -o dataset\acqua_genova --bbox 8.75 44.30 9.10 44.48
python run.py verify-water -o dataset\acqua_genova
```

(`fetch-water --dry-run` dice quanto pesa senza scaricare.)

## 2. Unreal

1. **Ricompila.** Ci sono file nuovi in GeoCore, GeoTiles e GeoRender
   (`Water/`, `Weather/`, le stampe): se Visual Studio non li vede, **rigenera i
   file di progetto** (tasto destro sul `.uproject`).
2. **Rifai i materiali**, una volta, dalla console dell'editor:
   ```
   geo.Imagery.CreateMaterial
   ```
   Oltre a quelli di prima crea, in `/GeoWorld/Materials`, **`MPC_GeoWater`**
   (i parametri delle onde), **`T_GeoWaterDefault`** (una texture di riposo) e
   **`M_GeoWater`**. Deve finire con "Materiali pronti".

   ⚠️ **È il punto da guardare.** Apri `M_GeoWater` (doppio clic): se il
   pannello "Stats" in basso mostra errori, o l'Output Log ha righe rosse con
   `M_GeoWater` o `CustomExpression`, **mandami quelle righe esatte**. Il
   materiale è costruito da codice con due nodi Custom in HLSL: un errore lì è
   del tipo "riga 12: identificatore sconosciuto", e si corregge in un giro.
   Nel log può comparire anche un avviso su `TruncateLWC`: non è grave, dimmelo.
3. **Lancia il Play** e nella console:
   ```
   geo.Water.Demo dataset\terreno dataset\ortofoto_torino_v2 dataset\acqua_torino
   ```
   Un quarto argomento facoltativo aggiunge le strade (`dataset\strade_torino`).
   Ti mette a 1,5 km sopra il centro dell'acqua, con l'overlay dell'acqua acceso.

## 3. Cosa guardare

### 3.1 Le stampe a destra

Sempre accese (anche nel viewport dell'editor senza Play):

| Riga | Esempio | Note |
|---|---|---|
| Lat | `45,07031° N   45° 4' 13,1" N` | |
| Lon | `7,68660° E   7° 41' 11,8" E` | |
| Quota | `1.547 m  ellissoide` | quella "vera" del motore |
| s.l.m. | `1.500 m  geoide` | solo con un dataset d'acqua aperto (porta il geoide): a Torino ~47 m meno dell'ellissoidica |
| Dal suolo | `1.270 m` | dalla superficie disegnata del terreno sotto di te |
| Rotta | `273°` | la direzione della camera, 000–359 |

Vai su un punto noto (`geo.Goto Torino`) e confronta lat/lon con una mappa:
devono coincidere alla quinta decimale. `geo.Hud 0` le spegne.

### 3.2 I fiumi

Scendi sul Po (100–300 m di quota):

- **la superficie** segue le sponde con un bordo netto, non a scalini di 20 m:
  la riva la disegna il raster della distanza, non la mesh;
- **il fondale** si vede attraverso l'acqua vicino alle sponde (l'acqua bassa è
  trasparente) e sparisce dove è più profonda: il terreno è **scavato** sotto
  l'acqua, con una rampa dalla riva;
- **la corrente**: l'increspatura scorre verso valle, senza scatti. Se scorre
  verso **monte**, dimmelo (vuol dire che la linea OSM è disegnata al contrario,
  o che ho invertito un segno);
- **schiuma** leggera lungo le sponde.

`geo.Water.Enable 0` spegne l'acqua e il terreno torna quello del DEM; `1` la
riaccende. È il modo più rapido per vedere lo scavo.

### 3.3 Il vento

```
geo.Weather.Wind 12 270      :: 12 m/s da ovest
geo.Weather.Wind 2           :: quasi calma, stessa direzione
geo.Weather.Wind 0           :: specchio
geo.Weather.Wind 8 200 0.4   :: 8 m/s da sud-sud-ovest, raffiche del 40%
```

- L'**increspatura** cambia subito (le raffiche la fanno pulsare);
- le **onde lunghe** (sul mare e sui laghi grandi) cambiano **in una decina di
  secondi**: le vecchie sfumano e le nuove crescono. Nessuna onda deve
  "scivolare" o correre all'improvviso, nemmeno lontano: era il difetto che il
  test della sezione 6 di `geowater_tests` esclude, e vorrei la conferma a occhio;
- sopra i ~6 m/s compare la **schiuma sulle creste** (mare, laghi grandi).

### 3.4 Il mare (se hai fatto Genova)

`geo.Water.Demo dataset\italia dataset\<ortofoto> dataset\acqua_genova`, poi
`geo.Weather.Wind 12 200` (libeccio). Le onde devono venire da sud-ovest, essere
alte un paio di metri (a 12 m/s il mare completamente sviluppato ha Hs ≈ 3 m) e
spegnersi negli ultimi 15 m prima della riva. Il colore lo fa la profondità:
turchese dove il fondale è vicino, blu scuro al largo.

### 3.5 L'overlay (`geo.Water.Debug 1`)

| Riga | Cosa aspettarsi |
|---|---|
| Superfici | da fermi: "in attesa" a zero; qualche ms per costruzione (sui worker) |
| Vento | il vento del mare, con Hs e onda di picco dello spettro |
| Mare | (gialla) solo mentre le onde cambiano dopo un cambio di vento |
| Tile d'acqua | in cache, errori 0 |
| riga rossa | manca un materiale: punto 2 |

## 4. I comandi

| Comando | Cosa fa |
|---|---|
| `geo.Water.Demo <terreno> <ortofoto\|-> <acqua> [strade\|-]` | apre tutto, accende e si mette sopra l'acqua |
| `geo.Water.Open <cartella>` | apre un dataset d'acqua; il terreno si rifà scavato |
| `geo.Water.Enable 0\|1` | acqua e scavo |
| `geo.Water.Debug 0\|1` | overlay |
| `geo.Water.Stats` | i numeri in console |
| `geo.Weather.Wind <m/s> [gradi] [raffiche 0..1]` | il vento; i gradi sono quelli **da cui** soffia (270 = da ovest); senza argomenti dice quello attuale |
| `geo.Hud 0\|1` | stampe della posizione |
| `geo.Imagery.CreateMaterial` | (editor) crea anche `M_GeoWater`, `MPC_GeoWater`, `T_GeoWaterDefault` |

## 5. Se qualcosa non va

| Sintomo | Causa probabile | Cosa fare |
|---|---|---|
| Errori di compilazione del materiale | il codice HLSL dei nodi Custom, mai compilato nel motore | mandami le righe dell'Output Log (sezione 2, passo 2) |
| L'acqua non c'è e il terreno non è scavato, riga rossa "manca M_GeoWater" | materiale non creato | `geo.Imagery.CreateMaterial`. Senza materiale lo scavo resta spento apposta: niente buche vuote |
| L'acqua è piatta e immobile, riga rossa "manca MPC_GeoWater" | collezione dei parametri mancante | come sopra |
| Il terreno non è scavato ma l'acqua c'è | dataset d'acqua di un altro terreno | rifai `build-water` con `--terrain` uguale al terreno aperto |
| Acqua sopra le sponde, o sponde sopra l'acqua | livello del lago o profilo del fiume | screenshot + `inspect-water` della tile: la quota della superficie viene dal 20° percentile del terreno nel lago, e dal minimo progressivo lungo il fiume |
| La corrente va verso monte | linea OSM al contrario, o segno sbagliato | dimmi quale fiume |
| Le onde "corrono" o saltano quando cambi vento | non dovrebbe (sezione 3.3) | dimmi a che distanza e con quale vento |
| Manca la riga "s.l.m." | nessun dataset d'acqua aperto: il geoide arriva con lui | normale; con `geo.Water.Open` compare |
| Manca la riga "Dal suolo" | nessun dataset di terreno aperto | normale |
| Il terreno ci mette di più a comparire | ogni tile aspetta la sua acqua (come per le strade) | normale il primo secondo dopo un salto; se dura, dimmi le righe dell'overlay |

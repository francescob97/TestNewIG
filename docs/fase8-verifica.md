# Fase 8 — Come provarla

> Le strade su Torino, dall'estratto OSM al terreno in Unreal. Le decisioni
> sono in `docs/fase8-design.md`.

**Cosa è già provato qui, senza Unreal:**

- la pipeline intera (estrazione, taglio, indici, verifica, ripresa dopo
  un'interruzione) su un estratto OSM sintetico: 28 test Python;
- il lettore C++ su un file scritto dalla pipeline vera, e il disegno misurato
  al centimetro: 42 test standalone (`georoads_tests`);
- i controlli statici che intercettano gli errori di compilazione già visti.

**Cosa non ho potuto provare:** la compilazione in Unreal e un estratto OSM
vero (da qui Geofabrik non è raggiungibile). Sono i due passi qui sotto.

---

## 1. I dati (5–15 minuti)

Dalla cartella `Pipeline`:

```bat
python run.py check-env
```

Devono comparire `[si] OSM` e `[si] GEOS`. Poi:

```bat
python run.py fetch-osm --area torino -o dati\osm
python run.py build-roads -i dati\osm\nord-ovest-latest.osm.pbf -o dataset\strade_torino --bbox 7.55 45.00 7.80 45.15
python run.py verify-roads -o dataset\strade_torino
```

- `fetch-osm --area torino` scarica il **nord-ovest** (~500 MB: Piemonte,
  Valle d'Aosta, Liguria, Lombardia); il `--bbox` di `build-roads` ne tiene
  solo l'area delle prove su Torino. `--dry-run` dice quanto pesa senza
  scaricare.
- Lo stadio lento è il primo ("estrazione"): il driver OSM legge **tutto** il
  file regionale anche se il bbox è piccolo. Per il nord-ovest, qualche minuto.
- Se lo interrompi, rilancia lo stesso comando: riparte da dove era.
- Alla fine `verify-roads` deve dire **TUTTO A POSTO**.

Per tutta l'Italia: `--area italia` e `build-roads` senza `--bbox` (la stima è
di decine di minuti; ti chiedo di annotare quanti, mi serve per la prossima
volta).

## 2. Unreal

1. **Ricompila.** File nuovi in GeoTiles e GeoRender: se Visual Studio non li
   vede, rigenera i file di progetto (tasto destro sul `.uproject`).
2. **Rifai il materiale**, una volta sola: nella console
   ```
   geo.Imagery.CreateMaterial
   ```
   Il materiale vecchio non ha i parametri delle strade: senza questo passo le
   strade non si vedono e l'overlay lo dice in rosso.
3. **Lancia il Play** e nella console:
   ```
   geo.Roads.Demo dataset\terreno dataset\ortofoto_torino_v2 dataset\strade_torino
   ```
   Al posto delle ortofoto puoi mettere `-` per vedere le strade sul terreno
   grigio. Ti mette a 2,5 km sopra il centro dell'area delle strade, con
   l'overlay delle strade acceso.

## 3. Cosa guardare

### 3.1 L'allineamento

```
geo.Roads.Style mappa
```

Colori da carta stradale (autostrade arancioni, statali gialle, ferrovie nere),
larghe almeno un pixel e mezzo. Le strade disegnate devono stare **sopra**
quelle della foto. Uno spostamento costante di qualche metro in una direzione
sarebbe un problema di georeferenziazione (dimmelo con uno screenshot); le foto
Sentinel sono a 10 m, quindi su una strada larga 6 m un disallineamento di un
pixel è normale.

Poi `geo.Roads.Style realistico` per tornare ai colori veri.

### 3.2 L'overlay (`geo.Roads.Debug 1`)

| Riga | Cosa aspettarsi |
|---|---|
| Tile con strade | quasi tutte "proprie" da ferme; "di un antenato" solo mentre ci si muove, per poco |
| in attesa | vicino a zero da fermi |
| Texture | sotto il budget; "fuori budget" a zero da fermi su un portatile |
| Disegno | qualche ms per disegno (sui worker: non pesa sul frame) |
| Tile di linee | poche decine in cache, errori 0 |

Se compare una riga **rossa** sul materiale: punto 2 qui sopra.

### 3.3 I flash

Muoviti e girati come nella terza prova. Le strade non devono comparire **dopo**
il terreno: una tile nuova si mostra solo con le sue strade o con quelle del
padre. Se vedi strade che spuntano un attimo dopo il terreno, dimmelo: è
esattamente il caso che il predicato di vestizione deve impedire.

### 3.4 Avvicinandosi

Scendendo di quota compaiono le strade minori. Con il profilo portatile, a
grandi linee: oltre i 20 km di distanza solo autostrade e ferrovie, sotto i
~15 km le statali e provinciali, sotto i ~8 km le strade locali, sotto i ~4 km
tutto. Le distanze dipendono dalla soglia del LOD (`geo.Lod.Error`): è il
livello del terreno a decidere quale tile vettoriale si usa (sezione 4.1 del
design).

Da 300–500 m le strade sono un po' morbide: è il limite del terreno, che non
si affina oltre il livello del DEM (design, sezione 8). Puoi alzare la
risoluzione delle tile vicine:

```
geo.Roads.Resolution 512 2048
```

## 4. I comandi

| Comando | Cosa fa |
|---|---|
| `geo.Roads.Open <cartella>` | apre un dataset di strade |
| `geo.Roads.Enable 0\|1` | accende e spegne le strade |
| `geo.Roads.Style realistico\|mappa` | colori veri o da carta stradale |
| `geo.Roads.Resolution <base> [fine]` | pixel per tile; `fine` vale per il livello più profondo del terreno |
| `geo.Roads.Budget <MB>` | memoria video massima per le strade |
| `geo.Roads.Strength <0..1>` | quanto si vedono sopra la foto |
| `geo.Roads.Debug 0\|1` | overlay |
| `geo.Roads.Stats` | i numeri in console |
| `geo.Roads.Demo <terreno> <ortofoto\|-> <strade>` | apre tutto e accende |
| `geo.Quality portatile\|workstation` | ora imposta anche risoluzione e budget delle strade |

## 5. Se qualcosa non va

| Sintomo | Causa probabile | Cosa fare |
|---|---|---|
| `[NO] OSM` in check-env | GDAL senza driver OSM | `conda install -c conda-forge gdal` (le build conda ce l'hanno) |
| `build-roads` fermo a "leggo..." per molti minuti | è lo stadio lento: il driver ricompone le way | aspetta; un file regionale richiede minuti, l'Italia decine |
| Le strade non si vedono, riga rossa sul materiale | `M_GeoTerrain` è quello di prima della Fase 8 | `geo.Imagery.CreateMaterial` |
| Le strade non si vedono, nessuna riga rossa | dataset non aperto, o area diversa da quella del terreno | `geo.Roads.Debug 1`: guarda "senza niente" e l'area stampata da `geo.Roads.Open` |
| Il terreno ci mette di più a comparire | le tile aspettano le loro strade | normale per il primo secondo dopo un salto; se dura, dimmi le righe "in attesa" e "Disegno" |
| Strade spostate rispetto alla foto | georeferenziazione | screenshot con `geo.Roads.Style mappa`, e la posizione (`geo.Origin`) |

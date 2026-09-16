# Fase 2 — Come verificare che funzioni

## A. Senza dati veri — 5 secondi

Genera un finto TINITALY, ci fa girare la pipeline completa e verifica il
risultato. Non serve scaricare niente.

```bash
cd Pipeline
python run.py check-env                      # prima di tutto: diagnosi ambiente
python -m unittest discover -s tests -v
```

Attesi **26 test verdi** (15 di schema e formato, 11 di integrazione).

| Test | Cosa dimostra | Misurato |
|---|---|---|
| `horizontal/vertical_seams_are_bit_identical` | i post di bordo condivisi sono **identici bit per bit** fra tile adiacenti | 758 + 725 coppie, zero differenze |
| `parent_value_lies_within_child_neighbourhood` | il kernel di riduzione e' normalizzato e centrato sul post giusto | 0 violazioni |
| `parent_posts_coincide_geographically_with_child_posts` | l'allineamento fra livelli e' esatto | — |
| `heights_are_ellipsoidal_not_orthometric` | la conversione verticale e' stata davvero applicata | bias **+0.002 m** (senza conversione sarebbe +48.4 m) |
| `missing_posts_are_filled_with_sea_level` | il mare sta a quota N, non a zero | entro 1 m da N |
| `index_matches_files_on_disk` | l'indice non cita tile inesistenti e i min/max coincidono | — |
| `second_run_rewrites_nothing` | idempotenza per stadio | tutti gli stadi saltati |
| `deleted_tiles_are_regenerated_and_the_others_are_not` | riavviabilita' vera | cancellate 5, riscritte 5 |

I test usano EGM96 (`EPSG:5773`) perche' e' inclusa in quasi tutte le
distribuzioni di PROJ. Il dataset di produzione usa EGM2008.

## B. Prima di lanciare sui dati veri

```bash
cd Pipeline
python run.py check-env
```

Deve stampare N fra 42 e 52 m per i cinque punti italiani. **Se questo comando
fallisce, fermati e installa la griglia**: e' l'unico controllo che sta fra te e
un dataset da 35 GB sbagliato di 48 metri. Il messaggio d'errore dice cosa
installare.

## C. Sui dati veri

```bash
python run.py build -i "/dati/tinitaly/*.tif" -o /dati/geoworld/italia
```

Cose da guardare nell'output:

1. **Stadio 1** — `bbox` deve coprire l'Italia (circa `6.6 35.4 18.6 47.1`) e
   `livello nativo consigliato` deve dire **14**. Se dice un numero diverso, il
   pixel del sorgente non e' quello che ci si aspetta.
2. **Stadio 2** — `N(Colosseo)` deve valere ~48 m. Se vale 0, la griglia
   geoidica non sta funzionando (ma il preflight avrebbe gia' fermato tutto).
3. **Stadio 3** — `post con dato` dice quale frazione del bbox e' terraferma:
   per l'Italia atteso ~20-25%. Un valore vicino al 100% significa che il nodata
   del sorgente non e' stato riconosciuto.
4. **Stadio 5** — `tile con dato` deve essere sensibilmente minore di `tile
   candidate`: e' il mare che non viene scritto.

Ispezione a campione:

```bash
python run.py inspect /dati/geoworld/italia/14/17525/4389.ght
```

Stampa bbox della tile, passo, quote e i quattro angoli.

### Verifica indipendente su un punto noto

Apri `_work/geoid_undulation.tif` in QGIS: deve mostrare ~45-50 m sull'Italia.
Poi prendi una cima di quota nota — il Monte Bianco e' a 4808 m **ortometrici** —
e verifica che la tile corrispondente riporti circa **4808 + 52 = 4860 m**
ellissoidici. Uno scarto di ~50 m rispetto all'atteso significa conversione
verticale mancante; uno scarto di pochi metri e' normale (risoluzione del DTM e
ricampionamento).

## D. Cosa NON e' ancora verificato

* La pipeline **non e' mai stata eseguita su TINITALY vero** in questa sessione:
  i dati non sono disponibili qui. E' stata verificata end-to-end su un sorgente
  sintetico che ne riproduce CRS, risoluzione, nodata e quote ortometriche.
* **EGM2008 non e' stata provata**: la griglia non e' installabile in questo
  ambiente (il proxy blocca `cdn.proj.org`). E' stato provato lo stesso identico
  percorso di codice con EGM96. Il preflight e' scritto proprio per intercettare
  il caso in cui EGM2008 manchi sulla tua macchina.
* Le stime di dimensione per l'Italia (~35 GB, ~5.5x10^5 tile) sono
  estrapolazioni geometriche, non misure.
* Nessun consumo dei tile dal runtime C++: e' la Fase 3. I vettori di prova
  dello schema di tiling sono gia' in
  `Plugins/GeoWorld/Source/GeoTiles/TestData/tiling_vectors.json`, cosi' le due
  implementazioni non potranno divergere.

# Fase 2 — Pipeline dati: decisioni e motivazioni

Codice in `Pipeline/`. Per l'uso vedi `Pipeline/README.md`, per la verifica
`docs/fase2-verifica.md`.

---

## 1. Formato dei tile

La specifica di partenza era: schema geografico WGS84, livello 0 = 2 tile (2x1),
heightmap 129x129 float32 con 1 pixel di overlap, file in
`<root>/<level>/<x>/<y>`, manifest JSON. L'ho tenuta, con **due scostamenti
motivati** e alcune aggiunte.

### 129x129 e la registrazione sui nodi

Una tile contiene 129x129 **post** (campioni puntuali), non celle. I 128
intervalli coprono la tile e il post 128 cade esattamente sul bordo, cioe' sul
post 0 della tile adiacente. Questo e' l'overlap di 1 pixel.

Due conseguenze, ed entrambe sono il motivo per cui 2^n+1 e' la scelta giusta:

1. **Le giunzioni a parita' di livello sono esatte per costruzione.** Due tile
   vicine non condividono valori "quasi uguali": condividono *lo stesso dato*,
   letto dalla stessa colonna dello stesso raster. Verificato bit per bit su
   1483 coppie.
2. **La riduzione di livello e' una decimazione su posizioni coincidenti.** Due
   figli affiancati hanno 129+129-1 = 257 post distinti, e 257 = 2*128+1, quindi
   il post *i* del padre corrisponde al post *2i* dei figli. Con tile 128x128
   (registrate sulle celle) i post del padre cadrebbero a meta' strada fra
   quelli dei figli, e ogni livello introdurrebbe mezzo pixel di errore.

### Scostamento 1: estensione `.ght`, non `.hgt`

`.hgt` identifica gia' il formato SRTM: int16 **big-endian**, senza header,
1201x1201 o 3601x3601, con le coordinate nel nome del file. GDAL ha un driver
`SRTMHGT` che si attiva proprio sull'estensione, quindi aprirebbe i nostri file
(float32 little-endian 129x129 con header) interpretandoli come quote intere
big-endian: nessun errore, solo numeri plausibili e sbagliati.

Lo schema di percorsi resta identico. Per tornare indietro e' una costante sola,
`tileformat.TILE_EXTENSION`.

### Scostamento 2: il min/max per tile e' nell'indice binario, non nel manifest

Il requisito era esplicito: *"min/max quota per tile — serve al culling e al
bounding volume: non ometterlo"*. Non e' stato omesso, e' stato **spostato**,
per un motivo dimensionale.

L'Italia al livello 14 ha dell'ordine di **4x10^5 tile** con dato. Una voce JSON
`{"x":8560,"y":3936,"min":12.5,"max":1843.2}` sono ~45 byte: **~18 MB di JSON da
parsare all'avvio** solo per l'ultimo livello. Un record binario di 16 byte
(`uint32 x`, `uint32 y`, `float32 min`, `float32 max`) costa 6.4 MB, si legge con
una `read` e si mappa in memoria senza parsing.

Quindi:

| Artefatto | Contenuto |
|---|---|
| `manifest.json` | metadati globali leggibili: schema, formato, bbox, provenienza, datum verticale applicato, min/max **per livello** |
| `<level>/index.bin` | quali tile esistono e il loro min/max, ordinato per `(y,x)` |

Il min/max e' anche nell'header di ogni tile, cosi' arriva insieme al dato senza
una seconda lettura.

### Aggiunta: header binario di 32 byte

SRTM non ha header. Noi si, e serve per un caso che capita davvero: **rigenerare
la piramide con parametri diversi lasciando i vecchi file sul disco**. Senza
header il runtime legge dati vecchi con geometria nuova e il terreno risulta
sottilmente sbagliato. Costa 32 byte su 66596, cioe' lo 0.05%.

### float32 e non int16

int16 in metri costerebbe meta' spazio ma quantizzerebbe a 1 m, producendo
terrazzamenti visibili sulle pendenze dolci — esattamente dove l'occhio li nota.
int16 con scala e offset per tile e' possibile, ma e' un'ottimizzazione da fare
dopo aver misurato che la banda su disco e' il collo di bottiglia.

---

## 2. Il fallimento silenzioso di PROJ, e perche' ha cambiato il design

Questo e' il punto piu' importante della fase.

La via "elegante" per convertire le quote sarebbe warpare da `EPSG:32632+3855`
(UTM32N + quota EGM2008) a `EPSG:4979` (WGS84 3D) e lasciare che PROJ applichi
la griglia geoidica insieme alla riproiezione orizzontale. **Misurato su questa
macchina, senza la griglia installata:**

```
Transformer.from_crs("EPSG:32632+3855", "EPSG:4979", allow_ballpark=True)
  -> lon=12.481689  lat=41.859138  h=0.000
     [... + Transformation from EGM2008 height to WGS 84
          (ballpark vertical transformation, without ellipsoid height
           to vertical height correction) ...]
```

PROJ ricade su una *ballpark vertical transformation* che semplicemente **non
applica lo scostamento**. Nessuna eccezione, nessun warning, nessun codice di
errore: restituisce le quote di partenza. E il ballpark e' il comportamento di
**default** di `gdalwarp`. Il risultato sarebbe un dataset sbagliato di ~48 m su
tutta l'Italia, plausibile a ogni ispezione superficiale.

**Conseguenza sul design: i due passi sono separati.**

1. Riproiezione **solo orizzontale** `EPSG:32632 -> EPSG:4326`. Nessun CRS
   composto, nessuna griglia dentro gdalwarp, nessun fallback possibile.
2. Somma **esplicita** di `N(lon, lat)`, presa da una griglia che calcoliamo noi
   con `allow_ballpark=False` e che finisce su disco come
   `_work/geoid_undulation.tif`.

Il passo 2 e' una riga di numpy e il suo input e' un file che si apre in QGIS: se
contiene zeri invece dei ~45 m attesi, si vede subito. La stessa griglia serve
gia' per un secondo scopo (riempire i post senza dato), quindi non e' un
artefatto in piu'.

Difese aggiuntive:

* **preflight** (stadio 2) che costruisce la trasformazione con
  `allow_ballpark=False`, calcola N su cinque punti italiani noti e si ferma se
  esce dall'intervallo plausibile 30–65 m, con istruzioni su cosa installare;
* **test sul risultato**, non sulla configurazione: `test_dataset.py` confronta
  le quote con il terreno analitico piu' N. Misurato: bias **+0.0021 m** con la
  conversione, **+48.36 m** senza. La soglia del test e' 0.5 m.

### Errore introdotto dal campionare N su griglia

EGM2008 e' uno sviluppo in armoniche sferiche fino al grado 2190, cioe' ha
risoluzione ~9 km: e' un campo liscio. Con passo 0.01 gradi (~1.1 km) l'errore
di interpolazione bilineare e' trascurabile — ma non lo diamo per buono:
`measure_interpolation_error()` lo **misura** contro i valori esatti di PROJ su
4000 punti casuali e la pipeline si ferma se supera `--max-geoid-error`.
Misurato su EGM96: **max 0.0036 mm**.

---

## 3. Quota del mare: N, non zero

I post senza dato sorgente (mare, fuori dall'Italia) non si riempiono con 0.
Lo zero ellissoidico e' la superficie dell'ellissoide, che in Italia sta ~48 m
**sotto** il livello medio del mare. Riempire con 0 produrrebbe uno scalino netto
di 48 m lungo tutta la costa.

La quota ellissoidica del livello medio del mare e' esattamente N, che abbiamo
gia' in griglia. Verificato: le tile costiere hanno `minHeight` pari a N entro 1 m.

Tile **interamente** priva di dato: non viene scritta affatto. Per l'Italia il
bbox e' circa cinque volte l'area emersa, quindi non scrivere il mare e' la
differenza fra un dataset proporzionale alla terraferma e uno proporzionale al
rettangolo che la contiene.

---

## 4. Piramide: kernel centrato, non media 2x2

La scelta ovvia sarebbe mediare 2x2. E' sbagliata: la media di 2x2 appartiene al
**centro delle quattro celle**, cioe' a meta' strada fra i post. Usarla come
valore del post del livello superiore sposterebbe il terreno di mezzo passo a
ogni livello, e l'errore si accumula.

Si usa invece `[1,4,6,4,1]/16` separabile, **centrato sul post che resta**
(riduzione di piramide gaussiana, Burt-Adelson): filtra le alte frequenze che
darebbero aliasing sulle creste, senza spostare nulla.

Con i buchi (nodata) il filtro e' una media pesata sui soli campioni validi.
Numeratore e denominatore restano separati fra il passaggio su x e quello su y:
cosi' la separabilita' resta **esatta** e non un'approssimazione. Se si
normalizzasse dopo il primo passaggio, il secondo perderebbe traccia di quanti
campioni validi hanno contribuito.

Verificato da `test_parent_value_lies_within_child_neighbourhood`: una media
pesata sta sempre fra il minimo e il massimo dei valori mediati, e se il kernel
non fosse normalizzato o gli indici fossero sfasati di mezzo passo, non sarebbe
cosi'.

### Perche' la piramide si costruisce sui RASTER e non sui tile

Il filtro ha bisogno di 2 post oltre il bordo. Costruendo un tile padre dai suoi
4 figli, quei post appartengono ai figli *di altri padri*: o si leggono 12 file
invece di 4, o si replica il bordo — e replicare romperebbe le giunzioni, perche'
due padri adiacenti replicherebbero campioni diversi.

Lavorando sui raster di livello il margine c'e' naturalmente, e i tile si
ritagliano da un raster gia' ridotto. E' anche il motivo per cui le giunzioni
risultano identiche bit per bit: due tile adiacenti leggono la stessa colonna
dello stesso raster.

### Allineamento: i raster sono ancorati alla griglia globale dei post

Ogni raster di livello comincia su un post della griglia di quel livello, con i
post al **centro** dei pixel. GDAL usa "pixel-is-area", quindi il geotransform
arretra di mezzo pixel. Sbagliare questo mezzo pixel e' l'errore piu' comune e
piu' difficile da vedere: il terreno risulta traslato di meta' passo.

Il raster e' allineato ai **tile**, non al bbox: le tile di bordo sporgono oltre
il bbox e altrimenti i loro post esterni risulterebbero mancanti.

---

## 5. Idempotenza e riavviabilita'

Due meccanismi distinti, perche' risolvono problemi diversi.

**Per stadio.** Ogni stadio ha un'impronta (SHA-256 di parametri + percorso,
dimensione e mtime degli input). A stadio concluso, impronta e output finiscono
in `_work/state.json`. Al rilancio lo stadio si salta se l'impronta coincide
**e** tutti gli output esistono con la dimensione registrata. La seconda
condizione non e' pedanteria: cancellare un intermedio per fare spazio e' una
cosa che si fa. Nell'impronta entra il mtime, non l'hash del contenuto: fare
l'hash di decine di GB a ogni lancio costerebbe piu' del lavoro da evitare.

**Per tile.** Una tile gia' presente, di dimensione giusta e con header coerente
(livello e coordinate corrispondenti) non viene riscritta.

**Scrittura atomica**: ogni tile si scrive su `.tmp` e poi `os.replace`. E' cio'
che rende affidabile il controllo di dimensione: senza, un'interruzione lascia un
file di dimensione giusta e contenuto parziale, che al riavvio viene considerato
buono e resta corrotto per sempre.

Verificato da `test_deleted_tiles_are_regenerated_and_the_others_are_not`:
cancellate 5 tile su 571, il rilancio ne riscrive esattamente 5.

---

## 6. Dimensionamento per l'Italia vera

TINITALY 1.1 e' a 10 m. Alle latitudini italiane il livello il cui passo eguaglia
quella risoluzione e' il **14** (passo 9.54 m in latitudine, 7.12 m in
longitudine), scelto in automatico dalla pipeline.

Il passo in longitudine e' piu' fine perche' un grado di longitudine vale meno
metri: si sovracampiona di circa il 30%. E' la scelta conservativa giusta —
sovracampionare non inventa dettaglio, mentre fermarsi al livello 13
butterebbe via meta' della risoluzione del dato.

Stime a livello 14 (da estrapolare, non misurate su dati reali):

| | |
|---|---|
| Tile con dato, livello 14 | ~4x10^5 |
| Tile totali, tutti i livelli | ~5.5x10^5 |
| Dataset finale | ~35 GB |
| Intermedi in `_work/` | ~8 GB (compressi; i blocchi di solo NaN vanno quasi a zero) |

Per una prima prova conviene `--max-level 11` (passo ~76 m): stessa struttura,
qualche centinaio di MB.

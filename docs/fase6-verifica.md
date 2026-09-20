# Fase 6 — Come verificare che funzioni

> Le decisioni e il perché: `docs/fase6-design.md`.

---

## A. Senza Unreal — un secondo

### I test C++

```bat
cmake -S Plugins/GeoWorld/Tools/StandaloneTests -B build
cmake --build build
build\Debug\geoimagery_tests.exe
```

Attesi **44 test verdi**, in sei gruppi:

| Gruppo | Cosa dimostra |
|---|---|
| Geometria | 256 pixel contro 128 celle; e la conseguenza: l'immagine di livello L ha la risoluzione al suolo del terreno di livello L+1, verificato per tutti i livelli da 4 a 17 |
| Ritaglio | gli esempi del design; i quattro figli tassellano il padre esattamente; gli offset sono rappresentabili in float senza errore; **il ritaglio calcolato con gli interi coincide con quello misurato sui rettangoli geografici, a meno di 1e-9** |
| Scelta | ripiega sull'antenato residente e **intanto chiede quella giusta**; senza immagini dice di no invece di inventare; arriva al livello 0 senza andare in overflow |
| Formato `.gim` | legge una tile valida; **rifiuta una tile di quote**; si accorge di un payload troncato e di un lato sbagliato |
| Indice | legge le voci con la copertura; **rifiuta l'indice di un dataset di quote** |
| Cache template | la cache di immagini conta i byte del payload e sfratta; `FTileCache` resta l'alias di quella delle quote e funziona come prima |

Il totale dei test C++ eseguibili senza motore sale a **164**:

```
geocore_tests       37      geoquadtree_tests   37
geotiles_tests      18      geomesh_tests       28
geoimagery_tests    44
```

### I test Python

```bat
cd Pipeline
python -m unittest discover -s tests
```

Attesi **70 test** (erano 41). I nuovi coprono il formato `.gim`, la riduzione
della piramide, il drappeggio, i quadrati MGRS e la struttura degli import.

Senza Pillow ne saltano 14 invece di fallire: quelli che comprimono un JPEG. La
pipeline delle quote e tutta la matematica del drappeggio restano verificate.
Un test rosso deve voler dire "il codice e' sbagliato", non "ti manca una
libreria opzionale".

Uno merita di essere segnalato: `test_mgrs.py` non verifica solo la coerenza
interna del calcolo. I quadrati attesi per Roma, Milano, Torino, Napoli,
Palermo e Cagliari sono i **prefissi sotto cui il bucket pubblico Sentinel-2
tiene davvero le scene** di quelle città. Se il calcolo fosse sbagliato, il
download non troverebbe niente.

---

## B. Procurarsi le ortofoto

```bat
cd Pipeline
python run.py check-env
```

Prima di tutto il resto: **serve Pillow**, che non era nelle dipendenze fino
alla Fase 6.

```bat
conda install -c conda-forge pillow
```

Serve solo alle ortofoto; la pipeline delle quote funziona anche senza, e
`check-env` lo dice a chiare lettere.

Nuova sezione **formati leggibili**: dice quali driver GDAL hai davvero. Quasi
certamente l'ECW risulterà assente — è normale, richiede l'SDK proprietario
ERDAS che non è incluso in nessun pacchetto conda-forge.

### Sentinel-2 (la via automatica)

```bat
python run.py fetch-imagery --area roma -o dati/sentinel
```

Calcola i quadrati MGRS che coprono l'area, elenca le scene sul bucket, legge la
copertura nuvolosa di ognuna e scarica la migliore. Circa 230 MB per scena.

Per provare senza scaricare niente:

```bat
python run.py fetch-imagery --area test -o dati/sentinel --stream
```

Restituisce percorsi `/vsicurl/...` che GDAL legge direttamente dalla rete,
scaricando solo le finestre che servono. Comodo per un'area piccola, lento su
un'area grande.

### Le tue ECW

Prova prima:

```bat
gdalinfo "D:\database\Imagery\qualcosa.ecw"
```

**Se funziona**, non devi fare altro:

```bat
python run.py build-imagery -i "D:\database\Imagery\*.ecw" -o dataset/ortofoto
```

**Se fallisce** (il caso probabile), il driver non c'è. Converti una volta sola
in GeoTIFF — con QGIS, o con il visualizzatore fornito insieme al database — e
poi dai i GeoTIFF alla pipeline. Da lì in avanti è identico.

È la stessa proprietà che hai già visto sulle quote: `build` accetta i GeoTIFF
di TINITALY e i `.dt2` che hai scaricato senza distinzione, perché tutto passa
da `gdal.Open` e nessuna riga della pipeline guarda il formato del file.

```bat
:: quote da TINITALY            :: quote dai tuoi DTED
python run.py build -i "tinitaly/*.tif" -o dataset/quote
python run.py build -i "Elevation/*.dt2" -o dataset/quote-dted
```

### Costruire la piramide

```bat
python run.py build-imagery -i "dati/sentinel/*_TCI.tif" -o dataset/ortofoto --name "Sentinel-2 Roma"
python run.py verify-imagery -o dataset/ortofoto
python run.py inspect-imagery dataset/ortofoto/13/4332/1012.gim
```

`build-imagery` stampa la tabella dei livelli **prima** di cominciare: è quella
che permette di accorgersi di aver chiesto mezzo terabyte di tile prima di
generarle, non dopo tre ore.

Aspettati il livello **13** come massimo per Sentinel-2 (9,5 m per pixel) e
circa 15–25 KB per tile.

---

## C. In Unreal

### Il materiale, una volta sola

Il drappeggio ha bisogno di un materiale, che è un file binario e quindi non
può nascere dal codice sorgente. Prova:

```
geo.Imagery.CreateMaterial
```

Se funziona, crea `/GeoWorld/Materials/M_GeoTerrain` e hai finito.

**Se quel comando non compila o fallisce**, il materiale si fa a mano in un
minuto. Nel Content Browser, cartella `GeoWorld Content/Materials`, nuovo
Material chiamato **`M_GeoTerrain`**:

1. **TextureCoordinate** (nessuna impostazione da cambiare).
2. **VectorParameter**, nome **`UvOffsetScale`**, valore di default
   `(0, 0, 1, 1)`.
3. **Multiply**: A = TextureCoordinate, B = `UvOffsetScale` mascherato **BA**.
4. **Add**: A = uscita del Multiply, B = `UvOffsetScale` mascherato **RG**.
5. **TextureSampleParameter2D**, nome **`BaseColor`**, con l'ingresso **UVs**
   collegato all'uscita dell'Add.
6. L'uscita RGB del sampler va in **Base Color**.

I nomi dei due parametri devono essere esatti: sono quelli che il codice cerca.

### La demo

```
geo.Imagery.Demo D:\geoworld\quote D:\geoworld\ortofoto
```

Apre entrambi i dataset, si posiziona a 6 km, accende terreno e drappeggio e
**spegne il wireframe** — al contrario di `geo.Terrain.Demo`, perché con
un'ortofoto addosso il terreno non si confonde più col cielo, e il reticolo
coprirebbe proprio quello che sei venuto a vedere.

```
--- GeoWorld | Fase 6: ortofoto ---
Tile vestite  : 96   senza immagine 4
Livelli usati : 11..13   grossolane 18
Texture       : 74   19.4 MB video   +4 questo frame
Streaming     : 118 in cache, 6 in volo, 31.2/512 MB
Per tile      : lettura 0.41 ms, decodifica 1.83 ms (sul worker)
```

### Le cinque prove

**1. Si vede la Terra.** Vola su una città: strade, campi, il mare. Se il
terreno resta grigio, manca il materiale (sopra) — e il log lo dice
esplicitamente all'avvio.

**2. La geografia combacia con il rilievo.** È la prova che vale di più, perché
mette alla prova due catene indipendenti insieme. La linea di costa
dell'ortofoto deve stare dove il terreno smette di salire; il letto di un fiume
deve stare nel fondovalle. Se l'immagine è traslata rispetto al rilievo, il
sospetto non è il drappeggio: sono i due dataset che non condividono lo stesso
bbox o lo stesso schema.

**3. Il drappeggio si affina da solo.** Scendi di quota lentamente e guarda
`Livelli usati` nell'overlay: il numero deve salire, e `grossolane` scendere.
Quello che vedi è la regola della sezione 5 del design in azione — la tile
grossolana viene disegnata subito e sostituita appena arriva quella giusta.
Se `grossolane` resta alto e non cala mai, o la piramide delle ortofoto è più
bassa di quella del terreno (normale, e l'overlay lo mostra come `Livelli usati`
fermo al massimo del dataset), oppure lo streaming non sta al passo.

**4. LA prova delle UV — la scacchiera.**

```
geo.Imagery.Checker 1
```

Sostituisce le ortofoto con una scacchiera generata sul momento, con il
**bordo rosso** su ogni tile. Su una foto di un bosco un disallineamento di
mezzo pixel non lo nota nessuno; su una scacchiera salta all'occhio.

Cosa deve succedere:

* i bordi rossi di due tile adiacenti **dello stesso livello** devono formare
  una riga sola, non due righe parallele né una riga spessa il doppio;
* i quadri devono avere tutti la stessa dimensione a parità di livello;
* dove una tile usa un'immagine grossolana i quadri appaiono **più grandi**, ed
  è corretto: sta guardando un ritaglio ingrandito. Il bordo rosso in quel caso
  non c'è, perché appartiene alla tile dell'antenato.

Se vedi la scacchiera **ripetuta più volte** dentro una tile, il ritaglio o
l'indirizzamento della texture è sbagliato. Se vedi una riga di colori presi
dall'altro lato dell'immagine, è `TA_Wrap` al posto di `TA_Clamp`.

**5. Il costo.** `decodifica` deve stare sotto i 5 ms per tile e comunque non
tocca il game thread: avviene sul worker. `Texture` in memoria video cresce di
256 KB per tile; con un centinaio di tile a schermo sono circa 25 MB.

### Comandi

```
geo.Imagery.Demo <quote> <ortofoto>   apre tutto e accende
geo.Imagery.Open <cartella>           apre solo il dataset di ortofoto
geo.Imagery.Enable <0|1>              drappeggio acceso o spento
geo.Imagery.Checker <0|1>             scacchiera: mostra le UV
geo.Imagery.Debug <0|1>               overlay
geo.Imagery.Budget <N>                texture create per frame
geo.Imagery.Stats                     statistiche
geo.Imagery.CreateMaterial            costruisce M_GeoTerrain (solo editor)
```

---

## D. Cosa NON è verificato

* **Il C++ non è mai stato compilato dentro Unreal.** Vale per tutte le fasi.
  La matematica del drappeggio è verificata numericamente e contro la geografia,
  il formato del file contro la pipeline che lo scrive, le convenzioni UE
  staticamente. La creazione delle texture e il materiale no: quelli si vedono
  solo a schermo.
* **`geo.Imagery.CreateMaterial` usa API di editor che non ho potuto
  compilare.** Se non funziona, le istruzioni manuali sopra sono la strada, e
  non è un ripiego: sono cinque nodi.
* **Il taglio del livello più fine non è stato provato**, perché richiede GDAL,
  che qui non c'è. Sono provati il formato, la riduzione della piramide, gli
  indici e il manifest. Il primo `build-imagery` su dati veri è la prima volta
  che quel codice gira.
* **Niente mipmap**: a viste radenti il terreno sfarfalla. Il LOD tiene il
  rapporto texel/pixel vicino a 1:1, quindi il problema è contenuto, ma esiste.
  La soluzione è scrivere i mip nel file e caricarli tutti.
* **Niente trasparenza** sulle tile parzialmente coperte: dove l'ortofoto non
  arriva si vede il grigio di riempimento. Serve un canale alfa, che il JPEG non
  ha.
* **Niente dissolvenza fra livelli**: il passaggio da un livello di immagine al
  successivo è uno stacco netto.
* **Una texture e una material instance per tile**, quindi un draw call per
  tile. È lo stesso limite del provider della Fase 5 e si risolverà insieme al
  suo.
* **Resta aperta la issue #1 sul jitter** (`docs/issues-aperte.md`).

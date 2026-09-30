# Dopo la prima prova su Torino

> Hai provato le ortofoto su Torino e hai trovato tre problemi: le immagini si
> vedevano male (sfocate e "a mosaico"), il gioco laggava, e mancava un modo per
> scaricare tutta l'Italia. Questo documento dice **cosa li causava**, **cosa è
> cambiato** e **cosa fare adesso**, in quest'ordine.

---

## 1. In una pagina

| Sintomo | Cause trovate | Corretto? |
|---|---|---|
| "Sembra un mosaico, i pezzi a rettangoli non sembrano giusti" | **bug nel materiale**: l'offset U finiva anche in V, e metà delle tile prendeva il pezzo d'immagine sbagliato | **sì** — va rifatto il materiale, vedi sezione 2 |
| | **scene Sentinel vuote a metà**: su Torino una vuota al 37% e una al 58%, di giorni diversi | **sì** — nuova scelta delle scene, va riscaricato |
| "Si vedono molto male" | **niente mipmap**: da lontano il terreno brulicava | **sì** |
| | **Sentinel-2 è a 10 m**: a bassa quota è sfocato per natura | no, è il dato: servono ortofoto migliori (sezione 4) |
| "Lagga notevolmente" | la costruzione delle mesh stava **sul game thread**: qualche ms per tile, 4 tile per frame, e 24 nel riscaldamento | **sì** — ora sta sui thread di lavoro |
| | il terreno **proiettava ombre**: centinaia di mesh da 33.000 triangoli ridisegnate nelle mappe d'ombra | **sì** — spente di default |
| | budget di memoria pensati per 64 GB, su una macchina da **16** | **sì** — ora si adattano alla RAM |
| "Mi serve tutta l'Italia" | mancava l'area per le ortofoto | **sì** — `--area italia`, sezione 5 |

Per strada sono saltati fuori altri due bug, già corretti: in modalità
`--stream` tutte le scene si sovrascrivevano a vicenda (sezione 3.3), e le
scene che coprono i buchi di un'altra potevano finirle sopra invece che sotto
(sezione 3.2).

---

## 2. Cosa fare, in ordine

1. **Aggiorna e compila** (Visual Studio, *Development Editor*).
2. **Rifai il materiale**, anche se ce l'hai già:
   ```
   geo.Imagery.CreateMaterial
   ```
   Ora rifà quello esistente invece di rifiutarsi. Finché non lo fai,
   `geo.Imagery.Debug 1` mostra una riga **rossa**: *"M_GeoTerrain è la
   versione VECCHIA"*.
3. **Riscarica le scene di Torino** con la regola nuova, e ricostruisci:
   ```bat
   python run.py fetch-imagery --area torino -o dati/sentinel_torino
   python run.py build-imagery -i @dati/sentinel_torino/ordine_scene.txt -o dataset/ortofoto_torino --name "Sentinel-2 Torino"
   ```
   Nota la `@`: è l'elenco delle scene **nell'ordine giusto**, non un glob.
4. Riprova, e se lagga ancora segui la **sezione 6** prima di concludere
   qualunque cosa.

---

## 3. Il "mosaico": tre cause, tutte vere

### 3.1 Il materiale sommava l'offset sbagliato

Una tile di terreno di livello 14, con Sentinel che arriva fino al 13, si veste
con **un quarto** dell'immagine di livello 13 che la contiene. Quale quarto lo
dicono due offset, U e V, che il materiale somma alle coordinate della mesh:

```
uv_immagine = uv_mesh × scala + (offsetU, offsetV)
```

Il materiale prendeva i due offset da un parametro vettoriale. Nel codice che lo
costruiva, però, all'addizione era collegato il **pin R** del parametro, cioè
un canale solo. In un materiale di Unreal un canale solo è uno **scalare**, e
sommare uno scalare a un vettore lo somma a **tutte** le componenti:

```
voluto:   (u, v) + (offsetU, offsetV)
fatto:    (u, v) + (offsetU, offsetU)
```

Per ogni tile con X e Y di parità diversa, V finiva sul quarto sbagliato.
**Metà delle tile**, esattamente (il test lo conta: 32 su 64): un mosaico di
rettangoli ognuno con un pezzo di foto fuori posto. È il tuo "non sembrano
giusti".

La parte istruttiva: il design (`fase6-design.md`) disegnava correttamente
`.RG`. Il codice aveva "risparmiato un nodo" di maschera, con tanto di commento
che lo rivendicava. **Un nodo risparmiato, un mosaico.**

**Cosa è cambiato:**
- il materiale ha una `ComponentMask` con R e G, e il parametro si chiama
  `DrapeUv` (non più `UvOffsetScale`);
- il nome nuovo serve a **riconoscere** un materiale vecchio: il provider lo
  controlla all'avvio e lo segnala in rosso, invece di disegnare in silenzio il
  mosaico sbagliato;
- `geo.Imagery.CreateMaterial` rifà il materiale esistente sul posto;
- un test (`geoimagery_tests`, sezione 8) descrive la formula giusta e mostra
  su quali tile quella sbagliata sbagliava.

### 3.2 Le scene Sentinel erano vuote a metà

Il satellite fotografa strisce larghe 290 km, e i quadrati MGRS al bordo della
striscia restano coperti in parte. La regola vecchia guardava solo le nuvole.
Letti dal bucket, i candidati di Torino per l'estate 2024:

| Quadrato | Scelta vecchia | Scelta nuova |
|---|---|---|
| 32TLQ | 29 luglio, nuvole 5,5%, **vuota al 36,9%** | 21 agosto, nuvole 6,0%, vuota allo 0% |
| 32TMQ | 22 luglio, nuvole 0,3%, **vuota al 57,9%** | 29 luglio, nuvole 0,3%, vuota allo 0% |

Le parti vuote finivano nel dataset come **grigio di riempimento**; il resto
erano due giorni diversi, con luce diversa. Rettangoli grigi e pezzi di colore
diverso: l'altra metà del "mosaico".

**La regola nuova** (`Pipeline/geoworld/fetchimagery.py`, `choose_scenes`):

1. si scartano le scene vuote più dell'1% o nuvolose più del 10%;
2. fra le rimaste si preferisce lo **stesso giorno** per più quadrati possibile:
   stesso passaggio del satellite, stessa luce, nessuna cucitura;
3. un quadrato senza scene piene prende la migliore scena **serena** più fino a
   due scene di **riempimento** di altri giorni, che vanno sotto di lei nel
   mosaico. Serena prima che piena: due metà serene fanno un'immagine migliore
   di una scena intera con un terzo di nuvole.

Il punto 3 ha richiesto un file d'ordine. Nel mosaico virtuale di GDAL chi viene
dopo copre chi viene prima, e con un glob l'ordine è alfabetico: un riempimento
poteva finire **sopra** la scena principale. Ora `fetch-imagery` scrive
`ordine_scene.txt` e `build-imagery` lo legge con `-i @file`.

La funzione di scelta è pura (niente rete) e i test usano i numeri veri di
Torino, compreso quello che dimostra che la regola vecchia avrebbe scelto le
scene vuote.

### 3.3 Un bug trovato per strada: `--stream`

Con `--stream` le scene si leggono dalla rete, e i loro indirizzi finiscono
tutti con `.../TCI.tif`. La pipeline dava a ogni scena riproiettata un file
intermedio chiamato come il sorgente: **tutti** `TCI_4326.vrt`. Ognuno
sovrascriveva il precedente, e il mosaico conteneva N volte l'ultima scena.
Ora il nome porta la posizione nell'elenco e la cartella della scena.

---

## 4. "Si vede male": mipmap sì, risoluzione no

### 4.1 Le mipmap mancavano

Una tile lontana che occupa 20 pixel sullo schermo, senza mipmap, viene letta
prendendo 20 pixel su 256 e saltando gli altri. Quali si prendono cambia a ogni
minimo movimento: il terreno **brulica**. Con le mipmap la scheda video legge una
versione già ridotta alla dimensione giusta.

- Si calcolano sul **worker**, subito dopo la decodifica JPEG, quindi non
  costano niente al frame.
- La media si fa in **luce lineare**, non sui valori sRGB: una scacchiera
  bianco/nero deve diventare grigio 188, non 128 (che sarebbe troppo scuro, e
  il terreno si scurirebbe a gradini allontanandosi). Il test lo verifica.
- La texture usa il filtro del gruppo **World**, che è **anisotropico**: a
  viste radenti, cioè sempre da un aereo, fa un'enorme differenza.
- Costano un terzo di memoria in più, e la cache lo conta.

### 4.2 Sentinel-2 è a 10 metri, e da vicino si vede

Qui non c'è niente da correggere nel codice: è il dato. Un conto a spanne,
camera a 90° di campo e 1.920 pixel di larghezza, guardando in basso:

| Quota | Terreno per pixel dello schermo | Pixel dello schermo per pixel Sentinel |
|---:|---:|---:|
| 500 m | 0,5 m | ~20 |
| 1.000 m | 1 m | ~10 |
| 5.000 m | 5 m | ~2 |

Sotto i 3-5 km ogni pixel di Sentinel è una macchia di molti pixel sullo
schermo: sfocato per natura. Per volare bassi servono ortofoto **sotto il
metro**.

**Le hai già**: il tuo database ha una cartella `Imagery` con file `.ECW`
(`docs/consegna.md`, sezione 3). Se sono ortofoto aeree, sono probabilmente a
20-50 cm. Due strade:

1. `gdalinfo --formats | findstr /I ecw` — se il driver c'è, `build-imagery`
   le legge direttamente;
2. altrimenti convertile una volta in GeoTIFF (QGIS con supporto ECW, o il
   visualizzatore gratuito ERDAS), e poi `build-imagery` come sempre, con
   `--src-nodata none` se nelle tue foto il nero è un colore vero.

La pipeline sceglie da sola il livello massimo dalla risoluzione: a 50 cm sono
4-5 livelli in più di Sentinel. **Attenzione alle dimensioni**: ogni livello in
più moltiplica per quattro il numero di tile. A 50 cm conviene partire da
un'area limitata (Torino e dintorni), non da tutta Italia.

---

## 5. Scaricare tutta l'Italia

### Quote

Già possibile prima, con Copernicus a 30 m (le tile di mare non esistono e
vengono saltate):

```bat
python run.py fetch --area italia -o dati/copernicus_italia
python run.py build -i "dati/copernicus_italia/*.tif" -o dataset/quote_italia --name "Copernicus Italia"
```

Se hai TINITALY (10 m) intero, è meglio: tre volte più fine, e lo sai già
costruire.

### Ortofoto Sentinel-2

```bat
python run.py fetch-imagery --area italia -o dati/sentinel_italia --dry-run
python run.py fetch-imagery --area italia -o dati/sentinel_italia
python run.py build-imagery -i @dati/sentinel_italia/ordine_scene.txt -o dataset/ortofoto_italia --name "Sentinel-2 Italia 2024"
```

Il `--dry-run` non scarica niente: dice quali scene e quanto pesano. **Misurato**
sul bucket vero (estate 2024):

| | |
|---|---|
| Quadrati MGRS | 89 (il mare aperto è escluso con un contorno approssimato dell'Italia, isole comprese fino a Pantelleria e Lampedusa) |
| Scene | 96: 77 principali + 19 di riempimento |
| Dimensione | **24,7 GB** |
| Tempo per scegliere | 2 minuti (3.034 metadati letti, 16 in parallelo) |
| Coerenza | la maggior parte dei quadrati condivide il giorno con altri 12-15 |

Il download va a 4 file alla volta (`--workers`), e se si interrompe basta
rilanciarlo: i file completi si saltano.

**Non misurato**, perché qui GDAL non c'è: il tempo di `build-imagery` su tutta
Italia. Sono circa centomila tile al livello più fine, ognuna ritagliata e
compressa: aspettati **ore**, non minuti, e qualche GB di dataset. Conviene
lanciarlo la sera; se si interrompe, riparte da dove era arrivato.

---

## 6. Le prestazioni

### 6.1 Cosa è cambiato

**La costruzione delle mesh è passata sui thread di lavoro.** Prima, per ogni
tile, il game thread faceva la geodesia (~1 ms misurato), costruiva la
`FDynamicMesh3` con la sua topologia e poi il proxy di scena. La geodesia l'ho
misurata; il resto qui non si può misurare, ma a stima sono diversi millisecondi
per tile: 4 tile per frame vogliono dire decine di millisecondi **ogni volta che
ci si muoveva**, e nel riscaldamento (24 per frame) ben oltre i 100. E la residenza, costruendo in
anticipo, teneva il game thread occupato anche in volo dritto. Ora:

```
game thread                        thread di lavoro (UE::Tasks)
-----------                        ----------------------------
chiede la tile  ───────────────►   BuildTileMesh (geodesia)
                                   FDynamicMesh3 (topologia)
consegna al componente  ◄──coda──  risultato
```

Al game thread restano la consegna (`SetMesh`, uno spostamento, non una copia)
e la creazione del proxy a fine frame. L'overlay del terreno mostra i due tempi
separati: **Costruzione** (su thread, non pesa sul frame) e **Consegna** (sul
game thread: è quella da guardare).

**Ombre spente sul terreno.** Le ortofoto le ombre le hanno già: sono foto fatte
con il sole. Calcolarle di nuovo le raddoppia e costa moltissimo, perché ogni
mesh (non Nanite) va ridisegnata nelle mappe d'ombra. `geo.Terrain.Shadows 1`
le riaccende, se vuoi vedere la differenza.

**Ray tracing spento sui componenti del terreno.** Se il progetto ha il ray
tracing attivo, ogni tile costruirebbe la propria struttura di accelerazione.

**Budget in base alla RAM.** Con 16 GB:

| | 16 GB | 32 GB | 64 GB |
|---|---:|---:|---:|
| mesh tenute (visibili + nascoste) | 700 | 1.500 | 3.000 |
| cache delle quote | 512 MB | 1 GB | 4 GB |
| cache delle immagini | 512 MB | 1 GB | 3 GB |

L'Output Log dice cosa ha scelto all'avvio (`RAM 16 GB: budget di 700 mesh`).

### 6.2 Se lagga ancora: dove guardare

Prima di cambiare qualcosa, **misura**. Nella console:

```
stat unit
```

Mostra quattro tempi in millisecondi. Il più alto è il collo di bottiglia:

| Il più alto è... | Vuol dire | Cosa provare |
|---|---|---|
| **Game** | il nostro codice sul game thread | overlay del terreno, riga *Consegna*: se è alta, `geo.Terrain.Budget 2` |
| **Draw** | troppi oggetti da preparare per il rendering | `geo.Lod.Error 8`: meno tile, meno draw call |
| **GPU** | la scheda video | vedi sotto |

Se è la **GPU**, prova uno per uno (sono comandi del motore, reversibili):

| Comando | Cosa spegne |
|---|---|
| `r.Shadow.Virtual.Enable 0` | le Virtual Shadow Maps di UE5, pesanti con geometria non Nanite |
| `r.DynamicGlobalIlluminationMethod 0` | Lumen (illuminazione globale) |
| `r.ScreenPercentage 70` | risoluzione di rendering al 70% |
| `geo.Lod.Error 8` | dimezza il dettaglio del terreno (circa un quarto delle tile) |

Se uno di questi fa la differenza, **dimmi quale**: vuol dire che conviene
cambiare un'impostazione del progetto, non il codice.

### 6.3 Tre cose del portatile, prima di tutto

- **La scheda video giusta.** Sui portatili con grafica integrata più una
  NVIDIA, Windows può far girare l'editor sull'integrata. Impostazioni →
  Schermo → Grafica → aggiungi `UnrealEditor.exe` → *Prestazioni elevate*.
- **L'alimentatore collegato.** A batteria la 4070 laptop va a una frazione.
- **Misura nel posto giusto.** L'editor con il Play dentro il viewport costa di
  più del gioco vero. Per un numero onesto: *Play → Standalone Game*.

### 6.4 Cosa resta lento per costruzione

Ogni tile è ancora un `UDynamicMeshComponent`, con il proprio proxy di scena e
il proprio draw call. Con centinaia di tile a schermo è il limite successivo
(`consegna.md`, "un secondo provider di mesh"). Non l'ho toccato adesso perché
prima vanno visti i numeri di `stat unit` con le correzioni di oggi: se il
collo di bottiglia risulta **Draw**, è quello il prossimo lavoro.

---

## 7. Possibili errori di compilazione

Queste modifiche usano API di Unreal che qui non ho potuto compilare. Se una
non esiste nella tua versione:

| Errore su... | Cosa fare |
|---|---|
| `SetEnableRaytracing` | togli la riga in `DynamicMeshTerrainProvider.cpp`: è un'ottimizzazione |
| `SetMesh` | sostituisci con `EditMesh([&](FDynamicMesh3& M){ M = MoveTemp(Ready.Mesh); })` |
| `FTexture2DMipMap` / `Realloc` | mandami l'errore: è il caricamento delle mipmap in `GeoImagerySubsystem.cpp` |
| `UE::Tasks::Launch` | manca l'include `Tasks/Task.h`, oppure mandami l'errore |
| `DeleteAllMaterialExpressions` | cancella `M_GeoTerrain` dal Content Browser e rilancia `geo.Imagery.CreateMaterial` |

---

## 8. Dove sta nel codice

| File | Cosa è cambiato |
|---|---|
| `GeoWorldEditor/Private/GeoTerrainMaterialFactory.cpp` | maschera RG, parametro `DrapeUv`, rifà il materiale esistente |
| `GeoRender/Private/Terrain/DynamicMeshTerrainProvider.cpp` | `DrapeUv`, avviso sul materiale vecchio, preparazione della mesh separata dalla consegna, ombre e ray tracing |
| `GeoRender/Public/Terrain/GeoTerrainMeshProvider.h` | `FGeoPreparedTileMesh`, funzione di preparazione, `CommitPreparedTile`, `SetCastShadows` |
| `GeoRender/Private/Terrain/GeoTerrainSubsystem.cpp` | costruzione su `UE::Tasks`, coda di ritorno, generazioni, consegna con budget, RAM |
| `GeoTiles/Public/Tiles/ImageTileFormat.h` | `BuildMipChain`, mediata in luce lineare |
| `GeoTiles/Private/Streaming/GeoImageryStreamingSubsystem.cpp` | mipmap sul worker, cache in base alla RAM |
| `GeoRender/Private/Imagery/GeoImagerySubsystem.cpp` | mipmap nella texture, filtro World, avviso sul materiale |
| `GeoTiles/Private/Streaming/GeoTileStreamingSubsystem.cpp` | cache delle quote in base alla RAM |
| `GeoRender/Private/GeoRenderModule.cpp` | `geo.Terrain.Shadows`, `geo.Terrain.Threads` |
| `Pipeline/geoworld/fetchimagery.py` | scelta per copertura e giorno, riempimenti, area italia, `--dry-run`, download paralleli |
| `Pipeline/geoworld/imagerybuild.py` | `-i @elenco.txt`, nell'ordine scritto |
| `Pipeline/geoworld/raster.py` | nomi unici dei VRT intermedi |
| `Pipeline/tests/test_fetchimagery.py` | 13 test nuovi, sui dati veri di Torino |
| `Tools/StandaloneTests/geoimagery_main.cpp` | 8 test nuovi: mipmap e formula del materiale |

---

## Riepilogo

- Il mosaico aveva **due cause vere**: un materiale che sommava l'offset U anche
  a V, e scene Sentinel vuote a metà e di giorni diversi. Entrambe corrette:
  **rifai il materiale e riscarica le scene**.
- Le **mipmap** ora ci sono, mediate in luce lineare, con filtro anisotropico.
- **Sentinel resta a 10 m**: per volare bassi servono le tue ECW.
- Le **mesh si costruiscono sui thread di lavoro**, le **ombre del terreno sono
  spente**, i **budget seguono la RAM**. Se lagga ancora, `stat unit` dice dove.
- **Tutta l'Italia**: `fetch --area italia` per le quote, `fetch-imagery --area
  italia` per le ortofoto (96 scene, 24,7 GB, misurato).

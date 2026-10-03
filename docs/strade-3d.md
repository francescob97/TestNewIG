# Strade 3D, transizioni morbide, e i flash

> Due giri di prove.
>
> **Primo giro:** "funzionano le strade, si vedono", ma (1) da vicino le vuoi
> **3D**, e (2) muovendosi **flasha ancora**, ogni tanto.
>
> **Secondo giro:** "le strade dai, più o meno. **Manca la tridimensionalità.
> Manca il materiale e le texture.** C'è anche molto **aliasing** (le vedo
> sfrigolare). In più quando mi muovo continua sto maledetto **flash, quando
> ricalcola la geometria**. Non si può fare qualcosa di più soft? Un lerp con
> quella precedente?"
>
> Si può, ed è proprio un lerp con quella precedente. Qui cosa causava cosa e
> cosa è cambiato.

---

## 1. In una pagina

| | Cosa si vedeva | Causa | Cosa è cambiato |
|---|---|---|---|
| Flash del raffinamento | lampo quando il terreno "ricalcola la geometria" | le quattro figlie prendono il posto del padre **in un frame**: cambiano insieme forma, luce e foto | **geomorphing** e **dissolvenza**: le figlie nascono identiche al padre e diventano se stesse in 0,6 s (sezione 2) |
| Flash del rebase (primo giro) | ogni 10 km, tutto lo schermo cambia luminosità | l'origine del mondo stava alla quota della camera, cielo e nebbia saltavano | origine sempre a quota zero (sezione 6) |
| Manca la tridimensionalità | un nastro sottile, uguale alla strada dipinta | il nastro era inclinato come il terreno e alto 20 cm | **sezione trasversale vera**: carreggiata in piano, scarpate, marciapiedi con cordolo, guardrail, parapetti e pile (sezione 3) |
| Manca il materiale | strade grigie, tutte della stessa "plastica" | (a) `M_GeoRoad` cercato solo all'avvio; (b) ruvidità fissa | il materiale si ricerca ogni 2 s; la ruvidità sta nell'atlante (sezione 4) |
| Sfrigolio | pixel che lampeggiano lungo le strade | strade alla stessa quota agli incroci, strisce larghe un texel, colori della striscia accanto, sentieri sotto il pixel | quote diverse per ogni strada, strisce sfumate, bande di guardia, sentieri solo dipinti (sezione 5) |

**Da fare dopo l'aggiornamento** (tutti e tre servono):

1. ricompila;
2. nell'editor **`geo.Imagery.CreateMaterial`**: rifà `M_GeoTerrain` (ora con
   le transizioni) e `M_GeoRoad` (ora con ruvidità e geomorphing). Senza, il
   terreno funziona come prima, con il lampo, e l'overlay lo dice in rosso;
3. **rifai le strade** (`python run.py build-roads ...`, stesso comando di
   prima): la classificazione è alla versione 2 e porta i **marciapiedi**
   dai tag OSM. Con i dati vecchi le strade 3D funzionano lo stesso, ma i
   marciapiedi li hanno solo le residenziali (il default).

---

## 2. Il flash quando ricalcola la geometria

### 2.1 La causa

Quando ti avvicini, il quadtree **raffina**: una tile viene sostituita dalle
sue quattro figlie, ognuna con quattro volte il dettaglio. La sostituzione
avviene in **un frame**, e in quel frame cambiano tre cose insieme:

- la **forma**: il terreno delle figlie ha più dettaglio, quindi i vertici
  salgono e scendono di qualche metro;
- la **luce**: le normali nuove cambiano quanto è illuminato ogni pendio. È
  la parte che si nota di più: un versante intero diventa più chiaro o più
  scuro di colpo;
- la **foto**: le figlie hanno spesso un'ortofoto più nitida, a volte di un
  altro giorno, con un altro colore.

L'occhio legge "tutto cambia in un frame" come un lampo. È il *popping* del
LOD, il problema classico di ogni terreno a livelli di dettaglio.

### 2.2 Il rimedio: un lerp con quella precedente

Le figlie **nascono identiche al padre** e diventano se stesse in 0,6 secondi.
Due strumenti, entrambi nel materiale, entrambi comandati da un numero per
tile che va da 1 a 0:

**Il geomorphing (forma e luce).** Mentre si costruisce la mesh di una figlia
(sul thread di lavoro, nessun costo per il frame) si calcola per ogni vertice
quanto sta **sopra la superficie del padre** (`MorphDeltas`) e qual è la
**normale del padre** in quel punto (`ParentNormals`). La superficie del padre
è quella *disegnata* (`FSurfaceSampler`, la stessa delle strade 3D), quindi la
forma di partenza è quella che c'era a schermo, al centimetro. Il materiale:

- sposta ogni vertice in giù di `Delta × Morph` (World Position Offset);
- usa la normale `lerp(propria, padre, Morph)`.

Con `Morph = 1` la figlia è il padre; con `Morph = 0` è se stessa.

**La dissolvenza (foto e strade dipinte).** La figlia riceve come foto
"precedente" quella del padre, ritagliata sul suo quarto, e il materiale fa
`lerp(foto attuale, foto precedente, Fade)`. Lo stesso vale **sempre** quando
una tile a schermo cambia foto, non solo quando nasce: il caso più comune è la
tile che mostra la foto sgranata dell'antenato finché non arriva la propria.
Prima la foto nitida arrivava di scatto; ora sfuma.

Morph e Fade scendono da 1 a 0 con una curva morbida (*smoothstep*: parte e
arriva piano), in `geo.Terrain.Morph` secondi.

> 💡 **Perché non basta la dissolvenza.** Si potrebbe sfumare fra la mesh del
> padre e quella delle figlie disegnandole tutte e due, semitrasparenti. Ma un
> terreno semitrasparente si vede attraverso, e due superfici quasi coincidenti
> si contendono i pixel (z-fighting). Il geomorphing ha una superficie sola,
> che cambia forma.

### 2.3 Dove vivono i numeri: Custom Primitive Data

Morph e Fade cambiano a ogni frame per decine di tile. Scriverli come
parametri dell'istanza di materiale ricostruirebbe a ogni frame il proxy di
rendering di ogni istanza. Viaggiano invece come **Custom Primitive Data** del
componente: float che il motore tiene nella GPU Scene, accanto alla posizione
del componente, e che il materiale legge con un parametro scalare marcato
"Use Custom Primitive Data". Indice 0 = Morph, 1 = Fade.

Funzionano anche per le strade 3D, che hanno un'istanza di materiale sola per
tutte le tile: la strada di una tile scivola **con la sua tile**, perché ha i
suoi `MorphDeltas` (calcolati dalle stesse due superfici) e lo stesso Morph.

### 2.4 Cosa non fa

- **Il contrario non si sfuma.** Quando ti allontani, le quattro figlie
  tornano padre di colpo: il padre non sa com'erano le figlie. Allontanandosi
  il cambio è molto meno visibile (le tile sono più lontane, e il dettaglio
  che sparisce era già piccolo sullo schermo). Se si nota, si può fare con la
  stessa tecnica al contrario: il padre nasce con le forme delle figlie.
- **Le tile che compaiono dal nulla** (dopo un teletrasporto, o ai bordi della
  vista) non hanno un "prima": compaiono e basta.
- Una tile costruita quando le quote del padre non erano in memoria non ha i
  dati del morphing: compare di colpo come prima. Capita di rado (il padre è
  a schermo, le sue quote sono pinnate).

### 2.5 Se il flash c'è ancora

`geo.Terrain.Morph 0` spegne le transizioni. Muoviti, poi riaccendile con
`geo.Terrain.Morph 0.6`:

- se con 0 lampeggia e con 0,6 no, ha funzionato;
- se lampeggia uguale, il lampo non è il raffinamento. Guarda l'overlay del
  terreno (`geo.Terrain.Debug 1`): la riga **Transizioni** dice quante ne sono
  in corso. Se il lampo arriva quando il contatore è a zero, dimmi cosa stavi
  facendo: girarti, salire, scendere. E prova `geo.AutoRebase 0` (sezione 6).

Se la riga Transizioni è **rossa**: `M_GeoTerrain` è quello vecchio, rifallo
con `geo.Imagery.CreateMaterial`.

---

## 3. Le strade 3D, seconda versione

### 3.1 Perché la prima "non era 3D"

La prima versione posava un nastro largo quanto la strada, 20 cm sopra il
terreno e inclinato come lui. Dall'alto un nastro così è **identico** a una
strada dipinta, e da vicino è un foglio che galleggia. Una strada vera ha uno
**spessore** e dei **bordi**: è quello che l'occhio legge come "3D".

### 3.2 La sezione trasversale

```
                 marciapiede                          scarpata
               ______________                  ______
     cordolo  |              |\  carreggiata  /      \__  guardrail (autostrade)
  ____________|              | \____________ /          \______  terreno
```

| Pezzo | Dove | Com'è fatto |
|---|---|---|
| **Carreggiata** | tutte | **in piano di traverso**, al punto più alto del terreno sotto di lei + 15 cm (+ 2 cm per gradino di classe) |
| **Scarpata** | strade senza marciapiede, piste | dal bordo al terreno con la pendenza delle strade vere (2:3); in rilevato scende, in trincea sale; il piede entra 15 cm nel terreno |
| **Cordolo e marciapiede** | dove OSM lo dice; le residenziali di default | cordolo di 15 cm, marciapiede di 1,8 m in autobloccanti, scalino fino al suolo |
| **Guardrail** | autostrade e superstrade | una lama d'acciaio da 40 a 75 cm, ai due bordi della carreggiata |
| **Massicciata** | ferrovie | la carreggiata è il pietrisco (35 cm più su), con i fianchi in pendenza |
| **Ponti** | `bridge=*` | impalcato dritto fra le spalle, **parapetti** di 1 m, fianchi e fondo in cemento, **pile** ogni 35 m dove l'impalcato è più di 2,5 m sopra il suolo |

La carreggiata **in piano** è la differenza più visibile su un pendio: prima
la strada era inclinata di traverso come il versante; ora è orizzontale, e la
scarpata a valle (o la trincea a monte) la raccorda al terreno. È quello che
si vede di una strada di montagna.

> 💡 **Perché al punto più alto.** Il terreno che si vede non si può
> modificare (sarebbe un lavoro sulla mesh del terreno, sezione 3.6). Una
> carreggiata più bassa del terreno sotto di lei verrebbe **bucata**: il prato
> spunterebbe dall'asfalto. Al punto più alto non succede mai (il test lo
> verifica su un terreno ondulato: minimo 25 cm sopra). Il prezzo: su un
> pendio ripido il bordo a valle sta più in alto del terreno, e la scarpata
> lo raccorda. Su pendii veri (10–30%) sono decine di centimetri.

### 3.3 I marciapiedi e gli incroci

OSM dice dove sono i marciapiedi con `sidewalk=both|left|right|no|separate`
(e `sidewalk:left=...`, `sidewalk:right=...`). La pipeline ora li legge
(classificazione versione 2) e li scrive in tre bit dei flag. Il C++ li
interpreta: sinistra e destra **nel verso della linea**, come OSM. `separate`
vuol dire che il marciapiede è mappato a parte, come sentiero: non si disegna
due volte. Senza tag, le residenziali hanno i marciapiedi e le altre no.

**Gli incroci.** Un marciapiede che proseguisse dritto attraverso un incrocio
sarebbe un gradino di 15 cm in mezzo alla strada che incrocia. Prima di
costruire una tile si fa un indice di tutte le carreggiate; marciapiedi,
scarpate e guardrail **si interrompono** dove cadono sulla carreggiata di
un'altra strada. Attorno a ogni incrocio si mettono sezioni ogni metro e mezzo,
perché le sezioni normali (dove la strada taglia gli spigoli della mesh) sono
ogni 5–10 m, e una via larga 5 m potrebbe stare tutta fra due di loro.

Il test: due residenziali a croce, nessun vertice di marciapiede dentro l'altra
carreggiata.

### 3.4 Cosa diventa 3D e cosa no

- Sì: tutte le strade da autostrada a tratturo, ferrovie, piste.
- **No: i sentieri.** Larghi un metro e mezzo, a un chilometro sono meno di un
  pixel: un nastro così appare e scompare fra un frame e l'altro (sfrigolio),
  e dipinti sulla foto si vedono già bene.

Dove compaiono non è cambiato: sulle tile di terreno al livello più fine,
cioè sotto i 1.000–1.500 m dal suolo con il profilo portatile; più lontano
con `geo.Roads.3DLevels 2` (il profilo workstation lo fa già).

### 3.5 Quanto costa

Una tile "di città" di prova, con 3.000 strade corte che si incrociano
dappertutto (molto più fitta di Torino): **~250 ms** su un thread di lavoro,
~520.000 triangoli. La prima versione ne faceva 90.000: ogni sezione ora ha
fino a sette facce invece di una. Una tile vera di Torino ha circa un sesto di
quelle strade: ~100.000 triangoli. L'overlay delle strade (`geo.Roads.Debug 1`)
dice i triangoli 3D in memoria; se è troppo per il portatile, `geo.Roads.3D 0`
o `geo.Roads.3DLevels 1`.

### 3.6 Cosa non fa ancora

- **I cavalcavia non salgono**: un ponte sopra un'altra strada in pianura ha
  le spalle a quota del terreno, e resta a terra.
- **Il terreno non si spiana sotto la strada**: la scarpata raccorda, ma il
  terreno resta quello del DEM, e su un pendio ripido la scarpata può essere
  alta qualche metro.
- **Niente lampioni, segnali, alberi lungo la strada**: arrivano con gli
  oggetti (Fasi 10–11).
- La segnaletica non conta le corsie: mezzeria e bordi.

---

## 4. "Manca il materiale e le texture"

Due cause, entrambe corrette.

**(a) `M_GeoRoad` cercato una volta sola.** Il provider cercava i materiali
all'avvio del terreno. Se `geo.Imagery.CreateMaterial` veniva lanciato dopo
(con il Play già partito, o prima di aver aggiornato il codice), il materiale
c'era ma il provider non lo sapeva: strade 3D grigie fino al Play successivo,
con l'overlay che continuava a dire "manca M_GeoRoad". Ora i materiali si
**ricercano ogni 2 secondi**, e quando compaiono si applicano alle strade già
costruite.

**(b) Tutto della stessa plastica.** `M_GeoRoad` aveva la ruvidità fissa a
0,85. Ora l'atlante ha la ruvidità nel **canale alfa**: l'asfalto è opaco,
le strisce un po' meno, rotaie e guardrail sono metallo che luccica col sole
basso. È la differenza fra "un colore" e "un materiale".

L'atlante stesso è più ricco: 16 strisce da 64 pixel (prima 8 da 32), con
cordolo, scarpata, massicciata, acciaio del guardrail, lastre di cemento con i
giunti, autobloccanti.

Nell'overlay delle strade la riga **Materiale 3D** dice se `M_GeoRoad` c'è e ha
l'atlante. Se le strade 3D sono ancora grigie e quella riga è bianca, mandami
uno screenshot: è un caso che non ho previsto.

---

## 5. Lo sfrigolio

Quattro cause, tutte da "pixel che lampeggiano lungo le strade":

| Causa | Rimedio |
|---|---|
| **Due strade alla stessa quota** agli incroci (stessa classe, stesso sollevamento): il renderer sceglie pixel per pixel quale disegnare, in modo diverso a ogni frame (*z-fighting*) | ogni strada ha qualche millimetro in più dell'altra (25 valori diversi); fra classi diverse restano i 2 cm per gradino |
| **Strisce dipinte larghe un texel**, "dentro o fuori": a seconda di dove cade il texel rispetto ai pixel dello schermo, la striscia c'è o non c'è | strisce disegnate con la **copertura** (bordi sfumati nella texture), larghe ~1,3 texel |
| **Il colore della striscia accanto**: da lontano si leggono i mip piccoli dell'atlante, e il filtro prende anche il texel vicino, cioè l'altra superficie (asfalto con il bordo color binario) | **bande di guardia**: 8 pixel per lato che ripetono il bordo; la mesh legge solo i 48 centrali. E le mipmap a blocchi 2×2 su strisce larghe una potenza di due non mescolano mai due strisce (verificato al mip 4) |
| **Nastri più sottili di un pixel** (i sentieri) | i sentieri restano dipinti |

Quello che resta è l'aliasing normale dei bordi della geometria, che è compito
dell'anti-aliasing del motore (TSR, il default di UE5). Se lo vedi ancora,
dimmi se è **su tutta la strada** (texture), **ai bordi** (geometria), o **agli
incroci** (quote): sono tre rimedi diversi.

---

## 6. Il flash del rebase (primo giro)

Il motore lavora vicino a un'**origine** che si sposta con la camera (il
*rebase*, capitolo 2 del manuale): ogni 10 km percorsi l'origine si porta sotto
la camera. Prima l'origine nuova era la posizione della camera **compresa la
quota**, e Unreal ancora alla Z = 0 il **cielo** (`SkyAtmosphere`) e la
**nebbia** (`ExponentialHeightFog`): a ogni rebase il terreno saltava, rispetto
a cielo e nebbia, di quanto si era saliti o scesi, e tutto lo schermo cambiava
luminosità in un frame.

Ora l'origine sta **sempre a quota zero** (la Z del mondo è la quota) e il
rebase scatta sulla distanza **orizzontale**. Per escluderlo come causa di un
lampo: `geo.AutoRebase 0`, e guarda il contatore dei rebase in `geo.Debug 1`.

---

## 7. Come provarla

```bat
git pull
```

1. Ricompila (se Visual Studio non vede file nuovi, rigenera i file di
   progetto).
2. Nell'editor: **`geo.Imagery.CreateMaterial`**.
3. Rifai le strade, per i marciapiedi:
   ```bat
   python run.py build-roads -i dati\osm\nord-ovest-latest.osm.pbf -o dataset\strade_torino --bbox 7.55 45.00 7.80 45.15
   ```
   La classificazione è cambiata: la pipeline lo riconosce e rifà le tile
   invece di mescolare le due versioni.
4. Play, poi `geo.Roads.Demo dataset\terreno dataset\ortofoto_torino_v2 dataset\strade_torino`.
5. **Il flash:** `geo.Terrain.Debug 1`, scendi e avvicinati a una collina.
   Guarda la riga Transizioni mentre le tile si affinano. Confronta con
   `geo.Terrain.Morph 0` (di colpo) e `geo.Terrain.Morph 2` (lentissimo: si
   vede bene la forma che scivola).
6. **Le strade:** scendi sotto i 500 m su Torino, `geo.Roads.Debug 1`. Guarda
   le righe "Strade 3D" e "Materiale 3D". Cerca un incrocio, un viale con i
   marciapiedi, la tangenziale (guardrail), un ponte sul Po (parapetti).

| Comando | Cosa fa |
|---|---|
| `geo.Terrain.Morph <secondi>` | durata delle transizioni (default 0,6; 0 = di colpo) |
| `geo.Roads.3D 0\|1` | strade 3D accese o spente |
| `geo.Roads.3DLevels <1..3>` | su quanti livelli fini (più livelli = più lontano, più triangoli) |
| `geo.AutoRebase 0\|1` | per escludere il rebase (sezione 6) |

---

## 8. Dove sta nel codice

| File | Cosa |
|---|---|
| `GeoRender/Public/Mesh/TileMesh.h` | `FSurfaceSampler` (la superficie disegnata) e `ComputeMorphTargets` |
| `GeoRender/Public/Terrain/GeoTerrainMeshProvider.h` | l'interfaccia delle transizioni: `BeginTransitionFromParent`, `TickTransitions` |
| `GeoRender/Private/Terrain/DynamicMeshTerrainProvider.cpp` | UV 1 e 2 con i dati del morphing, Custom Primitive Data, foto precedente, materiali ricercati ogni 2 s |
| `GeoRender/Private/Terrain/GeoTerrainSubsystem.cpp` | chi compare al posto di un antenato; le quote del padre al worker |
| `GeoRender/Public/Roads/RoadMesh.h` | sezione trasversale, marciapiedi, incroci, guardrail, ponti e pile, atlante con ruvidità e bande di guardia |
| `GeoRender/Private/Roads/GeoRoadsSubsystem.cpp` | le quote del padre anche per le strade; riga "Materiale 3D" |
| `GeoWorldEditor/Private/GeoTerrainMaterialFactory.cpp` | `M_GeoTerrain` con dissolvenza, WPO e normali; `M_GeoRoad` con ruvidità e WPO |
| `Pipeline/geoworld/roadclasses.py` | i marciapiedi dai tag OSM (`FLAG_SIDEWALK_*`, classificazione 2) |
| `Tools/StandaloneTests/geomesh_main.cpp` | sezione 9: i dati del morphing |
| `Tools/StandaloneTests/georoads_main.cpp` | sezioni 8–12: quote, sezione trasversale, ponti e pile, marciapiedi e incroci, atlante, costo |

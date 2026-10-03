# Strade 3D, e il flash che restava

> Dopo la prima prova della Fase 8: "funzionano le strade, si vedono", ma
> (1) da vicino le vuoi **3D**, e (2) muovendosi **flasha ancora**, ogni tanto.
> Qui cosa causava il flash, cosa è cambiato, e come sono fatte le strade 3D.

---

## 1. In una pagina

| | Cosa | Cosa è cambiato |
|---|---|---|
| Flash | a ogni **rebase** (ogni 10 km percorsi) il terreno saltava rispetto a cielo e nebbia | l'origine del mondo sta sempre a **quota zero**; il rebase guarda la distanza orizzontale |
| Strade 3D | sulle tile di terreno più fini le strade diventano **geometria**: asfalto con segnaletica, binari con traversine, ponti sollevati | nuovo `RoadMesh.h` (C++ puro, 21 test), un componente per tile nel provider, il materiale `M_GeoRoad` |

**Da fare dopo l'aggiornamento:** ricompila e rilancia **`geo.Imagery.CreateMaterial`**, che ora crea anche `M_GeoRoad`. Senza, le strade 3D restano grigie, e l'overlay delle strade lo segnala in rosso.

---

## 2. Il flash

### 2.1 La causa

Il motore lavora vicino a un'**origine** che si sposta con la camera (il
*rebase*, capitolo 2 del manuale): ogni 10 km percorsi l'origine si porta sotto
la camera, e tutto il mondo viene ricalcolato attorno a lei. Geometricamente
non cambia niente: il terreno resta dov'è, la camera resta dov'è.

Ma l'origine nuova era la posizione della camera **compresa la quota**. A 2.500 m
la Z = 0 del mondo stava 2.500 m sopra il terreno. E Unreal ancora alla Z = 0
due cose che si vedono su tutto lo schermo:

- il **cielo** (`SkyAtmosphere`): di default il "pianeta" ha la cima
  nell'origine del mondo, quindi il terreno stava 2.500 m *sotto* la superficie
  del pianeta dell'atmosfera;
- la **nebbia** (`ExponentialHeightFog`): la sua densità dipende dalla Z del
  mondo, e cresce scendendo.

A ogni rebase il terreno saltava, rispetto a cielo e nebbia, di quanto si era
saliti o scesi dall'ultimo. Cambiavano di colpo quanta nebbia e quanta atmosfera
ci sono fra te e il suolo: **tutto lo schermo cambiava luminosità in un frame**.
Ogni 10 km, cioè "ogni tanto, muovendosi". Anche il primo salto (`geo.Roads.Demo`
che ti porta a 2,5 km) metteva l'origine a 2,5 km di quota.

### 2.2 Cosa è cambiato

- L'origine sta **sempre a quota zero**, sull'ellissoide sotto la camera. La Z
  del mondo è la quota (a meno della curvatura: 8 m a 10 km dall'origine). Il
  terreno sta dove cielo e nebbia se lo aspettano, e un rebase sposta il mondo
  solo in orizzontale, di qualche metro in verticale al massimo.
- Il rebase scatta sulla distanza **orizzontale** dall'origine. Con l'origine a
  quota zero e la distanza 3D, un aereo a 12 km di quota sarebbe sempre "oltre
  soglia".

### 2.3 Se il flash c'è ancora

Ho trovato e tolto una causa certa, ma non posso vedere il tuo schermo. Se
ricompare, due prove da 30 secondi ciascuna mi dicono dov'è:

1. `geo.AutoRebase 0`, poi muoviti per un paio di minuti. Se il flash sparisce,
   è ancora legato al rebase: dimmelo. Il prossimo sospettato sono le cache del
   renderer (ombre, illuminazione globale), che vedono la camera "saltare" di
   10 km.
2. `geo.Roads.Enable 0` e poi `geo.Imagery.Enable 0`. Se il flash sparisce
   spegnendo uno dei due, è un cambio di materiale: dimmi quale dei due.

E guarda il contatore dei rebase nell'overlay della georeferenziazione
(`geo.Debug 1`): se cresce nello stesso istante del flash, abbiamo la conferma.

---

## 3. Le strade 3D

### 3.1 Dove compaiono

Sulle tile di terreno al **livello più fine** del DEM (il 14 con TINITALY, il 13
con DTED e Copernicus). Il quadtree le sceglie solo vicino alla camera: con il
profilo portatile entro ~2 km, con il workstation entro ~4 km. In pratica:

- **sotto i 1.000–1.500 m dal suolo** le strade attorno a te sono 3D;
- **salendo** il quadtree smette di scegliere quelle tile, e le strade tornano
  dipinte da sole. È il comportamento che chiedevi: 3D da bassi, 2D da alti.

`geo.Roads.3DLevels 2` estende il 3D al livello successivo (il doppio della
distanza, circa il quadruplo dei triangoli). Il profilo `workstation` lo fa già.

### 3.2 Come si sta sul terreno

È il problema vero. Il terreno che vedi non è il DEM: è una mesh con un post
ogni due e ogni cella divisa lungo la diagonale. Una strada che prendesse le
quote dal DEM starebbe in certi punti sotto la superficie disegnata (e sparirebbe
a tratti) e in altri sopra (e galleggerebbe). Quindi:

1. la quota si prende **dalla mesh**, nello stesso triangolo che il terreno
   disegna (`FSurfaceSampler`, verificato contro i vertici di `BuildTileMesh`);
2. la strada si **spezza** dove l'asse o uno dei due bordi attraversano uno
   spigolo della mesh (linee della griglia e diagonali). Fra due tagli il terreno
   è un piano, e il nastro ci sta sopra;
3. la si **solleva** di 20 cm, più 2 cm per ogni gradino di importanza:
   all'incrocio fra una primaria e una residenziale ci sono 6 cm di differenza,
   e le due superfici non si contendono lo stesso piano (niente z-fighting).

Il test misura ogni vertice su un terreno ondulato: tutti fra 28 e 35 cm sopra
la superficie disegnata.

### 3.3 I ponti

Un ponte dipinto sta sul fondo della valle. In 3D no: l'impalcato va **dritto
fra le due spalle** (la quota del terreno dove il ponte comincia e dove finisce),
non scende mai sotto il terreno, e ha **fianchi e fondo** in cemento: da sotto e
di lato non è un foglio di carta. Il test: su una valle profonda 60 m, la strada
normale scende a 340 m, l'impalcato resta a 400 m.

Le spalle possono cadere fuori dalla tile di terreno: per quelle si usano le
quote dell'antenato che copre tutta la tile vettoriale, se è in memoria.

### 3.4 L'aspetto

Una texture sola, generata dal codice (niente file binari da mantenere), divisa
in otto strisce:

| Striscia | Per |
|---|---|
| asfalto con segnaletica | autostrade, statali, provinciali: bordi bianchi e mezzeria tratteggiata (4,5 m su 12, come quella extraurbana) |
| asfalto semplice | strade locali e residenziali, servizio |
| sterrato | tratturi e strade sterrate, con i due solchi delle ruote |
| ferrovia | massicciata, traversine ogni 60 cm, due rotaie a scartamento normale |
| cemento | piste d'aeroporto, fianchi dei ponti |
| lastricato | zone pedonali |
| ghiaia | sentieri |

### 3.5 Quanto costa

Misurato nei test: una tile "di città" con 3.000 strade costa **29 ms** su un
thread di lavoro e produce ~90.000 triangoli. Una tile vera di Torino ne ha di
meno. Le costruzioni sono al massimo 4 in parallelo e le consegne 4 per frame
(16 dopo un salto). L'overlay delle strade mostra i triangoli 3D in memoria.

### 3.6 Le regole contro i flash, anche qui

- La strada 3D è **parte della tile**: stessa trasformazione, stessa visibilità,
  stesso rebase, e sparisce con lei. Non può restare a schermo senza il suo
  terreno, né il terreno comparire e la strada un frame dopo.
- Una tile nuova compare **solo con la sua strada 3D pronta**, se le spetta.
- Una tile **già a schermo** non viene mai "svestita" per colpa del 3D: se la
  sua strada 3D manca (quote buttate dalla cache, un cambio di passo), resta con
  le strade dipinte e la 3D arriva dopo. Svestirla farebbe tornare il padre per
  qualche frame: un lampo.

### 3.7 Cosa non fa

- **I cavalcavia non salgono**: un ponte sopra un'altra strada in pianura ha le
  spalle a quota del terreno, e resta a terra. Servirebbe sapere dove passa la
  strada di sotto, e lo farò se si nota.
- **Il terreno non si spiana sotto la strada**: su un versante ripido la strada
  segue la pendenza anche di traverso, mentre una strada vera è in piano con una
  scarpata. Il rimedio (abbassare o alzare il terreno lungo la strada) è un
  lavoro sulla mesh del terreno.
- **Niente marciapiedi, guardrail, lampioni**: arrivano con gli oggetti della
  Fase 10.
- La **segnaletica** non conta le corsie: c'è la mezzeria e ci sono i bordi.

---

## 4. Come provarla

```bat
git pull
```

1. Ricompila (file nuovi in GeoRender: se Visual Studio non li vede, rigenera i
   file di progetto).
2. Nell'editor: **`geo.Imagery.CreateMaterial`** (rifà `M_GeoTerrain` e crea
   `M_GeoRoad`).
3. Play, poi `geo.Roads.Demo dataset\terreno dataset\ortofoto_torino_v2 dataset\strade_torino`.
4. **Scendi** sotto i 1.000 m dal suolo. Con `geo.Roads.Debug 1` la riga
   "Strade 3D" dice quante tile le hanno, quante aspettano e quanti triangoli.

| Comando | Cosa fa |
|---|---|
| `geo.Roads.3D 0\|1` | strade 3D accese o spente |
| `geo.Roads.3DLevels <1..3>` | su quanti livelli fini (più livelli = più lontano, più triangoli) |
| `geo.AutoRebase 0\|1` | per la prova del flash (sezione 2.3) |

---

## 5. Dove sta nel codice

| File | Cosa |
|---|---|
| `GeoCore/Private/Georeference/GeoreferenceSubsystem.cpp` | origine a quota zero, rebase sulla distanza orizzontale |
| `GeoRender/Public/Roads/RoadMesh.h` | **nuovo**: superficie disegnata, nastri, ponti, atlante |
| `GeoRender/Private/Roads/GeoRoadsSubsystem.cpp` | costruzione 3D sui worker, consegna, regole di vestizione |
| `GeoRender/.../Terrain/*Provider*` | componente strada per tile: `CommitRoadMesh`, `RemoveRoadMesh`, visibilità e rebase con la tile |
| `GeoRender/.../Imagery/GeoRuntimeTexture.*` | texture ripetuta lungo V (per l'atlante) |
| `GeoWorldEditor/Private/GeoTerrainMaterialFactory.cpp` | crea anche `M_GeoRoad` |
| `Tools/StandaloneTests/georoads_main.cpp` | sezioni 8–11: quote, larghezze, verso delle facce, ponti, atlante, costo |

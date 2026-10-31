# Capitolo 10 — Glossario e mappa dei file

---

## Glossario

Termini in ordine alfabetico. Tra parentesi, il capitolo dove sono spiegati.

**Acqua appoggiata** — un'acqua troppo stretta per la mesh del terreno a quel livello (meno di 4 post): non si scava, e la sua superficie sta sul terreno, 0,3 m sopra, con gli stessi vertici. La forma la disegna la maschera della riva. (13)

**Acqua risolta** — un'acqua larga almeno 4 post al livello della tile: si scava, con la sponda a V, e la superficie è piana. (13)

**Albedo** — in un materiale di dominio Volume, quanta della luce che una nuvola intercetta viene diffusa invece che assorbita. Le nuvole sono quasi bianche: 0,9. (15)

**Alfa premoltiplicato** — un'immagine trasparente il cui colore è già moltiplicato per la
copertura. Le medie fra pixel (mipmap, filtro bilineare) vengono giuste; in alfa normale i
bordi si scurirebbero. Le strade sono così. (12)

**Antenato** — la tile di un livello più grossolano che contiene una tile data.
L'antenato di livello `La` di `(L, X, Y)` è `(La, X >> d, Y >> d)` con `d = L − La`. (7)

**Assorbimento e diffusione** — i due coefficienti, per metro e per canale di colore, con cui Single Layer Water colora l'acqua: quanta luce si perde e quanta rimbalza nell'acqua. Il mare assorbe il rosso molto più del blu: per questo l'acqua profonda è blu. (13)

**Attore** (`AActor`) — un oggetto che esiste nella scena di Unreal, con una
posizione. È fatto di componenti. (1)

**Ballpark** — la "trasformazione approssimata" di PROJ, che in mancanza della
griglia geoidica non applica lo scostamento verticale, in silenzio. (3)

**Banda (della vegetazione)** — quanti alberi tiene una cella secondo la distanza dalla camera: vicino (fino a 700 m) tutti e con l'ombra, medio (1.500 m) la metà, lontano (2.500 m) un quarto. Si cambia banda solo 75 m oltre il confine (isteresi). (16)

**BC1 / DXT1** — compressione delle texture che la scheda video legge
direttamente. Non usata oggi; il formato è pronto per aggiungerla. (7)

**Banda di guardia** — pixel attorno a una striscia di un atlante che ripetono il suo bordo.
Il filtro della scheda video legge anche i texel accanto a quello chiesto; senza banda, il
bordo di una superficie prende il colore della vicina. (12)

**Bounds** — il volume che contiene una primitiva. Il renderer lo usa per decidere
se è visibile. (1, 6)

**Bowring** — formula approssimata (1976) per passare da ECEF a latitudine,
longitudine, quota in un passo solo. Qui con una raffinazione. (2)

**Buffer (di una tile vettoriale)** — quanto le linee escono dalla tile: un ottavo del
lato. Serve perché una strada larga lungo il bordo compaia anche nella tile accanto. (12)

**Build unity** — Unreal compila più `.cpp` dello stesso modulo come un file solo,
per fare prima. Fa scontrare le funzioni omonime nei namespace anonimi. (1)

**CDO** (*Class Default Object*) — l'oggetto "prototipo" che Unreal crea per ogni
classe `UCLASS`, con i valori di default. I file `.ini` vengono applicati a lui. (1)

**Cella (della vegetazione)** — una tile del livello 15 (~610 x 430 m) dentro cui gli alberi si costruiscono insieme, su un worker, e diventano qualche componente di istanze. Solo quelle entro 2,5 km dalla camera. (16)

**Copertura del suolo** (*land cover*) — una mappa che divide il territorio in classi: bosco, arbusti, prato, campi, costruito, acqua, roccia, neve. La nostra è ESA WorldCover. (16)

**Custom Primitive Data** — float che un componente di Unreal porta con sé nella GPU Scene,
letti dal materiale come parametri. Si possono cambiare a ogni frame senza ricostruire
l'istanza di materiale. Qui: Morph e Fade delle transizioni. (6)

**Coefficiente di estinzione** — quanta luce una nebbia, una pioggia o una nevicata toglie per chilometro. La pioggia ha `0,29 · R^0,67` per km (R in mm/h), la neve `3,9 · S^0,75`: dieci volte tanto a parità d'acqua. Le estinzioni di cose diverse si sommano. (15)

**Colpo di ritorno** — ognuna delle scariche che passano sullo stesso canale di un fulmine: in media tre, a 30–90 ms l'una dall'altra. È per questo che il lampo sfarfalla. (15)

**Componente** (`UActorComponent`) — un pezzo di un attore che gli dà una capacità:
disegnarsi, muoversi, stare a una coordinata geografica. (1)

**Copertura** — la percentuale di una tile di immagine effettivamente coperta
dall'ortofoto. (7)

**Copertura (della precipitazione)** — quanta parte del suolo è sotto la precipitazione: 1 un fronte che copre tutto, 0,3 rovesci sparsi. Decide la soglia sulla mappa dei rovesci, con l'inversa della normale. (15)

**COG** (*Cloud Optimized GeoTIFF*) — un GeoTIFF organizzato in modo che se ne
possano leggere pezzi via rete senza scaricarlo tutto. (7)

**Cumulo** — la nuvola di bel tempo: una chiazza con la base piatta (dove l'aria che sale condensa, alla stessa quota per tutte) e la cima a cupola. Quanti, lo dice la copertura (`geo.Weather.Clouds`). (15)

**Delegato** — un meccanismo di Unreal per "avvisami quando succede X".
`OnGeoreferenceRebased` è un delegato. (2)

**DEM** (*Digital Elevation Model*) — una griglia di quote del terreno. (3)

**Draw call** — una richiesta di disegno dalla CPU alla scheda video. Ogni tile
oggi ne costa una. (6)

**ECEF** (*Earth-Centered, Earth-Fixed*) — coordinate cartesiane con l'origine nel
centro della Terra. **L'unica autorità** del progetto. (2)

**Editor / PIE / Game** — i tre contesti in cui gira il codice. Nell'editor la
camera non è un attore. (1)

**Ellissoide** — la forma matematica della Terra: un'ellisse fatta ruotare. (2)

**Errore geometrico** — di quanti metri, al massimo, un livello si discosta dal
terreno vero. Qui approssimato con il passo fra i post. (5)

**Errore su schermo** (SSE) — l'errore geometrico proiettato in pixel. La manopola
del LOD. Soglia di default: 8 pixel (era 4: vedi `docs/prova-torino-2.md`). (5)

**ESA WorldCover** — la copertura del suolo di tutto il mondo a 10 m, fatta dall'Agenzia Spaziale Europea con Sentinel-1 e Sentinel-2 (2021, CC BY 4.0). File COG da 3 x 3 gradi su un bucket pubblico, letti a pezzi dalla pipeline. (16)

**Esposizione (alle onde)** — quanto un punto d'acqua sente le onde del vento: 1 il mare aperto, 0,35 una laguna, da 0,12 a 0,8 un lago secondo la grandezza (il *fetch*, la distanza su cui il vento spinge), quasi 0 fiumi e canali. Sta nel canale G del raster. (13)

**ExponentialHeightFog** — l'attore della nebbia di Unreal: una densità che cala con la quota. La precipitazione ne alza la densità (`56,4 / visibilità in m`) e la rimette com'era; nell'editor quella del livello non si tocca, perché si salverebbe. (15)

**Fascia** (del piano di residenza) — un gruppo di tile del piano con la stessa urgenza: 0 = adesso, 1..3 = posizioni previste, l'ultima = anello di sicurezza. (11)

**Firma del terreno** — un hash delle tile di terreno visibili sotto una cella (e del passo della mesh). Quando cambia, il terreno sotto si è affinato e la cella si rifà sopra il terreno nuovo. (16)

**Flow map** — far scorrere una texture lungo una corrente con due fasi sfalsate di mezzo ciclo, ognuna che ricomincia quando ha peso zero: un flusso continuo senza che la texture si stiri. (13)

**FMeshDescription** — il formato "modificabile" delle mesh in Unreal: vertici, istanze di vertice (normale, colore, UV), triangoli, gruppi di materiale. Da qui `BuildFromMeshDescriptions` fa una `UStaticMesh`, anche a runtime. (16)

**Frame locale** — un sistema di assi appoggiato sulla superficie in un punto.
Nel progetto è sempre NEU. (2, 6)

**Frustum** — la piramide tronca che la camera vede. (5)

**Game thread** — il thread principale di Unreal, dove girano tutti i `Tick`. Non
va mai bloccato. (1)

**Garbage collector** (GC) — il meccanismo che cancella da solo gli `UObject`
che nessuno usa più. Vede solo i riferimenti che conosce. (1)

**GDAL** — la libreria standard per leggere e trasformare dati geografici. (3)

**Gelicidio** — pioggia che arriva liquida su un suolo sotto zero e gela al contatto, coprendolo di vetrato. Nel modello: pioggia imposta con il gelo. (15)

**Geofabrik** — chi ripubblica ogni giorno OpenStreetMap tagliato per nazione e regione, in
`.osm.pbf`. Da lì scarica `fetch-osm`. (12)

**Geoide** — la superficie del livello medio del mare, irregolare. (3)

**Geomorphing** — far nascere una tile più fine con la forma e la luce della tile che
sostituisce, e farla scivolare verso le proprie in qualche decimo di secondo. Toglie il lampo
del raffinamento. (6)

**Gerstner (onda di)** — un'onda in cui i punti della superficie girano in piccoli cerchi invece di andare solo su e giù: creste strette, cavi larghi. La somma di sei fa il mare del progetto. (13)

**Gonna** (*skirt*) — striscia verticale che scende dal bordo di una tile, per
nascondere le crepe fra livelli diversi. (6)

**Grandine posata** — la grandine al suolo, in strisce sotto il percorso dei nuclei dei temporali; una griglia di celle da 470 m ferma rispetto al terreno, che fonde con il caldo (`GroundHail.h`). (15)

**Griglia fissa (degli alberi)** — un punto ogni 8 x 8 m numerato dall'equatore e da Greenwich, ognuno con il suo hash: lo stesso albero sta sempre nello stesso punto, da qualunque cella lo si guardi. (16)

**Header-only** — codice scritto interamente negli header, con `inline`. Lo strato
puro è così, per non dover esportare simboli dalle DLL. (0, 1)

**HISM** (`UHierarchicalInstancedStaticMeshComponent`) — un componente che disegna migliaia di copie della stessa mesh, raggruppate in un albero gerarchico: per ogni gruppo sceglie se è visibile e quale LOD. Gli alberi sono HISM. (16)

**Horizon culling** — scartare ciò che sta oltre la curvatura della Terra. Oggi sul rettangolo della tile nello spazio scalato. (5, 11)

**Indice** (`index.bin`) — per ogni livello, l'elenco delle tile esistenti con
quota minima e massima (quote) o copertura (immagini). (3)

**Insieme ideale** — le tile che si disegnerebbero da un punto se tutto fosse già in memoria. È la base del piano di residenza. (11)

**Isteresi** — due soglie diverse per entrare e per uscire da uno stato (riscaldamento: entra sotto il 60%, esce sopra il 95%; raffinamento di una tile: si entra alla soglia, si esce all'80%), per non oscillare attorno a una soglia unica. (11)

**Koschmieder** — la regola della visibilità: si vede fin dove il contrasto scende al 2%, cioè `visibilità = 3,912 / estinzione`. (15)

**Latitudine parametrica** — tan β = (b/a)·tan φ. È la latitudine di un punto dell'ellissoide nello spazio scalato, dove l'ellissoide diventa una sfera. (11)

**Limite del bosco** — la quota oltre la quale non crescono alberi (2.100 m s.l.m.): sopra solo arbusti, sopra i 2.700 m niente. Vicino al limite tutto si fa più piccolo. (16)

**LOD** (*Level Of Detail*) — usare versioni più semplici di ciò che è lontano. (5)

**LRU** (*Least Recently Used*) — cache che butta per primo ciò che non si usa da
più tempo. (4)

**Large World Coordinates** (LWC) — in Unreal 5, `FVector` in `double`. Nei
materiali la posizione nel mondo è LWC (due float): un nodo Custom la vuole come
float normale, e per questo passa da un nodo `TruncateLWC`. (1, 2, 13)

**Manifest** — il file `manifest.json` che descrive un dataset. (3)

**Mappa meteo** — la texture di 256 × 256 texel (160 km attorno alla camera) con le nubi di pioggia, calcolata dal C++ su un thread di lavoro ogni 8 s e spostata dallo shader con il vento fra un ricalcolo e l'altro. (15)

**Mare aperto** — il mare lontano dalla costa, dove il dataset del terreno non ha tile (nemmeno un post di dato). `build-water` annota le tile che mancano e sono tutte mare (`opensea.bin`), e il runtime ci costruisce terreno e acqua virtuali. (13)

**MarkRenderStateDirty** — dire a Unreal che una proprietà di disegno è cambiata e
il proxy di scena va rifatto. (6)

**Material Parameter Collection** (MPC) — un insieme di parametri globali condiviso da tutti i materiali che lo usano. Le onde stanno in `MPC_GeoWater`: una scrittura per frame per tutto il mondo. (13)

**micromamba** — un gestore di pacchetti conda in un solo eseguibile, senza installazione. `geoworld.bat` lo scarica (versione e impronta SHA-256 fissate) e ci prepara l'ambiente della pipeline in `Pipeline/ambiente/`. (3)

**MGRS** — il sistema di quadrati da 100 km usato per organizzare le scene
Sentinel-2. (7)

**MID** (`UMaterialInstanceDynamic`) — una copia di un materiale con parametri
modificabili a runtime. (1, 7)

**Modulo** — un'unità di compilazione di Unreal, che diventa una DLL. (1)

**MPSC** (*multiple producers, single consumer*) — una coda in cui molti thread
aggiungono e uno solo toglie. (4)

**NEU** (*North-East-Up*) — l'ordine degli assi scelto per il progetto: Nord → X,
Est → Y, Alto → Z. (2)

**Nodo Custom** — un nodo di un materiale di Unreal che contiene codice HLSL scritto a mano. Le onde e la normale dell'acqua sono due nodi Custom. (13)

**Normale geodetica** — la perpendicolare all'ellissoide. Non punta al centro
della Terra. (2)

**Nucleo (di un rovescio)** — il centro di un rovescio, dove l'intensità sale al massimo. La grandine cade lì, mai ai margini. (15)

**Ondulazione** (`N`) — la differenza fra geoide ed ellissoide. In Italia fra +42
e +52 m. (3)

**OpenStreetMap (OSM)** — la mappa libera del mondo fatta dai volontari: strade, ferrovie,
edifici, boschi, acqua. Licenza ODbL, da citare. Fonte della Fase 8 e delle successive. (12)

**Nube-suolo** — il fulmine che arriva a terra, circa uno su tre; gli altri restano dentro la nube e si vedono solo come il cielo che si illumina. (15)

**Ortofoto** — immagine aerea o satellitare raddrizzata, in cui ogni pixel sta alla
sua coordinata. (7)

**Per Instance Custom Data** — qualche numero per ogni istanza di un componente di istanze, letto dal materiale con il nodo PerInstanceCustomData. Per gli alberi: il verde, l'altezza, la fase del vento. (16)

**Piano di residenza** — la lista, in ordine di urgenza, delle tile da tenere pronte: qui, dove si sarà fra qualche secondo, e un anello di sicurezza. Rifatto ogni 0,25 s. (11)

**Pierson-Moskowitz** — lo spettro di un mare "completamente sviluppato" (1964): dal vento U dà l'altezza significativa Hs = 0,21·U²/g e la lunghezza d'onda di picco. Da lì si scelgono le onde. (13)

**Pin** — marcare una tile della cache come "non sfrattabile". (4)

**Plugin** — un pacchetto di codice e contenuti riutilizzabile. (1)

**Poisson diradato** (processo di) — un modo di estrarre eventi a caso con una densità che cambia da punto a punto: si tirano punti come se la densità fosse massima ovunque, e ognuno si tiene con la probabilità della densità del suo posto. Così nascono i fulmini nei nuclei dei temporali. (15)

**Post** — un punto di campionamento di una griglia di quote. (3)

**Post riempito** — un post del terreno dove il sorgente non aveva dati, scritto dalla pipeline con il livello del mare. Per l'acqua non è terreno: si riconosce perché vale esattamente il geoide. (13)

**Previsione del moto** — stima della velocità della camera dalle sue posizioni (`FMotionPredictor`) e proiezione in avanti su una retta. (11)

**Primitiva** (`UPrimitiveComponent`) — un componente che si disegna. (1)

**PROJ** — la libreria di proiezioni cartografiche usata da GDAL. (3)

**Provider** — l'implementazione dell'interfaccia `IGeoTerrainMeshProvider`, che
trasforma mesh astratte in qualcosa che Unreal disegna. (6)

**Proxy di scena** (`FPrimitiveSceneProxy`) — la copia di una primitiva che vive
sul thread di rendering. (1)

**Quadtree** — albero in cui ogni nodo ha quattro figli. La piramide delle tile. (5)

**Quadrato degenere** — un quadrato con i quattro vertici nello stesso punto: non si vede. Le particelle della pioggia sono quadrati degeneri che il materiale apre (World Position Offset) solo dove serve. (15)

**Quota ellissoidica / ortometrica** — sopra l'ellissoide / sopra il livello del
mare. Il motore usa solo la prima. (3)

**Quota neve** — la quota sopra cui la precipitazione cade come neve: 300 m sotto lo zero termico (i fiocchi ci mettono qualche centinaio di metri a fondere), con 150 m di neve mista sopra e sotto. (15)

**Raffica** — la variazione lenta del vento attorno alla media. Muove l'increspatura e la schiuma, non le onde lunghe, che seguono il vento medio. (13)

**Rebasing** — spostare l'origine di Unreal vicino alla camera, per tenere piccoli
i numeri. (2)

**Registrazione sui nodi / sulle aree** — un campione rappresenta un punto (le
quote) o un'area (i pixel). Decide se il bordo è condiviso. (3, 7)

**Residenza** — l'insieme delle scelte su cosa tenere in memoria e pronto, in base a posizione e velocità invece che alla direzione dello sguardo. (11)

**Ridosso** — la zona sottovento a una cresta ripida: il vento si stacca dalla cresta e dietro resta un vortice (il rotore). Meno vento, molta più turbolenza. (14)

**Rombo** (del tuono) — il tuono che dura secondi: ogni pezzo del canale fa il botto nello stesso istante, ma i pezzi sono a distanze diverse da chi ascolta, e il suono ci mette tre secondi per chilometro. (15)

**Riscaldamento** — dopo un salto, qualche decimo di secondo in cui si costruiscono più mesh per frame (24 invece di 4): il frame rallenta, il terreno arriva tutto insieme. (11)

**Rugosità** (`z0`) — quanto il suolo è ruvido per il vento, in metri: 0,2 mm il mare, 3 cm un prato, 1 m una città. Decide quanto lo strato limite frena il vento al suolo e quanto è rafficato. (14)

**Rumore periodico** — un rumore che si ripete identico dopo un certo passo (le celle che escono da una parte rientrano dall'altra). Permette di riportare un numero che cresce dentro il periodo, per la precisione dei `float`, senza che si veda: così il disegno dei cumuli ogni 1.500 km e il loro orologio ogni giorno. (15)

**Scavo** — abbassare le quote del terreno sotto l'acqua fino al fondale prima di costruirne la mesh, così l'acqua ha una profondità vera. Lo fa il terreno, con la funzione che gli consegna l'acqua (`SetHeightEditor`). (13)

**SCL** (*Scene Classification Layer*) — il file di Sentinel-2 L2A che classifica ogni pixel da 20 m: nuvole, ombre di nuvole, cirri, neve, acqua... Serve a togliere le nuvole dalle ortofoto. (7)

**ScreenSize** — quanto è grande un oggetto sullo schermo (circa il raggio diviso l'altezza dello schermo): decide quale LOD di una static mesh si disegna. Il LOD semplice degli alberi parte da 0,02, ~450 m per un albero di 15 m. (16)

**SDF** (*signed distance field*) — un'immagine in cui ogni pixel dice quanto si è lontani da un bordo, con il segno (positivo in acqua, negativo a terra). Filtrata, dà un bordo nitido a ogni ingrandimento. È la riva dell'acqua. (13)

**Shader a doppia sintassi** — un testo che è HLSL e C++ insieme (`CloudShader.inl`): la factory lo trasforma in una stringa per il nodo Custom, i test lo compilano come C++ con dei tipi finti (`HlslShim.h`). Quello che si prova è quello che gira sulla scheda video. (15)

**Single Layer Water** — il modello d'ombreggiatura di Unreal per l'acqua: il colore viene da assorbimento e diffusione nello spessore d'acqua letto dal depth buffer, non da un BaseColor. (13)

**Spazio scalato** — le coordinate ECEF divise per i semiassi (x/a, y/a, z/b): l'ellissoide diventa la sfera unitaria e il test d'orizzonte diventa esatto. (5, 11)

**Sponda a V** — la forma dello scavo attorno alla riva: la stessa funzione della distanza fa la profondità dentro l'acqua e il tetto del terreno fuori, zero sulla riva. Così i triangoli della mesh incrociano l'acqua proprio sulla riva. (13)

**sRGB** — lo spazio di colore delle immagini normali. Una texture da JPEG va
marcata sRGB. (7)

**Snapshot** — la copia della georeferenziazione che i thread ricevono, invece di
toccare il subsystem. (2)

**Stampe della posizione** — il riquadro a destra con latitudine, longitudine, quota (ellissoide, livello del mare, dal suolo) e rotta. Disegnato con `UDebugDrawService`, anche nell'editor. (13)

**Strato limite** — i primi ~600 m d'aria, dove il vento sente l'attrito del suolo: cresce con il logaritmo della quota e gira con lei. Sopra c'è il vento di gradiente. (14)

**Strato puro / ponte Unreal** — la divisione di ogni modulo in C++ standard
testabile e codice che usa il motore. (0)

**Subsystem** — un "gestore" che Unreal crea e distrugge da solo. (1)

**Teletrasporto** — uno spostamento, in un aggiornamento solo, oltre metà della quota. Svuota le code, azzera la velocità, fa partire il riscaldamento. (11)

**Tick** — il metodo chiamato a ogni frame. (1)

**Tile** — un pezzo quadrato della piramide: 129 × 129 quote o 256 × 256 pixel. (3, 7)

**Tile vettoriale** (`.gvt`) — una tile che contiene linee (punti e proprietà) invece di
pixel. Il runtime le disegna alla risoluzione che serve. Livelli 10..13. (12)

**Tile virtuale** — una tile che il dataset non ha e che il runtime costruisce in memoria, sui thread del pool come una lettura: oggi il fondale e l'acqua del mare aperto. Il resto del motore non sa che è virtuale. (13)

**TINITALY** — il modello del terreno dell'Italia a 10 m dell'INGV. (3)

**Transient** (`RF_Transient`) — l'etichetta di un oggetto che Unreal non salva mai. L'attore delle particelle e la nebbia creata dal meteo la portano. (15)

**Turbolenza congelata** — l'ipotesi di Taylor: su tempi brevi le raffiche non cambiano, vengono solo trasportate dal vento. Il campo delle raffiche scorre con il vento medio, e una raffica si vede correre sul lago. (14)

**UBT / UHT** — Unreal Build Tool e Unreal Header Tool: compilano e generano codice. (1)

**ULP** (*unit in the last place*) — il passo fra due numeri `float` consecutivi.
A 6.371 km è 64 cm, a 10 km è 0,6 mm. (2)

**UObject** — la classe base universale di Unreal, con riflessione e garbage
collector. (1)

**UpdateTextureRegions** — riscrive i pixel di una texture esistente: l'oggetto resta lo stesso, quindi chi lo usa non va avvisato. La copia la fa il render thread più tardi: i dati vanno tenuti vivi fino ad allora. Così si aggiorna la grandine posata. (15)

**Vento di gradiente** — il vento sopra lo strato limite, che non sente il suolo. Con 10 m/s alla stazione meteo è ~17 m/s. (14)

**Vento di riferimento** — il dato da cui parte la simulazione del vento: velocità e direzione a 10 m su un prato, come in un bollettino (METAR). Tutto il resto lo calcola il campo. (14)

**Vestitore** — chi mette qualcosa sopra il terreno e decide quando una tile è pronta da
mostrare: le ortofoto e le strade. Il terreno ne accetta più d'uno, per nome. (11, 12)

**Vetrato** (*verglas*) — un velo di ghiaccio trasparente sul suolo: lucidissimo, scuro come il bagnato. Viene dal bagnato che gela o dal gelicidio. (15)

**Vista-indipendente** — selezione che dipende solo dalla posizione della camera, non dalla direzione: il frustum lo applica il renderer di Unreal. È il default. (11)

**VolumetricCloud** — il componente di Unreal che disegna le nuvole: un guscio attorno al pianeta, fra due quote, attraversato a passi chiedendo a ogni passo il materiale (dominio **Volume**: estinzione, albedo, emissivo). Se ne vede una sola per volta. (15)

**VRT** — un raster virtuale di GDAL: un file XML che fa apparire più file come
uno solo. (3, 7)

**USoundWaveProcedural** — un suono di Unreal i cui campioni li scrive il codice invece di un file: così si suona il tuono sintetizzato. (15)

**WGS84** — l'ellissoide del GPS, usato dal progetto. (2)

**Wireframe** — la visualizzazione a reticolo dei triangoli. `viewmode wireframe` è
il primo comando da provare quando non si vede niente. (6)

**World Position Offset** (WPO) — l'uscita di un materiale che sposta ogni vertice, calcolata dalla scheda video a ogni frame. Le onde e le particelle della pioggia si muovono così, senza che la CPU tocchi la mesh. (13, 15)

**Zero termico** — la quota dove l'aria è a 0 °C. Con 15 °C al mare e 6,5 °C in meno ogni chilometro, a 2.308 m. (15)

---

## Mappa completa dei file

### Il progetto

| File | Contenuto |
|---|---|
| `TestNewIG.uproject` | il progetto Unreal |
| `Source/TestNewIG/*` | modulo C++ minimo: serve solo a far compilare il plugin |
| `Config/DefaultEngine.ini` | impostazioni del motore |
| `Config/DefaultGame.ini` | impostazioni di GeoWorld (`[/Script/GeoCore.GeoWorldSettings]`) |
| `Plugins/GeoWorld/GeoWorld.uplugin` | il plugin e i suoi quattro moduli |

### GeoCore — geodesia e rebasing

| File | Contenuto |
|---|---|
| `GeoCore.Build.cs` | dipendenze; `UnrealEd` solo per l'editor |
| `Public/GeoCoreModule.h` | la categoria di log `LogGeoWorld` |
| `Public/GeoConsoleArgs.h` | argomenti dei comandi console: i percorsi fra virgolette restano interi anche con gli spazi |
| `Public/Geo/GeoTypes.h` | `FGeodetic`, `FEcef`, `FEnu`, `FMat3` |
| `Public/Geo/Ellipsoid.h` | WGS84, conversioni, basi ENU/NEU |
| `Public/Geo/EnuFrame.h` | frame locale ENU |
| `Public/Geo/GeoUnits.h` | `MetersToUu`: l'unico posto del numero 100 |
| `Public/Geo/GeoFormat.h` | numeri, gradi, metri, vento, distanze e precipitazione all'italiana, per le stampe |
| `Public/Geo/Georeference.h` | `P_unreal = M·(P − O)·100` |
| `Public/Georeference/GeoreferenceSnapshot.h` | la copia per i thread |
| `Public/Georeference/GeoreferenceSubsystem.h` | il subsystem del rebasing |
| `Public/Georeference/GeoTransformComponent.h` | tieni un attore a una coordinata |
| `Public/Georeference/GeoWorldSettings.h` | impostazioni in Project Settings |
| `Public/Georeference/GeoWorldTypes.h` | `FGeoCoordinate`, per i Blueprint |
| `Public/Georeference/GeoPlaces.h` | i 28 luoghi noti |
| `Private/GeoCoreModule.cpp` | comandi `geo.*` di base, `geo.Goto`, `geo.ViewSpeed` |
| `Private/Georeference/GeoreferenceSubsystem.cpp` | il rebasing in quattro passi |
| `Private/Georeference/GeoTransformComponent.cpp` | ricalcolo della posizione |
| `Private/Georeference/GeoWorldSettings.cpp` | registrazione delle impostazioni |
| `Private/Tests/GeoCoreTests.cpp` | Automation Test di Unreal |

### GeoTiles — formato, dataset, streaming

| File | Contenuto |
|---|---|
| `GeoTiles.Build.cs` | dipendenze, `ImageWrapper` per il JPEG |
| `Public/Tiles/TileKey.h` | `FTileKey`, `Pack()`, `PackXY()` |
| `Public/Tiles/TilingScheme.h` | lo schema di tiling |
| `Public/Tiles/TileFormat.h` | formato `.ght` e indice delle quote |
| `Public/Tiles/ImageTileFormat.h` | formato `.gim` e indice delle immagini |
| `Public/Tiles/TileCache.h` | `TTileCache<Payload>`, LRU con budget e pin |
| `Public/Streaming/GeoTileDataset.h` | dataset di quote su disco |
| `Public/Streaming/GeoImageryDataset.h` | dataset di immagini su disco |
| `Public/Streaming/GeoLoaderPool.h` | pool di thread per l'I/O |
| `Public/Streaming/GeoTileStreamingSubsystem.h` | streaming delle quote |
| `Public/Streaming/GeoImageryStreamingSubsystem.h` | streaming e decodifica delle immagini |
| `Public/Tiles/WaterTileFormat.h` | formato `.gwa`, indice e geoide dell'acqua |
| `Public/Streaming/GeoWaterDataset.h`, `GeoWaterStreamingSubsystem.h` | dataset e streaming dell'acqua |
| `Public/Tiles/VegetationClasses.h`, `VegetationTileFormat.h` | classi della copertura del suolo, formato `.gvg` e indice |
| `Public/Streaming/GeoVegetationDataset.h`, `GeoVegetationStreamingSubsystem.h` | dataset e streaming della vegetazione |
| `TestData/water_fixture.gwa` | una tile d'acqua scritta dalla pipeline, per i test |
| `Private/GeoTilesModule.cpp` | comandi `geo.Tiles.*` |
| `Private/Streaming/*.cpp` | le implementazioni dei cinque header sopra |
| `TestData/tiling_vectors.txt` | vettori di prova generati dalla pipeline |

### GeoRender — decidere e disegnare

| File | Contenuto |
|---|---|
| `GeoRender.Build.cs` | `GeometryFramework`, `GeometryCore` |
| `Public/Quadtree/QuadtreeTypes.h` | vista, selezione, statistiche |
| `Public/Quadtree/Culling.h` | volume, frustum, margine, orizzonte (sfera e rettangolo) |
| `Public/Quadtree/TileSelector.h` | errore su schermo, attraversamento |
| `Public/Quadtree/Residency.h` | insieme ideale, previsione del moto, piano di residenza |
| `Public/Mesh/TileMesh.h` | da quote a mesh, gonne |
| `Public/Imagery/ImageryMapping.h` | il ritaglio dell'immagine |
| `Public/Lod/GeoQuadtreeSubsystem.h` | il subsystem della selezione |
| `Public/Terrain/GeoTerrainMeshProvider.h` | l'interfaccia del disegno |
| `Public/Terrain/DynamicMeshTerrainProvider.h` | l'implementazione con `UDynamicMeshComponent` |
| `Public/Terrain/GeoTerrainSubsystem.h` | il subsystem del terreno |
| `Public/Imagery/GeoImagerySubsystem.h` | il subsystem del drappeggio |
| `Public/GeoMarkerActor.h` | il cubo di verifica |
| `Public/GeoFlyPawn.h` | la camera di volo |
| `Public/Water/WaterMesh.h`, `WaveSpectrum.h`, `WaterTextures.h` | acqua: scavo, mesh, onde, texture (puri) |
| `Public/Water/GeoWaterSubsystem.h` | il subsystem dell'acqua |
| `Public/Weather/Wind.h`, `WindField.h`, `GeoWeatherSubsystem.h` | il vento (il campo e' puro) |
| `Public/Weather/Precipitation.h`, `GeoPrecipitationRenderer.h` | pioggia, neve, grandine: il modello (puro) e il disegno (particelle, suolo, nebbia) |
| `Public/Weather/Lightning.h`, `GeoLightningRenderer.h` | fulmini e tuono: il modello (puro) e il disegno (canale, luce, suono) |
| `Public/Weather/Clouds.h`, `CloudShader.inl`, `GeoCloudRenderer.h` | le nuvole: il modello (puro), lo shader (HLSL e C++ insieme) e il disegno (VolumetricCloud, mappa meteo) |
| `Public/Weather/GroundHail.h`, `GroundShader.inl`, `GeoHailMap.h` | la grandine posata (puro), il meteo sul suolo nel materiale (HLSL e C++ insieme), la griglia in Unreal |
| `Public/Vegetation/TreePlacement.h`, `TreeMesh.h`, `VegetationCells.h` | la vegetazione (puro): chi cresce dove, le forme, celle e bande |
| `Public/Vegetation/VegetationShader.inl`, `GeoVegetationSubsystem.h` | il materiale degli alberi (HLSL e C++), il subsystem in Unreal |
| `Public/GeoPositionHudSubsystem.h` | le stampe della posizione |
| `Private/GeoRenderModule.cpp` | comandi `geo.Lod.*`, `geo.Terrain.*`, `geo.Imagery.*`, `geo.Roads.*`, `geo.Water.*`, `geo.Weather.*`, `geo.Hud`, `geo.Fly*` |
| `Private/**/*.cpp` | le implementazioni |

### GeoWorldEditor — solo nell'editor

| File | Contenuto |
|---|---|
| `GeoWorldEditor.Build.cs` | `MaterialEditor`, `AssetTools` |
| `Private/GeoWorldEditorModule.cpp` | il modulo |
| `Private/GeoTerrainMaterialFactory.cpp` | `geo.Imagery.CreateMaterial`: `M_GeoTerrain`, `M_GeoRoad`, `M_GeoWater`, `MPC_GeoWater`, `MPC_GeoWeather`, `M_GeoPrecipitation`, `M_GeoLightning`, `M_GeoClouds`, `M_GeoVegetation` |

### Strumenti

| File | Contenuto |
|---|---|
| `Tools/StandaloneTests/*` | i cinque eseguibili di test senza Unreal |
| `Tools/StandaloneTests/HlslShim.h` | quel tanto di HLSL che serve a compilare gli shader delle nuvole e del suolo come C++ |
| `Tools/CheckSourceDiscipline.sh` | cinque regole di disciplina |
| `Tools/CheckShadowedParameters.py` | parametri che nascondono campi |
| `Tools/CheckModuleExports.py` | simboli invisibili fra moduli |
| `Tools/CheckUnityCollisions.py` | collisioni da build unity |
| `Tools/CheckOwnApiCalls.py` | metodi inesistenti |

### Pipeline

| File | Contenuto |
|---|---|
| `run.py` | punto d'ingresso |
| `make_synthetic_source.py` | sorgente finto per i test |
| `README.md` | installazione e uso |
| `geoworld/cli.py` | comandi e orchestrazione delle quote |
| `geoworld/tiling.py` | schema di tiling |
| `geoworld/tileformat.py`, `manifest.py` | formato e indice delle quote |
| `geoworld/geoid.py` | ondulazione del geoide |
| `geoworld/raster.py` | VRT, riproiezione, risoluzione, piramide |
| `geoworld/tilecut.py` | taglio in tile |
| `geoworld/state.py` | riavvio |
| `geoworld/fetch.py` | download del Copernicus DEM |
| `geoworld/environment.py` | `check-env` |
| `geoworld/imageformat.py`, `imagerymanifest.py` | formato e indice delle immagini |
| `geoworld/imagecut.py`, `imagerybuild.py` | la pipeline delle immagini |
| `geoworld/fetchimagery.py`, `mgrs.py` | download di Sentinel-2 |
| `geoworld/waterclasses.py`, `waterformat.py`, `waterbuild.py` | la pipeline dell'acqua |
| `geoworld/vegclasses.py`, `vegformat.py`, `vegbuild.py` | la pipeline della vegetazione (ESA WorldCover) |
| `tests/*.py` | 77 test |

### Documenti

| File | Contenuto |
|---|---|
| `README.md` | il progetto in breve |
| `docs/manuale/` | **questo manuale** |
| `docs/consegna.md` | stato, comandi, errori noti, come continuare |
| `docs/programma.md` | il programma delle sei fasi |
| `docs/dati.md` | da dove vengono i dati, licenze |
| `docs/faseN-design.md` | decisioni e motivazioni, fase per fase |
| `docs/faseN-verifica.md` | come verificare, fase per fase |
| `docs/fase7-design.md` | entità e interoperabilità (solo design) |
| `docs/fase8-design.md` | strade, ferrovie, piste; il programma delle fasi 8–12 |
| `docs/fase8-verifica.md` | come provare le strade |
| `docs/strade-3d.md` | strade 3D da vicino, e il flash del rebase |
| `docs/fase9-design.md` | acqua, vento, stampe della posizione |
| `docs/fase9-verifica.md` | come provare l'acqua (e il mare aperto) |
| `docs/vento-design.md` | il vento: la simulazione |
| `docs/vento-verifica.md` | come provare il vento |
| `docs/vegetazione-design.md` | la vegetazione: dati, regole, celle, Unreal |
| `docs/vegetazione-verifica.md` | come provare la vegetazione |
| `docs/residenza-design.md` | cosa tenere pronto: posizione, velocità, mesh nascoste |
| `docs/residenza-verifica.md` | come verificare la residenza |
| `docs/prova-torino.md` | la prima prova vera: cause del mosaico, prestazioni, tutta Italia |
| `docs/prova-torino-2.md` | la seconda: troppi triangoli, e quanto pesa ogni manopola |
| `docs/prova-torino-3.md` | la terza: flash, popping, nuvole |
| `docs/issues-aperte.md` | problemi noti non risolti |

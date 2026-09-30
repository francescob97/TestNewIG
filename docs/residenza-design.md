# Residenza — cosa tenere pronto, in base a dove sei e dove vai

> Questo documento spiega **cosa** è cambiato nel modo di caricare le tile e
> **perché**. Nasce da una proposta tua; qui trovi la proposta, il mio giudizio,
> il progetto che ne è venuto fuori e le alternative scartate.
> Per provarlo in Unreal: `docs/residenza-verifica.md`.

---

## 1. In una pagina

**Prima.** La Fase 4 sceglieva le tile guardando il **frustum**, cioè la
piramide di ciò che la camera inquadra. Si caricava quello che si vedeva, nel
momento in cui lo si vedeva. Girando la testa, tutto ciò che entrava nel campo
andava letto, trasformato in mesh e vestito con l'ortofoto **mentre** lo si
stava già guardando. Da qui il bordo nero, gli scatti in rotazione, e uno
specchietto retrovisore che non avrebbe avuto niente da mostrare.

**La tua proposta.** Caricare in base alla **posizione** (e alla velocità), non
alla direzione. Tenere in memoria tutto quello che sta attorno, in ogni
direzione; caricare in anticipo verso dove si va; scaricare quello che si
lascia indietro. Meglio un'attesa lunga una volta sola che tanti piccoli
caricamenti mentre si vola.

**Il mio giudizio.** È giusta, ed è come lavorano i generatori di immagini per
simulatori di volo. L'ho implementata con tre precisazioni, che sono il cuore di
questo documento:

1. **"In ogni direzione" sì, "a piena risoluzione" no.** Il dettaglio deve
   comunque calare con la distanza. La regola resta quella dell'errore su
   schermo; cambia solo che la si applica a 360 gradi.
2. **Il collo di bottiglia non è il disco.** Leggere una tile di quote costa
   una frazione di millisecondo, e lo fa un thread di lavoro: il frame non se
   ne accorge. Quello che costa sul game thread è **costruire la mesh**
   (qualche millisecondo per tile). Quindi precaricare le sole quote servirebbe
   a poco: bisogna **precostruire le mesh**, e tenerle.
3. **"Scaricare dietro" conviene farlo pigramente.** Non si butta via niente
   finché la memoria non serve: se torni indietro, è ancora lì.

**Cosa cambia per te, in pratica:**

| Situazione | Prima | Adesso |
|---|---|---|
| Giri la camera, anche di scatto | tile nuove da caricare e costruire, bordo nero | **nessun lavoro**: le tile ci sono già tutte |
| Specchietto, seconda vista | mancherebbe tutto quello che sta dietro | c'è già |
| Voli dritto a 250 m/s | le tile arrivano quando entrano nel campo | costruite **12 secondi prima**, nascoste, poi mostrate |
| Torni indietro | ricaricate e ricostruite | ancora in memoria, si rimostrano |
| Salto con `geo.Goto` | si riempie a pezzi per secondi | **riscaldamento**: per un attimo il frame rallenta, poi è tutto pronto |

Il prezzo è la memoria: circa **2,7 volte** le tile a schermo (da ~230 a ~630
sopra Roma a 3 km), più le mesh tenute in anticipo. Con 64 GB non è un
problema, ed è esattamente lo scambio che proponevi.

---

## 2. La proposta, punto per punto

### 2.1 "Il caricamento non dipende da dove guarda la camera"

**Giusto.** L'errore su schermo di una tile dipende solo dalla **distanza**
(sezione 4 del capitolo 5 del manuale): non dalla direzione. Anche l'orizzonte
dipende solo dalla posizione. Il frustum era l'unica cosa, nella selezione, che
dipendeva dalla direzione, e l'ho tolto (`FViewParameters::bFrustumCulling`).

**Chi toglie allora ciò che sta dietro?** Il renderer di Unreal, che lo fa già
per conto suo: a ogni frame confronta i bounds di ogni componente con il frustum
della camera, e quello che è fuori non lo disegna. Farlo anche nella selezione
serviva solo a risparmiare memoria. E la memoria c'è.

> 💡 **Esempio, misurato dai test.** Camera sopra Roma a 3 km, soglia 4 pixel:
> con il frustum si selezionano ~230 tile, senza ~630. La selezione costa
> sempre meno di mezzo millisecondo. Girando la camera verso nord, verso sud o
> verso il basso, senza frustum l'insieme è **identico tile per tile** (test
> "girare la camera non cambia NESSUNA tile").

### 2.2 "Tenere cariche più tile possibili, in ogni direzione"

**Giusto, con una precisazione.** "Più possibili" non può voler dire "tutto a
piena risoluzione": l'Italia a livello 14 sono ~400.000 tile. Come quote
sarebbero 35 GB, e su 64 GB ci starebbero anche; come mesh (qualche MB l'una)
no, e come immagini nemmeno. Il dettaglio deve calare con la distanza.

La risposta è che la regola dell'errore su schermo **già** fa quello che vuoi:
applicata a 360 gradi, produce anelli concentrici attorno alla camera, fini
vicino e grossolani lontano, con circa lo stesso numero di tile per livello
(una settantina sopra Roma). È questo l'insieme "tutto attorno".

Poi c'è un margine in più, l'**anello di sicurezza** (sezione 4): lo stesso
insieme calcolato con una soglia dimezzata, cioè "come se fossi al doppio del
dettaglio". È la risposta a "e se giro all'improvviso?", per i movimenti che
nessuna previsione può indovinare. Siccome serve solo come riserva, di
quell'anello si tengono in RAM le quote, non le mesh.

### 2.3 "Anche in base alla velocità, perché un aereo si muove in fretta"

**Giusto.** Si stima la velocità della camera dalle sue posizioni e si prevede
dove sarà fra 4, 8 e 12 secondi. Le tile che servirebbero **là** si caricano e
si costruiscono **adesso**, nascoste. Quando la camera ci arriva, si tratta solo
di mostrarle (sezione 5).

### 2.4 "E si scarica viceversa"

**Sì, ma pigramente.** Buttare via esplicitamente quello che si lascia indietro
sarebbe una scommessa sul fatto che non ci si tornerà. Un simulatore ci torna
spesso: circuiti di attesa, avvicinamenti ripetuti, una virata stretta.

Quindi non si scarica niente di proposito. Tutto quello che il piano vuole
viene **toccato** (spostato in testa alla coda LRU) a ogni aggiornamento; quando
serve spazio, si butta per primo quello che il piano non tocca da più tempo.
Finché la memoria basta, quello che hai visto resta lì.

### 2.5 "Meglio un caricamento più lungo, ma una tantum"

**Giusto**, ed è diventato il **riscaldamento** (sezione 8): dopo un salto, o
quando manca più del 40% di quello che serve adesso, si consegnano 16 mesh
per frame invece di 4. Per qualche decimo di secondo il frame rate scende; in
cambio il terreno arriva tutto insieme invece di riempirsi a pezzi per secondi.

---

## 3. L'architettura: tre domande, tre insiemi

Prima c'era una sola domanda: "cosa vedo?". Adesso sono tre, e ognuna ha il
suo insieme di tile, con un costo e un budget diversi.

| Domanda | Insieme | Cosa costa | Chi decide | Budget |
|---|---|---|---|---|
| Cosa **disegno** adesso? | la selezione, a ogni frame | niente: le mesh esistono già | quadtree | — |
| Cosa ho **pronto** da mostrare? | mesh costruite, visibili + nascoste | qualche MB a tile, ms di costruzione | terreno | 2.000 mesh |
| Cosa ho in **RAM**? | quote del piano, anche l'anello di sicurezza | 66 KB a tile, lettura su thread | streaming | 1 GB |

Ogni insieme contiene il precedente. Un disegno di come stanno insieme:

```
                 ┌────────────────────────────────────────────────┐
  RAM (quote)    │  fascia di sicurezza: tutto attorno, più fine   │
                 │   ┌─────────────────────────────────────────┐   │
  mesh pronte    │   │ posizioni previste: dove sarò fra 4-12 s │   │
  (nascoste)     │   │   ┌───────────────────────────────┐      │   │
                 │   │   │ adesso: l'insieme ideale qui   │      │   │
  a schermo ───▶ │   │   │   (quello che si disegna, se   │      │   │
                 │   │   │    tutto è pronto)             │      │   │
                 │   │   └───────────────────────────────┘      │   │
                 │   └─────────────────────────────────────────┘   │
                 └────────────────────────────────────────────────┘
```

Il ciclo, a ogni frame:

1. **Quadtree** — aggiorna la stima della velocità; seleziona cosa disegnare
   (senza frustum, "pronta" = "ha la mesh"); ogni 0,25 s rifà il **piano**;
   chiede allo streaming le quote che mancano, in ordine di urgenza.
2. **Streaming** — legge le quote sui thread di lavoro (niente di nuovo).
3. **Terreno** — mostra le mesh selezionate e nasconde le altre; costruisce
   prima quelle che il disegno aspetta, poi quelle del piano, nascoste; se ha
   superato il budget, butta le nascoste più vecchie che il piano non vuole.
4. **Ortofoto** — veste tutte le mesh costruite, prima quelle a schermo, poi
   quelle nascoste: quando una mesh nascosta comparirà, avrà già la sua foto.

---

## 4. Il piano di residenza

Il **piano** è la lista delle tile che conviene avere, in ordine di urgenza,
divisa in **fasce**:

| Fascia | Cosa contiene | Si costruisce la mesh? |
|---|---|---|
| 0 — adesso | l'insieme ideale alla posizione attuale | sì |
| 1, 2, 3 — previste | l'insieme ideale a 4, 8 e 12 secondi | sì, nascoste |
| 4 — sicurezza | l'insieme ideale qui, con soglia d'errore dimezzata | no, solo quote in RAM |

Una tile compare una volta sola, nella fascia più urgente che la vuole.

**"Ideale"** vuol dire: quello che si disegnerebbe se tutto fosse già in
memoria. La selezione per il disegno invece si ferma dove i dati finiscono
(regola anti-buchi della Fase 4): per decidere cosa *caricare* serve sapere dove
si vorrebbe arrivare, non dove si è arrivati. Tecnicamente è lo **stesso**
selettore della Fase 4, a cui si passa una disponibilità che risponde sempre
"sì, è caricata" (`FAssumeEverythingLoaded`). Riusarlo, invece di scrivere un
secondo attraversamento, vuol dire che le due selezioni non possono divergere
per un errore di copia.

> 💡 **Esempio, dai test.** Volo verso nord a 2.500 m e 250 m/s, previsione 12 s:
> fascia 0 = 443 tile; fasce previste = 78 tile **nuove** (le altre erano già
> nella fascia 0); fascia di sicurezza = 993 tile. Delle 78 previste, 62 stanno
> davanti alla camera e 16 dietro: quelle dietro sono tile grossolane e lontane,
> che cambiano livello perché la distanza da esse cambia.

**Perché ogni 0,25 secondi e non a ogni frame.** Il piano costa cinque selezioni,
circa un millisecondo. In un quarto di secondo, anche a 250 m/s, ci si sposta di
60 m e l'insieme cambia solo ai bordi. Dopo un teletrasporto invece si rifà
subito.

**Perché tre posizioni previste e non una.** Una sola, a 12 secondi, salterebbe
i livelli intermedi: le tile che servono a 4 secondi da qui non sono quelle che
servono a 12. Tre campioni coprono il percorso.

---

## 5. La previsione del moto

### Da dove viene la velocità

Dalle **posizioni** della camera, non da una velocità dichiarata. La camera può
muoverla un pawn, l'editor, `geo.Goto` e, con la Fase 7, l'host via CIGI o DIS.
Nessuno di questi è obbligato a dichiarare una velocità; tutti producono
posizioni. Stimarla funziona con tutti senza toccarli. Quando la Fase 7 porterà
velocità vere dall'host, basterà usarle al posto della stima: l'interfaccia verso
chi la usa è una funzione sola, `PredictPosition(secondi)`.

La velocità istantanea fra due frame è rumorosa (un frame lento, uno scatto del
mouse), quindi si filtra con una media esponenziale con costante di mezzo
secondo. Il peso del campione nuovo dipende dal tempo passato, non dal numero di
frame: con frame irregolari il filtro si comporta lo stesso.

> 💡 **Dai test.** Volo simulato a 250 m/s e 60 fps: dopo 5 secondi la stima
> vale 249,9 m/s; la posizione prevista a 10 secondi sbaglia di 0,6 m. Un frame
> da 100 ms in mezzo a quelli da 16 ms sposta la stima di meno di 4 m/s.

La previsione è una **retta** in ECEF, non un arco sulla superficie: in 12
secondi a 250 m/s la differenza fra le due è 0,7 m. Per decidere cosa caricare
non conta niente.

### Il teletrasporto

Un `geo.Goto` da Roma a Milano sposta la camera di 480 km in un frame. Come
velocità sarebbero trentamila chilometri al secondo, e la previsione metterebbe
la camera in Siberia. Un salto va riconosciuto e trattato per quello che è:
non un moto, ma un ricominciare da capo.

**Come si riconosce.** Uno spostamento, in un solo aggiornamento, oltre **metà
della quota** (e comunque oltre 500 m). Perché relativo alla quota:
l'insieme di tile scala con la quota. Da 2 km il dettaglio fine copre qualche
chilometro; da 600 km copre mezza Italia. Una soglia fissa sbaglierebbe in un
verso o nell'altro.

| Caso | Spostamento | Soglia | Esito |
|---|---|---|---|
| `geo.Goto` Roma → Milano a 2.500 m | 480 km | 1,25 km | teletrasporto |
| salita da 2.500 a 6.000 m in un frame | 3,5 km | 3 km | teletrasporto |
| volo a 250 m/s, 60 fps | 4 m | 1,25 km | moto |
| `geo.Fly` a 600 km di quota, 300 km/s | 5 km | 300 km | moto |

**Cosa succede dopo un salto:** le code di lettura (quote e immagini) vengono
svuotate, perché riguardano il posto da cui si è partiti; la velocità riparte da
zero; il piano si rifà subito; il terreno entra in riscaldamento.

---

## 6. Le mesh nascoste

Qui sta il guadagno vero. Una mesh si costruisce **prima** che serva (dal piano)
e si **tiene** dopo che non serve più (finché c'è posto). In entrambi i casi
esiste ma non va disegnata: sovrapposta al padre o ai figli produrrebbe
z-fighting. Quindi si nasconde.

**Cosa costa.** Costruirla: 16.641 conversioni geodetiche più la topologia
della mesh, qualche millisecondo sul game thread. Nasconderla: niente.
Rimostrarla: il renderer ricrea il suo proxy copiando i vertici nei buffer della
scheda video, una copia lineare, molto meno della costruzione.

> ⚠️ **Nota Unreal: `SetVisibility`, non `SetHiddenInGame`.**
> `SetHiddenInGame` vale solo in gioco: nel viewport dell'editor la tile
> resterebbe visibile, sovrapposta al padre. `SetVisibility` vale ovunque.
> Sotto il cofano, un componente invisibile viene tolto dalla scena del
> renderer (il suo *proxy*, la copia che il thread di rendering usa, viene
> distrutto), ma i dati della mesh restano sul game thread. Per questo
> rimostrarlo costa poco: non si rifà la geometria, si rifà solo la copia.

**Il budget.** Di default 2.000 mesh in tutto, visibili più nascoste. Le visibili
non si toccano mai. Oltre il budget si buttano per prime le nascoste che il
piano non vuole più, dalla meno recente. Se il piano stesso chiedesse più del
budget, si smette di costruire in anticipo, ma quello che serve a schermo si
costruisce comunque: una tile che manca a schermo è un buco, e un buco è peggio
di un budget sforato.

**Perché l'anello di sicurezza non si costruisce.** Sono ~1.000 tile per posti
dove probabilmente non si andrà. Come quote costano 66 MB; come mesh costerebbero
gigabyte. Se servono davvero, le quote sono già in RAM e resta solo la
costruzione.

---

## 7. La regola anti-buchi, rivista

La Fase 4 scende su un livello più fine solo quando **tutti** i figli sono
pronti: altrimenti tiene il padre. "Pronti" voleva dire "le quote sono in RAM".

Rileggendo il codice ho trovato che questo **non bastava** già nella Fase 5.
Il quadtree vedeva le quote dei figli, sceglieva i figli, il terreno toglieva
subito il padre e costruiva i figli 4 per frame: per qualche frame, al posto
del padre, non c'era niente. Un buco breve, che si confondeva con gli scatti.

Adesso, con il terreno acceso, "pronta per il disegno" vuol dire **"ha la
mesh"**. Il terreno registra questa risposta nel quadtree
(`SetRenderReadiness`), e la regola anti-buchi torna a valere per quello che si
vede davvero: il padre resta finché i figli non sono **costruiti**, e padre e
figli si scambiano nello stesso frame.

---

## 8. Il riscaldamento

Dopo un teletrasporto, o all'accensione, manca quasi tutto. Due strade:

- costruire al ritmo normale (4 mesh per frame): il frame resta fluido, ma il
  terreno si riempie a pezzi per secondi, e si vede;
- costruire di più per poco tempo: il frame rallenta per qualche decimo di
  secondo, e poi è tutto pronto.

Hai scelto la seconda, e la regola è:

- **si entra** in riscaldamento dopo un teletrasporto, o quando le mesh pronte
  della fascia "adesso" scendono sotto il **60%**;
- **si esce** quando superano il **95%**;
- nel frattempo si consegnano **16 mesh per frame** (erano 24 quando la
  costruzione stava sul game thread) e si creano **32 texture
  per frame** invece di 4.

Due soglie invece di una (si chiama *isteresi*) per non oscillare: con una
soglia sola al 90%, un frame al 89% e il successivo al 91% farebbero entrare e
uscire di continuo.

> 💡 **Per un IG vero.** Un host di simulazione di solito sa quando sta per
> spostare la camera (riposizionamento, cambio di scenario) e può aspettare un
> segnale di "pronto". Lo stato di riscaldamento è esattamente quel segnale:
> quando la Fase 7 esisterà, andrà esposto all'host.

---

## 9. Una scoperta per strada: l'orizzonte che non scartava

Il primo test sul piano, su un dataset **mondiale**, selezionava tile
sull'America guardando da Roma. Il frustum aveva nascosto per mesi un difetto
dell'**horizon culling**.

**Il difetto.** Il test d'orizzonte della Fase 4 lavora sulla **sfera** di
contenimento della tile, e rinuncia ("non so, la tengo") appena la sfera scende
sotto la superficie del pianeta. Ma una sfera che contiene un pezzo di
superficie curva scende **sempre** sotto la superficie, di una quantità che
cresce con la tile: in pratica il test rinunciava per tutte le tile dal
livello 0 fin verso il 10. Finché il frustum scartava l'altra faccia del
pianeta, non se ne accorgeva nessuno.

**La correzione.** Un secondo test, `IsTileRectBeyondHorizon`, che ragiona sul
**rettangolo** geografico vero invece che sulla sfera. Due idee:

- si lavora nello **spazio scalato** (x/a, y/a, z/b), dove l'ellissoide diventa
  esattamente la sfera unitaria. È una trasformazione che conserva rette e
  tangenze, quindi il cono d'orizzonte calcolato lì è quello vero. È lo stesso
  trucco che usa Cesium;
- in quello spazio, la latitudine di un punto in superficie è la **latitudine
  parametrica** (tan β = (b/a)·tan φ). È monotona rispetto a quella geodetica,
  quindi il rettangolo resta un rettangolo, e la distanza angolare minima fra
  la camera e un rettangolo di latitudine e longitudine su una sfera ha una
  formula chiusa.

La tile è oltre l'orizzonte se `gamma > alphaC + alphaT`, dove gamma è l'angolo
minimo fra camera e rettangolo, alphaC e alphaT gli angoli di visibilità della
camera e del punto più alto della tile.

> 💡 **Risultato.** Sul dataset mondiale a 3 km sopra Roma: da 983 a 543 tile.
> I test controllano anche il verso opposto, quello pericoloso: su 534 punti
> visibili sparsi fino a 800 km, nessuno cade in una tile scartata.

La prima versione della correzione usava la sfera inscritta (raggio polare)
invece dello spazio scalato: corretta, ma così prudente da tenere tile a livello
del mare centinaia di chilometri oltre l'orizzonte. L'hanno scoperto i test sui casi limite già
esistenti (la vetta a 600 km, il livello del mare a 300 km).

---

## 10. Un altro difetto latente: una funzione non `inline`

In `TileSelector.h`, la funzione `Detail::PriorityFromError` era **definita**
nell'header ma senza `inline`. In C++ questo vuol dire che ogni `.cpp` che
include l'header ne produce una copia pubblica, e il linker, trovandone due, si
ferma con `LNK2005: already defined`. Non è mai successo solo per caso: l'header
lo includevano pochi file, e la unity build di Unreal li fondeva in uno solo.

Corretta, e soprattutto aggiunta una **regola 3** a
`Tools/CheckModuleExports.py`: negli header dello strato puro ogni funzione
definita a livello di namespace deve essere `inline`. Ho verificato che la
regola trova il difetto sulla versione vecchia del file. Già che c'ero, i
controlli coprono ora anche `Mesh/TileMesh.h` e `Imagery/ImageryMapping.h`, che
sono strato puro ma non erano nell'elenco.

---

## 11. Le priorità nella coda del disco

Il pool di lettura ha una coda con priorità. Il principio: quello che manca a
schermo passa davanti a tutto.

| Chi chiede | Priorità | Coda di Unreal |
|---|---:|---|
| il disegno (figli che il padre aspetta) | 2–3 | Normal / High |
| piano, fascia "adesso" | 2 | Normal |
| piano, posizioni previste | 1 | Low |
| piano, anello di sicurezza | 0 | Lowest |
| ortofoto di una tile a schermo | 1 | Low (coda delle immagini) |
| ortofoto di una tile nascosta | 0 | Lowest (coda delle immagini) |

**Il tetto di 128 letture di precarico in volo.** Una richiesta già accodata
non si riordina. Senza tetto, un piano da tremila tile riempirebbe la coda, e
una tile diventata urgente dovrebbe aspettare dietro a tutte quelle a bassa
priorità già entrate. Con il tetto, il precarico prende i posti liberi e lascia
spazio a quello che serve. Le richieste del disegno non hanno tetto.

---

## 12. La memoria

| Cosa | Per tile | Default | Sulla macchina da 64 GB |
|---|---:|---:|---|
| quote in RAM | 66 KB | cache 1 GB (~16.000 tile) | `geo.Tiles.Budget 8192` |
| immagini decodificate in RAM | 256 KB | cache 1 GB (~4.000 tile) | invariato per ora |
| mesh (RAM + scheda video) | qualche MB, **da misurare** | 2.000 mesh | `geo.Terrain.MeshBudget 4000` |
| texture sulla scheda video | 256 KB | quante le mesh vestite | — |

Il piano tipico sopra l'Italia a bassa quota vuole ~500 tile in fascia 0,
qualche decina–centinaio in quelle previste, ~1.000 in sicurezza. I default
bastano con ampio margine per le quote; per le mesh il numero da verificare è
quanta memoria occupa davvero una `UDynamicMeshComponent` con 33.792 triangoli
(la mesh CPU di Unreal tiene anche la topologia degli spigoli, quindi è più
grande dei soli vertici). È il primo numero che ti chiedo di leggere nella
verifica.

---

## 13. Alternative considerate

**Una griglia fissa attorno alla camera** (come `geo.Tiles.LoadAround`, "tutte
le tile di livello 14 in un raggio di 10 km"). Scartata: ignora il LOD. O il
raggio è piccolo e manca il lontano, o è grande e si caricano migliaia di tile
fini dove ne basterebbe una grossolana. La regola dell'errore su schermo dà
gli anelli giusti gratis.

**Caricare tutta l'Italia all'avvio.** Per le quote sulla macchina finale è
fattibile (35 GB su 64), ma non risolve niente: il disco non era il problema.
Mesh e immagini di tutta l'Italia non stanno in memoria.

**Tenere il frustum e allargarlo di più.** È quello che facevamo (margine 1,2).
Allargarlo tende alla soluzione vista-indipendente, ma con un difetto: una
rotazione più veloce del margine lo supera comunque. Senza frustum il problema
sparisce per costruzione, invece di spostarsi. Il modo classico resta
disponibile (`geo.Lod.ViewIndependent 0`) per confrontare.

**Previsione su una curva invece che su una retta.** Utile per un aereo in
virata stretta. Non l'ho fatta: in 12 secondi l'errore della retta resta sotto
lo spessore dell'anello di sicurezza per le virate normali, e la stima della
curvatura dalle posizioni è rumorosa. Da rivedere quando avremo dati veri dal
volo.

**Scaricare esplicitamente ciò che sta dietro.** Scartata (sezione 2.4): si
perde tutto quello che si rivedrà presto, senza guadagnare niente finché la
memoria basta.

**Mostrare la vista solo quando tutto è pronto** (schermata di caricamento dopo
un salto). Utile per un IG, ma è una decisione dell'host, non del terreno.
Il riscaldamento dà il segnale; come usarlo si deciderà con la Fase 7.

---

## 14. Limiti noti e prossimi passi

- ~~**La costruzione delle mesh sta ancora sul game thread.**~~ **Risolto
  dopo la prima prova** (`docs/prova-torino.md`, sezione 6): geodesia e
  `FDynamicMesh3` ora si costruiscono su `UE::Tasks`. Il testo originale:
  è il limite
  principale, già dichiarato nella Fase 5. Il precarico lo nasconde (si costruisce
  prima, con il budget che avanza) ma non lo elimina: in riscaldamento il frame
  rallenta proprio per questo. Il prossimo passo naturale è spostare
  `BuildTileMesh` e la costruzione della `FDynamicMesh3` su un thread di lavoro,
  lasciando al game thread solo la consegna al componente.
- **Il costo di rimostrare una mesh non è misurato.** Il proxy viene ricreato
  alla fine del frame, non dentro `SetVisibility`, quindi non si può
  cronometrare dal codice. Va guardato con `stat unit` durante un volo.
- **La memoria di una mesh non è misurata** (sezione 12).
- **La velocità si misura con l'orologio**, non con il tempo del mondo: il
  tempo del mondo si ferma con la pausa e nell'editor fuori dal Play non è
  detto che avanzi, mentre la camera si muove comunque. È voluto, ma vuol dire
  che in pausa, muovendo la camera, la previsione continua a lavorare.
- **Il riscaldamento abbassa il frame rate per scelta.** Se in qualche uso non
  va bene, `geo.Terrain.Warmup 0` lo spegne.

---

## 15. Comandi nuovi

| Comando | Default | Cosa fa |
|---|---|---|
| `geo.Lod.ViewIndependent <0\|1>` | 1 | selezione indipendente dalla direzione; 0 = modo classico con frustum |
| `geo.Lod.Prefetch <0\|1>` | 1 | piano di residenza e precarico |
| `geo.Lod.Lookahead <s>` | 12 | orizzonte della previsione |
| `geo.Lod.Safety <fattore>` | 0,5 | anello di sicurezza; 0 = spento |
| `geo.Terrain.MeshBudget <N>` | 2000 | mesh costruite in tutto |
| `geo.Terrain.Warmup <N>` | 16 | mesh consegnate per frame in riscaldamento; 0 = spento |

Gli overlay `geo.Lod.Debug 1` e quello del terreno hanno righe nuove, spiegate
in `docs/residenza-verifica.md`. `geo.Lod.Stats` e `geo.Terrain.Stats` stampano
anche i numeri del piano.

---

## 16. Dove sta nel codice

| File | Cosa è cambiato |
|---|---|
| `GeoRender/Public/Quadtree/Residency.h` | **nuovo**, strato puro: insieme ideale, `FMotionPredictor`, `BuildResidencyPlan` |
| `GeoRender/Public/Quadtree/QuadtreeTypes.h` | `FViewParameters::bFrustumCulling` |
| `GeoRender/Public/Quadtree/TileSelector.h` | frustum opzionale, orizzonte sul rettangolo, `inline` mancante |
| `GeoRender/Public/Quadtree/Culling.h` | `IsTileRectBeyondHorizon` |
| `GeoRender/.../Lod/GeoQuadtreeSubsystem.*` | moto, piano ogni 0,25 s, precarico con tetto, readiness del terreno |
| `GeoRender/.../Terrain/GeoTerrainSubsystem.*` | mesh nascoste, costruzione in anticipo, budget e sfratto, riscaldamento |
| `GeoRender/.../Terrain/*Provider*` | `SetTileVisible` |
| `GeoRender/.../Imagery/GeoImagerySubsystem.*` | prima le tile a schermo, riscaldamento, coda svuotata al salto |
| `GeoTiles/Public/Tiles/TileCache.h` | `Touch`: in testa alla LRU senza contare un accesso |
| `GeoTiles/.../GeoTileStreamingSubsystem.h` | `TouchTile`, `GetInFlightCount`, cache 1 GB |
| `GeoTiles/.../GeoImageryStreamingSubsystem.cpp` | cache immagini 1 GB |
| `GeoRender/Private/GeoRenderModule.cpp` | i sei comandi nuovi, statistiche |
| `Tools/CheckModuleExports.py` | regola 3: nello strato puro tutto `inline` |
| `Tools/StandaloneTests/geoquadtree_main.cpp` | sezioni 3b, 7, 8, 9: 32 test nuovi |
| `Tools/StandaloneTests/geotiles_main.cpp` | 4 test su `Touch` |

---

## Riepilogo

- La selezione **non dipende più da dove guardi**: il frustum lo applica il
  renderer di Unreal. Girare la camera non costa niente.
- Il **piano di residenza** dice cosa tenere pronto: qui (fascia 0), dove sarai
  fra 4, 8 e 12 secondi (fasce previste), e un anello di sicurezza più fine.
- La velocità si **stima dalle posizioni**; un salto oltre metà della quota è un
  **teletrasporto**, e si riparte da capo.
- Le mesh si **costruiscono in anticipo e si tengono**, nascoste: è lì il
  guadagno, perché il costo vero è costruirle, non leggerle.
- Si scarica **pigramente**: si butta per primo ciò che il piano non vuole da
  più tempo, solo quando serve spazio.
- Dopo un salto c'è il **riscaldamento**: un attimo di frame lento, poi tutto
  pronto.
- Per strada: l'**orizzonte** ora scarta davvero l'altra faccia del pianeta, e
  una funzione non `inline` che prima o poi avrebbe rotto il link è corretta e
  sorvegliata da un controllo.

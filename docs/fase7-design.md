# Fase 7 — Entità e interoperabilità: decisioni e motivazioni

> Capitolo di design: fissa le decisioni prima, perché qui sbagliarle costa
> molto più che nelle fasi precedenti. Il **primo pezzo** (sezione 12, punti 1
> e 2: lo strato puro e la memoria condivisa, più il modulo Unreal) è scritto:
> la sezione 13 dice cosa c'è e cosa si è deciso scrivendolo. Come provarlo:
> `docs/fase7-verifica.md`; per chi impara: `docs/manuale/20-entita.md`.

---

## 1. Di cosa stiamo parlando

Finora il motore disegna un pianeta fermo. Questa fase gli fa **ricevere e
pubblicare lo stato di entità in movimento**: aerei, veicoli, navi, bersagli —
ciascuna con una posizione, un assetto, una velocità e un modello 3D.

Non è una funzionalità in più: è il motivo per cui il progetto si chiama
`TestNewIG`. Un **Image Generator** è precisamente questo — un visualizzatore
che riceve lo stato del mondo da un simulatore esterno e lo disegna. Il terreno
delle Fasi 1-6 è la scena; le entità sono ciò che si muove dentro.

I protocolli richiesti sono quattro famiglie:

| | Chi comanda | Trasporto | Nota |
|---|---|---|---|
| **CIGI** | l'host comanda, l'IG obbedisce | UDP, di solito a frame | nato apposta per gli IG |
| **DIS** | ognuno possiede le proprie entità | UDP broadcast/multicast | IEEE 1278.1 |
| **HLA** | come DIS, ma mediato | RTI (libreria) | IEEE 1516 + RPR-FOM |
| **Memoria condivisa** | dipende | locale | latenza minima, formato tuo |

---

## 2. L'osservazione che regge tutta la fase

I quattro protocolli sembrano quattro mondi. Non lo sono: **trasportano tutti
lo stesso contenuto fisico**, con codifiche e convenzioni diverse.

Ogni entità, in ognuno di essi, è:

```
identità  +  che modello sono  +  dove sono  +  come sono orientato
          +  come mi sto muovendo  +  quando è vero tutto questo
```

Quindi la fase **non** è "scrivere quattro sottosistemi". È scrivere una
rappresentazione interna e quattro **adattatori** sottili che traducono verso
di essa. Se un giorno arriva un quinto protocollo, si scrive solo il quinto
adattatore.

È la stessa forma già usata due volte in questo progetto: `IGeoTerrainMeshProvider`
per il disegno, `FGeoLoaderPool` per l'I/O. La differenza è che qui l'interfaccia
va progettata **prima**, non estratta dopo, perché i protocolli sono quattro fin
dal primo giorno.

---

## 3. La rappresentazione canonica

**Decisione.** Una sola struttura interna, `FEntityState`:

| Campo | Tipo | Frame |
|---|---|---|
| Posizione | `double[3]` | **ECEF**, metri |
| Orientamento | quaternione `double[4]` | da corpo a **ECEF** |
| Velocità lineare | `double[3]` | ECEF, m/s |
| Accelerazione lineare | `double[3]` | ECEF, m/s² |
| Velocità angolare | `double[3]` | **corpo**, rad/s |
| Accelerazione angolare | `double[3]` | corpo, rad/s² |
| Tempo di validità | `double` | secondi di simulazione |

**Perché ECEF.** Non è una scelta nuova: è l'invariante fissata in Fase 1, *la
posizione ECEF è l'unica autorità*. Le entità entrano nello stesso sistema del
terreno, e ne ereditano gratis due cose importanti. La prima è che DIS e
RPR-FOM **usano già ECEF** per posizione, velocità e accelerazione: sul
protocollo più diffuso la conversione è zero. La seconda sta nella sezione 10.

**Perché un quaternione e non angoli di Eulero.** Gli angoli di Eulero sono ciò
che i protocolli trasmettono, e vanno benissimo per quello. Come
rappresentazione **interna** sono una trappola: hanno una singolarità a pitch
±90° (il gimbal lock), non si interpolano in modo sensato, e soprattutto ogni
protocollo li definisce con una convenzione diversa — vedi la sezione 4. Un
quaternione non ha convenzioni: o è giusto o è sbagliato, e si verifica con un
round-trip.

**Perché la velocità angolare nel frame del CORPO e le altre in ECEF.** Perché è
così che arrivano, in tutti e quattro i protocolli, e perché è così che ha senso
fisicamente: un aereo ruota attorno ai *propri* assi, non attorno a quelli della
Terra.

---

## 4. Le tre trappole delle convenzioni

Questa è la sezione da rileggere quando qualcosa non tornerà. Nella mia
esperienza di questo progetto, **tutti** i bug lunghi sono nati da una
convenzione data per scontata: ENU contro NEU in Fase 1, quote ortometriche
contro ellissoidiche in Fase 2, post contro pixel in Fase 6. Qui ce ne sono tre
insieme.

### Trappola 1 — NED non è NEU

Il mondo della simulazione usa **NED**: North, East, **Down**. Questo progetto
usa **NEU**: North, East, **Up**. Non è una differenza di gusto: la terza
componente ha il segno opposto.

Una velocità verticale di `+5` in NED significa che l'entità sta **scendendo**.
Interpretata come NEU, la fa salire. L'errore non produce nulla di vistoso
subito — produce aerei che salgono mentre atterrano, e ci si mette un po' a
credere ai propri occhi.

Peggio: NED è **destrorso**, NEU come lo usiamo noi in Unreal è sinistrorso.
È la stessa riflessione che la Fase 1 ha gestito al confine, e la conversione
deve passare dallo stesso posto, non da una nuova.

### Trappola 2 — gli angoli di DIS e quelli di CIGI non sono gli stessi angoli

Sembrano gli stessi: tre numeri, chiamati più o meno imbardata, beccheggio,
rollio. Non lo sono.

* **DIS** trasmette psi, theta, phi che orientano il corpo rispetto agli assi
  **ECEF**. Sono angoli rispetto a un sistema fisso nel centro della Terra.
* **CIGI** trasmette yaw, pitch, roll rispetto al frame **locale** dell'entità —
  imbardata misurata dal nord vero *nel punto dove l'entità si trova*.

Sono grandezze diverse: un aereo con imbardata 0 a Roma e uno con imbardata 0 a
Tokyo puntano entrambi a nord, ma i loro psi DIS sono completamente diversi.

Convertire fra le due richiede di comporre con la base locale nel punto
dell'entità — cioè esattamente `MakeNeuBasis()`, che esiste dalla Fase 1. La
cosa da NON fare è scriverne una seconda versione qui.

### Trappola 3 — gli assi del corpo

In aeronautica il frame del corpo è **X avanti, Y a destra, Z in basso**. In
Unreal un attore ha **X avanti, Y a destra, Z in alto**. Di nuovo la terza
componente, di nuovo un cambio di mano.

Conseguenza pratica: un rateo di rollio positivo secondo DIS diventa negativo in
Unreal, e un aereo che accenna una virata a destra la fa a sinistra.

### La regola che ne discende

> Ogni conversione di frame avviene **una volta sola, in un punto solo**, negli
> adattatori. Dentro il motore esiste solo la rappresentazione canonica.

E ogni adattatore porta con sé un test di **round-trip**: prendi uno stato,
convertilo nel formato del protocollo, riconvertilo, e verifica che torni
uguale entro una tolleranza dichiarata. È un test che si scrive in venti righe e
che coglie tutte e tre le trappole insieme.

---

## 5. Il tempo

Ogni protocollo ha la sua idea di tempo, e nessuna coincide con la nostra.

DIS ha un timestamp a 31 bit dove un'ora vale 2³¹ unità — circa 1,68 µs per
unità — **e riparte da zero ogni ora**. CIGI ragiona per numero di frame. La
memoria condivisa, di solito, per contatore di sequenza.

**Decisione.** All'ingresso ogni timestamp diventa un `double` di secondi di
simulazione, monotòno, e il valore originale si conserva a fianco per la
diagnostica. Il motore non deve sapere che DIS gira su un orologio che si
riazzera: è un problema dell'adattatore, e il punto in cui si gestisce il
wrap-around deve essere uno solo.

Il costo di sbagliare qui è subdolo: un salto all'indietro di un'ora fa
estrapolare all'indietro le entità, che schizzano via e poi rientrano. Succede
una volta ogni ora, che è il peggior intervallo possibile per accorgersene.

---

## 6. L'estrapolazione, e perché non è un dettaglio

Gli aggiornamenti arrivano a 1-30 Hz. Il motore disegna a 60-120 Hz. Fra un
aggiornamento e il successivo **qualcuno deve inventare** dove sta l'entità,
altrimenti si muove a scatti.

DIS formalizza la cosa con nove algoritmi di *dead reckoning*, e distingue in
particolare quelli che esprimono l'accelerazione in coordinate mondo da quelli
che la esprimono negli assi del corpo. CIGI, all'opposto, non prevede
estrapolazione: l'host manda lo stato a ogni frame e l'IG obbedisce.

**Decisione.** Un solo modello interno — posizione con velocità e accelerazione,
orientamento con velocità angolare — e l'algoritmo dichiarato dal protocollo
trattato come un *suggerimento* che sceglie quali termini usare. Non nove
implementazioni: una, parametrica.

**Il problema vero non è estrapolare, è il rientro.** Quando arriva
l'aggiornamento successivo, quasi mai coincide con ciò che avevamo estrapolato.
Saltare alla posizione vera produce uno scatto visibile; ignorarla produce
deriva. La soluzione standard è fondere le due in una frazione di secondo, e la
durata della fusione è una manopola da esporre in console, non una costante da
indovinare. Vale la stessa lezione di `geo.Lod.Margin`: se un numero decide
l'aspetto di quello che si vede, deve essere regolabile a caldo.

---

## 7. Identità e modello 3D

Due cose distinte che vengono spesso confuse.

**L'identità** è "quale entità sei". Ogni protocollo la esprime a modo suo: DIS
usa una terna sito/applicazione/entità, CIGI un intero a 16 bit, la memoria
condivisa quello che decidi tu. Internamente serve una chiave unica che non
vada in collisione fra sorgenti diverse: **sorgente + identificatore nativo**,
non il solo numero.

**Il modello** è "che cosa disegnare". Qui DIS e CIGI divergono in modo
profondo:

* CIGI manda un **indice del database dell'IG**: un numero che significa
  qualcosa solo per noi, concordato in anticipo con l'host.
* DIS e HLA mandano una **enumerazione descrittiva** di sette campi — genere,
  dominio, paese, categoria, sottocategoria, specifico, extra — che descrive
  *cosa* è l'entità, non quale modello usare.

**Decisione.** La traduzione da identificativo di protocollo a modello 3D è una
**tabella di dati**, non codice. Un file, caricato all'avvio, ricaricabile a
caldo. L'elenco delle enumerazioni DIS è un documento di migliaia di righe che
viene aggiornato più volte l'anno: metterlo in un `switch` significa
ricompilare il motore perché è uscito un nuovo modello di elicottero.

La tabella deve anche prevedere una **regola di ripiego** — se non conosco
questo esatto sottotipo, uso il modello generico della sua categoria — e dire a
schermo quando ripiega. Un'entità disegnata come cubo rosso con scritto il suo
tipo è infinitamente più utile di un'entità che non compare.

---

## 8. Chi possiede cosa, e la direzione di uscita

La richiesta era "ricevere **e/o** mandare". Le due cose hanno regole diverse e
vanno tenute separate fin dall'inizio.

Ogni entità ha una **proprietà**: locale o remota. Le remote le riceviamo e non
le tocchiamo; le locali le simuliamo noi e le pubblichiamo.

Il modello di proprietà, però, cambia con il protocollo:

* in **CIGI** l'IG non possiede niente. È un visualizzatore: riceve e disegna.
  Verso l'host manda risposte e richieste (collisioni, interrogazioni sul
  terreno), non stato di entità.
* in **DIS** ogni simulatore possiede le proprie entità e le annuncia. Una
  entità posseduta va ripubblicata quando supera una soglia di scostamento
  rispetto a ciò che gli altri stanno estrapolando, **e comunque ogni tot
  secondi** come battito cardiaco. Chi non batte per un po' viene dato per
  scomparso.
* in **HLA** la proprietà è un concetto dell'RTI e può perfino essere
  *trasferita* a runtime.

**Decisione.** La pubblicazione è un adattatore separato dalla ricezione, anche
quando parla lo stesso protocollo. Un `IGeoEntitySource` e un `IGeoEntitySink`,
non un'unica interfaccia bidirezionale: le due direzioni hanno ritmi, errori e
motivi di fallimento diversi, e un'interfaccia sola costringerebbe a metodi che
metà delle implementazioni lascia vuoti.

---

## 9. Architettura e thread

```
  rete / memoria condivisa
        |
   [ adattatore ]  <- thread di I/O, uno per sorgente
        |          traduce nel formato canonico
        v
   coda MPSC      <- l'unico punto di contatto
        |
   [ UGeoEntitySubsystem ]  <- game thread: registro, estrapolazione, spawn
        |
   [ attori / componenti ]  <- trasformazione derivata dall'ECEF
```

Tre regole, e sono le stesse di sempre in questo progetto:

1. **Nessun socket sul game thread.** Una `recvfrom` che si blocca è un frame
   perso, ed è la stessa ragione per cui la Fase 3 ha un pool dedicato alle
   letture da disco. `FGeoLoaderPool` è già lì e non è legato ai file.
2. **Il registro vive sul game thread e basta**, come la cache delle tile. I
   worker producono e consegnano via coda; nessun lock nel percorso caldo.
3. **La traduzione avviene sul worker**, non sul game thread. Il game thread
   riceve stati canonici già pronti, esattamente come riceve pixel già
   decodificati dalla Fase 6.

Modulo nuovo, `GeoEntities`, diviso come gli altri: uno strato **puro** con
`FEntityState`, le conversioni di frame e l'estrapolazione — tutto testabile in
un secondo senza il motore — e uno strato Unreal con il subsystem e gli attori.

Gli adattatori stanno in moduli **separati e opzionali**, uno per protocollo.
Il motivo è pratico: l'HLA richiede di collegare un RTI, che è quasi sempre un
prodotto commerciale con la sua licenza. Se l'adattatore HLA fosse dentro il
modulo principale, nessuno potrebbe compilare il progetto senza avere quell'RTI.

---

## 10. Quello che è già fatto e non ce ne accorgiamo

Tre cose che le fasi precedenti regalano a questa.

**Il rebasing è gratis.** Un'entità tiene la propria posizione in ECEF e la sua
trasformazione Unreal è derivata, esattamente come una tile di terreno. Quando
l'origine si sposta, si ricalcolano le trasformazioni e **nessuna posizione
viene toccata**. È lo stesso `RefreshTransforms` della Fase 5, e vale anche qui
che un'implementazione che ricalcolasse le posizioni vanificherebbe la Fase 1.

**Il problema delle quote si ripresenta identico.** Un'entità posizionata in
lat/lon/altitudine porta la stessa domanda della Fase 2: quell'altitudine è
sull'ellissoide o sul livello del mare? I due valori in Italia differiscono di
una cinquantina di metri, che è abbastanza per mettere un carro armato sotto
terra. La risposta dipende dal protocollo e dalla versione, va **dichiarata per
ogni sorgente** e non indovinata. La pipeline della Fase 2 ha già gli strumenti
per la conversione.

**Il ground clamp chiede il terreno.** CIGI prevede che l'host dica "appoggia
questa entità al suolo" e lasci all'IG il calcolo della quota. Farlo significa
campionare l'altezza del terreno sotto l'entità, cioè interrogare la cache
delle tile della Fase 3 — che potrebbe non avere quella tile in memoria. Serve
una politica dichiarata: appoggiare con l'ultima quota nota e correggere quando
la tile arriva, oppure tenere l'entità nascosta finché il terreno non c'è. La
prima è quasi sempre quella giusta, ed è la stessa scelta del drappeggio della
Fase 6: meglio approssimato subito che esatto fra tre frame.

---

## 11. Numeri, per avere un'idea della scala

| | |
|---|---|
| Entità in un esercizio DIS di dimensioni normali | da qualche decina a qualche migliaio |
| Frequenza tipica di aggiornamento per entità | 1-5 Hz, più il battito |
| Frequenza CIGI | uno stato a ogni frame, 30-60 Hz |
| Uno stato canonico | ~150 byte |
| 5.000 entità in registro | meno di un megabyte |

Il registro non è il problema. Il problema sono gli **attori**: 5.000 attori
Unreal con i loro componenti costano molto più dei dati che rappresentano. La
conseguenza di design è che **il registro e gli attori vanno tenuti separati**:
tutte le entità stanno nel registro, e solo quelle visibili e abbastanza vicine
ricevono un attore. È la stessa relazione che c'è fra il quadtree e le mesh
della Fase 5, ed è il motivo per cui la Fase 4 esiste.

---

## 12. Da dove comincerei

In ordine, e ognuno è verificabile prima del successivo.

1. **Lo strato puro**: `FEntityState`, le conversioni di frame, l'estrapolazione.
   Zero Unreal, zero rete, tutto coperto da test di round-trip. È la parte dove
   stanno le tre trappole della sezione 4, ed è la sola che posso verificare
   davvero senza il motore.
2. **La memoria condivisa**, non CIGI. Sembra controintuitivo, ma è il
   trasporto più semplice — nessun parsing, nessuna rete — e permette di vedere
   entità che si muovono sul terreno con un generatore finto scritto in Python.
   Serve a validare tutto ciò che sta *dopo* l'adattatore prima di affrontare un
   protocollo vero.
3. **CIGI**, perché è quello per cui l'IG esiste, e perché il modello
   host-comanda è il più semplice: nessuna estrapolazione, nessuna proprietà.
4. **DIS**, che aggiunge il dead reckoning, la proprietà e le enumerazioni.
5. **HLA** per ultimo, perché richiede un RTI di terze parti e perché, a valle
   dell'RPR-FOM, la semantica è quella di DIS: se DIS funziona, il grosso del
   lavoro concettuale è già fatto.

Una nota sul punto 2, perché è la decisione meno ovvia di questo capitolo: la
tentazione è partire dal protocollo più importante. Ma un protocollo vero
introduce insieme il parsing, la rete, le convenzioni e l'estrapolazione, e
quando qualcosa non si muove come dovrebbe non si sa quale dei quattro
incolpare. Con la memoria condivisa e un generatore finto, i primi tre sono
fuori discussione e resta solo il quarto.

---

## 13. Il primo pezzo: cosa c'è, e cosa si è deciso scrivendolo

Fatti i punti 1 e 2 della sezione 12, e il modulo Unreal che li usa.

### 13.1 Dove sta

| | |
|---|---|
| `GeoEntities/Public/Entities/EntityState.h` | lo stato canonico, il quaternione, la chiave (sorgente + numero), il tipo di DIS, gli algoritmi |
| `.../Entities/EntityFrames.h` | le tre trappole: NED, Eulero 3-2-1, angoli locali (CIGI) e ECEF (DIS), gli assi dell'attore, la quota |
| `.../Entities/DeadReckoning.h` | l'estrapolazione parametrica, il rientro (`FEntityTrack`), l'ora di DIS, l'orologio della simulazione |
| `.../Entities/EntityRegistry.h` | il registro: aggiornamenti, disordine, riavvii, silenzi, geoide |
| `.../Entities/EntityModels.h` | la tabella dei modelli (tipo o indice CIGI → forma o asset), il ripiego |
| `.../Entities/SharedMemoryFormat.h` | il formato della memoria condivisa, il seqlock, la traduzione |
| `GeoEntities/Public/Sources/GeoEntitySource.h` | l'interfaccia di una sorgente e la coda MPSC |
| `GeoEntities/Private/Sources/GeoSharedMemory*.{h,cpp}` | la memoria su Windows/Linux/macOS e il thread che la legge |
| `GeoEntities/Public/GeoEntitySubsystem.h` | il subsystem: coda, registro, attori, suolo, nomi, camera |
| `Pipeline/geoworld/entitygen.py` | il generatore finto (`geoworld demo-entities`) e la fixture dei test |
| `Tools/StandaloneTests/geoentities_main.cpp` | 97 test, compresa la fixture di Python |

Lo strato puro è header-only e senza Unreal, come gli altri; i controlli
statici (`CheckSourceDiscipline.sh`, `CheckModuleExports.py`) lo sanno.

### 13.2 Decisioni prese scrivendo

**Gli angoli della memoria condivisa sono quelli di CIGI** (rispetto al NED del
punto), le velocità in NED. È il formato che un simulatore scrive più
facilmente, ed esercita due delle tre trappole fin dal primo giorno. Le quote
sono sul livello del mare o sull'ellissoide secondo un bit dell'intestazione:
dichiarate, non indovinate (sezione 10).

**La memoria condivisa è una fotografia, non un flusso.** Ci sono tutte le
entità vive adesso; chi sparisce dalla fotografia se ne va (la sorgente manda
un "rimuovi"). La coerenza fra i due processi è un **seqlock** (sequenza
dispari durante la scrittura): nessun lock fra processi, chi scrive non
aspetta mai.

**Il nome della memoria si apre com'è.** `FPlatformMemory::MapNamedSharedMemoryRegion`
di Unreal, su Windows, mette `Global\` davanti al nome (lo spazio di tutto il
sistema, che per creare vuole l'amministratore); Python crea nello spazio della
sessione. Un wrapper nostro, `FGeoSharedMemoryRegion`, usa
`OpenFileMappingW`/`shm_open` con il nome nudo. Se la sequenza resta ferma
per 2 s la memoria si riapre: su Linux un generatore ripartito ne crea una
nuova con lo stesso nome.

**Negli algoritmi del corpo (6–9) l'accelerazione nel corpo è
`R⁻¹ a − ω × v`.** Lo stato canonico ha l'accelerazione nel mondo, che in
virata contiene già la centripeta; la rotazione del corpo la produce da sé.
La prima versione la contava due volte (5 m di errore in un secondo, trovato
dal test). L'adattatore DIS farà il contrario: `a = R (ab + ω × vb)`.

**Il rientro** smaltisce un errore nel mondo (un vettore e una rotazione a
sinistra) con `3u² − 2u³`; 0,4 s di default (`geo.Entities.Blend`); oltre
100 m o 60° si salta e si conta; oltre 5 s senza notizie ci si ferma.

**Il tempo**: `FClockSync` per sorgente, il minimo degli scarti fra il tempo
di simulazione e il nostro, che risale piano (1% a campione) e riparte oltre
2 s di salto. Un aggiornamento più vecchio dell'ultimo si scarta; più vecchio
di un secondo vuol dire simulazione ripartita.

**I nomi sul Canvas, non nella scena**: un `UTextRenderComponent` di taglio non
si legge e da lontano è un puntino; il Canvas (come le stampe della posizione)
li tiene dritti e leggibili, anche nell'editor senza Play.

**Il suolo** è il terreno **disegnato** della tile più fine in memoria
(`UGeoPositionHudSubsystem::GroundHeightAt`, reso pubblico e statico, ora con
`PeekLoadedTile` per non falsare le statistiche della cache), con l'ultima
quota nota quando la tile non c'è (la politica della sezione 10). Per ora
solo la quota, non l'assetto sul pendio.

**Il geoide** dal dataset dell'acqua se c'è; altrimenti i 48 m dell'Italia
(`Places::ItalyGeoidUndulationM`), e `geo.Entities.List` lo dice.

**Le righe del riquadro** dei moduli che GeoRender non conosce arrivano da un
delegato (`UGeoPositionHudSubsystem::ExtraLines`): GeoEntities dipende da
GeoRender, non il contrario.

### 13.3 Il prossimo pezzo

CIGI (sezione 12, punto 3): un adattatore UDP sul suo thread, le Entity
Control (angoli locali come la memoria condivisa, il modello per indice: la
tabella ha già `cigi N`), il ground clamp anche di assetto, le risposte
all'host (Start of Frame, le interrogazioni di quota). Poi DIS, con il
timestamp (`FHourUnwrapper` c'è), gli algoritmi (ci sono), le enumerazioni (la
tabella c'è) e la direzione di uscita, con un'interfaccia sua.

In parallelo, **i modelli veri** al posto delle scatole: letti da una
cartella anche fuori dal progetto (OpenFlight, OBJ; glTF in arrivo), con le
parti mobili, e poi termica, collisioni e danno. Il design e la scaletta sono
in `docs/modelli-design.md`; il primo pezzo (M1) c'è (sezione 12 di quel
documento).

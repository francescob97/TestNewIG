# Capitolo 20 — Le entità: aerei, veicoli, navi che arrivano da fuori

> Come si fa entrare nel mondo di GeoWorld qualcosa che si muove e che non
> decidiamo noi: un aereo di un simulatore di volo, i carri di un'esercitazione,
> le auto di un generatore di traffico. Lo stato canonico, le tre trappole delle
> convenzioni, l'estrapolazione fra un aggiornamento e l'altro, il tempo, la
> memoria condivisa. Il capitolo di progetto è `docs/fase7-design.md`; come
> provarlo è in `docs/fase7-verifica.md`.

---

## L'idea in breve

Fino al capitolo 19 GeoWorld disegna un pianeta **fermo**: terreno, strade,
acqua, alberi, edifici, cielo. Questa fase gli fa ricevere lo stato di
**entità** che si muovono, e lo fa da fuori: è il mestiere di un *Image
Generator* (IG), il visualizzatore di un simulatore. Il simulatore decide dove
sta l'aereo; l'IG lo disegna.

I protocolli sono quattro (CIGI, DIS, HLA, memoria condivisa) ma il contenuto
è sempre lo stesso:

```
chi sono + che cosa sono + dove + come orientato + come mi muovo + quando
```

Quindi il lavoro **non** è "scrivere quattro sottosistemi". È scrivere **una**
rappresentazione interna, e tanti **adattatori** sottili che traducono verso
di lei. Questo primo pezzo ha:

1. lo **strato puro**: lo stato canonico, le conversioni di frame (con le tre
   trappole), l'estrapolazione con il rientro morbido, il tempo, il registro,
   la tabella dei modelli, il formato della memoria condivisa — tutto provato
   da 97 test senza Unreal;
2. la **memoria condivisa** e un **generatore finto** in Python
   (`geoworld demo-entities`): due aerei, un elicottero e quattro auto sopra
   Torino;
3. il modulo Unreal **`GeoEntities`**: una sorgente su un thread suo, il
   registro, gli attori, il suolo, la camera che segue, `geo.Entities.*`.

CIGI, DIS e HLA verranno dopo, uno per volta, ognuno come un adattatore in più.

---

## 20.1 Lo stato canonico

Dentro il motore esiste **una sola** forma (`Entities/EntityState.h`):

| | | |
|---|---|---|
| posizione | ECEF, metri | l'unica autorità dalla Fase 1 |
| orientamento | quaternione, dal **corpo** all'ECEF | |
| velocità, accelerazione | ECEF | |
| velocità angolare | negli assi del **corpo** | |
| tempo | secondi di simulazione | |

**Perché ECEF.** È già la regola del terreno (capitolo 2): la posizione vera è
in ECEF, quella di Unreal si **ricava**, con l'origine di adesso. Così il
rebase non tocca le entità, come non tocca le tile. E DIS usa già l'ECEF: sul
protocollo più diffuso la conversione è zero.

**Il corpo** è quello dell'aeronautica: X avanti, Y a destra, **Z in basso**
(*FRD*). Un aereo che vira a destra gira attorno al suo Z: positivo.

**Perché un quaternione.** Gli angoli di Eulero sono quello che i protocolli
trasmettono, e vanno benissimo **sul filo**. Dentro sono una trappola: a
beccheggio ±90° imbardata e rollio si confondono (il *gimbal lock*), non si
interpolano bene, e ogni protocollo li definisce a modo suo. Un quaternione
non ha convenzioni: o è giusto o è sbagliato, e lo si verifica con un
andata-e-ritorno.

> 💡 **Il quaternione in una riga.** Una rotazione di un angolo θ attorno a un
> asse unitario **u** è il quaternione (cos θ/2, **u** sin θ/2). Comporre due
> rotazioni è moltiplicare i quaternioni; ruotare un vettore è `q v q*`.
> `FQuatD` ha `FromAxisAngle`, `Rotate`, `ToMatrix`, `FromMatrix` (il metodo
> di Shepperd, stabile anche vicino a 180°) e `Slerp`.

---

## 20.2 Le tre trappole

Nella storia di questo progetto i bug lunghi sono nati tutti da una
convenzione data per scontata (ENU contro NEU, quote ortometriche contro
ellissoidiche, post contro pixel). Qui ce ne sono tre insieme, e stanno tutte
in un file solo: `Entities/EntityFrames.h`.

### Trappola 1 — NED non è NEU

La simulazione usa **NED**: Nord, Est, **Giù**. GeoWorld usa **NEU**: Nord,
Est, Su. Una velocità verticale di **+5** in NED vuol dire che l'entità
**scende**. Letta come NEU sale: aerei che salgono mentre atterrano.

`MakeNedBasis` è `MakeNeuBasis` con la terza riga capovolta: NED è destrorso
(determinante +1), NEU sinistrorso (−1). Il test lo dice così:

```
[ ok ] una velocita' NED di +5 sulla terza componente SCENDE di 5 m/s
```

### Trappola 2 — gli angoli di CIGI e quelli di DIS

Tutti e due mandano tre angoli 3-2-1 (imbardata, beccheggio, rollio), ma:

- **CIGI** (e la nostra memoria condivisa) li misura rispetto al **NED del
  punto** dove l'entità si trova: imbardata 0 = il nord di lì;
- **DIS** li misura rispetto agli **assi ECEF**, fissi nel centro della Terra.

Un aereo che punta a nord ha imbardata CIGI 0 a Roma e a Tokyo; i suoi angoli
DIS sono completamente diversi:

```
Roma psi -167.5 theta -48.1, Tokyo psi -40.3 theta -54.3
```

E all'equatore, a Greenwich, un aereo livellato verso nord ha θ DIS = −90°: è
il caso che si trova nei libri, e il test lo controlla. La conversione passa
dalla base del punto: `OrientationFromLocalEuler` compone la base NED con la
rotazione degli angoli, `OrientationFromDisEuler` usa gli angoli e basta.

### Trappola 3 — gli assi del corpo e quelli dell'attore

Il corpo ha Z in **basso**, un attore di Unreal in **alto**. Gli assi
dell'attore sono avanti, destra e il **contrario** di Z del corpo:

```cpp
Forward = q.Rotate( 1, 0,  0)
Right   = q.Rotate( 0, 1,  0)
Up      = q.Rotate( 0, 0, -1)     // Z del corpo e' in basso
```

Portati nello spazio di Unreal con la base della georeferenziazione
(`ActorAxesInUnreal`), formano una **rotazione vera** (determinante +1): il
corpo FRD è destrorso, il capovolgimento di Z lo fa sinistrorso, e Unreal è
sinistrorso. Due riflessioni che si annullano: `FMatrix::ToQuat` è lecito.

Le **velocità angolari** sono più sottili. Negli assi dell'attore diventano
(−p, −q, +r): un rollio a destra di DIS è **negativo** per l'attore. Sembra
strano che cambino X e Y, uguali nei due frame, e non Z, che è capovolto. Il
motivo è che una velocità angolare non è solo un asse ma anche un **verso**
di rotazione, deciso dalla regola della mano destra; in un frame sinistrorso
quella regola gira al contrario. Quindi tutte e tre cambiano segno per la
mano, e Z cambia ancora per l'asse capovolto. Il test non si fida del
ragionamento: ruota il corpo di un attimo e controlla che, con le componenti
dell'attore, `v' = ω × v` torni sui tre assi.

> 🎯 **La regola che ne discende.** Ogni conversione di frame avviene **una
> volta sola, in un punto solo**: negli adattatori, con le funzioni di
> `EntityFrames.h`. Dentro il motore c'è solo lo stato canonico.

---

## 20.3 Fra un aggiornamento e l'altro: l'estrapolazione

Gli aggiornamenti arrivano da 1 a 30 volte al secondo; il motore disegna a
60–120. Fra uno e l'altro **qualcuno deve inventare** dove sta l'entità,
altrimenti si muove a scatti. DIS lo chiama *dead reckoning* (la "navigazione
stimata" dei marinai) e ne definisce nove algoritmi:

| | orientamento | posizione | negli assi |
|---|---|---|---|
| 1 fermo | fermo | ferma | |
| 2 FPW, 5 FVW | fermo | + v t (+ a t²/2) | del mondo |
| 3 RPW, 4 RVW | ruota | + v t (+ a t²/2) | del mondo |
| 6 FPB, 9 FVB | fermo | come 2 e 5 | del corpo |
| 7 RPB, 8 RVB | ruota | la velocità gira **con** il corpo | del corpo |

Non nove implementazioni: **una**, parametrica (`Extrapolate` in
`Entities/DeadReckoning.h`). L'algoritmo è un suggerimento che sceglie i
termini.

**L'orientamento** che ruota: `q(t) = q₀ · exp(ω t / 2)`, con ω negli assi
del corpo. Esatto per ω costante. La rotazione del corpo si compone a
**destra**: prima il corpo gira su se stesso, poi lo si porta nel mondo.

**W o B si vede solo in virata.** Un aereo che vira con la velocità "del
mondo" prosegue per la tangente; con quella "del corpo" la velocità gira con
lui e continua a girare. Il test: 100 m/s, 6 gradi al secondo, dopo mezzo
giro è a 2r verso destra (1.909,86 m) e dopo un giro intero torna al punto di
partenza entro un millimetro.

> ⚠️ **L'errore che i test hanno trovato.** La prima versione, con
> l'algoritmo 8 (RVB), portava nel corpo l'accelerazione **del mondo**. In
> virata quella contiene già la centripeta, e la rotazione del corpo la
> produce un'altra volta: la virata veniva **doppia**, 5 m di errore in un
> secondo. Nel corpo va solo la variazione della velocità vista dal corpo:
> `ab = R⁻¹ a − ω × vb` (il teorema del trasporto). In una virata stabile è
> zero, e il cerchio è esatto.

### Il rientro

Quando arriva l'aggiornamento successivo, quasi mai coincide con quello che
avevamo inventato. Saltarci sopra è uno **scatto**; ignorarlo è **deriva**.

`FEntityTrack` fa così: all'arrivo misura l'**errore**, cioè dove l'entità è
disegnata meno dove dovrebbe essere, in posizione (un vettore) e in assetto
(una rotazione). Poi lo porta a zero in `BlendSeconds` (0,4 s, regolabile con
`geo.Entities.Blend`) con una curva morbida, `3u² − 2u³`, che parte e arriva
piatta:

```
disegnato(t) = vero(t) + errore · (1 − curva(t))
```

All'arrivo il disegno **non salta**: è esattamente dove era un attimo prima.
A metà rientro c'è metà errore; alla fine è sulla verità. Oltre i 100 m (un
teletrasporto, un riposizionamento) non si rientra: si salta, e lo si conta.
Dopo 5 s senza notizie si smette di inventare.

---

## 20.4 Il tempo

Ogni protocollo ha il suo tempo, e nessuno è il nostro.

**L'ora di DIS.** Il timestamp di DIS è di 31 bit, e 2³¹ unità fanno **un'ora**
(1,676 µs l'una): ogni ora riparte da zero. Se non lo si sa, una volta all'ora
le entità vengono estrapolate di un'ora all'indietro, schizzano via e
rientrano — il peggior intervallo possibile per accorgersene. `FHourUnwrapper`
conta le ore: un valore che torna indietro di più di mezz'ora è l'ora nuova,
uno che va avanti di più di mezz'ora è un pacchetto in ritardo dell'ora
vecchia.

**L'orologio della simulazione.** Un aggiornamento dice "vero al tempo T"
(della simulazione) e arriva al tempo L (nostro). `L − T` è lo scarto fra i
due orologi **più** il ritardo del viaggio, che è sempre positivo. Il pacchetto
più veloce dà la stima migliore: `FClockSync` tiene il **minimo** degli
scarti, lo lascia risalire piano (un orologio che deriva) e riparte da capo se
cambia tutto (la simulazione riavviata). Con ritardi a caso da 2 a 40 ms, lo
scarto trovato è giusto entro 5 millesimi.

---

## 20.5 Il registro, e perché non è gli attori

Il registro (`Entities/EntityRegistry.h`) tiene **tutte** le entità: la
chiave, il tipo, il nome, la traccia (stato + estrapolazione + rientro).

- **La chiave è sorgente + numero.** Due sorgenti possono usare lo stesso
  numero: l'entità 42 della memoria condivisa e la 42 di un host CIGI sono
  due entità.
- **Un pacchetto più vecchio** dell'ultimo si scarta (UDP non garantisce
  l'ordine) e si conta; uno **molto** più vecchio (più di un secondo) vuol dire
  che la simulazione è ripartita, e la traccia ricomincia.
- **Chi tace** da più di 5 s se ne va (`geo.Entities.Timeout`): è il battito
  cardiaco di DIS.
- **Le quote sul livello del mare** si alzano dell'ondulazione del geoide
  (capitolo 3: in Italia ~48 m, abbastanza per mettere un carro sotto terra).
  Lo dichiara la sorgente, non si indovina.

Il registro **non** sono gli attori. Cinquemila entità occupano meno di un
megabyte; cinquemila attori di Unreal no. Ogni quarto di secondo il subsystem
sceglie le più vicine alla camera, entro un raggio (30 km) e al massimo un
numero (400): solo quelle hanno un attore. È la stessa relazione che c'è fra
il quadtree e le mesh (capitolo 5).

---

## 20.6 Che cosa disegnare: la tabella dei modelli

L'identità dice **chi** è un'entità; il tipo dice **che cosa** è; il modello è
un'altra cosa ancora. DIS manda sette numeri (genere, dominio, paese,
categoria, sottocategoria, specifico, extra: `1.2.105.1.0.0.0` è una
piattaforma aerea italiana da caccia); CIGI un indice del database dell'IG.

La traduzione è una **tabella di dati**, non codice: l'elenco dei tipi di DIS
cambia più volte l'anno, e non si ricompila il motore per un elicottero.

```
# tipo (7 campi, * = qualsiasi)    modello
1.1.105.1.*.*.*                    box 7 3.5 2.4
1.2.*.*.*.*.*                      plane 15 12 4
1.2.*.21                           heli 12 11 3.5
cigi 12                            /Game/Veicoli/SM_Camion.SM_Camion
```

- i campi mancanti in fondo valgono `*`;
- vince la regola **più specifica**; a parità, l'**ultima** (si aggiungono
  correzioni in fondo, come in un foglio di stile);
- `cigi N` viene prima delle regole sul tipo;
- un modello è un **asset** di Unreal, oppure una forma fatta di scatole:
  `box` (un veicolo, appoggiato sull'origine), `plane` (fusoliera, ala, piano
  di coda, deriva), `heli` (cabina, trave di coda, disco del rotore);
- le righe sbagliate si saltano e si elencano con il loro numero.

Se nessuna regola del file vale, si usa la **tabella di serie**, una forma per
dominio (un'auto per la terra, un aereo per l'aria, una nave...), e **lo si
dice**: l'entità diventa magenta e il nome porta un `?` con il tipo. Un'entità
disegnata come una scatola con scritto il suo tipo è infinitamente più utile
di una che non compare.

---

## 20.7 La memoria condivisa

È il trasporto più semplice che c'è: nessuna rete, nessun protocollo. Un altro
programma tiene aperta una zona di memoria con un **nome** e ci scrive lo
stato di tutte le sue entità; noi la leggiamo. Per questo si comincia da qui
(e non da CIGI): se un'entità non si muove come dovrebbe, rete e parsing sono
fuori discussione, e resta solo quello che sta dopo l'adattatore.

Il formato (`Entities/SharedMemoryFormat.h`, tutto little-endian):

```
intestazione 64 byte   "GENT", versione, misure, bandiere, capacità, quante,
                       SEQUENZA (offset 24), tempo di simulazione (offset 32)
un'entità   128 byte   id, tipo (8 byte), bandiere, algoritmo, fazione,
                       indice di modello, lat/lon/quota (double), imbardata,
                       beccheggio, rollio (NED locale, come CIGI), velocità e
                       accelerazione N/E/GIÙ, p/q/r, il nome (32 byte)
```

È una **fotografia**, non un flusso di messaggi: ci sono tutte le entità vive,
adesso. Chi non c'è più nella fotografia se n'è andato.

### Il seqlock

Due programmi leggono e scrivono la stessa memoria **senza un lock** fra loro.
Come fa chi legge a non copiare una scrittura a metà? Con la **sequenza**:

1. chi scrive la porta a un numero **dispari**, scrive, la porta al **pari**
   successivo;
2. chi legge la legge, copia tutto, la rilegge: se era dispari, o se è
   cambiata nel frattempo, ha copiato a metà, e riprova.

Chi scrive non aspetta mai; chi legge riprova raramente (una scrittura di
mille entità dura microsecondi). Il test lo prova con **due thread veri**, uno
che scrive di continuo e uno che legge per 300 ms: ogni fotografia accettata
deve essere intera. Togliendo il controllo della sequenza, lo stesso test
trova migliaia di fotografie rotte: il test sa davvero accorgersene.

### Il nome, su Windows

Unreal ha una sua funzione per la memoria condivisa, ma su Windows mette
davanti al nome `Global\`: lo spazio dei nomi di tutto il sistema, dove creare
vuole i diritti di amministratore. Python crea nello spazio della sessione,
senza prefisso, e Unreal non la troverebbe mai. `FGeoSharedMemoryRegion` la
apre con il nome com'è (`OpenFileMappingW` su Windows, `shm_open` su Linux e
macOS).

### Il generatore

```bat
cd Pipeline
geoworld demo-entities                       :: Torino, 10 volte al secondo, Ctrl+C per fermare
geoworld demo-entities --lat 41.9 --lon 12.5 :: Roma
geoworld demo-entities --crowd 1000          :: piu' mille auto, per provare il carico
```

Tutto si calcola nel piano tangente del **centro**, dove un cerchio è un
cerchio; poi posizione, velocità e assetto si portano nel NED del punto dove
l'entità si trova davvero. A 5 km dal centro il nord locale è girato di 0,05°
rispetto a quello del centro: poco, ma i test lo vedono. È la trappola 2.

Gli aerei virano **coordinati**, senza scivolare: rollio atan(v²/(r·g))
verso l'interno, e nel corpo il rateo di virata ω si divide fra q (beccheggio)
e r (imbardata): `q = ω sin(rollio)`, `r = ω cos(rollio)`.

**Il controllo incrociato.** Il generatore scrive anche un file di prova per
i test C++ (`TestData/entities_fixture.bin`): due fotografie, a 10,0 e 10,5
secondi. Il test C++ estrapola la prima di mezzo secondo e deve ritrovare la
seconda. Se Python e C++ non sono d'accordo su **una** convenzione (NED, il
verso degli angoli, gli assi del corpo), non la ritrova. Risultato:

```
AEREO-1 (RVB): 0.0000 m     (la virata negli assi del corpo e' esatta)
AEREO-2 (RVW): 0.0034 m     (v ω² t³/6: l'errore teorico del terzo ordine)
ELICOTTERO (RPW): 0.1250 m  (a t²/2: senza accelerazione)
```

---

## 20.8 In Unreal

```
sorgenti (thread)  ->  coda MPSC  ->  registro  ->  attori
```

- **La sorgente** (`FGeoSharedMemorySource`) ha un thread suo: guarda la
  sequenza ~500 volte al secondo, e quando cambia copia, traduce nella forma
  canonica **sul suo thread** e mette gli aggiornamenti in una coda MPSC
  (molti produttori, un consumatore). Nessuna attesa sul game thread.
- **Il subsystem** (`UGeoEntitySubsystem`), a ogni frame: vuota la coda nel
  registro, toglie chi tace, sceglie chi ha un attore, dà agli attori la posa
  di adesso — estrapolata, con il rientro, dall'ECEF allo spazio di Unreal con
  l'origine di **adesso**. Nessuna posizione Unreal si conserva: il rebase non
  tocca niente.
- **Il suolo.** Chi chiede di stare appoggiato (le auto) sta sul terreno
  **disegnato**, come gli alberi e i lampioni; se la tile non c'è ancora, con
  l'ultima quota nota, corretta quando arriva.
- **Gli attori** sono scatole del cubo del motore, colorate per fazione (blu
  amico, rosso avversario, verde neutrale, giallo altro, magenta ripiego), o
  un asset della tabella.
- **I nomi** non sono testo nella scena (una `UTextRenderComponent` è una
  mesh: di taglio non si legge) ma scritte sul Canvas di ogni viewport, come
  le stampe della posizione: sempre dritte, sempre leggibili.
- **La camera che segue** (`geo.Entities.Follow AEREO-1`): dietro e sopra,
  secondo la **rotta** e non il muso (un aereo che rolla non fa rollare la
  camera).

I comandi:

| Comando | |
|---|---|
| `geo.Entities.Listen [nome]` | in ascolto della memoria condivisa |
| `geo.Entities.Stop` | ferma tutto e svuota la scena |
| `geo.Entities.List [n]` | sorgenti ed entità più vicine |
| `geo.Entities.Follow <nome\|off>` | la camera segue |
| `geo.Entities.Blend <s>` | la durata del rientro (0: si vedono gli scatti) |
| `geo.Entities.Timeout <s>` | dopo quanto silenzio se ne va |
| `geo.Entities.Radius <m> [max]` | fin dove, e quanti, gli attori |
| `geo.Entities.Models [file\|default]` | la tabella dei modelli, a caldo |
| `geo.Entities.Labels`, `geo.Entities.Debug` | i nomi; velocità ed errore del rientro |

In Project Settings → Plugins → **GeoWorld - Entita'** le stesse cose una
volta per tutte, più `bListenOnStart`.

---

## 20.9 Che cosa manca

- **CIGI**: il prossimo pezzo. L'host comanda, l'IG obbedisce: nessuna
  estrapolazione, nessuna proprietà, il modello per indice. Il ground clamp
  diventerà anche di **assetto** (l'auto che segue il pendio).
- **DIS**: le PDU di stato, il timestamp con l'ora, gli algoritmi 1–9 (già
  pronti qui), le enumerazioni (la tabella c'è), e la direzione **di uscita**
  (pubblicare le entità nostre), con un'interfaccia sua, separata.
- **HLA**: per ultimo, in un modulo **opzionale**: vuole un RTI di terze
  parti, e nessuno deve aver bisogno di un RTI per compilare GeoWorld.
- **I modelli veri** al posto delle scatole: il capitolo 21 (il primo pezzo
  c'è: OpenFlight e OBJ da una cartella qualsiasi, con le parti mobili).

---

## Riassunto

- Una sola forma dentro: **ECEF**, quaternione dal corpo, velocità angolari
  nel corpo, tempo di simulazione. I protocolli sono adattatori.
- Le tre trappole (NED/NEU, angoli locali/ECEF, Z del corpo giù/su) stanno in
  **un file**, ognuna con il suo test di andata e ritorno.
- **Estrapolare** è una funzione sola; il problema vero è il **rientro**:
  l'errore si smaltisce con una curva morbida, senza scatti né deriva.
- Il **tempo** si normalizza all'ingresso (l'ora di DIS, l'orologio della
  simulazione) in un punto solo.
- Il **registro** ha tutte le entità, gli **attori** solo le vicine.
- La memoria condivisa è una fotografia protetta da un **seqlock**; il
  generatore Python e i test C++ si controllano a vicenda.

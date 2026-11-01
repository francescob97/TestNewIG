# I modelli delle entità — decisioni, motivazioni e scaletta

> Il seguito della Fase 7 (`docs/fase7-design.md`): le entità arrivano, si
> muovono, ma sono scatole. Qui diventano **modelli veri**, letti da una
> cartella fuori dal progetto, con le loro parti mobili, e poi con quello che
> serve a una simulazione: la temperatura (per i sensori), le collisioni, il
> danno. La sezione 11 è la scaletta: ogni pezzo si verifica prima del
> successivo. Il racconto per chi impara: `docs/manuale/21-modelli.md`; come
> provarlo: `docs/modelli-verifica.md`.

---

## 1. Cosa si chiede

1. **Caricare i modelli dall'esterno**, anche a gioco avviato, dando una
   cartella fuori dal progetto: `.flt`, `.obj`, `.fbx`, `.ive` e altri, con le
   loro texture.
2. **Parti articolate**: piloni, cavi, missili, torrette, carrelli, rotori;
   **animabili** con curve, con i DOF dei modelli, con file di animazione.
3. **Collisioni**, e il **danno** che ne viene.
4. Una **simulazione termica** delle entità: serve ai sensori (IR) che
   verranno dopo.
5. Altro che serva: sezione 10.

---

## 2. L'osservazione che regge tutto (di nuovo)

È la stessa della Fase 7. I formati sembrano tanti mondi; il contenuto è uno:

```
un albero di nodi
  ogni nodo: una trasformazione, e forse della geometria
  alcuni nodi si MUOVONO (DOF), altri si ACCENDONO e si SPENGONO (switch,
  livelli di dettaglio, sequenze)
la geometria: triangoli, normali, coordinate di texture, colori, materiali
```

Quindi: **una** rappresentazione interna del modello (`Models/ModelData.h`) e
un **lettore per formato** che la riempie, come gli adattatori dei protocolli.
Sopra la rappresentazione, tutto il resto (le parti che si muovono, la
temperatura, le collisioni, il danno) è scritto una volta sola e vale per
tutti i formati.

E una seconda separazione, la stessa del registro e degli attori: **il modello**
(la forma, letta una volta, condivisa da tutte le entità dello stesso tipo) e
**lo stato del modello** in un'entità (quanto è girata la torretta, quale
pezzo è rotto, quanto è calda la marmitta), che è per entità e cambia a ogni
frame.

---

## 3. I formati: quali si leggono, quali si convertono

| Formato | Che cos'è | Aperto? | Qui |
|---|---|---|---|
| **OpenFlight** `.flt` | lo standard dei database di simulazione visiva (Presagis Creator): gerarchia, **DOF**, **switch**, **LOD**, sequenze, riferimenti esterni, palette di texture e materiali, codici IR | **sì**, specifica pubblicata | **letto direttamente**, a runtime (M1) |
| **Wavefront** `.obj` + `.mtl` | geometria statica e materiali semplici, testo | sì | **letto direttamente** (M1) |
| **glTF 2.0** `.gltf`, `.glb` | il formato aperto moderno: gerarchia, materiali PBR, **animazioni**, scheletri | sì (Khronos) | **letto direttamente** (M2): è il ponte per tutto il resto |
| **FBX** `.fbx` | Autodesk, il più diffuso fra i modellatori | **no**: binario chiuso, si legge solo con l'FBX SDK, che nel gioco impacchettato non c'è | **convertito** in glTF (M2) |
| **OpenSceneGraph** `.ive`, `.osgb`, `.osg`, `.osgt` | il formato interno di OSG: `.ive` è la serializzazione binaria delle sue classi C++ | **no**: nessuna specifica, cambia con la versione di OSG; lo legge solo OSG | **convertito** con `osgconv` (di OSG) in `.flt` (conserva DOF, switch, LOD) (M2) |
| 3DS, DAE (Collada), LWO, STL, PLY, BLEND... | vari | vari | **convertiti** in glTF (M2) |

**Perché leggere direttamente, e non importare nel progetto.** L'importatore
di Unreal (Interchange, FBX) esiste solo nell'editor e crea asset nel
progetto: niente cartella esterna, niente gioco impacchettato. Un IG deve
poter ricevere un modello nuovo in una cartella e usarlo **senza ricompilare
né riaprire il progetto**. Quindi i formati aperti si leggono a runtime con
codice nostro, nello strato puro (testato senza Unreal), su un thread di
lavoro; la mesh di Unreal si costruisce dopo, sul game thread, come per gli
alberi (`bFastBuild`, funziona anche impacchettato).

**Perché convertire i formati chiusi, e non leggerli.** FBX si può leggere solo
con l'FBX SDK (licenza Autodesk, e nel gioco impacchettato non c'è); `.ive` lo
sa leggere solo OSG, e non esiste una descrizione del formato da cui scrivere
un lettore. Scriverne uno a tentativi è lavoro enorme che si rompe alla
prossima versione. La conversione è un passo **una volta sola**, fuori da
Unreal, con gli strumenti di chi il formato l'ha fatto: `osgconv` per OSG,
FBX2glTF o Blender per FBX. Diventerà un comando della pipeline
(`geoworld convert-models`) che li usa se ci sono e dice cosa manca se non ci
sono. Per chi preferisce, resta la strada dell'editor: importare l'FBX come
asset (con l'importatore di Unreal, il migliore per gli scheletri) e indicarlo
nella tabella dei modelli con il suo percorso `/Game/...`, che c'è già.

**Le texture.** PNG, JPEG, BMP, TGA, TIFF, DDS non compressi li decodifica
Unreal (`ImageWrapper`). I formati **SGI** (`.rgb`, `.rgba`, `.int`, `.inta`,
`.sgi`), i classici di OpenFlight, li legge un lettore nostro (puro, M1). I
percorsi scritti nei modelli sono spesso quelli della macchina di chi li ha
fatti (`D:\Modelli\Texture\f16.rgb`): si cercano in ordine così come sono,
accanto al modello, nella sua sottocartella `textures`, e per **nome** in tutta
la cartella dei modelli.

---

## 4. Le convenzioni (le trappole di questa fase)

Come nella Fase 7, i bug lunghi verranno da qui. Si convertono **in un punto
solo**, alla fine della lettura (`Models/ModelRig.h`), e ognuna ha il suo
test.

| | OpenFlight | OBJ (di solito) | glTF | Attore di GeoWorld |
|---|---|---|---|---|
| avanti | +Y | −Z | +Z | +X |
| alto | +Z | +Y | +Y | +Z |
| mano | destra | destra | destra | **sinistra** |
| unità | nell'intestazione (m, km, piedi, pollici, miglia) | non dette (metri) | metri | metri |
| origine delle UV | in basso | in basso | in alto | in alto |
| faccia davanti | antioraria | antioraria | antioraria | (B−A)×(C−A) verso fuori |

Tre conseguenze, da non dimenticare:

1. **Passare da destra a sinistra è una riflessione** (determinante −1): i
   triangoli vanno girati, altrimenti si vedono da dentro. Vale anche per le
   matrici con una scala negativa nei modelli (le ali simmetriche fatte
   "a specchio"): ogni pezzo con determinante negativo si gira.
2. **L'asse "avanti" di un modello non è garantito.** La convenzione del
   formato è un'abitudine, non una legge: molti modelli guardano altrove. La
   tabella dei modelli ha `yaw`, `up`, `forward`, `scale`, `offset` per regola.
3. **Gli angoli dei DOF di OpenFlight** sono matematici: l'imbardata positiva
   gira in senso **antiorario** visto dall'alto, al contrario della bussola e
   degli angoli di DIS e CIGI. Il passaggio fra i due sta nella tabella delle
   articolazioni (M3), non nel lettore.

---

## 5. Il modello e il "rig"

`ModelData.h` è fedele al file: l'albero dei nodi com'è, la geometria nelle
coordinate del formato. **Il rig** (`ModelRig.h`) è quello che si disegna:

- tutto in assi dell'attore e in metri, triangoli girati dove serve;
- i nodi che **non si muovono e non si spengono** (gruppi, oggetti, matrici)
  **spariscono**: la loro trasformazione si cuoce nei vertici. Un modello di
  OpenFlight ha spesso centinaia di gruppi; diventerebbero centinaia di
  componenti di Unreal, ognuno con il suo costo di disegno;
- restano **parti** solo dove serve: la radice, ogni **DOF** (una parte che si
  muove), ogni figlio di **switch**, di **LOD**, di **sequenza** (una parte che
  si accende e si spegne). Ogni parte ha le sue **sezioni**, una per
  materiale, e le **condizioni** per essere visibile (quale figlio di quale
  switch, a che distanza);
- il movimento di un DOF è `Prima · Moto(valori) · Dopo`: le due matrici fisse
  portano dal sistema del DOF a quello della parte madre, il moto (traslazione,
  rotazioni attorno agli assi del DOF, scala) è quello del file, con i suoi
  limiti.

In Unreal: un attore per entità, un componente per parte (`USceneComponent`
per il perno del DOF, `UStaticMeshComponent` per la geometria), una
`UStaticMesh` per parte **condivisa** fra tutte le entità con lo stesso
modello. Le texture e le mesh si creano una volta per modello.

---

## 6. Parti articolate e animazione (M3)

Una parte si muove per una di quattro ragioni. Tutte finiscono nello **stesso**
posto, i valori dei DOF dello stato del modello:

| Da dove | Esempio | Come |
|---|---|---|
| il **protocollo** | la torretta di un carro DIS, i flap di un aereo CIGI | DIS: i *parametri articolati* dell'Entity State PDU (tipo 4096 torretta, 4416 elevazione del cannone...). CIGI: *Articulated Part Control* (numero della parte, spostamenti, angoli). Una **tabella delle articolazioni** per modello dice quale numero va a quale DOF, con quale segno |
| una **curva** | il carrello che scende in 8 s, un portellone | chiavi (tempo, valore) con interpolazione a gradino, lineare o cubica (Hermite); una curva per DOF e asse; in un file accanto al modello o nella tabella |
| la **fisica semplice** dell'entità | rotore che gira con i giri, ruote che rotolano con la velocità, eliche, cingoli | regole procedurali: angolo += velocità / raggio × dt |
| un **file di animazione** | una sequenza fatta nel modellatore | le animazioni di glTF (canali traslazione/rotazione/scala sui nodi); quelle di FBX passano per glTF; le **sequenze** di OpenFlight (gruppi animati: i fotogrammi si accendono a turno) |

**I cavi.** Un cavo fra due punti (le linee elettriche fra due piloni, un cavo
di traino, il filo di un missile filoguidato) è una **catenaria**:
`z = a cosh(x/a)`, con `a` = tensione orizzontale / peso al metro. Dati i due
estremi e la lunghezza (o la freccia), `a` si trova con qualche iterazione di
Newton; il cavo si ridisegna quando un estremo si muove. Puro, testabile (la
freccia di una campata da 300 m, la simmetria, la lunghezza).

**I carichi esterni (missili, serbatoi).** Un modello ha dei **punti di
attacco** (le stazioni sotto le ali); un carico è un altro modello agganciato
lì. Quando parte (DIS: una nuova entità munizione; CIGI: il comando
dell'host; o un nostro evento) si **stacca**: diventa un'entità propria,
**locale** (la simuliamo noi: balistica, o una guida semplice), con il suo
motore caldo (sezione 9) e la sua scia.

---

## 7. Collisioni (M5)

**Chi fa cosa.** In un simulatore distribuito la dinamica la possiede l'host:
l'IG non fa rimbalzare gli aerei. All'IG si chiede di **accorgersi** e di
**dirlo**: CIGI ha proprio queste richieste (segmenti e volumi di collisione,
quota del terreno sotto un punto — HAT/HOT —, linea di vista — LOS —) e le
risposte da mandare indietro. La fisica vera serve solo a quello che
simuliamo noi: i carichi staccati, i detriti del danno.

**Decisioni.**

- **Le forme di collisione** per parte: scatole, sfere, capsule. Dal file
  (OpenFlight ha i volumi di contenimento), da un descrittore accanto al
  modello, o fatte da sole (la scatola orientata di ogni parte). Seguono le
  parti: una torretta girata ha la sua scatola girata.
- **Il test** in due tempi, come sempre: prima chi è vicino a chi (una griglia
  nello spazio locale attorno alla camera), poi le forme vere (scatola contro
  scatola con gli assi separatori, sfera, capsula, segmento contro tutto).
- **Il terreno**: un segmento contro la superficie DISEGNATA (la stessa delle
  auto appoggiate e degli alberi), con la quota del suolo campionata lungo il
  segmento. Gli edifici dopo, con le loro sagome.
- **Il risultato** è un evento: chi, contro chi o che cosa, dove, la normale,
  la velocità relativa, l'energia. Va al danno (sezione 8), all'host (le
  risposte di CIGI), a Blueprint.
- **In Unreal** le parti hanno anche una collisione "solo interrogazioni":
  così le tracce di Unreal (il mouse che sceglie un'entità, i sensori che
  guardano) le colpiscono. Nessuna fisica di Unreal sulle entità remote.

---

## 8. Danno (M6)

Il danno ha due ingressi e tre uscite.

**Ingressi.** Il **protocollo** (DIS: i bit di aspetto dell'Entity State —
nessuno, leggero, medio, distrutto; fumo, fiamme —; CIGI: i comandi di
componente dell'host) e gli **eventi nostri** (una collisione, un impatto: il
danno di una parte cresce con l'energia oltre una soglia).

**Lo stato.** Per entità un livello (intatto, leggero, medio, distrutto) e per
parte una "salute" da 1 a 0. Si sale solo: un'entità non si ripara da sola.

**Uscite.**

1. **Come si vede**: lo switch del danno del modello se c'è (molti modelli
   OpenFlight hanno uno switch "damage" con le versioni intatta e distrutta),
   o un modello alternativo dalla tabella; altrimenti il materiale si annerisce
   (bruciato) a chiazze. Fumo e fiamme: particelle come la pioggia (una mesh
   aperta dal materiale), legate a un punto del modello.
2. **Cosa si stacca**: le parti oltre la soglia diventano detriti, entità
   locali con una balistica semplice.
3. **Il calore**: un incendio è una sorgente di calore enorme per l'IR
   (sezione 9). Un mezzo bruciato resta caldo a lungo.

E le articolazioni di una parte distrutta si bloccano.

---

## 9. La simulazione termica (M4)

**A cosa serve.** Un sensore IR non vede il colore: vede la **radianza**
emessa, cioè la temperatura e l'emissività di ogni superficie, attenuata
dall'aria. Per disegnare l'immagine IR di un carro serve sapere quanto è caldo
il motore, la marmitta, i cingoli, la corazza al sole e quella all'ombra.

**Il modello: nodi termici concentrati.** Ogni entità ha poche **zone**
(scafo, motore, scarico, pneumatici o cingoli, freni, abitacolo, canna, ugello
e pennacchio per i missili). Ogni zona è un nodo con:

- una **capacità termica** `C` (J/K): quanto ci mette a scaldarsi;
- un'**area** esposta, un'**emissività** ε e un **assorbimento solare** α
  (vernice chiara o scura);
- una **potenza interna** `P` (W): il motore acceso al regime dato, lo
  sparo, le frecce dei freni, il fuoco;
- **conduttanze** verso le altre zone (il motore scalda lo scafo).

L'equazione di ogni nodo:

```
C dT/dt = P                                  potenza interna
        + α A E_sole cos(angolo)             il sole (dal cielo: direzione e luce)
        + ε σ A (F_cielo T_cielo⁴ + F_suolo T_suolo⁴ − T⁴)   irraggiamento
        + h A (T_aria − T)                    convezione (h cresce con la velocità
                                              dell'aria: vento + moto dell'entità)
        + Σ G_ij (T_j − T)                    conduzione fra zone
```

Si integra con Eulero implicito (stabile anche con passi lunghi e zone
leggere), a pochi hertz: la temperatura cambia in minuti, non in frame.

**Gli ingressi ci sono quasi tutti.** La temperatura dell'aria alla quota
dell'entità e il vento: dal meteo (`UGeoWeatherSubsystem`). Il sole (direzione
e luce) e la notte: dal cielo (`UGeoSkySubsystem`). Le nuvole (che cambiano la
temperatura apparente del cielo: un cielo sereno di notte "è" a −40 °C, uno
coperto è quasi all'aria): dal meteo. Il moto: dallo stato canonico. Lo stato
del motore: dal protocollo (DIS: il bit "motore acceso"; CIGI: i componenti) o
dalla velocità.

**Gli effetti che il modello deve dare, e che i test controlleranno:**

- di giorno al sole una vernice scura è più calda di una chiara, e un mezzo
  fermo è più caldo dell'aria;
- di notte con il cielo sereno un mezzo spento diventa **più freddo dell'aria**
  (irraggia verso il cielo freddo): il classico "contrasto invertito" dell'IR;
- un motore acceso si scalda in minuti e si raffredda in decine di minuti;
- il vento e la velocità raffreddano (convezione forzata);
- un pennacchio di missile è a centinaia di gradi; un incendio di più.

**Le proprietà** vengono da un descrittore per modello (le zone, quali parti e
materiali ne fanno parte, i numeri). Senza descrittore, valori di default per
dominio (un'auto, un carro, un aereo, una nave). OpenFlight ha già, per ogni
faccia, un **codice di materiale IR** e un **codice di superficie** (SMC): il
lettore li conserva (M1), e una tabella li traduce in zone e materiali.

**L'uscita** è, per ogni zona, una temperatura (K) e un'emissività: vanno al
materiale delle parti (Custom Primitive Data) e a un'interfaccia per i
sensori. Il sensore (dopo) convertirà la temperatura in radianza nella banda
(Planck integrato su 3–5 µm o 8–12 µm), aggiungerà l'atmosfera (la
visibilità del meteo), il guadagno automatico e il rumore. Anche il **fondo**
(terreno, acqua, edifici, alberi) avrà la sua temperatura: le classi di
copertura del suolo ci sono già (ESA WorldCover), e sarà un capitolo dei
sensori.

---

## 10. Altro da mettere in scaletta

Cose che non sono state chieste ma che un IG con entità vere si troverà a
dover avere. In ordine di quanto si faranno sentire:

1. **Visibilità da lontano.** Un aereo a 20 km è più piccolo di un pixel e
   sparisce; un IG lo disegna almeno come un punto (come i lampioni e le
   stelle: una dimensione minima in pixel), e per le grandi distanze usa
   versioni semplificate (i LOD del modello, poi impostori).
2. **Le luci delle entità**: luci di navigazione (rossa a sinistra, verde a
   destra, bianca in coda), anticollisione lampeggianti, fari di atterraggio,
   fari delle auto di notte. OpenFlight ha i **punti luce** nel modello: il
   lettore li conserva già (M1). Con il cielo di notte e i lampioni, senza
   luci le entità di notte sono invisibili.
3. **Assetto sul terreno e sull'acqua.** Un'auto appoggiata segue la quota ma
   non la pendenza; con tre o quattro punti di contatto segue anche il pendio.
   Una nave sulle onde (lo spettro delle onde c'è già): beccheggio, rollio e
   sussulto dai punti dello scafo; e la **scia**.
4. **Effetti**: fumo dello scarico, polvere sugli sterrati e sotto i rotori, la
   scia di condensazione degli aerei in quota (il criterio di Appleman: con la
   temperatura del meteo si sa dove si formano), vampa e scia dei missili,
   esplosioni, crateri (il terreno ha già un aggancio per modificare le quote).
5. **Interrogazioni dell'IG**: quota sotto un punto (HAT/HOT), linea di vista,
   intersezioni — servono a CIGI, alle collisioni e ai sensori.
6. **Il meteo sulle entità**: bagnate sotto la pioggia, la neve sulle
   superfici orizzontali, il ghiaccio (i parametri del meteo ci sono già).
7. **Registrazione e riproduzione** del flusso delle entità: per rivedere un
   esercizio, per riprodurre un problema.
8. **Selezione e ispezione**: clic su un'entità, un riquadro con lo stato, le
   parti, le temperature, il danno.
9. **Suoni**: motori con l'effetto Doppler (il tuono ha già la sua sintesi).
10. **Sensori** (la fase dopo): IR (questa sezione 9), intensificatori di luce
    (NVG: il cielo di notte, la luna, le stelle, i lampioni ci sono già), TV,
    radar (la sezione equivalente radar, dalla forma e dai materiali).

---

## 11. La scaletta

Ogni pezzo si verifica prima del successivo; l'ordine si può cambiare.

| | Pezzo | Si verifica con |
|---|---|---|
| **M1** | **Lettura dei modelli**: dati del modello, OpenFlight (gerarchia, DOF, switch, LOD, sequenze, riferimenti esterni, istanze, palette di texture/materiali/colori, mesh, codici IR), OBJ+MTL, texture SGI; il rig; le regole `file` nella tabella dei modelli con `scale`/`yaw`/`up`/`forward`/`offset`; in Unreal la cartella esterna, il caricamento su un thread, mesh e texture condivise, le parti con i DOF, gli switch e i LOD; `geo.Entities.ModelFolder`, `geo.Entities.ModelInfo`, `geo.Entities.Dof`, `geo.Entities.Switch`; uno strumento da riga di comando (`geomodel_info`) per guardare dentro i propri `.flt` senza Unreal | test (file costruiti byte per byte dalla specifica: un cubo, unità in piedi, un DOF, switch, LOD, riferimento esterno, mesh a strisce), `geomodel_info` sui modelli veri, e in Unreal i modelli del database |
| **M2** | **glTF 2.0** (testo e binario, materiali PBR, gerarchia, animazioni) e **conversione** dei formati chiusi: `geoworld convert-models` con `osgconv` (`.ive`, `.osgb`...) e FBX2glTF/Blender (`.fbx`, `.3ds`, `.dae`...) | test su glTF costruiti a mano; conversione di un `.ive` e di un `.fbx` veri |
| **M3** | **Articolazioni e animazione**: lo stato del modello per entità, la tabella delle articolazioni (DIS, CIGI, nomi), curve, regole procedurali (rotori, ruote, eliche), animazioni di glTF, sequenze di OpenFlight, cavi a catenaria, punti di attacco e carichi che si staccano | test (interpolazioni, limiti, catenaria) e in Unreal un carro con la torretta, un elicottero con il rotore |
| **M4** | **Termica**: zone, descrittore, solutore implicito, accoppiamento con meteo, cielo e moto; temperatura per parte verso il materiale; codici IR di OpenFlight | test fisici (stato stazionario, costanti di tempo, sole e vernice, notte serena, vento) e una vista "falsi colori" in Unreal |
| **M5** | **Collisioni e interrogazioni**: forme per parte, griglia, test esatti, terreno, eventi; HAT/HOT, linea di vista; collisione "solo interrogazioni" in Unreal | test geometrici; due entità del generatore che si scontrano |
| **M6** | **Danno**: livelli, salute per parte, switch/modello/materiale bruciato, fumo e fuoco, detriti, calore | test; il generatore che "colpisce" un'entità |
| **M7** | **Luci e visibilità** (luci di navigazione e punti luce di OpenFlight, dimensione minima in pixel), **assetto** su terreno e onde, **effetti** (scarico, polvere, scie, contrail) | in Unreal, di notte e di giorno |

Poi CIGI e DIS (la Fase 7 continua: le loro articolazioni, i comandi di
componente, le richieste di collisione e di quota usano M3, M5, M6), e i
sensori, che useranno M4.

---

## 12. Il primo pezzo (M1): cosa c'è, e cosa si è deciso scrivendolo

### 12.1 Dove sta

| | |
|---|---|
| `GeoEntities/Public/Models/ModelData.h` | il modello fedele al file: `FAffine`, gli assi dei formati, materiali, texture, mesh, nodi (gruppo, oggetto, DOF, LOD, switch, sequenza, riferimento esterno), punti luce; `AddPolygon` (normale di Newell, ventaglio) |
| `.../Models/ModelFiles.h` | i percorsi (`\` e `/`, maiuscole) e **dove si cerca una texture** (`ResolveReferencedFile`) |
| `.../Models/SgiImage.h` | le immagini SGI (`.rgb`, `.rgba`, `.int`, `.inta`, `.sgi`, `.bw`) di OpenFlight, grezze e RLE |
| `.../Models/ObjReader.h` | OBJ + MTL |
| `.../Models/OpenFlightReader.h` | OpenFlight (15.x e 16.x): record, palette, gerarchia, DOF, LOD, switch, sequenze, istanze, riferimenti esterni, mesh, punti luce |
| `.../Models/ModelRig.h` | **il rig**: assi dell'attore, metri, parti, condizioni di visibilità, il movimento dei DOF |
| `.../Models/ModelLoader.h` | quale lettore per quale estensione, e cosa dire per i formati da convertire |
| `.../Entities/EntityModels.h` | le regole `file` della tabella dei modelli, con `scale`, `yaw`, `offset`, `up`, `forward` |
| `GeoEntities/Private/ModelLibrary/GeoModelLibrary.{h,cpp}` | la libreria: un thread di lavoro per leggere, il game thread per mesh, texture e materiali |
| `GeoEntities/Public/GeoEntitySubsystem.h` | gli attori con le parti, `geo.Entities.Dof`, `geo.Entities.Switch`, LOD dalla camera |
| `GeoWorldEditor/Private/GeoTerrainMaterialFactory.cpp` | `M_GeoEntity`, `M_GeoEntityMasked`, `M_GeoEntityTranslucent` (`geo.Imagery.CreateMaterial`) |
| `Tools/StandaloneTests/geomodels_main.cpp` | 48 test, con un OpenFlight scritto byte per byte |
| `Tools/StandaloneTests/geomodel_info.cpp` | `geomodel_info <modello> [cartella]`: dentro un modello senza Unreal |

Lo strato puro (`Models/`) è header-only e senza Unreal; i controlli statici
lo sanno. La cartella privata si chiama `ModelLibrary`, non `Models`: un nome
uguale a una cartella pura farebbe pensare ai controlli che sia pura.

### 12.2 Decisioni prese scrivendo

**Un punto solo di conversione.** I lettori riempiono `FModel` nelle
convenzioni del formato e in metri (le unità di OpenFlight si convertono
subito, sono un fatto del file); `BuildRig` porta tutto negli assi
dell'attore, gira i triangoli, gira le V delle UV. Il test di ogni formato
guarda **nell'attore**: l'alto del file è +Z, l'avanti è +X, i triangoli
guardano fuori (calcolato con le coordinate, non con le normali, che
potrebbero sbagliare nello stesso modo).

**Le correzioni della regola si applicano dopo gli assi**: prima il formato
diventa attore, poi `scale`, `yaw` (positiva a destra, come la bussola: è il
verso degli utenti, non quello di OpenFlight), `offset`. `up` e `forward`
sostituiscono gli assi del formato quando il modello non li rispetta.

**Le parti nascono solo dove servono** (sezione 5). Il carro dei test, fatto di
gruppi, oggetti, matrici, una torretta, uno switch del danno, un LOD e una
sequenza, diventa 8 parti: la radice e solo i nodi che si muovono o si
spengono. Le parti
vuote (un DOF senza geometria sotto e senza figli) si tolgono.

**Il DOF è `Prima · Moto · Dopo`**, con Prima = (trasformazioni sopra) ·
Frame e Dopo = Frame⁻¹ · (trasformazioni sopra)⁻¹: il moto avviene negli assi
del DOF (origine, asse x, piano xy del record), ma la parte resta nel
sistema della madre. Il moto è T · Rz(imbardata) · Rx(beccheggio) ·
Ry(rollio) · S, con i limiti del file (`ClampDof`). I valori sono **quelli di
OpenFlight**: imbardata positiva a **sinistra**. Il test lo fissa con una
canna che, a +90, va a sinistra.

**Le texture nominate con il percorso di un'altra macchina** sono la regola,
non l'eccezione (`D:\Progetti\db\tex\carro.rgb`). L'ordine: com'è
scritto; relativo al modello; il nome accanto al modello; nelle cartelle
`textures`, `Textures`, `tex`; nella cartella sopra; **per nome** in tutta la
cartella dei modelli (un indice fatto una volta, sul thread di lavoro, al
primo modello). Le maiuscole non contano. Quelle che mancano si contano e si
dicono; la faccia resta del suo colore.

**Le SGI con il decodificatore nostro**, le altre (PNG, JPEG, BMP, TGA,
TIFF...) con l'`ImageWrapper` di Unreal, tutte in BGRA, con le
mipmap fatte da noi (`Tiles::BuildMipChain`, come l'ortofoto); oltre 4096
pixel di lato si parte dalla prima mip che ci sta. Le texture si ripetono
(wrap) in U e V: i modelli le usano così.

**Tre materiali, non uno**: opaco (a una faccia), **tagliato** (a due facce,
con la maschera dall'alfa: alberi, reti, cavi), **trasparente** (vetri). La
scelta è per materiale del file (`MaterialMode`): trasparente se il
materiale lo dice, tagliato se è a due facce o la texture ha l'alfa, opaco
altrimenti. Il colore è il colore del vertice (OpenFlight lo mette sulla
faccia) per la texture per una tinta; i colori dei vertici sono sRGB, si
elevano al quadrato nel materiale per tornare lineari. Se i materiali non ci
sono, il materiale di base del motore e una frase rossa che dice cosa
lanciare.

**Il lavoro diviso in due**: sul thread di lavoro (UE::Tasks) il file, i
riferimenti esterni, il rig, le texture decodificate e le loro mip; sul game
thread **un modello per frame**: le `UTexture2D`, le istanze dei materiali, una
`UStaticMesh` per parte con `BuildFromMeshDescriptions` e `bFastBuild` (come
gli alberi, funziona anche nel gioco impacchettato). Il primo F-16 non deve
fermare il frame per leggere un file da 20 MB.

**Un modello si legge una volta**, con chiave = file + testo della regola:
lo stesso file con due correzioni sono due rig. Mesh, texture e materiali
sono condivisi fra tutte le entità con quella regola; ogni entità ha il suo
**stato del rig** (valori dei DOF, maschere degli switch).

**Mentre il modello arriva**, l'entità ha le scatole del suo dominio; quando
è pronto l'attore si rifà, al quarto di secondo dopo. Un modello che non si
legge lascia le scatole in **magenta** e il nome dice "(modello non letto)";
`geo.Entities.Models` dice perché.

**L'attore**: la radice (la posizione e l'assetto della Fase 7), sotto un
componente "Modello" scalato di 100 (il rig è in metri, Unreal in
centimetri), sotto le parti in metri. Senza collisioni (M5).

**I LOD** si scelgono con la distanza della camera **dal centro del LOD**
(nel sistema del modello), quattro volte al secondo, insieme alla scelta
degli attori. Gli **switch** partono dalla maschera del file. Le
**sequenze** mostrano il fotogramma 0: farle girare è M3.

**OpenFlight, i dettagli che contano** (tutti con un test): i bit dei flag si
contano dal più significativo; i record di continuazione si attaccano al
precedente; il record di matrice è a vettore riga (la traslazione nell'ultima
riga); la mesh è la faccia spostata di 4 byte; le istanze si clonano dove
sono usate; i riferimenti esterni si leggono ricorsivamente (fino a 8 livelli)
e si attaccano sotto il loro nodo; un record sconosciuto seguito da un Push
diventa un gruppo, così i figli non si perdono; le facce **nascoste** e i
**punti luce** non sono geometria (i punti luce restano da parte per M7); il
codice IR di faccia e materiale resta da parte per M4.

**FBX e IVE non si leggono, si dice come convertirli.** FBX è chiuso (l'SDK di
Autodesk non è ridistribuibile in un plugin); IVE è il formato binario
interno di OpenSceneGraph, cambia con la versione. Il messaggio dice il
comando: `osgconv modello.ive modello.flt`, o FBX2glTF / Blender verso glTF
(M2) o OBJ. Nell'editor, un FBX si importa come asset e la regola usa
`asset`.

**Gli strumenti**: `geo.Entities.ModelInfo` in Unreal e `geomodel_info` da riga
di comando dicono le stesse cose (formato, misure, parti, DOF con i limiti,
switch, texture trovate e no, avvisi): il primo controllo da fare su un
modello nuovo, prima di metterlo in una regola.

### 12.3 Cosa non è provato

Tutto il codice Unreal (non si compila qui) e i `.flt` **veri**: i test usano
file scritti byte per byte dalla specifica 16.4. Un database reale avrà
record e varianti che la specifica descrive male o che i modellatori usano a
modo loro: `geomodel_info` è fatto per trovarli.

### 12.4 Il prossimo pezzo

M2: glTF 2.0 (`.gltf` e `.glb`, gerarchia, materiali PBR, animazioni) e
`geoworld convert-models`, che converte una cartella di `.ive`/`.osgb`/`.fbx`
con `osgconv` e FBX2glTF se ci sono.

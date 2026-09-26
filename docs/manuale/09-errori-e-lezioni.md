# Capitolo 9 — Gli errori fatti, e cosa insegnano

> Un progetto si impara meglio dai suoi errori che dai suoi successi. Questo
> capitolo li raccoglie tutti, raggruppati per **tipo**, perché lo stesso tipo di
> errore si ripresenta sotto forme diverse.

---

## Perché un capitolo sugli errori

Durante lo sviluppo sono arrivati sulla tua macchina parecchi errori — di
compilazione, a runtime, di dati. Letti uno per uno sembrano incidenti. Letti
insieme, mostrano **quattro schemi** che si ripetono. Riconoscerli è la cosa più
utile che puoi portarti via da questo progetto, perché li incontrerai in qualunque
progetto futuro.

---

## Schema 1 — La convenzione data per scontata

**Tutti i bug lunghi di questo progetto sono nati qui.** Non sono errori di
programmazione: sono due parti del sistema che intendono la stessa cosa in modo
diverso.

| Dove | La convenzione confusa | Cosa sarebbe successo |
|---|---|---|
| Fase 1 | ENU (destrorso) contro Unreal (sinistrorso) | mondo allo specchio |
| Fase 2 | quota sul livello del mare contro quota sull'ellissoide | tutta l'Italia 48 m sotto terra |
| Fase 2 | pixel in gradi contro pixel in metri | livello 24, 517 TB |
| Fase 4 | FOV orizzontale di Unreal contro FOV verticale della formula | LOD sbagliato di quasi il doppio |
| Fase 6 | post sui nodi contro pixel sulle aree | strisce che sfarfallano ai bordi |
| Fase 6 | un VRT elenca i file una volta per **banda**, non per file | "scartati −4 file" |
| Fase 7 (design) | NED contro NEU, angoli DIS contro angoli CIGI | aerei che salgono mentre atterrano |

**La difesa:**

* **Ogni conversione di convenzione avviene in un punto solo.** Il numero 100 sta
  in `GeoUnits.h`; la riflessione NEU sta in `MakeNeuBasis()`; l'ondulazione del
  geoide si somma in un passo esplicito.
* **Le convenzioni si scrivono**: nel manifest (`"yAxis": "sud"`), nei commenti,
  nei nomi (`HeightM`, `LatRad`).
* **Si rendono impossibili da confondere**, quando si può: `FGeodetic` non ha un
  costruttore con tre numeri, perché latitudine e longitudine si scambiano
  facilmente.

---

## Schema 2 — L'errore silenzioso

Il tipo più pericoloso: **niente si rompe**, il risultato è sbagliato, e sembra
giusto.

| Errore | Perché era silenzioso |
|---|---|
| PROJ "ballpark" | nessuna eccezione: restituisce le quote invariate |
| `gdalbuildvrt` con proiezioni diverse | un warning in mezzo ad altri, e prosegue |
| `FMatrix::ToQuat()` su una riflessione | restituisce un quaternione, sbagliato |
| il tick di `FTickableGameObject` senza mondo | compila, parte, e non ticka |
| il materiale in un `TWeakObjectPtr` | funziona finché il garbage collector non passa |
| `EQueuedWorkPriority::Count` | è un valore dell'enum, il compilatore lo accetta |
| i contatori dell'overlay che misuravano l'intenzione | "3.538.944 triangoli", e schermo vuoto |
| `geo.Terrain.FlipWinding 1` che rispondeva "ON" | non aveva cambiato niente |

**La difesa:**

* **Rendere rumoroso ciò che è silenzioso.** `allow_ballpark=False` fa fallire PROJ
  invece di tacere. Il VRT viene contato. I comandi dicono quando non hanno
  cambiato niente.
* **Misurare il risultato, non l'intenzione.** I triangoli si contano nel
  renderer, non nella lista di ciò che si credeva di aver costruito.
* **Verificare contro qualcosa di indipendente.** L'orizzonte contro √(2Rh), i
  quadrati MGRS contro il bucket vero.

---

## Schema 3 — Il nome ricordato invece che controllato

Quando si scrive una seconda implementazione guardando la prima, **si ricorda il
concetto, non il nome esatto**. Il compilatore di Unreal se ne accorge, ma solo
sulla tua macchina, un errore alla volta.

| Scritto | Vero |
|---|---|
| `Component->SetWireframe(...)` | `bExplicitShowWireframe` + `MarkRenderStateDirty()` |
| `FAutoConsoleCommandWithArgs` | non esiste: `FAutoConsoleCommandWithWorldAndArgs` |
| `Cache.SetBudget(...)` | `Cache.SetBudgetBytes(...)` |
| `resolution["x_metres"]` | `resolution["finestMetres"]` |
| `Public/Unreal/...` in tre moduli | nomi di cartella ambigui fra moduli |

**La difesa:** per ogni errore di questo tipo arrivato sulla tua macchina, un
controllo statico che lo intercetta in locale (capitolo 8.3). Oggi i controlli
sono sette, e ognuno è stato provato reintroducendo l'errore da cui è nato.

La lezione più generale: **se non puoi eseguire un pezzo di codice, controlla
almeno i nomi che usa.** È il minimo, ed è sorprendentemente efficace.

---

## Schema 4 — La regola di Unreal che il C++ non ha

Errori che in C++ normale non esisterebbero, e che nascono da come Unreal compila
e organizza il codice.

| Errore | La regola di Unreal |
|---|---|
| `unresolved external symbol GeodeticToEcef` | ogni modulo è una **DLL**: senza `_API` il codice non è visibile agli altri |
| `C2084: PackKey already has a body` | la **build unity** unisce più `.cpp`: i namespace anonimi si fondono |
| parametro `bEnabled` che nasconde il campo `bEnabled` | lo **shadowing** in Unreal è un errore, non un avviso |
| `cannot open source file` su file appena aggiunti | bisogna **rigenerare i file di progetto** |

**La difesa:** conoscere le regole (capitolo 1), e il principio che le aggira quasi
tutte: **lo strato puro è header-only e non conosce Unreal.** Nessuna macro di
esportazione, nessuna dipendenza, e si compila con qualunque compilatore.

---

## Errori di processo

Non tutti gli errori stanno nel codice. Alcuni stanno nel **modo di lavorare**, e
valgono la pena di essere detti.

**Documentazione che mancava.** Alla Fase 3 non c'erano né il documento di
design né quello di verifica, e hai dovuto chiederli. Da lì in avanti ogni fase ne
ha due.

**Promesse del design non mantenute nel codice.** Due casi, detti apertamente:

* il design della Fase 6 dice che `FGeoLoaderPool` è "usato da entrambi" gli
  streaming: **lo usa solo quello delle ortofoto**;
* il design della Fase 6 prevede `geo.Imagery.ShowLevels`: **non è stato
  implementato**.

**Verifiche invalide.** Due volte un controllo è stato "verificato" in modo che
non poteva fallire: `CheckUnityCollisions.py`, provato reintroducendo **una sola**
delle due funzioni in conflitto; e il conteggio dei file del VRT, provato con un
VRT **a una sola banda**, dove il bug non poteva manifestarsi. Un test che non può
fallire non verifica niente.

**Numeri scritti a memoria.** Anche scrivendo questo manuale: la chiave della tile
del Colosseo, la firma di un'interfaccia, l'albero dei componenti di un attore
erano stati scritti a memoria e sbagliati. Sono stati corretti controllando il
codice. Ogni numero di questo manuale è stato ricontrollato allo stesso modo.

---

## La lezione in una riga

> **Un sistema che non puoi eseguire lo devi rendere il più possibile
> verificabile senza eseguirlo — e ogni volta che scopri di aver sbagliato,
> aggiungi il controllo che te lo avrebbe detto.**

È il modo in cui è stato costruito tutto questo progetto, ed è il motivo per cui
lo strato puro, i test standalone e i controlli statici esistono.

---

## Riepilogo

* **Quattro schemi** si ripetono: convenzione data per scontata, errore
  silenzioso, nome ricordato invece che controllato, regola di Unreal che il C++
  non ha.
* Contro le **convenzioni**: conversione in un punto solo, convenzioni scritte,
  tipi che impediscono di sbagliare.
* Contro il **silenzio**: rendere rumoroso, misurare il risultato, confrontare con
  qualcosa di indipendente.
* Contro i **nomi**: controlli statici, provati reintroducendo l'errore vero.
* Contro le **regole di Unreal**: conoscerle, e tenere lo strato puro fuori dal
  motore.
* Un test che **non può fallire** non verifica niente.

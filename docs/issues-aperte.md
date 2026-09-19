# Issue aperte

## #1 — Jitter residuo sui cubi di verifica (Fase 1) — APERTA

**Sintomo.** Gli spigoli dei cubi tremolano orizzontalmente quando la camera si
muove. Su tutti i cubi, non solo su quelli lontani dall'origine.

**Stato.** Non risolta. Un bug reale e' stato trovato e corretto
(`UGeoreferenceSubsystem` ereditava a mano da `UWorldSubsystem` +
`FTickableGameObject` senza sovrascrivere `GetTickableGameObjectWorld()`, quindi
il Tick non era legato a nessun mondo e il rebasing non partiva mai — commit
`fb32fe4`), **ma il jitter persiste anche dopo il fix**. Quindi o la causa era
un'altra, o erano due cause sovrapposte.

**Cosa NON e' stato ancora escluso.** Nessuno ha ancora riportato l'output di
`geo.Diag`, che distingue le tre cause possibili:

1. il Tick non gira (→ contatore `Tick eseguiti` a 0);
2. il rebase non scatta pur girando il tick (→ distanza dall'origine oltre soglia);
3. le coordinate sono piccole e corrette, e a tremolare e' l'**antialiasing
   temporale** TSR/TAA, che su uno spigolo netto senza texture contro il cielo
   vuoto produce esattamente questo sintomo (→ `r.AntiAliasingMethod 0` per
   confermare in cinque secondi).

**Prossimo passo.** Servono l'output di `geo.Diag` e l'informazione se si sta
guardando in PIE o nel viewport dell'editor: i due percorsi per leggere e
spostare la camera sono diversi e vanno verificati separatamente.

**Impatto sulla Fase 2.** Nessuno. La pipeline dati e' offline e non dipende dal
rendering.

**Impatto sulla Fase 5 (aggiornato il 2026-09-19).** La Fase 5 e' stata scritta
senza aspettare questa issue: bloccarla su una segnalazione senza dati non
avrebbe prodotto informazione, e se la causa e' TSR riguarda qualunque
geometria, non la mesh del terreno. Resta pero' vero che **finche' non e' chiusa
non si possono valutare le finiture visive**: un bordo che balla puo' essere
antialiasing o precisione, e i due casi si correggono in posti opposti. La mesh
della Fase 5 fornisce per contro un test migliore dei cubi — una superficie
testurizzata e continua e' molto meno soggetta al falso positivo dell'aliasing
temporale di uno spigolo netto contro il cielo.


## #2 — `build` "non funziona ancora bene" — CHIUSA il 2026-09-19

Erano due cause distinte, entrambe corrette:

1. la risoluzione del sorgente letta nelle unita' del CRS invece che in metri,
   che su un sorgente geografico dava livello 24 e un raster da 518 TB
   (commit `b869e0b`);
2. il passo di campionamento della griglia geoidica non allineato al passo
   nativo di EGM2008, che su dati TINITALY reali faceva superare la soglia
   dell'errore di interpolazione e fermava lo stadio 2.

Testo originale della segnalazione, per storia:

### #2 (storico) — `build` "non funziona ancora bene"

Segnalato senza dettagli. Per riprenderla servono: il comando esatto, l'output
completo dello stadio 1 (bbox, formato, **risoluzione sul terreno**, livello
consigliato) e l'output di `python run.py check-env`.

Gia' viste e corrette, da escludere per prime:

* risoluzione letta nelle unita' del CRS invece che in metri, che dava livello
  24 e un raster da 518 TB (corretto, commit `b869e0b`);
* `_work/` di un tentativo fallito che lascia uno stato incoerente: va
  **cancellata prima di rilanciare** dopo un errore;
* griglia geoidica assente, che fa fallire lo stadio 2.

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
rendering. Il jitter va risolto prima della Fase 5 (mesh), dove diventerebbe
impossibile distinguere un problema di precisione da un problema di generazione
della geometria.

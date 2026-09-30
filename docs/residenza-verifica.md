# Residenza — come verificarla in Unreal

> Il perché di tutto questo è in `docs/residenza-design.md`. Qui trovi solo
> cosa fare, cosa dovresti vedere, e cosa mandarmi se qualcosa non torna.
>
> Serve lo stesso materiale della Fase 5 (un dataset di quote) e, per l'ultima
> parte, quello della Fase 6 (un dataset di ortofoto).

---

## 0. Prima di cominciare

1. Compila il progetto (Visual Studio, *Development Editor*). I file cambiati
   sono elencati alla fine del documento di design.
2. Apri il livello di sempre e avvia il terreno come nella Fase 5:

   ```
   geo.Terrain.Demo <cartella del dataset di quote>
   geo.Terrain.Wireframe 0
   geo.Lod.Debug 1
   ```

3. Se hai le ortofoto:

   ```
   geo.Imagery.Demo <cartella quote> <cartella ortofoto>
   ```

Per volare davvero (servono velocità e previsione) entra nel **Play** e usa
`geo.Fly`. Anche la camera dell'editor va bene: la velocità si stima dalle
posizioni, qualunque cosa muova la camera.

---

## 1. Leggere gli overlay

**Overlay del LOD** (`geo.Lod.Debug 1`), le righe nuove:

```
Modo          : vista-indipendente (il frustum lo applica Unreal)
Moto          : 0 m/s   previsione 12 s   teletrasporti 1
Piano (RAM)   : adesso 440 (100%)  previste 0 (100%)  sicurezza 990 (100%)
Precarico     : +0 richieste   piano 0.80 ms
```

- **Moto**: la velocità stimata. Da fermo deve scendere a 0 in un paio di
  secondi. I teletrasporti contano i salti riconosciuti (il `Demo` stesso ne
  fa uno).
- **Piano (RAM)**: quante tile vuole ogni fascia e che percentuale ha già le
  quote in memoria. "adesso" deve arrivare al 100% in pochi secondi.
- **piano … ms**: quanto costa rifare il piano (ogni 0,25 s). Atteso: circa
  1 ms, comunque sotto i 3.

**Overlay del terreno** (acceso dal `Demo`), le righe nuove:

```
Mesh          : 612 a schermo + 85 nascoste = 697 / 2000   triangoli nel renderer ...
Pronte (mesh) : adesso 440/440 (100%)   previste 0/0 (100%)
Questo frame  : +0 a schermo  +0 in anticipo  -0 (sfrattate 0)
```

- **Mesh**: quante sono a schermo, quante costruite e nascoste, e il budget.
  La riga è verde se i triangoli nel renderer coincidono con quelli costruiti.
- **Pronte (mesh)**: quante tile di ogni fascia hanno già la mesh.
- **Questo frame**: cosa è successo in questo frame. Da fermo deve essere tutto
  zero.
- **RISCALDAMENTO** (arancione) compare solo dopo un salto.

---

## 2. Prova A — Girare la camera non costa niente

È la prova che conta di più, perché è il difetto da cui è partito tutto.

1. Fermo, a 2–3 km di quota, aspetta che "Pronte (mesh) adesso" sia al 100%.
2. Gira la camera **velocemente**, a destra e a sinistra, anche di 180 gradi.

**Atteso:**
- nessun bordo nero, nessun vuoto ai lati;
- "Questo frame" resta a **+0 +0 -0** mentre giri;
- "Tile disegnate" nell'overlay del LOD **non cambia** girando.

**Controprova:**

```
geo.Lod.ViewIndependent 0
```

Rifai la stessa rotazione: ora "Questo frame" si muove a ogni rotazione e, se
giri abbastanza in fretta, il bordo che entra resta vuoto per un attimo. Torna
al default:

```
geo.Lod.ViewIndependent 1
```

---

## 3. Prova B — Volare: le tile arrivano prima di te

1. Nel Play: `geo.Fly`, poi vola dritto a quota costante, qualche centinaio di
   m/s (la velocità della camera di volo cresce con la quota).
2. Guarda "Moto": deve mostrare una velocità stabile, vicina a quella vera.

**Atteso:**
- "Piano (RAM)" **previste** vicino al 100%: le quote di dove stai andando sono
  già in memoria;
- "Pronte (mesh)" **previste** alto (sopra l'80% dopo qualche secondo di volo
  dritto);
- "Questo frame" mostra soprattutto **+N in anticipo** (costruite nascoste) e
  pochi o nessun **+N a schermo** (costruite perché mancavano);
- a occhio: il dettaglio davanti a te è già fine quando ci arrivi, senza tile
  che cambiano livello sotto il naso.

**Esperimento:** `geo.Lod.Lookahead 0` spegne la previsione. Rifai lo stesso
volo: "+N a schermo" deve crescere, perché ora le tile si costruiscono solo
quando servono. Poi `geo.Lod.Lookahead 12`.

---

## 4. Prova C — Il salto e il riscaldamento

```
geo.Goto Milano
```

**Atteso:**
- nell'Output Log: `[GeoLod] Teletrasporto riconosciuto: richieste in coda annullate, piano da rifare.`;
- "teletrasporti" sale di uno;
- compare la riga arancione **RISCALDAMENTO**;
- per un attimo il frame rate scende (è voluto), poi la riga sparisce e
  "Pronte (mesh) adesso" è sopra il 95%.

Cronometra a occhio quanto dura. Poi confronta con il riscaldamento spento:

```
geo.Terrain.Warmup 0
geo.Goto Roma
```

Ora il frame resta fluido ma il terreno si riempie a pezzi, per più tempo. Torna
al default con `geo.Terrain.Warmup 24`. Dimmi quale dei due preferisci: il
valore 24 è una stima, e si può cambiare.

---

## 5. Prova D — Tornare indietro

1. Da fermo su un punto, aspetta il 100%.
2. Vola via per qualche chilometro, poi torna indietro sullo stesso punto.

**Atteso:** tornando, "+N a schermo" resta basso e le mesh ricompaiono senza
essere ricostruite: erano nascoste, non buttate. Lo conferma "nascoste" nella
riga **Mesh**, che durante il volo cresce.

---

## 6. Prova E — Il budget e lo sfratto

```
geo.Terrain.MeshBudget 700
```

Vola un po' in giro. **Atteso:** "nascoste" non supera più di tanto il budget
meno quelle a schermo, e in "Questo frame" compaiono **sfrattate N**. Il terreno
a schermo resta completo: si buttano solo le nascoste. Torna al default con
`geo.Terrain.MeshBudget 2000`.

---

## 7. Prova F — Le ortofoto seguono

Con le ortofoto accese (`geo.Imagery.Demo …`, `geo.Imagery.Debug 1`):

- ripeti la **Prova B**: le tile che compaiono davanti a te devono avere già la
  loro foto, non una versione sfocata che poi si affina;
- ripeti la **Prova C**: dopo il salto le texture si creano più in fretta (32
  per frame in riscaldamento).

---

## 8. Numeri da mandarmi

Sono quelli che non posso misurare da qui, e decidono i default.

1. **Memoria per mesh.** Da fermo, con il 100% pronto: apri il Task Manager (o
   `stat memory` nella console), annota la memoria dell'editor; poi
   `geo.Terrain.MeshBudget 3000` e `geo.Lod.Safety 0.25` (anello più largo, più
   mesh), vola un po', annota di nuovo e dimmi anche il numero di mesh
   dell'overlay. Dalla differenza si ricava il costo di una mesh.
2. **Costo di rimostrare.** Durante la **Prova B**, `stat unit`: il valore di
   *Game* con "+0 a schermo" e poche tile che cambiano. Se vedi picchi quando
   molte tile nascoste diventano visibili insieme, dimmelo.
3. **Tempo del piano** dall'overlay del LOD, e "Tempo selezione".
4. **Durata del riscaldamento** dopo `geo.Goto Milano`, con 24 e con 0.
5. `geo.Lod.Stats` e `geo.Terrain.Stats` in un momento qualunque del volo.

---

## 9. Se qualcosa non va

| Sintomo | Prima cosa da guardare |
|---|---|
| Il terreno non compare più dopo l'aggiornamento | `geo.Terrain.Stats`: se "Pronte adesso" resta a 0, mandami anche `geo.Lod.Stats` |
| z-fighting (due livelli sovrapposti che sfarfallano) | una mesh nascosta non si è nascosta: `geo.Terrain.Diag` dice "visibile" o "nascosto" per le prime tile |
| Buchi per qualche frame quando cambia il livello | non dovrebbero più esserci (sezione 7 del design): dimmi dove e a che quota |
| "RISCALDAMENTO" non se ne va mai | manda l'overlay del terreno: vuol dire che la fascia "adesso" non arriva al 95% |
| "teletrasporti" sale mentre voli normalmente | la soglia è sbagliata per il tuo caso: dimmi quota e velocità |
| Scatti periodici in volo | `stat unit` e la riga "Questo frame": se coincidono con "+N a schermo" alto, la previsione non basta; se con "+N in anticipo", il budget di 4 per frame è troppo per la tua macchina (`geo.Terrain.Budget 2`) |

---

## Riepilogo

- Prova A (girare) è la verifica principale: **zero lavoro in rotazione**.
- Prova B (volare) mostra la previsione: tile costruite **in anticipo**.
- Prova C (saltare) mostra il riscaldamento: un attimo lento, poi tutto pronto.
- Prove D ed E mostrano che si tiene quello che si è visto, entro un budget.
- I numeri della sezione 8 servono a tarare i default sulla macchina vera.

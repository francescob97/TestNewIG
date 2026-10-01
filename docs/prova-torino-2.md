# Seconda prova: "lagga da paura, vedo una miriade di poligoni"

> Hai riprovato dopo le correzioni di `prova-torino.md`: lag tale da non
> potersi muovere, una miriade di poligoni a schermo, RAM alle stelle. La
> domanda era: *siamo sicuri che stia disegnando mesh con i LOD?* Qui c'è la
> risposta, con i numeri, e cosa è cambiato.

---

## 1. La risposta breve

**Il LOD funziona**: le tile vicine sono fini, quelle lontane grossolane, e i
test lo verificano. **Ma i valori di default che avevo scelto chiedevano
troppo**, e non me n'ero accorto perché non avevo mai contato i triangoli
totali. Li ho contati adesso, con il selettore vero, sopra Torino:

| Configurazione | Tile tenute | Triangoli in memoria | Triangoli in vista |
|---|---:|---:|---:|
| **Prima** (soglia 4 px, mesh piena, uguale in ogni direzione) | 520 | **17,6 milioni** | **5,7 milioni** |
| **Adesso** (soglia 8 px, mesh a passo 2, dietro ×4) | 290 | **2,5 milioni** | **1,5 milioni** |

Sette volte meno in memoria, quattro volte meno a schermo. Più le mesh nascoste
tenute pronte dalla residenza: prima fino a 700 da 33.792 triangoli, cioè altri
**23 milioni** di triangoli in RAM. Lì andava la memoria.

E la "miriade di poligoni" che vedevi era quasi certamente il **wireframe**:
`geo.Terrain.Demo` lo accendeva di default, e su milioni di triangoli è una
ragnatela fittissima e una passata di disegno in più. Adesso parte spento.

---

## 2. Perché erano così tanti

Tre moltiplicatori, ognuno ragionevole da solo:

1. **La soglia di 4 pixel.** È la manopola più potente che c'è: dimezzarla
   quadruplica le tile. 4 px è un valore da workstation; su un portatile 8 è
   già un ottimo dettaglio.
2. **La selezione uguale in ogni direzione** (la residenza): tutto quello che
   sta dietro di te era dettagliato quanto quello che hai davanti. Circa **3
   volte** le tile, per cose che nessuno guarda.
3. **Tile da 33.792 triangoli.** Una tile grande e fine sovra-dettaglia la sua
   parte lontana: il LOD la sceglie guardando il suo punto più vicino.

Più un quarto: la **stima di memoria** nell'overlay contava 12 byte a
triangolo (solo le posizioni). Il conto vero per una mesh di Unreal
(`FDynamicMesh3`, con vertici in double, topologia degli spigoli, normali e UV)
è circa **125 byte**: dieci volte tanto. L'overlay mostrava un numero che non
spiegava niente.

---

## 3. Cosa è cambiato

### 3.1 Soglia 8 pixel

`MaxScreenSpaceError` da 4 a 8. Si cambia a caldo con `geo.Lod.Error`.

### 3.2 Mesh a passo 2: un post ogni due

La mesh di una tile ora usa un post ogni due: 65×65 invece di 129×129, **8.704
triangoli invece di 33.792**. Il LOD lo sa: l'errore geometrico raddoppia
(`GeometricErrorScale`), quindi sceglie tile più piccole dove serve. A parità
di dettaglio sullo schermo servono meno triangoli, perché il dettaglio si
distribuisce più fine.

I test verificano che ogni vertice della mesh rada sia un post vero della tile
(non un'interpolazione), che le giunzioni fra tile vicine combacino ancora al
millimetro, e che le UV coprano sempre [0,1], cioè che le ortofoto si
drappeggino uguale. `geo.Terrain.MeshStep 1|2|4` per cambiarlo.

### 3.3 Dietro di te, meno dettaglio

Fuori dalla vista si tollera un errore **4 volte** più grande: tile circa 4
volte più grossolane per lato, una sedicesima dei triangoli. Questo è un
**compromesso** rispetto all'idea della residenza, e va detto chiaramente:

- **dietro c'è sempre terreno** (il test lo verifica: tutto ciò che prima era
  coperto lo è ancora, solo più grossolano). Girandoti non vedi nero;
- quello che entra nella vista si **affina** in una frazione di secondo, perché
  le mesh si costruiscono sui thread di lavoro e il padre grossolano resta a
  schermo finché i figli non sono pronti;
- cioè: girare la testa torna a costare un po' di lavoro, ma niente buchi.

Sulla macchina da 64 GB puoi tornare a "tutto uguale in ogni direzione" con
`geo.Lod.OutOfView 1`, oppure con il profilo workstation (sotto).

Il bordo dello schermo resta dettagliato: "fuori vista" si giudica con il
frustum allargato del 20%. Nella vista il dettaglio è **identico** a prima (c'è
un test che lo controlla tile per tile).

### 3.4 Wireframe spento, e un numero che diventa rosso

- `geo.Terrain.Demo` non accende più il wireframe.
- L'overlay del terreno ha una riga nuova:
  ```
  Triangoli     : 2.47 milioni in memoria, ~294 MB di RAM (stima)
  ```
  **Gialla sopra i 3 milioni, rossa sopra i 5.** È il numero che mancava: se
  diventa rosso, il terreno sta chiedendo troppo, e lo vedi subito invece di
  scoprirlo dal lag.

### 3.5 Un comando invece di sei

```
geo.Quality portatile       soglia 8, dietro x4, passo 2, 600 mesh, 4 thread
geo.Quality workstation     soglia 4, dietro x1, passo 2, 3000 mesh, 8 thread
```

I default all'avvio sono già quelli del portatile (tranne il budget di mesh,
che segue la RAM).

---

## 4. Cosa fare adesso

1. Compila e lancia come al solito (`geo.Terrain.Demo` o `geo.Imagery.Demo`).
2. Accendi gli overlay: `geo.Lod.Debug 1` (quello del terreno parte da solo).
3. **Prima di muoverti**, aspetta che "Pronte (mesh) adesso" arrivi al 100% e
   leggi la riga **Triangoli**. Atteso: **2-3 milioni**.
4. Muoviti, e prova a girare la camera.

**Se lagga ancora**, mandami queste righe (bastano copiate a mano o una foto
dello schermo):

- dall'overlay del terreno: **Mesh**, **Triangoli**, **Consegna**;
- dall'overlay del LOD: **Tile disegnate**, **Tempo selezione**, **piano … ms**;
- da `stat unit`: i quattro numeri **Game, Draw, GPU, RHIT**;
- la risoluzione dello schermo del portatile.

L'ultima conta: la soglia è in pixel **veri**. Uno schermo da 2560×1600 ha il
48% di righe in più di uno da 1080, e a parità di soglia chiede il 56% di
triangoli in più (misurato, tabella sotto). Se il tuo è così,
`geo.Lod.Error 12` ti riporta ai numeri di questo documento.

Quanto contano le manopole, **misurato** con il selettore vero sopra Torino a
3 km (default: 2,52 milioni in memoria, 1,46 in vista, schermo da 1080 righe):

| Comando | In memoria | In vista | Nota |
|---|---:|---:|---|
| (default) | 2,52 M | 1,46 M | 290 tile |
| `geo.Lod.Error 12` | 1,78 M | 0,99 M | |
| `geo.Lod.Error 16` | 1,35 M | 0,68 M | circa metà |
| `geo.Terrain.MeshStep 4` | 1,54 M | 0,97 M | ma 667 tile: più oggetti da gestire |
| `geo.Lod.OutOfView 1` | 4,53 M | 1,46 M | il doppio in memoria, uguale in vista |
| `geo.Lod.Error 4` | 5,81 M | 3,66 M | da workstation |

E la risoluzione dello schermo, con i default: a **1.600 righe** sono **3,93 M**
in memoria e 2,36 M in vista; con `geo.Lod.Error 12` si torna esattamente ai
2,52 M della prima riga.

---

## 5. La lezione

Ho scelto i default guardando il **dettaglio** (4 px è un valore bello da
vedere) e mai il **costo totale**. Il conto dei triangoli era a un ciclo for di
distanza, e l'ho fatto solo dopo che il portatile si è piantato. Ora c'è nel
codice (la riga Triangoli dell'overlay, colorata) e nei test (la sezione 10 dei
test del quadtree misura quanto pesano le manopole). La regola generale, che
vale per qualunque motore: **ogni scelta di qualità va accompagnata dal suo
costo in numeri, e il numero va mostrato dove lo vede chi prova.**

---

## 6. Dove sta nel codice

| File | Cosa è cambiato |
|---|---|
| `GeoRender/Public/Mesh/TileMesh.h` | `FTileMeshParameters::Step`: un post ogni N |
| `GeoRender/Public/Quadtree/QuadtreeTypes.h` | `OutOfViewErrorFactor`, `GeometricErrorScale` |
| `GeoRender/Public/Quadtree/TileSelector.h` | soglia più alta fuori vista, errore scalato dal passo |
| `GeoRender/.../Lod/GeoQuadtreeSubsystem.*` | soglia 8, fattore 4, scala dal terreno, overlay |
| `GeoRender/.../Terrain/GeoTerrainSubsystem.*` | passo 2, `SetMeshStep`, stima di memoria vera, riga Triangoli |
| `GeoRender/Private/GeoRenderModule.cpp` | wireframe spento nel demo, `geo.Lod.OutOfView`, `geo.Terrain.MeshStep`, `geo.Quality` |
| `Tools/StandaloneTests/geomesh_main.cpp` | sezione 8: passo della mesh (6 test) |
| `Tools/StandaloneTests/geoquadtree_main.cpp` | sezione 10: passo e dettaglio fuori vista (5 test) |

---

## Riepilogo

- Il LOD funzionava; i **default** chiedevano 18 milioni di triangoli. Ora 2,5.
- La "miriade di poligoni" era il **wireframe** acceso dal demo: ora è spento.
- Tre leve: **soglia 8 px**, **mesh a passo 2**, **dietro ×4**. Dietro c'è
  sempre terreno, solo più grossolano.
- L'overlay mostra i **triangoli in memoria** e diventa rosso sopra i 5 milioni.
- `geo.Quality portatile | workstation` per cambiare tutto insieme.
- Se lagga ancora: mandami le righe della sezione 4.

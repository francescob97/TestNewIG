# Fase 5 — Come verificare che funzioni

## A. Senza Unreal — 1 secondo

```bat
cmake -S Plugins/GeoWorld/Tools/StandaloneTests -B build
cmake --build build
build\Debug\geomesh_tests.exe
```

Attesi **28 test verdi**, in sette gruppi:

| Gruppo | Cosa dimostra |
|---|---|
| Conteggi | 16641 post interni + 516 di gonna = 17157 vertici; 32768 triangoli di superficie + 1024 di gonna = 33792; nessun indice fuori array; spegnendo la gonna restano esattamente i 32768 |
| Frame locale | i vertici restano dentro la tile; **ULP del float 0.0039 m al livello 8, 0.0001 m al livello 14** (contro i 64 cm che si avrebbero in coordinate mondo); il raggio dichiarato contiene davvero i vertici |
| Normali e UV | tutte unitarie a 3e-08; su terreno piatto puntano in alto; le UV coprono esattamente [0,1] agli angoli |
| Gonne | 152.6 m al livello 13; ogni vertice scende **esattamente** della profondita' (scarto 0.0000 m); nessuno scostamento laterale; raddoppia per livello come l'errore geometrico |
| **Giunzioni** | due tile adiacenti costruite in **frame locali diversi**, riportate in ECEF, hanno i 129 vertici di bordo coincidenti a **0.0000 m** (est-ovest) e **0.0001 m** (nord-sud) |
| Orientamento | tutti e 32768 i triangoli di superficie orientati nello stesso verso; `bFlipWinding` inverte davvero |
| Costo | 0.91 MB per tile, 182 MB con 200 tile a schermo |

Il gruppo delle giunzioni e' quello che conta piu' di tutti: dimostra che le
crepe **non** possono nascere fra tile dello stesso livello, quindi che le gonne
servono solo fra livelli diversi.

Il totale dei test C++ eseguibili senza motore sale a **125**:

```
geocore_tests       37
geotiles_tests      23
geoquadtree_tests   37
geomesh_tests       28
```

---

## B. In Unreal — la parte che si vede

Prerequisito: un dataset generato dalla Fase 2.

```
geo.Terrain.Demo D:\geoworld\italia
```

Apre il dataset, ti porta in quota, accende selezione LOD, costruzione della
geometria e statistiche.

### Cosa devi vedere

Terreno. Non rettangoli colorati: rilievo vero, con le valli al posto giusto.

```
--- GeoWorld | Fase 5: terreno ---
Provider      : DynamicMesh
Tile disegnate: 96   triangoli 3244032
In attesa     : 12   (budget 4 per frame)
Questo frame  : +4  -2
Costruzione   : 1.90 ms per tile
Gonne         : on   rebase gestiti: 3
```

Due righe cambiano colore da sole quando c'e' qualcosa da guardare: `In attesa`
diventa gialla se il budget non sta al passo con quanto in fretta ti muovi, e
`Costruzione` diventa gialla sopra gli 8 ms per tile.

### Se non vedi niente

```
geo.Terrain.Diag
```

Prima di provare rimedi a caso, guarda i numeri. `Diag` stampa cio' che il
**renderer** ha davvero in mano, non cio' che il subsystem crede di aver
costruito — sono due cose diverse, e la differenza e' tutta l'informazione:

| Cosa leggi | Cosa significa |
|---|---|
| `triangoli 0` su una tile | la mesh non e' arrivata al componente: problema nel provider |
| `raggio 0.0 km` | bounds degeneri, il renderer scarta la primitiva prima di disegnarla |
| `distanza` enorme (migliaia di km) | la geometria e' altrove: problema di trasformazione |
| `davanti` negativo | le tile selezionate stanno **dietro** la camera: problema nel frustum della selezione, non nella mesh |
| `NON REGISTRATO` / `NASCOSTO` | il componente non e' nella scena |
| tutto verde e sensato | la geometria c'e' ed e' al posto giusto: resta l'orientamento delle facce |

In quest'ultimo caso:

```
geo.Terrain.FlipWinding 0
```

**Nota: il default e' gia' `1`.** Dare `geo.Terrain.FlipWinding 1` non cambia
niente — il valore da provare e' `0`. I comandi a interruttore adesso lo dicono
("era GIA' ON, nessun cambiamento") invece di rispondere "ON" come se avessero
fatto qualcosa.

La convenzione di faccia frontale non e' mai stata verificata dentro il motore
(vedi `fase5-design.md`, sezione 5). Se dopo il flip si vede, il default va
cambiato in `FTileMeshParameters::bFlipWinding` — dimmelo e lo cambio.

### Le quattro prove

**1. Il rilievo e' vero.** Vola sull'Appennino o sulle Alpi. Le creste devono
stare dove stanno sulla mappa, e le quote devono essere plausibili. Se il
terreno e' speculare (valli e creste scambiate, geografia a specchio), il
sospetto va al frame: e' il sintomo della riflessione descritta nella sezione 2
del design.

**2. Il LOD adesso si vede.** Scendi di quota lentamente con
`geo.Terrain.Wireframe 1`. I triangoli devono **infittirsi** man mano, e il
reticolo suddividersi in quattro. Risalendo, riunirsi. E' la stessa prova della
Fase 4, ma ora sulla geometria vera.

**3. LA prova della fase — vedere le crepe.**

```
geo.Terrain.Skirt 0
```

Mettiti a quota media, dove convivono due o tre livelli, e guarda quasi di
taglio. Devono comparire **fessure sottili di cielo** lungo le linee dove un
livello incontra il successivo — e **solo** li'. Lungo le giunzioni fra tile
dello stesso livello non deve comparire niente, mai.

```
geo.Terrain.Skirt 1
```

Le fessure spariscono.

Questa e' la verifica piu' importante, perche' prova due cose insieme: che le
gonne servono, e che servono **esattamente dove la teoria dice**. Se vedi crepe
anche fra tile dello stesso livello, allora il problema non sono le gonne — e'
il frame locale o l'overlap del formato, e va cercato li'.

**4. Il rebase non ricostruisce niente.**

```
geo.Terrain.Stats
geo.Rebase
geo.Terrain.Stats
```

`Tile disegnate` deve restare **identico**, `Questo frame` deve restare
`+0 -0`, e `rebase gestiti` deve salire di uno. Il rebase cambia solo le
trasformazioni dei componenti: e' l'incasso del lavoro della Fase 1. Se invece
dopo un rebase le tile si ricostruiscono, la geometria non e' nel frame locale
come dovrebbe.

**5. Il budget fa quello che dice.**

```
geo.Terrain.Budget 1
```

Ora una tassellatura nuova si riempie visibilmente una tile alla volta. Con
`geo.Terrain.Budget 64` si riempie subito, ma su uno scatto. Il default 4 e' il
compromesso.

### Comandi

```
geo.Terrain.Demo <cartella>    apre, si posiziona, accende tutto
geo.Terrain.Enable <0|1>       costruzione della geometria
geo.Terrain.Wireframe <0|1>    reticolo dei triangoli
geo.Terrain.Skirt <0|1>        gonne ai bordi
geo.Terrain.FlipWinding <0|1>  orientamento delle facce
geo.Terrain.Budget <N>         tile costruite per frame
geo.Terrain.Stats              statistiche dell'ultimo frame
geo.Terrain.Diag               cosa il renderer ha davvero, e dove
```

---

## C. Cosa NON e' verificato

* **Errori di compilazione gia' incontrati e corretti su Windows** (se tornano,
  li riconosci): `'SetWireframe': is not a member of 'UDynamicMeshComponent'` —
  il wireframe non ha un setter sul componente, vive come proprieta' pubblica
  `bExplicitShowWireframe` su `UBaseDynamicMeshComponent`, e va seguito da
  `MarkRenderStateDirty()` perche' il proxy di scena non rilegge i flag da solo.
* **Il C++ non e' mai stato compilato dentro Unreal.** L'ambiente e' Linux senza
  motore. La matematica della mesh e' verificata numericamente, le convenzioni
  UE staticamente (`CheckSourceDiscipline.sh`, `CheckShadowedParameters.py`,
  `CheckModuleExports.py`). L'orientamento delle facce e' per definizione
  verificabile solo a video: per questo e' un parametro a caldo.
* **La mesh si costruisce sul game thread**, di proposito e con un budget per
  frame. `BuildTileMesh` e' puro e senza stato, quindi spostarlo sul pool della
  Fase 3 e' una modifica localizzata; non e' stato fatto perche' non c'e' ancora
  una misura che lo giustifichi. `Tempo build` nell'overlay sara' quel numero.
* **Le normali ai bordi della tile usano differenze unilaterali**, perche' non si
  guarda la tile vicina. La geometria resta continua; l'illuminazione puo'
  mostrare una cucitura sottile lungo i bordi. Si risolve nello shader, non
  nella mesh.
* **`UDynamicMeshComponent` non e' la soluzione finale.** Fa una conversione e un
  ricalcolo interno per tile. E' isolato dietro `IGeoTerrainMeshProvider`
  proprio per poter essere sostituito da un `FPrimitiveSceneProxy` custom senza
  toccare nient'altro.
* **Nessun materiale.** Il terreno esce con il materiale di default di
  `UDynamicMeshComponent`. Le UV ci sono e sono giuste: l'ortofoto e' la Fase 6.
* **Resta aperta la issue #1 sul jitter** (`docs/issues-aperte.md`). Se i bordi
  ballano muovendo la camera, e' quella e non questa fase: la geometria e' ferma
  nel proprio frame locale.

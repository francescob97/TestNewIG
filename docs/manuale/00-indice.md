# GeoWorld — Manuale del progetto

> Un documento per capire **tutto** quello che è stato costruito: cosa fa ogni
> pezzo, perché è fatto così, quali alternative c'erano, e dove sta nel codice.
> È scritto anche per imparare Unreal Engine, partendo da zero.

---

## Come leggere questo manuale

I capitoli sono pensati per essere letti **in ordine**, perché ognuno usa i
concetti del precedente. Se hai poco tempo, il minimo indispensabile è:
questo indice, il capitolo 1 (Unreal) e il capitolo 2 (geodesia). Tutto il
resto poggia su quei due.

Ogni capitolo ha la stessa struttura:

* **L'idea in breve** — cosa fa quel pezzo, in poche righe.
* **Il ragionamento** — il perché delle scelte, con esempi.
* **Alternative considerate** — cosa si poteva fare invece, e perché no.
* **Nota Unreal** — i concetti del motore che quel pezzo usa. Sono il tuo
  corso di Unreal, distribuito dove serve.
* **Dove sta nel codice** — i file, con una riga su cosa contengono.
* **Riepilogo** — da rileggere quando torni sull'argomento.

Due convenzioni tipografiche:

> 💡 **Esempio** — un caso concreto, con numeri veri.

> ⚠️ **Trappola** — un errore facile da fare, spesso già fatto in questo
> progetto.

---

## I capitoli

| # | File | Argomento |
|---:|---|---|
| 0 | `00-indice.md` | Questo: quadro generale e mappa |
| 1 | `01-unreal.md` | **Unreal Engine per chi viene da fuori** |
| 2 | `02-geodesia.md` | Fase 1 — La Terra, le coordinate, il rebasing |
| 3 | `03-pipeline-dati.md` | Fase 2 — Dai file GIS alle tile |
| 4 | `04-streaming.md` | Fase 3 — Caricare senza bloccare |
| 5 | `05-quadtree-lod.md` | Fase 4 — Decidere cosa disegnare |
| 6 | `06-mesh-terreno.md` | Fase 5 — Dalle quote ai triangoli |
| 7 | `07-ortofoto.md` | Fase 6 — Vestire il terreno |
| 8 | `08-strumenti-verifica.md` | Test e controlli automatici |
| 9 | `09-errori-e-lezioni.md` | Gli errori fatti, e cosa insegnano |
| 10 | `10-glossario.md` | Glossario e mappa completa dei file |

La Fase 7 (entità, CIGI, DIS, HLA) è solo progettata: la trovi in
`docs/fase7-design.md`.

---

## Il progetto in una pagina

**Cosa fa.** GeoWorld è un motore di terreno per Unreal Engine 5 che disegna
la Terra vera — con le sue quote e le sue immagini — nella posizione giusta,
alla scala giusta, a qualunque distanza, senza usare plugin di terzi come
Cesium e senza il sistema Landscape di Unreal.

**Perché è difficile.** Tre ragioni, e ognuna ha generato una fase intera.

1. **La Terra è enorme e i computer sono imprecisi.** Un numero `float` a 6.400
   km dal centro della Terra ha una precisione di **64 centimetri**: gli oggetti
   tremolano. Bisogna tenere le coordinate piccole senza perdere la posizione
   vera. → *Capitolo 2, il rebasing.*
2. **I dati sono troppi.** L'Italia a 10 metri di risoluzione sono circa
   400.000 tile e 35 GB. Non si possono caricare tutti: bisogna caricare solo
   quello che si vede, e farlo senza bloccare il gioco. → *Capitoli 3, 4, 5.*
3. **I dati arrivano in formati scomodi.** Proiezioni diverse, quote misurate
   dal livello del mare invece che dall'ellissoide, file giganti. → *Capitolo 3,
   la pipeline.*

### Il viaggio di un dato, dall'inizio alla fine

Segui un singolo punto del terreno — diciamo la cima del Colosseo — da quando
è un pixel in un file scaricato a quando è un vertice sullo schermo.

```
 ┌──────────────────────────────────────────────────────────────────────┐
 │  OFFLINE (Python, una volta sola)                                    │
 │                                                                      │
 │  GeoTIFF TINITALY           quota ~21 m sul livello del mare,        │
 │  (UTM 32N, 10 m)            coordinate in metri UTM                  │
 │        │                                                             │
 │        │  riproiezione in gradi WGS84                                │
 │        │  + ondulazione del geoide (circa +48 m a Roma)              │
 │        ▼                                                             │
 │  tile 129x129 float32       quota ~69 m sull'ELLISSOIDE              │
 │  <root>/14/17521/4379.ght   piramide di livelli, dal 0 al 14         │
 └──────────────────────────────────────────────────────────────────────┘
                                   │
 ┌─────────────────────────────────┼────────────────────────────────────┐
 │  RUNTIME (C++ in Unreal, ogni frame)                                 │
 │                                 ▼                                    │
 │  QUADTREE  "da qui la tile 14/17521/4379 è abbastanza vicina?"       │
 │     │      → sì: chiedila                                            │
 │     ▼                                                                │
 │  STREAMING  un thread in background legge il file (0,4 ms)           │
 │     │       e lo mette in cache                                      │
 │     ▼                                                                │
 │  MESH       129x129 quote → 17.157 vertici in un frame locale        │
 │     │       alla tile, in metri, con le "gonne" ai bordi             │
 │     ▼                                                                │
 │  GEOREFERENCE  frame locale → coordinate Unreal (centimetri),        │
 │     │          relative a un'origine vicina alla camera              │
 │     ▼                                                                │
 │  ORTOFOTO   texture 256x256 del livello giusto, ritagliata se serve  │
 │     │                                                                │
 │     ▼                                                                │
 │  SCHERMO                                                             │
 └──────────────────────────────────────────────────────────────────────┘
```

Tieni questo schema a portata di mano: ogni capitolo ne descrive una casella.

---

## Come è organizzato il codice

Il progetto Unreal si chiama `TestNewIG` ed è un **guscio vuoto**: tutto il
codice sta nel plugin `Plugins/GeoWorld`. Il capitolo 1 spiega perché questa è
la struttura giusta.

Il plugin è diviso in **quattro moduli**, più una pipeline Python esterna:

```
Plugins/GeoWorld/
├── Source/
│   ├── GeoCore/          geodesia, georeferenziazione, rebasing     (Fase 1)
│   ├── GeoTiles/         formato tile, dataset, streaming, cache    (Fasi 3, 6)
│   ├── GeoRender/        quadtree, LOD, mesh, terreno, ortofoto     (Fasi 4, 5, 6)
│   └── GeoWorldEditor/   strumenti solo per l'editor               (Fase 6)
└── Tools/
    ├── StandaloneTests/  test C++ che girano SENZA Unreal
    └── Check*.py / .sh   controlli statici sul sorgente

Pipeline/                 Python + GDAL: dai file GIS alle tile      (Fasi 2, 6)
docs/                     documenti di design e verifica, e questo manuale
```

I moduli dipendono l'uno dall'altro in una sola direzione:

```
GeoCore  ←──  GeoTiles  ←──  GeoRender  ←──  GeoWorldEditor
(nessuna      (conosce        (conosce         (conosce
 dipendenza)   GeoCore)        entrambi)        tutto)
```

Questa direzione unica non è un caso: significa che si può capire `GeoCore`
senza sapere niente degli altri, e che un errore in `GeoRender` non può rompere
`GeoCore`.

### I due strati dentro ogni modulo

Questa è la decisione architetturale più importante di tutto il progetto, e
ricorre in ogni capitolo. **Ogni modulo è diviso in due strati fisicamente
separati:**

| Strato | Cartelle | Cosa contiene | Si può testare senza Unreal? |
|---|---|---|---|
| **C++ puro** | `Public/Geo`, `Public/Tiles`, `Public/Quadtree`, `Public/Mesh`, `Public/Imagery` | Matematica, formati, algoritmi | **Sì**, in un secondo |
| **Ponte Unreal** | tutto il resto | Subsystem, componenti, attori, comandi | No |

Lo strato puro non include **nessun** header di Unreal. È C++ standard, che si
compila con qualunque compilatore. Questo ha permesso di verificare 169 test
della matematica in un ambiente dove Unreal non c'era affatto.

> 💡 **Esempio.** La conversione da latitudine/longitudine a coordinate
> cartesiane (`GeodeticToEcef`) sta nello strato puro. Il comando console
> `geo.Goto Roma`, che la usa per spostare la camera, sta nel ponte Unreal. Se
> la conversione è sbagliata, un test lo dice in un secondo; se il comando è
> sbagliato, lo si vede solo in Unreal.

Lo strato puro è anche **header-only** (tutto il codice sta nei `.h`, nessun
`.cpp`). Il motivo è un dettaglio di Unreal che il capitolo 1 spiega bene: ogni
modulo è una DLL, e il codice in un `.cpp` non è visibile agli altri moduli a
meno di "esportarlo".

---

## Riepilogo

* GeoWorld disegna la Terra vera in Unreal, senza plugin di terzi.
* Le difficoltà sono tre: **precisione** (coordinate enormi), **volume**
  (troppi dati), **formati** (dati scomodi).
* Il codice sta tutto in `Plugins/GeoWorld`, in quattro moduli che dipendono
  l'uno dall'altro in una sola direzione.
* Ogni modulo ha uno **strato puro** (C++ standard, testabile senza Unreal,
  header-only) e un **ponte Unreal**.
* La pipeline Python prepara i dati una volta sola; il runtime C++ li usa a
  ogni frame.

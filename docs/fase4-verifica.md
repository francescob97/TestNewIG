# Fase 4 — Come verificare che funzioni

## A. Senza Unreal — 1 secondo

```bat
cmake -S Plugins/GeoWorld/Tools/StandaloneTests -B build
cmake --build build
build\Debug\geoquadtree_tests.exe
```

Attesi **42 test verdi**, in sei gruppi:

| Gruppo | Cosa dimostra |
|---|---|
| Volume di contenimento | tutti i campioni stanno nella sfera; **i campioni dei figli stanno nella sfera del padre** (condizione perche' scartare un padre scarti l'intero sottoalbero) |
| Frustum | punti davanti/dietro/di lato; il bordo laterale coincide con il semiangolo analitico; una sfera grande che interseca viene tenuta |
| Orizzonte | distanza misurata **113.0 km** contro **112.9 km** teorici di `sqrt(2Rh)`; una vetta a 10 km resta visibile a 300 km e sparisce a 600; gli antipodi sono occlusi; **la tile di livello 0 che contiene la camera non viene scartata** (regressione) |
| Errore su schermo | l'errore geometrico raddoppia per livello; raddoppiare la distanza lo dimezza; la formula coincide con la proiezione calcolata a mano |
| Margine | con 1.0 una tile appena fuori dal bordo e' scartata, con 1.2 e' selezionata; cio' che e' davanti resta dentro con qualunque margine; un margine assurdo non degenera i piani |
| Selezione | soglia enorme → solo radici; soglia severa da vicino → livello 14; salendo di quota il livello non aumenta mai; guardando a nord non si sceglie nulla a sud; **niente buchi** con i figli non caricati |

La progressione del LOD misurata dai test, a soglia 4 px:

```
2 km -> L14     20 km -> L12     200 km -> L8     2000 km -> L5
```

---

## B. In Unreal — la parte che si vede

Prerequisito: un dataset generato dalla Fase 2.

```
geo.Lod.Demo D:\geoworld\italia
```

Apre il dataset, ti porta a una quota da cui si inquadra tutto, accende
selezione, overlay e disegno.

### Cosa devi vedere

**1. L'overlay.**

```
--- GeoWorld | Fase 4: quadtree e LOD ---
Soglia errore : 4.0 px
Errore peggiore: 3.8 px
Tile disegnate: 96   livelli 8..12
Da caricare   : 14
Nodi visitati : 412   (frustum 180, orizzonte 96, assenti 24)
Tempo selezione: 0.184 ms
```

**2. Una tassellatura colorata.** Ogni tile selezionata ha il perimetro
disegnato, un colore per livello, con l'errore su schermo scritto sopra. Tile
grandi lontano, piccole vicino.

### Le quattro prove

**Il LOD segue la distanza.** Scendi di quota lentamente: le tile devono
**suddividersi da sole** in quattro, e il livello nell'etichetta salire. Risali:
devono riunirsi. Se non succede, `Da caricare` resta alto e il dataset non sta
arrivando.

**La soglia fa quello che dice.**
```
geo.Lod.Error 1
geo.Lod.Stats
geo.Lod.Error 16
geo.Lod.Stats
```
Dimezzare la soglia quadruplica all'incirca le tile disegnate. Da 1 px a 16 px
ci sono quattro dimezzamenti, quindi circa un fattore 256 fra i due conteggi.

**Vedere cosa il culling ha scartato.** È la prova più istruttiva:
```
geo.Lod.Freeze 1
```
poi allontanati e girati. La selezione resta quella di prima, ma ora la guardi
da fuori: devi vedere **solo la porzione che era nel frustum**, e nient'altro
dietro la curvatura. Poi `geo.Lod.Freeze 0` e tutto torna a seguirti.

**Il costo.** `Tempo selezione` deve stare sotto il millisecondo. Se sale, guarda
`Nodi visitati`: un numero enorme significa che il culling non sta scartando, e
il sospetto va prima all'orientamento della camera in ECEF.

### Il bordo nero, e il margine

Con un frustum esatto una tile viene chiesta allo streaming nel momento in cui
e' **gia'** visibile. Fra la richiesta e il disegno ci sono una lettura da
disco, una decodifica e la costruzione della mesh: diversi frame. Nel frattempo
il bordo dello schermo resta vuoto, e ruotando la camera il vuoto ti segue.

Il rimedio non e' caricare piu' in fretta, e' chiedere prima:

```
geo.Lod.Margin 1.2      (default: frustum allargato del 20% per la SELEZIONE)
geo.Lod.Margin 1.0      (frustum esatto: serve a VEDERE il problema)
```

Le tile in piu' non vengono disegnate -- il renderer di Unreal le scarta
comunque -- ma sono gia' pronte quando ci arrivi. Il costo e' qualche tile in
memoria, non un pixel a schermo.

Se anche con il margine il bordo resta vuoto, allora non e' latenza:
`geo.Lod.Freeze 1`, poi allontanati e guarda la selezione da fuori. Se si ferma
esattamente al bordo del cono visivo, il problema e' nel frustum; se invece si
estende oltre, le tile ci sono e il problema e' nella costruzione della mesh
(`geo.Terrain.Diag`).

### Comandi

```
geo.Lod.Demo <cartella>    apre, si posiziona, accende tutto
geo.Lod.Enable <0|1>       selezione per frame
geo.Lod.Error <pixel>      soglia dell'errore su schermo
geo.Lod.Freeze <0|1>       congela la vista usata dal LOD
geo.Lod.Debug <0|1>        overlay statistiche
geo.Lod.Draw <0|1>         tassellatura selezionata
geo.Lod.Stats              statistiche dell'ultima selezione
geo.Lod.Margin <fattore>   allargamento del frustum per il precaricamento
```

---

## C. Cosa NON e' verificato

* **Il C++ non e' mai stato compilato dentro Unreal.** L'ambiente e' Linux senza
  motore. La matematica e' verificata numericamente, le convenzioni UE
  staticamente.
* **Nessuna misura su dataset grandi.** I 174 nodi visitati vengono da un
  dataset finto. Sull'Italia a livello 14 il numero sara' maggiore, e
  `Nodi visitati` nell'overlay sara' il primo dato reale.
* **L'errore geometrico e' il passo fra post**, non lo scostamento verticale
  vero fra livelli (vedi `fase4-design.md`, sezione 6). Su terreno molto
  irregolare il LOD sara' piu' grossolano di quanto potrebbe.
* **Le tile selezionate non vengono pinnate nella cache.** Con un budget stretto
  possono essere sfrattate mentre sono ancora in uso, e ricaricate subito dopo.
  Il meccanismo del pin esiste gia' in Fase 3: va collegato quando la Fase 5
  comincera' a tenerne dei riferimenti.

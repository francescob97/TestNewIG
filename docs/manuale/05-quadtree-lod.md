# Capitolo 5 — Decidere cosa disegnare (Fase 4)

> A ogni frame, il motore guarda dove sta la camera e decide quali tile servono
> e a quale livello di dettaglio. È il cervello del sistema.

---

## L'idea in breve

Hai una piramide di tile: poche tile grandi e grossolane in cima, milioni di
tile piccole e dettagliate in fondo. Non puoi disegnarle tutte. Devi scegliere,
per ogni zona del mondo, **quale livello basta**:

* il terreno vicino alla camera va disegnato fine, perché lo vedi bene;
* quello lontano può essere grossolano, perché un triangolo di un chilometro a
  cento chilometri di distanza occupa pochi pixel;
* quello dietro la camera, o dietro l'orizzonte, non va disegnato affatto.

La struttura che permette di scegliere in fretta si chiama **quadtree**: un albero
dove ogni nodo ha quattro figli, che è esattamente la forma della piramide.

---

## 5.1 Il quadtree

La piramide del capitolo 3 **è già un quadtree**: la tile `(L, X, Y)` ha come
figli le quattro tile `(L+1, 2X, 2Y)`, `(L+1, 2X+1, 2Y)`, `(L+1, 2X, 2Y+1)`,
`(L+1, 2X+1, 2Y+1)`.

```
                    livello 0:  [ ovest ][ est ]
                                            │
                    livello 1:          [ ][ ]
                                        [ ][▣]
                                             │
                    livello 2:             [ ][ ]
                                           [▣][ ]
                                           ...
```

L'algoritmo parte dalle radici (le due tile del livello 0) e scende. Per ogni
nodo si chiede, in ordine:

1. **Esiste?** — se è mare, non ha figli da considerare.
2. **È nel campo visivo?** — se no, scarta lui e tutti i suoi figli.
3. **È sopra l'orizzonte?** — se no, scarta.
4. **È abbastanza dettagliato da qui?** — se sì, disegnalo; se no, scendi nei
   figli.

L'ordine va **dal controllo più economico al più caro**. "Esiste?" è una ricerca
in una tabella in memoria; il campo visivo sono cinque prodotti scalari;
l'orizzonte qualche funzione trigonometrica; il dettaglio si calcola solo per i
sopravvissuti. Scartare un nodo scarta **tutto il suo sottoalbero**: ecco perché
il quadtree è veloce.

> 💡 **Numeri.** Sul dataset dei test: **174 nodi visitati per selezionarne 40**,
> in meno di un millisecondo. Senza quadtree bisognerebbe controllare milioni di
> tile.

---

## 5.2 Il volume che contiene una tile

Per sapere se una tile è visibile senza guardarne i 16.641 punti, la si chiude
dentro un volume semplice. La scelta è una **sfera** in coordinate ECEF.

### Come si costruisce

Da **18 punti**: una griglia 3 × 3 sulla superficie della tile (angoli, metà dei
lati, centro), presa alle due quote estreme — minima e massima.

Perché non solo gli otto spigoli? Perché la Terra è curva: su una tile grande la
superficie **si gonfia verso l'esterno** rispetto agli spigoli. Una sfera costruita
solo sugli spigoli lascerebbe fuori il rigonfiamento centrale.

**E da dove vengono minima e massima?** Dall'indice del dataset (capitolo 3),
**senza leggere la tile dal disco**. È il motivo per cui quelle due quote sono
state messe nell'indice fin dalla Fase 2: senza, per decidere se una montagna è
visibile bisognerebbe prima caricarla.

### Alternative

| Volume | Pro | Contro |
|---|---|---|
| **Sfera** ✓ | test contro un piano = un prodotto scalare | un po' più larga del necessario |
| Scatola allineata agli assi | semplice | in ECEF è orientata male per quasi tutte le tile |
| Scatola orientata (OBB) | stretta | il test contro un piano costa otto volte tanto |

Con decine di migliaia di nodi per frame, la differenza di costo si sente, e su
tile quasi quadrate la scatola orientata scarterebbe poco di più.

File: `GeoRender/Public/Quadtree/Culling.h`, funzione `MakeTileBoundingVolume()`.

---

## 5.3 Il campo visivo: il frustum

Il **frustum** è la piramide tronca che la camera vede: quattro piani laterali
(sinistra, destra, sopra, sotto) più un piano vicino.

Una sfera è **fuori** se sta interamente dalla parte sbagliata di almeno un piano.

```
          sinistra        destra
              \              /
               \   ░░░░░░   /     ← il frustum
                \ ░░░░░░░░ /
                 \░░░░░░░░/
                  \░░░░░░/
                   [cam]
```

### Cinque piani, non sei

Di solito un frustum ha anche un **piano lontano**, oltre il quale non si vede
niente. Qui **non c'è**, di proposito: su scala planetaria il terreno lontano si
scarta da solo — per l'orizzonte e perché il dettaglio basta — molto prima che
un piano lontano abbia senso. Metterne uno arbitrario taglierebbe le montagne
visibili all'orizzonte.

### Il margine (aggiunto dopo)

Con un frustum **esatto**, una tile viene chiesta allo streaming nel momento in
cui è **già** visibile. Fra richiesta e disegno passano diversi frame (lettura,
decodifica, costruzione della mesh), e intanto al bordo dello schermo non c'è
niente. Girando la camera, il bordo nero ti segue.

La soluzione non è caricare più in fretta, è **chiedere prima**. Il frustum usato
per la selezione è allargato del **20%** (`FrustumMarginFactor = 1.2`): si
selezionano anche le tile appena fuori dalla vista, che sono già pronte quando
ci arrivi. Il renderer di Unreal non le disegna comunque, quindi il costo è
qualche tile in memoria, non un pixel sullo schermo.

Il margine moltiplica la **tangente** del semiangolo, non l'angolo: è la
tangente a misurare quanto è largo il frustum sullo schermo. Il semiangolo è
limitato a 85° perché un valore assurdo digitato in console non produca piani
degeneri.

> 💡 **Per vederlo.** `geo.Lod.Margin 1.0` e giri la camera: vedi il bordo vuoto.
> `geo.Lod.Margin 1.2`: sparisce.

---

## 5.4 L'orizzonte, e il bug che i test hanno trovato

La Terra è curva: da un aereo a 10 km di quota vedi fino a circa 360 km, oltre
c'è la curvatura. Le tile oltre l'orizzonte non vanno disegnate.

### La prima versione era sbagliata

La prima idea: prendere i 18 punti del volume della tile, e scartarla se sono
**tutti** oltre l'orizzonte. Sembra prudente. **Non lo è.**

I punti sono punti: una tile grande può avere zone visibili che non cadono su
nessuno di essi. Il caso trovato dai test: una tile del livello 0 copre **mezzo
pianeta**, e i suoi 18 punti stanno a longitudine 0°, 90° e 180°. Con la camera
sopra Roma (12° E), erano tutti oltre l'orizzonte — e **la tile che conteneva la
camera veniva scartata**. La selezione restituiva zero tile.

Il bug l'ha trovato un test dello strato puro, **prima** che il codice arrivasse
in Unreal. È esattamente il motivo per cui lo strato puro esiste.

### La versione giusta

Si ragiona sulla **sfera intera**, in angoli visti dal centro della Terra:

```
θ   angolo fra la direzione della camera e quella del centro della sfera
β   semiangolo sotteso dalla sfera
αC  quanto lontano vede la camera:        acos(R / distanza_camera)
αT  quanto lontano "si vede" la sfera

la sfera è occlusa  ⇔  θ − β > αC + αT
```

Il pianeta si approssima con la sfera **inscritta** (raggio polare, il più
piccolo). Una sfera più piccola occlude **meno**, quindi al più si tiene qualche
tile che si poteva scartare.

> ⚠️ **Il verso dell'errore.** In ogni approssimazione di culling, sbaglia
> sempre dalla parte di **tenere**. Tenere una tile di troppo costa un po' di
> lavoro. Scartarne una visibile apre un buco nel terreno.

### Verificato con la fisica

Il test cerca a che distanza sparisce un punto a livello del mare, con la camera
a 1.000 m, e la confronta con la formula classica dell'orizzonte √(2Rh):

| | |
|---|---|
| Orizzonte misurato | **113,0 km** |
| √(2Rh) teorico | 112,9 km |

E il caso che la versione sbagliata avrebbe fallito: **una vetta alta 10 km a
300 km di distanza resta visibile**, perché anche lei "vede" oltre l'orizzonte
(113 + 356 = 469 km di portata combinata). A 600 km sparisce.

---

## 5.5 L'errore su schermo: la manopola del LOD

Ecco la domanda centrale: **"questa tile è abbastanza dettagliata, vista da
qui?"**

### L'idea

Ogni livello ha un **errore geometrico**: di quanto, al massimo, la sua superficie
si discosta dal terreno vero. Al livello 14 il passo fra i post è di circa 10 m;
al 13 di 20 m; ogni livello verso l'alto raddoppia.

Ma 20 metri di errore **a che distanza**? A 500 m sono un difetto evidente; a
200 km sono meno di un pixel. Quello che conta non è l'errore in metri, è
l'errore **in pixel sullo schermo**.

### La formula

```
errore_su_schermo = errore_geometrico × altezza_schermo
                    ─────────────────────────────────────
                    distanza × 2 × tan(fov_verticale / 2)
```

È la proiezione prospettica: un oggetto grande `E` a distanza `D` occupa sullo
schermo una frazione `E / (D × 2 tan(fov/2))` dell'altezza. Si raffina finché
questo numero scende sotto una **soglia**, di default **4 pixel**.

> 💡 **Esempio.** Schermo alto 1.080 px, campo visivo verticale 60°
> (tan 30° = 0,577). Una tile di livello 13, errore 20 m, a 5 km:
> ```
> 20 × 1080 / (5000 × 2 × 0,577) = 3,7 px   → sotto 4: va bene così
> ```
> La stessa tile a 2 km: 9,4 px → troppo, si scende al livello 14.

Progressione misurata dai test, a soglia 4 px:

| Quota della camera | Livello scelto |
|---:|---:|
| 2 km | 14 |
| 20 km | 12 |
| 200 km | 8 |
| 2.000 km | 5 |

### Tre dettagli che, sbagliati, costano caro

**1. La distanza si misura dalla superficie della sfera, non dal centro.** Con il
centro, una tile enorme che contiene la camera avrebbe una distanza grande, quindi
un errore piccolo, e **non verrebbe mai raffinata** proprio quando ci stai sopra.

**2. Il campo visivo di Unreal è orizzontale.** La formula vuole quello
**verticale**. Su uno schermo 16:9 differiscono di quasi il doppio:

```cpp
// in GeoQuadtreeSubsystem.cpp
const double HalfHorizontal = FMath::DegreesToRadians(Info.HorizontalFovDegrees) * 0.5;
OutView.VerticalFovRad = 2.0 * FMath::Atan(FMath::Tan(HalfHorizontal) / Aspect);
```

**3. La selezione lavora in ECEF, non in coordinate Unreal.** Se dipendesse dalle
coordinate Unreal, un rebase cambierebbe le tile scelte a parità di vista. In ECEF
il rebase è invisibile.

### Un'approssimazione, detta onestamente

L'errore geometrico usato è il **passo fra i post**. L'errore vero sarebbe lo
scostamento massimo fra la superficie della tile e quella dei suoi figli: quasi
zero in pianura, molto più grande del passo su una cresta alpina. Calcolarlo è
compito della pipeline e richiederebbe un campo in più nel formato.

Il passo è una stima **conservativa**: in pianura raffina più del necessario, che
è il verso giusto in cui sbagliare.

---

## 5.6 La regola che evita i buchi

Supponi che una tile vada raffinata, ma i suoi quattro figli non siano ancora in
memoria. Cosa si disegna?

La risposta sbagliata è "i figli che ci sono": se ne mancano due, restano due
buchi.

**La regola:**

> Non si scende nei figli **finché non ci sono tutti e quattro**. Nel frattempo si
> disegna il padre, e si chiedono i figli allo streaming.

Risultato: il dettaglio arriva con qualche frame di ritardo, ma **non c'è mai un
buco**. Si vede il terreno affinarsi man mano che scendi, invece di vederlo
sparire e ricomparire.

### La priorità delle richieste

Non tutte le tile mancanti sono ugualmente urgenti. La priorità di una richiesta
si ricava dall'errore su schermo: le tile che si vedono peggio arrivano prima.
Guardando davanti a te, carichi prima quello che guardi, e dopo i bordi.

---

## 5.7 Attraversamento iterativo

L'albero si visita con una **pila esplicita**, non con una funzione ricorsiva.
Al livello 14 la profondità è 15, che con la ricorsione starebbe comunque in
piedi; ma la pila esplicita rende il costo prevedibile e permette di mettere un
tetto ai nodi visitati senza artifici.

---

## 5.8 Come la selezione parla con il resto

Il quadtree **non sa niente** di file, di cache, di Unreal: tutto questo è
nascosto dietro un'interfaccia, `ITileAvailability`:

```cpp
class ITileAvailability
{
    virtual bool TileExists(const FTileKey& Key) const = 0;
    virtual bool GetHeightRange(const FTileKey& Key, double& OutMin, double& OutMax) const = 0;
    virtual bool IsTileLoaded(const FTileKey& Key) const = 0;
};
```

*(in `GeoRender/Public/Quadtree/TileSelector.h`)*

In Unreal la implementa `FStreamingAvailability`, che interroga il subsystem di
streaming. Nei test la implementa un oggetto finto, che dice "esiste" o "non
esiste" come vuole il test.

È lo stesso schema che si ritrova in tutto il progetto: **la logica pura parla
con un'interfaccia, e il ponte Unreal la implementa**. È ciò che permette di
testare la selezione con 42 test in un secondo, senza un dataset e senza il motore.

---

## 5.9 Debug: vedere il ragionamento

Il quadtree non disegna terreno: disegna **la propria decisione**. Ogni tile
selezionata ha il perimetro colorato per livello, con l'errore su schermo scritto
sopra.

Lo strumento più istruttivo è il **congelamento**:

```
geo.Lod.Freeze 1
```

La selezione si ferma quella che è, e tu puoi muoverti e guardarla **da fuori**.
Vedi esattamente la porzione che era nel frustum, e niente dietro la curvatura.
È il modo più diretto di capire cosa sta facendo il culling.

---

## Dove sta nel codice

| File | Cosa contiene |
|---|---|
| `GeoRender/Public/Quadtree/QuadtreeTypes.h` | vista, tile selezionata, risultato, statistiche |
| `GeoRender/Public/Quadtree/Culling.h` | volume, frustum, orizzonte, margine |
| `GeoRender/Public/Quadtree/TileSelector.h` | errore su schermo, attraversamento, regola anti-buchi |
| `GeoRender/Public/Lod/GeoQuadtreeSubsystem.h` + `.cpp` | il subsystem: costruisce la vista dalla camera, lancia la selezione |
| `GeoRender/Private/GeoRenderModule.cpp` | i comandi `geo.Lod.*` |
| `Tools/StandaloneTests/geoquadtree_main.cpp` | 42 test senza Unreal |
| `docs/fase4-design.md`, `docs/fase4-verifica.md` | i documenti originali |

### Comandi

```
geo.Lod.Demo <cartella>    apre, si posiziona, accende tutto
geo.Lod.Enable <0|1>       selezione a ogni frame
geo.Lod.Error <pixel>      la soglia dell'errore su schermo
geo.Lod.Margin <fattore>   allargamento del frustum per il precaricamento
geo.Lod.Freeze <0|1>       congela la vista: guarda la selezione da fuori
geo.Lod.Debug <0|1>        overlay
geo.Lod.Draw <0|1>         la tassellatura, un colore per livello
geo.Lod.Stats              nodi visitati, scarti, tempo
```

---

## Riepilogo

* Il **quadtree** è la piramide delle tile vista come albero; scartare un nodo
  scarta tutto il suo sottoalbero.
* Controlli **dal più economico al più caro**: esiste → frustum → orizzonte →
  errore su schermo.
* Il volume di una tile è una **sfera da 18 punti**, costruita con min e max
  dell'indice, **senza leggere la tile**.
* Frustum a **cinque piani** (niente piano lontano) e allargato del **20%** per
  chiedere le tile prima che servano.
* L'**orizzonte** si testa sulla sfera intera con la Terra inscritta. **Sbagliare
  sempre dalla parte di tenere.**
* **Errore su schermo** = errore geometrico proiettato in pixel; soglia 4 px;
  distanza dalla **superficie** del volume; FOV **verticale**; tutto in **ECEF**.
* **Niente buchi**: si scende nei figli solo quando ci sono tutti e quattro.
* La logica parla con un'**interfaccia**; il ponte Unreal la implementa, i test
  la fingono.

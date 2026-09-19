# Fase 4 — Quadtree e selezione LOD: decisioni e motivazioni

Codice in `Plugins/GeoWorld/Source/GeoRender/`.
Per la verifica vedi `docs/fase4-verifica.md`.

---

## 1. Cosa fa questa fase

A ogni frame guarda dove sta la camera, attraversa il quadtree e decide **quali
tile andrebbero disegnate e a che livello di dettaglio**. Quelle che mancano le
chiede al loader della Fase 3; quelle che ci sono finiscono in una lista che la
Fase 5 trasformera' in mesh.

Non disegna terreno: disegna solo la propria decisione, in debug. Il terreno
arriva in Fase 5.

---

## 2. Di nuovo i due strati, e di nuovo hanno ripagato

Tutta la matematica — volumi di contenimento, frustum, orizzonte, errore su
schermo, attraversamento — sta in `Public/Quadtree` senza una riga di Unreal ed
e' **header-only**, ed e' coperta da **37 test eseguibili in un secondo**.

Header-only non e' una finezza: in Unreal ogni modulo e' una DLL, e un simbolo
definito in un `.cpp` non e' visibile agli altri moduli se non esportato con la
macro API. Esportarlo metterebbe una macro del motore dentro lo strato che non
deve sapere di stare dentro Unreal.

Non e' un principio astratto: **il bug piu' grave di questa fase e' stato
trovato da quei test, non aprendo l'editor** (vedi sezione 5).

---

## 3. Volume di contenimento: una sfera, da 18 campioni

Per decidere se una tile e' visibile serve un volume che la contenga. La scelta
e' una **sfera in ECEF**.

Una scatola orientata sarebbe piu' stretta e scarterebbe qualche tile in piu',
ma il test sfera-contro-piano e' un prodotto scalare e un confronto, mentre
scatola-contro-piano ne richiede otto. Con decine di migliaia di nodi per frame
la differenza si sente, e su una tile quasi quadrata il guadagno della scatola
e' modesto.

La sfera si costruisce da **18 punti**: una griglia 3x3 sulla superficie (angoli,
meta' dei lati, centro) alle due quote estreme, che l'indice del dataset
fornisce **senza leggere la tile da disco**. Non solo dagli otto spigoli: su una
tile grande la superficie si incurva verso l'esterno rispetto agli spigoli, e
una sfera costruita solo su quelli lascerebbe fuori il rigonfiamento centrale.

Il min/max quota dall'indice e' cio' che rende possibile tutto questo prima di
chiedere la tile. E' il motivo per cui in Fase 2 non e' stato omesso.

---

## 4. Frustum: cinque piani, non sei

I piani si costruiscono dalla base della camera in ECEF. La normale del piano
sinistro e' `F·sin(h) + R·cos(h)`: il bordo sinistro punta lungo
`F·cos(h) − R·sin(h)`, e il prodotto scalare fra i due e' `sin·cos − cos·sin = 0`,
quindi la normale e' perpendicolare al bordo e punta verso l'interno. Verificato
dai test contro il semiangolo calcolato analiticamente.

**Il piano FAR non c'e'.** Su scala planetaria il terreno lontano si scarta da
solo per errore su schermo e per orizzonte, molto prima che un far plane abbia
senso; metterne uno arbitrario taglierebbe montagne visibili all'orizzonte.

---

## 5. Horizon culling: il bug che i test hanno trovato

Questo test e' cio' che evita di considerare mezzo pianeta a ogni frame: senza,
guardando l'orizzonte tutte le tile dell'emisfero passerebbero il frustum.

### La prima versione era sbagliata

Testava i 18 punti campione del volume e scartava la tile se erano occlusi
**tutti**. Sembra conservativo e non lo e': i campioni sono punti, e una tile
grande puo' contenere zone visibili che non cadono su nessuno di essi.

Il caso reale, trovato dai test: una tile di livello 0 copre mezzo pianeta e i
suoi campioni stanno a longitudine 0, 90 e 180 gradi. Con la camera sopra Roma
erano **tutti** oltre l'orizzonte, e la tile che *conteneva la camera* veniva
scartata. La selezione restituiva zero tile a qualunque quota.

### La versione corretta

Si ragiona sulla **sfera intera**, in angoli visti dal centro del pianeta:

```
theta   angolo fra la direzione della camera e quella del centro sfera
beta    semiangolo sotteso dalla sfera
alphaC  angolo di visibilita' della camera:  acos(R / distanza_camera)
alphaT  angolo di visibilita' del punto piu' lontano della sfera

occlusa  <=>  theta - beta > alphaC + alphaT
```

Il pianeta e' approssimato con la sfera **inscritta** (raggio polare), non con
l'ellissoide: una sfera piu' piccola occlude meno, quindi al piu' si tiene
qualche tile che si sarebbe potuta scartare. **L'errore va tenuto in questo
verso**: tenere una tile di troppo costa lavoro, scartarne una visibile apre un
buco nel terreno.

Se la sfera attraversa la superficie — il caso delle tile grandi, il cui volume
ingloba parte del pianeta — non si conclude niente e non si scarta.

### Verificato con la fisica, non solo con se stesso

Il test cerca empiricamente la distanza a cui un punto al livello del mare
sparisce e la confronta con `sqrt(2Rh)`:

| | |
|---|---|
| Camera a 1000 m, orizzonte misurato | **113.0 km** |
| `sqrt(2Rh)` teorico | 112.9 km |

E la verifica che conta davvero, perche' e' quella che la prima condizione da
sola sbaglierebbe: **una vetta a 10 km di quota, a 300 km, resta visibile** —
la sua portata combinata e' 113 + 356 = 469 km. A 600 km sparisce.

---

## 6. Errore su schermo: la manopola del LOD

```
sse = errore_geometrico * altezza_schermo / (distanza * 2 * tan(fov_verticale / 2))
```

E' la proiezione prospettica dell'errore geometrico. Si raffina finche' `sse`
scende sotto la soglia.

Tre dettagli che sbagliati costano caro:

1. **La distanza si misura dalla SUPERFICIE del volume, non dal centro.** Usando
   il centro, una tile enorme che contiene la camera darebbe distanza grande e
   quindi errore piccolo, e non verrebbe mai raffinata proprio quando si ha il
   naso sopra.
2. **Il FOV di Unreal e' ORIZZONTALE**, la formula vuole quello verticale. Le
   due grandezze differiscono per le proporzioni dello schermo: confonderle
   sbaglia il LOD di quasi il doppio su un 16:9.
3. **La selezione lavora in ECEF, non in spazio Unreal.** Se dipendesse dallo
   spazio Unreal, un rebase cambierebbe l'insieme di tile selezionate a parita'
   di vista.

### L'errore geometrico e' un'approssimazione, ed e' giusto saperlo

Oggi vale il **passo fra post** del livello: raffinando, il campionamento si
dimezza, quindi l'errore che si toglie e' di quell'ordine.

L'errore geometrico *vero* di una tile sarebbe lo scostamento verticale massimo
fra la sua superficie e quella dei figli: su una pianura e' quasi zero anche a
livello grossolano, su una cresta alpina e' molto piu' grande del passo.
Calcolarlo e' compito della pipeline e richiederebbe un campo in piu'
nell'header della tile — per questo il formato ha un numero di versione.

Finche' non c'e', il passo e' la stima conservativa ragionevole: **sovrastima in
pianura**, cioe' raffina un po' piu' del necessario, che e' il verso giusto in
cui sbagliare.

---

## 7. La regola che evita i buchi

```
se l'errore e' accettabile, o sono al livello massimo:
    disegna questa tile (o chiedila, se manca)
altrimenti:
    se TUTTI i figli esistenti sono gia' caricati -> scendi
    altrimenti -> chiedi i figli e INTANTO continua a disegnare il padre
```

Scendere subito significherebbe smettere di disegnare il padre senza avere
ancora niente da mettere al suo posto: il terreno sparirebbe per i frame
necessari al caricamento. Meglio sfocato che assente.

Verificato da un test che carica solo i livelli fino al 6 e pretende soglia 2 px
da 2 km di quota: la selezione restituisce comunque tile, nessuna oltre il
livello 6, e i figli mancanti risultano richiesti.

### La priorita' delle richieste

Deriva dall'errore su schermo, su scala **logaritmica**: una tile con errore
dieci volte oltre la soglia e' molto piu' urgente di una appena sopra, e gli
errori si distribuiscono su ordini di grandezza. Il meccanismo della Fase 3 era
gia' in piedi: qui si e' solo collegato chi calcola il numero.

---

## 8. Attraversamento iterativo

Stack esplicito, non ricorsione. A livello 14 la profondita' e' 15, che
ricorsivamente starebbe anche in piedi, ma lo stack esplicito rende il costo
prevedibile e permette di imporre un tetto ai nodi visitati senza artifici.

Misurato sul dataset finto dei test: **174 nodi visitati per selezionarne 40**.

L'ordine dei controlli va dal piu' economico al piu' caro, e non e' casuale:

1. **esiste?** — una ricerca nell'indice in memoria; un nodo inesistente non ha
   nemmeno figli da considerare;
2. **frustum** — cinque prodotti scalari;
3. **orizzonte** — qualche funzione trigonometrica;
4. **errore su schermo** — solo per i nodi sopravvissuti.

---

## 9. Debug

Come in Fase 3, la visualizzazione non e' un accessorio: senza la Fase 5 non
c'e' altro modo di vedere cosa decide il LOD.

* **`geo.Lod.Draw 1`** disegna il perimetro di ogni tile selezionata, un colore
  per livello, con l'errore su schermo scritto sopra. Si vede la **tassellatura**:
  tile grandi lontano, piccole vicino, e il confine fra i livelli.
* **`geo.Lod.Freeze 1`** congela la vista usata dal LOD e lascia muovere la
  camera. E' lo strumento per *vedere da fuori cosa il culling ha scartato*: la
  selezione resta quella di prima, ma ora la si guarda dall'esterno.
* **`geo.Lod.Error <px>`** cambia la soglia a caldo. Dimezzarla quadruplica
  all'incirca il numero di tile: e' la relazione da avere in mente quando si
  sceglie il valore.

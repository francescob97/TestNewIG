# Capitolo 14 — Il vento: da un numero a un campo

> Come si simula il vento in ogni punto e a ogni quota: lo strato limite, la
> rugosità, i rilievi, le raffiche. Il documento di progetto con tutti i numeri
> è `docs/vento-design.md`; come provarlo è in `docs/vento-verifica.md`.

---

## L'idea in breve

Un bollettino dice "vento 10 nodi da 240°". È **un numero solo**, misurato a
10 m da terra su un prato (la stazione meteo dell'aeroporto). Ma il vento vero
cambia da posto a posto: sul mare è più forte, in città più debole, in cima a
una collina accelera, dietro una cresta si calma e diventa rafficato, salendo
cresce e gira. E non è mai fermo: le raffiche passano.

La simulazione parte dal numero del bollettino e lo trasforma in un **campo**:
una funzione che, dato un punto, una quota e un istante, restituisce il vento
lì. Quattro passi, dal più grande al più piccolo:

1. **il tempo che fa**: il vento del bollettino, che può derivare piano;
2. **lo strato limite**: l'attrito del suolo, che dipende dalla rugosità;
3. **i rilievi**: creste, valli, pendii;
4. **la turbolenza**: le raffiche.

---

## 14.1 Lo strato limite e la rugosità

Il vento striscia sul suolo e l'attrito lo frena. Lo frena solo vicino al
suolo: sopra qualche centinaio di metri (qui 600 m) l'aria non sente più il
terreno, e il vento è quello che le pressioni gli impongono, il **vento di
gradiente**. In mezzo c'è lo **strato limite**, dove la velocità cresce con il
**logaritmo** della quota:

```
U(z) ∝ ln((z + z0) / z0)
```

`z0` è la **rugosità**: quanto il suolo è "ruvido" per il vento. Non è
l'altezza degli ostacoli, ma le è proporzionale (circa un decimo):

| Suolo | z0 |
|---|---|
| Mare | 0,2 mm |
| Prato (la stazione meteo) | 3 cm |
| Campagna, siepi | 10 cm |
| Città | 1 m |

> 💡 **Esempio.** Con "10 m/s" al bollettino, il vento di gradiente è 17 m/s.
> Scendendo di nuovo a 10 m: sul mare 12,4 m/s, in campagna 9,0, in città 6,4.
> **Lo stesso tempo, quattro venti diversi.** Per questo le onde (capitolo 13)
> usano il vento sul mare e non quello della stazione.

Il vento al suolo è anche **ruotato**: l'attrito lo fa deviare verso la bassa
pressione. Nell'emisfero nord, salendo, il vento gira in **senso orario**: di
10° sul mare, una trentina sulla terra, una quarantina in città (è la *spirale
di Ekman*). Nell'emisfero sud al contrario.

Sopra lo strato limite si possono aggiungere i **venti in quota** (quota,
velocità, direzione), come nei bollettini per i piloti: il vento va da uno
strato all'altro in linea retta.

## 14.2 I rilievi

Il terreno attorno a un punto si guarda in 25 quote, da tile grossolane e lisce
(post da ~75 m):

- **in cima a una collina il vento accelera**: l'aria che sale sopra l'ostacolo
  si stringe e va più veloce. Quanto, dipende da quanto il punto sta sopra i
  dintorni (a 500 m e 2 km): +27% in cima a una collina di 300 m larga un paio
  di chilometri;
- **dietro una cresta ripida c'è il ridosso**: il vento si stacca dalla cresta e
  dietro resta un vortice (il "rotore"): meno vento, molta più turbolenza;
- **sui pendii il vento sale e scende**: l'aria segue il terreno. Sopravvento a
  una collina sale (è la "dinamica" dei piloti di aliante), sottovento scende.

Tutti e tre si spengono salendo: a qualche centinaio di metri sopra la collina
il vento non sa più che c'è.

> ⚠️ **Trappola.** Sull'acqua il terreno sotto è piatto ma quello attorno no: un
> lago fra le montagne ha il ridosso delle montagne (vero), ma non il vento
> verticale "di pendio" (l'acqua è piana). Il codice spegne il secondo e tiene
> il primo.

## 14.3 Le raffiche: turbolenza congelata

Una raffica non è un numero che sale e scende nel tempo: è una **zona** di
aria più veloce, grande da decine a centinaia di metri, che **viaggia con il
vento**. Sul lago si vede: una macchia scura di increspature che corre sulla
superficie.

Per avere questo serve un campo **coerente nello spazio**: due punti vicini
devono sentire la stessa raffica, due lontani no. Si usa un **rumore di
gradiente** (lo stesso tipo di rumore che genera le texture "naturali"),
sommato su quattro scale con le proporzioni della turbolenza vera (la cascata
di Kolmogorov: ogni scala metà più piccola ha un po' meno energia). Poi lo si
fa **scorrere** con il vento: la raffica che ora è qui, fra due secondi è più in
là. È l'ipotesi della **turbolenza congelata** di Taylor (1938): su tempi brevi
la turbolenza non ha tempo di cambiare, viene solo trasportata.

Quanto è forte? La norma europea sul vento (Eurocodice) dice che l'intensità
della turbolenza è circa `1 / ln(z / z0)`: a 10 m il 9% del vento sul mare, il
17% sul prato, un terzo in città. La raffica di picco che le stampe mostrano è
la media più tre volte questa deviazione.

> 💡 **Esempio.** Con 10 m/s sul prato, a 10 m: deviazione 1,7 m/s, raffiche di
> picco ~15 m/s, zone di raffica grandi ~70 m. Sul mare: zone di ~145 m,
> raffiche più deboli in proporzione.

## 14.4 Niente salti, di nuovo

Il campo delle raffiche scorre con il vento. Il modo ingenuo di farlo è
"posizione − velocità × tempo". Ma la velocità può cambiare (la variabilità, o
un comando): dopo un'ora di gioco, un metro al secondo in più sposterebbe tutte
le raffiche di 3,6 km **in un colpo**. È lo stesso errore che le onde evitano
(capitolo 13, sezione 13.8), e la cura è la stessa: lo spostamento **si
somma** un pezzetto alla volta, a ogni frame, con la velocità di quel momento.

## 14.5 Un campo che tutti interrogano

Il campo è **deterministico**: dato il seme, il punto e l'istante, il vento è
sempre lo stesso. Chiunque lo chiede ottiene la stessa raffica:

- le **onde** (il vento medio sul mare) e l'**increspatura** (la raffica di
  adesso sotto la camera);
- le **stampe** a destra: vento alla quota della vista, raffiche, vento a 10 m;
- le **frecce** del debug, 9 × 9 attorno alla camera;
- chiunque chiami `GetWindNorthEastUp` (anche da Blueprint): domani gli aerei
  della Fase 7, le nuvole e gli alberi.

---

## Alternative considerate

- **Un solutore vero (CFD).** Darebbe l'incanalamento nelle valli e le scie
  giuste, ma costa secondi per chilometro quadrato e va rifatto a ogni cambio di
  vento. Le regole usate qui sono quelle delle norme di progetto e dei
  simulatori di volo: istantanee, senza dati in più.
- **La turbolenza dei simulatori di volo** (Dryden, von Kármán), filtrata nel
  tempo lungo la traiettoria di un aereo. Giusta per UN aereo, ma due punti
  vicini non vedrebbero la stessa raffica, e sul lago non si vedrebbe niente
  correre.
- **Il vento al gradiente come dato.** Più pulito per la fisica, ma nessuno lo
  conosce: il vento a 10 m è quello di ogni bollettino.

---

## Nota Unreal

### `UFUNCTION(BlueprintCallable)` su un subsystem

Un subsystem è un `UObject`: le sue funzioni marcate `UFUNCTION(BlueprintCallable)`
si chiamano da Blueprint con il nodo "Get GeoWeatherSubsystem" e poi la
funzione. Per questo `GetWindNorthEastUp` prende e restituisce un `FVector`:
è un tipo che i Blueprint conoscono, le nostre strutture C++ pure no.

### Interrogare la cache senza toccarla

La cache delle tile ricorda l'ordine d'uso (LRU) e conta i "colpi". Il vento
chiede centinaia di quote per frame: se usasse la stessa chiamata dello
streaming, terrebbe in vita tile che nessuno guarda e falserebbe le
statistiche. Per questo esiste `PeekLoadedTile`: guarda senza toccare.

### Disegnare frecce per un frame

`DrawDebugDirectionalArrow` con `bPersistentLines = false` e `LifeTime = -1`
disegna per **un solo frame**: ridisegnandole a ogni tick si muovono con il
vento senza accumularsi. Lo spessore è in unità di Unreal (centimetri), e
segue il passo della griglia: si vedono da qualunque quota.

---

## Dove sta nel codice

| File | Cosa |
|---|---|
| `GeoRender/Public/Weather/WindField.h` | il campo (puro): profilo, rotazione, rilievi, raffiche |
| `GeoRender/Public/Weather/Wind.h` | `FWind`: velocità e direzione |
| `GeoRender/.../Weather/GeoWeatherSubsystem.*` | il campo in Unreal: suolo, acqua, stampe, frecce |
| `GeoRender/Private/GeoRenderModule.cpp` | i comandi `geo.Weather.*` |
| `GeoCore/Public/Geo/GeoFormat.h` | "7,2 m/s (14 kt) da 245°" |
| `Tools/StandaloneTests/geoweather_main.cpp` | i test |

---

## Riepilogo

- Il bollettino dà **un numero** (10 m su un prato); il campo ne fa il vento
  in ogni punto, a ogni quota, a ogni istante.
- Lo **strato limite**: il vento cresce con il logaritmo della quota e gira;
  la **rugosità** decide quanto: sul mare più vento, in città meno.
- I **rilievi**: accelera in cresta, ridosso e rotori sottovento, sale e scende
  sui pendii; tutto si spegne salendo.
- Le **raffiche** sono zone che viaggiano con il vento (turbolenza congelata):
  coerenti nello spazio, con la forza e la scala della norma europea.
- Il trasporto **si somma** frame per frame: niente salti, come per le onde.
- Un **seme**, un campo, lo stesso vento per tutti.

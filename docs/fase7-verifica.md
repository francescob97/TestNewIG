# Fase 7, primo pezzo — come provare le entità

> Le decisioni sono in `docs/fase7-design.md` (la sezione 13 dice cosa c'è
> già); il racconto per chi impara in `docs/manuale/20-entita.md`.

## 0. Cosa è già provato qui, senza Unreal

**C++** (`geoentities_tests`, 97 controlli):

1. **il quaternione**: rotazione attiva, `Rotate` e la matrice d'accordo su 2.000
   rotazioni a caso, dalla matrice al quaternione e ritorno anche a 180°, la
   composizione, slerp (metà strada, la via corta col segno opposto);
2. **trappola 1**: NED destrorso e NEU sinistrorso; +5 sulla terza
   componente NED **scende** di 5 m/s;
3. **Eulero 3-2-1**: andata e ritorno su 2.000 assetti; il gimbal lock a
   beccheggio 90 (angoli diversi, stesso assetto); i versi (imbardata +90 a
   est, beccheggio + muso su, rollio + ala destra giù);
4. **trappola 2**: un aereo livellato verso nord a lat 0, lon 0 ha θ DIS = −90
   (il caso dei libri); verso nord a Roma e a Tokyo: imbardata CIGI 0 per
   tutti e due, angoli DIS diversissimi, e il muso torna al nord locale; locale
   → DIS → locale in 1.000 punti e assetti a caso (errore 4·10⁻¹⁴ rad);
5. **trappola 3**: gli assi dell'attore (det +1, anche nello spazio di Unreal
   con l'origine a Roma e l'entità a Torino, dove la verticale è inclinata di
   4,7°); le velocità angolari dell'attore (−p, −q, +r) verificate ruotando il
   corpo: `v' = ω × v` torna sui tre assi;
6. **l'estrapolazione**: fermo, FPW, RVW, all'indietro; una virata a 6°/s con
   RPB: mezzo giro a 2r verso destra, un giro intero al punto di partenza
   entro un millimetro, il muso a sud; RPW e FPB con la stessa virata vanno
   dritti; con la centripeta RVW sbaglia di v ω² t³/6 (18 cm in un secondo)
   e RVB è esatto (non la conta due volte);
7. **il rientro**: all'arrivo dell'aggiornamento il disegno non salta; l'errore
   è quello vero; a metà rientro metà errore (5 m e 2°); alla fine sulla
   verità; oltre 100 m si salta e si conta; con `Blend 0` si salta sempre;
   dopo 5 s senza notizie ci si ferma e lo si segnala;
8. **il tempo**: l'unità di DIS (1,676 µs), il bit assoluto/relativo, l'ora che
   scocca (3599 → 3602), il pacchetto in ritardo dell'ora prima, tre ore di
   passi sempre in avanti; l'orologio della simulazione con ritardi da 2 a
   40 ms (scarto giusto entro 5 ms), il riavvio;
9. **il registro**: chiave sorgente + numero, `GetTypeHash`, mezzo secondo
   nostro = mezzo secondo di simulazione, il pacchetto in disordine, la
   simulazione riavviata, chi tace, chi se ne va, le quote sul mare (+48 m),
   una sorgente che si ferma;
10. **la tabella dei modelli**: le righe sbagliate con il numero, la più
    specifica, l'ultima a parità, il ripiego di serie, `cigi N` prima del tipo,
    un asset, le forme;
11. **la memoria condivisa**: scritta e riletta (id a 64 bit, −1, nome
    tagliato), la copia coerente, la sequenza dispari, una memoria che non è
    la nostra, la traduzione (velocità Giù −3 sale, imbardata 90 a est); **due
    thread veri** per 300 ms, nessuna fotografia a metà (togliendo il
    controllo della sequenza il test ne trova migliaia di rotte);
12. **la fixture di Python**: due fotografie a 10,0 e 10,5 s; per ogni entità
    la rotta della velocità è l'imbardata, e l'estrapolazione C++ ritrova la
    posizione che Python ha calcolato mezzo secondo dopo (RVB 0,0000 m, RVW
    0,0034 m, RPW 0,125 m, FPW 1–3 cm, come dice la teoria); la virata
    coordinata degli aerei.

**Python** (`tests/test_entities.py`, 15 test): le misure del formato uguali al
C++, record avanti e indietro, il seqlock, la fixture nel repository uguale a
quella che il generatore scrive adesso; NED destrorso, geodetiche e Eulero
avanti e indietro; la scena: le velocità sono le derivate delle posizioni, le
accelerazioni delle velocità, l'imbardata è la rotta, la virata è coordinata,
p, q, r sono davvero negli assi del corpo (dalla derivata dell'assetto); la
memoria condivisa vera e il comando `demo-entities`.

Quello che **non** è provato: tutto quello che passa per Unreal (il thread, la
mappatura della memoria su Windows, gli attori, i nomi sul Canvas, il suolo, la
camera che segue). È il lavoro di questa pagina.

## 1. Compilare

1. Rigenera i file del progetto (c'è un modulo nuovo, `GeoEntities`, nel
   `.uplugin`) e compila.
2. Le API più a rischio sono nella sezione 5.

Non servono materiali nuovi: le scatole usano il cubo e il materiale di base
del motore (`/Engine/BasicShapes`).

## 2. Il generatore e l'ascolto

In un prompt, nella cartella `Pipeline`:

```bat
geoworld demo-entities
```

Atteso: "Memoria condivisa 'GeoWorldEntities': 7 entita' attorno a 45.0700,
7.6800, 10 volte al secondo". Lascialo andare.

In Unreal, con il terreno di Torino aperto (`geo.Terrain.Demo <cartella>`, o
anche senza terreno), nel Play o nell'editor:

```
geo.Entities.Listen
```

Atteso: "Entita': in ascolto", poi la riga della sorgente: `memoria condivisa
'GeoWorldEntities' [1]: ricevo - 'GeoWorldEntities': 7 entita', tempo ...,
quote sul livello del mare`. Nelle stampe a destra la riga **Entita'**: "7, 7
a schermo; ricevo".

Se dice **in attesa** con "la memoria 'GeoWorldEntities' non c'e'": il
generatore non è acceso, o gira in un'altra sessione di Windows (un altro
utente, un servizio). Se dice **ERRORE**: leggi il messaggio, di solito una
versione diversa del formato.

## 3. Cosa guardare

```
geo.Entities.List
```

Le sette entità, dalla più vicina: `AEREO-1` (blu, un aereo di scatole, a
~1.550 m), `AEREO-2` (a ~2.550 m, vira a sinistra), `ELICOTTERO` (~500 m,
muso un po' giù), `AUTO-1`…`AUTO-4` (verdi, "al suolo").

Da guardare, in ordine:

1. **Si muovono lisce**, a 60 Hz e oltre, anche se il generatore scrive a 10
   Hz: fra un aggiornamento e l'altro l'estrapolazione sbaglia di millimetri.
   Per vedere il rientro al lavoro, rallenta il generatore a un aggiornamento
   al secondo (`geoworld demo-entities --rate 1`) e metti `geo.Entities.Blend 0`:
   l'elicottero (l'unico senza accelerazione) salta di ~50 cm a ogni secondo e
   le auto girano a scatti di un grado; gli aerei restano lisci, perché la
   loro estrapolazione è esatta. Con `geo.Entities.Blend 0.4` gli scatti
   spariscono.
2. **Gli aerei sono inclinati verso l'interno della curva** (~26° e ~28°): un
   aereo che vira a destra ha l'ala destra giù. Se l'ala alta è quella
   interna, una trappola è stata presa al contrario (sezione 6).
3. **Il muso guarda dove va.** Con `geo.Entities.Debug 1` la linea verde (dove
   sarà fra un secondo) esce dal muso, tangente al cerchio.
4. **Le auto stanno sul terreno**, non sopra né sotto, anche in collina; se
   il terreno non c'è ancora, alla quota del mare (+48 m) finché la tile
   arriva.
5. **La quota degli aerei** dalle stampe: `geo.Entities.Follow AEREO-1` nel
   Play, poi la riga "Seguo": quota ~1.550 m (1.500 sul mare + ~48 di geoide).
   Se è 1.500, il geoide non è stato aggiunto (sezione 6).
6. **La camera che segue** sta dietro e sopra, e non rolla con l'aereo.
   `geo.Entities.Follow off` per riprenderla.
7. **Il rebase**: segui un aereo per qualche minuto (fa 18 km a giro): le
   entità non devono saltare quando l'origine si sposta.
8. **Ferma il generatore** (Ctrl+C): le entità spariscono subito (il
   generatore scrive una fotografia vuota prima di uscire). Uccidilo invece
   dal Task Manager: restano ferme per 5 s, poi spariscono (il timeout).
   Riaccendilo: ricompaiono da sole, senza rifare Listen.

## 4. Il carico e i modelli

```bat
geoworld demo-entities --crowd 2000
```

2.007 entità entro 20 km. Atteso: `geo.Entities.List` dice "2007 nel
registro, 400 a schermo"; il frame non deve crollare. Con
`geo.Entities.Radius 5000 100` restano le 100 più vicine entro 5 km.

La tabella dei modelli: crea un file, per esempio `Config/entita.txt`:

```
1.2.105.1     plane 18 10 5      # gli aerei da caccia piu' lunghi
1.1.105.81    box 4 1.7 1.4
1.2.105.4     box 30 30 1        # una prova: AEREO-2 diventa una tavola
```

`geo.Entities.Models Config/entita.txt`: le regole elencate, gli attori
rifatti al quarto di secondo dopo. Una riga sbagliata (`1.x box 1 1 1`) deve
dire il suo numero. `geo.Entities.Models default` torna alla tabella di
serie. Senza regole per un tipo, l'entità diventa **magenta** e il nome porta
`?` e il tipo (prova con `1.2.105.4` tolta dal file e un tipo che la tabella
di serie non ha).

## 5. Le API più a rischio (se non compila)

| Dove | Cosa | Se non c'è |
|---|---|---|
| `GeoSharedMemoryRegion.cpp` | `Windows/WindowsHWrapper.h`, `OpenFileMappingW`, `MapViewOfFile`, `VirtualQuery` | includere `Windows/AllowWindowsPlatformTypes.h`, `<windows.h>`, `Windows/HideWindowsPlatformTypes.h` |
| `GeoSharedMemorySource.cpp` | `FRunnableThread::Create`, `Kill(true)` | è l'uso standard: controllare gli include `HAL/Runnable*.h` |
| `GeoEntitySubsystem.cpp` | `UDebugDrawService::Register`, `FCanvasTextItem::bCentreX/bCentreY` | come in `GeoPositionHudSubsystem.cpp` |
| `GeoEntitySubsystem.cpp` | `TMap<FEntityKey, ...>` | `GetTypeHash(FEntityKey)` sta in `Entities/EntityState.h` (stesso namespace della chiave) |
| `GeoPositionHudSubsystem.h` | `DECLARE_MULTICAST_DELEGATE_ThreeParams` con `FGeoHudLines&` | l'alias esiste perché la virgola di `TPair<FString, FString>` spezzerebbe la macro |

## 6. Se qualcosa non torna

- **Gli aerei virano con l'ala esterna giù**, o vanno "di traverso": una
  convenzione degli angoli. Prima di toccare il codice, rifai i test
  (`geoentities_tests`): la sezione 12 confronta Python e C++. Se passano, il
  problema è a valle di `ActorAxesInUnreal` (la matrice dell'attore).
- **Gli aerei sono 48 m troppo bassi**: le quote sul mare non sono state
  alzate. `geo.Entities.List` dice se il geoide è "di ripiego" (48 m fissi,
  manca il dataset dell'acqua): è giusto in Italia, sbagliato altrove.
- **Le auto galleggiano o affondano di qualche metro**: il suolo è quello
  della tile più fine in memoria, non per forza quella disegnata; si corregge
  da solo quando arriva la tile giusta.
- **Scatti regolari ogni decimo di secondo**: `geo.Entities.Blend` a 0, o un
  orologio che riparte (`geo.Entities.List`: "ripartito N volte").
- **"N letture rifatte" che cresce in fretta**: il generatore scrive mentre
  noi copiamo, ed è normale che succeda ogni tanto; se cresce come i messaggi,
  qualcosa scrive senza rispettare la sequenza.

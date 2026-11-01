# Modelli, primo pezzo (M1) — come provare i modelli dai file

> Le decisioni sono in `docs/modelli-design.md` (la sezione 12 dice cosa c'è
> già); il racconto per chi impara in `docs/manuale/21-modelli.md`. Prima
> serve la Fase 7 funzionante (`docs/fase7-verifica.md`): le entità devono
> già muoversi come scatole.

## 0. Cosa è già provato qui, senza Unreal

**C++** (`geomodels_tests`, 48 controlli):

1. **la matematica e gli assi**: l'inversa di una trasformazione con scala; la
   conversione da OpenFlight all'attore (l'avanti +Y va in +X, l'alto +Z resta
   +Z, determinante −1: è una riflessione); gli assi scritti a mano nelle regole
   (`-z`, `Y`; `w` è un errore);
2. **SGI**: un'immagine grezza 2×2 a tre canali (letta dall'alto in basso, al
   contrario del file), una RLE a quattro canali, file rotti che non fanno
   danni;
3. **i percorsi**: `\` e `/`, `..`, la texture nominata con il percorso di
   un'altra macchina (`D:\Lavoro\Texture\mimetica.rgb`) trovata in
   `textures/` accanto al modello, e per nome in tutta la cartella;
4. **OBJ**: un cubo con due materiali, una texture, un vetro (trasparente ed
   emissivo), i colori; nell'attore i triangoli guardano fuori (calcolato con
   le coordinate) e le normali escono dal cubo; l'alto del file (+Y) è l'alto
   dell'attore, l'avanti (−Z) è il suo avanti (+X); le V girate; `yaw 90` lo
   gira a destra;
5. **OpenFlight**, un carro scritto byte per byte dalla specifica 16.4, in
   piedi: l'intestazione, i palette (colori, texture, materiali), la
   gerarchia, un **DOF** (la torretta, con i limiti), uno **switch** (intatto /
   distrutto), un **LOD** (1000 piedi = 304,8 m), una **sequenza**, una
   faccia nascosta, una trasparente, una a due facce, un codice IR, due punti
   luce, una **mesh** a strisce, un'**istanza** usata due volte, un
   **riferimento esterno** (un altro `.flt`, in metri). Nell'attore: il cubo di
   10 piedi è alto 3,048 m con la faccia di sopra a 1,524 m; i triangoli dello
   scafo guardano fuori; **imbardata +90 del DOF: la canna va a sinistra** (gli
   angoli di OpenFlight sono matematici); il beccheggio oltre il limite si
   ferma; lo switch e il LOD accendono e spengono le parti giuste;
6. **il caricatore e la tabella**: `.fbx` e `.ive` danno un errore che dice
   come convertirli; un `.flt` che non è OpenFlight è un errore, non un
   crash; le regole `file` con le correzioni (`F16/f16.flt yaw 90`,
   `"auto rossa/auto.obj" scale 0.01 up z forward x`), con gli errori di riga.

Quello che **non** è provato: tutto quello che passa per Unreal (il thread di
lavoro, le texture, le mesh, i materiali, le parti, i LOD dalla camera), e i
**file veri** del tuo database. È il lavoro di questa pagina.

## 1. Compilare e creare i materiali

1. Compila. Il modulo `GeoEntities` ha tre dipendenze nuove
   (`MeshDescription`, `StaticMeshDescription`, `ImageWrapper`): se Visual
   Studio non le vede, rigenera i file del progetto.
2. Nell'editor, una volta:

   ```
   geo.Imagery.CreateMaterial
   ```

   Crea (fra gli altri) `M_GeoEntity`, `M_GeoEntityMasked` e
   `M_GeoEntityTranslucent` in `/GeoWorld/Materials`. Il registro deve
   elencarli senza righe rosse. Senza, i modelli escono grigi e senza texture,
   e `geo.Entities.Models` lo dice in rosso.

## 2. Prima di Unreal: `geomodel_info`

Il primo controllo su un modello del tuo database, senza aprire niente:

```bat
cmake -S Plugins/GeoWorld/Tools/StandaloneTests -B build
cmake --build build --target geomodel_info
build\Debug\geomodel_info.exe D:\db\Models\F16\f16.flt D:\db\Models
```

Il secondo argomento è la cartella dove cercare le texture per nome (come
`geo.Entities.ModelFolder`). Stampa il formato e la versione, gli avvisi,
l'albero dei nodi (DOF con origine e limiti, switch con le maschere, LOD con
le distanze, sequenze, riferimenti esterni), i materiali con i codici IR, le
texture **trovate o no**, e il rig: le parti, i triangoli, le misure in metri
(avanti, destra, alto).

Da guardare:

- **le misure**: un F-16 è lungo ~15 m e largo ~10. Se dice 49 × 31, il
  file è in piedi e l'unità dell'intestazione è sbagliata (succede): `scale
  0.3048` nella regola. Se dice 1500 × 1000, centimetri: `scale 0.01`;
- **le texture NON TROVATE**: guarda dove le cerca (sezione 12.2 di
  `modelli-design.md`); di solito basta che la cartella dei modelli le
  contenga, in qualsiasi sottocartella;
- **gli avvisi**: record sconosciuti, riferimenti esterni che mancano. Un
  avviso non ferma la lettura, ma dice cosa manca;
- **i nomi dei DOF e degli switch**: servono per `geo.Entities.Dof` e
  `geo.Entities.Switch`.

Se un file vero dà un errore o misure assurde, mandamelo (o mandami l'uscita
di `geomodel_info`): i test sono fatti dalla specifica, un database vero avrà
le sue varianti.

## 3. Una freccia, per controllare gli assi

Prima dei modelli veri, un modello di cui sappiamo tutto. In una cartella
**fuori dal progetto**, per esempio `D:\ModelliProva\freccia\`, scrivi due
file di testo.

`freccia.obj` (OBJ: avanti −Z, alto +Y):

```
mtllib freccia.mtl
v -1 0 2
v  1 0 2
v  0 0 -3
v  0 0 2
v  0 1.5 2
v  0 0 0
usemtl rosso
f 1 2 3
usemtl blu
f 4 6 5
f 4 5 6
```

`freccia.mtl`:

```
newmtl rosso
Kd 1 0 0
newmtl blu
Kd 0 0 1
```

È un triangolo rosso lungo 5 m, con la **punta in avanti**, e una pinna blu
**in alto**, sul fondo. Il triangolo è a una faccia: si vede da sopra, non da
sotto.

Poi il file della tabella, per esempio `Config/entita.txt`:

```
1.1.105.81    freccia/freccia.obj
```

e nella console (con il generatore acceso e `geo.Entities.Listen`, come in
`fase7-verifica.md`):

```
geo.Entities.ModelFolder D:\ModelliProva
geo.Entities.Models Config/entita.txt
```

Atteso: le quattro auto diventano frecce (per un attimo restano scatole: il
modello si legge su un altro thread). `geo.Entities.Models` elenca la regola e
il modello letto: `OBJ, 1 parti (0 DOF, 0 switch), 3 triangoli, lungo 5.0 m`.

Da guardare:

1. **La punta va dove va l'auto.** Con `geo.Entities.Debug 1` la linea verde
   esce dalla punta. Se esce dal fondo, l'avanti è girato (un errore nostro,
   non del file: dimmelo);
2. **La pinna è in alto** e il triangolo rosso **si vede dall'alto**. Se si vede
   solo da sotto, i triangoli sono girati al contrario (la riflessione da
   destra a sinistra, sezione 4 di `modelli-design.md`);
3. **Le correzioni**: `freccia/freccia.obj yaw 90` nella regola e di nuovo
   `geo.Entities.Models Config/entita.txt`: la punta va a **destra** della
   direzione di marcia. `scale 2`: lunga 10 m. `offset 0 0 3`: sollevata di
   3 m;
4. **Un file che non c'è** (`freccia/nonce.obj`): le auto restano scatole
   **magenta**, il nome dice "(modello non letto)", `geo.Entities.Models` dice
   perché.

## 4. I modelli veri

Copia (o lascia dov'è) il database e dai la cartella:

```
geo.Entities.ModelFolder D:\db\Models
geo.Entities.ModelInfo F16/f16.flt
```

`ModelInfo` dice in Unreal quello che `geomodel_info` dice fuori: formato,
misure, DOF con i limiti, switch, texture trovate e no, avvisi. Poi le regole:

```
1.2.105.1     F16/f16.flt
1.2.105.21    NH90/nh90.flt
1.1.105.81    Iveco/lmv.flt
```

(i nomi sono i tuoi). Da guardare:

1. **Il muso va dove va l'aereo.** Se l'aereo vola di fianco, il modello non
   guarda verso +Y di OpenFlight: `yaw 90` o `yaw -90`, o `forward x`. Se vola
   **all'indietro**, `yaw 180`;
2. **Le texture**: ci sono, dritte (non capovolte: l'origine delle UV di
   OpenFlight è in basso e la giriamo), ripetute dove il modello le ripete;
   quelle con l'alfa (alberi, griglie) tagliate, non nere;
3. **Le misure**: affianco alle scatole di prima, un F-16 è grande come
   `plane 15 10 5`;
4. **I LOD**: allontanandosi, il modello cambia dettaglio alle distanze che
   `geomodel_info` dice. Si scelgono quattro volte al secondo: un cambio
   può arrivare con un quarto di secondo di ritardo;
5. **Le parti nascoste**: lo switch del danno parte dalla maschera del file
   (di solito intatto); una sequenza mostra il primo fotogramma (le eliche
   ferme: le fa girare M3);
6. **Il tempo**: `geo.Entities.Models` dice quanto ci ha messo ogni modello
   (`0.35 s`); il frame non deve fermarsi mentre legge (il file si legge su un
   altro thread; sul game thread si fanno le mesh, un modello per frame).

## 5. DOF e switch a mano

```
geo.Entities.Dof AEREO-1
```

Senza altri argomenti: l'elenco dei DOF e degli switch del modello di
quell'entità, con i limiti. Poi, con i nomi del tuo modello:

```
geo.Entities.Dof AEREO-1 canopy 0 30
geo.Entities.Dof ELICOTTERO rotor 45
geo.Entities.Switch AEREO-1 gear 1
```

I valori sono: imbardata, beccheggio, rollio (gradi), x, y, z (metri), **come
li intende OpenFlight**: l'imbardata positiva gira a **sinistra** (vista
dall'alto), al contrario della bussola. Oltre i limiti del modello ci si
ferma al limite: il comando risponde con i valori veri, dopo il limite.

Da guardare: la parte gira **attorno al suo perno** (la cerniera del tettuccio,
l'asse del rotore), non attorno al centro dell'aereo. Se gira attorno a un
punto sbagliato, è il caso da mandarmi con l'uscita di `geomodel_info`.

## 6. Il carico

```bat
geoworld demo-entities --crowd 2000
```

con la regola della freccia (o di un modello vero) per `1.1.105.81`. Il
modello si legge **una volta**: `geo.Entities.Models` deve elencarlo una volta
sola, e 400 attori lo condividono (una `UStaticMesh` per parte, non una per
attore).

## 7. A caldo

- `geo.Entities.ReloadModels`: rilegge tutti i modelli (dopo aver cambiato un
  file o una texture). Gli attori si rifanno.
- `geo.Entities.ModelFolder` con un'altra cartella: lo stesso, e le texture
  si cercano nella nuova.
- Il valore di partenza della cartella sta in **Project Settings → Plugins →
  GeoWorld - Entita' → Modelli → Model Folder**.

## 8. Le API più a rischio (se non compila)

| Dove | Cosa | Se non c'è |
|---|---|---|
| `GeoModelLibrary.cpp` | `UStaticMesh::BuildFromMeshDescriptions` con `FBuildMeshDescriptionsParams::bFastBuild` | è quello degli alberi (`GeoVegetationSubsystem.cpp`): copiare da lì |
| `GeoModelLibrary.cpp` | `IImageWrapperModule::DetectImageFormat`, `CreateImageWrapper`, `GetRaw(ERGBFormat::BGRA, 8, TArray64<uint8>&)`, `EImageFormat::TGA` | in 5.x `GetRaw` vuole `TArray64`; se `EImageFormat::TGA` non c'è, togliere la riga del TGA |
| `GeoModelLibrary.cpp` | `UE::Tasks::Launch`, `FTask::IsCompleted`, `TQueue<..., EQueueMode::Mpsc>` | come nella vegetazione e nelle nuvole |
| `GeoModelLibrary.cpp` | `CreateGeoRuntimeDataTexture` con le mip e il wrap | è di GeoCore (`Imagery/GeoRuntimeTexture.h`), lo usa l'ortofoto |
| `GeoEntitySubsystem.cpp` | `FMatrix` da righe (`FPlane`), `FTransform(FMatrix)` | in `AffineToTransform`: le righe sono le immagini degli assi |
| `GeoTerrainMaterialFactory.cpp` | `UMaterialExpressionVertexColor`, `BLEND_Masked`, `BLEND_Translucent`, `OpacityMaskClipValue`, `TwoSided` | come gli altri materiali della fabbrica |

## 9. Se qualcosa non torna

- **I modelli sono grigi**: mancano i materiali (sezione 1), o le texture
  non sono state trovate (`geo.Entities.ModelInfo`: "NON TROVATA").
- **Le texture sono capovolte**: un formato con l'origine delle UV diversa
  da quella dichiarata. Dimmi il formato e mandami il file: la correzione è in
  un punto solo (`BuildRig`).
- **Il modello si vede "da dentro"** (le facce davanti trasparenti, quelle
  dietro visibili): i triangoli girati al contrario. Se succede anche con la
  freccia, è un errore nostro; se succede solo con un modello, guarda se ha
  matrici con scala negativa (`geomodel_info` le stampa) e mandamelo.
- **Il modello è enorme o minuscolo**: le unità (sezione 2), `scale`.
- **Il modello è sotto terra o galleggia**: l'origine del file non è sotto le
  ruote. `offset 0 0 H` nella regola.
- **Resta una scatola**: il modello è ancora "in lettura" (il nome lo dice),
  oppure la regola non vale per quel tipo (`geo.Entities.List` dice il tipo e
  il modello di ogni entità).
- **Un `.fbx` o un `.ive`**: non si leggono (sezione 3 di `modelli-design.md`);
  il messaggio dice come convertirli. Un FBX si può anche importare
  nell'editor come asset e usare con una regola `/Game/...`.

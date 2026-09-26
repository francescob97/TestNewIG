# Capitolo 1 — Unreal Engine per chi viene da fuori

> Questo capitolo non è un manuale di Unreal: è **il pezzo di Unreal che serve
> per capire questo progetto**, spiegato attraverso il codice del progetto
> stesso. Ogni concetto ha un esempio preso da un file che puoi aprire.

---

## L'idea in breve

Unreal Engine è un motore di videogiochi scritto in C++. Ma il C++ di Unreal
**non è il C++ normale**: il motore aggiunge un sistema di riflessione, un
garbage collector, macro speciali, contenitori propri, e un sistema di build
suo. Chi arriva dal C++ standard si trova spaesato non perché il linguaggio sia
diverso, ma perché **sopra il linguaggio c'è un secondo sistema** con regole
proprie.

Questo capitolo ti dà le regole di quel secondo sistema, nell'ordine in cui le
incontri leggendo il codice.

---

## 1.1 Progetto, plugin, modulo

Sono tre scatole una dentro l'altra.

| Scatola | File che la definisce | In questo progetto |
|---|---|---|
| **Progetto** | `TestNewIG.uproject` | il guscio vuoto |
| **Plugin** | `GeoWorld.uplugin` | tutto il codice |
| **Modulo** | `<Nome>.Build.cs` | `GeoCore`, `GeoTiles`, `GeoRender`, `GeoWorldEditor` |

**Il progetto** è ciò che apri con l'editor. Contiene i livelli, le impostazioni
(`Config/`), e i contenuti (`Content/`).

**Un plugin** è un pacchetto di codice e contenuti riutilizzabile, che si può
copiare in un altro progetto. Sta in `Plugins/`.

**Un modulo** è l'unità di compilazione: ogni modulo diventa **una DLL** su
Windows. Un plugin ne contiene uno o più.

### Perché tutto il codice sta in un plugin

Si poteva mettere tutto in `Source/TestNewIG`, il modulo del progetto. È stato
scelto il plugin per due ragioni:

1. **Riusabilità.** Il giorno che serve GeoWorld in un altro progetto, si copia
   la cartella `Plugins/GeoWorld` e basta.
2. **Confini chiari.** Il progetto è "la scena", il plugin è "il motore di
   terreno". Mescolarli renderebbe impossibile capire cosa dipende da cosa.

> ⚠️ **Trappola.** Il progetto ha comunque un modulo C++ minimo,
> `Source/TestNewIG`, che non fa quasi niente. Esiste per un motivo pratico:
> **Unreal non compila il C++ di un plugin se il progetto è "solo Blueprint"**.
> Serve almeno un modulo C++ nel progetto perché l'editor offra di compilare.
> File: `Source/TestNewIG/TestNewIG.Build.cs`, `TestNewIG.cpp`.

### Il file `.uplugin`

È un file JSON che elenca i moduli del plugin. Guardalo:
`Plugins/GeoWorld/GeoWorld.uplugin`. Due campi per modulo contano:

* **`Type`**: `Runtime` significa "serve anche nel gioco finito";
  `Editor` significa "serve solo nell'editor, non finisce nel gioco". Per questo
  `GeoWorldEditor` è di tipo `Editor`: il comando che crea il materiale non ha
  senso in un gioco distribuito.
* **`LoadingPhase`**: quando caricarlo. `GeoCore` è `PreDefault`, cioè prima
  degli altri, perché tutti gli altri dipendono da lui.

E un campo per tutto il plugin: `"CanContainContent": true`, che permette al
plugin di avere una cartella `Content/` con asset propri. Serve al materiale del
drappeggio (capitolo 7).

### Il file `.Build.cs`

Ogni modulo ha un file C# (sì, C#, non C++) che dice a Unreal **come
compilarlo**: da quali altri moduli dipende, quali opzioni usare. Ecco quello di
`GeoTiles`, semplificato:

```csharp
public class GeoTiles : ModuleRules
{
    public GeoTiles(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PublicDependencyModuleNames.AddRange(new string[]
        {
            "Core", "CoreUObject", "Engine", "GeoCore", "ImageWrapper",
        });
    }
}
```

**Public** o **Private** dependency? La differenza è sottile e importante:

* **Public**: "i miei header usano questo modulo, quindi chi include i miei
  header ne ha bisogno anche lui". `GeoTiles` è pubblicamente dipendente da
  `GeoCore` perché i suoi header parlano di tipi di `GeoCore`.
* **Private**: "lo uso solo nei miei `.cpp`". Chi usa `GeoTiles` non ne sa
  niente.

> 💡 **Esempio dal progetto.** `GeoCore.Build.cs` aggiunge `UnrealEd`
> (il modulo dell'editor) **solo se si sta compilando l'editor**:
> ```csharp
> if (Target.bBuildEditor)
> {
>     PrivateDependencyModuleNames.Add("UnrealEd");
> }
> ```
> Serve per muovere la camera del viewport dell'editor. Senza questa
> condizione, il progetto compilerebbe nell'editor e si romperebbe quando lo
> impacchetti come gioco, che è il momento peggiore per scoprirlo.

---

## 1.2 Come si compila: UBT e UHT

Quando premi "Build" in Visual Studio, non parte direttamente il compilatore
C++. Partono prima due programmi di Unreal:

1. **UHT — Unreal Header Tool.** Legge i tuoi `.h` cercando le macro speciali
   (`UCLASS`, `UPROPERTY`...) e **genera codice C++** aggiuntivo: i file
   `.generated.h`.
2. **UBT — Unreal Build Tool.** Legge i `.Build.cs`, decide cosa compilare e
   con quali opzioni, e chiama il compilatore vero (MSVC su Windows).

Per questo ogni header che dichiara una classe Unreal finisce con un include
strano:

```cpp
#include "GeoMarkerActor.generated.h"   // DEVE essere l'ultimo include
```

Quel file **non esiste** finché UHT non l'ha generato. Deve essere l'ultimo
include del file: è una regola rigida, e violarla dà errori incomprensibili.

> ⚠️ **Trappola che hai già incontrato.** Quando aggiungi file o cartelle
> nuove, Visual Studio non li vede finché non rigeneri i file di progetto:
> tasto destro sul `.uproject` → **Generate Visual Studio project files**.
> Prima di quel passaggio, IntelliSense segnala come mancanti include che il
> compilatore invece trova benissimo. È la causa degli errori
> `cannot open source file "Unreal/..."` di qualche settimana fa.

---

## 1.3 Il sistema UObject: le macro magiche

Questa è la differenza più grande dal C++ normale.

Unreal ha una classe base universale, **`UObject`**. Ogni oggetto che deriva da
`UObject` ottiene gratis: riflessione (il motore sa quali campi e metodi ha),
garbage collection, serializzazione (salvataggio su disco), esposizione
all'editor e ai Blueprint.

Per ottenere tutto questo, la classe va **dichiarata con le macro**:

```cpp
UCLASS(ClassGroup = (GeoWorld), meta = (BlueprintSpawnableComponent))
class GEOCORE_API UGeoTransformComponent : public USceneComponent
{
    GENERATED_BODY()

public:
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GeoWorld")
    FGeoCoordinate GeoCoordinate;

    UFUNCTION(BlueprintCallable, Category = "GeoWorld")
    void SetGeoCoordinate(const FGeoCoordinate& InCoordinate);
};
```

*(da `GeoCore/Public/Georeference/GeoTransformComponent.h`)*

Riga per riga:

| Macro | Cosa fa |
|---|---|
| `UCLASS(...)` | "questa è una classe Unreal, generale codice di riflessione" |
| `GEOCORE_API` | "esporta questa classe dalla DLL" — vedi 1.10 |
| `GENERATED_BODY()` | dove UHT inserisce il codice generato. Obbligatorio |
| `UPROPERTY(...)` | "questo campo è noto al motore" |
| `UFUNCTION(...)` | "questo metodo è noto al motore" |

Le opzioni fra parentesi dicono cosa fare del campo:
`EditAnywhere` lo rende modificabile nel pannello Details dell'editor,
`BlueprintReadWrite` lo espone ai Blueprint, `Category` lo raggruppa. E la più
importante per noi, **`config`**, lo fa leggere da un file `.ini` (vedi 1.14).

### USTRUCT: le strutture leggere

Per dati semplici senza comportamento si usa `USTRUCT` invece di `UCLASS`. Le
struct non derivano da `UObject`, non hanno garbage collector, sono leggere.

```cpp
USTRUCT()
struct FGeoTerrainStats
{
    GENERATED_BODY()

    UPROPERTY() int32 TileConGeometria = 0;
    UPROPERTY() int32 TriangoliTotali = 0;
    // ...
};
```

*(da `GeoRender/Public/Terrain/GeoTerrainSubsystem.h`)*

### I prefissi dei nomi

Unreal impone prefissi alle classi, e **UHT si rifiuta di compilare** se li
sbagli. Imparali a memoria, ti fanno leggere il codice molto più in fretta:

| Prefisso | Significa | Esempio dal progetto |
|---|---|---|
| `U` | deriva da `UObject` | `UGeoreferenceSubsystem` |
| `A` | deriva da `AActor` (un attore nel mondo) | `AGeoMarkerActor`, `AGeoFlyPawn` |
| `F` | struct o classe normale | `FGeodetic`, `FTileCache`, `FGeoLoaderPool` |
| `E` | enumerazione | `EGeoTileState` |
| `I` | interfaccia (classe astratta) | `IGeoTerrainMeshProvider` |
| `T` | template | `TTileCache<Payload>`, `TArray<T>` |
| `b` | variabile booleana | `bAutoRebase`, `bFlipWinding` |

E una convenzione per i parametri: **`In`** davanti al nome, per non
confonderli con i campi della classe. `SetWireframe(bool bInWireframe)`, non
`SetWireframe(bool bWireframe)`. Il capitolo 9 racconta perché questa regola in
Unreal non è estetica: il compilatore la impone.

---

## 1.4 Il garbage collector, e perché puoi perdere oggetti senza accorgertene

In C++ normale, un oggetto creato con `new` vive finché non fai `delete`. In
Unreal, **gli oggetti `UObject` li cancella il motore da solo**, quando nessuno
li usa più. È il *garbage collector* (GC).

Il punto delicato è: **come fa il GC a sapere se "qualcuno lo usa"?** Guarda solo
i riferimenti che conosce, cioè:

* i campi marcati `UPROPERTY` in altri `UObject`;
* i riferimenti esplicitamente dichiarati con strumenti appositi.

Un puntatore C++ nudo (`UMaterial*`) in una classe che **non** è un `UObject`,
o in un campo senza `UPROPERTY`, è **invisibile** al GC. L'oggetto può essere
cancellato mentre il puntatore lo sta ancora indicando.

> ⚠️ **Trappola trovata in questo progetto.** `FDynamicMeshTerrainProvider`
> non è un `UObject`, e teneva il materiale del terreno così:
> ```cpp
> TWeakObjectPtr<UMaterialInterface> Material;   // SBAGLIATO
> ```
> Il materiale era caricato con `LoadObject` e nessun altro lo teneva in vita.
> Al primo passaggio del GC sarebbe sparito, e le tile costruite dopo sarebbero
> rimaste con il materiale grigio di default, senza un solo messaggio d'errore.
> Correzione:
> ```cpp
> TStrongObjectPtr<UMaterialInterface> Material;  // GIUSTO
> ```
> *(in `GeoRender/Public/Terrain/DynamicMeshTerrainProvider.h`)*

### I quattro puntatori di Unreal

| Puntatore | Tiene vivo l'oggetto? | Quando usarlo |
|---|---|---|
| `UPROPERTY() UFoo* Ptr` | **Sì** | campo di un `UObject`: la scelta normale |
| `TObjectPtr<UFoo>` | Sì (se `UPROPERTY`) | come sopra, versione moderna UE5 |
| `TWeakObjectPtr<UFoo>` | **No** | "puntami, ma se sparisci dimmelo" |
| `TStrongObjectPtr<UFoo>` | **Sì** | tenere vivo un oggetto da una classe che **non** è `UObject` |

`TWeakObjectPtr` è utile e sicuro: se l'oggetto viene cancellato, `.Get()`
restituisce `nullptr` invece di un indirizzo marcio. Il progetto lo usa per i
componenti delle tile, che sono già tenuti in vita dal loro attore. Ma **non
tiene vivo niente**, ed è l'errore della trappola sopra.

---

## 1.5 Il mondo, gli attori, i componenti

Un livello di Unreal è un **`UWorld`**. Dentro il mondo vivono gli **attori**
(`AActor`): tutto ciò che ha una posizione nella scena. Ogni attore è composto
da **componenti** (`UActorComponent`), che gli danno capacità.

Pensalo come un mobile e i suoi pezzi: l'attore è il mobile, i componenti sono
le ante, i cassetti, le maniglie.

```
AGeoMarkerActor                  (l'attore: un cubo di verifica)
└── UGeoTransformComponent       RADICE: lo tiene a una coordinata geografica
    └── UStaticMeshComponent     figlio: disegna il cubo, e si muove con la radice
```

*(da `GeoRender/Private/GeoMarkerActor.cpp`)*

Il componente geografico è la **radice** dell'attore, e il cubo è suo figlio.
Così quando il rebasing ricalcola la posizione della radice, il cubo la segue da
solo. I componenti di un attore si creano nel **costruttore** con
`CreateDefaultSubobject<T>(TEXT("Nome"))` — che è diverso dal `NewObject` usato
a runtime, e il paragrafo che segue spiega la differenza.

I **`USceneComponent`** sono quelli che hanno una posizione (una
*trasformazione*: posizione, rotazione, scala). Si agganciano uno all'altro ad
albero, e ogni figlio si muove con il padre.

### La scelta di un componente invece di una classe base

`UGeoTransformComponent` è un componente, non una classe base come
`AGeoActor`. È una scelta tipica e importante di Unreal:

* **Classe base**: per georeferenziare un oggetto devi farlo derivare da
  `AGeoActor`. Ma un oggetto può derivare da una sola classe: se ti serve un
  `ACharacter` georeferenziato, sei bloccato.
* **Componente**: lo aggiungi a **qualunque** attore esistente. Composizione
  invece di ereditarietà.

### Due dettagli che danno problemi a runtime

**Mobilità.** Ogni `USceneComponent` ha una `Mobility`: `Static`, `Stationary`
o `Movable`. Gli oggetti statici hanno illuminazione precalcolata e **non si
possono spostare**. Le nostre tile e i nostri marker si spostano a ogni
rebasing, quindi sono tutti `Movable`:

```cpp
Component->SetMobility(EComponentMobility::Movable);
```

**Registrazione.** Ci sono due modi di creare un componente, e non sono
intercambiabili:

| Dove | Funzione | Registrazione |
|---|---|---|
| nel **costruttore** dell'attore | `CreateDefaultSubobject<T>()` | automatica, quando l'attore entra nel mondo |
| **a runtime**, in qualunque momento dopo | `NewObject<T>(Outer)` | **a mano**, con `RegisterComponent()` |

`CreateDefaultSubobject` fuori dal costruttore fa fallire un'asserzione. E un
componente creato con `NewObject` **non esiste per il motore** finché non chiami
`RegisterComponent()`: non si disegna, non ticka, non ha collisioni. Le nostre
tile nascono a runtime, quindi usano la seconda strada:

```cpp
UDynamicMeshComponent* Component = NewObject<UDynamicMeshComponent>(Actor);
Component->SetupAttachment(Actor->GetRootComponent());   // prima
Component->RegisterComponent();                          // poi
```

*(da `GeoRender/Private/Terrain/DynamicMeshTerrainProvider.cpp`)*

`SetupAttachment` si usa **prima** della registrazione; dopo, si usa
`AttachToComponent`. Invertirli è un errore classico.

---

## 1.6 I subsystem: il posto giusto per un "gestore"

Quasi ogni progetto ha bisogno di un "gestore": un oggetto unico che coordina
qualcosa. In Unreal la risposta moderna è il **subsystem**: una classe che il
motore crea e distrugge **da solo**, legata a un ciclo di vita preciso.

| Famiglia | Vive quanto | Esempio d'uso |
|---|---|---|
| `UEngineSubsystem` | tutto il processo | servizi globali |
| `UGameInstanceSubsystem` | la partita, attraverso i cambi di livello | punteggio |
| **`UWorldSubsystem`** | **un singolo mondo** | ← quasi tutti i nostri |
| `ULocalPlayerSubsystem` | un giocatore | input |

GeoWorld ha sei subsystem di mondo:

| Subsystem | Modulo | Compito |
|---|---|---|
| `UGeoreferenceSubsystem` | GeoCore | origine e rebasing |
| `UGeoTileStreamingSubsystem` | GeoTiles | carica le tile di quota |
| `UGeoImageryStreamingSubsystem` | GeoTiles | carica le tile di ortofoto |
| `UGeoQuadtreeSubsystem` | GeoRender | decide cosa disegnare |
| `UGeoTerrainSubsystem` | GeoRender | costruisce la geometria |
| `UGeoImagerySubsystem` | GeoRender | veste il terreno |

**Perché "di mondo".** L'editor ha un mondo; quando premi Play ne crea un
**secondo**, una copia (il *PIE world*). Con un subsystem di mondo, ognuno ha il
proprio, e premere Play non mescola lo stato dell'editor con quello della
partita.

Per recuperarne uno:

```cpp
UGeoreferenceSubsystem* Geo = World->GetSubsystem<UGeoreferenceSubsystem>();
```

E per dire "ho bisogno che quell'altro sia pronto prima di me":

```cpp
Collection.InitializeDependency<UGeoTileStreamingSubsystem>();
```

### Il tick: fare qualcosa a ogni frame

Un subsystem normale non ha un metodo chiamato ogni frame. Per averlo si deriva
da **`UTickableWorldSubsystem`** e si implementa `Tick(float DeltaTime)`.

> ⚠️ **Trappola trovata in questo progetto — ed è stata la più subdola.**
> La prima versione di `UGeoreferenceSubsystem` otteneva il tick "a mano",
> derivando da `UWorldSubsystem` **e** da `FTickableGameObject`. Compila,
> sembra giusto. Ma `FTickableGameObject` ha un metodo
> `GetTickableGameObjectWorld()` che, se non lo sovrascrivi, restituisce
> `nullptr`: il tick **non è legato a nessun mondo**, e in certe
> configurazioni non parte mai. Risultato: il rebasing non scattava, e nessun
> errore lo diceva. `UTickableWorldSubsystem` fa tutto questo nel modo giusto.
> Il commento in `GeoreferenceSubsystem.h` racconta il bug per intero.

---

## 1.7 Il game thread, e la regola che non si viola

Unreal fa girare la logica del gioco su **un thread principale**, il *game
thread*. Ogni `Tick` di ogni attore e subsystem gira lì, uno dopo l'altro. Il
tempo disponibile è quello di un frame: a 60 fps, **16,6 millisecondi per
tutto**.

Da questo nasce la regola più importante del runtime:

> **Sul game thread non si fa mai niente che possa bloccarsi.**

Una lettura da disco può durare 0,1 ms oppure 50 ms, a seconda di cosa sta
facendo il sistema operativo. Nel secondo caso hai perso tre frame e il gioco
scatta. Per questo le letture delle tile avvengono su **altri thread**
(capitolo 4), e il game thread riceve solo i dati già pronti.

### Gli strumenti di Unreal per i thread

| Strumento | Cosa fa | Dove lo usiamo |
|---|---|---|
| `FQueuedThreadPool` | un gruppo di thread che eseguono lavori in coda | `FGeoLoaderPool` |
| `IQueuedWork` | un singolo lavoro da mettere in coda | `FGeoTileLoadWork`, `FGeoImageryLoadWork` |
| `TQueue<T, EQueueMode::Mpsc>` | coda sicura fra thread | consegna dei risultati |
| `FRWLock` | lucchetto lettori/scrittori | la fotografia della georeferenziazione |

Il task graph del motore (il sistema che distribuisce il lavoro su tutti i
core) esiste, ma **non va usato per l'I/O**: è dimensionato sui core, e un
lavoro che si blocca sul disco ruba un core a tutto il resto del motore.

---

## 1.8 Editor, PIE e gioco: tre mondi diversi

Un'altra cosa che confonde all'inizio: **il tuo codice gira in contesti
diversi**, e si comporta diversamente in ognuno.

| Contesto | `EWorldType` | La camera è... |
|---|---|---|
| Editor, fuori dal Play | `Editor` | uno stato del **viewport**, non un attore |
| Play In Editor (PIE) | `PIE` | un attore (il pawn del giocatore) |
| Gioco distribuito | `Game` | un attore |

Questo ha avuto conseguenze concrete nel progetto. Muovere la camera
(`geo.Goto`, il rebasing) richiede due strade diverse: nel Play si sposta un
attore; nell'editor si chiede al *viewport client*, che è codice di editor e si
raggiunge solo con `UnrealEd`.

Quel codice va chiuso tra due righe:

```cpp
#if WITH_EDITOR
    // codice che esiste solo quando compili l'editor
#endif
```

`WITH_EDITOR` vale 1 quando compili l'editor e 0 quando compili il gioco. Senza
la guardia, il gioco finito non compilerebbe, perché `UnrealEd` lì non esiste.

> 💡 **Esempio.** `geo.Fly` funziona solo nel Play, perché sostituisce un
> attore (la camera) con un altro. Nell'editor ti risponde che non c'è un
> `PlayerController` e ti rimanda a `geo.ViewSpeed`, che agisce sul viewport.
> File: `GeoRender/Private/GeoRenderModule.cpp` e `GeoCore/Private/GeoCoreModule.cpp`.

---

## 1.9 Coordinate di Unreal: centimetri, mano sinistra, double

Tre fatti da sapere, e il capitolo 2 li usa tutti.

**Unità: centimetri.** 1 unità Unreal (uu) = 1 cm. Un attore a `X = 100` è a un
metro dall'origine. In questo progetto la conversione metri → centimetri avviene
**in un punto solo** (`GeoCore/Public/Geo/GeoUnits.h`), e un controllo
automatico impedisce di scrivere `* 100` altrove.

**Assi: sinistrorsi.** X in avanti, Y a destra, Z in alto. La maggior parte della
matematica e dei sistemi geografici è **destrorsa**. Il capitolo 2 spiega perché
questo obbliga a una riflessione, e cosa succede se la si dimentica.

**Precisione: double, ma non ovunque.** In Unreal 4 le coordinate erano `float`
(32 bit), e il mondo era limitato a circa 20 km. Unreal 5 ha introdotto le
**Large World Coordinates**: `FVector` è diventato `double` (64 bit). Ma il
double si ferma alla CPU: **i vertici che vanno alla scheda video sono ancora
`float`**. È il motivo per cui il rebasing (capitolo 2) serve anche in UE5.

---

## 1.10 I moduli sono DLL, e questo cambia come scrivi il codice

Ogni modulo diventa una DLL separata. Una DLL, per default, **nasconde tutto
quello che contiene**: una funzione definita in `GeoCore.dll` non è chiamabile
da `GeoRender.dll`, a meno che non sia **esportata**.

Per esportare, Unreal genera per ogni modulo una macro `<MODULO>_API`:

```cpp
class GEOCORE_API UGeoreferenceSubsystem : public UTickableWorldSubsystem
//    ^^^^^^^^^^^ "esporta questa classe dalla DLL di GeoCore"
```

Senza la macro, il codice compila benissimo dentro `GeoCore`, e il linker
fallisce quando `GeoRender` prova a usarlo:

```
unresolved external symbol "GeodeticToEcef" referenced in function ...
```

È esattamente l'errore che hai visto con la Fase 4.

**Perché lo strato puro è header-only.** Le funzioni matematiche come
`GeodeticToEcef` stanno nello strato puro, che per principio non deve contenere
niente di Unreal — nemmeno la macro `GEOCORE_API`. La soluzione è scrivere il
codice **direttamente negli header**, con `inline`: così ogni modulo che include
l'header ne ottiene una copia compilata, e non c'è niente da esportare. Su
funzioni matematiche di poche righe non costa niente.

---

## 1.11 La build unity, e perché due file possono scontrarsi

Per compilare più in fretta, Unreal **incolla insieme parecchi `.cpp` dello
stesso modulo** in un unico file gigante, e compila quello. Si chiama *unity
build*.

Conseguenza sorprendente: due funzioni con lo stesso nome in due `.cpp` diversi,
che in C++ normale sarebbero indipendenti (perché dentro un `namespace` anonimo,
cioè private al file), **diventano la stessa funzione definita due volte**.

```
C2084: function 'PackKey' already has a body
```

È successo: `PackKey` era scritta in quattro file. La correzione non è
rinominare, ma **spostare la funzione in un header condiviso**: se serve in
quattro file, non appartiene a nessuno dei quattro. Ora è `FTileKey::Pack()` in
`GeoTiles/Public/Tiles/TileKey.h`, e `Tools/CheckUnityCollisions.py` controlla
che non ricapiti.

---

## 1.12 I contenitori di Unreal

Unreal ha i suoi contenitori, al posto di quelli della libreria standard:

| Unreal | Standard C++ | Note |
|---|---|---|
| `FString` | `std::string` | stringhe larghe (`TCHAR`), letterali con `TEXT("...")` |
| `TArray<T>` | `std::vector<T>` | |
| `TMap<K, V>` | `std::unordered_map<K, V>` | |
| `TSet<T>` | `std::unordered_set<T>` | |
| `TSharedPtr<T>` | `std::shared_ptr<T>` | per oggetti **non** `UObject` |
| `TUniquePtr<T>` | `std::unique_ptr<T>` | |

**Il ponte Unreal usa quelli di Unreal. Lo strato puro usa quelli standard.**
Non è incoerenza: lo strato puro non può includere niente di Unreal, quindi usa
`std::vector`, `std::shared_ptr`. Il ponte fa la traduzione al confine.

> 💡 **Esempio.** La cache delle tile (`TileCache.h`, strato puro) consegna le
> tile come `std::shared_ptr<const FHeightTile>`. Se il quadtree sta ancora
> usando una tile mentre la cache la sfratta, il `shared_ptr` la tiene viva
> finché l'ultimo utente non la rilascia. Senza, uno sfratto nel momento
> sbagliato lascerebbe un puntatore a memoria già liberata.

Le stringhe letterali vanno sempre scritte con `TEXT()`:

```cpp
UE_LOG(LogGeoWorld, Log, TEXT("Rebase numero %d"), Count);
```

---

## 1.13 Comandi console, log, messaggi a schermo

Questi sono i tuoi strumenti di debug, e il progetto li usa ovunque.

### Comandi console

Si apre la console con il tasto sotto `Esc`, a sinistra dell'`1` — su una
tastiera italiana è quello del `\`. Se non funziona, il tasto si cambia in
*Project Settings → Input → Console Keys*. Un comando si dichiara così:

```cpp
static FAutoConsoleCommandWithWorldAndArgs GeoGotoCommand(
    TEXT("geo.Goto"),                                    // il nome da digitare
    TEXT("geo.Goto <lat> <lon> [quota] - ..."),          // l'aiuto
    FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
        [](const TArray<FString>& Args, UWorld* World)   // cosa fa
    {
        // ...
    }));
```

*(da `GeoCore/Private/GeoCoreModule.cpp`)*

Il nome della classe dice cosa riceve la funzione: `WithWorld` riceve il mondo,
`AndArgs` riceve gli argomenti. Le combinazioni sono **solo sei**, e una che
sembra ovvia non esiste:

```
FAutoConsoleCommand                              (può ricevere anche argomenti)
FAutoConsoleCommandWithWorld
FAutoConsoleCommandWithWorldAndArgs
FAutoConsoleCommandWithOutputDevice
FAutoConsoleCommandWithArgsAndOutputDevice
FAutoConsoleCommandWithWorldArgsAndOutputDevice
```

`FAutoConsoleCommandWithArgs` **non esiste**. È un errore fatto in questo
progetto, e ora un controllo automatico lo intercetta.

### Log

```cpp
UE_LOG(LogGeoWorld, Warning, TEXT("[GeoImagery] tile %d mancante"), Key);
```

`LogGeoWorld` è una *categoria* di log, dichiarata in `GeoCoreModule.h` e
definita in `GeoCoreModule.cpp`. Le categorie permettono di filtrare: nella
finestra Output Log dell'editor puoi mostrare solo i messaggi di GeoWorld.

### Messaggi a schermo

```cpp
GEngine->AddOnScreenDebugMessage(Key, 0.0f, FColor::Green, TEXT("..."));
```

Il primo parametro è una **chiave**: se chiami di nuovo con la stessa chiave, il
messaggio viene sostituito invece di accumularsi. Gli overlay di debug usano
chiavi fisse (`0x6E90`, `0x6EA0`...) e durata `0.0f`, così vengono ridisegnati a
ogni frame senza riempire lo schermo.

---

## 1.14 Configurazione: i file .ini e le impostazioni di progetto

Unreal legge le impostazioni da file `.ini` nella cartella `Config/`. Una classe
derivata da `UDeveloperSettings`, con campi marcati `config`, **compare da sola**
in *Project Settings* e si salva da sola nel `.ini`.

```cpp
UCLASS(config = Game, defaultconfig, meta = (DisplayName = "GeoWorld"))
class GEOCORE_API UGeoWorldSettings : public UDeveloperSettings
{
    UPROPERTY(config, EditAnywhere, Category = "Georeference|Rebasing")
    double RebaseThresholdKm = 10.0;
};
```

*(da `GeoCore/Public/Georeference/GeoWorldSettings.h`)*

E nel file `Config/DefaultGame.ini` compare:

```ini
[/Script/GeoCore.GeoWorldSettings]
RebaseThresholdKm=10.000000
```

Il nome della sezione segue una regola fissa: `/Script/<Modulo>.<Classe senza la U>`.

> ⚠️ **Trappola.** Il valore nel `.ini` **vince** su quello scritto nel codice.
> Se cambi `RebaseThresholdKm = 10.0` in `20.0` nel C++ e non vedi effetto,
> guarda il `.ini`: il motore applica il file dopo aver costruito l'oggetto.

---

## 1.15 Asset, materiali e texture

Tutto ciò che sta in `Content/` è un **asset**, salvato in un file `.uasset`.
Un asset è **binario**: non si scrive a mano, lo crea l'editor.

Questo ha avuto una conseguenza pratica nella Fase 6: il materiale del terreno
è un asset, e chi scriveva il codice non aveva l'editor. Le strade erano due:
un comando che lo costruisce da codice (`geo.Imagery.CreateMaterial`) e le
istruzioni per farlo a mano. Il capitolo 7 le descrive.

Tre oggetti da distinguere:

| Oggetto | Cos'è | Si modifica a runtime? |
|---|---|---|
| `UMaterial` | lo shader, con i suoi nodi | No |
| `UMaterialInstanceDynamic` (MID) | una copia con **parametri** modificabili | **Sì** |
| `UTexture2D` | un'immagine | Sì, se creata a runtime |

Il pattern è: **un materiale, tante istanze dinamiche**. Il materiale del
terreno ha un parametro `BaseColor` (la texture); ogni tile ha la sua MID con la
propria texture. Creare un materiale diverso per ogni tile sarebbe impossibile
(non si creano a runtime) e costosissimo.

---

## 1.16 Come arriva qualcosa sullo schermo

Ultimo concetto, e serve per i capitoli 6 e 7.

Un componente che si disegna è una **primitiva** (`UPrimitiveComponent`). Per
essere disegnata, una primitiva crea un oggetto parallelo che vive sul **thread
di rendering**: il *scene proxy* (`FPrimitiveSceneProxy`). Il game thread e il
render thread lavorano in parallelo, ognuno con la sua copia dei dati.

Ogni primitiva ha dei **bounds**: il volume che la contiene. Il renderer li usa
per decidere se la primitiva è visibile. Se i bounds sono sbagliati (troppo
piccoli, o nel posto sbagliato), la primitiva sparisce anche se la geometria è
perfetta.

Il progetto usa `UDynamicMeshComponent`, un componente di Unreal che accetta
una mesh costruita a runtime. È comodo ma costoso (un componente, un proxy e un
draw call per tile). Per questo sta dietro un'interfaccia
(`IGeoTerrainMeshProvider`) che permette di sostituirlo in futuro con un proxy
scritto da zero. Il capitolo 6 spiega la scelta.

---

## Riepilogo

* **Tre scatole**: progetto (`.uproject`) ⊃ plugin (`.uplugin`) ⊃ moduli
  (`.Build.cs`). Ogni modulo è una DLL.
* **UHT genera codice** dalle macro `UCLASS`, `UPROPERTY`, `UFUNCTION`; il
  `.generated.h` va incluso per ultimo. Dopo aver aggiunto file, **rigenera i
  file di progetto**.
* **Prefissi obbligatori**: `U`, `A`, `F`, `E`, `I`, `T`, `b`. Parametri con `In`.
* **Il garbage collector** vede solo i riferimenti che conosce: da una classe
  non-`UObject` usa `TStrongObjectPtr` per tenere vivo un oggetto.
* **Attori e componenti**: composizione invece di ereditarietà. `Movable` se si
  muove, `RegisterComponent()` dopo averlo creato.
* **Subsystem** per i gestori; `UTickableWorldSubsystem` per il tick.
* **Mai bloccare il game thread**: I/O e decodifica su altri thread.
* **Editor, PIE, gioco** sono mondi diversi; codice di editor in `#if WITH_EDITOR`.
* **Centimetri, mano sinistra, double sulla CPU ma float sulla GPU.**
* **`_API`** per esportare dalle DLL; lo strato puro è header-only per non doverlo fare.
* **Unity build**: niente funzioni omonime in namespace anonimi di file diversi.
* **Log, console, overlay** sono gli strumenti di debug; il `.ini` vince sul codice.

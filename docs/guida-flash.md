# Guida: dalla tile alla mesh, fino al rebase (a caccia del flash)

> Una scaletta per leggere il codice nell'ordine in cui una tile viaggia: dal
> disco alla cache, dalla selezione del quadtree alla mesh, dalla mesh al
> renderer, e infine il rebase. Per ogni tappa: cosa leggere (file e riga),
> cosa capire, come vederlo in gioco, e dove potrebbe nascere un flash.
>
> Le cause di flash qui sotto sono **ipotesi da verificare**, dedotte leggendo
> il codice: nessuna è stata vista girare. Le cause già trovate e corrette
> sono raccontate in `docs/strade-3d.md` (sezioni 2 e 6) e in
> `docs/prova-torino-3.md` (sezione 2).
>
> I numeri di riga valgono per il codice di oggi: se nel frattempo il file è
> cambiato, cerca il nome della funzione.

---

## 0. La mappa di un frame

```
UGeoreferenceSubsystem::Tick   -> eventuale REBASE (sposta la camera, ricalcola le trasformazioni)
UGeoTileStreamingSubsystem     -> le quote lette dal disco entrano in cache
UGeoQuadtreeSubsystem::Tick    -> SELEZIONE: cosa va a schermo, cosa caricare
UGeoTerrainSubsystem::Tick     -> mostra/nasconde, consegna le mesh pronte, lancia costruzioni, transizioni
UGeoImagerySubsystem::Tick     -> veste le tile con le foto
fine frame (Unreal)            -> ricrea i proxy di scena, la GPU disegna
```

**L'ordine dei tick fra subsystem non è garantito**: verificalo per primo.
Metti un `UE_LOG` con `GFrameCounter` all'inizio di ognuno dei cinque `Tick` e
in `ApplyRebase`. Ti servirà per capire in che frame succede cosa.

---

## 1. La tile: cos'è

| Dove | Cosa |
|---|---|
| `Plugins/GeoWorld/Source/GeoTiles/Public/Tiles/TilingScheme.h:21` | 129 × 129 post per tile |
| `Plugins/GeoWorld/Source/GeoTiles/Public/Tiles/TilingScheme.h:51` | `GetTileBounds`: il rettangolo geografico di una tile |
| `Plugins/GeoWorld/Source/GeoTiles/Public/Tiles/TileKey.h:31`, `:37`, `:72` | il padre, i figli, `Pack()` (la chiave di tutte le mappe) |
| `Plugins/GeoWorld/Source/GeoTiles/Public/Tiles/TileFormat.h:40` | `FHeightTile`: le quote |

**Da capire:** due tile vicine dello stesso livello condividono il post di
bordo, quindi fra loro non ci sono crepe. Le gonne servono solo fra livelli
diversi.

---

## 2. Lo streaming: dalla richiesta alla cache

| Dove | Cosa |
|---|---|
| `Plugins/GeoWorld/Source/GeoTiles/Private/Streaming/GeoTileStreamingSubsystem.cpp:297` | `RequestTile`: stati, priorità, il pool che legge dal disco |
| `Plugins/GeoWorld/Source/GeoTiles/Private/Streaming/GeoTileStreamingSubsystem.cpp:175` | `DrainCompletedLoads`: sul game thread le tile entrano in cache, come `shared_ptr` immutabili |

**In gioco:** `geo.Tiles.Debug 1`.

**Flash:** qui non c'è geometria. Conta solo una cosa: se le quote del
**padre** sono già state sfrattate dalla cache, la figlia nascerà senza
geomorphing (tappa 5).

---

## 3. Il quadtree: cosa va a schermo

| Dove | Cosa |
|---|---|
| `Plugins/GeoWorld/Source/GeoRender/Private/Lod/GeoQuadtreeSubsystem.cpp:160` | `RunSelection`; alla `:212` il log del teletrasporto, alla `:225` la chiamata al selettore |
| `Plugins/GeoWorld/Source/GeoRender/Public/Quadtree/TileSelector.h:136` | `SelectTiles`, l'attraversamento dell'albero |
| `Plugins/GeoWorld/Source/GeoRender/Public/Quadtree/TileSelector.h:255`–`283` | **la regola chiave**: si scende ai figli solo se sono pronti **tutti e quattro**, altrimenti resta a schermo il padre |
| `Plugins/GeoWorld/Source/GeoRender/Private/Lod/GeoQuadtreeSubsystem.cpp:55` | "pronta" non vuol dire "caricata": si chiede al terreno |
| `Plugins/GeoWorld/Source/GeoRender/Private/Terrain/GeoTerrainSubsystem.cpp:119` | la risposta del terreno: "ha la mesh **ed** è vestita" |

**Da verificare:** la readiness di una tile **già a schermo** può tornare
falsa? Per esempio se perde il drappeggio. Allora il padre tornerebbe di colpo
al posto delle figlie, senza dissolvenza: sarebbe un flash.

**In gioco:** `geo.Lod.Debug 1`; `geo.Lod.Draw 1` (un colore per livello);
`geo.Lod.Freeze 1` (congela la selezione e ti lascia muovere).

---

## 4. Il terreno: dalla selezione alla visibilità

È il cuore. `Plugins/GeoWorld/Source/GeoRender/Private/Terrain/GeoTerrainSubsystem.cpp:153`,
`SynchroniseWithSelection`:

| Riga | Cosa |
|---|---|
| `:193` | **Refinements**: chi compare al posto di un antenato che era a schermo. Va calcolato *prima* di cambiare le visibilità, che sono proprio lo stato del frame prima |
| `:213` | `SetBuiltVisible`: mostra e nasconde |
| `:225` | `BeginTransitionFromParent`: fa partire geomorphing e dissolvenza |
| `:233` | **Urgent**: tile selezionate ma senza mesh. Con la readiness attiva non dovrebbe succedere; se succede, compaiono **senza transizione** |

**Da capire:** solo il raffinamento è sfumato. Il contrario (le figlie che
tornano padre, allontanandosi) è voluto di colpo.

---

## 5. La costruzione sui thread di lavoro

| Dove | Cosa |
|---|---|
| `Plugins/GeoWorld/Source/GeoRender/Private/Terrain/GeoTerrainSubsystem.cpp:361` | `DispatchBuild`: tutto copiato nella lambda, niente `this` |
| `Plugins/GeoWorld/Source/GeoRender/Private/Terrain/GeoTerrainSubsystem.cpp:430` | `BuildTileMesh` |
| `Plugins/GeoWorld/Source/GeoRender/Private/Terrain/GeoTerrainSubsystem.cpp:433` | `ComputeMorphTargets`, solo se ci sono le quote del padre |
| `Plugins/GeoWorld/Source/GeoRender/Public/Mesh/TileMesh.h:188` | la mesh: i vertici stanno nel **frame locale della tile** (NEU, in metri) |
| `Plugins/GeoWorld/Source/GeoRender/Public/Mesh/TileMesh.h:210` | l'origine del frame: il centro della tile |
| `Plugins/GeoWorld/Source/GeoRender/Public/Mesh/TileMesh.h:250` | da ECEF al frame locale |
| `Plugins/GeoWorld/Source/GeoRender/Public/Mesh/TileMesh.h:510` | i bersagli del geomorphing |
| `Plugins/GeoWorld/Source/GeoRender/Private/Terrain/DynamicMeshTerrainProvider.cpp:220` | `PrepareTileMesh`: UV1 e UV2 portano la normale e la quota del padre |

**Da capire:** il commento all'inizio di `TileMesh.h` spiega perché il rebase
non tocca i vertici: sono relativi al centro della loro tile.

**Flash possibile:** senza le quote del padre, `bHasMorph = false`, e la
figlia compare **senza** geomorphing
(`Plugins/GeoWorld/Source/GeoRender/Private/Terrain/DynamicMeshTerrainProvider.cpp:1048`,
il ramo che salta il morph). Un log lì dentro ti dice se capita.

---

## 6. La consegna al renderer

| Dove | Cosa |
|---|---|
| `Plugins/GeoWorld/Source/GeoRender/Private/Terrain/GeoTerrainSubsystem.cpp:449` | `CommitCompletedBuilds` |
| `Plugins/GeoWorld/Source/GeoRender/Private/Terrain/GeoTerrainSubsystem.cpp:480` | la trasformazione si calcola **adesso**, non quando il lavoro è partito: nel frattempo può esserci stato un rebase |
| `Plugins/GeoWorld/Source/GeoRender/Private/Terrain/DynamicMeshTerrainProvider.cpp:316` | `CommitPreparedTile`: il componente nasce alla `:335`, `SetMesh` alla `:368`, `SetWorldTransform` alla `:370` |
| `Plugins/GeoWorld/Source/GeoRender/Private/Terrain/GeoTerrainSubsystem.cpp:492` | una mesh costruita in anticipo si nasconde subito, nello stesso frame |
| `Plugins/GeoWorld/Source/GeoRender/Private/Terrain/DynamicMeshTerrainProvider.cpp:409`, `:445` | `SetVisibility`: un componente nascosto perde il proxy di scena; rimostrarlo lo ricrea a fine frame |

**Ipotesi:** un primitivo appena entrato nella scena può avere per qualche
frame un'illuminazione sbagliata (Lumen, cache delle ombre virtuali).

---

## 7. Vestire: foto e transizioni

| Dove | Cosa |
|---|---|
| `Plugins/GeoWorld/Source/GeoRender/Private/Imagery/GeoImagerySubsystem.cpp:184` | `SynchroniseWithTerrain`: se il budget del frame è finito, ripiega sulla foto di un antenato |
| `Plugins/GeoWorld/Source/GeoRender/Private/Imagery/GeoImagerySubsystem.cpp:142` | il predicato "vestita" |
| `Plugins/GeoWorld/Source/GeoRender/Private/Terrain/DynamicMeshTerrainProvider.cpp:569` | `SetTileDrape`; alla `:629` la dissolvenza quando una tile a schermo cambia foto |
| `Plugins/GeoWorld/Source/GeoRender/Private/Terrain/DynamicMeshTerrainProvider.cpp:1091` | `TickTransitions` |
| `Plugins/GeoWorld/Source/GeoRender/Private/Terrain/DynamicMeshTerrainProvider.cpp:957` | `WriteTransitionData`: Custom Primitive Data 0 = morph, 1 = dissolvenza |

**In gioco:** `geo.Terrain.Debug 1` (la riga **Transizioni**);
`geo.Terrain.Morph 0` contro `0.6`; `geo.Imagery.Checker 1`.

---

## 8. Il rebase

`Plugins/GeoWorld/Source/GeoCore/Private/Georeference/GeoreferenceSubsystem.cpp`:

| Riga | Cosa |
|---|---|
| `:126` | `Tick`; alla `:170` la soglia, sulla distanza **orizzontale** |
| `:189` | `ApplyRebase` |
| `:216` | l'origine nuova sta sempre a quota zero (il perché: `docs/strade-3d.md`, sezione 6) |
| `:295` | nuova origine, e `Generation` che cresce |
| `:325` | la camera torna dove stava geograficamente |
| `:340`, `:341` | i componenti registrati, poi il broadcast `OnGeoreferenceRebased` |
| `:412` | `CameraManagerLocation`: la correzione della cache della camera |

Chi riceve il broadcast:

- il terreno: `Plugins/GeoWorld/Source/GeoRender/Private/Terrain/GeoTerrainSubsystem.cpp:571`,
  che porta a `Plugins/GeoWorld/Source/GeoRender/Private/Terrain/DynamicMeshTerrainProvider.cpp:469`,
  `RefreshTransforms` (tutte le tile, anche le nascoste, più strade e acqua);
- l'acqua (`GeoWaterSubsystem.cpp:84`) e il meteo (`GeoWeatherSubsystem.cpp:84`).

`Plugins/GeoWorld/Source/GeoCore/Public/Georeference/GeoreferenceSnapshot.h:129`,
`GetLocalNeuTransform`: è la sola cosa che cambia per una tile.

**Da capire:** mondo e camera si spostano entrambi di −Δ, quindi a schermo
non dovrebbe cambiare niente.

**Ipotesi su cosa può comunque lampeggiare:**

- **Oggetti del livello senza georeferenza**: luci, nebbia, actor messi a
  mano. Non vengono spostati.
- **Le cache del renderer**: tutto il mondo si sposta di 10 km in un frame.
  Lumen, le ombre virtuali e la storia del TSR/TAA potrebbero dover
  riconvergere. Unreal ha un rebase suo (`UWorld::SetNewWorldOrigin`) che
  avvisa il renderer; noi spostiamo i componenti uno per uno, senza dirglielo.
- **Un picco di costo**: migliaia di `SetWorldTransform` (terreno, strade,
  acqua, nascoste comprese) nello stesso frame. Spiegherebbe il lag.

---

## Esperimenti per isolare il flash, in quest'ordine

1. **`geo.AutoRebase 0`** e vola. Se il flash sparisce, è il rebase: vai al
   punto 4. Se resta, è il LOD.
2. **`geo.Lod.Freeze 1`** e muoviti. Se il flash resta con la selezione
   congelata, non è il LOD.
3. **`geo.Terrain.Morph 0`** contro **`0.6`**, guardando la riga Transizioni
   nell'overlay del terreno (`geo.Terrain.Debug 1`).
4. **Da fermo, `geo.Rebase` più volte.** Se lampeggia anche da fermo, ripetilo
   con uno per volta di questi comandi, e guarda quale lo toglie:
   - `r.Lumen.DiffuseIndirect.Allow 0`
   - `r.Shadow.Virtual.Enable 0`
   - `r.AntiAliasingMethod 0`
5. **`stat unit`** durante il rebase: se il game thread ha un picco, è il
   costo dei `SetWorldTransform`.

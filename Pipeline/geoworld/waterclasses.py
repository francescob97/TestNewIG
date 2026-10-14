"""
Da tag OpenStreetMap a corpo d'acqua (Fase 9): cosa e', quanto e' profondo,
quanto si muove.

Un corpo d'acqua ha un TIPO (mare, laguna, lago, fiume, canale) e dal tipo
discendono le regole che il runtime vedra' come numeri nella tile:

  - la SUPERFICIE: il geoide per mare e laguna, una quota sola per un lago, un
    profilo in discesa per fiumi e canali (vedi waterbuild.py);
  - il FONDALE: quanto scavare il terreno sotto l'acqua, con una rampa dalla
    riva (depth_at) fino a una profondita' massima (max_depth);
  - l'ESPOSIZIONE alle onde: il mare aperto 1, un laghetto quasi niente
    (wave_exposure). In meteorologia si chiama "fetch": le onde crescono con
    la distanza su cui il vento soffia sull'acqua;
  - la CORRENTE: i fiumi scorrono nel verso della loro linea in OSM (regola del
    tag `waterway`: le way sono disegnate verso valle).

Tutto qui, in un punto solo, come roadclasses.py per le strade.
"""

from __future__ import annotations

import math
from dataclasses import dataclass

import numpy as np

#: Versione delle regole. Entra nell'impronta della pipeline: se cambia, le
#: tile si rifanno. Va nel manifest ("rulesVersion"), e il motore la
#: confronta con WaterRulesVersion (WaterTileFormat.h): un dataset fatto con
#: regole piu' vecchie lo dice in rosso. Cambiandola, cambia anche quella.
WATER_RULES_VERSION = 2

KIND_SEA = 1
KIND_LAGOON = 2
KIND_LAKE = 3
KIND_RIVER = 4
KIND_CANAL = 5

KIND_NAMES = {KIND_SEA: "mare", KIND_LAGOON: "laguna", KIND_LAKE: "lago",
              KIND_RIVER: "fiume", KIND_CANAL: "canale"}

#: Flag di una tile (header e indice). Stessi valori in WaterTileFormat.h.
TILE_SEA = 1 << 0
TILE_LAKE = 1 << 1
TILE_RIVER = 1 << 2
TILE_FLOW = 1 << 3
TILE_ALL_WATER = 1 << 4

KIND_TILE_FLAG = {KIND_SEA: TILE_SEA, KIND_LAGOON: TILE_SEA, KIND_LAKE: TILE_LAKE,
                  KIND_RIVER: TILE_RIVER, KIND_CANAL: TILE_RIVER}

#: Larghezza tipica di una linea d'acqua, in metri, quando OSM non la dice.
DEFAULT_LINE_WIDTH_M = {KIND_RIVER: 12.0, KIND_CANAL: 8.0}

#: Sotto questa larghezza un corso d'acqua resta la foto: a 10 m per post non
#: avrebbe neanche un post dentro, e sarebbe un nastro che appare e scompare.
MIN_LINE_WIDTH_M = 4.0

#: Velocita' della corrente in m/s (il raster la porta normalizzata a 1 m/s).
FLOW_SPEED = {KIND_RIVER: 1.0, KIND_CANAL: 0.35}

_LAKE_WATER = {"lake", "pond", "reservoir", "oxbow", "lock", "basin", "fishpond",
               "natural", "yes"}
_RIVER_WATER = {"river", "stream", "canal", "ditch", "rapids", "moat"}
_SKIP_WATER = {"wastewater", "swimming_pool", "pool", "fountain", "reflecting_pool"}
_LINE_KINDS = {"river": KIND_RIVER, "canal": KIND_CANAL, "stream": KIND_RIVER}


@dataclass(frozen=True)
class WaterArea:
    kind: int


@dataclass(frozen=True)
class WaterLine:
    kind: int
    width_m: float


def _invisible(tags: dict[str, str]) -> bool:
    """Acqua che dal cielo non si vede, o che non c'e' quasi mai."""
    return (tags.get("intermittent") == "yes" or tags.get("seasonal") == "yes"
            or tags.get("tunnel") not in (None, "no") or tags.get("covered") == "yes"
            or tags.get("location") in ("underground", "indoor")
            or tags.get("indoor") == "yes")


def classify_area(tags: dict[str, str]) -> WaterArea | None:
    """Un poligono d'acqua di OSM, o None se non va disegnato."""
    if _invisible(tags):
        return None
    water = tags.get("water")
    if water in _SKIP_WATER or tags.get("leisure") == "swimming_pool" \
            or tags.get("amenity") == "fountain":
        return None

    natural = tags.get("natural")
    if tags.get("waterway") == "riverbank":
        return WaterArea(KIND_RIVER)
    if natural == "water" or water is not None or tags.get("landuse") == "reservoir":
        if natural not in (None, "water") and water is None:
            return None
        if water == "lagoon":
            return WaterArea(KIND_LAGOON)
        if water in _RIVER_WATER:
            return WaterArea(KIND_CANAL if water in ("canal", "ditch", "moat") else KIND_RIVER)
        if water is None or water in _LAKE_WATER:
            return WaterArea(KIND_LAKE)
        return WaterArea(KIND_LAKE)
    return None


def parse_width(value: str | None) -> float | None:
    """`width` di OSM in metri: "12", "12 m", "12.5", "40 ft" (raro)."""
    if not value:
        return None
    text = value.strip().lower().replace(",", ".")
    factor = 1.0
    if text.endswith("ft"):
        factor, text = 0.3048, text[:-2]
    elif text.endswith("m"):
        text = text[:-1]
    try:
        width = float(text.strip()) * factor
    except ValueError:
        return None
    return width if 0.0 < width < 5000.0 else None


def classify_line(tags: dict[str, str]) -> WaterLine | None:
    """Una linea d'acqua di OSM (fiume, canale, torrente largo), o None."""
    if _invisible(tags):
        return None
    waterway = tags.get("waterway")
    kind = _LINE_KINDS.get(waterway)
    if kind is None:
        return None
    width = parse_width(tags.get("width"))
    if waterway == "stream":
        # I torrenti solo se OSM dice che sono larghi: la maggior parte e' un
        # fosso che dal cielo non si vede.
        if width is None or width < MIN_LINE_WIDTH_M:
            return None
    if width is None:
        width = DEFAULT_LINE_WIDTH_M[kind]
    if width < MIN_LINE_WIDTH_M:
        return None
    return WaterLine(kind, min(width, 400.0))


# =============================================================================
#  Le regole numeriche (sezione 3 di docs/fase9-design.md)
# =============================================================================

def wave_exposure(kind: int, area_km2: float = 0.0) -> float:
    """
    Quanto il vento fa onde qui, 0..1.

    Per un lago si usa la radice dell'area come misura della distanza su cui il
    vento soffia sull'acqua (il "fetch"): un lago di 1 km² ha onde da laghetto,
    il Garda (370 km²) quasi da mare. 15 km di fetch sono il "quasi mare".
    """
    if kind == KIND_SEA:
        return 1.0
    if kind == KIND_LAGOON:
        return 0.35
    if kind == KIND_LAKE:
        return float(min(0.8, max(0.12, math.sqrt(max(area_km2, 0.0)) / 15.0)))
    if kind == KIND_RIVER:
        return 0.06
    return 0.03


def max_depth(kind: int, area_km2: float = 0.0, width_m: float = 0.0) -> float:
    """Profondita' massima del fondale inventato, in metri (senza batimetria)."""
    if kind == KIND_SEA:
        return 60.0
    if kind == KIND_LAGOON:
        return 2.0
    if kind == KIND_LAKE:
        return float(min(120.0, max(5.0, 5.0 + 6.0 * math.sqrt(max(area_km2, 0.0)))))
    if kind == KIND_RIVER:
        return float(min(5.0, max(1.0, width_m / 15.0)))
    return 2.5


#: (profondita' sulla riva, pendenza) della rampa del fondale, per tipo.
_RAMP = {KIND_SEA: (0.5, 0.05), KIND_LAGOON: (0.3, 0.05), KIND_LAKE: (0.5, 0.12),
         KIND_RIVER: (0.3, 0.25), KIND_CANAL: (0.3, 0.5)}


def ramp(kind: int) -> tuple[float, float]:
    return _RAMP[kind]


#: La pendenza della SPONDA, ai due lati della riva (vedi waterbuild.cut_tile):
#: fuori il terreno vicino all'acqua non sale piu' ripido di cosi', dentro il
#: fondale scende almeno cosi' vicino alla riva. Stessa pendenza dai due lati:
#: allora i triangoli del terreno incrociano l'acqua proprio sulla riva, a
#: qualunque risoluzione della mesh. Spiagge dolci per il mare, argini per i
#: fiumi.
_SHORE_SLOPE = {KIND_SEA: 0.15, KIND_LAGOON: 0.15, KIND_LAKE: 0.3, KIND_RIVER: 0.5, KIND_CANAL: 0.5}


def shore_slope(kind: int) -> float:
    return _SHORE_SLOPE[kind]


#: Un'acqua si SCAVA a un livello solo se e' larga almeno cosi' tanti post:
#: piu' stretta, la mesh del terreno (a passo 2, una cella ogni due post) non
#: ha vertici dentro e lo scavo diventa un pettine di buchi. Allora niente
#: scavo, e l'acqua si appoggia sul terreno (WaterMesh.h).
RESOLVE_POSTS = 4.0

#: Fin dove, fuori dalla riva, la sponda abbassa il terreno (post): una cella
#: della mesh a passo 2 con un po' di margine.
SHORE_BAND_POSTS = 4.0

#: Fino a quanti post dalla riva il tetto della sponda segue la "V"; oltre sale
#: a 45° (i vertici piu' lontani non toccano le celle a cavallo della riva).
SHORE_FLAT_POSTS = 1.5

#: Fin dove, dentro, la sponda puo' fare il fondale piu' ripido della rampa
#: (post): piu' in la' basta la rampa, e il mare non diventa un pozzo.
SHORE_REACH_POSTS = 3.0


def depth_at(distance_m, shore_depth, slope, deepest):
    """
    Profondita' a `distance_m` metri dalla riva: una rampa che parte da qualche
    decimetro (la riva deve stare SOTTO l'acqua, non coincidere con lei:
    altrimenti terreno e superficie si contendono i pixel) e si ferma a
    `deepest`. Tutti gli argomenti possono essere array numpy della stessa
    forma: i parametri arrivano pixel per pixel, dal corpo d'acqua di quel
    pixel.
    """
    distance = np.maximum(np.asarray(distance_m, dtype=np.float64), 0.0)
    return np.minimum(np.asarray(deepest, dtype=np.float64),
                      np.asarray(shore_depth, dtype=np.float64)
                      + np.asarray(slope, dtype=np.float64) * distance)


#: Distanza dalla riva oltre la quale la rampa ha gia' raggiunto il fondo per
#: qualunque tipo (60 m di mare a 5%: 1.190 m). Serve a sapere quanto "contesto"
#: guardare attorno a una tile per calcolare la distanza dalla riva.
MAX_RAMP_DISTANCE_M = 1200.0

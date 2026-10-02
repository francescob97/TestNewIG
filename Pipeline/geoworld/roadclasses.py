"""
Le classi delle linee disegnate sul terreno (Fase 8): strade, ferrovie, piste.

DA DOVE VENGONO
---------------
Da OpenStreetMap, dove una strada e' una "way" con dei tag: `highway=primary`,
`lanes=2`, `bridge=yes`, `surface=gravel`. Qui si decide, una volta sola e in un
punto solo, cosa diventa ognuna:

  - la CLASSE (che colore ha, sopra o sotto le altre, da che livello si vede);
  - la LARGHEZZA in metri (dal tag `width`, se c'e', poi da `lanes`, poi un
    valore tipico per la classe);
  - i FLAG: ponte, galleria, sterrato, rampa;
  - il LAYER: chi sta sopra a chi, per i ponti.

OGNI NUMERO DI CLASSE E' UN CONTRATTO
-------------------------------------
Il numero finisce nelle tile su disco e il runtime C++ lo usa per scegliere lo
stile (Source/GeoTiles/Public/Tiles/VectorClasses.h). I due elenchi devono
coincidere, e un test (tests/test_roads.py) legge l'header C++ e lo confronta
con questo file: cambiare un numero da una parte sola fa fallire i test, non
colorare le ferrovie come autostrade.

DA CHE LIVELLO SI VEDE UNA CLASSE
---------------------------------
Una tile vettoriale di livello basso copre decine di chilometri. Metterci i
sentieri vorrebbe dire milioni di punti per disegnare linee larghe un
centesimo di pixel. Ogni classe ha quindi un livello minimo: le autostrade ci
sono dal 10 (tile di ~20 km), i sentieri solo al 13 (tile di ~2 km).
Avvicinandosi le strade minori compaiono: e' lo stesso principio del LOD del
terreno, applicato alle linee.
"""

from __future__ import annotations

import math
import re
from dataclasses import dataclass

#: Versione della classificazione. Entra nell'impronta della pipeline: se
#: cambia, le tile si rifanno invece di mescolare classificazioni diverse.
CLASSIFICATION_VERSION = 1


@dataclass(frozen=True)
class RoadClass:
    id: int
    name: str
    #: Larghezza tipica di UNA way, in metri. Le autostrade in OSM sono quasi
    #: sempre due way, una per senso di marcia: 11 m e' una carreggiata.
    default_width_m: float
    #: Primo livello di tile vettoriale in cui la classe compare.
    min_level: int
    #: Ordine di disegno: piu' alto = sopra. A parita' di layer, l'autostrada
    #: copre la strada locale che le passa sotto, non il contrario.
    rank: int
    #: "strada", "ferrovia" o "pista": serve solo ai messaggi.
    kind: str


# Il numero (primo campo) e' il contratto con VectorClasses.h: NON rinumerare.
CLASSES = [
    RoadClass(1, "motorway", 11.0, 10, 90, "strada"),
    RoadClass(2, "trunk", 9.0, 10, 85, "strada"),
    RoadClass(3, "primary", 7.5, 11, 80, "strada"),
    RoadClass(4, "secondary", 7.0, 11, 70, "strada"),
    RoadClass(5, "tertiary", 6.0, 12, 60, "strada"),
    RoadClass(6, "unclassified", 5.0, 12, 50, "strada"),
    RoadClass(7, "residential", 5.5, 12, 50, "strada"),
    RoadClass(8, "living_street", 4.5, 13, 45, "strada"),
    RoadClass(9, "service", 3.5, 13, 40, "strada"),
    RoadClass(10, "pedestrian", 4.0, 13, 35, "strada"),
    RoadClass(11, "track", 3.0, 13, 30, "strada"),
    RoadClass(12, "path", 1.5, 13, 20, "strada"),
    RoadClass(20, "rail", 4.5, 10, 75, "ferrovia"),
    RoadClass(21, "light_rail", 3.5, 12, 65, "ferrovia"),
    RoadClass(22, "rail_service", 4.0, 13, 55, "ferrovia"),
    RoadClass(30, "runway", 45.0, 10, 95, "pista"),
    RoadClass(31, "taxiway", 20.0, 12, 92, "pista"),
]

BY_ID = {road_class.id: road_class for road_class in CLASSES}
BY_NAME = {road_class.name: road_class for road_class in CLASSES}

# --- Flag (un byte per linea; stessi valori in VectorClasses.h) -------------
FLAG_BRIDGE = 1 << 0
FLAG_TUNNEL = 1 << 1          # oggi le gallerie si scartano: il flag e' per dopo
FLAG_UNPAVED = 1 << 2
FLAG_LINK = 1 << 3            # rampa di svincolo (motorway_link e simili)

# --- Da tag OSM a classe --------------------------------------------------

_HIGHWAY = {
    "motorway": "motorway", "trunk": "trunk", "primary": "primary",
    "secondary": "secondary", "tertiary": "tertiary",
    "unclassified": "unclassified", "road": "unclassified",
    "residential": "residential", "living_street": "living_street",
    "service": "service", "pedestrian": "pedestrian", "track": "track",
    "footway": "path", "path": "path", "cycleway": "path",
    "bridleway": "path", "steps": "path",
}

_LINKS = {
    "motorway_link": "motorway", "trunk_link": "trunk", "primary_link": "primary",
    "secondary_link": "secondary", "tertiary_link": "tertiary",
}

_RAILWAY = {
    "rail": "rail", "preserved": "rail",
    "light_rail": "light_rail", "tram": "light_rail", "subway": "light_rail",
    "monorail": "light_rail", "narrow_gauge": "light_rail", "funicular": "light_rail",
}

_AEROWAY = {"runway": "runway", "stopway": "runway",
            "taxiway": "taxiway", "taxilane": "taxiway"}

#: Gallerie: non si vedono dal cielo. `building_passage` e' il sottopasso di un
#: palazzo, `culvert` il tombino sotto la strada (vale per i corsi d'acqua).
_TUNNELS = {"yes", "building_passage", "culvert", "avalanche_protector", "covered"}

_UNPAVED = {"unpaved", "gravel", "fine_gravel", "dirt", "ground", "earth", "grass",
            "sand", "mud", "compacted", "pebblestone", "woodchips", "rock", "grass_paver"}
_PAVED = {"paved", "asphalt", "concrete", "concrete:plates", "concrete:lanes",
          "paving_stones", "sett", "cobblestone", "unhewn_cobblestone", "chipseal",
          "metal", "wood"}

#: Larghezza tipica di una corsia, in metri. Le corsie italiane vanno da 2,75 m
#: (strade locali) a 3,75 m (autostrade); 3,25 e' un valore medio onesto.
LANE_WIDTH_M = 3.25

MIN_WIDTH_M = 1.0
MAX_WIDTH_M = 100.0

_NUMBER = re.compile(r"^\s*([0-9]+(?:[.,][0-9]+)?)\s*(m|metres|meters|metri)?\s*$", re.IGNORECASE)


@dataclass(frozen=True)
class Classified:
    class_id: int
    flags: int
    layer: int
    width_m: float

    @property
    def width_dm(self) -> int:
        """Larghezza in decimetri: e' cosi' che sta su disco (uint16)."""
        # Arrotondamento "da scuola" (0,5 in su), non quello del round() di
        # Python, che va al pari: 11,25 m devono essere 113 dm, non 112.
        return max(1, min(65535, int(math.floor(self.width_m * 10.0 + 0.5))))


def parse_width(text: str | None) -> float | None:
    """
    Il tag `width` di OSM in metri, o None se non e' un numero in metri.

    Si accettano "7", "7.5", "7,5", "7 m". Si scartano i piedi ("24'") e le
    cose come "2 lanes": meglio il valore tipico della classe che un numero
    interpretato male.
    """
    if not text:
        return None
    match = _NUMBER.match(text)
    if not match:
        return None
    value = float(match.group(1).replace(",", "."))
    if not math.isfinite(value) or value <= 0.0:
        return None
    return value


def parse_layer(text: str | None) -> int:
    """Il tag `layer` come intero fra -5 e 5 (0 se manca o e' strano)."""
    if not text:
        return 0
    try:
        return max(-5, min(5, int(float(text.strip().split(";")[0]))))
    except ValueError:
        return 0


def _lanes(text: str | None) -> int | None:
    if not text:
        return None
    try:
        lanes = int(float(text.strip().split(";")[0]))
    except ValueError:
        return None
    return lanes if 1 <= lanes <= 12 else None


def classify(tags: dict[str, str]) -> Classified | None:
    """
    Decide cosa diventa una way di OSM, o None se non va disegnata.

    Si scarta cio' che dal cielo non si vede (gallerie, strade al chiuso), cio'
    che non esiste (progetti, cantieri, ferrovie smantellate) e cio' che e'
    un'AREA e non una linea (piazze pedonali disegnate come poligono: arrivano
    con le altre aree, in una fase successiva).
    """
    if tags.get("area") == "yes" or tags.get("indoor") == "yes":
        return None
    if tags.get("tunnel") in _TUNNELS or tags.get("covered") == "yes":
        return None

    flags = 0
    name = None
    default_width = None

    highway = tags.get("highway")
    railway = tags.get("railway")
    aeroway = tags.get("aeroway")

    if highway in _LINKS:
        name = _LINKS[highway]
        flags |= FLAG_LINK
        # Una rampa e' una corsia piu' la banchina, non una carreggiata intera.
        default_width = 6.0 if name in ("motorway", "trunk") else 5.5
    elif highway in _HIGHWAY:
        name = _HIGHWAY[highway]
    elif railway in _RAILWAY:
        name = _RAILWAY[railway]
        # Binari di servizio (scali, raccordi): fitti e brevi, si vedono solo
        # da vicino.
        if name == "rail" and tags.get("service") in ("yard", "siding", "spur", "crossover"):
            name = "rail_service"
    elif aeroway in _AEROWAY:
        name = _AEROWAY[aeroway]
    else:
        return None

    road_class = BY_NAME[name]

    if tags.get("bridge") not in (None, "no"):
        flags |= FLAG_BRIDGE

    # Sterrato: dal tag `surface`; senza, i tratturi e i sentieri si assumono
    # sterrati, il resto asfaltato. `tracktype=grade1` e' una carrareccia
    # pavimentata.
    surface = tags.get("surface")
    if surface in _UNPAVED:
        flags |= FLAG_UNPAVED
    elif surface not in _PAVED and road_class.kind == "strada":
        if name == "track" and tags.get("tracktype") != "grade1":
            flags |= FLAG_UNPAVED
        elif name == "path" and highway in ("path", "bridleway"):
            flags |= FLAG_UNPAVED

    # Larghezza: il dato esplicito vince, poi le corsie, poi il valore tipico.
    width = parse_width(tags.get("width"))
    if width is None and road_class.kind == "strada":
        lanes = _lanes(tags.get("lanes"))
        if lanes is not None:
            shoulder = 1.5 if name in ("motorway", "trunk") and not flags & FLAG_LINK else 0.5
            width = lanes * LANE_WIDTH_M + shoulder
    if width is None:
        width = default_width if default_width is not None else road_class.default_width_m
    width = max(MIN_WIDTH_M, min(MAX_WIDTH_M, width))

    return Classified(road_class.id, flags, parse_layer(tags.get("layer")), width)


def draw_order_key(class_id: int, flags: int, layer: int) -> tuple[int, int, int]:
    """
    Ordine di disegno dentro una tile: prima chi sta sotto.

    Il layer viene prima di tutto (un ponte di una strada locale passa SOPRA
    l'autostrada); poi i ponti senza layer esplicito; poi la classe.
    """
    return (layer, 1 if flags & FLAG_BRIDGE else 0, BY_ID[class_id].rank)

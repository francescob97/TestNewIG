"""
Le classi degli edifici, e come si stimano altezza, tetto e colore dai tag OSM.

Da OpenStreetMap arrivano le SAGOME (il contorno di ogni edificio) con i tag:
`building=house`, `building:levels=3`, `height=18`, `roof:shape=hipped`,
`building:colour=#e8d8b0`... Quasi sempre manca qualcosa: in Italia l'altezza
c'e' su pochi edifici, il numero di piani su qualcuno in piu', il tetto quasi
mai. Qui si decide cosa si sa e cosa si stima, con i valori di default per
classe.

I NUMERI DELLE CLASSI E DEI TETTI SONO UN CONTRATTO con il C++
(GeoTiles/Public/Tiles/BuildingClasses.h): stanno nelle tile su disco. Un test
(tests/test_buildings.py) legge l'header C++ e confronta.
"""

from __future__ import annotations

import re
from dataclasses import dataclass

#: Altezza di un piano, m: quella tipica italiana (2,7 m di stanza + solaio).
LEVEL_HEIGHT_M = 3.0
#: Quanto si aggiunge al piano terra (ingressi, negozi): sopra i piani.
GROUND_EXTRA_M = 0.5


@dataclass(frozen=True)
class BuildingClass:
    id: int
    name: str                 # il nome dell'enum C++ (EBuildingClass)
    values: tuple             # i valori di building=* che diventano questa classe
    levels: int               # piani di default (0: si usa height_m)
    height_m: float           # altezza di default se levels e' 0
    description: str


CLASSES = [
    BuildingClass(0, "Other", ("yes",), 2, 0.0, "edificio generico (building=yes)"),
    BuildingClass(1, "House", ("house", "detached", "semidetached_house", "villa", "bungalow", "terrace",
                               "farm", "residential_house"), 2, 0.0, "case, villette, case a schiera"),
    BuildingClass(2, "Apartments", ("apartments", "residential", "dormitory", "flats"), 5, 0.0,
                  "condomini, palazzi residenziali"),
    BuildingClass(3, "Commercial", ("commercial", "office", "retail", "supermarket", "hotel", "shop", "mall",
                                    "bank"), 3, 0.0, "negozi, uffici, alberghi"),
    BuildingClass(4, "Industrial", ("industrial", "warehouse", "factory", "manufacture", "hangar", "depot",
                                    "storage"), 0, 8.0, "capannoni, magazzini, fabbriche"),
    BuildingClass(5, "Religious", ("church", "chapel", "cathedral", "basilica", "mosque", "synagogue", "temple",
                                   "religious", "monastery", "convent", "shrine"), 0, 14.0,
                  "chiese, cappelle, conventi"),
    BuildingClass(6, "Civic", ("public", "civic", "government", "school", "university", "college", "hospital",
                               "kindergarten", "train_station", "transportation", "townhall", "museum",
                               "sports_hall", "stadium", "fire_station", "police", "library"), 3, 0.0,
                  "scuole, ospedali, municipi, stazioni"),
    BuildingClass(7, "Agricultural", ("barn", "farm_auxiliary", "stable", "cowshed", "greenhouse", "sty",
                                      "silo", "agricultural"), 0, 6.0, "fienili, stalle, serre"),
    BuildingClass(8, "Small", ("garage", "garages", "shed", "hut", "cabin", "carport", "toilets", "service",
                               "kiosk", "container", "bunker"), 0, 3.0, "box, tettoie chiuse, baracche"),
    BuildingClass(9, "Roof", ("roof", "canopy"), 0, 4.0, "pensiline: un tetto senza muri"),
    BuildingClass(10, "Tower", ("tower", "bell_tower", "campanile", "minaret"), 0, 25.0, "torri, campanili"),
    BuildingClass(11, "Historic", ("castle", "palace", "fortress", "fort", "ruins", "city_wall"), 3, 0.0,
                  "castelli, palazzi storici, rovine"),
]

BY_NAME = {item.name: item for item in CLASSES}
_BY_VALUE = {value: item for item in CLASSES for value in item.values}

#: Le forme del tetto. 0 = "decide il runtime" (dalla classe e dalla sagoma).
ROOF_SHAPES = {
    "Auto": 0, "Flat": 1, "Gabled": 2, "Hipped": 3, "Pyramidal": 4, "Dome": 5, "Skillion": 6,
}
#: roof:shape di OSM -> la nostra forma (quelle che non sappiamo fare diventano la piu' simile).
_ROOF_TAG = {
    "flat": "Flat", "gabled": "Gabled", "hipped": "Hipped", "pyramidal": "Pyramidal", "dome": "Dome",
    "skillion": "Skillion", "half-hipped": "Hipped", "half_hipped": "Hipped", "gambrel": "Gabled",
    "mansard": "Hipped", "round": "Dome", "onion": "Dome", "cone": "Pyramidal", "saltbox": "Gabled",
    "side_hipped": "Hipped", "crosspitched": "Hipped", "quadruple_saltbox": "Hipped",
}

#: Flag di un edificio (record della tile): stessi valori di BuildingClasses.h.
FLAG_WALL_COLOUR = 1 << 0
FLAG_ROOF_COLOUR = 1 << 1
FLAG_PART = 1 << 2
FLAG_HEIGHT_MEASURED = 1 << 3     # l'altezza viene da height= (comprende il tetto)
FLAG_LEVELS_MEASURED = 1 << 4     # i piani vengono da building:levels=
#: Un contorno che ha le sue parti (building:part): non si disegna (si disegnano
#: le parti, come vuole lo schema), resta solo per i monumenti della libreria.
FLAG_OUTLINE = 1 << 5

_NUMBER = re.compile(r"[-+]?\d+(?:[.,]\d+)?")

_COLOURS = {
    "white": (245, 245, 240), "black": (30, 30, 30), "grey": (150, 150, 150), "gray": (150, 150, 150),
    "lightgrey": (200, 200, 200), "lightgray": (200, 200, 200), "darkgrey": (90, 90, 90),
    "darkgray": (90, 90, 90), "silver": (190, 190, 195), "red": (170, 60, 50), "darkred": (120, 40, 35),
    "maroon": (110, 40, 35), "brown": (125, 85, 60), "tan": (205, 175, 135), "beige": (225, 205, 165),
    "cream": (240, 228, 195), "yellow": (230, 200, 110), "orange": (215, 130, 70), "pink": (225, 170, 160),
    "salmon": (230, 150, 125), "green": (90, 130, 80), "blue": (80, 110, 160), "terracotta": (185, 95, 65),
    "ochre": (205, 150, 80), "sand": (215, 195, 150),
}


def classify(tags: dict) -> BuildingClass | None:
    """La classe di un edificio (o di una parte), o None se non e' un edificio da disegnare."""
    value = (tags.get("building") or "").strip().lower()
    part = (tags.get("building:part") or "").strip().lower()
    if value in ("no", "none", "entrance", "construction", "proposed", "demolished", "abandoned:yes") and not part:
        return None
    if (tags.get("location") or "").lower() in ("underground", "indoor"):
        return None
    if part == "no":
        return None
    item = _BY_VALUE.get(value)
    # building=yes (o un valore che non conosciamo) dice solo "e' un edificio":
    # la classe la puo' dire la parte, o un altro tag.
    if item is None or item.name == "Other":
        item = _BY_VALUE.get(part) if part and part != "yes" else None
        if item is None and (tags.get("amenity") or "").lower() == "place_of_worship":
            item = BY_NAME["Religious"]
        if item is None and (tags.get("man_made") or "").lower() in ("tower", "bell_tower"):
            item = BY_NAME["Tower"]
    return item or BY_NAME["Other"]


def parse_length_m(text: str | None) -> float | None:
    """'12', '12 m', '12,5m', "40'" (piedi) -> metri; None se non si capisce."""
    if not text:
        return None
    match = _NUMBER.search(text)
    if match is None:
        return None
    value = float(match.group(0).replace(",", "."))
    lowered = text.lower()
    if "'" in lowered or "ft" in lowered or "feet" in lowered:
        value *= 0.3048
    if not (0.0 <= value <= 1000.0):
        return None
    return value


def parse_count(text: str | None) -> int | None:
    """'3', '3.5', '3;4' -> 3 (il primo numero, arrotondato); None se non si capisce."""
    if not text:
        return None
    match = _NUMBER.search(text)
    if match is None:
        return None
    value = float(match.group(0).replace(",", "."))
    if not (0.0 <= value <= 200.0):
        return None
    return int(round(value))


def parse_colour(text: str | None) -> tuple[int, int, int] | None:
    """'#e8d8b0', '#fc8', 'white', 'light grey' -> (r, g, b); None se non si capisce."""
    if not text:
        return None
    value = text.strip().lower().replace(" ", "").replace("_", "")
    if value.startswith("#"):
        digits = value[1:]
        if len(digits) == 3 and all(c in "0123456789abcdef" for c in digits):
            return tuple(int(c * 2, 16) for c in digits)
        if len(digits) == 6 and all(c in "0123456789abcdef" for c in digits):
            return tuple(int(digits[i:i + 2], 16) for i in (0, 2, 4))
        return None
    return _COLOURS.get(value)


@dataclass
class Estimate:
    height_m: float          # dal suolo alla cima (tetto compreso se FLAG_HEIGHT_MEASURED)
    min_height_m: float      # dove comincia (parti sospese, pensiline)
    roof_height_m: float     # 0 = lo decide il runtime dalla sagoma
    levels: int
    roof_shape: int
    flags: int
    wall_colour: tuple[int, int, int]
    roof_colour: tuple[int, int, int]


def estimate(tags: dict, item: BuildingClass, is_part: bool = False) -> Estimate:
    """
    Altezza, tetto e colori di un edificio dai suoi tag, con i default della classe.

    L'ALTEZZA: height= se c'e' (e' la cima, tetto compreso); altrimenti i piani
    (building:levels) x 3 m + 0,5 m di piano terra, con sopra il tetto. Senza
    nemmeno i piani, quelli di default della classe (una casa 2, un condominio
    5), o l'altezza di default (un capannone 8 m, una chiesa 14).
    """
    flags = FLAG_PART if is_part else 0
    levels = parse_count(tags.get("building:levels"))
    if levels is not None:
        flags |= FLAG_LEVELS_MEASURED
    min_level = parse_count(tags.get("building:min_level")) or 0

    roof_height = parse_length_m(tags.get("roof:height")) or 0.0
    roof_levels = parse_count(tags.get("roof:levels"))
    if not roof_height and roof_levels:
        roof_height = roof_levels * 2.6

    height = parse_length_m(tags.get("height")) or parse_length_m(tags.get("building:height"))
    if height is not None and height > 0.0:
        flags |= FLAG_HEIGHT_MEASURED
        if levels is None:
            levels = max(1, int(round((height - roof_height) / LEVEL_HEIGHT_M)))
    else:
        if levels is None:
            levels = item.levels
        if levels > 0:
            height = levels * LEVEL_HEIGHT_M + GROUND_EXTRA_M + roof_height
        else:
            height = item.height_m + roof_height
            levels = max(1, int(round(item.height_m / LEVEL_HEIGHT_M)))

    min_height = parse_length_m(tags.get("min_height"))
    if min_height is None:
        min_height = min_level * LEVEL_HEIGHT_M if min_level else 0.0
    if item.name == "Roof" and min_height == 0.0:
        # Una pensilina: il tetto sta in alto, sotto c'e' il vuoto.
        min_height = max(0.0, height - 0.6)
    height = max(height, min_height + 1.0)

    roof_name = _ROOF_TAG.get((tags.get("roof:shape") or "").strip().lower(), "Auto")
    wall = parse_colour(tags.get("building:colour") or tags.get("colour"))
    roof = parse_colour(tags.get("roof:colour"))
    if wall is not None:
        flags |= FLAG_WALL_COLOUR
    if roof is not None:
        flags |= FLAG_ROOF_COLOUR
    return Estimate(height_m=height, min_height_m=min_height, roof_height_m=roof_height,
                    levels=min(255, max(0, levels)), roof_shape=ROOF_SHAPES[roof_name], flags=flags,
                    wall_colour=wall or (0, 0, 0), roof_colour=roof or (0, 0, 0))

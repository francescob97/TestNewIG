"""
Le classi della copertura del suolo, per la vegetazione.

Vengono da ESA WorldCover (10 m, da Sentinel-1 e Sentinel-2: gratuito e senza
account), che ha i suoi codici (10 alberi, 20 arbusti, ...). Nelle tile
scriviamo i NOSTRI numeri, piccoli e consecutivi.

I NUMERI SONO UN CONTRATTO con il C++ (GeoTiles/Public/Tiles/VegetationClasses.h):
stanno nelle tile su disco. Un test (tests/test_vegetation.py) legge l'header
C++ e confronta ogni numero con questa tabella.
"""

from __future__ import annotations

from dataclasses import dataclass

import numpy as np


@dataclass(frozen=True)
class LandCover:
    id: int
    name: str                 # il nome dell'enum C++ (ELandCover)
    worldcover: tuple         # i codici WorldCover che diventano questa classe
    description: str


CLASSES = [
    LandCover(0, "NoData", (0,), "fuori dalla mappa, o la mappa non lo sa"),
    LandCover(1, "Trees", (10,), "copertura arborea: boschi, parchi, filari fitti"),
    LandCover(2, "Shrubs", (20,), "arbusteti, macchia"),
    LandCover(3, "Grass", (30,), "prati, pascoli"),
    LandCover(4, "Crops", (40,), "campi coltivati"),
    LandCover(5, "Built", (50,), "costruito: citta', paesi, strade larghe"),
    LandCover(6, "Bare", (60,), "roccia, sabbia, suolo nudo"),
    LandCover(7, "Snow", (70,), "neve e ghiacciai perenni"),
    LandCover(8, "Water", (80,), "acqua"),
    LandCover(9, "Wetland", (90,), "zone umide con erba"),
    LandCover(10, "Mangrove", (95,), "mangrovie"),
    LandCover(11, "Moss", (100,), "muschi e licheni"),
]

BY_NAME = {cover.name: cover for cover in CLASSES}

#: Flag della tile (header e indice): stessi valori di VegetationTileFormat.h.
TILE_TREES = 1 << 0
TILE_SHRUBS = 1 << 1

#: Le classi su cui cresce qualcosa (le probabilita' sono nel C++: TreePlacement.h,
#: PlantsFor). Una tile senza nessuna di queste non si scrive: mare aperto,
#: ghiacciai, deserti di roccia.
GROWING = tuple(BY_NAME[name].id for name in ("Trees", "Shrubs", "Grass", "Crops", "Built", "Wetland", "Mangrove"))


def worldcover_lookup() -> np.ndarray:
    """Tabella di 256 voci: codice WorldCover -> la nostra classe (NoData per i codici sconosciuti)."""
    table = np.zeros(256, dtype=np.uint8)
    for cover in CLASSES:
        for code in cover.worldcover:
            table[code] = cover.id
    return table


def tile_flags(classes: np.ndarray) -> int:
    """Cosa c'e' in una tile: bosco (alberi, mangrovie), arbusti."""
    flags = 0
    if np.any((classes == BY_NAME["Trees"].id) | (classes == BY_NAME["Mangrove"].id)):
        flags |= TILE_TREES
    if np.any(classes == BY_NAME["Shrubs"].id):
        flags |= TILE_SHRUBS
    return flags


def grows_anything(classes: np.ndarray) -> bool:
    return bool(np.isin(classes, GROWING).any())

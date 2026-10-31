"""
Formato delle tile di edifici (.gbd), con indice e manifest.

Una tile di edifici elenca le SAGOME degli edifici il cui centro cade nella
tile, con quello che serve a costruirli: altezza, piani, tetto, colori, l'id
OSM (per i monumenti della libreria). Tutte le tile stanno a UN livello, 15 di
default (~610 x 430 m a 45°). Il runtime ne fa le mesh (BuildingMesh.h).

    header   32 byte: "GWBD", versione, riservato, livello, x, y, numero di
             edifici, byte del payload compresso, byte del payload decompresso
             (la zlib di Unreal vuole sapere prima quanto spazio serve)
    payload  zlib di: gli edifici uno dopo l'altro, ognuno

        0  uint8   classe (bldclasses.py, BuildingClasses.h)
        1  uint8   tetto (0 = lo decide il runtime)
        2  uint8   flag
        3  uint8   piani
        4  uint16  altezza, dm (dal suolo alla cima)
        6  uint16  quota di partenza, dm (min_height)
        8  uint16  altezza del tetto, dm (0 = dalla sagoma)
       10  3 x uint8  colore dei muri (se il flag lo dice)
       13  3 x uint8  colore del tetto (se il flag lo dice)
       16  uint64  id OSM: tipo nei 2 bit alti (1 way, 2 relazione), numero sotto
       24  uint32  Wikidata (Q123 -> 123; 0 = nessuno)
       28  uint16  anelli: il primo e' il contorno, gli altri i cortili
       30  per anello: uint16 vertici, poi i vertici (int16 x, int16 y)

I VERTICI sono frazioni della tile x 16384, x verso EST dall'angolo sud-ovest,
y verso NORD: un passo e' ~4 cm. Il contorno gira in senso antiorario (visto
dall'alto), i cortili in senso orario; il primo vertice non si ripete in fondo.
Si va da -2 a +2 tile: un edificio grande che sporge dalla sua tile ci sta.
"""

from __future__ import annotations

import json
import os
import struct
import zlib
from dataclasses import dataclass, field

import numpy as np

from . import bldclasses, tiling

TILE_EXTENSION = ".gbd"
MAGIC = b"GWBD"
FORMAT_VERSION = 1
HEADER = struct.Struct("<4sHHIIIIII")        # 32 byte
RECORD = struct.Struct("<BBBBHHH3s3sQIH")   # 30 byte, senza gli anelli
QUANTUM = 16384

INDEX_MAGIC = b"GWBI"
INDEX_VERSION = 1
INDEX_HEADER = struct.Struct("<4sHHII")      # magic, versione, riservato, livello, numero
INDEX_RECORD = struct.Struct("<IIII")        # x, y, edifici, byte del file
INDEX_FILENAME = "index.bin"
MANIFEST_FILENAME = "manifest.json"

OSM_WAY = 1
OSM_RELATION = 2


@dataclass
class Building:
    building_class: int
    roof: int
    flags: int
    levels: int
    height_dm: int
    min_height_dm: int
    roof_height_dm: int
    wall_colour: tuple
    roof_colour: tuple
    osm_id: int                    # con il tipo nei 2 bit alti
    wikidata: int
    rings: list = field(default_factory=list)   # array int16 (N, 2), il primo e' il contorno


@dataclass
class BuildingTile:
    level: int
    x: int
    y: int
    buildings: list


@dataclass
class BuildingTileEntry:
    x: int
    y: int
    count: int
    size: int


def tile_path(root: str, level: int, x: int, y: int) -> str:
    return os.path.join(root, str(level), str(x), f"{y}{TILE_EXTENSION}")


def pack_osm_id(kind: int, number: int) -> int:
    return (kind << 62) | (number & ((1 << 62) - 1))


def quantize(level: int, x: int, y: int, lon: np.ndarray, lat: np.ndarray) -> np.ndarray | None:
    """Coordinate lon/lat -> int16 nella tile; None se escono dal campo (-2..+2 tile)."""
    bounds = tiling.tile_bounds(level, x, y)
    span = tiling.tile_span_deg(level)
    qx = np.round((lon - bounds.west) / span * QUANTUM)
    qy = np.round((lat - bounds.south) / span * QUANTUM)
    if qx.min() < -32768 or qx.max() > 32767 or qy.min() < -32768 or qy.max() > 32767:
        return None
    return np.stack([qx, qy], axis=1).astype(np.int16)


def dequantize(level: int, x: int, y: int, ring: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    bounds = tiling.tile_bounds(level, x, y)
    span = tiling.tile_span_deg(level)
    ring = ring.astype(np.float64)
    return bounds.west + ring[:, 0] / QUANTUM * span, bounds.south + ring[:, 1] / QUANTUM * span


def encode_building(building: Building) -> bytes:
    parts = [RECORD.pack(building.building_class, building.roof, building.flags, building.levels,
                         min(65535, building.height_dm), min(65535, building.min_height_dm),
                         min(65535, building.roof_height_dm), bytes(building.wall_colour),
                         bytes(building.roof_colour), building.osm_id, building.wikidata,
                         len(building.rings))]
    for ring in building.rings:
        ring = np.ascontiguousarray(ring, dtype="<i2")
        parts.append(struct.pack("<H", len(ring)))
        parts.append(ring.tobytes())
    return b"".join(parts)


def encode_tile(tile: BuildingTile) -> bytes:
    plain = b"".join(encode_building(building) for building in tile.buildings)
    payload = zlib.compress(plain, 6)
    header = HEADER.pack(MAGIC, FORMAT_VERSION, 0, tile.level, tile.x, tile.y, len(tile.buildings),
                         len(payload), len(plain))
    return header + payload


def decode_tile(blob: bytes) -> BuildingTile:
    if len(blob) < HEADER.size:
        raise ValueError("file troppo corto per l'header")
    magic, version, _, level, x, y, count, payload_bytes, plain_bytes = HEADER.unpack_from(blob)
    if magic != MAGIC:
        raise ValueError("non e' una tile di edifici")
    if version != FORMAT_VERSION:
        raise ValueError(f"versione {version} inattesa")
    if len(blob) != HEADER.size + payload_bytes:
        raise ValueError("dimensione incoerente con l'header (troncato?)")
    plain = zlib.decompress(blob[HEADER.size:])
    if len(plain) != plain_bytes:
        raise ValueError(f"payload di {len(plain)} byte, l'header ne dice {plain_bytes}")
    buildings, offset = [], 0
    for _ in range(count):
        if offset + RECORD.size > len(plain):
            raise ValueError("payload troncato")
        (cls, roof, flags, levels, height, minimum, roof_height, wall, roof_colour,
         osm_id, wikidata, ring_count) = RECORD.unpack_from(plain, offset)
        offset += RECORD.size
        rings = []
        for _ in range(ring_count):
            (vertices,) = struct.unpack_from("<H", plain, offset)
            offset += 2
            end = offset + vertices * 4
            if end > len(plain):
                raise ValueError("anello troncato")
            rings.append(np.frombuffer(plain[offset:end], dtype="<i2").reshape(vertices, 2).copy())
            offset = end
        buildings.append(Building(cls, roof, flags, levels, height, minimum, roof_height, tuple(wall),
                                  tuple(roof_colour), osm_id, wikidata, rings))
    if offset != len(plain):
        raise ValueError("byte in avanzo nel payload")
    return BuildingTile(level, x, y, buildings)


def write_file_atomic(path: str, blob: bytes) -> None:
    os.makedirs(os.path.dirname(path), exist_ok=True)
    temporary = f"{path}.tmp"
    with open(temporary, "wb") as handle:
        handle.write(blob)
    os.replace(temporary, path)


def read_tile(path: str) -> BuildingTile:
    with open(path, "rb") as handle:
        return decode_tile(handle.read())


# --- Indice ---------------------------------------------------------------------

def encode_index(level: int, entries: list[BuildingTileEntry]) -> bytes:
    ordered = sorted(entries, key=lambda entry: (entry.y, entry.x))
    parts = [INDEX_HEADER.pack(INDEX_MAGIC, INDEX_VERSION, 0, level, len(ordered))]
    parts.extend(INDEX_RECORD.pack(entry.x, entry.y, entry.count, entry.size) for entry in ordered)
    return b"".join(parts)


def write_index(root: str, level: int, entries: list[BuildingTileEntry]) -> str:
    path = os.path.join(root, str(level), INDEX_FILENAME)
    write_file_atomic(path, encode_index(level, entries))
    return path


def read_index(root: str, level: int) -> list[BuildingTileEntry]:
    with open(os.path.join(root, str(level), INDEX_FILENAME), "rb") as handle:
        blob = handle.read()
    magic, _, _, stored_level, count = INDEX_HEADER.unpack_from(blob)
    if magic != INDEX_MAGIC or stored_level != level:
        raise ValueError("non e' l'indice di questo livello di edifici")
    if len(blob) != INDEX_HEADER.size + count * INDEX_RECORD.size:
        raise ValueError("indice di dimensione incoerente")
    return [BuildingTileEntry(*INDEX_RECORD.unpack_from(blob, INDEX_HEADER.size + index * INDEX_RECORD.size))
            for index in range(count)]


# --- Manifest -------------------------------------------------------------------

def build_manifest(*, dataset_name: str, bbox: tuple, level: int, tile_count: int, building_count: int,
                   classes: dict, measured: dict, source: dict, pipeline_version: str) -> dict:
    west, south, east, north = bbox
    return {
        "formatVersion": 1,
        "generator": f"geoworld-pipeline {pipeline_version}",
        "datasetName": dataset_name,
        "datasetKind": "buildings",
        "content": "sagome degli edifici con altezza, piani, tetto, colori (OpenStreetMap)",
        "tilingScheme": {"type": "geodetic-wgs84", "crs": "EPSG:4326",
                         "level0TilesX": tiling.tiles_x(0), "level0TilesY": tiling.tiles_y(0)},
        "level": level,
        "tileFormat": {"extension": TILE_EXTENSION, "path": "<level>/<x>/<y>" + TILE_EXTENSION,
                       "headerBytes": HEADER.size, "recordBytes": RECORD.size, "quantum": QUANTUM,
                       "compression": "zlib"},
        "classes": {item.name: item.id for item in bldclasses.CLASSES},
        "roofShapes": dict(bldclasses.ROOF_SHAPES),
        "tiles": tile_count,
        "buildings": building_count,
        "byClass": classes,
        "measured": measured,
        "boundingBox": {"west": west, "south": south, "east": east, "north": north},
        "source": source,
        "levelIndex": {"path": "<level>/" + INDEX_FILENAME, "magic": INDEX_MAGIC.decode("ascii"),
                       "recordBytes": INDEX_RECORD.size,
                       "fields": ["uint32 x", "uint32 y", "uint32 edifici", "uint32 byte del file"],
                       "sortedBy": "(y, x)"},
    }


def write_manifest(root: str, manifest: dict) -> str:
    path = os.path.join(root, MANIFEST_FILENAME)
    os.makedirs(root, exist_ok=True)
    temporary = f"{path}.tmp"
    with open(temporary, "w", encoding="utf-8") as handle:
        json.dump(manifest, handle, indent=2, ensure_ascii=False)
    os.replace(temporary, path)
    return path


def read_manifest(root: str) -> dict:
    with open(os.path.join(root, MANIFEST_FILENAME), "r", encoding="utf-8") as handle:
        return json.load(handle)


# --- Tile di riferimento per i test C++ -------------------------------------
#
# Il lettore C++ (BuildingTileFormat.h) si prova su un file scritto da QUESTO
# codice, in Source/GeoTiles/TestData/buildings_fixture.gbd; un test Python
# controlla che coincida con quello che si rigenera qui.

#: La tile di livello 15 che contiene Torino (45,07° N, 7,68° E).
FIXTURE_KEY = (15, 34166, 8179)


def fixture_tile() -> BuildingTile:
    """Tre edifici: una casa 20 x 10 m, un palazzo con il cortile, una chiesa colorata."""
    level, x, y = FIXTURE_KEY
    # Un passo di quantizzazione: ~3,7 cm verso nord, ~2,6 cm verso est a 45°.
    house = np.array([[2000, 2000], [2770, 2000], [2770, 2270], [2000, 2270]], dtype=np.int16)
    block_outer = np.array([[6000, 6000], [9000, 6000], [9000, 8000], [6000, 8000]], dtype=np.int16)
    block_court = np.array([[7000, 6800], [7000, 7200], [8000, 7200], [8000, 6800]], dtype=np.int16)
    church = np.array([[12000, 3000], [12900, 3000], [12900, 4400], [12450, 4700], [12000, 4400]], dtype=np.int16)
    return BuildingTile(level, x, y, [
        Building(1, bldclasses.ROOF_SHAPES["Gabled"], bldclasses.FLAG_LEVELS_MEASURED, 2, 65, 0, 0,
                 (0, 0, 0), (0, 0, 0), pack_osm_id(OSM_WAY, 1001), 0, [house]),
        Building(2, 0, bldclasses.FLAG_HEIGHT_MEASURED, 6, 195, 0, 0, (0, 0, 0), (0, 0, 0),
                 pack_osm_id(OSM_RELATION, 2002), 0, [block_outer, block_court]),
        Building(5, 0, bldclasses.FLAG_WALL_COLOUR, 5, 140, 0, 0, (225, 205, 165), (0, 0, 0),
                 pack_osm_id(OSM_WAY, 3003), 12345, [church]),
    ])


def fixture_bytes() -> bytes:
    return encode_tile(fixture_tile())

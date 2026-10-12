"""
Formato delle tile d'acqua (.gwa) della Fase 9, con indice, manifest e geoide.

Una tile d'acqua accompagna una tile di TERRENO: stesso livello, stessa x e
y, e la sua griglia di post coincide con quella del terreno (129 x 129, i post
di bordo condivisi con le tile accanto). Cosi' il runtime puo' scavare il
terreno post per post, e due tile vicine scrivono lo stesso numero sul post
che condividono.

    header   32 byte: "GWWA", versione, flag, livello, x, y, posts, raster,
             byte del payload compresso, riservato
    payload  zlib di, nell'ordine:
               superficie   int16 x posts^2   quota dell'acqua, decimetri
               fondale      int16 x posts^2   quota a cui scavare, decimetri
               sdf          uint8 x raster^2  128 + 8 x distanza dalla riva in pixel
               onde         uint8 x raster^2  esposizione al vento 0..255
               corrente u   int8  x raster^2  solo se TILE_FLOW: verso est
               corrente v   int8  x raster^2  solo se TILE_FLOW: verso SUD

Righe dal nord, colonne dall'ovest, come le quote e le immagini.

I DECIMETRI SENZA OFFSET. Lo zero e' lo stesso per tutte le tile (l'ellissoide):
due tile vicine quantizzano allo stesso modo il post che condividono, e le
superfici non hanno fessure. -32768 vuol dire "niente acqua qui" (superficie)
o "non scavare" (fondale).

LA DISTANZA IN PIXEL. Positiva in acqua, negativa a terra, saturata a +-16
pixel. Il pixel e' quello del raster (lato della tile / 256) misurato in METRI
come media geometrica fra est e nord: alle nostre latitudini un pixel e' piu'
stretto in longitudine che in latitudine, e la distanza deve valere uguale in
tutte le direzioni. Il runtime ricostruisce i metri con lo stesso conto
(metres_per_pixel).
"""

from __future__ import annotations

import json
import math
import os
import struct
import zlib
from dataclasses import dataclass, asdict

import numpy as np

from . import tiling, waterclasses

TILE_EXTENSION = ".gwa"
MAGIC = b"GWWA"
FORMAT_VERSION = 1
HEADER = struct.Struct("<4sHHIIIHHII")           # 32 byte

POSTS = tiling.TILE_POSTS                       # 129
RASTER = 256

#: Quota non valida (niente acqua, o non scavare).
NO_HEIGHT = -32768

#: Quanti "ottavi di pixel" vale un passo dell'SDF, e dove sta lo zero.
SDF_SCALE = 8.0
SDF_ZERO = 128
SDF_RANGE_PX = 15.9

INDEX_MAGIC = b"GWWI"
INDEX_VERSION = 1
INDEX_HEADER = struct.Struct("<4sHHII")          # magic, versione, riservato, livello, numero
INDEX_RECORD = struct.Struct("<IIHHI")           # x, y, flag, riservato, byte del file
INDEX_FILENAME = "index.bin"
MANIFEST_FILENAME = "manifest.json"

GEOID_FILENAME = "geoid.bin"
GEOID_MAGIC = b"GWGE"
GEOID_HEADER = struct.Struct("<4sHHIIddd")       # 40 byte


@dataclass
class WaterTile:
    level: int
    x: int
    y: int
    flags: int
    surface: np.ndarray      # float32 (POSTS, POSTS), NaN = niente acqua
    bed: np.ndarray          # float32 (POSTS, POSTS), NaN = non scavare
    sdf: np.ndarray          # uint8 (RASTER, RASTER)
    wave: np.ndarray         # uint8 (RASTER, RASTER)
    flow_u: np.ndarray | None = None    # int8 (RASTER, RASTER)
    flow_v: np.ndarray | None = None


def metres_per_pixel(level: int, y: int) -> float:
    """Lato di un pixel del raster in metri (media geometrica est/nord), al centro della tile."""
    bounds = tiling.tile_bounds(level, 0, y)
    centre = 0.5 * (bounds.north + bounds.south)
    east, north = tiling.metres_per_degree(centre)
    span = tiling.tile_span_deg(level) / RASTER
    return math.sqrt(span * east * span * north)


def heights_to_dm(heights: np.ndarray) -> np.ndarray:
    """Metri (NaN = assente) -> decimetri int16, saturati, con NO_HEIGHT per i NaN."""
    values = np.asarray(heights, dtype=np.float64)
    out = np.full(values.shape, NO_HEIGHT, dtype=np.int16)
    valid = np.isfinite(values)
    out[valid] = np.clip(np.round(values[valid] * 10.0), NO_HEIGHT + 1, 32767).astype(np.int16)
    return out


def dm_to_heights(dm: np.ndarray) -> np.ndarray:
    values = dm.astype(np.float32) / 10.0
    values[dm == NO_HEIGHT] = np.nan
    return values


def sdf_to_bytes(distance_px: np.ndarray) -> np.ndarray:
    return np.clip(np.round(SDF_ZERO + SDF_SCALE * np.asarray(distance_px, dtype=np.float64)),
                   0, 255).astype(np.uint8)


def bytes_to_sdf(encoded: np.ndarray) -> np.ndarray:
    return (encoded.astype(np.float32) - SDF_ZERO) / SDF_SCALE


def tile_relative_path(level: int, x: int, y: int) -> str:
    return os.path.join(str(level), str(x), f"{y}{TILE_EXTENSION}")


def encode_tile(tile: WaterTile) -> bytes:
    flags = tile.flags & ~waterclasses.TILE_FLOW
    has_flow = tile.flow_u is not None and tile.flow_v is not None
    if has_flow:
        flags |= waterclasses.TILE_FLOW

    planes = [heights_to_dm(tile.surface).astype("<i2").tobytes(),
              heights_to_dm(tile.bed).astype("<i2").tobytes(),
              np.ascontiguousarray(tile.sdf, dtype=np.uint8).tobytes(),
              np.ascontiguousarray(tile.wave, dtype=np.uint8).tobytes()]
    if has_flow:
        planes.append(np.ascontiguousarray(tile.flow_u, dtype=np.int8).tobytes())
        planes.append(np.ascontiguousarray(tile.flow_v, dtype=np.int8).tobytes())
    payload = zlib.compress(b"".join(planes), 6)

    header = HEADER.pack(MAGIC, FORMAT_VERSION, flags, tile.level, tile.x, tile.y,
                         POSTS, RASTER, len(payload), 0)
    return header + payload


def decode_tile(blob: bytes) -> WaterTile:
    if len(blob) < HEADER.size:
        raise ValueError(f"{len(blob)} byte: troppo pochi per un header")
    magic, version, flags, level, x, y, posts, raster, size, _ = HEADER.unpack_from(blob, 0)
    if magic != MAGIC:
        raise ValueError(f"magic {magic!r}: non e' una tile d'acqua")
    if version != FORMAT_VERSION:
        raise ValueError(f"versione {version}, attesa {FORMAT_VERSION}")
    if posts != POSTS or raster != RASTER:
        raise ValueError(f"dimensioni {posts}/{raster}, attese {POSTS}/{RASTER}")
    if len(blob) != HEADER.size + size:
        raise ValueError(f"{len(blob)} byte, l'header ne dichiara {HEADER.size + size}")

    data = zlib.decompress(blob[HEADER.size:])
    grid = POSTS * POSTS
    pixels = RASTER * RASTER
    has_flow = bool(flags & waterclasses.TILE_FLOW)
    expected = 2 * grid * 2 + 2 * pixels + (2 * pixels if has_flow else 0)
    if len(data) != expected:
        raise ValueError(f"payload di {len(data)} byte, attesi {expected}")

    cursor = 0

    def take(count: int, dtype: str, shape: tuple[int, int]) -> np.ndarray:
        nonlocal cursor
        item = np.dtype(dtype).itemsize
        array = np.frombuffer(data, dtype=dtype, count=count, offset=cursor).reshape(shape)
        cursor += count * item
        return array

    surface = dm_to_heights(take(grid, "<i2", (POSTS, POSTS)))
    bed = dm_to_heights(take(grid, "<i2", (POSTS, POSTS)))
    sdf = take(pixels, "u1", (RASTER, RASTER)).copy()
    wave = take(pixels, "u1", (RASTER, RASTER)).copy()
    flow_u = flow_v = None
    if has_flow:
        flow_u = take(pixels, "i1", (RASTER, RASTER)).copy()
        flow_v = take(pixels, "i1", (RASTER, RASTER)).copy()
    return WaterTile(level, x, y, flags, surface, bed, sdf, wave, flow_u, flow_v)


def write_tile_atomic(path: str, blob: bytes) -> None:
    os.makedirs(os.path.dirname(path), exist_ok=True)
    temporary = path + ".tmp"
    with open(temporary, "wb") as handle:
        handle.write(blob)
    os.replace(temporary, path)


def read_tile(path: str) -> WaterTile:
    with open(path, "rb") as handle:
        return decode_tile(handle.read())


# --- Indice di livello --------------------------------------------------------

@dataclass
class WaterTileEntry:
    x: int
    y: int
    flags: int
    size: int


def write_level_index(root: str, level: int, entries: list[WaterTileEntry]) -> str:
    entries = sorted(entries, key=lambda e: (e.y, e.x))
    payload = bytearray(INDEX_HEADER.pack(INDEX_MAGIC, INDEX_VERSION, 0, level, len(entries)))
    for entry in entries:
        payload += INDEX_RECORD.pack(entry.x, entry.y, entry.flags, 0, entry.size)
    path = os.path.join(root, str(level), INDEX_FILENAME)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    temporary = f"{path}.tmp"
    with open(temporary, "wb") as handle:
        handle.write(payload)
    os.replace(temporary, path)
    return path


def read_level_index(root: str, level: int) -> list[WaterTileEntry]:
    path = os.path.join(root, str(level), INDEX_FILENAME)
    with open(path, "rb") as handle:
        blob = handle.read()
    magic, version, _reserved, stored_level, count = INDEX_HEADER.unpack_from(blob, 0)
    if magic != INDEX_MAGIC:
        raise ValueError(f"{path}: magic {magic!r}: non e' l'indice di un dataset d'acqua")
    if version != INDEX_VERSION or stored_level != level:
        raise ValueError(f"{path}: versione {version} / livello {stored_level} inattesi")
    if len(blob) != INDEX_HEADER.size + count * INDEX_RECORD.size:
        raise ValueError(f"{path}: dimensione incoerente con {count} voci")
    entries = []
    for index in range(count):
        x, y, flags, _, size = INDEX_RECORD.unpack_from(blob, INDEX_HEADER.size + index * INDEX_RECORD.size)
        entries.append(WaterTileEntry(x, y, flags, size))
    return entries


# --- Geoide ---------------------------------------------------------------------

def write_geoid(root: str, grid: np.ndarray, geotransform: tuple[float, float, float]) -> str:
    """
    Il geoide sull'area del dataset: N in metri su una griglia regolare, riga 0 a
    NORD. Il runtime lo usa per la quota sul livello del mare nelle stampe.
    """
    origin_lon, origin_lat, spacing = geotransform
    rows, columns = grid.shape
    path = os.path.join(root, GEOID_FILENAME)
    os.makedirs(root, exist_ok=True)
    temporary = f"{path}.tmp"
    with open(temporary, "wb") as handle:
        handle.write(GEOID_HEADER.pack(GEOID_MAGIC, 1, 0, columns, rows,
                                       float(origin_lon), float(origin_lat), float(spacing)))
        handle.write(np.ascontiguousarray(grid, dtype="<f4").tobytes())
    os.replace(temporary, path)
    return path


def read_geoid(root: str) -> tuple[np.ndarray, tuple[float, float, float]]:
    with open(os.path.join(root, GEOID_FILENAME), "rb") as handle:
        blob = handle.read()
    magic, version, _, columns, rows, lon, lat, spacing = GEOID_HEADER.unpack_from(blob, 0)
    if magic != GEOID_MAGIC or version != 1:
        raise ValueError("geoid.bin non valido")
    grid = np.frombuffer(blob, dtype="<f4", count=rows * columns,
                         offset=GEOID_HEADER.size).reshape(rows, columns)
    return grid, (lon, lat, spacing)


# --- Manifest -------------------------------------------------------------------

def build_manifest(*, dataset_name: str, bbox: tuple[float, float, float, float],
                   levels: list[dict], source: dict, body_counts: dict[str, int],
                   pipeline_version: str) -> dict:
    west, south, east, north = bbox
    return {
        "formatVersion": 1,
        "generator": f"geoworld-pipeline {pipeline_version}",
        "datasetName": dataset_name,
        "datasetKind": "water",
        "content": "acqua: mare, lagune, laghi, fiumi, canali",
        "tilingScheme": {"type": "geodetic-wgs84", "crs": "EPSG:4326",
                         "level0TilesX": tiling.tiles_x(0), "level0TilesY": tiling.tiles_y(0)},
        "tileFormat": {
            "extension": TILE_EXTENSION,
            "path": "<level>/<x>/<y>" + TILE_EXTENSION,
            "headerBytes": HEADER.size,
            "posts": POSTS,
            "raster": RASTER,
            "heights": "int16 decimetri ellissoidici, -32768 = assente",
            "sdf": f"uint8 = {SDF_ZERO} + {SDF_SCALE:g} x distanza dalla riva in pixel (positiva in acqua)",
            "compression": "zlib",
        },
        "rulesVersion": waterclasses.WATER_RULES_VERSION,
        "bodies": body_counts,
        "boundingBox": {"west": west, "south": south, "east": east, "north": north},
        "geoid": {"path": GEOID_FILENAME, "headerBytes": GEOID_HEADER.size},
        "source": source,
        "levels": levels,
        "levelIndex": {"path": "<level>/" + INDEX_FILENAME, "magic": INDEX_MAGIC.decode("ascii"),
                       "recordBytes": INDEX_RECORD.size,
                       "fields": ["uint32 x", "uint32 y", "uint16 flags", "uint16 riservato",
                                  "uint32 byte del file"],
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


def entry_as_dict(entry: WaterTileEntry) -> dict:
    return asdict(entry)


# --- Tile di riferimento per i test C++ -------------------------------------
#
# Come per le strade: il lettore C++ (WaterTileFormat.h) si prova su un file
# scritto da QUESTO codice. Sta in Source/GeoTiles/TestData/water_fixture.gwa e
# un test Python controlla che coincida con quello che si rigenera qui.

FIXTURE_KEY = (14, 17085, 4093)


def fixture_tile() -> WaterTile:
    """
    Mezza tile di lago a ovest (quota 230,4 m), terra a est; un fiume che
    scorre verso est lungo la riga 64. Valori facili da controllare dal C++.
    """
    level, x, y = FIXTURE_KEY
    surface = np.full((POSTS, POSTS), np.nan, dtype=np.float32)
    bed = np.full((POSTS, POSTS), np.nan, dtype=np.float32)
    surface[:, :65] = 230.4
    bed[:, :64] = 230.4 - (0.5 + 0.12 * np.arange(64, 0, -1, dtype=np.float32) * 9.5)
    columns = np.arange(RASTER, dtype=np.float32)
    distance = np.tile(127.5 - columns, (RASTER, 1))          # riva fra le colonne 127 e 128
    sdf = sdf_to_bytes(distance)
    wave = np.where(distance > 0, 51, 0).astype(np.uint8)    # 0,2: un lago piccolo
    flow_u = np.zeros((RASTER, RASTER), dtype=np.int8)
    flow_v = np.zeros((RASTER, RASTER), dtype=np.int8)
    flow_u[120:136, :] = 100
    return WaterTile(level, x, y, waterclasses.TILE_LAKE | waterclasses.TILE_RIVER,
                     surface, bed, sdf, wave, flow_u, flow_v)


def fixture_bytes() -> bytes:
    return encode_tile(fixture_tile())

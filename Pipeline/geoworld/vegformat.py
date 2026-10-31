"""
Formato delle tile di vegetazione (.gvg), con indice e manifest.

Una tile di vegetazione dice, pixel per pixel, che cosa copre il suolo: bosco,
arbusti, prato, campi, citta'... (vegclasses.py). Tutte le tile di un dataset
stanno a UN livello, 13 di default: 2,4 km di lato, 256 pixel da ~10 m, come
la risoluzione di WorldCover. Il runtime ne fa gli alberi (TreePlacement.h).

    header   32 byte: "GWVG", versione, flag, livello, x, y, raster (256),
             riservato, byte del payload compresso, riservato
    payload  zlib di raster x raster byte: la classe; righe dal NORD,
             colonne dall'OVEST, come le quote e le immagini

LA ZLIB: le classi sono lunghe file di numeri uguali (un bosco, un campo), e
si comprimono di dieci volte. Il runtime decomprime con FCompression.
"""

from __future__ import annotations

import json
import os
import struct
import zlib
from dataclasses import dataclass

import numpy as np

from . import tiling, vegclasses

TILE_EXTENSION = ".gvg"
MAGIC = b"GWVG"
FORMAT_VERSION = 1
HEADER = struct.Struct("<4sHHIIIHHII")           # 32 byte
RASTER = 256

INDEX_MAGIC = b"GWVI"
INDEX_VERSION = 1
INDEX_HEADER = struct.Struct("<4sHHII")          # magic, versione, riservato, livello, numero
INDEX_RECORD = struct.Struct("<IIHHI")           # x, y, flag, riservato, byte del file
INDEX_FILENAME = "index.bin"
MANIFEST_FILENAME = "manifest.json"


@dataclass
class VegetationTile:
    level: int
    x: int
    y: int
    flags: int
    classes: np.ndarray       # uint8 (RASTER, RASTER), riga 0 = nord


@dataclass
class VegetationTileEntry:
    x: int
    y: int
    flags: int
    size: int


def tile_path(root: str, level: int, x: int, y: int) -> str:
    return os.path.join(root, str(level), str(x), f"{y}{TILE_EXTENSION}")


def encode_tile(tile: VegetationTile) -> bytes:
    classes = np.ascontiguousarray(tile.classes, dtype=np.uint8)
    if classes.shape != (RASTER, RASTER):
        raise ValueError(f"classi {classes.shape}, attese ({RASTER}, {RASTER})")
    payload = zlib.compress(classes.tobytes(), 6)
    header = HEADER.pack(MAGIC, FORMAT_VERSION, tile.flags, tile.level, tile.x, tile.y, RASTER, 0, len(payload), 0)
    return header + payload


def decode_tile(blob: bytes) -> VegetationTile:
    if len(blob) < HEADER.size:
        raise ValueError("file troppo corto per l'header")
    magic, version, flags, level, x, y, raster, _, payload_bytes, _ = HEADER.unpack_from(blob)
    if magic != MAGIC:
        raise ValueError("non e' una tile di vegetazione")
    if version != FORMAT_VERSION or raster != RASTER:
        raise ValueError(f"versione {version} o raster {raster} inattesi")
    if len(blob) != HEADER.size + payload_bytes:
        raise ValueError("dimensione incoerente con l'header (troncato?)")
    plain = zlib.decompress(blob[HEADER.size:])
    if len(plain) != RASTER * RASTER:
        raise ValueError(f"payload di {len(plain)} byte, attesi {RASTER * RASTER}")
    classes = np.frombuffer(plain, dtype=np.uint8).reshape(RASTER, RASTER).copy()
    return VegetationTile(level, x, y, flags, classes)


def write_tile_atomic(path: str, blob: bytes) -> None:
    os.makedirs(os.path.dirname(path), exist_ok=True)
    temporary = f"{path}.tmp"
    with open(temporary, "wb") as handle:
        handle.write(blob)
    os.replace(temporary, path)


def read_tile(path: str) -> VegetationTile:
    with open(path, "rb") as handle:
        return decode_tile(handle.read())


# --- Indice ---------------------------------------------------------------------

def encode_index(level: int, entries: list[VegetationTileEntry]) -> bytes:
    ordered = sorted(entries, key=lambda entry: (entry.y, entry.x))
    parts = [INDEX_HEADER.pack(INDEX_MAGIC, INDEX_VERSION, 0, level, len(ordered))]
    parts.extend(INDEX_RECORD.pack(entry.x, entry.y, entry.flags, 0, entry.size) for entry in ordered)
    return b"".join(parts)


def write_index(root: str, level: int, entries: list[VegetationTileEntry]) -> str:
    path = os.path.join(root, str(level), INDEX_FILENAME)
    write_tile_atomic(path, encode_index(level, entries))
    return path


def read_index(root: str, level: int) -> list[VegetationTileEntry]:
    with open(os.path.join(root, str(level), INDEX_FILENAME), "rb") as handle:
        blob = handle.read()
    magic, _, _, stored_level, count = INDEX_HEADER.unpack_from(blob)
    if magic != INDEX_MAGIC or stored_level != level:
        raise ValueError("non e' l'indice di questo livello di vegetazione")
    if len(blob) != INDEX_HEADER.size + count * INDEX_RECORD.size:
        raise ValueError("indice di dimensione incoerente")
    entries = []
    for index in range(count):
        x, y, flags, _, size = INDEX_RECORD.unpack_from(blob, INDEX_HEADER.size + index * INDEX_RECORD.size)
        entries.append(VegetationTileEntry(x, y, flags, size))
    return entries


# --- Manifest -------------------------------------------------------------------

def build_manifest(*, dataset_name: str, bbox: tuple[float, float, float, float], level: int,
                   tile_count: int, cover: dict, source: dict, pipeline_version: str) -> dict:
    west, south, east, north = bbox
    return {
        "formatVersion": 1,
        "generator": f"geoworld-pipeline {pipeline_version}",
        "datasetName": dataset_name,
        "datasetKind": "vegetation",
        "content": "copertura del suolo: bosco, arbusti, prato, campi, costruito, ...",
        "tilingScheme": {"type": "geodetic-wgs84", "crs": "EPSG:4326",
                         "level0TilesX": tiling.tiles_x(0), "level0TilesY": tiling.tiles_y(0)},
        "level": level,
        "tileFormat": {
            "extension": TILE_EXTENSION,
            "path": "<level>/<x>/<y>" + TILE_EXTENSION,
            "headerBytes": HEADER.size,
            "raster": RASTER,
            "pixels": "uint8: la classe (vegclasses.py, VegetationClasses.h)",
            "rowOrder": "riga 0 = nord",
            "columnOrder": "colonna 0 = ovest",
            "compression": "zlib",
        },
        "classes": {cover.name: cover.id for cover in vegclasses.CLASSES},
        "cover": cover,
        "tiles": tile_count,
        "boundingBox": {"west": west, "south": south, "east": east, "north": north},
        "source": source,
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


# --- Tile di riferimento per i test C++ -------------------------------------
#
# Come per l'acqua: il lettore C++ (VegetationTileFormat.h) si prova su un file
# scritto da QUESTO codice, in Source/GeoTiles/TestData/vegetation_fixture.gvg,
# e un test Python controlla che coincida con quello che si rigenera qui.

#: La tile di livello 13 che contiene Torino (45,07° N, 7,68° E).
FIXTURE_KEY = (13, 8541, 2044)


def fixture_tile() -> VegetationTile:
    """Bosco nella meta' ovest, prato nella meta' est, acqua nell'ultima riga (a sud)."""
    level, x, y = FIXTURE_KEY
    classes = np.full((RASTER, RASTER), vegclasses.BY_NAME["Grass"].id, dtype=np.uint8)
    classes[:, :RASTER // 2] = vegclasses.BY_NAME["Trees"].id
    classes[-1, :] = vegclasses.BY_NAME["Water"].id
    return VegetationTile(level, x, y, vegclasses.tile_flags(classes), classes)


def fixture_bytes() -> bytes:
    return encode_tile(fixture_tile())

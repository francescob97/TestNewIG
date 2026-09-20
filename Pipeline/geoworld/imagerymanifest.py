"""
Indice e manifest di un dataset di ortofoto.

PERCHE' UN MODULO SEPARATO DA manifest.py
-----------------------------------------
Perche' le due cose che descrivono sono diverse, non perche' il codice sia
diverso. L'indice delle quote porta min e max quota per tile, che servono al
culling del quadtree PRIMA di leggere la tile; quello delle immagini porta la
copertura, che serve a sapere se la tile vale la pena di essere disegnata.
Ficcare entrambi in un record unico con campi che a volte significano una cosa e
a volte un'altra e' il modo piu' rapido per rendere illeggibili tutti e due.

Il magic e' diverso di proposito (GWIA contro GWIX): aprire un indice di
immagini credendo che sia di quote deve fallire subito e con un messaggio
chiaro, non leggere numeri a caso.
"""

from __future__ import annotations

import json
import os
import struct
from dataclasses import dataclass, asdict

from . import imagecut, imageformat, tiling

INDEX_MAGIC = b"GWIA"
INDEX_VERSION = 1
INDEX_HEADER = struct.Struct("<4sHHII")     # magic, versione, riservato, livello, numero
INDEX_RECORD = struct.Struct("<IIBBH")      # x, y, copertura, tipo payload, riservato
INDEX_FILENAME = "index.bin"
MANIFEST_FILENAME = "manifest.json"


@dataclass
class ImageTileEntry:
    x: int
    y: int
    coverage_percent: int
    payload_type: int = imageformat.PAYLOAD_JPEG


@dataclass
class ImageLevelSummary:
    level: int
    tile_count: int
    tile_x_min: int
    tile_x_max: int
    tile_y_min: int
    tile_y_max: int
    partial_tiles: int
    pixel_size_deg: float
    pixel_size_m_lat: float
    pixel_size_m_lon_at_centre: float


def write_level_index(root: str, level: int, entries: list[ImageTileEntry]) -> str:
    """Scrive <root>/<level>/index.bin, ordinato per (y, x) come quello delle quote."""
    entries = sorted(entries, key=lambda e: (e.y, e.x))

    payload = bytearray(INDEX_HEADER.pack(INDEX_MAGIC, INDEX_VERSION, 0, level, len(entries)))
    for entry in entries:
        payload += INDEX_RECORD.pack(entry.x, entry.y,
                                     max(0, min(100, entry.coverage_percent)),
                                     entry.payload_type, 0)

    path = os.path.join(root, str(level), INDEX_FILENAME)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    temporary = f"{path}.tmp"
    with open(temporary, "wb") as handle:
        handle.write(payload)
        handle.flush()
        os.fsync(handle.fileno())
    os.replace(temporary, path)
    return path


def read_level_index(root: str, level: int) -> list[ImageTileEntry]:
    path = os.path.join(root, str(level), INDEX_FILENAME)
    with open(path, "rb") as handle:
        blob = handle.read()

    magic, version, _reserved, stored_level, count = INDEX_HEADER.unpack_from(blob, 0)
    if magic != INDEX_MAGIC:
        raise ValueError(
            f"{path}: magic {magic!r}, atteso {INDEX_MAGIC!r}. "
            "Sembra l'indice di un dataset di quote, non di immagini")
    if version != INDEX_VERSION:
        raise ValueError(f"{path}: versione indice {version}, attesa {INDEX_VERSION}")
    if stored_level != level:
        raise ValueError(f"{path}: dichiara il livello {stored_level}, atteso {level}")

    expected = INDEX_HEADER.size + count * INDEX_RECORD.size
    if len(blob) != expected:
        raise ValueError(f"{path}: {len(blob)} byte, attesi {expected}")

    entries = []
    for i in range(count):
        x, y, coverage, payload_type, _pad = INDEX_RECORD.unpack_from(
            blob, INDEX_HEADER.size + i * INDEX_RECORD.size)
        entries.append(ImageTileEntry(x, y, coverage, payload_type))
    return entries


def summarise_level(level: int, entries: list[ImageTileEntry],
                    centre_latitude: float) -> ImageLevelSummary:
    xs = [e.x for e in entries]
    ys = [e.y for e in entries]
    metres_lon, metres_lat = imagecut.imagery_pixel_size_metres(level, centre_latitude)
    return ImageLevelSummary(
        level=level,
        tile_count=len(entries),
        tile_x_min=min(xs), tile_x_max=max(xs),
        tile_y_min=min(ys), tile_y_max=max(ys),
        partial_tiles=sum(1 for e in entries if e.coverage_percent < 100),
        pixel_size_deg=imagecut.imagery_pixel_size_deg(level),
        pixel_size_m_lat=metres_lat,
        pixel_size_m_lon_at_centre=metres_lon)


def build_manifest(*, dataset_name: str, bbox: tuple[float, float, float, float],
                   levels: list[ImageLevelSummary], source: dict,
                   quality: int, pipeline_version: str) -> dict:
    west, south, east, north = bbox
    return {
        "formatVersion": 1,
        "generator": f"geoworld-pipeline {pipeline_version}",
        "datasetName": dataset_name,
        "datasetKind": "imagery",

        # Identico a quello delle quote, ed e' il punto: e' cio' che permette a
        # una tile di terreno e a una di immagine con la stessa chiave di coprire
        # lo stesso rettangolo, e quindi al drappeggio di non richiedere calcoli.
        "tilingScheme": {
            "type": "geodetic-wgs84",
            "crs": "EPSG:4326",
            "level0TilesX": tiling.tiles_x(0),
            "level0TilesY": tiling.tiles_y(0),
            "xAxis": "est, x=0 a lon -180",
            "yAxis": "sud, y=0 a lat +90",
        },

        "tileFormat": {
            "extension": imageformat.TILE_EXTENSION,
            "path": "<level>/<x>/<y>" + imageformat.TILE_EXTENSION,
            "pixels": imageformat.TILE_PIXELS,
            "overlapPixels": 0,
            "registration":
                "area (i pixel coprono il rettangolo senza sovrapposizione: "
                "NON come i post delle quote, che condividono il bordo)",
            "payload": "jpeg",
            "jpegQuality": quality,
            "headerBytes": imageformat.HEADER_SIZE,
            "rowOrder": "riga 0 = nord",
            "columnOrder": "colonna 0 = ovest",
            "fillColour": list(imageformat.FILL_COLOUR),
        },

        "boundingBox": {"west": west, "south": south, "east": east, "north": north},
        "source": source,

        "levels": [asdict(level) for level in levels],
        "levelIndex": {
            "path": "<level>/" + INDEX_FILENAME,
            "magic": INDEX_MAGIC.decode("ascii"),
            "recordBytes": INDEX_RECORD.size,
            "fields": ["uint32 x", "uint32 y", "uint8 coveragePercent",
                       "uint8 payloadType", "uint16 reserved"],
            "sortedBy": "(y, x)",
        },
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

"""
Manifest del dataset e indici per livello.

--------------------------------------------------------------------------
PERCHE' DUE ARTEFATTI E NON SOLO IL MANIFEST JSON
--------------------------------------------------------------------------
La specifica chiedeva un manifest JSON con bounding box, livelli disponibili e
min/max quota PER TILE, sottolineando che il min/max serve al culling e al
bounding volume. Il min/max c'e' e non e' stato omesso: e' stato SPOSTATO,
perche' in JSON non ci sta.

I numeri: l'Italia al livello 14 (passo ~9.5 m, la risoluzione nativa di
TINITALY) ha dell'ordine di 4 x 10^5 tile con dato. Una voce JSON del tipo
{"x":8560,"y":3936,"min":12.5,"max":1843.2} sono ~45 byte, quindi ~18 MB di
manifest da leggere e parsare all'avvio solo per l'ultimo livello. Un indice
binario con record di 16 byte (x, y, min, max) costa 6.4 MB, si legge con una
sola read e si mappa in memoria senza parsing.

Quindi:
  manifest.json      -> metadati GLOBALI, leggibili a occhio: schema di tiling,
                        formato, bbox, livelli, provenienza, datum verticale
                        effettivamente applicato, min/max per LIVELLO.
  <level>/index.bin  -> quali tile esistono a quel livello e il loro min/max.

Il manifest resta la cosa che apri quando vuoi capire cosa c'e' dentro un
dataset; l'indice e' la cosa che il runtime legge.

--------------------------------------------------------------------------
PERCHE' SERVE SAPERE QUALI TILE ESISTONO
--------------------------------------------------------------------------
Il quadtree (Fase 4) deve poter decidere se raffinare PRIMA di aver caricato
alcunche'. Senza indice dovrebbe tentare una lettura su disco per ogni tile
candidata e interpretare il fallimento: un I/O per scoprire un'assenza, sul
percorso critico. Con l'indice, l'assenza si sa a costo zero.
"""

from __future__ import annotations

import json
import os
import struct
from dataclasses import dataclass, field, asdict

import numpy as np

from . import tiling, tileformat

INDEX_MAGIC = b"GWIX"
INDEX_VERSION = 1
INDEX_HEADER = struct.Struct("<4sHHII")      # magic, version, reserved, level, count
INDEX_RECORD = struct.Struct("<IIff")        # x, y, minHeight, maxHeight
INDEX_FILENAME = "index.bin"
MANIFEST_FILENAME = "manifest.json"


@dataclass
class TileEntry:
    x: int
    y: int
    min_height: float
    max_height: float


@dataclass
class LevelSummary:
    level: int
    tile_count: int
    tile_x_min: int
    tile_x_max: int
    tile_y_min: int
    tile_y_max: int
    min_height: float
    max_height: float
    post_spacing_deg: float
    post_spacing_m_lat: float
    post_spacing_m_lon_at_centre: float


def write_level_index(root: str, level: int, entries: list[TileEntry]) -> str:
    """
    Scrive <root>/<level>/index.bin, ordinato per (y, x).

    L'ordinamento non e' estetico: permette al runtime la ricerca binaria senza
    costruire una hash map, e rende i confronti fra due rigenerazioni del
    dataset un semplice diff binario.
    """
    entries = sorted(entries, key=lambda e: (e.y, e.x))

    payload = bytearray(INDEX_HEADER.pack(INDEX_MAGIC, INDEX_VERSION, 0, level, len(entries)))
    for entry in entries:
        payload += INDEX_RECORD.pack(entry.x, entry.y,
                                     float(entry.min_height), float(entry.max_height))

    path = os.path.join(root, str(level), INDEX_FILENAME)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    temporary = f"{path}.tmp"
    with open(temporary, "wb") as handle:
        handle.write(payload)
        handle.flush()
        os.fsync(handle.fileno())
    os.replace(temporary, path)
    return path


def read_level_index(root: str, level: int) -> list[TileEntry]:
    path = os.path.join(root, str(level), INDEX_FILENAME)
    with open(path, "rb") as handle:
        blob = handle.read()

    magic, version, _reserved, stored_level, count = INDEX_HEADER.unpack_from(blob, 0)
    if magic != INDEX_MAGIC:
        raise ValueError(f"{path}: magic errato {magic!r}")
    if version != INDEX_VERSION:
        raise ValueError(f"{path}: versione indice {version}, attesa {INDEX_VERSION}")
    if stored_level != level:
        raise ValueError(f"{path}: l'indice dichiara il livello {stored_level}, atteso {level}")

    expected = INDEX_HEADER.size + count * INDEX_RECORD.size
    if len(blob) != expected:
        raise ValueError(f"{path}: {len(blob)} byte, attesi {expected} per {count} voci")

    return [TileEntry(*INDEX_RECORD.unpack_from(blob, INDEX_HEADER.size + i * INDEX_RECORD.size))
            for i in range(count)]


def build_manifest(*, dataset_name: str, bbox: tuple[float, float, float, float],
                   levels: list[LevelSummary], source: dict, vertical: dict,
                   pipeline_version: str) -> dict:
    west, south, east, north = bbox
    return {
        "formatVersion": 1,
        "generator": f"geoworld-pipeline {pipeline_version}",
        "datasetName": dataset_name,

        # Schema di tiling: tutto cio' che serve per ricostruire la geometria di
        # una tile senza aver letto questo codice.
        "tilingScheme": {
            "type": "geodetic-wgs84",
            "crs": "EPSG:4326",
            "verticalCrs": "EPSG:4979 (quote ellissoidiche WGS84)",
            "level0TilesX": tiling.tiles_x(0),
            "level0TilesY": tiling.tiles_y(0),
            "xAxis": "est, x=0 a lon -180",
            "yAxis": "sud, y=0 a lat +90",
        },

        "tileFormat": {
            "extension": tileformat.TILE_EXTENSION,
            "path": "<level>/<x>/<y>" + tileformat.TILE_EXTENSION,
            "posts": tiling.TILE_POSTS,
            "overlapPosts": 1,
            "registration": "gridline (i post di bordo sono condivisi con le tile adiacenti)",
            "sampleType": "float32",
            "byteOrder": "little-endian",
            "headerBytes": tileformat.HEADER_SIZE,
            "bytesPerTile": tileformat.TILE_BYTES,
            "rowOrder": "riga 0 = nord",
            "columnOrder": "colonna 0 = ovest",
        },

        "boundingBox": {"west": west, "south": south, "east": east, "north": north},

        # Provenienza: senza, fra sei mesi non si sa piu' con quali dati e con
        # quale griglia geoidica e' stato generato il dataset.
        "source": source,
        "verticalDatum": vertical,

        "levels": [asdict(level) for level in levels],
        "levelIndex": {
            "path": "<level>/" + INDEX_FILENAME,
            "recordBytes": INDEX_RECORD.size,
            "fields": ["uint32 x", "uint32 y", "float32 minHeight", "float32 maxHeight"],
            "sortedBy": "(y, x)",
        },
    }


def write_manifest(root: str, manifest: dict) -> str:
    path = os.path.join(root, MANIFEST_FILENAME)
    os.makedirs(root, exist_ok=True)
    temporary = f"{path}.tmp"
    with open(temporary, "w", encoding="utf-8") as handle:
        json.dump(manifest, handle, indent="\t", ensure_ascii=False)
        handle.write("\n")
    os.replace(temporary, path)
    return path


def read_manifest(root: str) -> dict:
    with open(os.path.join(root, MANIFEST_FILENAME), encoding="utf-8") as handle:
        return json.load(handle)

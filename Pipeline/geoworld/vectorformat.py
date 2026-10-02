"""
Formato delle tile vettoriali (.gvt) della Fase 8, con indice e manifest.

PERCHE' VETTORIALI E NON GIA' DISEGNATE
---------------------------------------
Si potevano disegnare le strade qui, nella pipeline, e salvarle come immagini
accanto alle ortofoto. Non si fa per due ragioni di numeri.

1. **La risoluzione.** Una strada larga 6 m, vista da 300 m di quota, vuole
   pixel da mezzo metro: livello 17-18. Le tile di livello 18 che toccano una
   strada, in Italia, sono decine di milioni di file.
2. **Lo stile.** Un'immagine ha il colore deciso al momento del taglio. Una
   linea vettoriale si ridisegna come si vuole: realistica, a colori da carta
   stradale per controllare l'allineamento, piu' larga, piu' trasparente.

Le tile vettoriali invece si fermano al livello 13 (~2 km di lato) e pesano
pochissimo: e' il runtime a disegnarle, alla risoluzione che serve in quel
momento, per ogni tile di terreno.

IL CONTENUTO DI UNA TILE
------------------------
Header di 32 byte, poi un record di 12 byte per linea, poi tutti i punti.

    header   "GWVT", versione, flag, livello, x, y, extent, buffer,
             numero di linee, numero di punti
    linea    classe (u8), flag (u8), layer (i8), riservato (u8),
             larghezza in decimetri (u16), riservato (u16), numero di punti (u32)
    punti    (x, y) in int16, per tutte le linee una dopo l'altra

Le coordinate sono LOCALI alla tile: x = 0 sul bordo ovest, x = extent sul
bordo est; y = 0 sul bordo NORD, y = extent sul bordo sud. Nord in alto, come
le righe delle immagini e delle quote: nessuna inversione da ricordare.

Con extent = 16384 una tile di livello 13 (~2,4 km in latitudine) ha un passo
di 15 cm: sotto la precisione di qualunque dato OSM.

Le linee escono dalla tile di un BUFFER (un ottavo del lato, cioe' 2048 unita'):
una strada che corre lungo il bordo, larga 20 m, deve comparire anche nella
tile accanto, altrimenti meta' della sua larghezza sparirebbe sul confine.
L'int16 arriva fino a -32768 e 32767: c'e' spazio per un buffer di una tile
intera da ogni parte.
"""

from __future__ import annotations

import json
import os
import struct
from dataclasses import dataclass, field, asdict

import numpy as np

from . import roadclasses, tiling

TILE_EXTENSION = ".gvt"
MAGIC = b"GWVT"
FORMAT_VERSION = 1
HEADER = struct.Struct("<4sHHIIIHHII")          # 32 byte
FEATURE = struct.Struct("<BBbBHHI")              # 12 byte

#: Lato della tile in unita' locali. Potenza di due: il ritaglio di una tile di
#: terreno piu' profonda (un quarto, un sedicesimo...) cade su numeri interi.
EXTENT = 16384

#: Quanto le linee possono uscire dalla tile, in unita' (un ottavo del lato).
BUFFER = EXTENT // 8

INDEX_MAGIC = b"GWVI"
INDEX_VERSION = 1
INDEX_HEADER = struct.Struct("<4sHHII")          # magic, versione, riservato, livello, numero
INDEX_RECORD = struct.Struct("<III")             # x, y, numero di linee
INDEX_FILENAME = "index.bin"
MANIFEST_FILENAME = "manifest.json"

INT16_MIN = -32768
INT16_MAX = 32767


@dataclass
class Feature:
    class_id: int
    flags: int
    layer: int
    width_dm: int
    #: (N, 2) int16, coordinate locali della tile.
    points: np.ndarray


@dataclass
class VectorTile:
    level: int
    x: int
    y: int
    extent: int
    buffer: int
    features: list[Feature] = field(default_factory=list)

    @property
    def point_count(self) -> int:
        return sum(len(feature.points) for feature in self.features)


def tile_relative_path(level: int, x: int, y: int) -> str:
    """Percorso relativo alla root del dataset: <level>/<x>/<y>.gvt"""
    return os.path.join(str(level), str(x), f"{y}{TILE_EXTENSION}")


def encode_tile(level: int, x: int, y: int, features: list[Feature]) -> bytes:
    """Serializza una tile. Le linee si scrivono nell'ordine dato: e' l'ordine di disegno."""
    records = bytearray()
    points = []
    total = 0
    for feature in features:
        coords = np.asarray(feature.points)
        if coords.ndim != 2 or coords.shape[1] != 2 or len(coords) < 2:
            raise ValueError(f"linea con forma {coords.shape}: servono almeno due punti (N, 2)")
        if coords.min() < INT16_MIN or coords.max() > INT16_MAX:
            raise ValueError("coordinate fuori dall'int16: la linea non e' stata ritagliata")
        records += FEATURE.pack(feature.class_id, feature.flags, feature.layer, 0,
                                feature.width_dm, 0, len(coords))
        points.append(coords.astype("<i2", copy=False).tobytes())
        total += len(coords)

    header = HEADER.pack(MAGIC, FORMAT_VERSION, 0, level, x, y, EXTENT, BUFFER,
                         len(features), total)
    return header + bytes(records) + b"".join(points)


def decode_tile(blob: bytes) -> VectorTile:
    if len(blob) < HEADER.size:
        raise ValueError(f"blob di {len(blob)} byte: piu' corto dell'header")
    (magic, version, _flags, level, x, y, extent, buffer,
     feature_count, point_count) = HEADER.unpack_from(blob, 0)
    if magic != MAGIC:
        raise ValueError(f"magic {magic!r}, atteso {MAGIC!r}: non e' una tile vettoriale")
    if version != FORMAT_VERSION:
        raise ValueError(f"versione {version}, attesa {FORMAT_VERSION}")

    expected = HEADER.size + feature_count * FEATURE.size + point_count * 4
    if len(blob) != expected:
        raise ValueError(f"{len(blob)} byte, attesi {expected} per {feature_count} linee "
                         f"e {point_count} punti")

    tile = VectorTile(level, x, y, extent, buffer)
    coords = np.frombuffer(blob, dtype="<i2",
                           offset=HEADER.size + feature_count * FEATURE.size).reshape(-1, 2)
    cursor = 0
    for index in range(feature_count):
        class_id, flags, layer, _r0, width_dm, _r1, count = FEATURE.unpack_from(
            blob, HEADER.size + index * FEATURE.size)
        if cursor + count > point_count:
            raise ValueError(f"la linea {index} dichiara piu' punti di quelli presenti")
        tile.features.append(Feature(class_id, flags, layer, width_dm,
                                     coords[cursor:cursor + count]))
        cursor += count
    if cursor != point_count:
        raise ValueError(f"le linee usano {cursor} punti, l'header ne dichiara {point_count}")
    return tile


def write_tile_atomic(path: str, blob: bytes) -> None:
    """Prima un .tmp, poi la rinomina: un Ctrl-C non lascia mai un file a meta'."""
    os.makedirs(os.path.dirname(path), exist_ok=True)
    temporary = path + ".tmp"
    with open(temporary, "wb") as handle:
        handle.write(blob)
    os.replace(temporary, path)


def read_tile(path: str) -> VectorTile:
    with open(path, "rb") as handle:
        return decode_tile(handle.read())


# --- Da gradi a unita' locali e ritorno -------------------------------------

def lonlat_to_local(level: int, x: int, y: int, lon: np.ndarray, lat: np.ndarray):
    """Gradi -> unita' locali della tile (float: l'arrotondamento lo fa chi scrive)."""
    bounds = tiling.tile_bounds(level, x, y)
    span = tiling.tile_span_deg(level)
    return ((lon - bounds.west) / span * EXTENT, (bounds.north - lat) / span * EXTENT)


def local_to_lonlat(level: int, x: int, y: int, u: np.ndarray, v: np.ndarray):
    bounds = tiling.tile_bounds(level, x, y)
    span = tiling.tile_span_deg(level)
    return (bounds.west + np.asarray(u, dtype=float) / EXTENT * span,
            bounds.north - np.asarray(v, dtype=float) / EXTENT * span)


# --- Indice di livello --------------------------------------------------------

@dataclass
class VectorTileEntry:
    x: int
    y: int
    feature_count: int


def write_level_index(root: str, level: int, entries: list[VectorTileEntry]) -> str:
    """Scrive <root>/<level>/index.bin, ordinato per (y, x) come gli altri indici."""
    entries = sorted(entries, key=lambda e: (e.y, e.x))
    payload = bytearray(INDEX_HEADER.pack(INDEX_MAGIC, INDEX_VERSION, 0, level, len(entries)))
    for entry in entries:
        payload += INDEX_RECORD.pack(entry.x, entry.y, entry.feature_count)

    path = os.path.join(root, str(level), INDEX_FILENAME)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    temporary = f"{path}.tmp"
    with open(temporary, "wb") as handle:
        handle.write(payload)
    os.replace(temporary, path)
    return path


def read_level_index(root: str, level: int) -> list[VectorTileEntry]:
    path = os.path.join(root, str(level), INDEX_FILENAME)
    with open(path, "rb") as handle:
        blob = handle.read()
    magic, version, _reserved, stored_level, count = INDEX_HEADER.unpack_from(blob, 0)
    if magic != INDEX_MAGIC:
        raise ValueError(f"{path}: magic {magic!r}, atteso {INDEX_MAGIC!r}: "
                         "non e' l'indice di un dataset vettoriale")
    if version != INDEX_VERSION:
        raise ValueError(f"{path}: versione {version}, attesa {INDEX_VERSION}")
    if stored_level != level:
        raise ValueError(f"{path}: dichiara il livello {stored_level}, atteso {level}")
    if len(blob) != INDEX_HEADER.size + count * INDEX_RECORD.size:
        raise ValueError(f"{path}: dimensione incoerente con {count} voci")
    return [VectorTileEntry(*INDEX_RECORD.unpack_from(blob, INDEX_HEADER.size + i * INDEX_RECORD.size))
            for i in range(count)]


# --- Manifest ---------------------------------------------------------------

def build_manifest(*, dataset_name: str, bbox: tuple[float, float, float, float],
                   levels: list[dict], source: dict, class_counts: dict[int, int],
                   pipeline_version: str) -> dict:
    west, south, east, north = bbox
    return {
        "formatVersion": 1,
        "generator": f"geoworld-pipeline {pipeline_version}",
        "datasetName": dataset_name,
        "datasetKind": "vector",
        "content": "linee: strade, ferrovie, piste",
        "tilingScheme": {
            "type": "geodetic-wgs84",
            "crs": "EPSG:4326",
            "level0TilesX": tiling.tiles_x(0),
            "level0TilesY": tiling.tiles_y(0),
        },
        "tileFormat": {
            "extension": TILE_EXTENSION,
            "path": "<level>/<x>/<y>" + TILE_EXTENSION,
            "headerBytes": HEADER.size,
            "featureRecordBytes": FEATURE.size,
            "extent": EXTENT,
            "buffer": BUFFER,
            "coordinates": "int16 locali: x=0 bordo ovest, y=0 bordo NORD",
        },
        "classificationVersion": roadclasses.CLASSIFICATION_VERSION,
        "classes": [{"id": c.id, "name": c.name, "minLevel": c.min_level,
                     "defaultWidthM": c.default_width_m,
                     "features": class_counts.get(c.id, 0)}
                    for c in roadclasses.CLASSES],
        "boundingBox": {"west": west, "south": south, "east": east, "north": north},
        "source": source,
        "levels": levels,
        "levelIndex": {
            "path": "<level>/" + INDEX_FILENAME,
            "magic": INDEX_MAGIC.decode("ascii"),
            "recordBytes": INDEX_RECORD.size,
            "fields": ["uint32 x", "uint32 y", "uint32 featureCount"],
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


def entry_as_dict(entry: VectorTileEntry) -> dict:
    return asdict(entry)


# --- Tile di riferimento per i test C++ -------------------------------------
#
# Il lettore C++ (VectorTileFormat.h) si prova su un file scritto da QUESTO
# codice, non su byte scritti a mano nel test: e' l'unico modo di sapere che i
# due lati leggono e scrivono la stessa cosa. Il file sta in
# Source/GeoTiles/TestData/vector_fixture.gvt e un test Python controlla che
# coincida con quello che si rigenera qui.

FIXTURE_KEY = (13, 8538, 2044)       # sopra Torino


def fixture_features() -> list[Feature]:
    half = EXTENT // 2
    return [
        # Un'autostrada orizzontale a meta' tile, da bordo a bordo.
        Feature(1, 0, 0, 110, np.array([[0, half], [EXTENT, half]], dtype=np.int16)),
        # Una ferrovia su ponte, verticale, che esce dalla tile nel buffer.
        Feature(20, roadclasses.FLAG_BRIDGE, 1, 45,
                np.array([[half, -1000], [half, EXTENT + 1000]], dtype=np.int16)),
        # Un sentiero sterrato di tre punti nell'angolo nord-ovest.
        Feature(12, roadclasses.FLAG_UNPAVED, 0, 15,
                np.array([[1000, 1000], [4000, 2000], [6000, 6000]], dtype=np.int16)),
    ]


def fixture_bytes() -> bytes:
    level, x, y = FIXTURE_KEY
    return encode_tile(level, x, y, fixture_features())

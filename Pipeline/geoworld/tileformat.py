"""
Formato su disco di una tile di quote: ".ght" (GeoWorld HeighT).

--------------------------------------------------------------------------
PERCHE' NON ".hgt"
--------------------------------------------------------------------------
Nella specifica di partenza l'estensione era ".hgt". L'ho cambiata, e questo e'
l'unico punto in cui mi discosto dallo schema di percorsi concordato
(<root>/<level>/<x>/<y>.<est> resta identico).

Motivo: ".hgt" identifica gia' il formato SRTM, che e' int16 BIG-endian, senza
header, di dimensione 1201x1201 o 3601x3601, con latitudine e longitudine
codificate nel NOME del file. GDAL ha un driver SRTMHGT che si attiva proprio
sull'estensione. Un nostro file, che e' float32 little-endian 129x129 con
header, verrebbe aperto da quel driver e interpretato come quote intere
big-endian: nessun errore, solo numeri senza senso. Un'estensione diversa fa
fallire il tentativo in modo pulito invece di produrre spazzatura plausibile.

Se preferisci tornare a ".hgt" e' una costante sola: TILE_EXTENSION.

--------------------------------------------------------------------------
LAYOUT (little-endian, header di 32 byte, tutti i campi allineati)
--------------------------------------------------------------------------
    off  size  campo
      0     4  magic "GWHT"
      4     2  version            uint16
      6     2  flags              uint16   bit0 = contiene post riempiti
      8     4  level              uint32
     12     4  tileX              uint32
     16     4  tileY              uint32
     20     2  width              uint16   (129)
     22     2  height             uint16   (129)
     24     4  minHeight          float32  metri, quota ELLISSOIDICA
     28     4  maxHeight          float32
     32     -  dati: height*width float32, riga 0 = NORD, colonna 0 = OVEST

PERCHE' UN HEADER e non un file grezzo come SRTM:
 - il loader valida quello che apre. Il caso che si verifica davvero e'
   rigenerare la piramide con parametri diversi lasciando sul disco i vecchi
   file: senza header il runtime legge dati vecchi con geometria nuova e il
   terreno risulta sottilmente sbagliato, il che costa giorni di debug;
 - min/max arrivano insieme al dato, senza una seconda lettura;
 - il campo version permette di cambiare il formato senza ambiguita'.
Costa 32 byte su 66596, cioe' lo 0.05%.

PERCHE' float32 e non int16 come SRTM:
int16 in metri costerebbe meta' spazio ma quantizzerebbe a 1 m. Le nostre quote
sono ELLISSOIDICHE, cioe' ~45 m piu' alte di quelle ortometriche in Italia, e
soprattutto la quantizzazione a 1 m produce terrazzamenti visibili sulle
pendenze dolci, esattamente dove l'occhio li nota. Si potrebbe fare int16 con
scala e offset per tile; e' un'ottimizzazione che ha senso solo dopo aver
misurato che la banda su disco e' il collo di bottiglia, non prima.
"""

from __future__ import annotations

import os
import struct
from dataclasses import dataclass

import numpy as np

from . import tiling

TILE_EXTENSION = ".ght"

MAGIC = b"GWHT"
FORMAT_VERSION = 1
HEADER_SIZE = 32
HEADER_STRUCT = struct.Struct("<4sHHIIIHHff")

#: bit0 dei flags: la tile contiene post riempiti (mare o fuori dal dato sorgente).
FLAG_HAS_FILLED_POSTS = 1 << 0

TILE_BYTES = HEADER_SIZE + tiling.TILE_POSTS * tiling.TILE_POSTS * 4


@dataclass(frozen=True)
class TileHeader:
    version: int
    flags: int
    level: int
    tile_x: int
    tile_y: int
    width: int
    height: int
    min_height: float
    max_height: float

    @property
    def has_filled_posts(self) -> bool:
        return bool(self.flags & FLAG_HAS_FILLED_POSTS)


def tile_relative_path(level: int, x: int, y: int) -> str:
    """Percorso relativo alla root del dataset: <level>/<x>/<y>.ght"""
    return os.path.join(str(level), str(x), f"{y}{TILE_EXTENSION}")


def encode_tile(level: int, x: int, y: int, heights: np.ndarray,
                has_filled_posts: bool) -> bytes:
    """
    Serializza una tile. `heights` deve essere (129, 129) float32, riga 0 a NORD.

    Nessun NaN e' ammesso: a questo punto della pipeline ogni post ha una quota,
    perche' i post senza dato sorgente sono gia' stati riempiti con la quota
    ellissoidica del livello medio del mare. Un NaN qui significa un bug a monte,
    e propagarlo nel runtime lo renderebbe molto piu' difficile da trovare.
    """
    if heights.shape != (tiling.TILE_POSTS, tiling.TILE_POSTS):
        raise ValueError(
            f"attesa una tile {tiling.TILE_POSTS}x{tiling.TILE_POSTS}, ricevuta {heights.shape}")

    heights = np.ascontiguousarray(heights, dtype="<f4")

    if not np.isfinite(heights).all():
        raise ValueError(
            f"tile {level}/{x}/{y}: contiene valori non finiti (NaN o infiniti). "
            "Il riempimento dei post senza dato non e' stato applicato.")

    flags = FLAG_HAS_FILLED_POSTS if has_filled_posts else 0

    header = HEADER_STRUCT.pack(
        MAGIC, FORMAT_VERSION, flags, level, x, y,
        tiling.TILE_POSTS, tiling.TILE_POSTS,
        float(heights.min()), float(heights.max()))

    return header + heights.tobytes()


def decode_tile(blob: bytes) -> tuple[TileHeader, np.ndarray]:
    """Deserializza e VALIDA. Solleva ValueError su qualunque incoerenza."""
    if len(blob) < HEADER_SIZE:
        raise ValueError(f"file troncato: {len(blob)} byte, attesi almeno {HEADER_SIZE}")

    (magic, version, flags, level, tile_x, tile_y,
     width, height, min_height, max_height) = HEADER_STRUCT.unpack_from(blob, 0)

    if magic != MAGIC:
        raise ValueError(f"magic errato: {magic!r} invece di {MAGIC!r} (non e' una tile GeoWorld)")
    if version != FORMAT_VERSION:
        raise ValueError(f"versione del formato {version}, questo codice legge la {FORMAT_VERSION}")

    expected = HEADER_SIZE + width * height * 4
    if len(blob) != expected:
        raise ValueError(
            f"dimensione incoerente: {len(blob)} byte, attesi {expected} per {width}x{height}")

    heights = np.frombuffer(blob, dtype="<f4", count=width * height,
                            offset=HEADER_SIZE).reshape((height, width))

    header = TileHeader(version=version, flags=flags, level=level, tile_x=tile_x,
                        tile_y=tile_y, width=width, height=height,
                        min_height=min_height, max_height=max_height)
    return header, heights


def write_tile_atomic(path: str, blob: bytes) -> None:
    """
    Scrive con rename atomico.

    E' il meccanismo che rende la pipeline davvero RIAVVIABILE. Scrivendo
    direttamente sul file finale, un'interruzione (Ctrl-C, disco pieno, crash)
    lascia sul disco un file di dimensione giusta ma contenuto parziale: al
    riavvio il controllo "esiste ed e' della dimensione attesa" lo considera
    buono e quella tile resta corrotta per sempre. Con scrittura su .tmp e
    os.replace, il file finale o non esiste o e' completo: non esistono stati
    intermedi.
    """
    os.makedirs(os.path.dirname(path), exist_ok=True)
    temporary = f"{path}.tmp"
    with open(temporary, "wb") as handle:
        handle.write(blob)
        handle.flush()
        os.fsync(handle.fileno())
    os.replace(temporary, path)


def decode_header(blob: bytes) -> TileHeader:
    """Legge e valida il solo header. Usata dal controllo di riavviabilita',
    che deve stabilire se una tile e' gia' a posto senza rileggerne i 66 kB."""
    if len(blob) < HEADER_SIZE:
        raise ValueError(f"header troncato: {len(blob)} byte, attesi {HEADER_SIZE}")

    (magic, version, flags, level, tile_x, tile_y,
     width, height, min_height, max_height) = HEADER_STRUCT.unpack_from(blob, 0)

    if magic != MAGIC:
        raise ValueError(f"magic errato: {magic!r} invece di {MAGIC!r}")
    if version != FORMAT_VERSION:
        raise ValueError(f"versione del formato {version}, questo codice legge la {FORMAT_VERSION}")

    return TileHeader(version=version, flags=flags, level=level, tile_x=tile_x,
                      tile_y=tile_y, width=width, height=height,
                      min_height=min_height, max_height=max_height)


def read_tile_header(path: str) -> TileHeader:
    with open(path, "rb") as handle:
        return decode_header(handle.read(HEADER_SIZE))


def read_tile(path: str) -> tuple[TileHeader, np.ndarray]:
    with open(path, "rb") as handle:
        return decode_tile(handle.read())

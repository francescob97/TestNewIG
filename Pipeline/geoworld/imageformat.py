"""
Formato delle tile di immagine (.gim).

PERCHE' UN FORMATO NOSTRO E NON UN JPEG NUDO
--------------------------------------------
Un JPEG da solo non dice a quale tile appartiene, quanta parte del rettangolo
copre davvero, ne' come e' compresso il payload. Sono tre informazioni che il
runtime deve conoscere PRIMA di decodificare: la prima per accorgersi di un file
fuori posto, la seconda per sapere se la tile va disegnata, la terza per non
dover indovinare il codec.

Un header di 32 byte davanti al payload costa niente e rende il file
auto-descrittivo. E' la stessa scelta fatta per le quote in tileformat.py, con
lo stesso identico argomento.

IL BYTE DEL TIPO DI PAYLOAD
---------------------------
Oggi c'e' solo JPEG. Il byte esiste comunque, perche' l'evoluzione prevedibile
e' BC1 (la compressione che la GPU consuma senza decomprimere) e non si vuole
cambiare formato per aggiungerla: si aggiunge un valore e un ramo nel lettore.
"""

from __future__ import annotations

import io
import os
import struct
from dataclasses import dataclass

import numpy as np

from . import tiling


def _pillow_image():
    """
    Importa Pillow SOLO quando serve davvero.

    PERCHE' NON IN CIMA AL FILE. Un import in testa al modulo si propaga a tutta
    la catena: cli.py importa imagecut, che importa questo, che importerebbe
    Pillow. Risultato, senza Pillow installato NON PARTE NEMMENO check-env --
    cioe' proprio il comando il cui unico scopo e' dirti cosa manca
    nell'ambiente. Uno strumento di diagnosi che muore per la cosa che deve
    diagnosticare e' inutile.

    E' lo stesso motivo per cui GDAL viene importato dentro le funzioni in
    raster.py, e la stessa lezione gia' imparata una volta: gli strumenti di
    diagnosi devono essere la parte piu' robusta del progetto, non la piu'
    fragile.
    """
    try:
        from PIL import Image
    except ImportError as error:
        raise ImportError(
            "serve Pillow per leggere e scrivere le tile di immagine.\n"
            "    conda install -c conda-forge pillow\n"
            "    (oppure: pip install Pillow)\n"
            "Diagnosi completa dell'ambiente: python run.py check-env"
        ) from error
    return Image

TILE_EXTENSION = ".gim"

MAGIC = b"GWIM"
FORMAT_VERSION = 1
HEADER_SIZE = 32
HEADER_STRUCT = struct.Struct("<4sHHIIIHHBBHI")

#: Lato della tile in pixel.
#:
#: 256 e non 129 come le quote, e la differenza non e' arbitraria:
#:   - i post delle quote sono registrati SUI NODI e il bordo e' condiviso con
#:     la tile vicina (129 = 128 + 1), perche' le mesh devono combaciare;
#:   - i pixel di un'immagine sono registrati SULLE AREE e coprono il rettangolo
#:     senza sovrapposizione, perche' una colonna condivisa verrebbe disegnata
#:     due volte.
#: 256 e' potenza di due (mipmap, riduzione 2x2 esatta) e da' esattamente due
#: pixel per cella di terreno, dato che la mesh ha 128 celle per lato.
TILE_PIXELS = 256

PAYLOAD_JPEG = 0
PAYLOAD_PNG = 1
PAYLOAD_BC1 = 2          # non ancora prodotto: vedi la nota in testa al file

PAYLOAD_NAMES = {PAYLOAD_JPEG: "jpeg", PAYLOAD_PNG: "png", PAYLOAD_BC1: "bc1"}

#: bit0: la tile contiene pixel di riempimento (copertura parziale).
FLAG_HAS_FILLED_PIXELS = 1 << 0

#: Colore dei pixel senza dato. Un grigio neutro: non si confonde con il
#: terreno vero e non attira l'occhio come farebbe il magenta.
FILL_COLOUR = (128, 128, 128)

DEFAULT_QUALITY = 85


@dataclass(frozen=True)
class ImageTileHeader:
    version: int
    flags: int
    level: int
    tile_x: int
    tile_y: int
    width: int
    height: int
    payload_type: int
    coverage_percent: int
    payload_size: int

    @property
    def has_filled_pixels(self) -> bool:
        return bool(self.flags & FLAG_HAS_FILLED_PIXELS)

    @property
    def payload_name(self) -> str:
        return PAYLOAD_NAMES.get(self.payload_type, f"sconosciuto({self.payload_type})")


def tile_relative_path(level: int, x: int, y: int) -> str:
    """Percorso relativo alla root del dataset: <level>/<x>/<y>.gim"""
    return os.path.join(str(level), str(x), f"{y}{TILE_EXTENSION}")


def encode_tile(level: int, x: int, y: int, pixels: np.ndarray,
                coverage_percent: float,
                quality: int = DEFAULT_QUALITY) -> bytes:
    """
    Serializza una tile. `pixels` deve essere (256, 256, 3) uint8, riga 0 a NORD.

    La riga 0 a nord non e' una convenzione a caso: e' la stessa delle quote, ed
    e' anche l'ordine in cui la texture viene caricata in memoria video e quello
    in cui crescono le UV della mesh (J cresce verso sud). Tre versi concordi
    significano nessuna inversione da ricordare, in nessun punto della catena.
    """
    expected = (TILE_PIXELS, TILE_PIXELS, 3)
    if pixels.shape != expected:
        raise ValueError(f"attesa una tile {expected}, ricevuta {pixels.shape}")
    if pixels.dtype != np.uint8:
        raise ValueError(f"attesi pixel uint8, ricevuti {pixels.dtype}")

    coverage = int(round(max(0.0, min(100.0, coverage_percent))))

    Image = _pillow_image()

    buffer = io.BytesIO()
    Image.fromarray(pixels, mode="RGB").save(
        buffer, format="JPEG", quality=quality, optimize=True)
    payload = buffer.getvalue()

    flags = FLAG_HAS_FILLED_PIXELS if coverage < 100 else 0

    header = HEADER_STRUCT.pack(
        MAGIC, FORMAT_VERSION, flags, level, x, y,
        TILE_PIXELS, TILE_PIXELS,
        PAYLOAD_JPEG, coverage, 0, len(payload))

    return header + payload


def decode_header(blob: bytes) -> ImageTileHeader:
    if len(blob) < HEADER_SIZE:
        raise ValueError(f"blob troppo corto: {len(blob)} byte, attesi almeno {HEADER_SIZE}")

    (magic, version, flags, level, tile_x, tile_y,
     width, height, payload_type, coverage, _reserved,
     payload_size) = HEADER_STRUCT.unpack_from(blob, 0)

    if magic != MAGIC:
        raise ValueError(f"magic {magic!r}, atteso {MAGIC!r}: non e' una tile di immagine")
    if version != FORMAT_VERSION:
        raise ValueError(f"versione {version}, attesa {FORMAT_VERSION}")

    return ImageTileHeader(version, flags, level, tile_x, tile_y,
                           width, height, payload_type, coverage, payload_size)


def decode_tile(blob: bytes) -> tuple[ImageTileHeader, np.ndarray]:
    header = decode_header(blob)

    if header.payload_type != PAYLOAD_JPEG:
        raise ValueError(f"payload {header.payload_name}: questo lettore conosce solo jpeg")

    payload = blob[HEADER_SIZE:HEADER_SIZE + header.payload_size]
    if len(payload) != header.payload_size:
        raise ValueError(
            f"payload troncato: {len(payload)} byte su {header.payload_size} dichiarati")

    Image = _pillow_image()

    image = Image.open(io.BytesIO(payload))
    image.load()
    if image.mode != "RGB":
        image = image.convert("RGB")

    pixels = np.asarray(image, dtype=np.uint8)
    if pixels.shape != (header.height, header.width, 3):
        raise ValueError(
            f"immagine {pixels.shape}, attesa ({header.height}, {header.width}, 3)")

    return header, pixels


def write_tile_atomic(path: str, blob: bytes) -> None:
    """
    Scrive prima un .tmp e poi rinomina.

    Stessa ragione delle quote: la pipeline deve essere riavviabile. Un file
    interrotto a meta' da un Ctrl-C sarebbe indistinguibile da uno completo, e
    al riavvio verrebbe saltato perche' "esiste gia'". os.replace e' atomica su
    tutti i sistemi che ci interessano.
    """
    os.makedirs(os.path.dirname(path), exist_ok=True)
    temporary = path + ".tmp"
    with open(temporary, "wb") as handle:
        handle.write(blob)
    os.replace(temporary, path)


def read_tile_header(path: str) -> ImageTileHeader:
    with open(path, "rb") as handle:
        return decode_header(handle.read(HEADER_SIZE))


def read_tile(path: str) -> tuple[ImageTileHeader, np.ndarray]:
    with open(path, "rb") as handle:
        return decode_tile(handle.read())


def fill_tile() -> np.ndarray:
    """Una tile interamente di riempimento: serve dove non c'e' sorgente."""
    tile = np.empty((TILE_PIXELS, TILE_PIXELS, 3), dtype=np.uint8)
    tile[:, :] = FILL_COLOUR
    return tile


def reduce_2x2(pixels: np.ndarray) -> np.ndarray:
    """
    Riduce un'immagine a meta' lato facendo la media di ogni blocco 2x2.

    PERCHE' UNA MEDIA SEMPLICE E NON IL KERNEL DELLA PIRAMIDE DELLE QUOTE.
    Per le quote serviva un kernel [1,4,6,4,1]/16 CENTRATO SUL POST CONSERVATO,
    perche' i post sono punti: prendere la media di un quadrato 2x2 avrebbe
    spostato il terreno di mezzo passo a ogni livello.

    I pixel invece sono aree. Il pixel del livello grossolano E' l'unione esatta
    dei quattro pixel figli, e la media dei quattro e' il suo valore corretto per
    costruzione. Nessuno spostamento, nessun kernel.

    La media si calcola in uint16 e non in uint8: quattro valori a 255 sommati
    fanno 1020, che in uint8 andrebbe in overflow silenzioso.
    """
    height, width = pixels.shape[:2]
    if height % 2 or width % 2:
        raise ValueError(f"lati dispari: {pixels.shape}")

    wide = pixels.astype(np.uint16)
    blocks = wide.reshape(height // 2, 2, width // 2, 2, -1)
    return blocks.mean(axis=(1, 3)).round().astype(np.uint8)

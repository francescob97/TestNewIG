"""
Le stelle vere (Fase 12): dal catalogo di Yale a un file compatto per il cielo.

DA DOVE VENGONO
---------------
Dal **Bright Star Catalogue** di Yale, quinta edizione (Hoffleit e Warren,
1991): le 9.110 stelle piu' luminose del cielo, fino alla magnitudine 6,5,
cioe' tutte quelle che si vedono a occhio nudo nel cielo piu' buio. E' di
dominio pubblico; lo distribuisce il CDS di Strasburgo come catalogo V/50
(`catalog.gz`, ~0,4 MB, testo a colonne fisse).

Di ogni stella si tengono quattro numeri: la posizione (ascensione retta e
declinazione all'equinozio J2000), la magnitudine visuale (quanto e' luminosa)
e l'indice di colore B-V (di che colore e'). Il resto del catalogo (nomi,
spettri, moti propri, doppie) al cielo di GeoWorld non serve.

IL FILE (.gst)
--------------
    header   16 byte: "GWST", versione (u16), riservato (u16), numero di
             stelle (u32), equinozio (u32, 2000)
    stelle   12 byte ciascuna: ascensione retta in gradi (f32), declinazione
             in gradi (f32), magnitudine x 100 (i16), B-V x 1000 (i16)

Dalla piu' luminosa alla meno luminosa: chi ne vuole meno legge le prime.
Un float32 per l'ascensione retta e' preciso a 0,08 secondi d'arco, mille
volte piu' del pixel piu' piccolo.

Il lettore C++ e' Source/GeoRender/Public/Sky/Stars.h; un test Python
controlla che il file di prova dei test C++ (TestData/stars_fixture.gst)
coincida con quello che scrive questo modulo.
"""

from __future__ import annotations

import gzip
import math
import os
import struct
from dataclasses import dataclass

CATALOG_URL = "https://cdsarc.cds.unistra.fr/ftp/V/50/catalog.gz"
CATALOG_FILENAME = "bsc5_catalog.gz"

MAGIC = b"GWST"
FORMAT_VERSION = 1
EQUINOX = 2000
HEADER = struct.Struct("<4sHHII")      # 16 byte
RECORD = struct.Struct("<ffhh")        # 12 byte

#: B-V delle (poche) stelle senza colore nel catalogo: quello del sole.
DEFAULT_COLOUR_INDEX = 0.65

#: Fin dove si tiene: il catalogo arriva a ~6,5 (il cielo piu' buio a occhio nudo).
DEFAULT_MAX_MAGNITUDE = 6.5


@dataclass(frozen=True)
class Star:
    ra_deg: float
    dec_deg: float
    magnitude: float
    colour_index: float


# --- Il catalogo di Yale (V/50), a colonne fisse ---------------------------
#
# Le posizioni dei campi, contate da 1 come nel ReadMe del catalogo:
#   76-77 RAh, 78-79 RAm, 80-83 RAs      ascensione retta J2000
#   84 segno, 85-86 DEd, 87-88 DEm, 89-90 DEs   declinazione J2000
#   103-107 Vmag, 110-114 B-V
# Una manciata di voci (novae, oggetti tolti) non ha la posizione: si salta.

def _field(line: str, first: int, last: int) -> str:
    return line[first - 1:last].strip()


def parse_catalog_line(line: str) -> Star | None:
    """Una riga del catalogo, o None se non ha posizione o magnitudine."""
    if len(line) < 107:
        return None
    try:
        hours = _field(line, 76, 77)
        if not hours:
            return None
        ra = (int(hours) + int(_field(line, 78, 79)) / 60.0 + float(_field(line, 80, 83)) / 3600.0) * 15.0
        degrees = int(_field(line, 85, 86)) + int(_field(line, 87, 88)) / 60.0 + int(_field(line, 89, 90)) / 3600.0
        dec = -degrees if line[83] == "-" else degrees
        magnitude = float(_field(line, 103, 107))
    except ValueError:
        return None
    colour_text = _field(line, 110, 114)
    try:
        colour = float(colour_text) if colour_text else DEFAULT_COLOUR_INDEX
    except ValueError:
        colour = DEFAULT_COLOUR_INDEX
    if not (0.0 <= ra < 360.0 and -90.0 <= dec <= 90.0 and math.isfinite(magnitude)):
        return None
    return Star(ra, dec, magnitude, colour)


def read_catalog(path: str, max_magnitude: float = DEFAULT_MAX_MAGNITUDE) -> list[Star]:
    """Le stelle del catalogo (testo o .gz), dalla piu' luminosa."""
    opener = gzip.open if path.endswith(".gz") else open
    stars = []
    with opener(path, "rt", encoding="latin-1") as handle:
        for line in handle:
            star = parse_catalog_line(line.rstrip("\n"))
            if star is not None and star.magnitude <= max_magnitude:
                stars.append(star)
    stars.sort(key=lambda star: (star.magnitude, star.ra_deg, star.dec_deg))
    return stars


# --- Il file .gst ------------------------------------------------------------

def _round(value: float) -> int:
    """Arrotondamento "da scuola" (0,5 in su), come nel resto della pipeline."""
    return int(math.floor(value + 0.5))


def encode(stars: list[Star]) -> bytes:
    payload = bytearray(HEADER.pack(MAGIC, FORMAT_VERSION, 0, len(stars), EQUINOX))
    for star in stars:
        payload += RECORD.pack(star.ra_deg, star.dec_deg,
                               max(-32768, min(32767, _round(star.magnitude * 100.0))),
                               max(-32768, min(32767, _round(star.colour_index * 1000.0))))
    return bytes(payload)


def decode(blob: bytes) -> list[Star]:
    if len(blob) < HEADER.size:
        raise ValueError(f"{len(blob)} byte: piu' corto dell'header")
    magic, version, _reserved, count, equinox = HEADER.unpack_from(blob, 0)
    if magic != MAGIC:
        raise ValueError(f"magic {magic!r}, atteso {MAGIC!r}: non e' un catalogo di stelle")
    if version != FORMAT_VERSION:
        raise ValueError(f"versione {version}, attesa {FORMAT_VERSION}")
    if equinox != EQUINOX:
        raise ValueError(f"equinozio {equinox}, atteso {EQUINOX}")
    if len(blob) != HEADER.size + count * RECORD.size:
        raise ValueError(f"{len(blob)} byte, attesi {HEADER.size + count * RECORD.size} per {count} stelle")
    stars = []
    for index in range(count):
        ra, dec, magnitude, colour = RECORD.unpack_from(blob, HEADER.size + index * RECORD.size)
        stars.append(Star(ra, dec, magnitude / 100.0, colour / 1000.0))
    return stars


def write_atomic(path: str, blob: bytes) -> None:
    directory = os.path.dirname(os.path.abspath(path))
    os.makedirs(directory, exist_ok=True)
    temporary = path + ".tmp"
    with open(temporary, "wb") as handle:
        handle.write(blob)
    os.replace(temporary, path)


# --- Il comando ----------------------------------------------------------------

def build(output: str, catalog: str | None = None, max_magnitude: float = DEFAULT_MAX_MAGNITUDE,
          timeout: float = 120.0, report=print) -> list[Star]:
    """
    Il file delle stelle. Senza `catalog` il catalogo si scarica dal CDS
    (accanto al file di uscita, una volta sola).
    """
    if output.endswith(os.sep) or os.path.isdir(output):
        output = os.path.join(output, "stars.gst")
    if catalog is None:
        catalog = os.path.join(os.path.dirname(os.path.abspath(output)), CATALOG_FILENAME)
        if not os.path.exists(catalog):
            from . import fetchosm      # lo stesso scaricatore degli estratti OSM (riprende, atomico)
            report(f"Scarico il catalogo di Yale: {CATALOG_URL}")
            fetchosm.download(CATALOG_URL, catalog, timeout=timeout, report=report)
    if not os.path.exists(catalog):
        raise FileNotFoundError(f"catalogo non trovato: {catalog}")

    stars = read_catalog(catalog, max_magnitude)
    if not stars:
        raise ValueError(f"nessuna stella in {catalog}: e' il catalogo V/50 (Bright Star Catalogue)?")
    write_atomic(output, encode(stars))
    brightest = stars[0]
    report(f"Stelle: {len(stars)} fino alla magnitudine {max_magnitude:.1f} -> {output} "
           f"({os.path.getsize(output) / 1024:.0f} KB)")
    report(f"  la piu' luminosa: magnitudine {brightest.magnitude:.2f} "
           f"(ascensione retta {brightest.ra_deg:.3f}, declinazione {brightest.dec_deg:.3f}: Sirio e' -1,46)")
    return stars


# --- Il file di prova per i test C++ --------------------------------------------
#
# Diciotto stelle fra le piu' luminose, con posizioni J2000 e magnitudini dei
# cataloghi, arrotondate: abbastanza per provare il lettore, la precessione e
# il cielo visto da Torino (la Polare all'altezza della latitudine, Sirio basso
# a sud d'inverno). Il file sta in Source/GeoTiles/TestData/stars_fixture.gst.

def fixture_stars() -> list[Star]:
    stars = [
        Star(101.28715, -16.71612, -1.46, 0.00),    # Sirio
        Star(95.98796, -52.69567, -0.72, 0.15),     # Canopo
        Star(213.91530, 19.18241, -0.04, 1.23),     # Arturo
        Star(279.23473, 38.78369, 0.03, 0.00),      # Vega
        Star(79.17233, 45.99799, 0.08, 0.80),       # Capella
        Star(78.63446, -8.20164, 0.12, -0.03),      # Rigel
        Star(114.82549, 5.22499, 0.38, 0.42),       # Procione
        Star(88.79294, 7.40706, 0.50, 1.85),        # Betelgeuse
        Star(297.69583, 8.86832, 0.77, 0.22),       # Altair
        Star(68.98016, 16.50930, 0.85, 1.54),       # Aldebaran
        Star(247.35192, -26.43200, 0.96, 1.83),     # Antares
        Star(201.29825, -11.16132, 0.98, -0.23),    # Spica
        Star(116.32896, 28.02620, 1.14, 1.00),      # Polluce
        Star(344.41269, -29.62224, 1.16, 0.09),     # Fomalhaut
        Star(310.35798, 45.28034, 1.25, 0.09),      # Deneb
        Star(152.09296, 11.96721, 1.35, -0.11),     # Regolo
        Star(165.93196, 61.75103, 1.79, 1.07),      # Dubhe
        Star(37.95454, 89.26411, 2.02, 0.60),       # Polare
    ]
    return sorted(stars, key=lambda star: (star.magnitude, star.ra_deg, star.dec_deg))


def fixture_bytes() -> bytes:
    return encode(fixture_stars())

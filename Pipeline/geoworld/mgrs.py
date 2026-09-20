"""
Da latitudine e longitudine al quadrato MGRS da 100 km.

A COSA SERVE
------------
Le scene Sentinel-2 sul bucket pubblico sono organizzate per quadrato MGRS:

    sentinel-s2-l2a-cogs/33/T/TG/2024/6/S2A_33TTG_20240603_0_L2A/TCI.tif
                         ^^ ^ ^^
                       zona banda quadrato

Per sapere quali scene coprono un'area bisogna quindi saper calcolare quei tre
pezzi. E' l'unica ragione per cui questo modulo esiste.

PERCHE' NON UNA LIBRERIA
------------------------
Le librerie MGRS per Python o tirano dentro GDAL/pyproj per la sola proiezione,
o sono pacchetti non mantenuti. Qui servono ottanta righe, la matematica e'
pubblica da un secolo e il risultato si verifica contro il bucket vero: il test
chiede a Roma il suo quadrato e poi controlla che su S3 esista.

PRECISIONE
----------
La serie di Krueger usata qui e' accurata a frazioni di millimetro entro i tre
gradi dal meridiano centrale, e a pochi millimetri ai bordi della zona. Per
scegliere un quadrato da 100 km e' molto piu' di quanto serva, ma non costa
niente in piu' della versione approssimata.
"""

from __future__ import annotations

import math

#: Bande di latitudine, 8 gradi ciascuna a partire da -80. Mancano I e O perche'
#: si confondono con 1 e 0: e' una convenzione di tutto il sistema MGRS.
LATITUDE_BANDS = "CDEFGHJKLMNPQRSTUVWX"

#: Lettere di colonna, tre insiemi che si alternano ogni tre zone.
COLUMN_SETS = ("ABCDEFGH", "JKLMNPQR", "STUVWXYZ")

#: Lettere di riga. Le zone pari partono cinque lettere piu' avanti.
ROW_LETTERS = "ABCDEFGHJKLMNPQRSTUV"

# Ellissoide WGS84 e costanti della proiezione UTM.
SEMI_MAJOR_AXIS = 6378137.0
FLATTENING = 1.0 / 298.257223563
SCALE_FACTOR = 0.9996
FALSE_EASTING = 500000.0
FALSE_NORTHING = 10000000.0      # solo nell'emisfero sud


def utm_zone(longitude_deg: float) -> int:
    """Zona UTM di una longitudine. Le eccezioni norvegesi non ci riguardano."""
    return int(math.floor((longitude_deg + 180.0) / 6.0)) % 60 + 1


def latitude_band(latitude_deg: float) -> str:
    if not -80.0 <= latitude_deg <= 84.0:
        raise ValueError(f"latitudine {latitude_deg} fuori dal dominio MGRS (-80..84)")
    index = int(math.floor((latitude_deg + 80.0) / 8.0))
    index = min(index, len(LATITUDE_BANDS) - 1)     # la banda X e' alta 12 gradi
    return LATITUDE_BANDS[index]


def to_utm(latitude_deg: float, longitude_deg: float,
           zone: int | None = None) -> tuple[float, float, int]:
    """Proietta in UTM. Ritorna (easting, northing, zona)."""
    zone = zone if zone is not None else utm_zone(longitude_deg)
    central_meridian = (zone - 1) * 6.0 - 180.0 + 3.0

    phi = math.radians(latitude_deg)
    delta_lambda = math.radians(longitude_deg - central_meridian)

    e2 = FLATTENING * (2.0 - FLATTENING)
    ep2 = e2 / (1.0 - e2)

    n = FLATTENING / (2.0 - FLATTENING)
    nu = SEMI_MAJOR_AXIS / math.sqrt(1.0 - e2 * math.sin(phi) ** 2)
    t = math.tan(phi)
    eta2 = ep2 * math.cos(phi) ** 2
    cos_phi = math.cos(phi)

    # Lunghezza dell'arco di meridiano, serie fino al quarto ordine in n.
    a_bar = SEMI_MAJOR_AXIS / (1.0 + n) * (1.0 + n ** 2 / 4.0 + n ** 4 / 64.0)
    meridian = a_bar * (
        phi
        - (3.0 * n / 2.0 - 9.0 * n ** 3 / 16.0) * math.sin(2.0 * phi)
        + (15.0 * n ** 2 / 16.0 - 15.0 * n ** 4 / 32.0) * math.sin(4.0 * phi)
        - (35.0 * n ** 3 / 48.0) * math.sin(6.0 * phi)
        + (315.0 * n ** 4 / 512.0) * math.sin(8.0 * phi))

    l = delta_lambda
    easting = FALSE_EASTING + SCALE_FACTOR * nu * (
        l * cos_phi
        + l ** 3 / 6.0 * cos_phi ** 3 * (1.0 - t ** 2 + eta2)
        + l ** 5 / 120.0 * cos_phi ** 5 * (5.0 - 18.0 * t ** 2 + t ** 4
                                           + 14.0 * eta2 - 58.0 * t ** 2 * eta2))

    northing = SCALE_FACTOR * (
        meridian
        + nu * t * (
            l ** 2 / 2.0 * cos_phi ** 2
            + l ** 4 / 24.0 * cos_phi ** 4 * (5.0 - t ** 2 + 9.0 * eta2 + 4.0 * eta2 ** 2)
            + l ** 6 / 720.0 * cos_phi ** 6 * (61.0 - 58.0 * t ** 2 + t ** 4
                                               + 270.0 * eta2 - 330.0 * t ** 2 * eta2)))

    if latitude_deg < 0.0:
        northing += FALSE_NORTHING

    return easting, northing, zone


def square(latitude_deg: float, longitude_deg: float) -> tuple[int, str, str]:
    """
    Quadrato MGRS da 100 km di un punto: (zona, banda, due lettere).

    Per Roma restituisce (33, 'T', 'TG'), che e' esattamente il prefisso sotto
    cui il bucket Sentinel-2 tiene le scene che coprono Roma.
    """
    zone = utm_zone(longitude_deg)
    band = latitude_band(latitude_deg)
    easting, northing, _ = to_utm(latitude_deg, longitude_deg, zone)

    column_index = int(easting // 100000)            # da 1 a 8 dentro la zona
    column_set = COLUMN_SETS[(zone - 1) % 3]
    column_letter = column_set[column_index - 1]

    row_index = int(northing // 100000) % 20
    if zone % 2 == 0:
        row_index = (row_index + 5) % 20             # le zone pari sono sfalsate
    row_letter = ROW_LETTERS[row_index]

    return zone, band, column_letter + row_letter


def squares_for_bbox(west: float, south: float, east: float, north: float,
                     step_deg: float = 0.25) -> list[tuple[int, str, str]]:
    """
    Tutti i quadrati che toccano il rettangolo.

    Si campiona su una griglia invece di calcolare i bordi esatti dei quadrati:
    quelli sono trapezi curvilinei e diversi per ogni zona, mentre un passo di un
    quarto di grado (circa 28 km) non puo' saltare un quadrato da 100 km. E' una
    di quelle volte in cui la forza bruta e' anche la cosa corretta.
    """
    found: list[tuple[int, str, str]] = []
    seen = set()

    latitude = south
    while latitude <= north + 1e-9:
        longitude = west
        while longitude <= east + 1e-9:
            key = square(latitude, longitude)
            if key not in seen:
                seen.add(key)
                found.append(key)
            longitude += step_deg
        latitude += step_deg

    return found

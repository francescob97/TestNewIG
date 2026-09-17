"""
Schema di tiling geografico WGS84.

NESSUNA DIPENDENZA: solo la standard library. Questo modulo e' matematica pura
ed e' il gemello Python di quello che in Fase 3 vivra' in C++ nel modulo
GeoTiles. Le due implementazioni devono concordare bit per bit, ed e' per questo
che `dump_test_vectors()` produce un file di vettori di prova che il C++ potra'
usare come riferimento invece di riscrivere la matematica a memoria.

--------------------------------------------------------------------------
SCHEMA
--------------------------------------------------------------------------
Livello 0 = 2 tile (2x1), perche' il dominio geografico e' 360 x 180 gradi:
due tile quadrate in gradi lo coprono esattamente. Al livello L ci sono
2^(L+1) tile in longitudine e 2^L in latitudine, ognuna di lato

    span(L) = 180 / 2^L  gradi

Perche' uno schema GEOGRAFICO (EPSG:4326) e non Web Mercator: i dati di
partenza si riproiettano una volta sola in 4326 e da li' il ritaglio dei tile
e' puro ritaglio, senza nessun'altra riproiezione. Con Mercator ogni livello
avrebbe una deformazione diversa e le quote andrebbero ricampionate di continuo.
Prezzo da pagare: una tile e' quadrata in gradi ma non in metri. Alle latitudini
italiane (35-47 N) un grado di longitudine vale 78-91 km contro i 111 km di un
grado di latitudine, quindi le tile sono circa 1.3 volte piu' alte che larghe.
E' un fatto da tenere presente nella selezione LOD (Fase 4), non un problema.

--------------------------------------------------------------------------
REGISTRAZIONE SUI NODI (gridline registration) E OVERLAP DI 1 PIXEL
--------------------------------------------------------------------------
Una tile contiene 129 x 129 POST (campioni puntuali), non 129 x 129 celle.
129 = 128 + 1: i 128 intervalli coprono la tile, e il post numero 128 cade
ESATTAMENTE sul bordo, cioe' sul post numero 0 della tile adiacente.

E' questo il famoso "1 pixel di overlap": l'ultima colonna di una tile e' la
stessa identica colonna della prima della tile a est. Non e' uno spreco, e'
la condizione che rende le giunzioni a parita' di livello esatte per
costruzione: due tile vicine non possono avere quote diverse sul bordo comune,
perche' e' letteralmente lo stesso dato.

Il secondo motivo per cui 2^n+1 e' la scelta giusta: il tile padre copre
esattamente 2x2 figli. I due figli affiancati hanno 129+129-1 = 257 post
distinti, e 257 = 2*128+1. Quindi il post i del padre corrisponde al post 2i
dei figli: la riduzione di livello e' una DECIMAZIONE ESATTA su posizioni
coincidenti, non un ricampionamento con offset. Se le tile fossero 128x128
(registrate sulle celle) i post del padre cadrebbero a meta' strada fra quelli
dei figli e ogni livello introdurrebbe un errore di mezzo pixel.

--------------------------------------------------------------------------
ORIENTAMENTO DEGLI INDICI
--------------------------------------------------------------------------
    x cresce verso EST,  x = 0 a longitudine -180
    y cresce verso SUD,  y = 0 a latitudine  +90

y verso sud e' la convenzione di XYZ/slippy map ed e' anche l'ordine in cui
GDAL memorizza le righe di un raster (riga 0 = nord). Farle coincidere elimina
un'inversione di indice e con essa un'intera categoria di bug silenziosi.
"""

from __future__ import annotations

import json
from dataclasses import dataclass

# --- Costanti dello schema ------------------------------------------------

#: Post per lato di una tile. 129 = 2^7 + 1 (vedi nota sulla registrazione).
TILE_POSTS = 129

#: Intervalli per lato: 128. E' questo il numero che divide lo span.
TILE_CELLS = TILE_POSTS - 1

#: Estensione del dominio in gradi.
LON_MIN, LON_MAX = -180.0, 180.0
LAT_MIN, LAT_MAX = -90.0, 90.0

#: Conversione gradi -> metri, approssimazione sferica. Serve solo a
#: DIMENSIONARE (scegliere un livello, stimare uno spazio su disco): non entra
#: mai in un calcolo di posizione, dove si usa l'ellissoide vero di GeoCore.
#: Sta qui, in un punto solo, perche' confrontare la risoluzione del sorgente
#: con il passo di un livello usando due approssimazioni diverse darebbe
#: risposte incoerenti.
METRES_PER_DEGREE_LAT = 111132.0
METRES_PER_DEGREE_LON_AT_EQUATOR = 111320.0

#: Livello oltre il quale non ha senso spingersi. Al livello 20 il passo fra
#: post e' ~16 cm: sotto la risoluzione di qualunque DEM esistente, e con
#: raster che nessun formato raster gestisce. Serve a trasformare un errore di
#: unita' di misura in un messaggio comprensibile invece che in un crash di GDAL
#: quattro stadi piu' avanti.
MAX_SUPPORTED_LEVEL = 20


def metres_per_degree(latitude_deg: float) -> tuple[float, float]:
    """(metri per grado di longitudine, metri per grado di latitudine)."""
    import math
    return (METRES_PER_DEGREE_LON_AT_EQUATOR * math.cos(math.radians(latitude_deg)),
            METRES_PER_DEGREE_LAT)


# --- Geometria dei livelli ------------------------------------------------

def tiles_x(level: int) -> int:
    """Numero di tile in longitudine al livello dato: 2, 4, 8, ..."""
    return 2 << level          # 2 * 2^level


def tiles_y(level: int) -> int:
    """Numero di tile in latitudine al livello dato: 1, 2, 4, ..."""
    return 1 << level          # 2^level


def tile_span_deg(level: int) -> float:
    """Lato della tile in gradi (uguale in longitudine e latitudine)."""
    return 180.0 / (1 << level)


def post_spacing_deg(level: int) -> float:
    """Passo fra due post adiacenti, in gradi."""
    return tile_span_deg(level) / TILE_CELLS


def post_spacing_metres(level: int, latitude_deg: float = 0.0) -> tuple[float, float]:
    """
    Passo fra post in metri, per longitudine e latitudine, alla latitudine data.

    Serve a scegliere il livello massimo: ha senso spingersi fino al livello in
    cui il passo eguaglia la risoluzione nativa del dato sorgente, non oltre.
    Usa l'approssimazione sferica: e' una stima per dimensionare, non una misura.
    """
    lon_metres, lat_metres = metres_per_degree(latitude_deg)
    spacing = post_spacing_deg(level)
    return spacing * lon_metres, spacing * lat_metres


# --- Geometria della singola tile -----------------------------------------

@dataclass(frozen=True)
class TileBounds:
    """Estremi geografici di una tile, in gradi."""
    west: float
    south: float
    east: float
    north: float

    def as_tuple(self) -> tuple[float, float, float, float]:
        return (self.west, self.south, self.east, self.north)


def tile_bounds(level: int, x: int, y: int) -> TileBounds:
    """Estremi della tile. I bordi sono condivisi con le tile adiacenti."""
    span = tile_span_deg(level)
    west = LON_MIN + x * span
    north = LAT_MAX - y * span
    return TileBounds(west=west, south=north - span, east=west + span, north=north)


def tile_for_lonlat(level: int, lon: float, lat: float) -> tuple[int, int]:
    """
    Tile che contiene il punto dato.

    I bordi appartengono alla tile a est / a sud, tranne agli estremi del
    dominio dove si rientra nell'ultima tile: senza questo accorgimento il polo
    Nord e l'antimeridiano finirebbero in una tile che non esiste.
    """
    span = tile_span_deg(level)
    x = int((lon - LON_MIN) / span)
    y = int((LAT_MAX - lat) / span)
    return (min(max(x, 0), tiles_x(level) - 1),
            min(max(y, 0), tiles_y(level) - 1))


def tile_range_for_bbox(level: int, west: float, south: float, east: float,
                        north: float) -> tuple[int, int, int, int]:
    """
    Intervallo INCLUSIVO di tile (x0, y0, x1, y1) che copre il bbox dato.
    """
    x0, y0 = tile_for_lonlat(level, west, north)
    x1, y1 = tile_for_lonlat(level, east, south)
    return x0, y0, x1, y1


# --- Griglia globale dei post ---------------------------------------------
#
# Ogni livello ha una griglia globale di post, numerata da 0. Ragionare in
# indici globali invece che in (tile, post locale) e' cio' che rende banale
# l'allineamento fra livelli e fra raster: l'indice globale gx al livello L
# corrisponde all'indice 2*gx al livello L+1, esattamente.

def global_posts_x(level: int) -> int:
    """Numero totale di post distinti in longitudine (bordi condivisi contati una volta)."""
    return tiles_x(level) * TILE_CELLS + 1


def global_posts_y(level: int) -> int:
    return tiles_y(level) * TILE_CELLS + 1


def global_post_x(level: int, tile_x: int, i: int) -> int:
    """Indice globale del post locale i della tile tile_x."""
    return tile_x * TILE_CELLS + i


def global_post_y(level: int, tile_y: int, j: int) -> int:
    return tile_y * TILE_CELLS + j


def post_lon(level: int, global_x: int) -> float:
    return LON_MIN + global_x * post_spacing_deg(level)


def post_lat(level: int, global_y: int) -> float:
    return LAT_MAX - global_y * post_spacing_deg(level)


def global_post_range_for_bbox(level: int, west: float, south: float, east: float,
                               north: float, margin_posts: int = 0
                               ) -> tuple[int, int, int, int]:
    """
    Intervallo INCLUSIVO di indici globali di post (gx0, gy0, gx1, gy1) che
    copre il bbox, arrotondato verso l'ESTERNO piu' un margine.

    L'arrotondamento esterno e' obbligatorio: se si arrotondasse al post piu'
    vicino, il raster potrebbe non contenere i post di bordo delle tile
    marginali e quelle tile risulterebbero tagliate.
    """
    import math
    spacing = post_spacing_deg(level)

    gx0 = math.floor((west - LON_MIN) / spacing) - margin_posts
    gx1 = math.ceil((east - LON_MIN) / spacing) + margin_posts
    gy0 = math.floor((LAT_MAX - north) / spacing) - margin_posts
    gy1 = math.ceil((LAT_MAX - south) / spacing) + margin_posts

    return (max(gx0, 0), max(gy0, 0),
            min(gx1, global_posts_x(level) - 1),
            min(gy1, global_posts_y(level) - 1))


def recommended_max_level(source_resolution_m: float, latitude_deg: float) -> int:
    """
    Livello il cui passo fra post eguaglia (senza superarla) la risoluzione
    nativa del dato sorgente alla latitudine indicata.

    ATTENZIONE: `source_resolution_m` deve essere in METRI SUL TERRENO. Non e'
    la dimensione del pixel letta dal geotransform, che e' nelle unita' del CRS
    sorgente: per un raster geografico (EPSG:4326) sono GRADI, e passare 0.00028
    al posto di 31 fa chiedere una risoluzione centomila volte piu' fine.
    Usa raster.source_ground_resolution(), che misura la distanza vera fra due
    pixel adiacenti qualunque sia il CRS.

    Si sceglie il primo livello il cui passo in LATITUDINE scende sotto la
    risoluzione sorgente. In longitudine il passo e' gia' piu' fine, perche' un
    grado di longitudine vale meno metri: alle latitudini italiane si finisce
    per sovracampionare in longitudine di circa il 30%. E' la scelta
    conservativa giusta: sovracampionare non inventa dettaglio, mentre fermarsi
    al livello precedente butterebbe via meta' della risoluzione del dato.
    """
    if not (source_resolution_m > 0.0):
        raise ValueError(f"risoluzione sorgente non valida: {source_resolution_m}")

    for level in range(0, MAX_SUPPORTED_LEVEL + 1):
        _, spacing_lat = post_spacing_metres(level, latitude_deg)
        if spacing_lat <= source_resolution_m:
            return level
    return MAX_SUPPORTED_LEVEL


# --- Vettori di prova per l'implementazione C++ ---------------------------

def build_test_vectors() -> dict:
    """
    Valori di riferimento che l'implementazione C++ di Fase 3 dovra' riprodurre.
    Serializzarli evita che le due implementazioni divergano per una svista.
    """
    vectors = {
        "schema": {
            "tilePosts": TILE_POSTS,
            "tileCells": TILE_CELLS,
            "level0TilesX": tiles_x(0),
            "level0TilesY": tiles_y(0),
            "xAxis": "est, x=0 a lon -180",
            "yAxis": "sud, y=0 a lat +90",
        },
        "levels": [],
        "tileBounds": [],
        "lookups": [],
    }

    for level in range(0, 16):
        vectors["levels"].append({
            "level": level,
            "tilesX": tiles_x(level),
            "tilesY": tiles_y(level),
            "tileSpanDeg": tile_span_deg(level),
            "postSpacingDeg": post_spacing_deg(level),
            "globalPostsX": global_posts_x(level),
            "globalPostsY": global_posts_y(level),
        })

    for level, x, y in [(0, 0, 0), (0, 1, 0), (1, 1, 0), (2, 2, 1),
                        (8, 133, 61), (14, 8560, 3936)]:
        b = tile_bounds(level, x, y)
        vectors["tileBounds"].append({
            "level": level, "x": x, "y": y,
            "west": b.west, "south": b.south, "east": b.east, "north": b.north,
        })

    for level, lon, lat in [(0, 0.0, 0.0), (0, -180.0, 90.0), (0, 179.999, -89.999),
                            (8, 12.492231, 41.890210), (14, 12.492231, 41.890210),
                            (14, 6.865200, 45.832600)]:
        x, y = tile_for_lonlat(level, lon, lat)
        vectors["lookups"].append({"level": level, "lon": lon, "lat": lat, "x": x, "y": y})

    return vectors


def dump_test_vectors(path: str) -> None:
    """
    Scrive i vettori in JSON e, a fianco, in un formato di testo piatto.

    Il .txt esiste per un motivo pratico: i test C++ dello strato puro di
    GeoTiles non devono tirarsi dentro un parser JSON solo per leggere dei
    numeri di riferimento. Una riga per record, campi separati da spazi, si
    legge con due righe di iostream.
    """
    with open(path, "w", encoding="utf-8") as handle:
        json.dump(build_test_vectors(), handle, indent="\t")
        handle.write("\n")

    vectors = build_test_vectors()
    text_path = path[:-5] + ".txt" if path.endswith(".json") else path + ".txt"
    with open(text_path, "w", encoding="utf-8") as handle:
        handle.write("# vettori di riferimento dello schema di tiling GeoWorld\n")
        handle.write(f"# generati da geoworld/tiling.py -- non modificare a mano\n")
        handle.write(f"SCHEMA {TILE_POSTS} {TILE_CELLS} {tiles_x(0)} {tiles_y(0)}\n")
        for entry in vectors["levels"]:
            handle.write("LEVEL {level} {tilesX} {tilesY} {tileSpanDeg!r} "
                         "{postSpacingDeg!r}\n".format(**entry))
        for entry in vectors["tileBounds"]:
            handle.write("BOUNDS {level} {x} {y} {west!r} {south!r} {east!r} "
                         "{north!r}\n".format(**entry))
        for entry in vectors["lookups"]:
            handle.write("LOOKUP {level} {lon!r} {lat!r} {x} {y}\n".format(**entry))

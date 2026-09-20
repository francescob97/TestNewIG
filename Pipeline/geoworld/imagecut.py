"""
Costruzione della piramide di ortofoto.

LA STRUTTURA, IN DUE PASSI
--------------------------
1. Il livello PIU' FINE si taglia dalla sorgente: per ogni tile una warp di GDAL
   nel rettangolo esatto della tile, a 256x256 pixel.
2. Tutti i livelli sopra si costruiscono dai FIGLI, con una media 2x2. Non si
   torna mai alla sorgente.

Il secondo passo non tocca GDAL ed e' quindi verificabile senza. E' anche molto
piu' veloce: rileggere la sorgente per ogni livello significherebbe riprocessare
l'intero territorio tante volte quanti sono i livelli.

PERCHE' UNA WARP PER TILE E NON UN UNICO RASTER GIGANTE
-------------------------------------------------------
La pipeline delle quote costruisce un raster intero per livello e poi lo taglia.
Per le ortofoto conviene il contrario: un raster unico al livello 16 sull'Italia
sarebbe da centinaia di gigabyte, e ne servirebbe il doppio perche' sul disco ci
starebbero sia lui sia le tile. Una warp per tile lavora a memoria costante ed e'
banalmente riavviabile: la tile esiste oppure no.
"""

from __future__ import annotations

import math
import os
from dataclasses import dataclass, field

import numpy as np

from . import imageformat, manifest, tiling


def imagery_pixel_size_deg(level: int) -> float:
    """Lato di un pixel, in gradi, al livello dato."""
    return tiling.tile_span_deg(level) / imageformat.TILE_PIXELS


def imagery_pixel_size_metres(level: int, latitude_deg: float = 0.0) -> tuple[float, float]:
    metres_lon, metres_lat = tiling.metres_per_degree(latitude_deg)
    size = imagery_pixel_size_deg(level)
    return size * metres_lon, size * metres_lat


def recommended_max_level(source_resolution_m: float, latitude_deg: float) -> int:
    """
    Primo livello i cui pixel sono fini quanto la sorgente.

    NOTA SUL CONFRONTO CON LE QUOTE. A parita' di livello un'immagine e' il
    DOPPIO piu' fine di un terreno, perche' 256 pixel contro 128 celle. Quindi
    una sorgente a 10 m e' nativa al livello 13 per le immagini e al 14 per le
    quote. Usare la funzione delle quote qui darebbe un livello di troppo, cioe'
    quattro volte i file e nessun dettaglio in piu'.
    """
    if source_resolution_m <= 0:
        raise ValueError("risoluzione della sorgente non positiva")

    for level in range(0, tiling.MAX_SUPPORTED_LEVEL + 1):
        metres_lon, metres_lat = imagery_pixel_size_metres(level, latitude_deg)
        if min(metres_lon, metres_lat) <= source_resolution_m:
            return level
    return tiling.MAX_SUPPORTED_LEVEL


@dataclass
class ImageryStats:
    tiles_written: int = 0
    tiles_skipped_empty: int = 0
    tiles_partial: int = 0
    bytes_written: int = 0
    levels: dict = field(default_factory=dict)

    def note(self, level: int, blob_size: int, coverage: int) -> None:
        self.tiles_written += 1
        self.bytes_written += blob_size
        if coverage < 100:
            self.tiles_partial += 1
        self.levels[level] = self.levels.get(level, 0) + 1


# ---------------------------------------------------------------------------
#  Passo 1: il livello piu' fine, dalla sorgente. E' l'unica parte che usa GDAL.
# ---------------------------------------------------------------------------

def warp_tile(source_dataset, level: int, x: int, y: int):
    """
    Ritaglia dalla sorgente il rettangolo della tile, a 256x256.

    Ritorna (pixels RGB uint8, copertura in percentuale).

    dstAlpha chiede a GDAL di aggiungere un canale alfa che vale 0 dove non
    c'era sorgente. E' il modo per distinguere "nero perche' e' asfalto" da
    "nero perche' non c'e' niente", che altrimenti sarebbero lo stesso pixel.
    """
    from osgeo import gdal

    bounds = tiling.tile_bounds(level, x, y)

    warped = gdal.Warp(
        "", source_dataset,
        format="MEM",
        outputBounds=(bounds.west, bounds.south, bounds.east, bounds.north),
        width=imageformat.TILE_PIXELS, height=imageformat.TILE_PIXELS,
        dstSRS="EPSG:4326",
        resampleAlg="cubic",
        dstAlpha=True,
        multithread=False)

    if warped is None:
        raise RuntimeError(f"warp fallita sulla tile {level}/{x}/{y}")

    band_count = warped.RasterCount
    array = warped.ReadAsArray()
    warped = None

    array = np.asarray(array)
    if array.ndim == 2:                      # sorgente a banda singola
        array = array[np.newaxis, :, :]

    alpha = array[-1]
    colour = array[:-1] if band_count > 1 else array

    if colour.shape[0] == 1:                 # grigia: la si replica su RGB
        colour = np.repeat(colour, 3, axis=0)
    colour = colour[:3]

    pixels = np.ascontiguousarray(np.moveaxis(colour, 0, -1)).astype(np.uint8)

    covered = alpha > 0
    coverage = float(covered.mean() * 100.0)

    if coverage < 100.0:
        pixels[~covered] = imageformat.FILL_COLOUR

    return pixels, coverage


def cut_finest_level(source_path: str, root: str, level: int,
                     bbox: tuple[float, float, float, float],
                     quality: int, stats: ImageryStats,
                     progress=None) -> list:
    """Taglia tutte le tile del livello piu' fine che intersecano il bbox."""
    from osgeo import gdal

    source_dataset = gdal.Open(source_path)
    if source_dataset is None:
        raise RuntimeError(f"non riesco ad aprire {source_path}")

    west, south, east, north = bbox
    x0, y0, x1, y1 = tiling.tile_range_for_bbox(level, west, south, east, north)

    entries = []
    total = (x1 - x0 + 1) * (y1 - y0 + 1)
    done = 0

    for x in range(x0, x1 + 1):
        for y in range(y0, y1 + 1):
            done += 1
            if progress and done % 50 == 0:
                progress(f"livello {level}: {done}/{total} tile")

            path = os.path.join(root, imageformat.tile_relative_path(level, x, y))
            if os.path.exists(path):
                header = imageformat.read_tile_header(path)
                entries.append((x, y, header.coverage_percent))
                continue

            pixels, coverage = warp_tile(source_dataset, level, x, y)

            # Una tile senza un solo pixel di sorgente non si scrive: sul mare o
            # fuori dal volo sarebbero milioni di file tutti uguali.
            if coverage <= 0.0:
                stats.tiles_skipped_empty += 1
                continue

            blob = imageformat.encode_tile(level, x, y, pixels, coverage, quality)
            imageformat.write_tile_atomic(path, blob)
            stats.note(level, len(blob), int(round(coverage)))
            entries.append((x, y, int(round(coverage))))

    source_dataset = None
    return entries


# ---------------------------------------------------------------------------
#  Passo 2: i livelli superiori, dai figli. Niente GDAL: solo numpy.
# ---------------------------------------------------------------------------

def assemble_parent(children: dict) -> tuple[np.ndarray, float] | None:
    """
    Compone una tile di livello L dai suoi quattro figli di livello L+1.

    `children` e' un dizionario {(dx, dy): (pixels, copertura)} con dx, dy in
    {0, 1}: dx=0 e' il figlio a OVEST, dy=0 quello a NORD.

    I figli mancanti diventano riempimento. Il risultato e' 512x512 ridotto a
    256x256 dalla media 2x2, e la copertura del padre e' la media delle quattro
    coperture, contando zero per i figli assenti.
    """
    if not children:
        return None

    side = imageformat.TILE_PIXELS
    canvas = np.empty((side * 2, side * 2, 3), dtype=np.uint8)
    canvas[:, :] = imageformat.FILL_COLOUR

    coverage_sum = 0.0
    for (dx, dy), (pixels, coverage) in children.items():
        canvas[dy * side:(dy + 1) * side, dx * side:(dx + 1) * side] = pixels
        coverage_sum += coverage

    return imageformat.reduce_2x2(canvas), coverage_sum / 4.0


def build_level_from_children(root: str, level: int, child_entries: list,
                              quality: int, stats: ImageryStats,
                              progress=None) -> list:
    """Costruisce tutte le tile del livello `level` dai figli di `level + 1`."""
    by_parent: dict = {}
    for x, y, _coverage in child_entries:
        by_parent.setdefault((x // 2, y // 2), []).append((x, y))

    entries = []
    total = len(by_parent)
    done = 0

    for (parent_x, parent_y), child_keys in sorted(by_parent.items()):
        done += 1
        if progress and done % 50 == 0:
            progress(f"livello {level}: {done}/{total} tile")

        path = os.path.join(root, imageformat.tile_relative_path(level, parent_x, parent_y))
        if os.path.exists(path):
            header = imageformat.read_tile_header(path)
            entries.append((parent_x, parent_y, header.coverage_percent))
            continue

        children = {}
        for child_x, child_y in child_keys:
            child_path = os.path.join(
                root, imageformat.tile_relative_path(level + 1, child_x, child_y))
            if not os.path.exists(child_path):
                continue
            header, pixels = imageformat.read_tile(child_path)
            children[(child_x - parent_x * 2, child_y - parent_y * 2)] = (
                pixels, float(header.coverage_percent))

        assembled = assemble_parent(children)
        if assembled is None:
            continue

        pixels, coverage = assembled
        blob = imageformat.encode_tile(level, parent_x, parent_y, pixels, coverage, quality)
        imageformat.write_tile_atomic(path, blob)
        stats.note(level, len(blob), int(round(coverage)))
        entries.append((parent_x, parent_y, int(round(coverage))))

    return entries


# ---------------------------------------------------------------------------
#  Il drappeggio: quale immagine usare per una tile di terreno, e dove
#
#  Questa e' la matematica che il runtime C++ esegue a ogni tile. Sta anche qui,
#  in Python, per due motivi: e' testabile in un millisecondo, e i test possono
#  produrre i vettori di riferimento con cui si verifica la versione C++. E'
#  esattamente quello che gia' si fa per lo schema di tiling.
# ---------------------------------------------------------------------------

def ancestor_for(level: int, x: int, y: int, ancestor_level: int) -> tuple[int, int]:
    """Tile di livello `ancestor_level` che contiene la tile (level, x, y)."""
    if ancestor_level > level:
        raise ValueError(f"l'antenato ({ancestor_level}) non puo' stare sotto la tile ({level})")
    shift = level - ancestor_level
    return x >> shift, y >> shift


def uv_transform(level: int, x: int, y: int,
                 ancestor_level: int) -> tuple[float, float, float]:
    """
    Offset e scala da applicare alle UV della mesh: (offsetU, offsetV, scala).

        uv_texture = offset + uv_mesh * scala

    Con ancestor_level == level la funzione restituisce (0, 0, 1), cioe' il caso
    normale. Non e' un ramo separato: e' la stessa formula che degenera.
    """
    if ancestor_level > level:
        raise ValueError(f"l'antenato ({ancestor_level}) non puo' stare sotto la tile ({level})")

    shift = level - ancestor_level
    divisions = 1 << shift
    scale = 1.0 / divisions
    return (x & (divisions - 1)) * scale, (y & (divisions - 1)) * scale, scale

"""
Ritaglio dei tile da un raster di livello.

Il raster e' gia' allineato alla griglia globale dei post del livello, quindi
ritagliare una tile e' una lettura di finestra a indici INTERI: nessun
ricampionamento, nessun filtro, nessun offset. Due tile adiacenti leggono
letteralmente la stessa colonna di post dal medesimo raster, ed e' per questo
che il bordo comune coincide esattamente e non solo "quasi".
"""

from __future__ import annotations

import math
import os
from concurrent.futures import ProcessPoolExecutor

import numpy as np

from . import tiling, tileformat, geoid
from .manifest import TileEntry
from .raster import LevelGrid

#: Quante tile per lato si leggono in un colpo solo. Una tile e' 129x129 post:
#: leggerle una a una significherebbe centinaia di migliaia di letture da poche
#: decine di kB. Con blocchi 8x8 si legge circa 1033x1033 post per volta (4 MB),
#: che e' un accesso sensato per un GeoTIFF tiled.
DEFAULT_TILE_BLOCK = 8

# Stato per processo dei worker. I dataset GDAL non attraversano il confine di
# un processo e non vanno condivisi fra thread: ogni worker apre il suo.
_worker_state: dict = {}


def _read_post_window(band, grid: LevelGrid, col0: int, row0: int,
                      width: int, height: int) -> np.ndarray:
    """
    Legge una finestra di post riempiendo con NaN cio' che cade fuori dal raster.

    Serve perche' le tile ai bordi del dataset sporgono oltre il raster: senza
    il riempimento, GDAL solleverebbe un errore e si perderebbe l'intera tile di
    bordo, che invece contiene dato valido per la parte che ricade dentro.
    """
    output = np.full((height, width), np.nan, dtype=np.float32)

    src_col0 = max(col0, 0)
    src_row0 = max(row0, 0)
    src_col1 = min(col0 + width, grid.width)
    src_row1 = min(row0 + height, grid.height)

    if src_col1 <= src_col0 or src_row1 <= src_row0:
        return output

    data = band.ReadAsArray(src_col0, src_row0, src_col1 - src_col0, src_row1 - src_row0)
    output[src_row0 - row0: src_row1 - row0,
           src_col0 - col0: src_col1 - col0] = np.asarray(data, dtype=np.float32)
    return output


def _init_worker(raster_path: str, grid: LevelGrid, level: int, root: str,
                 undulation_grid: np.ndarray,
                 undulation_geotransform: tuple[float, float, float],
                 force: bool) -> None:
    from osgeo import gdal
    gdal.UseExceptions()

    dataset = gdal.Open(raster_path)
    _worker_state.update(
        dataset=dataset, band=dataset.GetRasterBand(1), grid=grid, level=level,
        root=root, undulation_grid=undulation_grid,
        undulation_geotransform=undulation_geotransform, force=force)


def _process_block(block: tuple[int, int, int, int]) -> list[tuple]:
    """Ritaglia un blocco di tile. Ritorna tuple grezze: si serializzano meglio."""
    x0, y0, x1, y1 = block
    state = _worker_state

    grid: LevelGrid = state["grid"]
    level: int = state["level"]
    root: str = state["root"]
    cells = tiling.TILE_CELLS

    # Finestra di post del blocco, incluso il post di bordo condiviso in fondo.
    block_col0 = x0 * cells - grid.gx0
    block_row0 = y0 * cells - grid.gy0
    block_width = (x1 - x0 + 1) * cells + 1
    block_height = (y1 - y0 + 1) * cells + 1

    window = _read_post_window(state["band"], grid, block_col0, block_row0,
                               block_width, block_height)

    # Ondulazione del geoide sugli stessi post: serve a riempire i post senza
    # dato con la quota ELLISSOIDICA del livello medio del mare.
    lons = tiling.LON_MIN + (grid.gx0 + block_col0 +
                             np.arange(block_width, dtype=np.float64)) * grid.spacing
    lats = tiling.LAT_MAX - (grid.gy0 + block_row0 +
                             np.arange(block_height, dtype=np.float64)) * grid.spacing
    undulation = geoid.sample_undulation(
        state["undulation_grid"], state["undulation_geotransform"],
        lons[np.newaxis, :], lats[:, np.newaxis])

    results = []
    for tile_y in range(y0, y1 + 1):
        for tile_x in range(x0, x1 + 1):
            local_col = (tile_x - x0) * cells
            local_row = (tile_y - y0) * cells

            heights = window[local_row: local_row + tiling.TILE_POSTS,
                             local_col: local_col + tiling.TILE_POSTS].copy()

            missing = ~np.isfinite(heights)
            missing_count = int(missing.sum())

            # Tile interamente priva di dato sorgente: non la scriviamo affatto.
            # E' cio' che tiene il dataset proporzionale alla terraferma invece
            # che al rettangolo che la contiene: per l'Italia il bbox e' circa
            # cinque volte l'area emersa.
            if missing_count == heights.size:
                continue

            if missing_count:
                sea_level = undulation[local_row: local_row + tiling.TILE_POSTS,
                                       local_col: local_col + tiling.TILE_POSTS]
                heights[missing] = sea_level[missing]

            path = os.path.join(root, tileformat.tile_relative_path(level, tile_x, tile_y))

            # Riavviabilita': se la tile c'e' gia' ed e' integra, non si riscrive.
            # Il controllo di dimensione basta perche' la scrittura e' atomica
            # (vedi tileformat.write_tile_atomic): un file di dimensione giusta
            # e' per costruzione un file completo.
            if not state["force"] and os.path.exists(path) \
                    and os.path.getsize(path) == tileformat.TILE_BYTES:
                header = tileformat.read_tile_header(path)
                if header.level == level and header.tile_x == tile_x and header.tile_y == tile_y \
                        and header.width == tiling.TILE_POSTS:
                    results.append((tile_x, tile_y, header.min_height, header.max_height, False))
                    continue
                # Header incoerente: e' un residuo di una generazione con
                # parametri diversi. Si riscrive invece di fidarsi.

            blob = tileformat.encode_tile(level, tile_x, tile_y, heights,
                                          has_filled_posts=missing_count > 0)
            tileformat.write_tile_atomic(path, blob)

            results.append((tile_x, tile_y, float(heights.min()), float(heights.max()), True))

    return results


def cut_level(raster_path: str, grid: LevelGrid, level: int, root: str,
              tile_range: tuple[int, int, int, int],
              undulation_grid: np.ndarray,
              undulation_geotransform: tuple[float, float, float],
              jobs: int = 1, force: bool = False,
              block_size: int = DEFAULT_TILE_BLOCK,
              progress=None) -> tuple[list[TileEntry], int]:
    """
    Ritaglia tutte le tile del livello. Ritorna (voci di indice, tile scritte).
    """
    x0, y0, x1, y1 = tile_range

    blocks = [(bx, by, min(bx + block_size - 1, x1), min(by + block_size - 1, y1))
              for by in range(y0, y1 + 1, block_size)
              for bx in range(x0, x1 + 1, block_size)]

    entries: list[TileEntry] = []
    written = 0
    initializer_args = (raster_path, grid, level, root, undulation_grid,
                        undulation_geotransform, force)

    def collect(raw_results):
        nonlocal written
        for tile_x, tile_y, minimum, maximum, was_written in raw_results:
            entries.append(TileEntry(tile_x, tile_y, minimum, maximum))
            written += int(was_written)

    if jobs <= 1:
        _init_worker(*initializer_args)
        for index, block in enumerate(blocks):
            collect(_process_block(block))
            if progress:
                progress(index + 1, len(blocks))
    else:
        with ProcessPoolExecutor(max_workers=jobs, initializer=_init_worker,
                                 initargs=initializer_args) as pool:
            for index, raw in enumerate(pool.map(_process_block, blocks, chunksize=1)):
                collect(raw)
                if progress:
                    progress(index + 1, len(blocks))

    return entries, written

"""
Pipeline dell'acqua (Fase 9): da OSM, poligoni del mare e terreno a tile .gwa.

TRE STADI, RIAVVIABILI (come le strade)
--------------------------------------
1. **estrai.** Si leggono i poligoni (laghi, lagune, fiumi larghi) e le linee
   (fiumi, canali) dell'estratto OSM, e i poligoni del mare. Per ogni corpo si
   calcolano le regole (waterclasses.py) e, LEGGENDO IL TERRENO, la quota:
   il 20° percentile dei post dentro un lago, il profilo in discesa lungo un
   fiume. Il risultato va nei BLOCCHI (tile del livello 9, ~40 km, con un
   margine di qualche km).
2. **taglia.** Per ogni tile di terreno vicina all'acqua, a tutti i livelli:
   rasterizzazione, distanza dalla riva, superficie e fondale sui post.
3. **indici**, manifest e il geoide sull'area.

LA DISTANZA DALLA RIVA SERVE DUE VOLTE
--------------------------------------
- Nel RASTER (256 x 256) come SDF: la riva nitida, la schiuma. Basta saperla
  fino a 16 pixel: si rasterizza la tile con 16 pixel di margine, a
  risoluzione doppia (per avere la riva con un quarto di pixel di precisione).
- Sui POST, per il FONDALE: la rampa del mare arriva a 60 m di profondita' a
  ~1,2 km dalla riva. Al livello 14 una tile e' larga ~1 km: per sapere quanto
  e' lontana la riva da un post bisogna guardare fino a 1,2 km oltre il bordo.
  Si fa su una griglia separata, alla risoluzione dei post, con quel margine.
  Due tile vicine vedono cosi' la stessa riva e calcolano lo stesso fondale
  sul post che condividono.

GDAL fa i due lavori pesanti: RasterizeLayer (dal poligono ai pixel) e
ComputeProximity (la distanza, in metri, dal pixel piu' vicino di un tipo).
"""

from __future__ import annotations

import glob
import json
import math
import os
import shutil
import struct
import time
from collections import OrderedDict
from concurrent.futures import ProcessPoolExecutor
from dataclasses import dataclass
from typing import Callable, Iterator

import numpy as np

from . import manifest as terrain_manifest
from . import tileformat, tiling, waterclasses, waterformat
from .osmextract import parse_other_tags, _linestring_from_wkb
from .state import PipelineState

PIPELINE_VERSION = "1.0"

#: Livello dei blocchi dell'estrazione: ~40 km di lato.
BLOCK_LEVEL = 9

#: Margine dei blocchi, in gradi (~4 km): copre il contesto del fondale (1,2 km)
#: e i 16 pixel di margine del raster fino al livello 9.
BLOCK_MARGIN_DEG = 0.04

#: Margine del raster, in pixel della tile (256).
RASTER_MARGIN = 16

#: Passo di campionamento del terreno lungo i fiumi, in metri.
RIVER_SAMPLE_M = 20.0

#: La quota di un corpo che non ce l'ha (un'area fluviale fuori dal terreno),
#: nei layer da rasterizzare: torna NaN dopo (_burn_attribute).
NO_LEVEL = -1.0e6

#: Un post del terreno che vale il geoide a meno di cosi' e' un riempimento
#: (tilecut.py scrive il livello del mare dove il sorgente non ha dato).
FILLED_POST_TOLERANCE_M = 0.05

#: Metri per grado di latitudine (lo stesso numero di WaterMetresPerPixel in C++):
#: il passo NOMINALE dei post di un livello, uguale in tutte le sue tile.
NOMINAL_METRES_PER_DEGREE = 111132.0

#: Quanti post attorno all'acqua "risolta" ricevono la quota della superficie:
#: le celle della mesh a cavallo della riva ne hanno bisogno in tutti e
#: quattro gli angoli (passo 2, piu' la fascia della sponda).
SURFACE_FILL_POSTS = 6

#: Fin dove la quota dell'asse di un fiume riempie un'area fluviale, in metri.
RIVER_AREA_FILL_M = 1500.0

#: Di quanto si allarga il ritaglio del mare oltre il bbox (~10 km).
SEA_CLIP_MARGIN_DEG = 0.1

#: Laghi piu' piccoli di cosi' (km²) non si disegnano: meno di 20 x 20 m.
MIN_LAKE_KM2 = 0.0004

#: Record di un corpo nei blocchi: tipo, linea?, riservato, quota, onde,
#: profondita' massima, larghezza, area km², byte del WKB.
BLOCK_RECORD = struct.Struct("<BBHfffffI")

Report = Callable[[str], None]


def _null_report(_message: str) -> None:
    pass


# =============================================================================
#  Il terreno, letto dalle tile .ght
# =============================================================================

class TerrainSampler:
    """
    Quote del terreno in punti qualunque, dalla tile piu' fine che le contiene.

    Bilineare sui post: per la quota di un lago o il profilo di un fiume non
    serve la diagonale della mesh, serve un numero robusto.
    """

    def __init__(self, root: str, cache_tiles: int = 256):
        document = terrain_manifest.read_manifest(root)
        self.root = root
        self.levels = sorted(int(level["level"]) for level in document["levels"])
        self.vertical_crs = (document.get("verticalDatum") or {}).get("verticalCrs", "EPSG:3855")
        bbox = document.get("boundingBox", {})
        self.bbox = (bbox.get("west", -180.0), bbox.get("south", -90.0),
                     bbox.get("east", 180.0), bbox.get("north", 90.0))
        self.index = {level: {(e.x, e.y) for e in terrain_manifest.read_level_index(root, level)}
                      for level in self.levels}
        #: Il geoide (GeoidSampler), per riconoscere i post riempiti col livello del mare.
        self.sea_level = None
        self._cache: OrderedDict = OrderedDict()
        self._cache_tiles = cache_tiles

    def tiles(self, level: int) -> set[tuple[int, int]]:
        return self.index.get(level, set())

    def _tile(self, level: int, x: int, y: int) -> np.ndarray | None:
        key = (level, x, y)
        if key in self._cache:
            self._cache.move_to_end(key)
            return self._cache[key]
        if (x, y) not in self.index.get(level, ()):
            return None
        _, heights = tileformat.read_tile(os.path.join(self.root, tileformat.tile_relative_path(level, x, y)))
        self._cache[key] = heights
        if len(self._cache) > self._cache_tiles:
            self._cache.popitem(last=False)
        return heights

    def covers(self, lons, lats) -> np.ndarray:
        """Vero dove il terreno ha dati veri: dentro il riquadro del sorgente."""
        west, south, east, north = self.bbox
        lons = np.asarray(lons, dtype=np.float64).ravel()
        lats = np.asarray(lats, dtype=np.float64).ravel()
        return (lons >= west) & (lons <= east) & (lats >= south) & (lats <= north)

    def heights(self, lons, lats) -> np.ndarray:
        """
        Quote ellissoidiche, NaN dove il terreno non c'e'.

        "Non c'e'" vuol dire tre cose, e la prima versione non ne vedeva
        nessuna. Il Po, che a Torino entra da sud fuori dal DEM, prendeva il
        "minimo progressivo" di quote finte, e tutto il fiume veniva scavato di
        200 m:

          - FUORI DAL RIQUADRO del sorgente;
          - dove il livello NATIVO non ha la tile: i livelli bassi coprono ben
            oltre il DEM, e le loro tile mescolano dati veri e riempimenti;
          - sui post RIEMPITI col livello del mare (tilecut.py li scrive cosi'
            dove il sorgente non ha dato: ai bordi, oltre un confine, in un
            buco). Sono un numero valido e sbagliato di centinaia di metri; si
            riconoscono perche' valgono esattamente il geoide, e si scartano
            se `sea_level` (il geoide) e' dato. L'interpolazione usa solo i
            post buoni attorno al punto.
        """
        lons = np.asarray(lons, dtype=np.float64).ravel()
        lats = np.asarray(lats, dtype=np.float64).ravel()
        result = np.full(lons.shape, np.nan)
        covered = self.covers(lons, lats)
        if not covered.any():
            return result
        cells = tiling.TILE_CELLS
        level = self.levels[-1]
        span = tiling.tile_span_deg(level)
        tx = np.floor((lons + 180.0) / span).astype(np.int64)
        ty = np.floor((90.0 - lats) / span).astype(np.int64)
        for x, y in set(zip(tx[covered].tolist(), ty[covered].tolist())):
            heights = self._tile(level, x, y)
            if heights is None:
                continue
            pick = covered & (tx == x) & (ty == y)
            west = -180.0 + x * span
            north = 90.0 - y * span
            fx = np.clip((lons[pick] - west) / span * cells, 0.0, cells)
            fy = np.clip((north - lats[pick]) / span * cells, 0.0, cells)
            i0 = np.minimum(np.floor(fx).astype(np.intp), cells - 1)
            j0 = np.minimum(np.floor(fy).astype(np.intp), cells - 1)
            ax, ay = fx - i0, fy - j0
            columns = np.stack([i0, i0 + 1, i0, i0 + 1])
            rows = np.stack([j0, j0, j0 + 1, j0 + 1])
            weights = np.stack([(1 - ax) * (1 - ay), ax * (1 - ay), (1 - ax) * ay, ax * ay])
            values = heights[rows, columns].astype(np.float64)
            valid = np.isfinite(values)
            if self.sea_level is not None:
                sea = np.asarray(self.sea_level(west + columns * span / cells, north - rows * span / cells),
                                 dtype=np.float64)
                valid &= np.abs(values - sea) > FILLED_POST_TOLERANCE_M
            weight = (weights * valid).sum(axis=0)
            h = (weights * np.where(valid, values, 0.0)).sum(axis=0) / np.maximum(weight, 1.0e-12)
            result[pick] = np.where(weight >= 0.5, h, np.nan)
        return result


class GeoidSampler:
    """N(lon, lat) dalla griglia del geoide (geoid.py), o una costante per i test."""

    def __init__(self, grid: np.ndarray | None = None, geotransform=None, constant: float | None = None):
        self.grid = grid
        self.geotransform = geotransform
        self.constant = constant

    def __call__(self, lons, lats) -> np.ndarray:
        if self.constant is not None:
            return np.full(np.broadcast(np.asarray(lons), np.asarray(lats)).shape, self.constant)
        from . import geoid
        return geoid.sample_undulation(self.grid, self.geotransform, lons, lats)


class BathymetrySampler:
    """
    Profondita' del mare da un raster (EMODnet, GEBCO): quote ORTOMETRICHE,
    negative sotto il livello del mare, in un CRS geografico. Restituisce la
    profondita' in metri (positiva), NaN fuori dal raster o sulla terra.
    """

    def __init__(self, path: str):
        from osgeo import gdal
        gdal.UseExceptions()
        self.dataset = gdal.Open(path)
        self.band = self.dataset.GetRasterBand(1)
        self.nodata = self.band.GetNoDataValue()
        self.gt = self.dataset.GetGeoTransform()
        self.width = self.dataset.RasterXSize
        self.height = self.dataset.RasterYSize

    def depth(self, lons, lats) -> np.ndarray:
        lons = np.asarray(lons, dtype=np.float64)
        lats = np.asarray(lats, dtype=np.float64)
        fx = (lons - self.gt[0]) / self.gt[1] - 0.5
        fy = (lats - self.gt[3]) / self.gt[5] - 0.5
        x0 = int(max(0, math.floor(np.nanmin(fx))))
        y0 = int(max(0, math.floor(np.nanmin(fy))))
        x1 = int(min(self.width - 1, math.ceil(np.nanmax(fx)) + 1))
        y1 = int(min(self.height - 1, math.ceil(np.nanmax(fy)) + 1))
        result = np.full(lons.shape, np.nan)
        if x1 <= x0 or y1 <= y0:
            return result
        window = self.band.ReadAsArray(x0, y0, x1 - x0 + 1, y1 - y0 + 1).astype(np.float64)
        if self.nodata is not None:
            window[window == self.nodata] = np.nan
        gx = np.clip(fx - x0, 0, window.shape[1] - 1.000001)
        gy = np.clip(fy - y0, 0, window.shape[0] - 1.000001)
        i0 = np.floor(gx).astype(np.intp)
        j0 = np.floor(gy).astype(np.intp)
        ax, ay = gx - i0, gy - j0
        i1 = np.minimum(i0 + 1, window.shape[1] - 1)
        j1 = np.minimum(j0 + 1, window.shape[0] - 1)
        value = (window[j0, i0] * (1 - ax) * (1 - ay) + window[j0, i1] * ax * (1 - ay)
                 + window[j1, i0] * (1 - ax) * ay + window[j1, i1] * ax * ay)
        inside = (fx >= -0.5) & (fx <= self.width - 0.5) & (fy >= -0.5) & (fy <= self.height - 0.5)
        result[inside] = -value[inside]
        result[result <= 0.0] = np.nan
        return result


# =============================================================================
#  Piccoli attrezzi GDAL
# =============================================================================

_GDAL_MODULES: tuple | None = None


def _gdal():
    """I moduli di GDAL, con le eccezioni accese (una volta: si chiama per ogni geometria)."""
    global _GDAL_MODULES
    if _GDAL_MODULES is None:
        from osgeo import gdal, ogr, osr
        gdal.UseExceptions()
        ogr.UseExceptions()
        osr.UseExceptions()
        _GDAL_MODULES = (gdal, ogr, osr)
    return _GDAL_MODULES


def _memory_vector():
    """Un datasource vettoriale in memoria (il nome del driver e' cambiato con GDAL 3.11)."""
    gdal, ogr, _ = _gdal()
    driver = ogr.GetDriverByName("Memory")
    if driver is not None:
        return driver.CreateDataSource("")
    return gdal.GetDriverByName("MEM").Create("", 0, 0, 0, gdal.GDT_Unknown)


def _mem_raster(width: int, height: int, geotransform, dtype=None, fill=0.0, nodata=None,
                lonlat: bool = True):
    gdal, _, _ = _gdal()
    dataset = gdal.GetDriverByName("MEM").Create("", width, height, 1,
                                                 dtype if dtype is not None else gdal.GDT_Float32)
    dataset.SetGeoTransform(list(geotransform))
    if lonlat:
        dataset.SetProjection(_lonlat_srs().ExportToWkt())
    band = dataset.GetRasterBand(1)
    if nodata is not None:
        band.SetNoDataValue(nodata)
    band.Fill(fill)
    return dataset


def _layer(datasource, name: str, fields: tuple[str, ...]):
    _, ogr, _ = _gdal()
    layer = datasource.CreateLayer(name, srs=_lonlat_srs(), geom_type=ogr.wkbUnknown)
    for field in fields:
        layer.CreateField(ogr.FieldDefn(field, ogr.OFTReal))
    return layer


def _add(layer, geometry, **values) -> None:
    _, ogr, _ = _gdal()
    feature = ogr.Feature(layer.GetLayerDefn())
    feature.SetGeometry(geometry)
    for key, value in values.items():
        feature.SetField(key, float(value))
    layer.CreateFeature(feature)


def _rasterize(dataset, layer, *, burn: float | None = None, attribute: str | None = None,
               from_z: bool = False, all_touched: bool = False) -> np.ndarray:
    """Rasterizza su un raster gia' riempito, e ne restituisce l'array."""
    gdal, _, _ = _gdal()
    options = []
    if all_touched:
        options.append("ALL_TOUCHED=TRUE")
    if attribute:
        options.append(f"ATTRIBUTE={attribute}")
        gdal.RasterizeLayer(dataset, [1], layer, options=options)
    elif from_z:
        options.append("BURN_VALUE_FROM=Z")
        gdal.RasterizeLayer(dataset, [1], layer, burn_values=[0.0], options=options)
    else:
        gdal.RasterizeLayer(dataset, [1], layer, burn_values=[burn], options=options)
    return dataset.GetRasterBand(1).ReadAsArray()


def _proximity_m(mask: np.ndarray, target: int, pixel_m: float) -> np.ndarray:
    """
    Distanza in metri da ogni pixel al pixel piu' vicino con mask == target.

    Si misura in PIXEL (ComputeProximity con DISTUNITS=PIXEL) e si moltiplica
    per il lato medio del pixel in metri. In gradi i pixel sono quadrati, in
    metri no (a 45° un grado di longitudine e' il 71% di uno di latitudine):
    la distanza vera e' fino al 16-19% diversa a seconda della direzione.
    Per una riva nitida, una fascia di schiuma e una rampa di fondale
    inventata e' piu' che abbastanza; e la prima idea (dare a GDAL pixel
    rettangolari in metri) non funziona: ComputeProximity usa solo il lato X,
    e lo dice con un avviso.
    """
    gdal, _, _ = _gdal()
    height, width = mask.shape
    if not (mask == target).any():
        return np.full(mask.shape, 1.0e9, dtype=np.float64)
    source = _mem_raster(width, height, (0.0, 1.0, 0.0, 0.0, 0.0, -1.0), gdal.GDT_Byte)
    source.GetRasterBand(1).WriteArray(mask.astype(np.uint8))
    destination = _mem_raster(width, height, (0.0, 1.0, 0.0, 0.0, 0.0, -1.0), gdal.GDT_Float32)
    gdal.ComputeProximity(source.GetRasterBand(1), destination.GetRasterBand(1),
                          options=[f"VALUES={target}", "DISTUNITS=PIXEL"])
    return destination.GetRasterBand(1).ReadAsArray().astype(np.float64) * pixel_m


def _fill_nearest(values: np.ndarray, max_pixels: int) -> np.ndarray:
    """Riempie i NaN con i valori vicini (gdal.FillNodata), fino a max_pixels di distanza."""
    gdal, _, _ = _gdal()
    if np.isfinite(values).all() or not np.isfinite(values).any():
        return values
    nodata = -1.0e30
    height, width = values.shape
    dataset = _mem_raster(width, height, (0, 1, 0, 0, 0, -1), gdal.GDT_Float64, nodata=nodata)
    band = dataset.GetRasterBand(1)
    band.WriteArray(np.where(np.isfinite(values), values, nodata))
    gdal.FillNodata(targetBand=band, maskBand=None, maxSearchDist=max_pixels, smoothingIterations=0)
    filled = band.ReadAsArray().astype(np.float64)
    filled[filled <= nodata * 0.5] = np.nan
    return filled


def _lonlat_srs():
    _, _, osr = _gdal()
    srs = osr.SpatialReference()
    srs.ImportFromEPSG(4326)
    srs.SetAxisMappingStrategy(osr.OAMS_TRADITIONAL_GIS_ORDER)
    return srs


def _metric_transforms(lon0: float, lat0: float):
    """Da lon/lat a metri attorno al punto dato (equidistante azimutale), e ritorno."""
    _, _, osr = _gdal()
    metric = osr.SpatialReference()
    metric.ImportFromProj4(f"+proj=aeqd +lat_0={lat0} +lon_0={lon0} +datum=WGS84 +units=m +no_defs")
    geographic = _lonlat_srs()
    return (osr.CoordinateTransformation(geographic, metric),
            osr.CoordinateTransformation(metric, geographic))


def _area_km2(geometry) -> float:
    west, east, south, north = geometry.GetEnvelope()
    forward, _ = _metric_transforms(0.5 * (west + east), 0.5 * (south + north))
    copy = geometry.Clone()
    copy.Transform(forward)
    return copy.GetArea() / 1.0e6


def _mean_width_m(geometry, area_km2: float) -> float:
    """
    La larghezza media di un'area d'acqua: 2 x area / perimetro. Per un nastro
    (un fiume) e' la sua larghezza; per un cerchio e' il raggio, cioe' meta'
    del diametro: per decidere se un'acqua "si vede" nella mesh e' dalla parte
    giusta (prudente). La prima versione usava la radice dell'area: per i
    26 km² del Po vicino a Torino, 5 km di "larghezza" a un fiume di 150 m.
    """
    west, east, south, north = geometry.GetEnvelope()
    forward, _ = _metric_transforms(0.5 * (west + east), 0.5 * (south + north))
    copy = geometry.Clone()
    copy.Transform(forward)
    boundary = copy.Boundary()
    perimeter = boundary.Length() if boundary is not None else 0.0
    return 2.0 * area_km2 * 1.0e6 / perimeter if perimeter > 0.0 else math.sqrt(area_km2) * 1000.0


# =============================================================================
#  I corpi d'acqua
# =============================================================================

@dataclass
class Body:
    kind: int
    is_line: bool
    level: float              # NaN: pixel per pixel (geoide, o la Z della linea)
    wave: float
    deepest: float
    width: float
    area_km2: float
    geometry: object          # ogr.Geometry, lon/lat; le linee con Z = quota dell'acqua
    envelope: tuple[float, float, float, float] = (0.0, 0.0, 0.0, 0.0)   # west, east, south, north

    def __post_init__(self):
        self.envelope = tuple(self.geometry.GetEnvelope())


def encode_body(body: Body) -> bytes:
    _, ogr, _ = _gdal()
    wkb = bytes(body.geometry.ExportToWkb(ogr.wkbNDR))
    return BLOCK_RECORD.pack(body.kind, 1 if body.is_line else 0, 0, body.level, body.wave,
                             body.deepest, body.width, body.area_km2, len(wkb)) + wkb


def read_block(path: str) -> list[Body]:
    _, ogr, _ = _gdal()
    with open(path, "rb") as handle:
        blob = handle.read()
    bodies = []
    offset = 0
    while offset < len(blob):
        kind, is_line, _, level, wave, deepest, width, area, size = BLOCK_RECORD.unpack_from(blob, offset)
        offset += BLOCK_RECORD.size
        geometry = ogr.CreateGeometryFromWkb(blob[offset: offset + size])
        offset += size
        bodies.append(Body(kind, bool(is_line), level, wave, deepest, width, area, geometry))
    return bodies


def _sample_inside(geometry, max_points: int = 4000) -> tuple[np.ndarray, np.ndarray]:
    """
    Punti regolari dentro un poligono (per la quota di un lago).

    Il passo viene dall'AREA, non dal riquadro: un fiume lungo e stretto ha un
    riquadro enorme e un'area piccola, e una griglia sul riquadro (la prima
    versione) metteva i punti a 1 km l'uno dall'altro, quasi tutti fuori dal
    fiume largo 150 m.
    """
    gdal, _, _ = _gdal()
    west, east, south, north = geometry.GetEnvelope()
    area = max(geometry.GetArea(), 1e-18)                       # gradi²
    step = max(math.sqrt(area / max_points), max(east - west, north - south) / 4096.0, 1e-9)
    width = max(1, int(math.ceil((east - west) / step)))
    height = max(1, int(math.ceil((north - south) / step)))
    raster = _mem_raster(width, height, (west, step, 0, north, 0, -step), gdal.GDT_Byte)
    datasource = _memory_vector()
    layer = _layer(datasource, "lago", ())
    _add(layer, geometry)
    mask = _rasterize(raster, layer, burn=1, all_touched=True)
    rows, columns = np.nonzero(mask)
    if len(rows) == 0:
        centroid = geometry.Centroid()
        return np.array([centroid.GetX()]), np.array([centroid.GetY()])
    return west + (columns + 0.5) * step, north - (rows + 0.5) * step


def lake_level(geometry, terrain: TerrainSampler) -> float:
    """
    La quota di un lago: il 20° percentile del terreno dentro il poligono.

    Il DEM sopra un lago e' di solito piano alla quota dell'acqua, ma ai bordi
    prende la riva e a volte ha buchi o picchi (barche, pontili, errori). Un
    percentile basso li ignora tutti e due senza scendere nel rumore dei buchi.
    """
    lons, lats = _sample_inside(geometry)
    heights = terrain.heights(lons, lats)
    heights = heights[np.isfinite(heights)]
    return float(np.percentile(heights, 20)) if heights.size else float("nan")


#: Mediana mobile sul profilo di un fiume, in campioni (9 x 20 m = 180 m): toglie
#: i buchi e i picchi brevi del DEM prima del minimo progressivo.
RIVER_MEDIAN_SAMPLES = 9

#: Un tratto del profilo piu' basso di cosi' rispetto alla mediana del km
#: attorno e' un errore del DEM, non il fiume: si scarta e si interpola. (Un
#: salto vero di 15 m in mezzo km e' una diga: meglio una rampa che un buco.)
RIVER_OUTLIER_DROP_M = 15.0
RIVER_OUTLIER_WINDOW = 51

#: La cucitura fra due linee non abbassa l'inizio di una linea di piu' di
#: cosi' rispetto al suo terreno: se la linea a monte finisce MOLTO piu' in
#: basso, l'errore e' a monte, e non si propaga a tutto il fiume.
MAX_STITCH_DROP_M = 5.0


def _densify(points: np.ndarray) -> np.ndarray:
    """I punti di una linea, con un punto ogni RIVER_SAMPLE_M metri (gli estremi restano quelli veri)."""
    lon = points[:, 0]
    lat = points[:, 1]
    dense_lon = [lon[0]]
    dense_lat = [lat[0]]
    for index in range(1, len(lon)):
        east, north = tiling.metres_per_degree(0.5 * (lat[index] + lat[index - 1]))
        dx = (lon[index] - lon[index - 1]) * east
        dy = (lat[index] - lat[index - 1]) * north
        steps = max(1, int(math.ceil(math.hypot(dx, dy) / RIVER_SAMPLE_M)))
        for step in range(1, steps + 1):
            t = step / steps
            dense_lon.append(lon[index - 1] + (lon[index] - lon[index - 1]) * t)
            dense_lat.append(lat[index - 1] + (lat[index] - lat[index - 1]) * t)
    return np.column_stack([dense_lon, dense_lat])


def _running_median(values: np.ndarray, window: int) -> np.ndarray:
    half = window // 2
    if len(values) <= 2 * half:
        return np.full(len(values), float(np.median(values)))
    padded = np.pad(values, half, mode="edge")
    return np.median(np.lib.stride_tricks.sliding_window_view(padded, 2 * half + 1), axis=1)


def _smooth_heights(raw: np.ndarray) -> np.ndarray | None:
    """
    Il terreno lungo un fiume, pulito; NaN dove non si sa. None se non si sa
    da nessuna parte.

    - Si usano SOLO i campioni validi, e non quelli a ridosso di un buco: un
      post sul bordo del DEM mescola il dato vero con il riempimento (a Torino,
      194 m invece di 262). La prima versione estendeva il primo campione
      valido a tutta la parte scoperta, e il minimo progressivo se lo portava
      dietro per tutto il fiume.
    - I crolli lunghi (un buco del DEM di qualche centinaio di metri) si
      confrontano con la mediana del km e si tolgono.
    - Una mediana corta toglie i buchi e i picchi brevi (un ponte).
    """
    valid = np.isfinite(raw)
    trusted = valid.copy()
    for shift in (1, 2):
        trusted[shift:] &= valid[:-shift]
        trusted[:-shift] &= valid[shift:]
    if trusted.sum() < 3:
        trusted = valid
    if not trusted.any():
        return None
    indices = np.nonzero(trusted)[0]
    values = raw[indices]
    keep = values > _running_median(values, RIVER_OUTLIER_WINDOW) - RIVER_OUTLIER_DROP_M
    if keep.any():
        indices, values = indices[keep], values[keep]
    result = np.full(len(raw), np.nan)
    result[indices] = _running_median(values, RIVER_MEDIAN_SAMPLES)
    return result


def _profile(heights: np.ndarray, cap: float = math.inf) -> np.ndarray:
    """
    La quota dell'acqua: il minimo progressivo dei campioni validi (partendo al
    massimo da `cap`), e nei tratti senza terreno la quota del tratto valido
    piu' vicino. Un tratto scoperto non entra nel minimo: non sa niente.
    """
    indices = np.nonzero(np.isfinite(heights))[0]
    running = np.minimum.accumulate(np.minimum(heights[indices], cap))
    return np.interp(np.arange(len(heights)), indices, running)


def _first_valid(heights: np.ndarray) -> float:
    return float(heights[np.isfinite(heights)][0])


def _flows_backwards(heights: np.ndarray) -> bool:
    """
    La linea sale nel suo verso? Le way dei fiumi in OSM vanno verso valle, ma
    non sempre. Si confrontano il primo e l'ultimo quinto dei campioni validi
    (mediane), con una soglia prudente: almeno 2 m, e 1 m per km (i fiumi di
    pianura scendono di pochi decimetri per km, un ponte o il rumore non
    fanno 2 m di mediana).
    """
    values = heights[np.isfinite(heights)]
    count = len(values)
    if count < 10:
        return False
    fifth = max(2, count // 5)
    rise = float(np.median(values[-fifth:]) - np.median(values[:fifth]))
    return rise > max(2.0, count * RIVER_SAMPLE_M / 1000.0)


def river_profile(points: np.ndarray, terrain: TerrainSampler) -> tuple[np.ndarray, np.ndarray] | None:
    """
    L'asse di UNA linea ogni RIVER_SAMPLE_M metri, con la quota dell'acqua: il
    terreno sull'asse (pulito, _smooth_heights) con il MINIMO PROGRESSIVO nel
    verso della linea. Dove il DEM risale (un ponte, una briglia) l'acqua non
    risale con lui. Per piu' linee dello stesso fiume: stitch_river_profiles.
    """
    dense = _densify(points)
    heights = _smooth_heights(terrain.heights(dense[:, 0], dense[:, 1]))
    if heights is None:
        return None
    return dense, _profile(heights)


@dataclass
class RiverLine:
    """Una linea d'acqua in attesa del profilo: i punti fitti e il terreno sotto."""
    kind: int
    width: float
    dense: np.ndarray        # (n, 2) lon, lat
    heights: np.ndarray      # terreno pulito sui punti (NaN dove non si sa)


def _endpoint(point) -> tuple[int, int]:
    return int(round(float(point[0]) * 1.0e7)), int(round(float(point[1]) * 1.0e7))


def stitch_river_profiles(lines: list[RiverLine], passes: int = 12) -> list[np.ndarray]:
    """
    I profili di tutte le linee, CUCITI dove una linea comincia nel punto in
    cui ne finisce un'altra.

    In OSM un fiume e' spezzato in decine di way. Col minimo progressivo fatto
    way per way, ogni way ripartiva dal terreno del suo primo punto: se la way a
    monte era finita piu' in basso (un tratto basso del DEM), l'acqua all'inizio
    della way a valle RISALIVA, con un gradino di metri. Qui ogni linea parte al
    massimo dalla quota a cui finiscono le linee che entrano nel suo primo
    punto (anche gli affluenti che sboccano li'). Le quote possono solo
    scendere, quindi qualche passata basta a stabilizzarle.
    """
    ending_at: dict[tuple[int, int], list[int]] = {}
    for index, line in enumerate(lines):
        ending_at.setdefault(_endpoint(line.dense[-1]), []).append(index)
    caps = [math.inf] * len(lines)
    profiles = [_profile(line.heights) for line in lines]
    # Prima le linee piu' alte (a monte): di solito basta una passata.
    order = sorted(range(len(lines)), key=lambda i: -_first_valid(lines[i].heights))
    for _ in range(passes):
        changed = False
        for index in order:
            incoming = [j for j in ending_at.get(_endpoint(lines[index].dense[0]), []) if j != index]
            if not incoming:
                continue
            cap = min(float(profiles[j][-1]) for j in incoming)
            cap = max(cap, _first_valid(lines[index].heights) - MAX_STITCH_DROP_M)
            if cap < caps[index] - 1.0e-6:
                caps[index] = cap
                profiles[index] = _profile(lines[index].heights, cap)
                changed = True
        if not changed:
            break
    return profiles


# =============================================================================
#  Stadio 1: estrai
# =============================================================================

#: Tag che servono alla classificazione dell'acqua (dalle colonne del driver OSM).
WATER_TAGS = ("natural", "water", "waterway", "landuse", "intermittent", "seasonal", "tunnel",
              "covered", "location", "width", "leisure", "amenity", "indoor", "name")


def iter_osm_water(path: str, bbox, report: Report, counts: dict) -> Iterator[tuple[str, object, object]]:
    """("area" | "linea", classificazione, geometria lon/lat) dall'estratto OSM."""
    gdal, ogr, _ = _gdal()
    gdal.SetConfigOption("OSM_MAX_TMPFILE_SIZE", "1024")
    source = gdal.OpenEx(path, gdal.OF_VECTOR, open_options=["INTERLEAVED_READING=YES"])
    if source is None:
        raise RuntimeError(f"GDAL non riesce ad aprire {path}")
    if source.GetDriver().GetDescription() == "OSM":
        # Solo i due layer che servono: niente punti, niente relazioni "altre".
        # E la lettura dal dataset, interlacciata: vedi osmextract.iter_lines.
        source.ExecuteSQL("SET interest_layers = lines,multipolygons")

    names = {}
    for layer_name in ("lines", "multipolygons"):
        layer = source.GetLayerByName(layer_name)
        if layer is None:
            continue
        definition = layer.GetLayerDefn()
        names[layer_name] = {definition.GetFieldDefn(i).GetName() for i in range(definition.GetFieldCount())}
        if bbox is not None:
            layer.SetSpatialFilterRect(*bbox)

    while True:
        feature, owner = source.GetNextFeature(include_layer=True)
        if feature is None:
            break
        if owner is None or owner.GetName() not in names:
            continue
        layer_name = owner.GetName()
        fields = names[layer_name]
        counts["lette"] = counts.get("lette", 0) + 1
        if counts["lette"] % 500000 == 0:
            report(f"      {counts['lette']:,} oggetti letti")

        tags = parse_other_tags(feature.GetField("other_tags")) if "other_tags" in fields else {}
        for tag in WATER_TAGS:
            if tag in fields:
                value = feature.GetField(tag)
                if value is not None:
                    tags[tag] = value

        geometry = feature.GetGeometryRef()
        if geometry is None:
            continue
        if layer_name == "lines":
            classified = waterclasses.classify_line(tags)
        else:
            classified = waterclasses.classify_area(tags)
        if classified is None:
            continue
        geometry = geometry.Clone()
        geometry.FlattenTo2D()
        if bbox is not None:
            west, east, south, north = geometry.GetEnvelope()
            if east < bbox[0] or west > bbox[2] or north < bbox[1] or south > bbox[3]:
                continue
        yield ("linea" if layer_name == "lines" else "area"), classified, geometry


def _open_sea(path: str):
    """Il file dei poligoni del mare: lo zip di osmdata, uno shapefile, un GeoPackage."""
    gdal, _, _ = _gdal()
    candidates = [path]
    if path.lower().endswith(".zip"):
        candidates = ["/vsizip/" + path]
        listing = gdal.ReadDirRecursive("/vsizip/" + path) or []
        candidates = ["/vsizip/" + os.path.join(path, name) for name in listing
                      if name.lower().endswith(".shp")] + candidates
    for candidate in candidates:
        try:
            dataset = gdal.OpenEx(candidate, gdal.OF_VECTOR)
        except RuntimeError:
            dataset = None
        if dataset is not None and dataset.GetLayerCount() > 0:
            return dataset
    raise RuntimeError(f"non trovo poligoni in {path}")


def iter_sea(path: str, bbox) -> Iterator[object]:
    _, ogr, _ = _gdal()
    dataset = _open_sea(path)
    layer = dataset.GetLayer(0)
    clip = None
    if bbox is not None:
        # Il ritaglio si allarga di SEA_CLIP_MARGIN_DEG: il bordo del ritaglio
        # e' una riva FINTA (il poligono finisce, oltre c'e' "terra"), e il
        # fondale vicino all'area deve sapere che la riva vera e' lontana.
        west, south, east, north = (bbox[0] - SEA_CLIP_MARGIN_DEG, bbox[1] - SEA_CLIP_MARGIN_DEG,
                                    bbox[2] + SEA_CLIP_MARGIN_DEG, bbox[3] + SEA_CLIP_MARGIN_DEG)
        layer.SetSpatialFilterRect(west, south, east, north)
        clip = ogr.CreateGeometryFromWkt(
            f"POLYGON(({west} {south},{east} {south},{east} {north},{west} {north},{west} {south}))")
    for feature in layer:
        geometry = feature.GetGeometryRef()
        if geometry is None:
            continue
        geometry = geometry.Clone()
        geometry.FlattenTo2D()
        if clip is not None:
            geometry = geometry.Intersection(clip)
            if geometry is None or geometry.IsEmpty():
                continue
        yield geometry


def _block_name(x: int, y: int) -> str:
    return f"{x}_{y}.bin"


def _block_box(bx: int, by: int):
    _, ogr, _ = _gdal()
    span = tiling.tile_span_deg(BLOCK_LEVEL)
    west = -180.0 + bx * span - BLOCK_MARGIN_DEG
    north = 90.0 - by * span + BLOCK_MARGIN_DEG
    east = west + span + 2 * BLOCK_MARGIN_DEG
    south = north - span - 2 * BLOCK_MARGIN_DEG
    return ogr.CreateGeometryFromWkt(
        f"POLYGON(({west} {south},{east} {south},{east} {north},{west} {north},{west} {south}))")


def extract_bodies(osm_paths: list[str], sea_path: str | None, terrain: TerrainSampler,
                   bbox, blocks_dir: str, report: Report = _null_report) -> dict:
    """Stadio 1: classifica, misura, mette nei blocchi. Ritorna un riassunto."""
    _, ogr, _ = _gdal()
    if os.path.isdir(blocks_dir):
        shutil.rmtree(blocks_dir)
    os.makedirs(blocks_dir)

    span = tiling.tile_span_deg(BLOCK_LEVEL)
    pending: dict[tuple[int, int], bytearray] = {}
    counts = {"lette": 0}
    bodies_by_kind: dict[str, int] = {}
    skipped = {"senza terreno": 0, "troppo piccoli": 0}
    extent = [180.0, 90.0, -180.0, -90.0]
    lines: list[RiverLine] = []

    def store(body: Body) -> None:
        west, east, south, north = body.envelope
        extent[0] = min(extent[0], west)
        extent[1] = min(extent[1], south)
        extent[2] = max(extent[2], east)
        extent[3] = max(extent[3], north)
        bodies_by_kind[waterclasses.KIND_NAMES[body.kind]] = \
            bodies_by_kind.get(waterclasses.KIND_NAMES[body.kind], 0) + 1
        x0 = int((west - BLOCK_MARGIN_DEG + 180.0) // span)
        x1 = int((east + BLOCK_MARGIN_DEG + 180.0) // span)
        y0 = int((90.0 - north - BLOCK_MARGIN_DEG) // span)
        y1 = int((90.0 - south + BLOCK_MARGIN_DEG) // span)
        for by in range(max(0, y0), y1 + 1):
            for bx in range(max(0, x0), x1 + 1):
                piece = body
                if not body.is_line and (x1 > x0 or y1 > y0):
                    # I poligoni grandi (il mare, il Garda) si tagliano sul
                    # blocco: ogni tile ne rasterizza solo il pezzo vicino.
                    clipped = body.geometry.Intersection(_block_box(bx, by))
                    if clipped is None or clipped.IsEmpty():
                        continue
                    piece = Body(body.kind, False, body.level, body.wave, body.deepest,
                                 body.width, body.area_km2, clipped)
                if not body.is_line and body.kind in (waterclasses.KIND_RIVER, waterclasses.KIND_CANAL):
                    # Il ripiego della quota, sul pezzo del blocco (NaN se il
                    # pezzo e' fuori dal terreno: allora solo l'asse).
                    piece = Body(piece.kind, False, lake_level(piece.geometry, terrain), piece.wave,
                                 piece.deepest, piece.width, piece.area_km2, piece.geometry)
                pending.setdefault((bx, by), bytearray()).extend(encode_body(piece))
        if sum(len(data) for data in pending.values()) > 64 * 1024 * 1024:
            flush()

    def flush() -> None:
        for (bx, by), data in pending.items():
            with open(os.path.join(blocks_dir, _block_name(bx, by)), "ab") as handle:
                handle.write(data)
        pending.clear()

    for path in osm_paths:
        report(f"      leggo {path}")
        for shape, classified, geometry in iter_osm_water(path, bbox, report, counts):
            if shape == "area":
                area = _area_km2(geometry)
                if area < MIN_LAKE_KM2:
                    skipped["troppo piccoli"] += 1
                    continue
                kind = classified.kind
                level = float("nan")
                if kind == waterclasses.KIND_LAKE:
                    level = lake_level(geometry, terrain)
                    if not math.isfinite(level):
                        skipped["senza terreno"] += 1
                        continue
                # I fiumi e i canali larghi prendono la quota dall'ASSE (la
                # linea che li attraversa), nel taglio: qui non serve niente.
                # Se l'asse manca, il ripiego si calcola pezzo per pezzo in
                # store(), sul pezzo vicino, non su 26 km² di fiume in discesa.
                width = _mean_width_m(geometry, area)
                store(Body(kind, False, level, waterclasses.wave_exposure(kind, area),
                           waterclasses.max_depth(kind, area, width), width, area, geometry))
            else:
                parts = _linestring_from_wkb(bytes(geometry.ExportToWkb(ogr.wkbNDR)))
                for part in parts:
                    if len(part) < 2:
                        continue
                    dense = _densify(np.asarray(part, dtype=np.float64)[:, :2])
                    heights = _smooth_heights(terrain.heights(dense[:, 0], dense[:, 1]))
                    if heights is None:
                        skipped["senza terreno"] += 1
                        continue
                    if _flows_backwards(heights):
                        dense, heights = dense[::-1].copy(), heights[::-1].copy()
                        skipped["girate (salivano)"] = skipped.get("girate (salivano)", 0) + 1
                    lines.append(RiverLine(classified.kind, classified.width_m, dense, heights))

    # Le linee, tutte insieme: i profili si cuciono fra una way e l'altra.
    for river, levels in zip(lines, stitch_river_profiles(lines)):
        line = ogr.Geometry(ogr.wkbLineString25D)
        for (lon, lat), z in zip(river.dense, levels):
            line.AddPoint(float(lon), float(lat), float(z))
        store(Body(river.kind, True, float("nan"), waterclasses.wave_exposure(river.kind),
                   waterclasses.max_depth(river.kind, 0.0, river.width), river.width, 0.0, line))

    if sea_path:
        report(f"      leggo il mare da {sea_path}")
        for geometry in iter_sea(sea_path, bbox):
            store(Body(waterclasses.KIND_SEA, False, float("nan"), 1.0,
                       waterclasses.max_depth(waterclasses.KIND_SEA), 0.0, 0.0, geometry))

    flush()
    blocks = sorted(os.path.basename(p) for p in glob.glob(os.path.join(blocks_dir, "*.bin")))
    return {"oggettiLetti": counts["lette"], "corpi": bodies_by_kind, "scartati": skipped,
            "estensione": extent if extent[0] <= extent[2] else None, "blocchi": blocks}


# =============================================================================
#  Stadio 2: taglia
# =============================================================================

#: La corrente nel raster: centesimi di m/s in int8 (1 m/s = 100).
FLOW_SCALE = 100.0


@dataclass
class _Layers:
    """I corpi vicini a una tile, divisi per come si rasterizzano."""
    sea: object
    river_areas: object
    river_buffers: object
    centrelines: object
    flow: object
    lakes: object
    datasource: object
    counts: dict
    #: Gli assi che ATTRAVERSANO un'area fluviale, e la loro corrente: solo loro
    #: danno quota e corrente alle aree (non il canale che le costeggia).
    area_axes: object = None
    area_flow: object = None


def _box(west, south, east, north):
    _, ogr, _ = _gdal()
    return ogr.CreateGeometryFromWkt(
        f"POLYGON(({west} {south},{east} {south},{east} {north},{west} {north},{west} {south}))")


def _resolved(body: Body, post_m: float) -> bool:
    """
    L'acqua e' abbastanza larga da scavarla a questo livello? (waterclasses.
    RESOLVE_POSTS.) Il mare e le lagune si'. Il passo dei post e' quello
    NOMINALE del livello (in latitudine): uguale in tutte le tile del livello,
    cosi' un fiume non e' scavato in una tile e non in quella accanto.
    """
    if body.kind in (waterclasses.KIND_SEA, waterclasses.KIND_LAGOON):
        return True
    return body.width >= waterclasses.RESOLVE_POSTS * post_m


def _prepare_layers(bodies: list[Body], roi, tile_centre, metres_per_px: float,
                    simplify_deg: float, post_m: float = 0.0) -> _Layers:
    """Ritaglia, bufferizza e mette in layer di memoria i corpi vicini alla tile."""
    _, ogr, _ = _gdal()
    west, south, east, north = roi
    clip = _box(west, south, east, north)
    datasource = _memory_vector()
    fields = ("level", "wave", "d0", "slope", "deep", "kind", "res", "vs")
    layers = {name: _layer(datasource, name, fields)
              for name in ("sea", "river_areas", "river_buffers", "lakes")}
    centrelines = _layer(datasource, "centrelines", ())
    flow = _layer(datasource, "flow", ("fu", "fv"))
    area_axes = _layer(datasource, "area_axes", ())
    area_flow = _layer(datasource, "area_flow", ("fu", "fv"))
    forward, backward = _metric_transforms(*tile_centre)
    counts = {name: 0 for name in ("sea", "river_areas", "river_buffers", "lakes")}

    # Le aree fluviali vicine, per sapere quali linee le attraversano. La
    # prima versione dava a ogni pixel di un'area la quota dell'asse PIU'
    # VICINO, qualunque fosse: un canale a fianco del Po, 4 m piu' alto,
    # inclinava mezzo fiume verso di se'.
    river_area_shapes = []
    for body in bodies:
        if not body.is_line and body.kind in (waterclasses.KIND_RIVER, waterclasses.KIND_CANAL):
            bw, be, bs, bn = body.envelope
            if not (be < west or bw > east or bn < south or bs > north):
                river_area_shapes.append(body.geometry)

    def crosses_river_area(line) -> bool:
        lw, le, ls, ln = line.GetEnvelope()[:4]
        for shape in river_area_shapes:
            aw, ae, as_, an = shape.GetEnvelope()
            if le < aw or lw > ae or ln < as_ or ls > an:
                continue
            if line.Intersects(shape):
                return True
        return False

    def add_flow(target, body, speed) -> None:
        points = body.geometry.GetPoints()
        for (lon0, lat0, *_), (lon1, lat1, *_) in zip(points[:-1], points[1:]):
            east_m, north_m = tiling.metres_per_degree(0.5 * (lat0 + lat1))
            dx = (lon1 - lon0) * east_m
            dy = (lat1 - lat0) * north_m          # verso NORD
            length = math.hypot(dx, dy)
            if length <= 0.0:
                continue
            segment = ogr.Geometry(ogr.wkbLineString)
            segment.AddPoint_2D(lon0, lat0)
            segment.AddPoint_2D(lon1, lat1)
            # u verso est, v verso SUD (come le righe del raster).
            _add(target, segment, fu=speed * dx / length, fv=-speed * dy / length)

    # Dentro ogni layer i corpi larghi per ultimi: dove due si sovrappongono
    # vincono le regole (profondita', "risolto") di quello largo.
    for body in sorted(bodies, key=lambda candidate: candidate.width):
        bw, be, bs, bn = body.envelope
        reach = 0.0 if not body.is_line else body.width / 111000.0 + 1e-6
        if be + reach < west or bw - reach > east or bn + reach < south or bs - reach > north:
            continue
        # Ai livelli bassi le cose piccole spariscono: un laghetto sotto i 4
        # pixel, un fiume piu' stretto di mezzo pixel.
        if body.kind == waterclasses.KIND_LAKE and body.area_km2 * 1.0e6 < 4.0 * metres_per_px ** 2:
            continue
        if body.is_line and crosses_river_area(body.geometry):
            # L'asse di un'area fluviale (il Po a Torino e' un'area, con la sua
            # linea dentro): porta quota e corrente a tutta l'area, a
            # qualunque livello, anche dove la linea e' troppo sottile per
            # disegnarla.
            _add(area_axes, body.geometry)
            add_flow(area_flow, body, waterclasses.FLOW_SPEED.get(body.kind, 0.0))
        if body.is_line and body.width < 0.5 * metres_per_px:
            continue

        shore, slope = waterclasses.ramp(body.kind)
        values = {"level": body.level if math.isfinite(body.level) else NO_LEVEL, "wave": body.wave,
                  "d0": shore, "slope": slope, "deep": body.deepest, "kind": body.kind,
                  "res": 1.0 if _resolved(body, post_m) else 0.0, "vs": waterclasses.shore_slope(body.kind)}

        if not body.is_line:
            geometry = body.geometry.Intersection(clip)
            if geometry is None or geometry.IsEmpty():
                continue
            if simplify_deg > 0.0:
                geometry = geometry.SimplifyPreserveTopology(simplify_deg)
            name = ("sea" if body.kind in (waterclasses.KIND_SEA, waterclasses.KIND_LAGOON)
                    else "lakes" if body.kind == waterclasses.KIND_LAKE else "river_areas")
            _add(layers[name], geometry, **values)
            counts[name] += 1
            continue

        # Le linee: l'asse con le quote (per la superficie), i segmenti con la
        # direzione (per la corrente), il buffer largo quanto il fiume (per la
        # maschera e il fondale).
        _add(centrelines, body.geometry)
        add_flow(flow, body, waterclasses.FLOW_SPEED.get(body.kind, 0.0))

        flat = body.geometry.Clone()
        flat.FlattenTo2D()
        flat = flat.Intersection(_box(west - reach, south - reach, east + reach, north + reach))
        if flat is None or flat.IsEmpty():
            continue
        flat.Transform(forward)
        buffered = flat.Buffer(0.5 * body.width, 4)
        buffered.Transform(backward)
        _add(layers["river_buffers"], buffered, **values)
        counts["river_buffers"] += 1

    return _Layers(layers["sea"], layers["river_areas"], layers["river_buffers"], centrelines, flow,
                   layers["lakes"], datasource, counts, area_axes, area_flow)


def _burn_masks(layers: _Layers, width: int, height: int, geotransform, all_touched: bool) -> dict:
    gdal, _, _ = _gdal()
    masks = {}
    for name in ("sea", "river_areas", "river_buffers", "lakes"):
        layer = getattr(layers, name)
        if layers.counts[name] == 0:
            masks[name] = np.zeros((height, width), dtype=bool)
            continue
        raster = _mem_raster(width, height, geotransform, gdal.GDT_Byte)
        masks[name] = _rasterize(raster, layer, burn=1, all_touched=all_touched).astype(bool)
    return masks


def _burn_attribute(layers: _Layers, names: tuple[str, ...], attribute: str, width: int, height: int,
                    geotransform, all_touched: bool = False) -> np.ndarray:
    """Un attributo dei corpi, nell'ordine dato (chi viene dopo copre)."""
    gdal, _, _ = _gdal()
    result = np.full((height, width), np.nan)
    for name in names:
        if layers.counts[name] == 0:
            continue
        raster = _mem_raster(width, height, geotransform, gdal.GDT_Float32, fill=np.nan)
        burned = _rasterize(raster, getattr(layers, name), attribute=attribute, all_touched=all_touched)
        result = np.where(np.isfinite(burned), burned, result)
    # Una quota che non c'e' (NO_LEVEL) torna NaN: chi viene dopo la riempie.
    return np.where(result <= NO_LEVEL + 1.0, np.nan, result)


def _downsample(values: np.ndarray, factor: int = 2) -> np.ndarray:
    height, width = values.shape
    return values.reshape(height // factor, factor, width // factor, factor).mean(axis=(1, 3))


#: L'ordine in cui i corpi si coprono: il mare sotto, i laghi sopra (un fiume
#: che entra in un lago prende la quota del lago dentro il lago). Le AREE dei
#: fiumi sopra le loro LINEE: il Po a Torino e' un'area larga 128 m con dentro
#: la sua linea, larga 12 m per OSM. Con la linea sopra (la prima versione),
#: il centro del fiume aveva le regole della linea: un metro di fondale, e
#: "troppo stretto per scavarlo".
ORDER = ("sea", "river_buffers", "river_areas", "lakes")


def cut_tile(level: int, x: int, y: int, bodies: list[Body], geoid: GeoidSampler,
             bathymetry: BathymetrySampler | None = None) -> waterformat.WaterTile | None:
    """Una tile d'acqua, o None se qui non c'e' acqua."""
    gdal, _, _ = _gdal()
    bounds = tiling.tile_bounds(level, x, y)
    span = tiling.tile_span_deg(level)
    centre_lat = 0.5 * (bounds.north + bounds.south)
    east_m, north_m = tiling.metres_per_degree(centre_lat)
    mpp = waterformat.metres_per_pixel(level, y)
    cells = tiling.TILE_CELLS
    posts = tiling.TILE_POSTS

    # --- Le due griglie ---------------------------------------------------------
    # F: il raster a risoluzione doppia, con 16 pixel (della tile) di margine.
    margin = RASTER_MARGIN
    px1 = span / waterformat.RASTER
    px2 = px1 / 2.0
    size_f = 2 * (waterformat.RASTER + 2 * margin)
    west_f = bounds.west - margin * px1
    north_f = bounds.north + margin * px1
    gt_f = (west_f, px2, 0.0, north_f, 0.0, -px2)

    # P: i post, con il margine che serve alla distanza dalla riva (fino a
    # 1,2 km, o almeno quanto F). Pixel CENTRATI sui post.
    post_deg = span / cells
    post_m = post_deg * min(east_m, north_m)
    margin_p = int(min(256, max(margin // 2 + 2, math.ceil(waterclasses.MAX_RAMP_DISTANCE_M / post_m))))
    size_p = posts + 2 * margin_p
    gt_p = (bounds.west - (margin_p + 0.5) * post_deg, post_deg, 0.0,
            bounds.north + (margin_p + 0.5) * post_deg, 0.0, -post_deg)

    roi = (bounds.west - (margin_p + 1) * post_deg, bounds.south - (margin_p + 1) * post_deg,
           bounds.east + (margin_p + 1) * post_deg, bounds.north + (margin_p + 1) * post_deg)
    nominal_post_m = tiling.post_spacing_deg(level) * NOMINAL_METRES_PER_DEGREE
    layers = _prepare_layers(bodies, roi, (0.5 * (bounds.west + bounds.east), centre_lat), mpp,
                             simplify_deg=px2 * 0.5 if level < 12 else 0.0, post_m=nominal_post_m)
    if sum(layers.counts.values()) == 0:
        return None

    # --- Il raster F: maschere, quota, onde, corrente ----------------------------
    masks = _burn_masks(layers, size_f, size_f, gt_f, all_touched=False)
    water = masks["sea"] | masks["river_areas"] | masks["river_buffers"] | masks["lakes"]
    river = (masks["river_areas"] | masks["river_buffers"]) & ~masks["lakes"]

    lons_f = west_f + (np.arange(size_f) + 0.5) * px2
    lats_f = north_f - (np.arange(size_f) + 0.5) * px2
    level_f = np.full((size_f, size_f), np.nan)
    if masks["sea"].any():
        sea_level = geoid(lons_f[np.newaxis, :], lats_f[:, np.newaxis])
        level_f = np.where(masks["sea"], sea_level, level_f)
    if layers.counts["river_areas"]:
        areas = _burn_attribute(layers, ("river_areas",), "level", size_f, size_f, gt_f)
        level_f = np.where(masks["river_areas"] & np.isfinite(areas), areas, level_f)
    has_lines = layers.centrelines.GetFeatureCount() > 0
    has_area_axes = layers.area_axes.GetFeatureCount() > 0 and masks["river_areas"].any()
    flow_u = flow_v = None
    px2_m = px2 * min(east_m, north_m)
    widest_px = int(min(400, max(8, max((b.width for b in bodies if b.is_line), default=0.0) / px2_m)))

    def axis_raster(layer, reach_px: int) -> np.ndarray:
        raster = _mem_raster(size_f, size_f, gt_f, gdal.GDT_Float32, fill=np.nan)
        return _fill_nearest(_rasterize(raster, layer, from_z=True, all_touched=True).astype(np.float64), reach_px)

    def flow_rasters(layer, reach_px: int) -> tuple[np.ndarray, np.ndarray]:
        result = []
        for attribute in ("fu", "fv"):
            raster = _mem_raster(size_f, size_f, gt_f, gdal.GDT_Float32, fill=np.nan)
            burned = _rasterize(raster, layer, attribute=attribute, all_touched=True).astype(np.float64)
            result.append(_fill_nearest(burned, reach_px))
        return result[0], result[1]

    u = v = None
    if has_lines:
        # Le linee: quota e corrente dal loro asse, larghe quanto la linea.
        axis = axis_raster(layers.centrelines, widest_px)
        level_f = np.where(masks["river_buffers"] & np.isfinite(axis), axis, level_f)
        u, v = flow_rasters(layers.flow, widest_px)
    if has_area_axes:
        # Le aree: dai soli assi che le attraversano, in tutta la larghezza.
        # La prima versione si fermava a ~8 pixel dall'asse, e il resto del
        # Po prendeva la quota di ripiego dell'area. Il riempimento costa
        # quanto il suo raggio: si usa quello che serve davvero (la distanza
        # dei pixel dell'area dall'asse piu' vicino).
        burned = _rasterize(_mem_raster(size_f, size_f, gt_f, gdal.GDT_Float32, fill=np.nan),
                            layers.area_axes, from_z=True, all_touched=True)
        needed = float(_proximity_m(np.isfinite(burned), 1, 1.0)[masks["river_areas"]].max()) + 2.0
        reach_px = int(min(size_f, RIVER_AREA_FILL_M / px2_m, max(widest_px, needed)))
        area_axis = axis_raster(layers.area_axes, reach_px)
        level_f = np.where(masks["river_areas"] & np.isfinite(area_axis), area_axis, level_f)
        area_u, area_v = flow_rasters(layers.area_flow, reach_px)
        u = area_u if u is None else np.where(masks["river_areas"] & np.isfinite(area_u), area_u, u)
        v = area_v if v is None else np.where(masks["river_areas"] & np.isfinite(area_v), area_v, v)
    if u is not None:
        flow_u = np.where(river & np.isfinite(u), u, 0.0)
        flow_v = np.where(river & np.isfinite(v), v, 0.0)
    if layers.counts["lakes"]:
        lakes = _burn_attribute(layers, ("lakes",), "level", size_f, size_f, gt_f)
        level_f = np.where(masks["lakes"] & np.isfinite(lakes), lakes, level_f)

    wave_f = _burn_attribute(layers, ORDER, "wave", size_f, size_f, gt_f)
    wave_f = np.where(water & np.isfinite(wave_f), wave_f, 0.0)

    # --- La distanza dalla riva (SDF) ---------------------------------------------
    pixel2_x, pixel2_y = px2 * east_m, px2 * north_m
    pixel2_m = math.sqrt(pixel2_x * pixel2_y)
    inside = _proximity_m(water, 0, pixel2_m)
    outside = _proximity_m(water, 1, pixel2_m)
    half = 0.5 * pixel2_m
    signed = np.where(water, inside - half, -(outside - half))

    core = slice(2 * margin, 2 * margin + 2 * waterformat.RASTER)
    sdf_px = _downsample(signed[core, core]) / mpp
    water_core = water[core, core]

    # --- I post: superficie e fondale ----------------------------------------------
    # Il post i della tile sta sullo spigolo fra i pixel di F di colonna
    # 2*margin + 4i - 1 e 2*margin + 4i: i suoi valori sono la media dei quattro
    # pixel attorno. Si calcolano con 7 post di margine, perche' i riempimenti
    # attorno all'acqua siano gli stessi nelle due tile che condividono un bordo.
    extra = 7
    count = posts + 2 * extra
    centre = 2 * margin + 4 * (np.arange(count) - extra)
    inner = slice(extra, extra + posts)

    def at_posts(field: np.ndarray) -> np.ndarray:
        stack = np.stack([field[np.ix_(centre + dr, centre + dc)] for dr in (-1, 0) for dc in (-1, 0)])
        with np.errstate(invalid="ignore"):
            valid = np.isfinite(stack)
            totals = np.where(valid, stack, 0.0).sum(axis=0)
            numbers = valid.sum(axis=0)
            return np.where(numbers > 0, totals / np.maximum(numbers, 1), np.nan)

    # ------------------------------------------------------------------
    #  ACQUA "RISOLTA" E NO. Un'acqua larga almeno RESOLVE_POSTS post (a
    #  questo livello) si scava; una piu' stretta no: la mesh del terreno, a
    #  passo 2, non ha vertici dentro, e lo scavo diventerebbe un pettine di
    #  buchi (era il fiume "a vertici alti e bassi" della prima prova). Per
    #  quella il file non porta ne' superficie ne' fondale: la superficie la
    #  appoggia sul terreno il runtime (WaterMesh.h), e la forma la da' la
    #  distanza dalla riva del raster, come sempre.
    # ------------------------------------------------------------------
    resolved = water & (np.nan_to_num(_burn_attribute(layers, ORDER, "res", size_f, size_f, gt_f)) > 0.5)
    unresolved = water & ~resolved
    signed_r = (np.where(resolved, _proximity_m(resolved, 0, pixel2_m) - half,
                         -(_proximity_m(resolved, 1, pixel2_m) - half))
                if resolved.any() else np.full(water.shape, -1.0e9))
    to_unresolved = _proximity_m(unresolved, 1, pixel2_m) if unresolved.any() else np.full(water.shape, 1.0e9)

    # La superficie: dai pixel d'acqua risolta attorno al post, poi allargata
    # di SURFACE_FILL_POSTS post (le celle a cavallo della riva la vogliono in
    # tutti gli angoli). Non verso un'acqua NON risolta piu' vicina: li'
    # comanda il terreno.
    surface_wide = _fill_nearest(at_posts(np.where(resolved, level_f, np.nan)), SURFACE_FILL_POSTS)
    shore_wide = at_posts(signed_r)
    nearer_unresolved = (shore_wide <= 0.0) & (at_posts(to_unresolved) < -shore_wide)
    surface_wide = np.where(nearer_unresolved, np.nan, surface_wide)
    surface = surface_wide[inner, inner]
    shore = shore_wide[inner, inner]          # distanza con segno dalla riva risolta, in metri

    # ------------------------------------------------------------------
    #  IL FONDALE, E LA SPONDA. Il terreno della tile e' una mesh a passo 2:
    #  fra un vertice in acqua e uno a terra c'e' un triangolo, e l'acqua si
    #  vede solo dove il triangolo sta SOTTO la superficie. La prima versione
    #  scavava solo dentro (0,3 m + una rampa dolce): il vertice a terra,
    #  alto, tirava su il triangolo, e il terreno copriva l'acqua per meta'
    #  cella lungo tutte le rive (il 20% dell'acqua del Po, a Torino).
    #
    #  Ora la riva e' una "V" con la STESSA pendenza dai due lati
    #  (waterclasses.shore_slope): dentro il fondale scende almeno di
    #  pendenza x distanza (fino a SHORE_REACH_POSTS), fuori il terreno non sale
    #  piu' di cosi' (fino a SHORE_BAND_POSTS). Lungo un lato del triangolo il
    #  terreno e' allora lineare nella distanza dalla riva, e incrocia
    #  l'acqua proprio sulla riva. Fuori, il "fondale" e' un tetto: lo scavo
    #  prende il minimo fra terreno e fondale, e un terreno gia' basso resta
    #  com'e'.
    # ------------------------------------------------------------------
    post_m = post_deg * math.sqrt(east_m * north_m)
    vs_f = np.where(resolved, _burn_attribute(layers, ORDER, "vs", size_f, size_f, gt_f), np.nan)
    vs_f = _fill_nearest(vs_f, int(4 * (waterclasses.SHORE_BAND_POSTS + 2)))
    shore_slope = np.nan_to_num(at_posts(vs_f)[inner, inner], nan=0.3)

    # Lontano dalla riva (la rampa del mare arriva a 1,2 km) la distanza viene
    # dalla griglia P, che vede fin la'; vicino, da F, che e' piu' precisa.
    res_p = _burn_attribute(layers, ORDER, "res", size_p, size_p, gt_p)
    resolved_p = np.nan_to_num(res_p) > 0.5
    distance_p = _proximity_m(resolved_p, 0, post_m)
    d0 = _burn_attribute(layers, ORDER, "d0", size_p, size_p, gt_p, all_touched=True)
    slope = _burn_attribute(layers, ORDER, "slope", size_p, size_p, gt_p, all_touched=True)
    deep = _burn_attribute(layers, ORDER, "deep", size_p, size_p, gt_p, all_touched=True)
    kind = _burn_attribute(layers, ORDER, "kind", size_p, size_p, gt_p, all_touched=True)

    # Le regole del corpo d'acqua arrivano anche ai post della sponda, fuori
    # dal poligono: dal corpo piu' vicino.
    reach_p = int(waterclasses.SHORE_BAND_POSTS) + 3
    d0, slope, deep = (_fill_nearest(field, reach_p) for field in (d0, slope, deep))

    tile_rows = slice(margin_p, margin_p + posts)
    near_limit = 6.0 * post_m
    # Distanza dalla riva risolta, dentro (dalla griglia P se lontana) e fuori.
    distance = np.where(shore > 0.0,
                        np.where(shore < near_limit, shore, distance_p[tile_rows, tile_rows]),
                        -shore)
    shore_depth = np.nan_to_num(d0, nan=0.3)[tile_rows, tile_rows]
    ramp_slope = np.nan_to_num(slope, nan=0.1)[tile_rows, tile_rows]
    deepest = np.nan_to_num(deep, nan=1.0)[tile_rows, tile_rows]
    steep = shore_slope * np.minimum(distance, waterclasses.SHORE_REACH_POSTS * post_m)
    # La stessa funzione della distanza dentro (profondita') e fuori (altezza
    # massima della sponda): la "V" e' simmetrica anche quando il fondale
    # tocca il massimo del corpo (5 m per un fiume), e il terreno incrocia
    # l'acqua sulla riva. Fuori, un pendio ripido a ridosso dell'acqua
    # diventa per una cella una sponda bassa: e' il prezzo, e si vede meno
    # del terreno che copre l'acqua.
    #
    # E CONTINUA sulla riva: zero sulla riva, e i primi decimetri di
    # profondita' (shore_depth) arrivano con la pendenza della sponda, non
    # di colpo. Con il salto, un vertice che cade proprio sulla riva valeva
    # +0,3 m se "appena fuori" e -0,3 m se "appena dentro", e quei 60 cm
    # bastavano a far spuntare il terreno dall'acqua.
    rise = np.minimum(shore_depth, shore_slope * distance)
    depth = np.minimum(deepest, np.maximum(rise + ramp_slope * distance, steep))
    if bathymetry is not None:
        post_lons = bounds.west + np.arange(posts) * post_deg
        post_lats = bounds.north - np.arange(posts) * post_deg
        lon_grid, lat_grid = np.meshgrid(post_lons, post_lats)
        measured = bathymetry.depth(lon_grid, lat_grid)
        sea_posts = np.isin(kind[tile_rows, tile_rows], (waterclasses.KIND_SEA, waterclasses.KIND_LAGOON))
        ramp_free = shore_depth + ramp_slope * distance
        use = sea_posts & np.isfinite(measured)
        depth = np.where(use, np.maximum(np.maximum(shore_depth, np.minimum(ramp_free, measured)), steep), depth)

    # Fuori, il tetto della sponda serve solo ai vertici delle celle a
    # cavallo della riva: oltre SHORE_FLAT_POSTS sale ripido (45°), e un
    # argine alto resta un argine alto, abbassato solo a ridosso dell'acqua.
    outside_roof = depth + np.maximum(0.0, distance - waterclasses.SHORE_FLAT_POSTS * post_m)

    inside = (shore > 0.0) & np.isfinite(surface)
    band = (shore <= 0.0) & (shore > -waterclasses.SHORE_BAND_POSTS * post_m) & np.isfinite(surface)
    bed = np.full((posts, posts), np.nan)
    bed = np.where(inside, surface - depth, bed)
    bed = np.where(band, surface + outside_roof, bed)
    carve = np.isfinite(bed)

    if not water_core.any() and not carve.any():
        return None

    # --- La tile ---------------------------------------------------------------------
    flags = 0
    if masks["sea"][core, core].any():
        flags |= waterclasses.TILE_SEA
    if masks["lakes"][core, core].any():
        flags |= waterclasses.TILE_LAKE
    if river[core, core].any():
        flags |= waterclasses.TILE_RIVER
    if water_core.all():
        flags |= waterclasses.TILE_ALL_WATER

    tile_flow_u = tile_flow_v = None
    if flow_u is not None:
        fu = np.clip(np.round(_downsample(flow_u[core, core]) * FLOW_SCALE), -127, 127).astype(np.int8)
        fv = np.clip(np.round(_downsample(flow_v[core, core]) * FLOW_SCALE), -127, 127).astype(np.int8)
        if fu.any() or fv.any():
            tile_flow_u, tile_flow_v = fu, fv
            flags |= waterclasses.TILE_FLOW

    wave = np.clip(np.round(_downsample(wave_f[core, core]) * 255.0), 0, 255).astype(np.uint8)
    return waterformat.WaterTile(level, x, y, flags, surface.astype(np.float32), bed.astype(np.float32),
                                 waterformat.sdf_to_bytes(sdf_px), wave, tile_flow_u, tile_flow_v)


# --- I lavori in parallelo -----------------------------------------------------------

_worker: dict = {}


def _init_worker(blocks_dir: str, output: str, geoid: GeoidSampler, bathymetry_path: str | None):
    _worker.clear()
    _worker.update(blocks_dir=blocks_dir, output=output, geoid=geoid, block_cache=OrderedDict(),
                   bathymetry=BathymetrySampler(bathymetry_path) if bathymetry_path else None)


def _bodies_for(blocks: list[str]) -> list[Body]:
    cache = _worker["block_cache"]
    bodies: list[Body] = []
    for name in blocks:
        if name not in cache:
            path = os.path.join(_worker["blocks_dir"], name)
            cache[name] = read_block(path) if os.path.exists(path) else []
            if len(cache) > 8:
                cache.popitem(last=False)
        else:
            cache.move_to_end(name)
        bodies.extend(cache[name])
    return bodies


def _run_task(task: dict) -> dict:
    """Un gruppo di tile che leggono gli stessi blocchi. Ritorna le voci d'indice."""
    bodies = _bodies_for(task["blocks"])
    results: dict[str, list] = {}
    for level, x, y in task["tiles"]:
        tile = cut_tile(level, x, y, bodies, _worker["geoid"], _worker["bathymetry"])
        if tile is None:
            continue
        blob = waterformat.encode_tile(tile)
        waterformat.write_tile_atomic(os.path.join(_worker["output"],
                                                   waterformat.tile_relative_path(level, x, y)), blob)
        results.setdefault(str(level), []).append([x, y, tile.flags, len(blob)])
    return {"id": task["id"], "levels": results}


def _blocks_covering(level: int, x: int, y: int, available: set[str]) -> list[str]:
    bounds = tiling.tile_bounds(level, x, y)
    span = tiling.tile_span_deg(BLOCK_LEVEL)
    x0 = int((bounds.west + 180.0) // span)
    x1 = int((bounds.east + 180.0 - 1e-12) // span)
    y0 = int((90.0 - bounds.north) // span)
    y1 = int((90.0 - bounds.south - 1e-12) // span)
    names = [_block_name(bx, by) for by in range(y0, y1 + 1) for bx in range(x0, x1 + 1)]
    return [name for name in names if name in available]


def plan_tasks(terrain: TerrainSampler, levels: list[int], blocks: list[str], bbox) -> list[dict]:
    """Le tile di terreno da guardare, raggruppate per blocco."""
    available = set(blocks)
    grouped: dict[tuple, dict] = {}
    for level in levels:
        for x, y in sorted(terrain.tiles(level)):
            if bbox is not None:
                bounds = tiling.tile_bounds(level, x, y)
                if bounds.east < bbox[0] or bounds.west > bbox[2] or \
                        bounds.north < bbox[1] or bounds.south > bbox[3]:
                    continue
            covering = _blocks_covering(level, x, y, available)
            if not covering:
                continue
            key = tuple(covering)
            task = grouped.setdefault(key, {"blocks": list(covering), "tiles": []})
            task["tiles"].append((level, x, y))
    tasks = []
    for index, task in enumerate(sorted(grouped.values(), key=lambda t: -len(t["tiles"]))):
        task["id"] = f"{index:06d}_" + "-".join(name[:-4] for name in task["blocks"][:4])
        tasks.append(task)
    return tasks


# =============================================================================
#  Tutto insieme
# =============================================================================

def build(*, osm: list[str], terrain_root: str, output: str, sea: str | None = None,
          name: str = "acqua", bbox=None, bathymetry: str | None = None,
          min_level: int | None = None, max_level: int | None = None, jobs: int = 1,
          geoid: GeoidSampler | None = None, work: str | None = None,
          report: Report = _null_report) -> dict:
    started = time.time()
    terrain = TerrainSampler(terrain_root)
    levels = [level for level in terrain.levels
              if (min_level is None or level >= min_level) and (max_level is None or level <= max_level)]
    if not levels:
        raise ValueError(f"nessun livello del terreno fra {min_level} e {max_level} "
                         f"(il terreno ha {terrain.levels})")
    region = bbox if bbox is not None else terrain.bbox

    work = work or os.path.join(output, "_work")
    os.makedirs(work, exist_ok=True)
    state = PipelineState(os.path.join(work, "state.json"))

    # --- il geoide -----------------------------------------------------------------
    if geoid is None:
        from . import geoid as geoid_module
        report(f"[0/3] geoide ({terrain.vertical_crs}) sull'area")
        # Su tutta l'area del terreno, non solo su quella dell'acqua: serve
        # anche a riconoscere i post riempiti col livello del mare lungo i
        # fiumi che escono dal riquadro (TerrainSampler.heights).
        geoid_region = (min(region[0], terrain.bbox[0]), min(region[1], terrain.bbox[1]),
                        max(region[2], terrain.bbox[2]), max(region[3], terrain.bbox[3]))
        grid, geotransform, _error, _spacing = geoid_module.build_aligned_undulation_grid(
            geoid_region, terrain.vertical_crs, max_error_m=0.01, log=report)
        geoid = GeoidSampler(grid, geotransform)
    terrain.sea_level = geoid

    # --- 1. estrai --------------------------------------------------------------
    inputs = list(osm) + ([sea] if sea else [])
    extract_fp = PipelineState.fingerprint(
        {"stage": "estrai", "rules": waterclasses.WATER_RULES_VERSION, "bbox": list(bbox) if bbox else None,
         "terrain": os.path.abspath(terrain_root), "blockLevel": BLOCK_LEVEL}, inputs)
    blocks_dir = os.path.join(work, "blocchi")
    summary_path = os.path.join(work, "estratto.json")
    summary = None
    report(f"[1/3] estrazione dell'acqua da {len(inputs)} file")
    if state.is_complete("estrai", extract_fp) and os.path.exists(summary_path):
        with open(summary_path, encoding="utf-8") as handle:
            summary = json.load(handle)
        if not all(os.path.exists(os.path.join(blocks_dir, b)) for b in summary["blocchi"]):
            summary = None
    if summary is not None:
        report(f"      gia' fatta: {sum(summary['corpi'].values()):,} corpi in {len(summary['blocchi'])} blocchi")
    else:
        summary = extract_bodies(list(osm), sea, terrain, bbox, blocks_dir, report)
        with open(summary_path, "w", encoding="utf-8") as handle:
            json.dump(summary, handle, indent=1)
        state.mark_complete("estrai", extract_fp, [summary_path], {})
        for kind, number in sorted(summary["corpi"].items()):
            report(f"        {kind:8s} {number:8,d}")
        for reason, number in summary["scartati"].items():
            if number:
                report(f"        scartati ({reason}): {number:,}")

    # --- 2. taglia --------------------------------------------------------------
    cut_fp = PipelineState.fingerprint(
        {"stage": "taglia", "extract": extract_fp, "levels": levels,
         "format": waterformat.FORMAT_VERSION, "bathymetry": bathymetry,
         "margin": RASTER_MARGIN}, [bathymetry] if bathymetry else None)
    done_dir = os.path.join(work, "fatti", cut_fp[:16])
    os.makedirs(done_dir, exist_ok=True)
    tasks = plan_tasks(terrain, levels, summary["blocchi"], bbox)
    todo = [task for task in tasks if not os.path.exists(os.path.join(done_dir, task["id"] + ".json"))]
    tile_total = sum(len(task["tiles"]) for task in tasks)
    report(f"[2/3] taglio: {tile_total:,} tile di terreno vicine all'acqua in {len(tasks)} gruppi "
           f"({len(tasks) - len(todo)} gia' fatti), livelli {levels[0]}..{levels[-1]}, {jobs} processi")

    def save(result: dict) -> None:
        temporary = os.path.join(done_dir, result["id"] + ".json.tmp")
        with open(temporary, "w", encoding="utf-8") as handle:
            json.dump(result, handle)
        os.replace(temporary, os.path.join(done_dir, result["id"] + ".json"))

    cut_started = time.time()
    finished = 0
    if jobs <= 1 or len(todo) <= 1:
        _init_worker(blocks_dir, output, geoid, bathymetry)
        for task in todo:
            save(_run_task(task))
            finished += 1
            if finished % 20 == 0 or finished == len(todo):
                report(f"      {finished}/{len(todo)} gruppi ({time.time() - cut_started:.0f} s)")
    else:
        with ProcessPoolExecutor(max_workers=jobs, initializer=_init_worker,
                                 initargs=(blocks_dir, output, geoid, bathymetry)) as pool:
            for result in pool.map(_run_task, todo):
                save(result)
                finished += 1
                if finished % 20 == 0 or finished == len(todo):
                    report(f"      {finished}/{len(todo)} gruppi ({time.time() - cut_started:.0f} s)")

    # --- 3. indici, manifest, geoide ------------------------------------------------
    report("[3/3] indici, manifest, geoide")
    per_level: dict[int, list[waterformat.WaterTileEntry]] = {level: [] for level in levels}
    for task in tasks:
        with open(os.path.join(done_dir, task["id"] + ".json"), encoding="utf-8") as handle:
            result = json.load(handle)
        for level, items in result["levels"].items():
            per_level[int(level)].extend(waterformat.WaterTileEntry(*item) for item in items)

    level_info = []
    for level in levels:
        entries = per_level[level]
        waterformat.write_level_index(output, level, entries)
        level_info.append({"level": level, "tile_count": len(entries),
                           "bytes": sum(e.size for e in entries)})
        report(f"      livello {level:2d}: {len(entries):7,d} tile, "
               f"{level_info[-1]['bytes'] / 1e6:8.1f} MB")

    if geoid.grid is not None:
        waterformat.write_geoid(output, geoid.grid, geoid.geotransform)
    else:
        west, south, east, north = region
        constant = np.full((3, 3), geoid.constant, dtype=np.float32)
        spacing = max(east - west, north - south)
        waterformat.write_geoid(output, constant, (west - spacing * 0.5, north + spacing * 0.5, spacing))

    extent = summary.get("estensione") or list(region)
    manifest = waterformat.build_manifest(
        dataset_name=name, bbox=tuple(extent), levels=level_info,
        source={"osm": [os.path.basename(path) for path in osm],
                "sea": os.path.basename(sea) if sea else None,
                "bathymetry": os.path.basename(bathymetry) if bathymetry else None,
                "terrain": os.path.abspath(terrain_root),
                "verticalCrs": terrain.vertical_crs,
                "description": "OpenStreetMap (c) contributori OpenStreetMap, licenza ODbL",
                "bbox": list(bbox) if bbox else None},
        body_counts=summary["corpi"], pipeline_version=PIPELINE_VERSION)
    waterformat.write_manifest(output, manifest)
    report(f"fatto in {time.time() - started:.0f} s: {output}")
    return manifest


# =============================================================================
#  Verifica
# =============================================================================

def verify(root: str, report: Report = _null_report, sample_per_level: int = 16) -> int:
    """Rilegge un dataset d'acqua e ne controlla la coerenza. Ritorna i problemi."""
    document = waterformat.read_manifest(root)
    if document.get("datasetKind") != "water":
        report("ERRORE: il manifest non dichiara un dataset d'acqua")
        return 1
    problems = 0
    report(f"dataset  : {document['datasetName']}")
    rules = document.get("rulesVersion", 0)
    if rules < waterclasses.WATER_RULES_VERSION:
        # Lo stesso controllo del motore (WaterRulesVersion): un dataset fatto
        # con regole vecchie e' leggibile e coerente, ma i suoi numeri no.
        report(f"regole   : {rules}, VECCHIE (adesso sono le {waterclasses.WATER_RULES_VERSION}): "
               f"rifai build-water su questa cartella")
        problems += 1
    else:
        report(f"regole   : {rules}")
    report(f"corpi    : " + ", ".join(f"{k} {v:,}" for k, v in sorted(document.get("bodies", {}).items())))
    try:
        grid, _ = waterformat.read_geoid(root)
        report(f"geoide   : {grid.shape[1]} x {grid.shape[0]}, N da {grid.min():.2f} a {grid.max():.2f} m")
    except Exception as error:                          # noqa: BLE001
        report(f"geoide   : ILLEGGIBILE ({error})")
        problems += 1

    for info in document["levels"]:
        level = info["level"]
        entries = waterformat.read_level_index(root, level)
        if len(entries) != info["tile_count"]:
            report(f"  livello {level}: indice con {len(entries)} voci, manifest {info['tile_count']}")
            problems += 1
            continue
        bad = 0
        sample = entries[:: max(1, len(entries) // sample_per_level)][:sample_per_level]
        for entry in sample:
            path = os.path.join(root, waterformat.tile_relative_path(level, entry.x, entry.y))
            try:
                tile = waterformat.read_tile(path)
            except Exception as error:                  # noqa: BLE001
                report(f"  livello {level}: {entry.x},{entry.y} illeggibile: {error}")
                bad += 1
                continue
            if (tile.level, tile.x, tile.y) != (level, entry.x, entry.y) or tile.flags != entry.flags:
                report(f"  livello {level}: {entry.x},{entry.y} non corrisponde all'indice")
                bad += 1
            # Dentro l'acqua il fondale sta sotto la superficie; fuori (la
            # sponda) e' un tetto per il terreno e puo' stare sopra.
            sdf = waterformat.bytes_to_sdf(tile.sdf).astype(np.float64)
            padded = np.pad(sdf, 1, mode="edge")
            at_post = 0.25 * (padded[0:257:2, 0:257:2] + padded[0:257:2, 1:258:2]
                              + padded[1:258:2, 0:257:2] + padded[1:258:2, 1:258:2])
            both = np.isfinite(tile.surface) & np.isfinite(tile.bed) & (at_post > 1.0)
            if both.any() and (tile.bed[both] > tile.surface[both] + 1e-3).any():
                report(f"  livello {level}: {entry.x},{entry.y} ha il fondale sopra la superficie in acqua")
                bad += 1
            if np.isfinite(tile.bed).any() and not np.isfinite(tile.surface).any():
                report(f"  livello {level}: {entry.x},{entry.y} scava senza superficie")
                bad += 1
        report(f"  livello {level:2d}: {len(entries):7,d} tile   {'ok' if bad == 0 else f'FALLITO ({bad})'}")
        problems += bad
    report("")
    report("TUTTO A POSTO" if problems == 0 else f"{problems} PROBLEMI")
    return problems

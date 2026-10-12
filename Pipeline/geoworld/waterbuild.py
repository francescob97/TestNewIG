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

    def heights(self, lons, lats) -> np.ndarray:
        """Quote ellissoidiche, NaN dove il terreno non c'e'."""
        lons = np.asarray(lons, dtype=np.float64).ravel()
        lats = np.asarray(lats, dtype=np.float64).ravel()
        result = np.full(lons.shape, np.nan)
        remaining = np.ones(lons.shape, dtype=bool)
        cells = tiling.TILE_CELLS
        for level in reversed(self.levels):
            if not remaining.any():
                break
            span = tiling.tile_span_deg(level)
            tx = np.floor((lons + 180.0) / span).astype(np.int64)
            ty = np.floor((90.0 - lats) / span).astype(np.int64)
            for x, y in set(zip(tx[remaining].tolist(), ty[remaining].tolist())):
                heights = self._tile(level, x, y)
                if heights is None:
                    continue
                pick = remaining & (tx == x) & (ty == y)
                west = -180.0 + x * span
                north = 90.0 - y * span
                fx = np.clip((lons[pick] - west) / span * cells, 0.0, cells)
                fy = np.clip((north - lats[pick]) / span * cells, 0.0, cells)
                i0 = np.minimum(np.floor(fx).astype(np.intp), cells - 1)
                j0 = np.minimum(np.floor(fy).astype(np.intp), cells - 1)
                ax, ay = fx - i0, fy - j0
                h = (heights[j0, i0] * (1 - ax) * (1 - ay) + heights[j0, i0 + 1] * ax * (1 - ay)
                     + heights[j0 + 1, i0] * (1 - ax) * ay + heights[j0 + 1, i0 + 1] * ax * ay)
                result[pick] = h
                remaining &= ~pick
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

def _gdal():
    from osgeo import gdal, ogr, osr
    gdal.UseExceptions()
    ogr.UseExceptions()
    osr.UseExceptions()
    return gdal, ogr, osr


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
    """Punti regolari dentro un poligono (per la quota di un lago)."""
    gdal, _, _ = _gdal()
    west, east, south, north = geometry.GetEnvelope()
    side = max(east - west, north - south, 1e-9)
    count = int(max(8, min(math.sqrt(max_points), 256)))
    step = side / count
    width = max(1, int(math.ceil((east - west) / step)))
    height = max(1, int(math.ceil((north - south) / step)))
    raster = _mem_raster(width, height, (west, step, 0, north, 0, -step), gdal.GDT_Byte)
    datasource = _memory_vector()
    layer = _layer(datasource, "lago", ())
    _add(layer, geometry)
    mask = _rasterize(raster, layer, burn=1)
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


def river_profile(points: np.ndarray, terrain: TerrainSampler) -> tuple[np.ndarray, np.ndarray] | None:
    """
    L'asse di un fiume ogni RIVER_SAMPLE_M metri, con la quota dell'acqua:
    il terreno sull'asse, con il MINIMO PROGRESSIVO nel verso della linea.

    Le way dei fiumi in OSM sono orientate a valle. Dove il DEM risale (un
    ponte, una briglia, il rumore) l'acqua non risale con lui.
    """
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
    dense = np.column_stack([dense_lon, dense_lat])
    heights = terrain.heights(dense[:, 0], dense[:, 1])
    valid = np.isfinite(heights)
    if not valid.any():
        return None
    positions = np.arange(len(heights))
    heights = np.interp(positions, positions[valid], heights[valid])
    return dense, np.minimum.accumulate(heights)


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
                if kind in (waterclasses.KIND_LAKE, waterclasses.KIND_RIVER, waterclasses.KIND_CANAL):
                    # I fiumi larghi prendono la quota dall'asse, dove c'e'; il
                    # percentile e' il ripiego.
                    level = lake_level(geometry, terrain)
                    if not math.isfinite(level):
                        skipped["senza terreno"] += 1
                        continue
                width = math.sqrt(area) * 1000.0
                store(Body(kind, False, level, waterclasses.wave_exposure(kind, area),
                           waterclasses.max_depth(kind, area, width), width, area, geometry))
            else:
                parts = _linestring_from_wkb(bytes(geometry.ExportToWkb(ogr.wkbNDR)))
                for part in parts:
                    if len(part) < 2:
                        continue
                    profile = river_profile(part, terrain)
                    if profile is None:
                        skipped["senza terreno"] += 1
                        continue
                    dense, levels = profile
                    line = ogr.Geometry(ogr.wkbLineString25D)
                    for (lon, lat), z in zip(dense, levels):
                        line.AddPoint(float(lon), float(lat), float(z))
                    kind = classified.kind
                    store(Body(kind, True, float("nan"), waterclasses.wave_exposure(kind),
                               waterclasses.max_depth(kind, 0.0, classified.width_m),
                               classified.width_m, 0.0, line))

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


def _box(west, south, east, north):
    _, ogr, _ = _gdal()
    return ogr.CreateGeometryFromWkt(
        f"POLYGON(({west} {south},{east} {south},{east} {north},{west} {north},{west} {south}))")


def _prepare_layers(bodies: list[Body], roi, tile_centre, metres_per_px: float,
                    simplify_deg: float) -> _Layers:
    """Ritaglia, bufferizza e mette in layer di memoria i corpi vicini alla tile."""
    _, ogr, _ = _gdal()
    west, south, east, north = roi
    clip = _box(west, south, east, north)
    datasource = _memory_vector()
    fields = ("level", "wave", "d0", "slope", "deep", "kind")
    layers = {name: _layer(datasource, name, fields)
              for name in ("sea", "river_areas", "river_buffers", "lakes")}
    centrelines = _layer(datasource, "centrelines", ())
    flow = _layer(datasource, "flow", ("fu", "fv"))
    forward, backward = _metric_transforms(*tile_centre)
    counts = {name: 0 for name in ("sea", "river_areas", "river_buffers", "lakes")}

    for body in bodies:
        bw, be, bs, bn = body.envelope
        reach = 0.0 if not body.is_line else body.width / 111000.0 + 1e-6
        if be + reach < west or bw - reach > east or bn + reach < south or bs - reach > north:
            continue
        # Ai livelli bassi le cose piccole spariscono: un laghetto sotto i 4
        # pixel, un fiume piu' stretto di mezzo pixel.
        if body.kind == waterclasses.KIND_LAKE and body.area_km2 * 1.0e6 < 4.0 * metres_per_px ** 2:
            continue
        if body.is_line and body.width < 0.5 * metres_per_px:
            continue

        shore, slope = waterclasses.ramp(body.kind)
        values = {"level": body.level if math.isfinite(body.level) else 0.0, "wave": body.wave,
                  "d0": shore, "slope": slope, "deep": body.deepest, "kind": body.kind}

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
        points = body.geometry.GetPoints()
        speed = waterclasses.FLOW_SPEED.get(body.kind, 0.0)
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
            _add(flow, segment, fu=speed * dx / length, fv=-speed * dy / length)

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
                   layers["lakes"], datasource, counts)


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
    return result


def _downsample(values: np.ndarray, factor: int = 2) -> np.ndarray:
    height, width = values.shape
    return values.reshape(height // factor, factor, width // factor, factor).mean(axis=(1, 3))


#: L'ordine in cui i corpi si coprono: il mare sotto, i laghi sopra (un fiume
#: che entra in un lago prende la quota del lago dentro il lago).
ORDER = ("sea", "river_areas", "river_buffers", "lakes")


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
    layers = _prepare_layers(bodies, roi, (0.5 * (bounds.west + bounds.east), centre_lat), mpp,
                             simplify_deg=px2 * 0.5 if level < 12 else 0.0)
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
    flow_u = flow_v = None
    if has_lines:
        raster = _mem_raster(size_f, size_f, gt_f, gdal.GDT_Float32, fill=np.nan)
        axis = _rasterize(raster, layers.centrelines, from_z=True, all_touched=True).astype(np.float64)
        widest_px = int(min(400, max(8, max((b.width for b in bodies if b.is_line), default=0.0)
                                     / (px2 * min(east_m, north_m)))))
        axis = _fill_nearest(axis, widest_px)
        pick = (masks["river_areas"] | masks["river_buffers"]) & np.isfinite(axis)
        level_f = np.where(pick, axis, level_f)

        raster_u = _mem_raster(size_f, size_f, gt_f, gdal.GDT_Float32, fill=np.nan)
        raster_v = _mem_raster(size_f, size_f, gt_f, gdal.GDT_Float32, fill=np.nan)
        u = _fill_nearest(_rasterize(raster_u, layers.flow, attribute="fu", all_touched=True).astype(np.float64),
                          widest_px)
        v = _fill_nearest(_rasterize(raster_v, layers.flow, attribute="fv", all_touched=True).astype(np.float64),
                          widest_px)
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
    # 2*margin + 4i - 1 e 2*margin + 4i: la sua superficie e' la media dei
    # quattro pixel attorno (quelli d'acqua). Si calcola con 7 post di margine,
    # perche' il riempimento dei post vicini all'acqua sia lo stesso nelle due
    # tile che condividono un bordo.
    extra = 7
    count = posts + 2 * extra
    centre = 2 * margin + 4 * (np.arange(count) - extra)
    stack = np.stack([level_f[np.ix_(centre + dr, centre + dc)] for dr in (-1, 0) for dc in (-1, 0)])
    with np.errstate(invalid="ignore"):
        valid = np.isfinite(stack)
        totals = np.where(valid, stack, 0.0).sum(axis=0)
        numbers = valid.sum(axis=0)
        surface_wide = np.where(numbers > 0, totals / np.maximum(numbers, 1), np.nan)
    surface_wide = _fill_nearest(surface_wide, 3)
    surface = surface_wide[extra: extra + posts, extra: extra + posts]

    # Il fondale: la distanza dalla riva sui post (griglia P, con il suo
    # margine largo) e le regole del corpo a cui il post appartiene.
    masks_p = _burn_masks(layers, size_p, size_p, gt_p, all_touched=True)
    water_p = masks_p["sea"] | masks_p["river_areas"] | masks_p["river_buffers"] | masks_p["lakes"]
    distance_p = _proximity_m(water_p, 0, post_deg * math.sqrt(east_m * north_m))
    d0 = _burn_attribute(layers, ORDER, "d0", size_p, size_p, gt_p, all_touched=True)
    slope = _burn_attribute(layers, ORDER, "slope", size_p, size_p, gt_p, all_touched=True)
    deep = _burn_attribute(layers, ORDER, "deep", size_p, size_p, gt_p, all_touched=True)
    kind = _burn_attribute(layers, ORDER, "kind", size_p, size_p, gt_p, all_touched=True)

    tile_rows = slice(margin_p, margin_p + posts)
    depth = waterclasses.depth_at(distance_p, np.nan_to_num(d0, nan=0.3), np.nan_to_num(slope, nan=0.1),
                                  np.nan_to_num(deep, nan=1.0))[tile_rows, tile_rows]
    if bathymetry is not None:
        post_lons = bounds.west + np.arange(posts) * post_deg
        post_lats = bounds.north - np.arange(posts) * post_deg
        lon_grid, lat_grid = np.meshgrid(post_lons, post_lats)
        measured = bathymetry.depth(lon_grid, lat_grid)
        sea_posts = np.isin(kind[tile_rows, tile_rows], (waterclasses.KIND_SEA, waterclasses.KIND_LAGOON))
        shore = np.nan_to_num(d0, nan=0.5)[tile_rows, tile_rows]
        ramp_free = shore + np.nan_to_num(slope, nan=0.05)[tile_rows, tile_rows] \
            * distance_p[tile_rows, tile_rows]
        use = sea_posts & np.isfinite(measured)
        depth = np.where(use, np.maximum(shore, np.minimum(ramp_free, measured)), depth)

    carve = water_p[tile_rows, tile_rows] & np.isfinite(surface)
    bed = np.where(carve, surface - depth, np.nan)

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
        grid, geotransform, _error, _spacing = geoid_module.build_aligned_undulation_grid(
            tuple(region), terrain.vertical_crs, max_error_m=0.01, log=report)
        geoid = GeoidSampler(grid, geotransform)

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
            both = np.isfinite(tile.surface) & np.isfinite(tile.bed)
            if both.any() and (tile.bed[both] > tile.surface[both] + 1e-3).any():
                report(f"  livello {level}: {entry.x},{entry.y} ha il fondale sopra la superficie")
                bad += 1
            if np.isfinite(tile.bed).any() and not np.isfinite(tile.surface).any():
                report(f"  livello {level}: {entry.x},{entry.y} scava senza superficie")
                bad += 1
        report(f"  livello {level:2d}: {len(entries):7,d} tile   {'ok' if bad == 0 else f'FALLITO ({bad})'}")
        problems += bad
    report("")
    report("TUTTO A POSTO" if problems == 0 else f"{problems} PROBLEMI")
    return problems

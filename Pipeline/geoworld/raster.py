"""
Stadi raster della pipeline: riproiezione, piramide, ritaglio dei tile.

Tutto e' a BLOCCHI. Non e' pedanteria: l'Italia al livello 14 e' un raster di
circa 140.000 x 136.000 pixel, cioe' 76 GB in float32. Caricarlo in memoria non
e' un'opzione, quindi ogni stadio legge finestre, elabora e scrive.
"""

from __future__ import annotations

import math
import os
from dataclasses import dataclass

import numpy as np

from . import tiling, tileformat, geoid
from .manifest import TileEntry

# Opzioni di creazione dei GeoTIFF intermedi.
#  TILED        : accesso a finestre senza rileggere righe intere;
#  DEFLATE+P3   : i blocchi interamente NaN (mare, fuori Italia) si comprimono
#                 quasi a zero, ed e' la maggior parte del bbox italiano;
#  PREDICTOR=3  : predittore per virgola mobile, tipicamente 30-40% in meno;
#  BIGTIFF      : oltre i 4 GB il formato TIFF classico non basta.
GTIFF_OPTIONS = ["TILED=YES", "BLOCKXSIZE=256", "BLOCKYSIZE=256",
                 "COMPRESS=DEFLATE", "PREDICTOR=3", "ZLEVEL=6", "BIGTIFF=IF_SAFER"]

#: Memoria indicativa per blocco di elaborazione. Regola quanti pixel si
#: tengono in RAM contemporaneamente negli stadi che scorrono i raster.
BLOCK_MEMORY_BUDGET_BYTES = 384 * 1024 * 1024


@dataclass(frozen=True)
class LevelGrid:
    """
    Geometria del raster di un livello, ancorata alla griglia globale dei post.

    E' l'oggetto che rende tutto il resto esatto: il raster non ha un'origine
    arbitraria, ma comincia su un post della griglia del livello. Cosi' ritagliare
    una tile e' una lettura di finestra a indici interi, senza ricampionamento e
    senza offset di mezzo pixel.
    """
    level: int
    gx0: int      # indice globale del primo post (ovest)
    gy0: int      # indice globale del primo post (nord)
    width: int    # numero di post in longitudine
    height: int   # numero di post in latitudine

    @property
    def spacing(self) -> float:
        return tiling.post_spacing_deg(self.level)

    @property
    def geotransform(self) -> tuple[float, float, float, float, float, float]:
        """
        GDAL usa la convenzione "pixel-is-area": il geotransform descrive
        l'ANGOLO in alto a sinistra del primo pixel. I nostri post sono PUNTI e
        devono cadere al CENTRO dei pixel, quindi l'origine arretra di mezzo
        pixel. Sbagliare questo mezzo pixel e' l'errore piu' comune e piu'
        difficile da vedere: il terreno risulta traslato di meta' passo.
        """
        spacing = self.spacing
        origin_lon = tiling.LON_MIN + (self.gx0 - 0.5) * spacing
        origin_lat = tiling.LAT_MAX - (self.gy0 - 0.5) * spacing
        return (origin_lon, spacing, 0.0, origin_lat, 0.0, -spacing)

    @property
    def output_bounds(self) -> tuple[float, float, float, float]:
        """(minX, minY, maxX, maxY) per gdal.Warp."""
        origin_lon, spacing, _, origin_lat, _, _ = self.geotransform
        return (origin_lon, origin_lat - self.height * spacing,
                origin_lon + self.width * spacing, origin_lat)

    def post_lons(self, col0: int, count: int) -> np.ndarray:
        gx = self.gx0 + col0 + np.arange(count, dtype=np.float64)
        return tiling.LON_MIN + gx * self.spacing

    def post_lats(self, row0: int, count: int) -> np.ndarray:
        gy = self.gy0 + row0 + np.arange(count, dtype=np.float64)
        return tiling.LAT_MAX - gy * self.spacing


def level_grid_for_bbox(level: int, bbox: tuple[float, float, float, float],
                        margin_posts: int = 2) -> LevelGrid:
    """
    Raster di livello allineato ai TILE, non al bbox.

    La differenza conta: le tile che toccano il bordo del bbox sporgono oltre di
    esso, perche' una tile e' una cella della griglia globale e non si adatta ai
    dati. Dimensionando il raster sul bbox, i post di quelle tile che cadono
    fuori risulterebbero mancanti e verrebbero riempiti come se fossero mare,
    anche dove il dato sorgente c'e'. Partendo dall'intervallo di tile invece,
    ogni tile e' interamente dentro il raster per costruzione.

    Il margine aggiuntivo serve al filtro della piramide, che ha bisogno di due
    post oltre il bordo.
    """
    west, south, east, north = bbox
    x0, y0, x1, y1 = tiling.tile_range_for_bbox(level, west, south, east, north)

    cells = tiling.TILE_CELLS
    gx0 = x0 * cells - margin_posts
    gx1 = x1 * cells + cells + margin_posts
    gy0 = y0 * cells - margin_posts
    gy1 = y1 * cells + cells + margin_posts

    gx0 = max(gx0, 0)
    gy0 = max(gy0, 0)
    gx1 = min(gx1, tiling.global_posts_x(level) - 1)
    gy1 = min(gy1, tiling.global_posts_y(level) - 1)

    return LevelGrid(level=level, gx0=gx0, gy0=gy0,
                     width=gx1 - gx0 + 1, height=gy1 - gy0 + 1)


# ---------------------------------------------------------------------------
#  Stadio 1: inventario e mosaico virtuale
# ---------------------------------------------------------------------------

def build_source_vrt(inputs: list[str], vrt_path: str,
                     source_crs: str | None = None) -> dict:
    """
    Mette tutti i GeoTIFF sorgente dietro un unico raster virtuale.

    PERCHE' UN VRT E NON UN MOSAICO VERO: un VRT e' un file XML di pochi
    kilobyte che fa apparire N file come un raster solo. Mosaicare fisicamente
    TINITALY produrrebbe un intermedio da diversi gigabyte, identico ai dati di
    partenza, che verrebbe letto una volta sola e poi buttato. Il VRT costa
    niente, si rigenera in un istante ed e' ispezionabile con gdalinfo.
    """
    from osgeo import gdal
    gdal.UseExceptions()

    os.makedirs(os.path.dirname(os.path.abspath(vrt_path)), exist_ok=True)

    vrt = gdal.BuildVRT(vrt_path, inputs, options=gdal.BuildVRTOptions(
        resampleAlg="nearest",
        # I file TINITALY si toccano senza sovrapporsi; se si sovrapponessero,
        # l'ultimo vincerebbe. Nessun ricampionamento in questa fase.
        addAlpha=False))
    if vrt is None:
        raise RuntimeError(f"gdal.BuildVRT ha fallito su {len(inputs)} input")

    projection = vrt.GetProjection()

    # ------------------------------------------------------------------
    #  Sorgenti senza proiezione dichiarata.
    #
    #  Capita davvero: i grid ESRI ASCII (.asc) non portano il CRS dentro il
    #  file, sta in un .prj a fianco che spesso manca. Senza CRS la
    #  riproiezione non puo' partire, e GDAL non se ne lamenta finche' non e'
    #  troppo tardi. Meglio fermarsi qui dicendo cosa fare.
    # ------------------------------------------------------------------
    if not projection:
        if not source_crs:
            vrt = None
            raise RuntimeError(
                "I file sorgente non dichiarano nessun sistema di riferimento.\n"
                "Succede tipicamente con i grid ESRI ASCII (.asc) privi del file .prj.\n"
                "Indica il CRS esplicitamente, per esempio:\n"
                "    --source-crs EPSG:32632      (TINITALY: UTM 32N WGS84)")

        from osgeo import osr
        srs = osr.SpatialReference()
        srs.SetFromUserInput(source_crs)
        srs.SetAxisMappingStrategy(osr.OAMS_TRADITIONAL_GIS_ORDER)
        projection = srs.ExportToWkt()
        vrt.SetProjection(projection)
        vrt.FlushCache()

    geotransform = vrt.GetGeoTransform()
    band = vrt.GetRasterBand(1)

    # Il driver del primo file: serve a segnalare i formati lenti da leggere.
    first = gdal.Open(inputs[0])
    driver_name = first.GetDriver().ShortName
    first = None

    info = {
        "driver": driver_name,
        "fileCount": len(inputs),
        "width": vrt.RasterXSize,
        "height": vrt.RasterYSize,
        "crs": projection,
        "pixelSizeX": abs(geotransform[1]),
        "pixelSizeY": abs(geotransform[5]),
        "nodata": band.GetNoDataValue(),
        "dataType": gdal.GetDataTypeName(band.DataType),
        "boundsProjected": (
            geotransform[0],
            geotransform[3] + vrt.RasterYSize * geotransform[5],
            geotransform[0] + vrt.RasterXSize * geotransform[1],
            geotransform[3]),
    }
    vrt = None
    return info


def source_bounds_wgs84(vrt_path: str) -> tuple[float, float, float, float]:
    """Bbox del sorgente in gradi, ottenuto proiettando gli angoli e i bordi."""
    from osgeo import gdal, osr
    gdal.UseExceptions()

    dataset = gdal.Open(vrt_path)
    geotransform = dataset.GetGeoTransform()

    source_srs = osr.SpatialReference(wkt=dataset.GetProjection())
    target_srs = osr.SpatialReference()
    target_srs.ImportFromEPSG(4326)
    # Senza questo, l'ordine degli assi di EPSG:4326 e' (lat, lon) come da
    # autorita' EPSG, e ci si ritrova con longitudini di 45 gradi in Italia.
    target_srs.SetAxisMappingStrategy(osr.OAMS_TRADITIONAL_GIS_ORDER)
    source_srs.SetAxisMappingStrategy(osr.OAMS_TRADITIONAL_GIS_ORDER)
    transform = osr.CoordinateTransformation(source_srs, target_srs)

    # Si campionano anche i punti lungo i bordi, non solo i quattro angoli: in
    # una proiezione UTM i bordi di un rettangolo sono curvi una volta proiettati
    # in geografiche, e usando solo gli angoli si perde una striscia.
    width, height = dataset.RasterXSize, dataset.RasterYSize
    steps = np.linspace(0.0, 1.0, 25)
    pixels = ([(s * width, 0) for s in steps] + [(s * width, height) for s in steps] +
              [(0, s * height) for s in steps] + [(width, s * height) for s in steps])

    lons, lats = [], []
    for px, py in pixels:
        x = geotransform[0] + px * geotransform[1] + py * geotransform[2]
        y = geotransform[3] + px * geotransform[4] + py * geotransform[5]
        lon, lat, _ = transform.TransformPoint(x, y)
        lons.append(lon)
        lats.append(lat)

    dataset = None
    return (min(lons), min(lats), max(lons), max(lats))


def source_ground_resolution(vrt_path: str) -> dict:
    """
    Risoluzione del sorgente in METRI SUL TERRENO, qualunque sia il suo CRS.

    PERCHE' NON BASTA IL GEOTRANSFORM: la dimensione del pixel che GDAL riporta
    e' nelle unita' del sistema di riferimento del file. Per TINITALY (UTM 32N)
    sono metri e si puo' usare direttamente; per il Copernicus DEM (EPSG:4326)
    sono GRADI, e 0.000277 gradi valgono ~31 metri. Usare il numero grezzo fa
    credere al codice di avere un dato mille volte piu' fine del vero, con la
    conseguenza di chiedere un livello di piramide assurdo.

    Qui si misura invece la distanza vera fra il centro di un pixel e quello dei
    suoi vicini, portando entrambi in coordinate geografiche. Funziona per
    qualunque CRS sorgente, comprese le proiezioni con rotazione, e non richiede
    di sapere in che unita' sia il file.
    """
    from osgeo import gdal, osr
    gdal.UseExceptions()

    dataset = gdal.Open(vrt_path)
    geotransform = dataset.GetGeoTransform()

    source_srs = osr.SpatialReference(wkt=dataset.GetProjection())
    target_srs = osr.SpatialReference()
    target_srs.ImportFromEPSG(4326)
    for srs in (source_srs, target_srs):
        srs.SetAxisMappingStrategy(osr.OAMS_TRADITIONAL_GIS_ORDER)
    transform = osr.CoordinateTransformation(source_srs, target_srs)

    def to_lonlat(pixel_x: float, pixel_y: float) -> tuple[float, float]:
        x = geotransform[0] + pixel_x * geotransform[1] + pixel_y * geotransform[2]
        y = geotransform[3] + pixel_x * geotransform[4] + pixel_y * geotransform[5]
        lon, lat, _ = transform.TransformPoint(x, y)
        return lon, lat

    # Si misura al CENTRO del raster: e' il punto meno soggetto alle
    # deformazioni di bordo di una proiezione.
    centre_x = dataset.RasterXSize / 2.0
    centre_y = dataset.RasterYSize / 2.0

    lon0, lat0 = to_lonlat(centre_x + 0.5, centre_y + 0.5)
    lon_east, lat_east = to_lonlat(centre_x + 1.5, centre_y + 0.5)
    lon_south, lat_south = to_lonlat(centre_x + 0.5, centre_y + 1.5)

    lon_metres, lat_metres = tiling.metres_per_degree(lat0)

    def distance(lon_a, lat_a, lon_b, lat_b) -> float:
        return math.hypot((lon_b - lon_a) * lon_metres, (lat_b - lat_a) * lat_metres)

    resolution_x = distance(lon0, lat0, lon_east, lat_east)
    resolution_y = distance(lon0, lat0, lon_south, lat_south)

    units = source_srs.GetAttrValue("UNIT") or "sconosciute"
    dataset = None

    return {
        "groundResolutionXMetres": resolution_x,
        "groundResolutionYMetres": resolution_y,
        "finestMetres": min(resolution_x, resolution_y),
        "nativePixelX": abs(geotransform[1]),
        "nativePixelY": abs(geotransform[5]),
        "nativeUnits": units,
        "centreLatitude": lat0,
    }


def check_level_is_feasible(grid: "LevelGrid") -> None:
    """
    Si ferma PRIMA di provare a creare un raster impossibile.

    GDAL, davanti a un raster smisurato, fallisce con
    'File too large regarding tile size. This would result in a file with tile
    arrays larger than 2GB' - un messaggio che descrive un dettaglio interno del
    formato TIFF e non dice niente sulla causa vera, che e' quasi sempre un
    livello massimo sbagliato. Meglio intercettarlo qui.
    """
    pixels = grid.width * grid.height
    blocks = (math.ceil(grid.width / 256.0) * math.ceil(grid.height / 256.0))

    # Il limite reale di GDAL riguarda gli array di offset dei blocchi TIFF.
    # Si taglia molto prima: oltre questa soglia il raster e' comunque
    # inutilizzabile in pratica.
    if blocks > 100_000_000 or pixels > 5e11:
        raise RuntimeError(
            f"Il raster del livello {grid.level} sarebbe {grid.width:,} x "
            f"{grid.height:,} post ({pixels * 4 / 1e12:.1f} TB non compressi).\n"
            "Non e' realizzabile, e quasi sempre significa che il livello massimo\n"
            "e' stato scelto su una risoluzione sorgente sbagliata.\n\n"
            "Cosa fare:\n"
            "  - controlla la riga 'risoluzione' nello stadio 1: deve essere in metri\n"
            "    e corrispondere al tuo dato (10 m per TINITALY, ~30 m per Copernicus);\n"
            "  - oppure imponi il livello a mano, per esempio --max-level 13.")


# ---------------------------------------------------------------------------
#  Stadio 2: riproiezione orizzontale + quote ellissoidiche
# ---------------------------------------------------------------------------

def warp_and_convert_heights(vrt_path: str, grid: LevelGrid, output_path: str,
                             undulation_grid: np.ndarray,
                             undulation_geotransform: tuple[float, float, float],
                             resampling: str, source_nodata: float | None,
                             progress=None) -> dict:
    """
    Riproietta in EPSG:4326 sulla griglia dei post del livello e converte le
    quote da ortometriche a ellissoidiche sommando N.

    Due cose importanti sul COME:

    1. La riproiezione produce un VRT WARPATO, non un GeoTIFF. Un VRT warpato e'
       un file XML: i pixel vengono calcolati al volo quando li leggi. Cosi'
       esiste un solo raster reale su disco (quello finale con le quote
       ellissoidiche) invece di due, il che su un dataset da decine di GB non e'
       un dettaglio.

    2. La riproiezione e' SOLO ORIZZONTALE, da EPSG:32632 a EPSG:4326. Nessun
       CRS composto, nessuna griglia geoidica dentro gdalwarp. Lo scostamento
       verticale lo applichiamo noi qui sotto, con una somma esplicita, per il
       motivo spiegato in geoid.py: gdalwarp accetta di default una "ballpark
       vertical transformation" che non applica nulla e non lo dice.
    """
    from osgeo import gdal
    gdal.UseExceptions()

    check_level_is_feasible(grid)

    warped = gdal.Warp("", vrt_path, options=gdal.WarpOptions(
        format="VRT",
        dstSRS="EPSG:4326",
        outputBounds=grid.output_bounds,
        width=grid.width,
        height=grid.height,
        resampleAlg=resampling,
        srcNodata=source_nodata,
        dstNodata=float("nan"),
        outputType=gdal.GDT_Float32,
        multithread=True))
    if warped is None:
        raise RuntimeError("gdal.Warp ha fallito")

    driver = gdal.GetDriverByName("GTiff")
    os.makedirs(os.path.dirname(os.path.abspath(output_path)), exist_ok=True)
    destination = driver.Create(output_path, grid.width, grid.height, 1,
                                gdal.GDT_Float32, options=GTIFF_OPTIONS)
    destination.SetGeoTransform(grid.geotransform)
    destination.SetProjection(warped.GetProjection())
    band = destination.GetRasterBand(1)
    band.SetNoDataValue(float("nan"))

    rows_per_block = max(1, min(grid.height,
                                BLOCK_MEMORY_BUDGET_BYTES // max(1, grid.width * 4 * 4)))

    valid_posts = 0
    minimum, maximum = math.inf, -math.inf

    for row0 in range(0, grid.height, rows_per_block):
        rows = min(rows_per_block, grid.height - row0)
        block = warped.GetRasterBand(1).ReadAsArray(0, row0, grid.width, rows)
        block = np.asarray(block, dtype=np.float32)

        # Le quote ortometriche diventano ellissoidiche sommando l'ondulazione
        # del geoide nel punto. N varia di pochi metri su centinaia di km,
        # quindi il broadcast lon x lat e' esatto quanto serve.
        lons = grid.post_lons(0, grid.width)
        lats = grid.post_lats(row0, rows)
        undulation = geoid.sample_undulation(
            undulation_grid, undulation_geotransform,
            lons[np.newaxis, :], lats[:, np.newaxis])

        valid = np.isfinite(block)
        block = np.where(valid, block + undulation, np.float32("nan"))

        if valid.any():
            valid_posts += int(valid.sum())
            minimum = min(minimum, float(np.nanmin(block)))
            maximum = max(maximum, float(np.nanmax(block)))

        band.WriteArray(block, 0, row0)
        if progress:
            progress(row0 + rows, grid.height)

    band.FlushCache()
    destination.FlushCache()
    destination = None
    warped = None

    return {
        "width": grid.width,
        "height": grid.height,
        "validPosts": valid_posts,
        "totalPosts": grid.width * grid.height,
        "minHeightM": None if minimum is math.inf else minimum,
        "maxHeightM": None if maximum == -math.inf else maximum,
    }


# ---------------------------------------------------------------------------
#  Stadio 3: piramide
# ---------------------------------------------------------------------------

#: Kernel di riduzione [1,4,6,4,1]/16, separabile, CENTRATO sul post che resta.
#
#  Perche' non una media 2x2 (la scelta ovvia): la media di 2x2 produce un
#  valore che appartiene al CENTRO delle quattro celle, cioe' a meta' strada fra
#  i post. Usarla come valore del post del livello superiore sposterebbe il
#  terreno di mezzo passo a ogni livello, e l'errore si accumula scendendo di
#  livello. Questo kernel invece e' centrato sul post conservato: filtra le alte
#  frequenze (che altrimenti darebbero aliasing sulle creste) senza spostare
#  nulla. E' la classica riduzione di piramide gaussiana di Burt e Adelson.
REDUCE_KERNEL = np.array([1.0, 4.0, 6.0, 4.0, 1.0], dtype=np.float64) / 16.0
REDUCE_RADIUS = 2


def _weighted_gather_1d(values: np.ndarray, weights: np.ndarray,
                        centre_indices: np.ndarray, axis: int,
                        limit: int) -> tuple[np.ndarray, np.ndarray]:
    """
    Filtro a 5 tap con gestione dei buchi, lungo un asse.

    Ritorna (numeratore, denominatore). Tenerli separati e' cio' che rende il
    filtro separabile ANCHE in presenza di dati mancanti: applicando il filtro
    prima su x e poi su y a numeratore e denominatore si ottiene esattamente la
    media pesata bidimensionale sui soli campioni validi, non un'approssimazione.
    Se si normalizzasse dopo il primo passaggio, il secondo perderebbe traccia di
    quanti campioni validi hanno contribuito.
    """
    numerator = None
    denominator = None

    for offset, weight in zip(range(-REDUCE_RADIUS, REDUCE_RADIUS + 1), weights):
        indices = np.clip(centre_indices + offset, 0, limit - 1)
        sample = np.take(values[0], indices, axis=axis)
        weight_sample = np.take(values[1], indices, axis=axis)

        if numerator is None:
            numerator = weight * sample
            denominator = weight * weight_sample
        else:
            numerator += weight * sample
            denominator += weight * weight_sample

    return numerator, denominator


def reduce_level(source_path: str, source_grid: LevelGrid,
                 destination_path: str, destination_grid: LevelGrid,
                 progress=None) -> dict:
    """
    Costruisce il raster del livello L-1 da quello del livello L.

    Il post di indice globale g al livello L-1 coincide con il post 2g al
    livello L: la corrispondenza e' esatta, non approssimata, ed e' la
    conseguenza diretta della scelta 2^n+1 per la dimensione delle tile.
    """
    from osgeo import gdal
    gdal.UseExceptions()

    source = gdal.Open(source_path)
    source_band = source.GetRasterBand(1)

    driver = gdal.GetDriverByName("GTiff")
    os.makedirs(os.path.dirname(os.path.abspath(destination_path)), exist_ok=True)
    destination = driver.Create(destination_path, destination_grid.width,
                                destination_grid.height, 1, gdal.GDT_Float32,
                                options=GTIFF_OPTIONS)
    destination.SetGeoTransform(destination_grid.geotransform)
    destination.SetProjection(source.GetProjection())
    destination_band = destination.GetRasterBand(1)
    destination_band.SetNoDataValue(float("nan"))

    # Colonna sorgente al centro del filtro, per ogni colonna di destinazione.
    destination_columns = np.arange(destination_grid.width, dtype=np.intp)
    source_centre_x = 2 * (destination_grid.gx0 + destination_columns) - source_grid.gx0

    rows_per_block = max(1, min(destination_grid.height,
                                BLOCK_MEMORY_BUDGET_BYTES //
                                max(1, source_grid.width * 4 * 8)))

    minimum, maximum = math.inf, -math.inf
    valid_posts = 0

    for row0 in range(0, destination_grid.height, rows_per_block):
        rows = min(rows_per_block, destination_grid.height - row0)

        destination_rows = np.arange(row0, row0 + rows, dtype=np.intp)
        source_centre_y = 2 * (destination_grid.gy0 + destination_rows) - source_grid.gy0

        # Finestra sorgente che copre tutti i tap necessari, con il margine del
        # filtro. I bordi si replicano (clip): l'unica zona interessata e' il
        # contorno del raster, che e' gia' fuori dal dato utile.
        first = int(np.clip(source_centre_y.min() - REDUCE_RADIUS, 0, source_grid.height - 1))
        last = int(np.clip(source_centre_y.max() + REDUCE_RADIUS, 0, source_grid.height - 1))

        window = np.asarray(
            source_band.ReadAsArray(0, first, source_grid.width, last - first + 1),
            dtype=np.float64)

        valid = np.isfinite(window)
        values = np.where(valid, window, 0.0)
        weights = valid.astype(np.float64)

        # Passaggio lungo x: (righe finestra) x (colonne destinazione)
        numerator, denominator = _weighted_gather_1d(
            (values, weights), REDUCE_KERNEL, source_centre_x, axis=1, limit=source_grid.width)

        # Passaggio lungo y: (righe destinazione) x (colonne destinazione)
        local_centre_y = np.clip(source_centre_y - first, 0, (last - first))
        numerator, denominator = _weighted_gather_1d(
            (numerator, denominator), REDUCE_KERNEL, local_centre_y,
            axis=0, limit=last - first + 1)

        with np.errstate(invalid="ignore", divide="ignore"):
            block = np.where(denominator > 1e-12, numerator / np.maximum(denominator, 1e-12),
                             np.nan).astype(np.float32)

        finite = np.isfinite(block)
        if finite.any():
            valid_posts += int(finite.sum())
            minimum = min(minimum, float(np.nanmin(block)))
            maximum = max(maximum, float(np.nanmax(block)))

        destination_band.WriteArray(block, 0, row0)
        if progress:
            progress(row0 + rows, destination_grid.height)

    destination_band.FlushCache()
    destination.FlushCache()
    destination = None
    source = None

    return {"validPosts": valid_posts,
            "minHeightM": None if minimum is math.inf else minimum,
            "maxHeightM": None if maximum == -math.inf else maximum}


# ---------------------------------------------------------------------------
#  Griglia di ondulazione del geoide, su disco
# ---------------------------------------------------------------------------

def write_undulation_geotiff(path: str, grid: np.ndarray,
                             geotransform: tuple[float, float, float]) -> None:
    """
    Salva N come GeoTIFF invece che come array binario.

    E' una scelta deliberata a favore della verificabilita': il file si apre in
    QGIS e si vede subito se contiene i ~45 m attesi sull'Italia o una distesa
    di zeri, che e' il sintomo di una griglia geoidica mancante. Un .npy non
    direbbe niente a nessuno.
    """
    from osgeo import gdal, osr
    gdal.UseExceptions()

    origin_lon, origin_lat, spacing = geotransform
    rows, columns = grid.shape

    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    dataset = gdal.GetDriverByName("GTiff").Create(
        path, columns, rows, 1, gdal.GDT_Float32,
        options=["TILED=YES", "COMPRESS=DEFLATE", "PREDICTOR=3"])

    # Anche qui i valori sono campioni PUNTUALI, quindi l'origine arretra di
    # mezzo pixel come per i raster dei livelli.
    dataset.SetGeoTransform((origin_lon - spacing / 2.0, spacing, 0.0,
                             origin_lat + spacing / 2.0, 0.0, -spacing))
    srs = osr.SpatialReference()
    srs.ImportFromEPSG(4326)
    srs.SetAxisMappingStrategy(osr.OAMS_TRADITIONAL_GIS_ORDER)
    dataset.SetProjection(srs.ExportToWkt())
    dataset.GetRasterBand(1).WriteArray(grid)
    dataset.GetRasterBand(1).SetDescription("ondulazione del geoide N = h_ell - H_orto [m]")
    dataset.FlushCache()
    dataset = None


def read_undulation_geotiff(path: str) -> tuple[np.ndarray, tuple[float, float, float]]:
    from osgeo import gdal
    gdal.UseExceptions()

    dataset = gdal.Open(path)
    geotransform = dataset.GetGeoTransform()
    grid = np.asarray(dataset.GetRasterBand(1).ReadAsArray(), dtype=np.float32)
    spacing = geotransform[1]
    # Si torna dal bordo del pixel al centro, cioe' alla posizione del campione.
    result = (grid, (geotransform[0] + spacing / 2.0, geotransform[3] - spacing / 2.0, spacing))
    dataset = None
    return result

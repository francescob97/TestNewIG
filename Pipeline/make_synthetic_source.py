#!/usr/bin/env python3
"""
Genera un finto TINITALY per provare la pipeline senza scaricare 10 GB.

Riproduce le caratteristiche che contano:
  - piu' GeoTIFF affiancati, da mosaicare;
  - CRS EPSG:32632 (UTM 32N), come TINITALY 1.1;
  - passo 10 m;
  - quote ORTOMETRICHE;
  - un valore di nodata (-9999) su una porzione, per simulare il mare.

Il terreno e' deterministico: somma di sinusoidi piu' un rilievo gaussiano.
Deterministico serve, perche' i test confrontano valori attesi.
"""
import argparse
import math
import os

import numpy as np
from osgeo import gdal, osr

gdal.UseExceptions()

NODATA = -9999.0


def synthetic_height(easting: np.ndarray, northing: np.ndarray) -> np.ndarray:
    """Quota ortometrica sintetica in metri, fra 0 e ~2200."""
    x = (easting - 780000.0) / 1000.0     # km dall'origine locale
    y = (northing - 4630000.0) / 1000.0

    height = (300.0
              + 120.0 * np.sin(x / 3.1) * np.cos(y / 2.7)
              + 45.0 * np.sin(x / 0.8 + 1.3)
              + 30.0 * np.cos(y / 0.6 - 0.7)
              # un rilievo netto: serve a verificare che la piramide non
              # appiattisca le creste piu' del dovuto
              + 1600.0 * np.exp(-(((x - 14.0) ** 2 + (y - 9.0) ** 2) / 18.0)))
    return height


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("-o", "--output", default="synthetic_source")
    parser.add_argument("--tiles-x", type=int, default=3)
    parser.add_argument("--tiles-y", type=int, default=2)
    parser.add_argument("--tile-pixels", type=int, default=1000)
    parser.add_argument("--pixel-size", type=float, default=10.0)
    # Un sorgente GEOGRAFICO (EPSG:4326) ha il pixel in GRADI, non in metri:
    # e' il caso del Copernicus DEM, ed e' quello che ha fatto emergere il bug
    # della risoluzione interpretata nelle unita' sbagliate.
    parser.add_argument("--epsg", type=int, default=32632, choices=[32632, 4326])
    args = parser.parse_args()

    if args.epsg == 4326:
        return write_geographic(args)

    os.makedirs(args.output, exist_ok=True)

    origin_easting = 780000.0
    origin_northing = 4650000.0      # angolo NORD-ovest
    span = args.tile_pixels * args.pixel_size

    srs = osr.SpatialReference()
    srs.ImportFromEPSG(32632)
    srs.SetAxisMappingStrategy(osr.OAMS_TRADITIONAL_GIS_ORDER)
    projection = srs.ExportToWkt()

    written = []
    for ty in range(args.tiles_y):
        for tx in range(args.tiles_x):
            west = origin_easting + tx * span
            north = origin_northing - ty * span

            columns = np.arange(args.tile_pixels) + 0.5
            rows = np.arange(args.tile_pixels) + 0.5
            easting = west + columns * args.pixel_size
            northing = north - rows * args.pixel_size

            heights = synthetic_height(easting[np.newaxis, :], northing[:, np.newaxis])

            # "Mare" nell'angolo sud-est del mosaico: verifica il riempimento
            # dei post mancanti e lo scarto delle tile completamente vuote.
            if tx == args.tiles_x - 1 and ty == args.tiles_y - 1:
                distance = np.hypot(
                    (easting[np.newaxis, :] - (west + span)) / span,
                    (northing[:, np.newaxis] - (north - span)) / span)
                heights = np.where(distance < 0.55, NODATA, heights)

            path = os.path.join(args.output, f"tinitaly_fake_{tx}_{ty}.tif")
            dataset = gdal.GetDriverByName("GTiff").Create(
                path, args.tile_pixels, args.tile_pixels, 1, gdal.GDT_Float32,
                options=["TILED=YES", "COMPRESS=DEFLATE", "PREDICTOR=3"])
            dataset.SetGeoTransform((west, args.pixel_size, 0.0,
                                     north, 0.0, -args.pixel_size))
            dataset.SetProjection(projection)
            band = dataset.GetRasterBand(1)
            band.SetNoDataValue(NODATA)
            band.WriteArray(heights.astype(np.float32))
            dataset.FlushCache()
            dataset = None
            written.append(path)

    total_km = (args.tiles_x * span / 1000.0, args.tiles_y * span / 1000.0)
    print(f"{len(written)} file in {args.output}/  "
          f"({total_km[0]:.0f} x {total_km[1]:.0f} km a {args.pixel_size:.0f} m)")
    return 0


def write_geographic(args) -> int:
    """
    Variante geografica: stesso terreno, CRS EPSG:4326, pixel in gradi.

    Riproduce la caratteristica del Copernicus DEM che conta per i test: il
    numero nel geotransform (1/3600 di grado) e' quattro ordini di grandezza
    piu' piccolo della risoluzione reale sul terreno (~30 m).
    """
    os.makedirs(args.output, exist_ok=True)

    pixel_degrees = 1.0 / 3600.0            # 1 arcosecondo, come il Copernicus
    origin_lon, origin_lat = 12.0, 42.0     # angolo nord-ovest

    srs = osr.SpatialReference()
    srs.ImportFromEPSG(4326)
    srs.SetAxisMappingStrategy(osr.OAMS_TRADITIONAL_GIS_ORDER)

    written = []
    for ty in range(args.tiles_y):
        for tx in range(args.tiles_x):
            west = origin_lon + tx * args.tile_pixels * pixel_degrees
            north = origin_lat - ty * args.tile_pixels * pixel_degrees

            columns = np.arange(args.tile_pixels) + 0.5
            rows = np.arange(args.tile_pixels) + 0.5
            lon = west + columns * pixel_degrees
            lat = north - rows * pixel_degrees

            # Stesso terreno, espresso in km approssimativi dall'origine locale.
            easting = 780000.0 + (lon - origin_lon) * 111320.0 * np.cos(np.radians(41.7))
            northing = 4650000.0 + (lat - origin_lat) * 111132.0
            heights = synthetic_height(easting[np.newaxis, :], northing[:, np.newaxis])

            path = os.path.join(args.output, f"geo_fake_{tx}_{ty}.tif")
            dataset = gdal.GetDriverByName("GTiff").Create(
                path, args.tile_pixels, args.tile_pixels, 1, gdal.GDT_Float32,
                options=["TILED=YES", "COMPRESS=DEFLATE", "PREDICTOR=3"])
            dataset.SetGeoTransform((west, pixel_degrees, 0.0,
                                     north, 0.0, -pixel_degrees))
            dataset.SetProjection(srs.ExportToWkt())
            dataset.GetRasterBand(1).WriteArray(heights.astype(np.float32))
            dataset.FlushCache()
            dataset = None
            written.append(path)

    print(f"{len(written)} file EPSG:4326 in {args.output}/  "
          f"(pixel {pixel_degrees:.8f} gradi = ~30 m)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

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
    args = parser.parse_args()

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


if __name__ == "__main__":
    raise SystemExit(main())

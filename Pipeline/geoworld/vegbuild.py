"""
Pipeline della vegetazione: da ESA WorldCover a tile .gvg.

LA SORGENTE
-----------
ESA WorldCover 2021 (v200): la copertura del suolo di tutto il mondo a 10 m,
fatta da Sentinel-1 e Sentinel-2. Gratuita, senza account (licenza CC BY 4.0).
Sta su un bucket pubblico di Amazon in file da 3 x 3 gradi, ognuno un GeoTIFF
"cloud optimized": GDAL ne legge solo i pezzi che servono, via HTTP
(/vsicurl/). Non c'e' niente da scaricare a mano: per Torino si leggono una
decina di MB, non i 70 MB del file intero.

Chi preferisce scaricare (o lavora senza rete) passa --worldcover con la
cartella dei file .tif: si usano tutti quelli che ci sono.

COME
----
1. Il bbox: quello del dataset del terreno (--terrain), o --bbox.
2. A BLOCCHI (le tile del livello 9, ~40 km): ogni blocco si legge con una
   sola gdal.Warp, al passo esatto delle tile del livello finale (13: 256
   pixel da ~10 m per tile, 4096 x 4096 per blocco), con il campione piu'
   vicino (sono classi, non si mediano). Poi si traducono i codici WorldCover
   nei nostri (vegclasses.py) e si taglia nelle tile.
3. Le tile dove non cresce niente (mare, ghiacciai, roccia) non si scrivono.
4. I blocchi fatti si segnano (_work/blocks.json): se il comando si ferma,
   rilanciandolo riparte da dove era.
5. Indice e manifest, dalle tile che ci sono.
"""

from __future__ import annotations

import glob
import json
import math
import os
import time
from typing import Callable

import numpy as np

from . import manifest as terrain_manifest
from . import tiling, vegclasses, vegformat

PIPELINE_VERSION = "1.0"

#: Il livello delle tile: 2,4 km, pixel da ~10 m come WorldCover.
DEFAULT_LEVEL = 13

#: I blocchi letti con una gdal.Warp sola: ~40 km.
BLOCK_LEVEL = 9

WORLDCOVER_URL = ("https://esa-worldcover.s3.eu-central-1.amazonaws.com/v200/2021/map/"
                  "ESA_WorldCover_10m_2021_v200_{name}_Map.tif")
WORLDCOVER_DEGREES = 3


def worldcover_tile_name(lat_deg: int, lon_deg: int) -> str:
    """Il nome di un file WorldCover dal suo angolo sud-ovest: N45E006, S03W060."""
    return (f"{'N' if lat_deg >= 0 else 'S'}{abs(lat_deg):02d}"
            f"{'E' if lon_deg >= 0 else 'W'}{abs(lon_deg):03d}")


def worldcover_tile_names(west: float, south: float, east: float, north: float) -> list[str]:
    """I file WorldCover (3 x 3 gradi) che coprono il bbox."""
    step = WORLDCOVER_DEGREES
    first_lat = int(math.floor(south / step)) * step
    first_lon = int(math.floor(west / step)) * step
    names = []
    lat = first_lat
    while lat < north:
        lon = first_lon
        while lon < east:
            names.append(worldcover_tile_name(lat, lon))
            lon += step
        lat += step
    return names


def configure_gdal_for_http() -> None:
    """Le impostazioni di GDAL per leggere un GeoTIFF remoto a pezzi, in fretta."""
    from osgeo import gdal
    # Senza, GDAL elencherebbe la "cartella" del bucket a ogni apertura.
    gdal.SetConfigOption("GDAL_DISABLE_READDIR_ON_OPEN", "EMPTY_DIR")
    gdal.SetConfigOption("CPL_VSIL_CURL_ALLOWED_EXTENSIONS", ".tif,.vrt")
    gdal.SetConfigOption("VSI_CACHE", "TRUE")
    gdal.SetConfigOption("GDAL_HTTP_MULTIRANGE", "YES")
    gdal.SetConfigOption("GDAL_HTTP_MERGE_CONSECUTIVE_RANGES", "YES")
    gdal.SetConfigOption("GDAL_HTTP_MAX_RETRY", "4")
    gdal.SetConfigOption("GDAL_HTTP_RETRY_DELAY", "2")


def worldcover_sources(bbox: tuple[float, float, float, float], local: str | None,
                       report: Callable[[str], None]) -> list[str]:
    """
    I file da leggere. Con --worldcover: i .tif della cartella (o il file dato).
    Senza: i file del bucket pubblico che coprono il bbox; quelli che non
    esistono (il mare aperto non ha file) si saltano.
    """
    from osgeo import gdal
    if local:
        if os.path.isdir(local):
            files = sorted(glob.glob(os.path.join(local, "**", "*.tif"), recursive=True))
        else:
            files = [local] if os.path.exists(local) else []
        if not files:
            raise FileNotFoundError(f"nessun file .tif in {local}")
        return files

    sources, missing = [], []
    for name in worldcover_tile_names(*bbox):
        url = "/vsicurl/" + WORLDCOVER_URL.format(name=name)
        try:
            gdal.Open(url)
        except RuntimeError:
            missing.append(name)
            report(f"      {name}: non raggiungibile (mare aperto, o niente rete?), salto")
            continue
        sources.append(url)
        report(f"      {name}: ok")
    if not sources:
        # Nessuno raggiungibile: quasi sempre e' la rete (o un proxy), non il
        # mare. Si dice quali file scaricare a mano, con l'indirizzo intero.
        links = "\n".join("    " + WORLDCOVER_URL.format(name=name) for name in missing)
        raise RuntimeError(
            "nessun file WorldCover raggiungibile per quest'area: controlla la rete (o il proxy).\n"
            "Senza rete: scarica questi file con il browser, mettili in una cartella e rilancia con\n"
            "--worldcover <cartella> (costruisci-vegetazione.bat li cerca da solo in dati\\worldcover):\n" + links)
    return sources


def _read_done(path: str) -> set[str]:
    try:
        with open(path, "r", encoding="utf-8") as handle:
            return set(json.load(handle))
    except (FileNotFoundError, ValueError):
        return set()


def _write_done(path: str, done: set[str]) -> None:
    os.makedirs(os.path.dirname(path), exist_ok=True)
    temporary = f"{path}.tmp"
    with open(temporary, "w", encoding="utf-8") as handle:
        json.dump(sorted(done), handle)
    os.replace(temporary, path)


def build(*, output: str, terrain_root: str | None = None, bbox: tuple | None = None,
          worldcover: str | None = None, name: str = "vegetazione", level: int = DEFAULT_LEVEL,
          block_level: int = BLOCK_LEVEL, force: bool = False,
          report: Callable[[str], None] = print) -> dict:
    from osgeo import gdal
    gdal.UseExceptions()
    configure_gdal_for_http()
    if block_level > level:
        raise ValueError(f"il livello dei blocchi ({block_level}) deve stare sopra quello delle tile ({level})")

    # --- 1. Il bbox ---------------------------------------------------------------
    if bbox is None:
        if not terrain_root:
            raise ValueError("serve --terrain (il dataset del terreno) o --bbox")
        box = terrain_manifest.read_manifest(terrain_root)["boundingBox"]
        bbox = (box["west"], box["south"], box["east"], box["north"])
    west, south, east, north = bbox
    report(f"[1/4] area: ovest {west:.4f}, sud {south:.4f}, est {east:.4f}, nord {north:.4f}")
    x0, y0, x1, y1 = tiling.tile_range_for_bbox(level, west, south, east, north)
    # I file WorldCover si scelgono sulle TILE, non sul bbox: le tile del bordo
    # sporgono dal bbox, e un bbox che comincia proprio a 45° N avrebbe una
    # riga di tile appena sotto, che sta nel file del 42°.
    first, last = tiling.tile_bounds(level, x0, y0), tiling.tile_bounds(level, x1, y1)
    extent = (first.west, last.south, last.east, first.north)

    # --- 2. La sorgente ---------------------------------------------------------------
    report("[2/4] ESA WorldCover 10 m" + (f" da {worldcover}" if worldcover else " dal bucket pubblico (lettura a pezzi)"))
    sources = worldcover_sources(extent, worldcover, report)
    vrt_path = "/vsimem/geoworld_worldcover.vrt"
    vrt = gdal.BuildVRT(vrt_path, sources)
    if vrt is None:
        raise RuntimeError("non riesco a mettere insieme i file WorldCover")
    lookup = vegclasses.worldcover_lookup()

    # --- 3. A blocchi ---------------------------------------------------------------------
    shift = level - block_level
    blocks: dict[tuple[int, int], list[tuple[int, int]]] = {}
    for tile_y in range(y0, y1 + 1):
        for tile_x in range(x0, x1 + 1):
            blocks.setdefault((tile_x >> shift, tile_y >> shift), []).append((tile_x, tile_y))
    done_path = os.path.join(output, "_work", f"blocks_{level}.json")
    done = set() if force else _read_done(done_path)
    side = vegformat.RASTER << shift
    report(f"[3/4] {len(blocks)} blocchi da ~{tiling.tile_span_deg(block_level) * 111:.0f} km, "
           f"{(x1 - x0 + 1) * (y1 - y0 + 1)} tile del livello {level}")
    started = time.time()
    written = skipped = 0
    for number, ((block_x, block_y), tiles) in enumerate(sorted(blocks.items()), start=1):
        key = f"{block_x}_{block_y}"
        if key in done:
            continue
        bounds = tiling.tile_bounds(block_level, block_x, block_y)
        warped = gdal.Warp("", vrt, format="MEM", outputBounds=(bounds.west, bounds.south, bounds.east, bounds.north),
                           width=side, height=side, dstSRS="EPSG:4326", resampleAlg="near",
                           outputType=gdal.GDT_Byte, multithread=True)
        codes = warped.GetRasterBand(1).ReadAsArray()
        classes_block = lookup[codes]
        for tile_x, tile_y in tiles:
            row = (tile_y - (block_y << shift)) * vegformat.RASTER
            column = (tile_x - (block_x << shift)) * vegformat.RASTER
            classes = classes_block[row:row + vegformat.RASTER, column:column + vegformat.RASTER]
            path = vegformat.tile_path(output, level, tile_x, tile_y)
            if not vegclasses.grows_anything(classes):
                if os.path.exists(path):
                    os.remove(path)
                skipped += 1
                continue
            tile = vegformat.VegetationTile(level, tile_x, tile_y, vegclasses.tile_flags(classes), classes)
            vegformat.write_tile_atomic(path, vegformat.encode_tile(tile))
            written += 1
        done.add(key)
        _write_done(done_path, done)
        report(f"      blocco {number}/{len(blocks)}: {written} tile scritte, {skipped} senza piante "
               f"({time.time() - started:.0f}s)")
    vrt = None                     # va chiuso prima di cancellarlo dalla memoria
    gdal.Unlink(vrt_path)

    # --- 4. Indice e manifest: dalle tile che ci sono ---------------------------------------
    report("[4/4] indice e manifest")
    entries, counts = [], np.zeros(256, dtype=np.int64)
    level_root = os.path.join(output, str(level))
    for directory, _, files in os.walk(level_root):
        for file_name in files:
            if not file_name.endswith(vegformat.TILE_EXTENSION):
                continue
            path = os.path.join(directory, file_name)
            tile = vegformat.read_tile(path)
            entries.append(vegformat.VegetationTileEntry(tile.x, tile.y, tile.flags, os.path.getsize(path)))
            counts += np.bincount(tile.classes.ravel(), minlength=256)
    vegformat.write_index(output, level, entries)
    total = max(1, int(counts.sum()))
    cover = {cover_class.name: round(float(counts[cover_class.id]) / total, 4)
             for cover_class in vegclasses.CLASSES if counts[cover_class.id] > 0}
    source = {"name": "ESA WorldCover 10m 2021 v200", "license": "CC BY 4.0",
              "attribution": "ESA WorldCover project 2021 / Contains modified Copernicus Sentinel data (2021) "
                             "processed by ESA WorldCover consortium",
              "files": [os.path.basename(path) for path in sources]}
    vegformat.write_manifest(output, vegformat.build_manifest(
        dataset_name=name, bbox=bbox, level=level, tile_count=len(entries), cover=cover,
        source=source, pipeline_version=PIPELINE_VERSION))
    report(f"      {len(entries)} tile; bosco {100.0 * cover.get('Trees', 0.0):.1f}%, "
           f"arbusti {100.0 * cover.get('Shrubs', 0.0):.1f}%, prato {100.0 * cover.get('Grass', 0.0):.1f}%")
    return {"tiles": len(entries), "cover": cover, "written": written, "skipped": skipped}


def verify(root: str, report: Callable[[str], None] = print) -> int:
    """Controlla un dataset: manifest, indice, ogni tile (chiave, dimensione, flag). Ritorna i problemi."""
    manifest = vegformat.read_manifest(root)
    level = int(manifest["level"])
    entries = vegformat.read_index(root, level)
    problems = 0
    for entry in entries:
        path = vegformat.tile_path(root, level, entry.x, entry.y)
        try:
            tile = vegformat.read_tile(path)
        except (OSError, ValueError) as error:
            report(f"  {path}: {error}")
            problems += 1
            continue
        if (tile.level, tile.x, tile.y) != (level, entry.x, entry.y):
            report(f"  {path}: la tile dichiara {tile.level}/{tile.x}/{tile.y}")
            problems += 1
        if os.path.getsize(path) != entry.size or tile.flags != vegclasses.tile_flags(tile.classes):
            report(f"  {path}: dimensione o flag diversi dall'indice")
            problems += 1
        if int(tile.classes.max()) >= len(vegclasses.CLASSES):
            report(f"  {path}: classi sconosciute")
            problems += 1
    report(f"{len(entries)} tile al livello {level}: {problems} problemi")
    return problems

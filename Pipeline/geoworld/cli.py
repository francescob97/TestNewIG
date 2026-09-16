#!/usr/bin/env python3
"""
Orchestrazione della pipeline: da GeoTIFF TINITALY a piramide di tile GeoWorld.

Gli stadi sono sei e si eseguono in ordine. Ognuno e' saltato se il suo stato
registrato dice che e' gia' stato fatto con gli stessi input e parametri, quindi
rilanciare il comando dopo un'interruzione riprende da dove era rimasto.

    1. inventory   scansiona i sorgenti e costruisce il mosaico virtuale (VRT)
    2. geoid       verifica la griglia geoidica e calcola N sull'area
    3. warp        riproietta in EPSG:4326 sulla griglia dei post del livello
                   piu' fine e converte le quote in ellissoidiche
    4. pyramid     riduce di livello in livello fino al livello minimo
    5. tiles       ritaglia le tile di ogni livello
    6. manifest    scrive manifest.json e gli indici per livello
"""

from __future__ import annotations

import argparse
import glob
import json
import math
import os
import sys
import time

import numpy as np

from . import __version__, tiling, tileformat, geoid, environment, manifest as manifest_module
from .raster import (LevelGrid, level_grid_for_bbox, build_source_vrt, source_bounds_wgs84,
                     warp_and_convert_heights, reduce_level,
                     write_undulation_geotiff, read_undulation_geotiff)
from .tilecut import cut_level
from .state import PipelineState

STAGES = ["inventory", "geoid", "warp", "pyramid", "tiles", "manifest"]


# --- utilita' di stampa ----------------------------------------------------

def log(message: str) -> None:
    print(message, file=sys.stderr, flush=True)


def stage_header(index: int, name: str, skipped: bool = False) -> None:
    marker = "  (gia' fatto, salto)" if skipped else ""
    log(f"\n[{index}/{len(STAGES)}] {name}{marker}")


def make_progress(label: str):
    state = {"last": -1.0, "start": time.time()}

    def progress(done: int, total: int) -> None:
        fraction = done / max(total, 1)
        if fraction - state["last"] < 0.05 and done < total:
            return
        state["last"] = fraction
        elapsed = time.time() - state["start"]
        log(f"      {label}: {fraction * 100:5.1f}%  ({done}/{total}, {elapsed:.0f}s)")

    return progress


def human_bytes(count: float) -> str:
    for unit in ("B", "kB", "MB", "GB", "TB"):
        if count < 1024 or unit == "TB":
            return f"{count:.1f} {unit}"
        count /= 1024
    return f"{count:.1f} TB"


# --- comando principale ----------------------------------------------------

def command_build(args: argparse.Namespace) -> int:
    # Fallire subito e con un messaggio utile, invece di un ImportError nudo
    # dopo che l'utente ha gia' lanciato un'elaborazione da ore.
    try:
        from osgeo import gdal  # noqa: F401
        import pyproj           # noqa: F401
    except ImportError as error:
        log(f"Dipendenza mancante: {error}")
        log("Lancia 'python run.py check-env' per la diagnosi e le istruzioni.")
        return 4

    inputs = sorted({path for pattern in args.input for path in glob.glob(pattern)})
    if not inputs:
        log(f"Nessun file corrisponde a {args.input}")
        return 2

    work_dir = args.work or os.path.join(args.output, "_work")
    os.makedirs(args.output, exist_ok=True)
    os.makedirs(work_dir, exist_ok=True)

    state = PipelineState(os.path.join(work_dir, "state.json"))
    for stage in args.redo:
        state.invalidate(stage)

    vrt_path = os.path.join(work_dir, "source.vrt")
    undulation_path = os.path.join(work_dir, "geoid_undulation.tif")

    # ---------------------------------------------------------------- 1 ----
    fingerprint = PipelineState.fingerprint({"stage": "inventory"}, inputs)
    if state.is_complete("inventory", fingerprint):
        stage_header(1, "inventory", skipped=True)
        source_info = state.info("inventory")
    else:
        stage_header(1, "inventory")
        log(f"      {len(inputs)} file sorgente")
        source_info = build_source_vrt(inputs, vrt_path)
        source_info["boundsWgs84"] = source_bounds_wgs84(vrt_path)
        source_info["files"] = [os.path.basename(path) for path in inputs[:20]]
        state.mark_complete("inventory", fingerprint, [vrt_path], source_info)

    west, south, east, north = source_info["boundsWgs84"]
    centre_latitude = (south + north) / 2.0
    source_resolution = max(source_info["pixelSizeX"], source_info["pixelSizeY"])

    log(f"      mosaico  : {source_info['width']} x {source_info['height']} px, "
        f"{source_info['dataType']}, nodata={source_info['nodata']}")
    log(f"      bbox     : {west:.5f} {south:.5f} {east:.5f} {north:.5f}")
    log(f"      pixel    : {source_resolution:.2f} (unita' del CRS sorgente)")

    native_level = tiling.recommended_max_level(source_resolution, centre_latitude)
    max_level = args.max_level if args.max_level is not None else native_level
    min_level = args.min_level

    spacing_lon, spacing_lat = tiling.post_spacing_metres(max_level, centre_latitude)
    log(f"      livello nativo consigliato: {native_level}"
        f"   -> uso livelli {min_level}..{max_level}")
    log(f"      passo post al livello {max_level}: "
        f"{spacing_lat:.2f} m in latitudine, {spacing_lon:.2f} m in longitudine")

    if max_level < native_level:
        log(f"      NOTA: al livello {max_level} si perde risoluzione rispetto al "
            f"dato sorgente (nativo: livello {native_level}).")

    # ---------------------------------------------------------------- 2 ----
    geoid_parameters = {"stage": "geoid", "verticalCrs": args.vertical_crs,
                        "spacing": args.geoid_spacing, "bbox": [west, south, east, north]}
    fingerprint = PipelineState.fingerprint(geoid_parameters)
    if state.is_complete("geoid", fingerprint):
        stage_header(2, "geoid", skipped=True)
        geoid_info = state.info("geoid")
    else:
        stage_header(2, "geoid")
        log(f"      verifica della trasformazione verticale verso {args.vertical_crs}...")
        transform_info = geoid.check_vertical_transform(args.vertical_crs)
        log(f"      pipeline PROJ: {transform_info.description}")
        for name, _, _, undulation in transform_info.sample_points:
            log(f"        N({name}) = {undulation:.3f} m")

        undulation_grid, undulation_geotransform = geoid.build_undulation_grid(
            (west, south, east, north), args.geoid_spacing, args.vertical_crs)
        write_undulation_geotiff(undulation_path, undulation_grid, undulation_geotransform)

        error = geoid.measure_interpolation_error(
            undulation_grid, undulation_geotransform,
            (west, south, east, north), args.vertical_crs)
        log(f"      griglia N: {undulation_grid.shape[1]} x {undulation_grid.shape[0]} "
            f"a passo {args.geoid_spacing} gradi")
        log(f"      errore di interpolazione: max {error['maxErrorM'] * 1000:.4f} mm, "
            f"rms {error['rmsErrorM'] * 1000:.4f} mm")

        if error["maxErrorM"] > args.max_geoid_error:
            log(f"      ERRORE: supera la soglia di {args.max_geoid_error * 1000:.1f} mm. "
                f"Riduci --geoid-spacing.")
            return 3

        geoid_info = {"transform": transform_info.as_dict(), "interpolationError": error}
        state.mark_complete("geoid", fingerprint, [undulation_path], geoid_info)

    undulation_grid, undulation_geotransform = read_undulation_geotiff(undulation_path)

    # ---------------------------------------------------------------- 3 ----
    grids = {level: level_grid_for_bbox(level, (west, south, east, north))
             for level in range(min_level, max_level + 1)}

    def level_raster(level: int) -> str:
        return os.path.join(work_dir, f"height_L{level:02d}.tif")

    warp_parameters = {"stage": "warp", "level": max_level, "resampling": args.resampling,
                       "bbox": [west, south, east, north], "verticalCrs": args.vertical_crs,
                       "geoidSpacing": args.geoid_spacing}
    fingerprint = PipelineState.fingerprint(warp_parameters, [vrt_path, undulation_path])
    if state.is_complete("warp", fingerprint):
        stage_header(3, "warp", skipped=True)
        warp_info = state.info("warp")
    else:
        stage_header(3, "warp")
        grid = grids[max_level]
        log(f"      raster livello {max_level}: {grid.width} x {grid.height} post "
            f"({human_bytes(grid.width * grid.height * 4)} non compressi)")
        log(f"      riproiezione orizzontale EPSG:4326, ricampionamento '{args.resampling}'")
        log(f"      quote ortometriche -> ellissoidiche (+N)")

        warp_info = warp_and_convert_heights(
            vrt_path, grid, level_raster(max_level), undulation_grid,
            undulation_geotransform, args.resampling, source_info["nodata"],
            progress=make_progress("warp"))

        coverage = warp_info["validPosts"] / max(warp_info["totalPosts"], 1)
        log(f"      post con dato: {warp_info['validPosts']:,} su "
            f"{warp_info['totalPosts']:,} ({coverage * 100:.1f}% del bbox)")
        log(f"      quote ellissoidiche: {warp_info['minHeightM']:.1f} .. "
            f"{warp_info['maxHeightM']:.1f} m")
        state.mark_complete("warp", fingerprint, [level_raster(max_level)], warp_info)

    # ---------------------------------------------------------------- 4 ----
    pyramid_parameters = {"stage": "pyramid", "minLevel": min_level, "maxLevel": max_level}
    fingerprint = PipelineState.fingerprint(pyramid_parameters, [level_raster(max_level)])
    if state.is_complete("pyramid", fingerprint):
        stage_header(4, "pyramid", skipped=True)
    else:
        stage_header(4, "pyramid")
        for level in range(max_level - 1, min_level - 1, -1):
            log(f"      livello {level}: {grids[level].width} x {grids[level].height} post")
            reduce_level(level_raster(level + 1), grids[level + 1],
                         level_raster(level), grids[level])
        state.mark_complete("pyramid", fingerprint,
                            [level_raster(level) for level in range(min_level, max_level)])

    # ---------------------------------------------------------------- 5 ----
    tiles_parameters = {"stage": "tiles", "minLevel": min_level, "maxLevel": max_level,
                        "tilePosts": tiling.TILE_POSTS,
                        "formatVersion": tileformat.FORMAT_VERSION}
    fingerprint = PipelineState.fingerprint(
        tiles_parameters, [level_raster(level) for level in range(min_level, max_level + 1)])

    level_summaries: list[manifest_module.LevelSummary] = []
    index_paths: list[str] = []

    if state.is_complete("tiles", fingerprint) and not args.force_tiles:
        stage_header(5, "tiles", skipped=True)
        level_summaries = [manifest_module.LevelSummary(**entry)
                           for entry in state.info("tiles").get("levels", [])]
        index_paths = [os.path.join(args.output, str(summary.level),
                                    manifest_module.INDEX_FILENAME)
                       for summary in level_summaries]
    else:
        stage_header(5, "tiles")
        total_written = 0
        for level in range(min_level, max_level + 1):
            tile_range = tiling.tile_range_for_bbox(level, west, south, east, north)
            x0, y0, x1, y1 = tile_range
            candidates = (x1 - x0 + 1) * (y1 - y0 + 1)
            log(f"      livello {level:2d}: {candidates:,} tile candidate "
                f"(x {x0}..{x1}, y {y0}..{y1})")

            entries, written = cut_level(
                level_raster(level), grids[level], level, args.output, tile_range,
                undulation_grid, undulation_geotransform,
                jobs=args.jobs, force=args.force_tiles,
                progress=make_progress(f"livello {level}") if candidates > 4096 else None)

            total_written += written
            index_paths.append(manifest_module.write_level_index(args.output, level, entries))

            spacing_lon, spacing_lat = tiling.post_spacing_metres(level, centre_latitude)
            level_summaries.append(manifest_module.LevelSummary(
                level=level,
                tile_count=len(entries),
                tile_x_min=min((e.x for e in entries), default=0),
                tile_x_max=max((e.x for e in entries), default=0),
                tile_y_min=min((e.y for e in entries), default=0),
                tile_y_max=max((e.y for e in entries), default=0),
                min_height=min((e.min_height for e in entries), default=0.0),
                max_height=max((e.max_height for e in entries), default=0.0),
                post_spacing_deg=tiling.post_spacing_deg(level),
                post_spacing_m_lat=spacing_lat,
                post_spacing_m_lon_at_centre=spacing_lon))

            log(f"                  {len(entries):,} tile con dato, {written:,} scritte, "
                f"quote {level_summaries[-1].min_height:.1f}..{level_summaries[-1].max_height:.1f} m")

        state.mark_complete("tiles", fingerprint, index_paths,
                            {"levels": [vars(summary) for summary in level_summaries],
                             "written": total_written})

    # ---------------------------------------------------------------- 6 ----
    stage_header(6, "manifest")
    document = manifest_module.build_manifest(
        dataset_name=args.name,
        bbox=(west, south, east, north),
        levels=level_summaries,
        source={
            "description": args.source_description,
            "fileCount": source_info["fileCount"],
            "crs": source_info.get("crs", "")[:200],
            "nativePixelSize": source_resolution,
            "nativeLevel": native_level,
            "resampling": args.resampling,
        },
        vertical=dict(geoid_info.get("transform", {}),
                      interpolationError=geoid_info.get("interpolationError", {}),
                      note=("Quote ELLISSOIDICHE WGS84. Le quote ortometriche del "
                            "sorgente sono state convertite sommando l'ondulazione "
                            "del geoide N, campionata su griglia e interpolata.")),
        pipeline_version=__version__)

    manifest_path = manifest_module.write_manifest(args.output, document)
    log(f"      {manifest_path}")

    total_tiles = sum(summary.tile_count for summary in level_summaries)
    log(f"\nFatto. {total_tiles:,} tile su {len(level_summaries)} livelli in {args.output}")
    log(f"Stima su disco: {human_bytes(total_tiles * tileformat.TILE_BYTES)}")
    return 0


# --- comandi accessori -----------------------------------------------------

def command_check_env(args: argparse.Namespace) -> int:
    """Diagnostica completa dell'ambiente. E' il primo comando da lanciare."""
    report = environment.collect()
    print(environment.format_report(report))
    return 0 if report.ready else 3


def command_check_geoid(args: argparse.Namespace) -> int:
    try:
        info = geoid.check_vertical_transform(args.vertical_crs)
    except RuntimeError as error:
        log(str(error))
        return 3
    log(f"Trasformazione verticale disponibile: {args.vertical_crs}")
    log(f"Pipeline PROJ: {info.description}")
    for name, lon, lat, undulation in info.sample_points:
        log(f"  {name:<18} ({lon:9.5f}, {lat:8.5f})  N = {undulation:7.3f} m")
    return 0


def command_inspect(args: argparse.Namespace) -> int:
    header, heights = tileformat.read_tile(args.tile)
    bounds = tiling.tile_bounds(header.level, header.tile_x, header.tile_y)
    log(f"tile   : livello {header.level}, x={header.tile_x}, y={header.tile_y}")
    log(f"formato: v{header.version}, {header.width}x{header.height} float32, "
        f"post riempiti: {'si' if header.has_filled_posts else 'no'}")
    log(f"bbox   : ovest {bounds.west:.6f}  sud {bounds.south:.6f}  "
        f"est {bounds.east:.6f}  nord {bounds.north:.6f}")
    log(f"passo  : {tiling.post_spacing_deg(header.level):.8f} gradi")
    log(f"quote  : header {header.min_height:.2f}..{header.max_height:.2f} m, "
        f"dati {heights.min():.2f}..{heights.max():.2f} m")
    log(f"angoli : NO={heights[0, 0]:.2f}  NE={heights[0, -1]:.2f}  "
        f"SO={heights[-1, 0]:.2f}  SE={heights[-1, -1]:.2f}")
    return 0


def command_test_vectors(args: argparse.Namespace) -> int:
    tiling.dump_test_vectors(args.output)
    log(f"Vettori di prova dello schema di tiling scritti in {args.output}")
    return 0


# --- parser ---------------------------------------------------------------

def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="geoworld-pipeline",
        description="Da GeoTIFF TINITALY a piramide di tile di quote per GeoWorld.")
    subparsers = parser.add_subparsers(dest="command", required=True)

    build = subparsers.add_parser("build", help="esegue la pipeline completa")
    build.add_argument("-i", "--input", nargs="+", required=True,
                       help="GeoTIFF sorgente (accetta glob, es. 'tinitaly/*.tif')")
    build.add_argument("-o", "--output", required=True, help="cartella radice del dataset")
    build.add_argument("--work", help="cartella degli intermedi (default: <output>/_work)")
    build.add_argument("--name", default="TINITALY 1.1", help="nome del dataset nel manifest")
    build.add_argument("--source-description", default="TINITALY 1.1 (INGV), EPSG:32632, quote ortometriche")
    build.add_argument("--min-level", type=int, default=0)
    build.add_argument("--max-level", type=int, default=None,
                       help="default: il livello che eguaglia la risoluzione nativa del sorgente")
    build.add_argument("--vertical-crs", default=geoid.VERTICAL_CRS_EGM2008,
                       help=f"datum verticale del sorgente (default {geoid.VERTICAL_CRS_EGM2008} = EGM2008; "
                            f"{geoid.VERTICAL_CRS_EGM96} = EGM96)")
    build.add_argument("--geoid-spacing", type=float, default=0.01,
                       help="passo della griglia di ondulazione, in gradi (default 0.01)")
    build.add_argument("--max-geoid-error", type=float, default=0.01,
                       help="errore massimo tollerato sull'interpolazione di N, in metri")
    build.add_argument("--resampling", default="bilinear",
                       choices=["near", "bilinear", "cubic", "cubicspline", "lanczos", "average"])
    build.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 2) // 2))
    build.add_argument("--force-tiles", action="store_true",
                       help="riscrive le tile anche se gia' presenti")
    build.add_argument("--redo", nargs="*", default=[], choices=STAGES,
                       help="invalida gli stadi indicati e li riesegue")
    build.set_defaults(func=command_build)

    subparsers.add_parser(
        "check-env",
        help="diagnostica l'ambiente (GDAL, PROJ, griglie geoidiche)"
    ).set_defaults(func=command_check_env)

    check = subparsers.add_parser("check-geoid",
                                  help="verifica che la griglia geoidica sia disponibile")
    check.add_argument("--vertical-crs", default=geoid.VERTICAL_CRS_EGM2008)
    check.set_defaults(func=command_check_geoid)

    inspect = subparsers.add_parser("inspect", help="stampa il contenuto di una tile")
    inspect.add_argument("tile")
    inspect.set_defaults(func=command_inspect)

    vectors = subparsers.add_parser(
        "test-vectors", help="scrive i vettori di prova dello schema di tiling per il C++")
    vectors.add_argument("-o", "--output", default="tiling_vectors.json")
    vectors.set_defaults(func=command_test_vectors)

    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())

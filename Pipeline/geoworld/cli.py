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

# NOTA: numpy NON si importa qui in cima, e nemmeno Pillow.
#
# Un import in testa a cli.py e' un import che avviene per OGNI comando,
# check-env compreso. Ma check-env esiste proprio per dire cosa manca
# nell'ambiente: se muore importando cio' che deve diagnosticare, l'utente
# riceve una traceback invece della risposta. Le due librerie si importano
# dentro le funzioni che le usano davvero.

from . import __version__, tiling, tileformat, geoid, environment, fetch as fetch_module, manifest as manifest_module
from . import (fetchimagery, imagecut, imageformat, imagerybuild,
               imagerymanifest)
from .raster import (LevelGrid, level_grid_for_bbox, build_source_vrt, source_bounds_wgs84,
                     source_ground_resolution, check_level_is_feasible,
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
        source_info = build_source_vrt(inputs, vrt_path, source_crs=args.source_crs)
        source_info["boundsWgs84"] = source_bounds_wgs84(vrt_path)
        source_info["resolution"] = source_ground_resolution(vrt_path)
        source_info["files"] = [os.path.basename(path) for path in inputs[:20]]
        state.mark_complete("inventory", fingerprint, [vrt_path], source_info)

    west, south, east, north = source_info["boundsWgs84"]
    centre_latitude = (south + north) / 2.0

    # ------------------------------------------------------------------
    #  La risoluzione va misurata SUL TERRENO, non letta dal geotransform.
    #  Il geotransform e' nelle unita' del CRS sorgente: metri per TINITALY
    #  (UTM 32N), GRADI per il Copernicus DEM (EPSG:4326). Confondere le due
    #  cose fa scegliere un livello di piramide centomila volte troppo fine.
    # ------------------------------------------------------------------
    resolution = source_info["resolution"]
    source_resolution = resolution["finestMetres"]

    log(f"      mosaico  : {source_info['width']} x {source_info['height']} px, "
        f"{source_info['dataType']}, nodata={source_info['nodata']}")
    log(f"      formato  : {source_info.get('driver', '?')}")

    # AAIGrid e' il driver dei grid ESRI ASCII. Sono file di TESTO: occupano
    # circa dieci volte un GeoTIFF equivalente e si leggono molto piu' lentamente,
    # perche' ogni valore va convertito da stringa. Su TINITALY intero la
    # differenza e' fra minuti e ore.
    if source_info.get("driver") == "AAIGrid":
        log("      NOTA: i sorgenti sono grid ESRI ASCII (file di testo). La pipeline")
        log("            li legge, ma convertirli in GeoTIFF prima e' molto piu' veloce:")
        log('            gdal_translate -of GTiff -co COMPRESS=DEFLATE -co PREDICTOR=3 in.asc out.tif')
    log(f"      bbox     : {west:.5f} {south:.5f} {east:.5f} {north:.5f}")
    log(f"      pixel    : {resolution['nativePixelX']:.8g} {resolution['nativeUnits']} "
        f"(unita' del CRS sorgente)")
    log(f"      risoluzione sul terreno: {resolution['groundResolutionXMetres']:.2f} m "
        f"(lon) x {resolution['groundResolutionYMetres']:.2f} m (lat)")

    native_level = tiling.recommended_max_level(source_resolution, centre_latitude)
    max_level = args.max_level if args.max_level is not None else native_level
    min_level = args.min_level

    if min_level > max_level:
        log(f"      --min-level {min_level} e' maggiore di --max-level {max_level}.")
        return 2

    spacing_lon, spacing_lat = tiling.post_spacing_metres(max_level, centre_latitude)
    log(f"      livello nativo consigliato: {native_level}"
        f"   -> uso livelli {min_level}..{max_level}")
    log(f"      passo post al livello {max_level}: "
        f"{spacing_lat:.2f} m in latitudine, {spacing_lon:.2f} m in longitudine")

    if native_level >= tiling.MAX_SUPPORTED_LEVEL:
        log("")
        log(f"      ATTENZIONE: il livello consigliato ha toccato il tetto "
            f"({tiling.MAX_SUPPORTED_LEVEL}).")
        log(f"      Significa che la risoluzione misurata ({source_resolution:.4g} m) e'")
        log("      implausibilmente fine. Controlla il CRS dei file sorgente, oppure")
        log("      imponi il livello a mano con --max-level.")
    elif max_level < native_level:
        log(f"      NOTA: al livello {max_level} si perde risoluzione rispetto al "
            f"dato sorgente (nativo: livello {native_level}).")

    # ---------------------------------------------------------------- 2 ----
    geoid_parameters = {"stage": "geoid", "verticalCrs": args.vertical_crs,
                        "spacing": args.geoid_spacing, "maxError": args.max_geoid_error,
                        "bbox": [west, south, east, north]}
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

        undulation_grid, undulation_geotransform, error, used_spacing = \
            geoid.build_aligned_undulation_grid(
                (west, south, east, north), args.vertical_crs,
                max_error_m=args.max_geoid_error,
                spacing_deg=args.geoid_spacing, log=log)

        write_undulation_geotiff(undulation_path, undulation_grid, undulation_geotransform)

        if error["maxErrorM"] > args.max_geoid_error:
            log("")
            log(f"      ERRORE: dopo il raffinamento l'errore resta "
                f"{error['maxErrorM'] * 1000:.3f} mm, sopra la soglia di "
                f"{args.max_geoid_error * 1000:.1f} mm.")
            log("      Se il datum verticale non e' EGM2008 o EGM96, il passo nativo")
            log("      della sua griglia non e' noto e l'allineamento non e' automatico:")
            log("      prova --geoid-spacing con un sottomultiplo del passo di quella griglia,")
            log(f"      oppure alza la soglia con --max-geoid-error.")
            return 3

        geoid_info = {"transform": transform_info.as_dict(), "interpolationError": error,
                      "samplingSpacingDeg": used_spacing}
        state.mark_complete("geoid", fingerprint, [undulation_path], geoid_info)

    undulation_grid, undulation_geotransform = read_undulation_geotiff(undulation_path)

    # ---------------------------------------------------------------- 3 ----
    grids = {level: level_grid_for_bbox(level, (west, south, east, north))
             for level in range(min_level, max_level + 1)}

    # Riepilogo PRIMA di muovere un byte: dimensione di ogni raster intermedio e
    # numero di tile candidate. E' qui che un livello massimo sbagliato si vede
    # a colpo d'occhio, invece di manifestarsi come un errore di GDAL dopo
    # minuti di elaborazione.
    log("")
    log("      livello      raster (post)        intermedio    tile candidate")
    total_intermediate = 0
    total_candidates = 0
    for level in range(max_level, min_level - 1, -1):
        grid = grids[level]
        intermediate = grid.width * grid.height * 4
        total_intermediate += intermediate
        x0, y0, x1, y1 = tiling.tile_range_for_bbox(level, west, south, east, north)
        candidates = (x1 - x0 + 1) * (y1 - y0 + 1)
        total_candidates += candidates
        log(f"      {level:>7}   {grid.width:>8,} x {grid.height:<8,}  "
            f"{human_bytes(intermediate):>10}    {candidates:>12,}")
    log(f"      totale intermedi ~{human_bytes(total_intermediate)} "
        f"(compressi: molto meno), fino a {total_candidates:,} tile "
        f"= {human_bytes(total_candidates * tileformat.TILE_BYTES)}")
    log("")

    try:
        check_level_is_feasible(grids[max_level])
    except RuntimeError as error:
        log(str(error))
        return 5

    def level_raster(level: int) -> str:
        return os.path.join(work_dir, f"height_L{level:02d}.tif")

    warp_parameters = {"stage": "warp", "level": max_level, "resampling": args.resampling,
                       "bbox": [west, south, east, north], "verticalCrs": args.vertical_crs,
                       "geoidSpacing": geoid_info.get("samplingSpacingDeg")}
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

def command_fetch(args: argparse.Namespace) -> int:
    if args.bbox:
        bbox = tuple(args.bbox)
    else:
        bbox = fetch_module.NAMED_AREAS[args.area]

    result = fetch_module.fetch_copernicus(
        bbox=bbox, directory=args.output, resolution=args.resolution,
        jobs=args.jobs, dry_run=args.dry_run, log=log)

    if not args.dry_run and result.get("tiles"):
        log("")
        name = args.area if not args.bbox else "area"
        log("Prossimo passo:")
        log(f'    python run.py build -i "{os.path.join(args.output, "*.tif")}" \\')
        log(f'        -o dataset/{name} --name "Copernicus DEM GLO-{args.resolution}" \\')
        log(f'        --source-description "Copernicus DEM GLO-{args.resolution}, '
            f'EPSG:4326, quote EGM2008"')
    return 0 if not result.get("failures") else 1


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


def command_verify(args: argparse.Namespace) -> int:
    """
    Controlla un dataset gia' generato.

    I test automatici girano su un sorgente sintetico. Questo comando gira sul
    dataset VERO, che e' quello che poi finisce nel motore: prima di fidarsi di
    35 GB conviene controllarli. Campiona invece di leggere tutto, perche' su
    centinaia di migliaia di tile la lettura completa durerebbe troppo per
    essere una cosa che si fa davvero.
    """
    import random

    root = args.output
    try:
        document = manifest_module.read_manifest(root)
    except FileNotFoundError:
        log(f"Nessun manifest.json in {root}: non e' un dataset GeoWorld.")
        return 2

    log(f"Dataset   : {document.get('datasetName')}")
    log(f"Generato da: {document.get('generator')}")
    vertical = document.get("verticalDatum", {})
    log(f"Datum verticale: {vertical.get('verticalCrs', '?')}")
    box = document["boundingBox"]
    log(f"Bbox      : {box['west']:.5f} {box['south']:.5f} {box['east']:.5f} {box['north']:.5f}")
    log("")

    random.seed(args.seed)
    problems: list[str] = []
    total_tiles = 0
    checked_tiles = 0
    checked_seams = 0

    for entry in document["levels"]:
        level = entry["level"]
        try:
            index = manifest_module.read_level_index(root, level)
        except (FileNotFoundError, ValueError) as error:
            problems.append(f"livello {level}: indice illeggibile ({error})")
            continue

        total_tiles += len(index)
        if entry["tile_count"] != len(index):
            problems.append(f"livello {level}: il manifest dichiara {entry['tile_count']} "
                            f"tile, l'indice ne ha {len(index)}")

        present = {(item.x, item.y): item for item in index}
        sample = random.sample(index, min(args.sample, len(index)))

        for item in sample:
            path = os.path.join(root, tileformat.tile_relative_path(level, item.x, item.y))
            if not os.path.exists(path):
                problems.append(f"{level}/{item.x}/{item.y}: nell'indice ma non su disco")
                continue
            try:
                header, heights = tileformat.read_tile(path)
            except (ValueError, OSError) as error:
                problems.append(f"{level}/{item.x}/{item.y}: illeggibile ({error})")
                continue

            checked_tiles += 1

            if (header.level, header.tile_x, header.tile_y) != (level, item.x, item.y):
                problems.append(f"{level}/{item.x}/{item.y}: l'header dichiara "
                                f"{header.level}/{header.tile_x}/{header.tile_y}")
            if abs(header.min_height - item.min_height) > 1e-3:
                problems.append(f"{level}/{item.x}/{item.y}: min nell'indice "
                                f"{item.min_height:.3f}, nell'header {header.min_height:.3f}")
            import numpy as np

            if not np.isfinite(heights).all():
                problems.append(f"{level}/{item.x}/{item.y}: contiene valori non finiti")

            # Giunzione con la tile a est, se esiste: e' la proprieta' che in
            # Fase 5 decide se il terreno ha crepe o no.
            neighbour = (item.x + 1, item.y)
            if neighbour in present:
                neighbour_path = os.path.join(
                    root, tileformat.tile_relative_path(level, *neighbour))
                if os.path.exists(neighbour_path):
                    _, right = tileformat.read_tile(neighbour_path)
                    checked_seams += 1
                    if not np.array_equal(heights[:, tiling.TILE_CELLS], right[:, 0]):
                        problems.append(
                            f"GIUNZIONE ROTTA fra {level}/{item.x}/{item.y} e "
                            f"{level}/{neighbour[0]}/{neighbour[1]}")

        log(f"  livello {level:2d}: {len(index):>8,} tile, {len(sample)} campionate, "
            f"quote {entry['min_height']:.1f}..{entry['max_height']:.1f} m")

    log("")
    log(f"Totale: {total_tiles:,} tile, {checked_tiles} verificate, "
        f"{checked_seams} giunzioni controllate.")
    log(f"Spazio stimato: {human_bytes(total_tiles * tileformat.TILE_BYTES)}")

    if problems:
        log("")
        log(f"{len(problems)} PROBLEMI:")
        for problem in problems[:30]:
            log(f"  {problem}")
        if len(problems) > 30:
            log(f"  ... e altri {len(problems) - 30}")
        return 1

    log("")
    log("Nessun problema rilevato.")
    return 0


def command_test_vectors(args: argparse.Namespace) -> int:
    tiling.dump_test_vectors(args.output)
    log(f"Vettori di prova dello schema di tiling scritti in {args.output}")
    return 0


# --- parser ---------------------------------------------------------------

# =============================================================================
#  ORTOFOTO (Fase 6)
#
#  Comandi separati da quelli delle quote, non varianti degli stessi. Le due
#  piramidi sono dataset indipendenti: mischiare i comandi porterebbe prima o
#  poi a scrivere immagini dentro un dataset di quote.
# =============================================================================

def command_fetch_imagery(args: argparse.Namespace) -> int:
    bbox = tuple(args.bbox) if args.bbox else fetchimagery.NAMED_AREAS[args.area]
    log(f"Area: ovest {bbox[0]} sud {bbox[1]} est {bbox[2]} nord {bbox[3]}")
    log("")

    try:
        paths = fetchimagery.fetch(
            bbox, args.output,
            year=args.year, months=args.months, max_cloud=args.max_cloud,
            stream=args.stream, report=log)
    except RuntimeError as error:
        log(f"ERRORE: {error}")
        return 1

    log("")
    log("Da dare in pasto alla pipeline:")
    if args.stream:
        log(f"  python run.py build-imagery -i {' '.join(repr(p) for p in paths)} -o dataset/ortofoto")
        log("  (GDAL legge dalla rete: comodo per provare, lento su aree grandi)")
    else:
        log(f"  python run.py build-imagery -i \"{args.output}/*_TCI.tif\" -o dataset/ortofoto")
    return 0


def command_build_imagery(args: argparse.Namespace) -> int:
    try:
        imagerybuild.build(
            inputs=args.input, output=args.output, work=args.work,
            name=args.name, source_description=args.source_description,
            min_level=args.min_level, max_level=args.max_level,
            quality=args.quality, source_crs=args.source_crs,
            report=log)
    except (FileNotFoundError, RuntimeError, ValueError) as error:
        log(f"ERRORE: {error}")
        return 1
    return 0


def command_inspect_imagery(args: argparse.Namespace) -> int:
    header, pixels = imageformat.read_tile(args.tile)
    bounds = tiling.tile_bounds(header.level, header.tile_x, header.tile_y)
    log(f"tile    : livello {header.level}, x={header.tile_x}, y={header.tile_y}")
    log(f"formato : v{header.version}, {header.width}x{header.height}, "
        f"payload {header.payload_name} da {header.payload_size} byte")
    log(f"copertura: {header.coverage_percent}%  "
        f"(pixel di riempimento: {'si' if header.has_filled_pixels else 'no'})")
    log(f"bbox    : ovest {bounds.west:.6f}  sud {bounds.south:.6f}  "
        f"est {bounds.east:.6f}  nord {bounds.north:.6f}")
    log(f"pixel   : {imagecut.imagery_pixel_size_deg(header.level):.8f} gradi")
    log(f"colori  : medio RGB {pixels.reshape(-1, 3).mean(axis=0).round(1).tolist()}")
    log(f"angoli  : NO={pixels[0, 0].tolist()}  NE={pixels[0, -1].tolist()}  "
        f"SO={pixels[-1, 0].tolist()}  SE={pixels[-1, -1].tolist()}")
    return 0


def command_verify_imagery(args: argparse.Namespace) -> int:
    """
    Controlla un dataset di ortofoto gia' generato, leggendolo davvero.

    Stessa filosofia di `verify` per le quote: i test automatici girano su un
    sorgente sintetico, questo gira sul dataset vero.
    """
    root = args.output
    failures = 0

    try:
        document = imagerymanifest.read_manifest(root)
    except FileNotFoundError:
        log(f"ERRORE: manca {imagerymanifest.MANIFEST_FILENAME} in {root}")
        return 1

    if document.get("datasetKind") != "imagery":
        log("ERRORE: questo manifest non dichiara un dataset di immagini. "
            "Hai puntato a una piramide di quote?")
        return 1

    log(f"dataset  : {document['datasetName']}")
    log(f"livelli  : {[level['level'] for level in document['levels']]}")

    for level_info in document["levels"]:
        level = level_info["level"]
        entries = imagerymanifest.read_level_index(root, level)

        if len(entries) != level_info["tile_count"]:
            log(f"  livello {level}: FALLITO - indice {len(entries)} voci, "
                f"manifest {level_info['tile_count']}")
            failures += 1
            continue

        # Si legge davvero qualche tile: un indice coerente con un manifest non
        # dimostra che i file esistano e siano decodificabili.
        sample = entries[:: max(1, len(entries) // 8)][:8]
        bad = 0
        for entry in sample:
            path = os.path.join(root, imageformat.tile_relative_path(level, entry.x, entry.y))
            try:
                header, pixels = imageformat.read_tile(path)
            except Exception as error:
                log(f"  livello {level}: tile {entry.x},{entry.y} illeggibile: {error}")
                bad += 1
                continue
            if (header.level, header.tile_x, header.tile_y) != (level, entry.x, entry.y):
                log(f"  livello {level}: tile {entry.x},{entry.y} dichiara "
                    f"{header.level}/{header.tile_x}/{header.tile_y}")
                bad += 1
            if pixels.shape != (imageformat.TILE_PIXELS, imageformat.TILE_PIXELS, 3):
                log(f"  livello {level}: tile {entry.x},{entry.y} ha forma {pixels.shape}")
                bad += 1

        status = "ok" if bad == 0 else f"FALLITO ({bad} problemi)"
        log(f"  livello {level:2d}: {len(entries):7d} tile, "
            f"{level_info['partial_tiles']:5d} parziali, "
            f"{level_info['pixel_size_m_lat']:7.2f} m/pixel   {status}")
        failures += bad

    log("")
    log("TUTTO A POSTO" if failures == 0 else f"{failures} PROBLEMI")
    return 0 if failures == 0 else 1



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
    build.add_argument("--name", default="senza nome",
                       help="nome del dataset, finisce nel manifest")
    build.add_argument("--source-description", default="non dichiarata",
                       help="provenienza dei dati, finisce nel manifest")
    build.add_argument("--min-level", type=int, default=0)
    build.add_argument("--max-level", type=int, default=None,
                       help="default: il livello che eguaglia la risoluzione nativa del sorgente")
    build.add_argument("--vertical-crs", default=geoid.VERTICAL_CRS_EGM2008,
                       help=f"datum verticale del sorgente (default {geoid.VERTICAL_CRS_EGM2008} = EGM2008; "
                            f"{geoid.VERTICAL_CRS_EGM96} = EGM96)")
    build.add_argument("--geoid-spacing", type=float, default=None,
                       help="passo della griglia di ondulazione in gradi. Per default viene "
                            "scelto come sottomultiplo intero del passo nativo della griglia "
                            "geoidica (1/96 per EGM2008), perche' l'errore di ricampionamento "
                            "dipende dall'allineamento e non dalla finezza. Impostalo solo se "
                            "sai cosa stai facendo.")
    build.add_argument("--max-geoid-error", type=float, default=0.01,
                       help="errore massimo tollerato sull'interpolazione di N, in metri")
    build.add_argument("--source-crs", default=None,
                       help="CRS del sorgente, se i file non lo dichiarano "
                            "(es. EPSG:32632 per i grid ESRI ASCII di TINITALY)")
    build.add_argument("--resampling", default="bilinear",
                       choices=["near", "bilinear", "cubic", "cubicspline", "lanczos", "average"])
    build.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 2) // 2))
    build.add_argument("--force-tiles", action="store_true",
                       help="riscrive le tile anche se gia' presenti")
    build.add_argument("--redo", nargs="*", default=[], choices=STAGES,
                       help="invalida gli stadi indicati e li riesegue")
    build.set_defaults(func=command_build)

    fetch = subparsers.add_parser(
        "fetch", help="scarica un DEM pubblico (Copernicus GLO-30) per un'area")
    area_group = fetch.add_mutually_exclusive_group()
    area_group.add_argument("--area", default="test",
                            choices=sorted(fetch_module.NAMED_AREAS),
                            help="area predefinita (default: test, una sola tile su Roma)")
    area_group.add_argument("--bbox", nargs=4, type=float,
                            metavar=("OVEST", "SUD", "EST", "NORD"))
    fetch.add_argument("-o", "--output", required=True, help="cartella di destinazione")
    fetch.add_argument("--resolution", type=int, default=30, choices=[30, 90])
    fetch.add_argument("--jobs", type=int, default=6)
    fetch.add_argument("--dry-run", action="store_true",
                       help="elenca le tile e quanto pesano, senza scaricare")
    fetch.set_defaults(func=command_fetch)

    subparsers.add_parser(
        "check-env",
        help="diagnostica l'ambiente (GDAL, PROJ, griglie geoidiche)"
    ).set_defaults(func=command_check_env)

    check = subparsers.add_parser("check-geoid",
                                  help="verifica che la griglia geoidica sia disponibile")
    check.add_argument("--vertical-crs", default=geoid.VERTICAL_CRS_EGM2008)
    check.set_defaults(func=command_check_geoid)

    verify = subparsers.add_parser(
        "verify", help="controlla un dataset gia' generato (indici, header, giunzioni)")
    verify.add_argument("-o", "--output", required=True, help="cartella radice del dataset")
    verify.add_argument("--sample", type=int, default=150,
                        help="tile da campionare per livello (default 150)")
    verify.add_argument("--seed", type=int, default=0)
    verify.set_defaults(func=command_verify)

    inspect = subparsers.add_parser("inspect", help="stampa il contenuto di una tile")
    inspect.add_argument("tile")
    inspect.set_defaults(func=command_inspect)

    vectors = subparsers.add_parser(
        "test-vectors", help="scrive i vettori di prova dello schema di tiling per il C++")
    vectors.add_argument("-o", "--output", default="tiling_vectors.json")
    vectors.set_defaults(func=command_test_vectors)

    # --- ortofoto ----------------------------------------------------------
    fetch_img = subparsers.add_parser(
        "fetch-imagery",
        help="scarica ortofoto Sentinel-2 (10 m, libere) per un'area")
    img_area = fetch_img.add_mutually_exclusive_group()
    img_area.add_argument("--area", default="test",
                          choices=sorted(fetchimagery.NAMED_AREAS),
                          help="area predefinita (default: test, un pezzo di Roma)")
    img_area.add_argument("--bbox", nargs=4, type=float,
                          metavar=("OVEST", "SUD", "EST", "NORD"))
    fetch_img.add_argument("-o", "--output", required=True, help="cartella di destinazione")
    fetch_img.add_argument("--year", type=int, default=2024)
    fetch_img.add_argument("--months", type=int, nargs="+", default=[6, 7, 8],
                           help="mesi in cui cercare (default: estate, poche nuvole)")
    fetch_img.add_argument("--max-cloud", type=float, default=10.0,
                           help="copertura nuvolosa massima accettata, in percentuale")
    fetch_img.add_argument("--stream", action="store_true",
                           help="non scaricare: restituisce percorsi /vsicurl/ che GDAL "
                                "legge dalla rete leggendo solo le finestre che servono")
    fetch_img.set_defaults(func=command_fetch_imagery)

    build_img = subparsers.add_parser(
        "build-imagery", help="costruisce la piramide di ortofoto")
    build_img.add_argument("-i", "--input", nargs="+", required=True,
                           help="raster sorgente. QUALUNQUE formato che GDAL sappia leggere: "
                                "GeoTIFF, JPEG2000, ECW se il driver c'e', anche /vsicurl/. "
                                "Accetta glob, es. 'ortofoto/*.tif'")
    build_img.add_argument("-o", "--output", required=True, help="cartella radice del dataset")
    build_img.add_argument("--work", help="cartella degli intermedi (default: <output>/_work)")
    build_img.add_argument("--name", default="senza nome")
    build_img.add_argument("--source-description", default="non dichiarata")
    build_img.add_argument("--min-level", type=int, default=0)
    build_img.add_argument("--max-level", type=int, default=None,
                           help="default: il livello che eguaglia la risoluzione della sorgente. "
                                "A parita' di livello un'immagine e' il doppio piu' fine di un "
                                "terreno, perche' 256 pixel contro 128 celle")
    build_img.add_argument("--quality", type=int, default=imageformat.DEFAULT_QUALITY,
                           help="qualita' JPEG (default 85)")
    build_img.add_argument("--source-crs", default=None,
                           help="CRS del sorgente, se i file non lo dichiarano")
    build_img.set_defaults(func=command_build_imagery)

    verify_img = subparsers.add_parser(
        "verify-imagery", help="controlla un dataset di ortofoto gia' generato")
    verify_img.add_argument("-o", "--output", required=True)
    verify_img.set_defaults(func=command_verify_imagery)

    inspect_img = subparsers.add_parser(
        "inspect-imagery", help="stampa il contenuto di una tile di immagine")
    inspect_img.add_argument("tile")
    inspect_img.set_defaults(func=command_inspect_imagery)

    return parser



def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())

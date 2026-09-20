"""
Orchestrazione della pipeline delle ortofoto: dai raster sorgente al dataset.

E' il gemello di quello che `cli.command_build` fa per le quote, ma molto piu'
corto, perche' manca tutta la parte verticale: un'ortofoto non ha un datum da
convertire. Restano tre passi.

    1. mosaico virtuale dei sorgenti (VRT)
    2. taglio del livello piu' fine, con una warp per tile
    3. riduzione verso l'alto, 2x2, senza mai ritoccare la sorgente

INDIFFERENZA ALLA SORGENTE
--------------------------
Non c'e' un solo punto in cui si assuma il formato del file d'ingresso. Tutto
passa da `gdal.Open`, quindi funziona con GeoTIFF, JPEG2000, ECW (se il driver
c'e'), MrSID, PNG con world file e qualunque altra cosa GDAL sappia leggere,
esattamente come la pipeline delle quote accetta sia i GeoTIFF di TINITALY sia
i DTED .dt2.

Il comando `check-env` dice quali driver sono effettivamente disponibili, perche'
"GDAL legge le ECW" e "la TUA installazione di GDAL legge le ECW" sono due
affermazioni diverse.
"""

from __future__ import annotations

import glob
import os
import time

from . import imagecut, imageformat, imagerymanifest, raster, tiling

PIPELINE_VERSION = "1.0"


def expand_inputs(patterns: list[str]) -> list[str]:
    """Espande i glob e verifica che qualcosa esista davvero."""
    files: list[str] = []
    for pattern in patterns:
        matched = sorted(glob.glob(pattern))
        if matched:
            files.extend(matched)
        elif os.path.exists(pattern):
            files.append(pattern)

    if not files:
        raise FileNotFoundError(
            f"nessun file corrisponde a {patterns}. "
            "Controlla il percorso, e ricorda le virgolette attorno ai glob su Windows.")
    return files


def build(*, inputs: list[str], output: str, work: str | None = None,
          name: str = "senza nome", source_description: str = "non dichiarata",
          min_level: int = 0, max_level: int | None = None,
          quality: int = imageformat.DEFAULT_QUALITY,
          source_crs: str | None = None,
          report=print) -> dict:
    """Costruisce la piramide di ortofoto. Riprende da dove era rimasta."""
    files = expand_inputs(inputs)
    work = work or os.path.join(output, "_work")
    os.makedirs(work, exist_ok=True)

    started = time.time()

    # --- 1. mosaico virtuale -------------------------------------------
    report(f"[1/3] mosaico virtuale di {len(files)} file")
    vrt_path = os.path.join(work, "imagery_source.vrt")
    vrt_info = raster.build_source_vrt(files, vrt_path, source_crs=source_crs)

    bbox = raster.source_bounds_wgs84(vrt_path)
    west, south, east, north = bbox
    centre_latitude = (south + north) * 0.5

    resolution = raster.source_ground_resolution(vrt_path)
    ground_metres = min(resolution["x_metres"], resolution["y_metres"])

    report(f"      area: {west:.4f} {south:.4f} {east:.4f} {north:.4f}")
    report(f"      risoluzione sul terreno: {ground_metres:.2f} m")

    if max_level is None:
        max_level = imagecut.recommended_max_level(ground_metres, centre_latitude)
        report(f"      livello massimo consigliato: {max_level}")

    if max_level > tiling.MAX_SUPPORTED_LEVEL:
        raise ValueError(
            f"livello {max_level} oltre il massimo supportato "
            f"({tiling.MAX_SUPPORTED_LEVEL}). Passa --max-level per limitarlo.")

    # Una tabella dei livelli PRIMA di cominciare: e' quella che permette di
    # accorgersi di aver chiesto mezzo terabyte di tile prima di generarle, non
    # dopo tre ore.
    report("")
    report("      livello   pixel       tile stimate")
    for level in range(min_level, max_level + 1):
        x0, y0, x1, y1 = tiling.tile_range_for_bbox(level, west, south, east, north)
        count = (x1 - x0 + 1) * (y1 - y0 + 1)
        _, metres_lat = imagecut.imagery_pixel_size_metres(level, centre_latitude)
        report(f"      {level:7d}   {metres_lat:7.2f} m   {count:12d}")
    report("")

    stats = imagecut.ImageryStats()

    # --- 2. livello piu' fine, dalla sorgente --------------------------
    report(f"[2/3] taglio del livello {max_level} dalla sorgente")
    entries = imagecut.cut_finest_level(
        vrt_path, output, max_level, bbox, quality, stats,
        progress=lambda message: report(f"      {message}"))

    if not entries:
        raise RuntimeError(
            "nessuna tile prodotta: la sorgente non copre nessuna tile del bbox. "
            "Controlla che i file abbiano una georeferenziazione valida.")

    levels: dict[int, list] = {max_level: entries}

    # --- 3. riduzione verso l'alto -------------------------------------
    report(f"[3/3] riduzione dai figli, dal livello {max_level - 1} al {min_level}")
    for level in range(max_level - 1, min_level - 1, -1):
        levels[level] = imagecut.build_level_from_children(
            output, level, levels[level + 1], quality, stats,
            progress=lambda message: report(f"      {message}"))

    # --- indici e manifest ---------------------------------------------
    summaries = []
    for level in sorted(levels):
        index_entries = [imagerymanifest.ImageTileEntry(x, y, coverage)
                         for x, y, coverage in levels[level]]
        if not index_entries:
            continue
        imagerymanifest.write_level_index(output, level, index_entries)
        summaries.append(imagerymanifest.summarise_level(level, index_entries, centre_latitude))

    document = imagerymanifest.build_manifest(
        dataset_name=name,
        bbox=bbox,
        levels=summaries,
        source={
            "description": source_description,
            "files": len(files),
            "firstFile": os.path.basename(files[0]),
            "driver": vrt_info.get("driver", "sconosciuto") if isinstance(vrt_info, dict) else "sconosciuto",
            "groundResolutionMetres": round(ground_metres, 3),
        },
        quality=quality,
        pipeline_version=PIPELINE_VERSION)
    imagerymanifest.write_manifest(output, document)

    elapsed = time.time() - started
    megabytes = stats.bytes_written / (1024 * 1024)
    report("")
    report(f"Fatto in {elapsed:.1f} s.")
    report(f"  tile scritte     : {stats.tiles_written}")
    report(f"  di cui parziali  : {stats.tiles_partial}")
    report(f"  tile vuote saltate: {stats.tiles_skipped_empty}")
    report(f"  spazio occupato  : {megabytes:.1f} MB")
    if stats.tiles_written:
        report(f"  media per tile   : {stats.bytes_written / stats.tiles_written / 1024:.1f} KB")

    return {
        "bbox": bbox,
        "max_level": max_level,
        "min_level": min_level,
        "stats": stats,
    }

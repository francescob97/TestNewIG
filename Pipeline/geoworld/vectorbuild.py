"""
Pipeline delle linee (Fase 8): da un estratto OpenStreetMap a tile vettoriali.

TRE STADI, RIAVVIABILI
----------------------
1. **estrai.** Si legge l'estratto OSM una volta sola, si classifica ogni way
   (roadclasses.py) e la si scrive nei BLOCCHI che tocca: un blocco e' una
   tile del livello minimo (il 10, ~20 km di lato). E' lo stadio lento,
   perche' il driver OSM deve ricomporre milioni di way dai loro nodi.

2. **taglia.** Ogni blocco produce tutte le sue tile, a tutti i livelli, in
   modo indipendente dagli altri: le tile dei livelli piu' fini stanno dentro
   un solo blocco. Per questo i blocchi vanno in parallelo, e un blocco gia'
   fatto non si rifa' (ogni blocco lascia un file di "fatto").

3. **indici.** Indice per livello e manifest, dalle liste dei blocchi.

PERCHE' I BLOCCHI
-----------------
Senza, per ogni tile si dovrebbe chiedere al file OSM "cosa passa di qui?": il
driver OSM non ha un indice spaziale, e ogni domanda rileggerebbe il file.
Con i blocchi lo si legge una volta, e ogni tile guarda solo il proprio blocco:
qualche migliaio di linee invece di qualche milione.

Una linea che tocca piu' blocchi finisce in tutti: i blocchi sono allargati di
un ottavo del lato (lo stesso buffer delle tile), cosi' una strada che corre
appena fuori dal blocco, ma dentro il buffer di una sua tile, c'e'.
"""

from __future__ import annotations

import glob
import json
import os
import shutil
import struct
import time
from concurrent.futures import ProcessPoolExecutor
from typing import Callable

import numpy as np

from . import osmextract, roadclasses, tiling, vectorformat
from .state import PipelineState

PIPELINE_VERSION = "1.0"

DEFAULT_MIN_LEVEL = 10
DEFAULT_MAX_LEVEL = 13

#: Record di una linea nei blocchi: classe, flag, layer, riservato,
#: larghezza in dm, numero di punti. I punti seguono in int32 da 1e-7 gradi
#: (la stessa unita' di OSM: un centimetro).
BLOCK_RECORD = struct.Struct("<BBbBHI")
COORD_SCALE = 1e7

#: Quanti byte di blocchi tenere in memoria prima di scriverli su disco.
FLUSH_BYTES = 64 * 1024 * 1024

Report = Callable[[str], None]


def _null_report(_message: str) -> None:
    pass


# =============================================================================
#  Stadio 1: estrai
# =============================================================================

def _block_name(x: int, y: int) -> str:
    return f"{x}_{y}.bin"


def extract_blocks(inputs: list[str], blocks_dir: str, block_level: int,
                   bbox: tuple[float, float, float, float] | None,
                   report: Report = _null_report) -> dict:
    """Legge gli estratti OSM e scrive le linee nei blocchi. Ritorna un riassunto."""
    if os.path.isdir(blocks_dir):
        shutil.rmtree(blocks_dir)
    os.makedirs(blocks_dir)

    span = tiling.tile_span_deg(block_level)
    margin = span / 8.0
    max_x = tiling.tiles_x(block_level) - 1
    max_y = tiling.tiles_y(block_level) - 1

    pending: dict[tuple[int, int], bytearray] = {}
    pending_bytes = 0
    class_counts: dict[int, int] = {}
    stats: dict = {}
    extent = [180.0, 90.0, -180.0, -90.0]
    total_points = 0

    def flush() -> None:
        nonlocal pending_bytes
        for (bx, by), data in pending.items():
            with open(os.path.join(blocks_dir, _block_name(bx, by)), "ab") as handle:
                handle.write(data)
        pending.clear()
        pending_bytes = 0

    for path in inputs:
        report(f"      leggo {path}")
        for classified, points in osmextract.iter_lines(path, bbox=bbox, report=report,
                                                        counts=stats):
            lon = points[:, 0]
            lat = points[:, 1]
            west, east = float(lon.min()), float(lon.max())
            south, north = float(lat.min()), float(lat.max())

            extent[0] = min(extent[0], west)
            extent[1] = min(extent[1], south)
            extent[2] = max(extent[2], east)
            extent[3] = max(extent[3], north)

            record = BLOCK_RECORD.pack(classified.class_id, classified.flags, classified.layer, 0,
                                       classified.width_dm, len(points))
            coords = np.round(points * COORD_SCALE).astype("<i4").tobytes()

            x0 = max(0, int((west - margin + 180.0) / span))
            x1 = min(max_x, int((east + margin + 180.0) / span))
            y0 = max(0, int((90.0 - north - margin) / span))
            y1 = min(max_y, int((90.0 - south + margin) / span))
            for by in range(y0, y1 + 1):
                for bx in range(x0, x1 + 1):
                    data = pending.setdefault((bx, by), bytearray())
                    data += record
                    data += coords
                    pending_bytes += len(record) + len(coords)

            class_counts[classified.class_id] = class_counts.get(classified.class_id, 0) + 1
            total_points += len(points)
            if pending_bytes > FLUSH_BYTES:
                flush()

    flush()
    blocks = sorted(os.path.basename(p) for p in glob.glob(os.path.join(blocks_dir, "*.bin")))
    return {
        "lineeLette": stats.get("lette", 0),
        "lineeDisegnate": stats.get("disegnate", 0),
        "lineeScartate": stats.get("scartate", 0),
        "punti": total_points,
        "classi": {str(k): v for k, v in sorted(class_counts.items())},
        "estensione": extent if extent[0] <= extent[2] else None,
        "blocchi": blocks,
    }


def read_block(path: str) -> list[tuple[int, int, int, int, np.ndarray]]:
    """Le linee di un blocco: (classe, flag, layer, larghezza dm, punti lon/lat)."""
    with open(path, "rb") as handle:
        blob = handle.read()
    lines = []
    offset = 0
    while offset < len(blob):
        class_id, flags, layer, _pad, width_dm, count = BLOCK_RECORD.unpack_from(blob, offset)
        offset += BLOCK_RECORD.size
        coords = np.frombuffer(blob, dtype="<i4", count=count * 2, offset=offset)
        offset += count * 8
        lines.append((class_id, flags, layer, width_dm,
                      coords.reshape(-1, 2).astype(np.float64) / COORD_SCALE))
    return lines


# =============================================================================
#  Stadio 2: taglia
# =============================================================================

def simplify_tolerance_deg(level: int, max_level: int) -> float:
    """
    Tolleranza di semplificazione al livello dato, in gradi.

    Ai livelli intermedi una tile verra' disegnata al massimo a 512 pixel prima
    che il terreno passi al livello successivo: un quarto di quel pixel e'
    invisibile. All'ultimo livello no: le sue tile servono anche ai livelli di
    terreno piu' profondi, ingrandite, e la tolleranza scende al passo di
    quantizzazione (15 cm al livello 13).
    """
    span = tiling.tile_span_deg(level)
    return span / 2048.0 if level < max_level else span / vectorformat.EXTENT


def _wkb_linestring(points: np.ndarray) -> bytes:
    return (b"\x01" + struct.pack("<II", 2, len(points))
            + np.ascontiguousarray(points, dtype="<f8").tobytes())


def _clip_and_simplify(points: np.ndarray, rect, inside: bool, tolerance: float) -> list[np.ndarray]:
    """
    Ritaglia una linea sul rettangolo (se serve) e la semplifica.

    Il ritaglio si fa solo se la linea esce dal rettangolo: e' il caso di una
    linea su qualche decina, e GEOS e' il passo piu' caro di tutta la catena.
    """
    from osgeo import ogr

    geometry = ogr.CreateGeometryFromWkb(_wkb_linestring(points))
    if not inside:
        geometry = geometry.Intersection(rect)
        if geometry is None or geometry.IsEmpty():
            return []
    if tolerance > 0.0:
        simplified = geometry.Simplify(tolerance)
        if simplified is not None and not simplified.IsEmpty():
            geometry = simplified

    parts = []
    kind = ogr.GT_Flatten(geometry.GetGeometryType())
    if kind == ogr.wkbLineString:
        parts.append(geometry)
    elif kind in (ogr.wkbMultiLineString, ogr.wkbGeometryCollection):
        for index in range(geometry.GetGeometryCount()):
            part = geometry.GetGeometryRef(index)
            if ogr.GT_Flatten(part.GetGeometryType()) == ogr.wkbLineString:
                parts.append(part)

    result = []
    for part in parts:
        for coords in osmextract._linestring_from_wkb(bytes(part.ExportToWkb(ogr.wkbNDR))):
            if len(coords) >= 2:
                result.append(coords)
    return result


def quantize(level: int, x: int, y: int, coords: np.ndarray) -> np.ndarray | None:
    """Gradi -> int16 locali, senza punti doppi consecutivi. None se degenera."""
    u, v = vectorformat.lonlat_to_local(level, x, y, coords[:, 0], coords[:, 1])
    local = np.stack([np.round(u), np.round(v)], axis=1)
    local = np.clip(local, vectorformat.INT16_MIN, vectorformat.INT16_MAX).astype(np.int16)
    keep = np.ones(len(local), dtype=bool)
    keep[1:] = np.any(local[1:] != local[:-1], axis=1)
    local = local[keep]
    return local if len(local) >= 2 else None


def cut_block(task: tuple) -> dict:
    """
    Produce tutte le tile di un blocco. Gira in un processo a parte.

    Ritorna, per livello, le voci d'indice delle tile scritte.
    """
    block_path, block_x, block_y, block_level, min_level, max_level, output = task
    from osgeo import ogr
    ogr.UseExceptions()

    lines = read_block(block_path)
    # Prima chi sta sotto: l'ordine nel file e' l'ordine di disegno.
    lines.sort(key=lambda line: roadclasses.draw_order_key(line[0], line[1], line[2]))

    count = len(lines)
    min_levels = np.array([roadclasses.BY_ID[line[0]].min_level for line in lines], dtype=np.int32)
    west = np.array([line[4][:, 0].min() for line in lines]) if count else np.zeros(0)
    east = np.array([line[4][:, 0].max() for line in lines]) if count else np.zeros(0)
    south = np.array([line[4][:, 1].min() for line in lines]) if count else np.zeros(0)
    north = np.array([line[4][:, 1].max() for line in lines]) if count else np.zeros(0)

    entries: dict[int, list[dict]] = {}
    for level in range(min_level, max_level + 1):
        division = 1 << (level - block_level)
        span = tiling.tile_span_deg(level)
        buffer_deg = span * vectorformat.BUFFER / vectorformat.EXTENT
        tolerance = simplify_tolerance_deg(level, max_level)
        wanted = min_levels <= level

        for ty in range(block_y * division, (block_y + 1) * division):
            for tx in range(block_x * division, (block_x + 1) * division):
                bounds = tiling.tile_bounds(level, tx, ty)
                bw, bs = bounds.west - buffer_deg, bounds.south - buffer_deg
                be, bn = bounds.east + buffer_deg, bounds.north + buffer_deg

                touching = wanted & (east >= bw) & (west <= be) & (north >= bs) & (south <= bn)
                indices = np.nonzero(touching)[0]
                if len(indices) == 0:
                    continue

                rect = ogr.CreateGeometryFromWkt(
                    f"POLYGON(({bw} {bs},{be} {bs},{be} {bn},{bw} {bn},{bw} {bs}))")
                features = []
                for index in indices:
                    class_id, flags, layer, width_dm, points = lines[index]
                    inside = (west[index] >= bw and east[index] <= be
                              and south[index] >= bs and north[index] <= bn)
                    for part in _clip_and_simplify(points, rect, inside, tolerance):
                        local = quantize(level, tx, ty, part)
                        if local is not None:
                            features.append(vectorformat.Feature(class_id, flags, layer,
                                                                 width_dm, local))
                if not features:
                    continue

                path = os.path.join(output, vectorformat.tile_relative_path(level, tx, ty))
                vectorformat.write_tile_atomic(path, vectorformat.encode_tile(level, tx, ty, features))
                entries.setdefault(level, []).append(
                    {"x": tx, "y": ty, "feature_count": len(features)})
    return {"block": [block_x, block_y], "levels": {str(k): v for k, v in entries.items()}}


# =============================================================================
#  Orchestrazione
# =============================================================================

def expand_inputs(patterns: list[str]) -> list[str]:
    files: list[str] = []
    for pattern in patterns:
        matched = sorted(glob.glob(pattern))
        if matched:
            files.extend(matched)
        elif os.path.exists(pattern):
            files.append(pattern)
    unique = list(dict.fromkeys(files))
    if not unique:
        raise FileNotFoundError(f"nessun file trovato per {patterns}")
    return unique


def build(*, inputs: list[str], output: str, work: str | None = None,
          name: str = "senza nome", bbox: tuple[float, float, float, float] | None = None,
          min_level: int = DEFAULT_MIN_LEVEL, max_level: int = DEFAULT_MAX_LEVEL,
          jobs: int = 1, report: Report = _null_report) -> dict:
    if not 0 <= min_level <= max_level <= tiling.MAX_SUPPORTED_LEVEL:
        raise ValueError(f"livelli {min_level}..{max_level} non validi")

    files = expand_inputs(inputs)
    work = work or os.path.join(output, "_work")
    os.makedirs(work, exist_ok=True)
    state = PipelineState(os.path.join(work, "state.json"))
    started = time.time()

    # --- 1. estrai --------------------------------------------------------
    blocks_dir = os.path.join(work, "blocchi")
    summary_path = os.path.join(work, "estratto.json")
    extract_fp = PipelineState.fingerprint(
        {"stage": "estrai", "classification": roadclasses.CLASSIFICATION_VERSION,
         "blockLevel": min_level, "bbox": bbox}, files)

    report(f"[1/3] estrazione delle linee da {len(files)} file OSM")
    summary = None
    if state.is_complete("estrai", extract_fp):
        with open(summary_path, encoding="utf-8") as handle:
            summary = json.load(handle)
        # I blocchi sono intermedi: se qualcuno li ha cancellati per fare
        # spazio, si rifa' l'estrazione invece di tagliare il vuoto.
        if not all(os.path.exists(os.path.join(blocks_dir, block)) for block in summary["blocchi"]):
            summary = None
    if summary is not None:
        report(f"      gia' fatta: {summary['lineeDisegnate']:,} linee in "
               f"{len(summary['blocchi'])} blocchi")
    else:
        summary = extract_blocks(files, blocks_dir, min_level, bbox, report)
        with open(summary_path, "w", encoding="utf-8") as handle:
            json.dump(summary, handle, indent=1)
        state.mark_complete("estrai", extract_fp, [summary_path], {})
        report(f"      {summary['lineeLette']:,} linee lette, {summary['lineeDisegnate']:,} "
               f"da disegnare in {len(summary['blocchi'])} blocchi "
               f"({time.time() - started:.0f} s)")
        for class_id, number in summary["classi"].items():
            road_class = roadclasses.BY_ID[int(class_id)]
            report(f"        {road_class.name:14s} {number:9,d}")

    # --- 2. taglia --------------------------------------------------------
    cut_fp = PipelineState.fingerprint(
        {"stage": "taglia", "extract": extract_fp, "min": min_level, "max": max_level,
         "extent": vectorformat.EXTENT, "buffer": vectorformat.BUFFER,
         "format": vectorformat.FORMAT_VERSION})
    done_dir = os.path.join(work, "fatti", cut_fp[:16])
    os.makedirs(done_dir, exist_ok=True)

    tasks = []
    for block in summary["blocchi"]:
        stem = block[:-4]
        if os.path.exists(os.path.join(done_dir, stem + ".json")):
            continue
        bx, by = (int(part) for part in stem.split("_"))
        path = os.path.join(blocks_dir, block)
        tasks.append((path, bx, by, min_level, min_level, max_level, output))
    # I blocchi piu' grossi per primi: in parallelo, l'ultimo a finire e' il
    # piu' lento, ed e' meglio che non sia anche l'ultimo a partire.
    tasks.sort(key=lambda task: -os.path.getsize(task[0]))

    already = len(summary["blocchi"]) - len(tasks)
    report(f"[2/3] taglio delle tile, livelli {min_level}..{max_level}: "
           f"{len(summary['blocchi'])} blocchi ({already} gia' fatti), {jobs} processi")
    cut_started = time.time()

    def save(result: dict) -> None:
        stem = f"{result['block'][0]}_{result['block'][1]}"
        temporary = os.path.join(done_dir, stem + ".json.tmp")
        with open(temporary, "w", encoding="utf-8") as handle:
            json.dump(result, handle)
        os.replace(temporary, os.path.join(done_dir, stem + ".json"))

    finished = 0
    if jobs <= 1 or len(tasks) <= 1:
        for task in tasks:
            save(cut_block(task))
            finished += 1
            if finished % 10 == 0 or finished == len(tasks):
                report(f"      {finished}/{len(tasks)} blocchi")
    else:
        with ProcessPoolExecutor(max_workers=jobs) as pool:
            for result in pool.map(cut_block, tasks):
                save(result)
                finished += 1
                if finished % 10 == 0 or finished == len(tasks):
                    report(f"      {finished}/{len(tasks)} blocchi "
                           f"({time.time() - cut_started:.0f} s)")

    # --- 3. indici e manifest ---------------------------------------------
    report("[3/3] indici e manifest")
    per_level: dict[int, list[vectorformat.VectorTileEntry]] = {
        level: [] for level in range(min_level, max_level + 1)}
    for block in summary["blocchi"]:
        with open(os.path.join(done_dir, block[:-4] + ".json"), encoding="utf-8") as handle:
            result = json.load(handle)
        for level, items in result["levels"].items():
            per_level[int(level)].extend(
                vectorformat.VectorTileEntry(item["x"], item["y"], item["feature_count"])
                for item in items)

    levels = []
    for level in range(min_level, max_level + 1):
        entries = per_level[level]
        vectorformat.write_level_index(output, level, entries)
        levels.append({"level": level, "tile_count": len(entries),
                       "feature_count": sum(e.feature_count for e in entries)})
        report(f"      livello {level:2d}: {len(entries):7,d} tile, "
               f"{levels[-1]['feature_count']:9,d} linee")

    extent = summary.get("estensione") or [0.0, 0.0, 0.0, 0.0]
    if bbox is not None:
        extent = [max(extent[0], bbox[0]), max(extent[1], bbox[1]),
                  min(extent[2], bbox[2]), min(extent[3], bbox[3])]
    manifest = vectorformat.build_manifest(
        dataset_name=name, bbox=tuple(extent), levels=levels,
        source={"files": [os.path.basename(f) for f in files],
                "description": "OpenStreetMap (c) contributori OpenStreetMap, licenza ODbL",
                "bbox": list(bbox) if bbox else None},
        class_counts={int(k): v for k, v in summary["classi"].items()},
        pipeline_version=PIPELINE_VERSION)
    vectorformat.write_manifest(output, manifest)

    report(f"fatto in {time.time() - started:.0f} s: {output}")
    return manifest


# =============================================================================
#  Verifica
# =============================================================================

def verify(root: str, report: Report = _null_report, sample_per_level: int = 12) -> int:
    """Rilegge un dataset vettoriale e ne controlla la coerenza. Ritorna i problemi."""
    document = vectorformat.read_manifest(root)
    if document.get("datasetKind") != "vector":
        report("ERRORE: il manifest non dichiara un dataset vettoriale")
        return 1

    problems = 0
    report(f"dataset  : {document['datasetName']}")
    for level_info in document["levels"]:
        level = level_info["level"]
        entries = vectorformat.read_level_index(root, level)
        if len(entries) != level_info["tile_count"]:
            report(f"  livello {level}: indice con {len(entries)} voci, manifest {level_info['tile_count']}")
            problems += 1
            continue

        bad = 0
        sample = entries[:: max(1, len(entries) // sample_per_level)][:sample_per_level]
        for entry in sample:
            path = os.path.join(root, vectorformat.tile_relative_path(level, entry.x, entry.y))
            try:
                tile = vectorformat.read_tile(path)
            except Exception as error:                  # noqa: BLE001
                report(f"  livello {level}: {entry.x},{entry.y} illeggibile: {error}")
                bad += 1
                continue
            if (tile.level, tile.x, tile.y) != (level, entry.x, entry.y):
                report(f"  livello {level}: {entry.x},{entry.y} dichiara {tile.level}/{tile.x}/{tile.y}")
                bad += 1
            if len(tile.features) != entry.feature_count:
                report(f"  livello {level}: {entry.x},{entry.y} ha {len(tile.features)} linee, "
                       f"l'indice {entry.feature_count}")
                bad += 1
            for feature in tile.features:
                if feature.class_id not in roadclasses.BY_ID:
                    report(f"  livello {level}: classe {feature.class_id} sconosciuta")
                    bad += 1
                    break
                low = -vectorformat.BUFFER - 1
                high = vectorformat.EXTENT + vectorformat.BUFFER + 1
                if feature.points.min() < low or feature.points.max() > high:
                    report(f"  livello {level}: {entry.x},{entry.y} ha punti oltre il buffer")
                    bad += 1
                    break
                if roadclasses.BY_ID[feature.class_id].min_level > level:
                    report(f"  livello {level}: classe {feature.class_id} sotto il suo livello minimo")
                    bad += 1
                    break

        report(f"  livello {level:2d}: {len(entries):7,d} tile, {level_info['feature_count']:9,d} linee"
               f"   {'ok' if bad == 0 else f'FALLITO ({bad})'}")
        problems += bad

    report("")
    report("TUTTO A POSTO" if problems == 0 else f"{problems} PROBLEMI")
    return problems

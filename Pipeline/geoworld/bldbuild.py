"""
Pipeline degli edifici: da un estratto OpenStreetMap a tile .gbd.

LA SORGENTE
-----------
Le sagome degli edifici di OpenStreetMap: lo stesso estratto .osm.pbf delle
strade e dell'acqua (geoworld fetch-osm). In Italia sono quasi complete: molte
regioni hanno importato la loro cartografia. Dati (c) contributori di
OpenStreetMap, licenza ODbL.

Si leggono:
  - le way chiuse e i multipoligoni con building=* (il driver OSM di GDAL li
    mette nel layer "multipolygons", con i cortili come buchi);
  - le PARTI (building:part=*, lo schema "Simple 3D Buildings"): un campanile
    alto sopra una chiesa bassa, i corpi di altezze diverse di un palazzo. Le
    way chiuse con solo building:part il driver le mette fra le "lines": si
    richiudono qui in poligoni.
Un edificio che ha delle parti NON si disegna: si disegnano le parti (e' la
regola dello schema). Si riconosce perche' il centro di qualche parte gli
cade dentro. Resta pero' nella tile, marcato FLAG_OUTLINE, con le sue parti
accanto: e' lui che porta il tag wikidata dei monumenti.

COME
----
1. Una passata sul file: ogni edificio nell'area diventa un record (classe,
   altezza, tetto, colori: bldclasses.py) e va nel BLOCCO (tile del livello
   10, ~20 x 14 km a 45 gradi) del suo centro, in _work/blocks/. Se la passata e' gia'
   finita (_work/extract_15.done) non si rifa'.
2. Blocco per blocco: i contorni che hanno delle parti si marcano; ogni edificio
   nella tile (livello 15) del suo centro, le parti in quella del loro
   contorno; le tile si scrivono. I blocchi fatti si segnano: rilanciando,
   riparte da dove era.
3. Indice e manifest, dalle tile che ci sono.
"""

from __future__ import annotations

import dataclasses
import json
import math
import os
import pickle
import shutil
import time
from collections import Counter
from typing import Callable, Iterator

import numpy as np

from . import bldclasses, bldformat, tiling
from . import manifest as terrain_manifest
from .osmextract import parse_other_tags

PIPELINE_VERSION = "1.0"
DEFAULT_LEVEL = 15
BLOCK_LEVEL = 10
#: Oltre tanti vertici un anello si semplifica (0,3 m): un palazzo non ne ha bisogno.
MAX_RING_VERTICES = 1000

TAGS = ("building", "building:part", "height", "building:height", "min_height", "building:levels",
        "building:min_level", "roof:shape", "roof:height", "roof:levels", "building:colour", "colour",
        "roof:colour", "amenity", "man_made", "location", "wikidata", "name")

Report = Callable[[str], None]


def _gdal():
    from osgeo import gdal, ogr
    gdal.UseExceptions()
    return gdal, ogr


def _ring_points(ring) -> np.ndarray:
    points = np.array(ring.GetPoints(), dtype=np.float64)[:, :2]
    if len(points) >= 2 and np.allclose(points[0], points[-1]):
        points = points[:-1]
    # Via i vertici ripetuti (capita negli import).
    if len(points) >= 2:
        keep = np.ones(len(points), dtype=bool)
        keep[1:] = np.any(np.abs(np.diff(points, axis=0)) > 1e-9, axis=1)
        points = points[keep]
    return points


def _signed_area(points: np.ndarray, lat0: float) -> float:
    """Area con il segno (m^2, positiva = antioraria vista dall'alto, x est y nord)."""
    x = points[:, 0] * 111320.0 * math.cos(math.radians(lat0))
    y = points[:, 1] * 111132.0
    return 0.5 * float(np.dot(x, np.roll(y, -1)) - np.dot(np.roll(x, -1), y))


def _polygons(geometry) -> list[list[np.ndarray]]:
    """I poligoni di una geometria OGR: per ognuno [contorno, buchi...] in lon/lat, orientati."""
    _, ogr = _gdal()
    kind = ogr.GT_Flatten(geometry.GetGeometryType())
    if kind == ogr.wkbPolygon:
        polygons = [geometry]
    elif kind == ogr.wkbMultiPolygon:
        polygons = [geometry.GetGeometryRef(i) for i in range(geometry.GetGeometryCount())]
    elif kind in (ogr.wkbLineString,):
        points = np.array(geometry.GetPoints(), dtype=np.float64)[:, :2]
        if len(points) < 4 or not np.allclose(points[0], points[-1]):
            return []
        ring = ogr.Geometry(ogr.wkbLinearRing)
        for lon, lat in points:
            ring.AddPoint_2D(float(lon), float(lat))
        polygon = ogr.Geometry(ogr.wkbPolygon)
        polygon.AddGeometry(ring)
        polygons = [polygon]
    else:
        return []

    result = []
    for polygon in polygons:
        if polygon is None or polygon.GetGeometryCount() == 0:
            continue
        rings = []
        for index in range(polygon.GetGeometryCount()):
            points = _ring_points(polygon.GetGeometryRef(index))
            if len(points) > MAX_RING_VERTICES:
                simplified = polygon.GetGeometryRef(index).Clone().SimplifyPreserveTopology(0.3 / 111132.0)
                points = _ring_points(simplified) if simplified is not None else points
            if len(points) < 3:
                if index == 0:
                    break
                continue
            area = _signed_area(points, float(points[0, 1]))
            if abs(area) < 1.0:            # meno di un metro quadro: un errore, non un edificio
                if index == 0:
                    break
                continue
            # Il contorno antiorario, i cortili orari.
            if (index == 0) != (area > 0.0):
                points = points[::-1]
            rings.append(points)
        if rings and len(rings[0]) >= 3:
            result.append(rings)
    return result


def _centroid(points: np.ndarray) -> tuple[float, float]:
    """Il baricentro dell'area di un anello (lon, lat); la media dei vertici se e' degenere."""
    x, y = points[:, 0], points[:, 1]
    cross = x * np.roll(y, -1) - np.roll(x, -1) * y
    area = cross.sum() * 0.5
    if abs(area) < 1e-14:
        return float(x.mean()), float(y.mean())
    cx = ((x + np.roll(x, -1)) * cross).sum() / (6.0 * area)
    cy = ((y + np.roll(y, -1)) * cross).sum() / (6.0 * area)
    return float(cx), float(cy)


def _inside(points: np.ndarray, lon: float, lat: float) -> bool:
    """Il punto sta nell'anello? (raggio verso est, pari/dispari)"""
    x, y = points[:, 0], points[:, 1]
    x2, y2 = np.roll(x, -1), np.roll(y, -1)
    crosses = ((y > lat) != (y2 > lat))
    with np.errstate(divide="ignore", invalid="ignore"):
        at = x + (lat - y) * (x2 - x) / (y2 - y)
    return bool(np.count_nonzero(crosses & (lon < at)) % 2 == 1)


def _wikidata(text: str | None) -> int:
    if not text or not text.upper().startswith("Q"):
        return 0
    digits = text[1:].split(";")[0].strip()
    return int(digits) if digits.isdigit() and int(digits) < 2 ** 32 else 0


def iter_osm_buildings(path: str, bbox, report: Report, counts: Counter) -> Iterator[tuple[dict, int, list]]:
    """(tag, id OSM impacchettato, poligoni) per ogni edificio o parte nell'area."""
    gdal, _ = _gdal()
    gdal.SetConfigOption("OSM_MAX_TMPFILE_SIZE", "1024")
    source = gdal.OpenEx(path, gdal.OF_VECTOR, open_options=["INTERLEAVED_READING=YES"])
    if source is None:
        raise RuntimeError(f"GDAL non riesce ad aprire {path}")
    if source.GetDriver().GetDescription() != "OSM":
        raise RuntimeError(f"{path} non e' un estratto OpenStreetMap (.osm.pbf o .osm)")
    # Vedi osmextract.iter_lines: solo i layer che servono, lettura dal dataset.
    source.ExecuteSQL("SET interest_layers = lines,multipolygons")

    names = {}
    for layer_name in ("lines", "multipolygons"):
        layer = source.GetLayerByName(layer_name)
        if layer is None:
            continue
        definition = layer.GetLayerDefn()
        fields = {definition.GetFieldDefn(i).GetName() for i in range(definition.GetFieldCount())}
        names[layer_name] = fields
        if bbox is not None:
            layer.SetSpatialFilterRect(*bbox)
        # Il filtro dentro GDAL, in C: le strade, i confini e i campi non
        # arrivano nemmeno al Python.
        clauses = []
        if "building" in fields:
            clauses.append("building IS NOT NULL")
        if "other_tags" in fields:
            clauses.append("other_tags LIKE '%\"building:part\"=>%'")
            if "building" not in fields:
                clauses.append("other_tags LIKE '%\"building\"=>%'")
        if clauses:
            layer.SetAttributeFilter(" OR ".join(clauses))

    while True:
        feature, owner = source.GetNextFeature(include_layer=True)
        if feature is None:
            break
        if owner is None or owner.GetName() not in names:
            continue
        layer_name = owner.GetName()
        fields = names[layer_name]
        counts["lette"] += 1
        if counts["lette"] % 200000 == 0:
            report(f"      {counts['lette']:,} oggetti letti")
        tags = parse_other_tags(feature.GetField("other_tags")) if "other_tags" in fields else {}
        for tag in TAGS:
            if tag in fields:
                value = feature.GetField(tag)
                if value is not None:
                    tags[tag] = str(value)
        if "building" not in tags and "building:part" not in tags:
            continue
        geometry = feature.GetGeometryRef()
        if geometry is None:
            continue
        # L'id: le way chiuse nei multipoligoni hanno osm_way_id, le relazioni osm_id.
        osm_kind, osm_number = bldformat.OSM_WAY, 0
        if layer_name == "multipolygons":
            if "osm_way_id" in fields and feature.GetField("osm_way_id"):
                osm_number = int(feature.GetField("osm_way_id"))
            elif "osm_id" in fields and feature.GetField("osm_id"):
                osm_kind, osm_number = bldformat.OSM_RELATION, int(feature.GetField("osm_id"))
        elif "osm_id" in fields and feature.GetField("osm_id"):
            osm_number = int(feature.GetField("osm_id"))
        polygons = _polygons(geometry.Clone())
        if not polygons:
            counts["sagome rotte"] += 1
            continue
        yield tags, bldformat.pack_osm_id(osm_kind, osm_number), polygons


def _block_path(work: str, block_x: int, block_y: int) -> str:
    return os.path.join(work, "blocks", f"{block_x}_{block_y}.pkl")


def extract(osm_paths: list[str], bbox, level: int, block_level: int, work: str, report: Report) -> Counter:
    """La passata sul file: un record per edificio (o parte), nel file del suo blocco."""
    west, south, east, north = bbox
    blocks_dir = os.path.join(work, "blocks")
    if os.path.isdir(blocks_dir):
        shutil.rmtree(blocks_dir)
    os.makedirs(blocks_dir)
    handles: dict = {}
    counts: Counter = Counter()
    try:
        for path in osm_paths:
            report(f"      {path}")
            for tags, osm_id, polygons in iter_osm_buildings(path, bbox, report, counts):
                is_part = "building:part" in tags and "building" not in tags
                item = bldclasses.classify(tags)
                if item is None:
                    counts["non edifici"] += 1
                    continue
                guess = bldclasses.estimate(tags, item, is_part=is_part)
                for rings in polygons:
                    lon, lat = _centroid(rings[0])
                    if not (west <= lon < east and south <= lat < north):
                        continue
                    tile_x, tile_y = tiling.tile_for_lonlat(level, lon, lat)
                    shift = level - block_level
                    key = (tile_x >> shift, tile_y >> shift)
                    handle = handles.get(key)
                    if handle is None:
                        handle = open(_block_path(work, *key), "ab")
                        handles[key] = handle
                    pickle.dump({"tile": (tile_x, tile_y), "centre": (lon, lat), "part": is_part,
                                 "class": item.id, "estimate": guess, "osm_id": osm_id,
                                 "wikidata": _wikidata(tags.get("wikidata")), "rings": rings}, handle)
                    counts["parti" if is_part else "edifici"] += 1
                    counts[item.name] += 1
    finally:
        for handle in handles.values():
            handle.close()
    return counts


def _read_block(path: str) -> list[dict]:
    records = []
    with open(path, "rb") as handle:
        while True:
            try:
                records.append(pickle.load(handle))
            except EOFError:
                break
    return records


def _mark_outlines_with_parts(records: list[dict]) -> tuple[list[dict], int]:
    """
    Gli edifici che hanno delle parti non si disegnano: si disegnano le parti
    (e' la regola dello schema Simple 3D Buildings). Il contorno pero' resta,
    marcato FLAG_OUTLINE: e' lui che porta il nome e il tag wikidata, e un
    monumento della libreria lo cerca li'. Le sue parti vanno nella SUA tile:
    cosi' il runtime, quando lo sostituisce con un modello, le ha tutte sotto
    mano e le toglie.
    """
    parts = [index for index, record in enumerate(records) if record["part"]]
    if not parts:
        return records, 0
    grid: dict = {}
    for index in parts:
        lon, lat = records[index]["centre"]
        grid.setdefault((int(lon * 1000), int(lat * 1000)), []).append(index)
    outlines = 0
    for record in records:
        if record["part"]:
            continue
        outer = record["rings"][0]
        lon_min, lat_min = outer.min(axis=0)
        lon_max, lat_max = outer.max(axis=0)
        inside = []
        for gx in range(int(lon_min * 1000), int(lon_max * 1000) + 1):
            for gy in range(int(lat_min * 1000), int(lat_max * 1000) + 1):
                for index in grid.get((gx, gy), ()):
                    lon, lat = records[index]["centre"]
                    if lon_min <= lon <= lon_max and lat_min <= lat <= lat_max and _inside(outer, lon, lat):
                        inside.append(index)
        if not inside:
            continue
        outlines += 1
        guess: bldclasses.Estimate = record["estimate"]
        record["estimate"] = dataclasses.replace(guess, flags=guess.flags | bldclasses.FLAG_OUTLINE)
        for index in inside:
            records[index]["tile"] = record["tile"]
    return records, outlines


def _to_building(record: dict, level: int) -> bldformat.Building | None:
    tile_x, tile_y = record["tile"]
    rings = []
    for points in record["rings"]:
        quantized = bldformat.quantize(level, tile_x, tile_y, points[:, 0], points[:, 1])
        if quantized is None:
            if not rings:
                return None
            continue
        # Due vertici che dopo la quantizzazione coincidono: uno basta.
        keep = np.ones(len(quantized), dtype=bool)
        keep[1:] = np.any(np.diff(quantized.astype(np.int32), axis=0) != 0, axis=1)
        quantized = quantized[keep]
        if len(quantized) >= 2 and np.array_equal(quantized[0], quantized[-1]):
            quantized = quantized[:-1]
        if len(quantized) >= 3:
            rings.append(quantized)
        elif not rings:
            return None
    guess: bldclasses.Estimate = record["estimate"]
    return bldformat.Building(
        building_class=record["class"], roof=guess.roof_shape, flags=guess.flags, levels=guess.levels,
        height_dm=int(round(guess.height_m * 10)), min_height_dm=int(round(guess.min_height_m * 10)),
        roof_height_dm=int(round(guess.roof_height_m * 10)), wall_colour=guess.wall_colour,
        roof_colour=guess.roof_colour, osm_id=record["osm_id"], wikidata=record["wikidata"], rings=rings)


def _read_done(path: str) -> set[str]:
    try:
        with open(path, "r", encoding="utf-8") as handle:
            return set(json.load(handle))
    except (FileNotFoundError, ValueError):
        return set()


def _write_done(path: str, done: set[str]) -> None:
    temporary = f"{path}.tmp"
    with open(temporary, "w", encoding="utf-8") as handle:
        json.dump(sorted(done), handle)
    os.replace(temporary, path)


def build(*, output: str, osm_paths: list[str], terrain_root: str | None = None, bbox: tuple | None = None,
          name: str = "edifici", level: int = DEFAULT_LEVEL, block_level: int = BLOCK_LEVEL,
          force: bool = False, report: Report = print) -> dict:
    if block_level > level:
        raise ValueError(f"il livello dei blocchi ({block_level}) deve stare sopra quello delle tile ({level})")
    for path in osm_paths:
        if not os.path.exists(path):
            raise FileNotFoundError(f"non trovo l'estratto OSM {path} (geoworld fetch-osm lo scarica)")

    # --- 1. L'area ---------------------------------------------------------------
    if bbox is None:
        if not terrain_root:
            raise ValueError("serve --terrain (il dataset del terreno) o --bbox")
        box = terrain_manifest.read_manifest(terrain_root)["boundingBox"]
        bbox = (box["west"], box["south"], box["east"], box["north"])
    west, south, east, north = bbox
    report(f"[1/4] area: ovest {west:.4f}, sud {south:.4f}, est {east:.4f}, nord {north:.4f}")

    # --- 2. La passata sul file OSM ------------------------------------------------------
    work = os.path.join(output, "_work")
    extracted = os.path.join(work, f"extract_{level}.done")
    started = time.time()
    if force or not os.path.exists(extracted):
        report("[2/4] sagome da OpenStreetMap (una passata sul file: per una regione qualche minuto)")
        counts = extract(osm_paths, bbox, level, block_level, work, report)
        with open(extracted, "w", encoding="utf-8") as handle:
            json.dump(dict(counts), handle)
        done_path = os.path.join(work, f"blocks_{level}.json")
        if os.path.exists(done_path):
            os.remove(done_path)
    else:
        with open(extracted, "r", encoding="utf-8") as handle:
            counts = Counter(json.load(handle))
        report("[2/4] sagome gia' estratte (si salta: --force per rifarle)")
    report(f"      {counts.get('edifici', 0):,} edifici e {counts.get('parti', 0):,} parti "
           f"({time.time() - started:.0f}s)")

    # --- 3. Blocco per blocco, le tile ------------------------------------------------------
    done_path = os.path.join(work, f"blocks_{level}.json")
    done = set() if force else _read_done(done_path)
    blocks = sorted(name for name in os.listdir(os.path.join(work, "blocks")) if name.endswith(".pkl"))
    report(f"[3/4] {len(blocks)} blocchi da ~{tiling.tile_span_deg(block_level) * 111:.0f} km")
    written = outlines = rejected = 0
    for number, block in enumerate(blocks, start=1):
        if block in done:
            continue
        records = _read_block(os.path.join(work, "blocks", block))
        records, marked = _mark_outlines_with_parts(records)
        outlines += marked
        tiles: dict = {}
        for record in records:
            building = _to_building(record, level)
            if building is None:
                rejected += 1
                continue
            tiles.setdefault(record["tile"], []).append(building)
        for (tile_x, tile_y), buildings in tiles.items():
            tile = bldformat.BuildingTile(level, tile_x, tile_y, buildings)
            bldformat.write_file_atomic(bldformat.tile_path(output, level, tile_x, tile_y), bldformat.encode_tile(tile))
            written += 1
        done.add(block)
        _write_done(done_path, done)
        report(f"      blocco {number}/{len(blocks)}: {len(tiles)} tile, {len(records)} edifici")
    if outlines:
        report(f"      {outlines} contorni con le parti (building:part): non si disegnano, restano per i monumenti")
    if rejected:
        report(f"      {rejected} edifici scartati: sagome troppo grandi o rotte")

    # --- 4. Indice e manifest ----------------------------------------------------------------
    report("[4/4] indice e manifest")
    entries, by_class, measured, total = [], Counter(), Counter(), 0
    names_by_id = {item.id: item.name for item in bldclasses.CLASSES}
    level_root = os.path.join(output, str(level))
    for directory, _, files in os.walk(level_root):
        for file_name in files:
            if not file_name.endswith(bldformat.TILE_EXTENSION):
                continue
            path = os.path.join(directory, file_name)
            tile = bldformat.read_tile(path)
            entries.append(bldformat.BuildingTileEntry(tile.x, tile.y, len(tile.buildings), os.path.getsize(path)))
            for building in tile.buildings:
                if building.flags & bldclasses.FLAG_OUTLINE:
                    continue    # non si disegna: si disegnano le sue parti
                total += 1
                by_class[names_by_id.get(building.building_class, "Other")] += 1
                if building.flags & bldclasses.FLAG_HEIGHT_MEASURED:
                    measured["altezza"] += 1
                elif building.flags & bldclasses.FLAG_LEVELS_MEASURED:
                    measured["piani"] += 1
                else:
                    measured["stimata"] += 1
    bldformat.write_index(output, level, entries)
    source = {"name": "OpenStreetMap", "license": "ODbL 1.0",
              "attribution": "(c) contributori di OpenStreetMap, https://www.openstreetmap.org/copyright",
              "files": [os.path.basename(path) for path in osm_paths]}
    bldformat.write_manifest(output, bldformat.build_manifest(
        dataset_name=name, bbox=bbox, level=level, tile_count=len(entries), building_count=total,
        classes=dict(by_class), measured=dict(measured), source=source, pipeline_version=PIPELINE_VERSION))
    share = 100.0 * (measured["altezza"] + measured["piani"]) / max(1, total)
    report(f"      {len(entries)} tile, {total:,} edifici; altezza o piani da OSM per il {share:.0f}%, "
           f"il resto stimato dalla classe")
    return {"tiles": len(entries), "buildings": total, "by_class": dict(by_class), "measured": dict(measured),
            "written": written, "outlines": outlines}


def verify(root: str, report: Report = print) -> int:
    """Controlla un dataset: manifest, indice, ogni tile (chiave, dimensione, anelli). Ritorna i problemi."""
    manifest = bldformat.read_manifest(root)
    level = int(manifest["level"])
    entries = bldformat.read_index(root, level)
    problems = 0
    for entry in entries:
        path = bldformat.tile_path(root, level, entry.x, entry.y)
        try:
            tile = bldformat.read_tile(path)
        except (OSError, ValueError) as error:
            report(f"  {path}: {error}")
            problems += 1
            continue
        if (tile.level, tile.x, tile.y) != (level, entry.x, entry.y):
            report(f"  {path}: la tile dichiara {tile.level}/{tile.x}/{tile.y}")
            problems += 1
        if os.path.getsize(path) != entry.size or len(tile.buildings) != entry.count:
            report(f"  {path}: dimensione o numero di edifici diversi dall'indice")
            problems += 1
        for building in tile.buildings:
            if not building.rings or len(building.rings[0]) < 3 or building.building_class >= len(bldclasses.CLASSES):
                report(f"  {path}: un edificio senza contorno o di classe sconosciuta")
                problems += 1
                break
    report(f"{len(entries)} tile al livello {level}: {problems} problemi")
    return problems

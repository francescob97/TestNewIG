"""
Lettura delle linee di OpenStreetMap (.osm.pbf o .osm) attraverso GDAL.

PERCHE' GDAL E NON UNA LIBRERIA OSM
-----------------------------------
GDAL ha un driver OSM che legge sia il formato binario .pbf (quello che si
scarica da Geofabrik) sia l'XML .osm, e ricompone da solo le way a partire dai
nodi. E' gia' nell'ambiente: nessuna dipendenza nuova, nessuna installazione,
e lo stesso principio del resto della pipeline (tutto passa da GDAL).

Il driver espone cinque layer; qui serve solo `lines`, cioe' le way aperte:
strade, ferrovie, piste. Le aree (piazze, laghi, boschi) stanno in
`multipolygons` e arriveranno con le fasi successive.

I TAG
-----
Il driver mette alcuni tag in colonne proprie (`highway`, `railway`, ...) e
tutti gli altri in una colonna `other_tags`, scritta cosi':

    "bridge"=>"yes","layer"=>"1","lanes"=>"2"

Quali tag hanno una colonna propria dipende dalla versione di GDAL e dal suo
file osmconf.ini. Per non dipendere da nessuno dei due, si leggono ENTRAMBE le
fonti e si fondono in un dizionario solo.
"""

from __future__ import annotations

import re
import struct
from typing import Callable, Iterator

import numpy as np

from . import roadclasses

_OTHER_TAG = re.compile(r'"((?:[^"\\]|\\.)*)"=>"((?:[^"\\]|\\.)*)"')

#: Tag che servono alla classificazione. Solo questi si leggono dalle colonne.
USEFUL_TAGS = ("highway", "railway", "aeroway", "service", "tunnel", "covered",
               "bridge", "layer", "width", "lanes", "surface", "tracktype",
               "area", "indoor")


def parse_other_tags(text: str | None) -> dict[str, str]:
    """La colonna other_tags del driver OSM come dizionario."""
    if not text:
        return {}
    return {key.replace('\\"', '"'): value.replace('\\"', '"')
            for key, value in _OTHER_TAG.findall(text)}


def _attribute_filter(field_names: set[str]) -> str:
    """
    Il filtro da dare al layer, perche' le way inutili (confini, linee
    elettriche, recinzioni...) vengano scartate dentro GDAL, in C, senza
    passare dal Python. Sull'Italia sono la maggioranza delle linee.
    """
    clauses = []
    for tag in ("highway", "railway", "aeroway"):
        if tag in field_names:
            clauses.append(f"{tag} IS NOT NULL")
        elif "other_tags" in field_names:
            clauses.append(f"other_tags LIKE '%\"{tag}\"=>%'")
    return " OR ".join(clauses)


def _linestring_from_wkb(blob: bytes) -> list[np.ndarray]:
    """
    Le coordinate di una LineString o MultiLineString in WKB, come array (N, 2).

    Leggere il WKB con numpy invece di chiedere i punti uno per uno a OGR e'
    cio' che rende sopportabile l'Italia intera: milioni di linee, decine di
    milioni di punti.
    """
    order = "<" if blob[0] == 1 else ">"
    kind = struct.unpack_from(order + "I", blob, 1)[0] & 0xFF   # niente Z/M
    if kind == 2:                                   # LineString
        count = struct.unpack_from(order + "I", blob, 5)[0]
        return [np.frombuffer(blob, dtype=order + "f8", count=count * 2, offset=9).reshape(-1, 2)]
    if kind == 5:                                   # MultiLineString
        parts = []
        parts_count = struct.unpack_from(order + "I", blob, 5)[0]
        offset = 9
        for _ in range(parts_count):
            part_order = "<" if blob[offset] == 1 else ">"
            count = struct.unpack_from(part_order + "I", blob, offset + 5)[0]
            parts.append(np.frombuffer(blob, dtype=part_order + "f8", count=count * 2,
                                       offset=offset + 9).reshape(-1, 2))
            offset += 9 + count * 16
        return parts
    return []


def iter_lines(path: str, bbox: tuple[float, float, float, float] | None = None,
               report: Callable[[str], None] | None = None,
               counts: dict | None = None) -> Iterator[tuple[roadclasses.Classified, np.ndarray]]:
    """
    Le linee da disegnare, gia' classificate: (classificazione, punti lon/lat).

    `counts`, se dato, riceve le statistiche: linee lette, scartate e perche'.
    """
    from osgeo import gdal, ogr

    gdal.UseExceptions()
    # Il driver tiene in memoria gli indici dei nodi fino a questa soglia, poi
    # passa al disco. 100 MB (il default) su un file regionale vuol dire
    # lavorare quasi sempre su disco.
    gdal.SetConfigOption("OSM_MAX_TMPFILE_SIZE", "1024")

    source = gdal.OpenEx(path, gdal.OF_VECTOR)
    if source is None:
        raise RuntimeError(f"GDAL non riesce ad aprire {path}")
    layer = source.GetLayerByName("lines")
    if layer is None:
        raise RuntimeError(f"{path}: nessun layer 'lines'. E' davvero un file OSM?")

    definition = layer.GetLayerDefn()
    field_names = {definition.GetFieldDefn(i).GetName()
                   for i in range(definition.GetFieldCount())}
    column_tags = [tag for tag in USEFUL_TAGS if tag in field_names]
    has_other = "other_tags" in field_names

    where = _attribute_filter(field_names)
    if where:
        layer.SetAttributeFilter(where)
    if bbox is not None:
        layer.SetSpatialFilterRect(*bbox)

    stats = counts if counts is not None else {}
    stats.setdefault("lette", 0)
    stats.setdefault("disegnate", 0)
    stats.setdefault("scartate", 0)

    feature = layer.GetNextFeature()
    while feature is not None:
        stats["lette"] += 1
        if report and stats["lette"] % 200000 == 0:
            report(f"      {stats['lette']:,} linee lette, {stats['disegnate']:,} da disegnare")

        tags = parse_other_tags(feature.GetField("other_tags")) if has_other else {}
        for tag in column_tags:
            value = feature.GetField(tag)
            if value is not None:
                tags[tag] = value

        classified = roadclasses.classify(tags)
        geometry = feature.GetGeometryRef()
        if classified is None or geometry is None:
            stats["scartate"] += 1
        else:
            geometry.FlattenTo2D()
            for part in _linestring_from_wkb(bytes(geometry.ExportToWkb(ogr.wkbNDR))):
                if len(part) >= 2:
                    stats["disegnate"] += 1
                    yield classified, part
        feature = layer.GetNextFeature()

"""
Test della pipeline delle linee (Fase 8): classificazione, formato, taglio.

Il taglio vero gira su un estratto OSM SINTETICO scritto qui (XML .osm, che il
driver GDAL legge come il .pbf): da questo ambiente Geofabrik non e'
raggiungibile, e comunque un file costruito apposta permette di sapere
esattamente cosa deve uscire.
"""
import os
import re
import sys
import tempfile
import unittest

import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from geoworld import roadclasses, tiling, vectorformat  # noqa: E402

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
CLASSES_HEADER = os.path.join(REPO, "Plugins", "GeoWorld", "Source", "GeoTiles", "Public",
                              "Tiles", "VectorClasses.h")
FIXTURE = os.path.join(REPO, "Plugins", "GeoWorld", "Source", "GeoTiles", "TestData",
                       "vector_fixture.gvt")


def _gdal_with_osm():
    try:
        from osgeo import gdal, ogr
    except Exception:                                   # noqa: BLE001
        return False
    if gdal.GetDriverByName("OSM") is None:
        return False
    line = ogr.CreateGeometryFromWkt("LINESTRING(0 0,2 2)")
    box = ogr.CreateGeometryFromWkt("POLYGON((0 0,1 0,1 1,0 1,0 0))")
    return line.Intersection(box) is not None


HAS_GDAL_OSM = _gdal_with_osm()


class Classification(unittest.TestCase):

    def classify(self, **tags):
        return roadclasses.classify({k.replace("__", ":"): v for k, v in tags.items()})

    def test_motorway_width_from_lanes(self):
        result = self.classify(highway="motorway", lanes="3")
        self.assertEqual(result.class_id, roadclasses.BY_NAME["motorway"].id)
        # 3 corsie da 3,25 m piu' la banchina di 1,5 m.
        self.assertAlmostEqual(result.width_m, 3 * 3.25 + 1.5)
        self.assertEqual(result.width_dm, 113)

    def test_explicit_width_wins(self):
        self.assertAlmostEqual(self.classify(highway="primary", width="9,5 m", lanes="2").width_m, 9.5)
        # I piedi non si interpretano: valore tipico della classe.
        self.assertAlmostEqual(self.classify(highway="primary", width="24'").width_m,
                               roadclasses.BY_NAME["primary"].default_width_m)

    def test_link_is_narrow_and_flagged(self):
        result = self.classify(highway="motorway_link")
        self.assertEqual(result.class_id, roadclasses.BY_NAME["motorway"].id)
        self.assertTrue(result.flags & roadclasses.FLAG_LINK)
        self.assertAlmostEqual(result.width_m, 6.0)

    def test_invisible_things_are_dropped(self):
        self.assertIsNone(self.classify(highway="residential", tunnel="yes"))
        self.assertIsNone(self.classify(highway="residential", tunnel="building_passage"))
        self.assertIsNone(self.classify(highway="pedestrian", area="yes"))
        self.assertIsNone(self.classify(highway="proposed"))
        self.assertIsNone(self.classify(highway="construction"))
        self.assertIsNone(self.classify(railway="abandoned"))
        self.assertIsNone(self.classify(railway="platform"))
        self.assertIsNone(self.classify(power="line"))
        self.assertIsNone(self.classify(highway="corridor"))

    def test_bridge_and_layer(self):
        result = self.classify(highway="secondary", bridge="viaduct", layer="2")
        self.assertTrue(result.flags & roadclasses.FLAG_BRIDGE)
        self.assertEqual(result.layer, 2)
        self.assertEqual(self.classify(highway="secondary", bridge="no").flags, 0)
        self.assertEqual(self.classify(highway="secondary", layer="banana").layer, 0)

    def test_unpaved(self):
        self.assertTrue(self.classify(highway="track").flags & roadclasses.FLAG_UNPAVED)
        self.assertFalse(self.classify(highway="track", tracktype="grade1").flags & roadclasses.FLAG_UNPAVED)
        self.assertFalse(self.classify(highway="track", surface="asphalt").flags & roadclasses.FLAG_UNPAVED)
        self.assertTrue(self.classify(highway="residential", surface="gravel").flags & roadclasses.FLAG_UNPAVED)
        self.assertFalse(self.classify(highway="footway").flags & roadclasses.FLAG_UNPAVED)
        self.assertTrue(self.classify(highway="path").flags & roadclasses.FLAG_UNPAVED)

    def test_railways(self):
        self.assertEqual(self.classify(railway="rail").class_id, roadclasses.BY_NAME["rail"].id)
        self.assertEqual(self.classify(railway="rail", service="yard").class_id,
                         roadclasses.BY_NAME["rail_service"].id)
        self.assertEqual(self.classify(railway="tram").class_id, roadclasses.BY_NAME["light_rail"].id)
        # Una metropolitana in galleria non si vede; all'aperto si'.
        self.assertIsNone(self.classify(railway="subway", tunnel="yes"))
        self.assertIsNotNone(self.classify(railway="subway"))

    def test_runway_width(self):
        result = self.classify(aeroway="runway", width="45")
        self.assertEqual(result.class_id, roadclasses.BY_NAME["runway"].id)
        self.assertAlmostEqual(result.width_m, 45.0)
        self.assertAlmostEqual(self.classify(aeroway="runway").width_m, 45.0)

    def test_width_is_clamped(self):
        self.assertEqual(self.classify(highway="primary", width="900").width_m, roadclasses.MAX_WIDTH_M)
        self.assertEqual(self.classify(highway="path", width="0.2").width_m, roadclasses.MIN_WIDTH_M)

    def test_draw_order(self):
        motorway = roadclasses.BY_NAME["motorway"].id
        residential = roadclasses.BY_NAME["residential"].id
        # A parita' di layer l'autostrada sta sopra...
        self.assertLess(roadclasses.draw_order_key(residential, 0, 0),
                        roadclasses.draw_order_key(motorway, 0, 0))
        # ...ma un cavalcavia (layer 1) sta sopra l'autostrada.
        self.assertGreater(roadclasses.draw_order_key(residential, roadclasses.FLAG_BRIDGE, 1),
                           roadclasses.draw_order_key(motorway, 0, 0))


class ContractWithCpp(unittest.TestCase):
    """I numeri delle classi sono scritti nelle tile e letti dal C++: devono coincidere."""

    @classmethod
    def setUpClass(cls):
        with open(CLASSES_HEADER, encoding="utf-8") as handle:
            cls.header = handle.read()

    def test_every_class_has_the_same_number(self):
        enum = dict((name, int(value)) for name, value in
                    re.findall(r"^\s*([A-Z][A-Za-z]+)\s*=\s*(\d+),", self.header, re.MULTILINE))
        names = dict(re.findall(r"case ERoadClass::([A-Za-z]+):\s*return \"([a-z_]+)\";", self.header))
        for road_class in roadclasses.CLASSES:
            cpp_name = next((cpp for cpp, py in names.items() if py == road_class.name), None)
            self.assertIsNotNone(cpp_name, f"{road_class.name} manca in RoadClassName()")
            self.assertEqual(enum.get(cpp_name), road_class.id,
                             f"{road_class.name}: {road_class.id} in Python, {enum.get(cpp_name)} in C++")
        self.assertEqual(len(names), len(roadclasses.CLASSES))
        count = int(re.search(r"RoadClassCount\s*=\s*(\d+)", self.header).group(1))
        self.assertGreater(count, max(c.id for c in roadclasses.CLASSES))

    def test_flags_match(self):
        for name, value in (("Bridge", roadclasses.FLAG_BRIDGE), ("Tunnel", roadclasses.FLAG_TUNNEL),
                            ("Unpaved", roadclasses.FLAG_UNPAVED), ("Link", roadclasses.FLAG_LINK)):
            match = re.search(rf"RoadFlag{name}\s*=\s*1\s*<<\s*(\d+)", self.header)
            self.assertIsNotNone(match, name)
            self.assertEqual(1 << int(match.group(1)), value, name)

    def test_fixture_is_up_to_date(self):
        # Il test C++ (georoads_tests) legge questo file. Se il formato cambia
        # qui, il file va rigenerato, altrimenti il C++ proverebbe il vecchio.
        with open(FIXTURE, "rb") as handle:
            self.assertEqual(handle.read(), vectorformat.fixture_bytes(),
                             "vector_fixture.gvt non e' aggiornato: rigeneralo con "
                             "vectorformat.fixture_bytes()")


class Format(unittest.TestCase):

    def test_round_trip(self):
        features = vectorformat.fixture_features()
        tile = vectorformat.decode_tile(vectorformat.encode_tile(13, 8538, 2044, features))
        self.assertEqual((tile.level, tile.x, tile.y), (13, 8538, 2044))
        self.assertEqual(len(tile.features), 3)
        for original, decoded in zip(features, tile.features):
            self.assertEqual((original.class_id, original.flags, original.layer, original.width_dm),
                             (decoded.class_id, decoded.flags, decoded.layer, decoded.width_dm))
            np.testing.assert_array_equal(original.points, decoded.points)

    def test_header_sizes(self):
        self.assertEqual(vectorformat.HEADER.size, 32)
        self.assertEqual(vectorformat.FEATURE.size, 12)
        self.assertEqual(len(vectorformat.fixture_bytes()), 32 + 3 * 12 + 7 * 4)

    def test_rejects_unclipped_lines(self):
        line = vectorformat.Feature(1, 0, 0, 100, np.array([[0, 0], [40000, 0]]))
        with self.assertRaises(ValueError):
            vectorformat.encode_tile(13, 0, 0, [line])

    def test_rejects_garbage(self):
        blob = vectorformat.fixture_bytes()
        with self.assertRaises(ValueError):
            vectorformat.decode_tile(blob[:-2])
        with self.assertRaises(ValueError):
            vectorformat.decode_tile(b"GWIM" + blob[4:])

    def test_local_coordinates(self):
        bounds = tiling.tile_bounds(13, 8538, 2044)
        u, v = vectorformat.lonlat_to_local(13, 8538, 2044, np.array([bounds.west, bounds.east]),
                                            np.array([bounds.north, bounds.south]))
        np.testing.assert_allclose(u, [0, vectorformat.EXTENT])
        np.testing.assert_allclose(v, [0, vectorformat.EXTENT])

    def test_index_round_trip(self):
        with tempfile.TemporaryDirectory() as root:
            entries = [vectorformat.VectorTileEntry(5, 2, 9), vectorformat.VectorTileEntry(1, 2, 3)]
            vectorformat.write_level_index(root, 12, entries)
            read = vectorformat.read_level_index(root, 12)
            self.assertEqual([(e.x, e.y, e.feature_count) for e in read], [(1, 2, 3), (5, 2, 9)])
            # Lo stesso file messo nella cartella di un altro livello: rifiutato.
            os.makedirs(os.path.join(root, "11"))
            os.replace(os.path.join(root, "12", vectorformat.INDEX_FILENAME),
                       os.path.join(root, "11", vectorformat.INDEX_FILENAME))
            with self.assertRaises(ValueError):
                vectorformat.read_level_index(root, 11)


# --- Estratto OSM sintetico ----------------------------------------------------

def write_synthetic_osm(path: str) -> None:
    """
    Un pezzo di Torino inventato:
      - un'autostrada lunga 20 km in direzione est-ovest, che attraversa molte tile;
      - una residenziale corta, tutta dentro una tile di livello 13;
      - una ferrovia su ponte;
      - una strada in galleria (da scartare) e una linea elettrica (da ignorare);
      - una pista d'aeroporto con la larghezza esplicita.
    """
    nodes = []
    ways = []
    node_id = [0]

    def node(lon, lat):
        node_id[0] += 1
        nodes.append(f'  <node id="{node_id[0]}" lat="{lat:.7f}" lon="{lon:.7f}" version="1"/>')
        return node_id[0]

    def way(way_id, points, **tags):
        refs = "".join(f'<nd ref="{node(lon, lat)}"/>' for lon, lat in points)
        tag_xml = "".join(f'<tag k="{k}" v="{v}"/>' for k, v in tags.items())
        ways.append(f'  <way id="{way_id}" version="1">{refs}{tag_xml}</way>')

    way(100, [(7.50 + 0.01 * i, 45.0700 + 0.0005 * (i % 2)) for i in range(21)],
        highway="motorway", lanes="2", ref="A55")
    way(101, [(7.6810, 45.0710), (7.6830, 45.0712), (7.6850, 45.0720)], highway="residential")
    way(102, [(7.60, 45.05), (7.70, 45.09)], railway="rail", bridge="yes", layer="1")
    way(103, [(7.65, 45.06), (7.66, 45.06)], highway="primary", tunnel="yes")
    way(104, [(7.55, 45.10), (7.75, 45.10)], power="line")
    way(105, [(7.6200, 45.2000), (7.6400, 45.2150)], aeroway="runway", width="60")

    with open(path, "w", encoding="utf-8") as handle:
        handle.write("<?xml version='1.0' encoding='UTF-8'?>\n<osm version=\"0.6\">\n")
        handle.write("\n".join(nodes) + "\n" + "\n".join(ways) + "\n</osm>\n")


@unittest.skipUnless(HAS_GDAL_OSM, "serve GDAL con il driver OSM e GEOS")
class BuildOnSyntheticOsm(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        from geoworld import vectorbuild
        cls.vectorbuild = vectorbuild
        cls.directory = tempfile.TemporaryDirectory()
        cls.osm = os.path.join(cls.directory.name, "torino.osm")
        write_synthetic_osm(cls.osm)
        cls.output = os.path.join(cls.directory.name, "strade")
        cls.messages = []
        cls.manifest = vectorbuild.build(inputs=[cls.osm], output=cls.output, name="prova",
                                         jobs=1, report=cls.messages.append)

    @classmethod
    def tearDownClass(cls):
        cls.directory.cleanup()

    def tiles_with(self, level, class_name):
        class_id = roadclasses.BY_NAME[class_name].id
        found = []
        for entry in vectorformat.read_level_index(self.output, level):
            tile = vectorformat.read_tile(os.path.join(
                self.output, vectorformat.tile_relative_path(level, entry.x, entry.y)))
            if any(f.class_id == class_id for f in tile.features):
                found.append(tile)
        return found

    def test_manifest(self):
        self.assertEqual(self.manifest["datasetKind"], "vector")
        self.assertEqual([level["level"] for level in self.manifest["levels"]], [10, 11, 12, 13])
        counts = {c["name"]: c["features"] for c in self.manifest["classes"]}
        self.assertEqual(counts["motorway"], 1)
        self.assertEqual(counts["primary"], 0, "la strada in galleria non deve esserci")
        self.assertEqual(counts["runway"], 1)

    def test_minor_roads_appear_only_when_close(self):
        self.assertEqual(self.tiles_with(10, "residential"), [])
        self.assertEqual(self.tiles_with(11, "residential"), [])
        # Al 12 c'e': nella tile che la contiene e, se passa vicino al bordo,
        # nel buffer della vicina.
        at12 = self.tiles_with(12, "residential")
        self.assertGreaterEqual(len(at12), 1)
        self.assertIn(tiling.tile_for_lonlat(12, 7.6810, 45.0710), [(t.x, t.y) for t in at12])
        self.assertGreaterEqual(len(self.tiles_with(10, "motorway")), 1)
        self.assertGreaterEqual(len(self.tiles_with(10, "runway")), 1)

    def test_long_motorway_is_cut_across_tiles(self):
        tiles = self.tiles_with(13, "motorway")
        # 0,2 gradi in est-ovest: deve esserci in OGNI colonna di tile che
        # attraversa, da quella del primo nodo a quella dell'ultimo.
        first, _ = tiling.tile_for_lonlat(13, 7.50, 45.07)
        last, _ = tiling.tile_for_lonlat(13, 7.70, 45.07)
        self.assertTrue(set(range(first, last + 1)) <= {tile.x for tile in tiles},
                        f"colonne {first}..{last}, trovate {sorted(t.x for t in tiles)}")
        for tile in tiles:
            for feature in tile.features:
                self.assertGreaterEqual(feature.points.min(), -vectorformat.BUFFER - 1)
                self.assertLessEqual(feature.points.max(), vectorformat.EXTENT + vectorformat.BUFFER + 1)

    def test_coordinates_land_where_the_road_is(self):
        # Il primo nodo della residenziale, riportato in gradi dalla tile del 13.
        level = 13
        x, y = tiling.tile_for_lonlat(level, 7.6810, 45.0710)
        tile = vectorformat.read_tile(os.path.join(self.output, vectorformat.tile_relative_path(level, x, y)))
        residential = [f for f in tile.features if f.class_id == roadclasses.BY_NAME["residential"].id]
        self.assertEqual(len(residential), 1)
        lon, lat = vectorformat.local_to_lonlat(level, x, y, residential[0].points[0, 0],
                                                residential[0].points[0, 1])
        # Un passo di quantizzazione al livello 13 e' ~15 cm: 1e-5 gradi sono ~1 m.
        self.assertAlmostEqual(float(lon), 7.6810, delta=1e-5)
        self.assertAlmostEqual(float(lat), 45.0710, delta=1e-5)

    def test_width_and_flags_survive(self):
        rail = self.tiles_with(13, "rail")[0]
        feature = next(f for f in rail.features if f.class_id == roadclasses.BY_NAME["rail"].id)
        self.assertTrue(feature.flags & roadclasses.FLAG_BRIDGE)
        self.assertEqual(feature.layer, 1)
        runway = self.tiles_with(13, "runway")[0]
        self.assertEqual(next(f.width_dm for f in runway.features), 600)

    def test_draw_order_in_tile(self):
        # Dove la ferrovia su ponte incrocia l'autostrada, la ferrovia (layer 1)
        # deve venire DOPO: e' disegnata sopra.
        for tile in self.tiles_with(13, "rail"):
            classes = [f.class_id for f in tile.features]
            if roadclasses.BY_NAME["motorway"].id in classes:
                self.assertGreater(classes.index(roadclasses.BY_NAME["rail"].id),
                                   classes.index(roadclasses.BY_NAME["motorway"].id))
                return
        self.fail("nessuna tile con ferrovia e autostrada insieme")

    def test_verify_passes(self):
        self.assertEqual(self.vectorbuild.verify(self.output), 0)

    def test_rerun_skips_and_matches(self):
        before = {path: open(path, "rb").read()
                  for path in sorted(self._tile_files())}
        messages = []
        self.vectorbuild.build(inputs=[self.osm], output=self.output, name="prova",
                               jobs=1, report=messages.append)
        self.assertTrue(any("gia' fatta" in m for m in messages), messages)
        blocks = len(os.listdir(os.path.join(self.output, "_work", "blocchi")))
        self.assertTrue(any(f"({blocks} gia' fatti)" in m for m in messages), messages)
        after = {path: open(path, "rb").read() for path in sorted(self._tile_files())}
        self.assertEqual(before, after)

    def test_bbox_keeps_only_the_area(self):
        with tempfile.TemporaryDirectory() as other:
            manifest = self.vectorbuild.build(inputs=[self.osm], output=other, name="ritaglio",
                                              bbox=(7.60, 45.18, 7.70, 45.25), jobs=1)
            counts = {c["name"]: c["features"] for c in manifest["classes"]}
            self.assertEqual(counts["runway"], 1)
            self.assertEqual(counts["motorway"], 0)

    def _tile_files(self):
        for directory, _subdirs, files in os.walk(self.output):
            if "_work" in directory:
                continue
            for name in files:
                if name.endswith(vectorformat.TILE_EXTENSION):
                    yield os.path.join(directory, name)


@unittest.skipUnless(HAS_GDAL_OSM, "serve GDAL con il driver OSM e GEOS")
class ManyPointsBeforeTheWays(unittest.TestCase):
    """
    Il caso della prima prova vera sul nord-ovest: un file con moltissimi nodi
    "interessanti" (panchine, negozi, fermate) prima delle way. Leggendo solo il
    layer delle linee, il driver OSM accumulava i punti finche' si fermava con
    "Too many features have accumulated in points layer".
    """

    def test_lines_are_read_despite_150000_points(self):
        from geoworld import osmextract
        with tempfile.TemporaryDirectory() as directory:
            path = os.path.join(directory, "punti.osm")
            with open(path, "w", encoding="utf-8") as handle:
                handle.write("<?xml version='1.0' encoding='UTF-8'?>\n<osm version=\"0.6\">\n")
                for index in range(1, 150001):
                    handle.write(f'<node id="{index}" lat="{45.0 + (index % 997) * 1e-4:.7f}" '
                                 f'lon="{7.5 + (index % 991) * 1e-4:.7f}" version="1">'
                                 '<tag k="amenity" v="bench"/></node>\n')
                for index in range(200):
                    a, b = 200001 + 2 * index, 200002 + 2 * index
                    handle.write(f'<node id="{a}" lat="45.05" lon="{7.6 + index * 1e-4:.7f}" version="1"/>\n')
                    handle.write(f'<node id="{b}" lat="45.06" lon="{7.6 + index * 1e-4:.7f}" version="1"/>\n')
                for index in range(200):
                    handle.write(f'<way id="{index + 1}" version="1"><nd ref="{200001 + 2 * index}"/>'
                                 f'<nd ref="{200002 + 2 * index}"/><tag k="highway" v="residential"/></way>\n')
                handle.write("</osm>\n")

            counts = {}
            lines = list(osmextract.iter_lines(path, counts=counts))
            self.assertEqual(len(lines), 200)
            # E il ritaglio sull'area funziona anche leggendo dal dataset.
            inside = list(osmextract.iter_lines(path, bbox=(7.60, 45.0, 7.6099, 45.1)))
            self.assertEqual(len(inside), 100)


if __name__ == "__main__":
    unittest.main()

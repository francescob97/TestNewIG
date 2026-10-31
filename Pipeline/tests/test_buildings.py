"""
Test della pipeline degli edifici: classi e stime, formato, taglio.

Il taglio vero gira su un estratto OSM SINTETICO scritto qui (XML .osm, lo
legge lo stesso driver di GDAL dei .pbf), attorno a Torino:
  - una casa 20 x 10 m con due piani e il tetto a due falde;
  - un condominio a L alto 18 m;
  - una chiesa con due PARTI (navata 15 m, campanile 40 m): si disegnano le
    parti, non il contorno;
  - un palazzo a corte (multipoligono con il cortile);
  - una pensilina (building=roof), un edificio colorato con il Wikidata;
  - building=no (da scartare) e una casa fuori dall'area.
"""
import math
import os
import re
import shutil
import sys
import tempfile
import unittest

import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from geoworld import bldbuild, bldclasses, bldformat, tiling  # noqa: E402

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
TILES_PUBLIC = os.path.join(REPO, "Plugins", "GeoWorld", "Source", "GeoTiles", "Public", "Tiles")
CLASSES_HEADER = os.path.join(TILES_PUBLIC, "BuildingClasses.h")
FORMAT_HEADER = os.path.join(TILES_PUBLIC, "BuildingTileFormat.h")
FIXTURE = os.path.join(REPO, "Plugins", "GeoWorld", "Source", "GeoTiles", "TestData", "buildings_fixture.gbd")


def _osm_ready():
    try:
        from osgeo import gdal
    except Exception:                                   # noqa: BLE001
        return False
    return gdal.GetDriverByName("OSM") is not None


HAS_OSM = _osm_ready()


def _read(path):
    with open(path, encoding="utf-8") as handle:
        return handle.read()


class ContractWithCpp(unittest.TestCase):

    def enum(self, name):
        body = re.search(rf"enum class {name}[^{{]*\{{(.*?)\}};", _read(CLASSES_HEADER), re.S)
        self.assertIsNotNone(body, name)
        return {key: int(value) for key, value in re.findall(r"^\s*(\w+)\s*=\s*(\d+),", body.group(1), re.M)}

    def constant(self, header, name):
        match = re.search(rf"{name}\s*=\s*([0-9a-fx<> ]+?)u?;", header)
        self.assertIsNotNone(match, name)
        return eval(match.group(1))                     # noqa: S307 -- costanti dell'header

    def test_classes_and_roofs(self):
        self.assertEqual(self.enum("EBuildingClass"), {item.name: item.id for item in bldclasses.CLASSES})
        self.assertEqual(self.enum("ERoofShape"), bldclasses.ROOF_SHAPES)

    def test_flags_and_format(self):
        classes = _read(CLASSES_HEADER)
        for name, value in (("BuildingWallColour", bldclasses.FLAG_WALL_COLOUR),
                            ("BuildingRoofColour", bldclasses.FLAG_ROOF_COLOUR),
                            ("BuildingPart", bldclasses.FLAG_PART),
                            ("BuildingHeightMeasured", bldclasses.FLAG_HEIGHT_MEASURED),
                            ("BuildingLevelsMeasured", bldclasses.FLAG_LEVELS_MEASURED),
                            ("BuildingOutline", bldclasses.FLAG_OUTLINE)):
            self.assertEqual(self.constant(classes, name), value, name)
        header = _read(FORMAT_HEADER)
        self.assertEqual(self.constant(header, "BuildingTileMagic").to_bytes(4, "little"), bldformat.MAGIC)
        self.assertEqual(self.constant(header, "BuildingIndexMagic").to_bytes(4, "little"), bldformat.INDEX_MAGIC)
        self.assertEqual(self.constant(header, "BuildingTileVersion"), bldformat.FORMAT_VERSION)
        self.assertEqual(self.constant(header, "BuildingTileHeaderBytes"), bldformat.HEADER.size)
        self.assertEqual(self.constant(header, "BuildingRecordBytes"), bldformat.RECORD.size)
        self.assertEqual(self.constant(header, "BuildingQuantum"), bldformat.QUANTUM)

    def test_fixture_is_up_to_date(self):
        with open(FIXTURE, "rb") as handle:
            self.assertEqual(handle.read(), bldformat.fixture_bytes(),
                             "buildings_fixture.gbd non e' aggiornato: rigeneralo con bldformat.fixture_bytes()")

    def test_fixture_key_is_turin(self):
        level, x, y = bldformat.FIXTURE_KEY
        self.assertEqual(tiling.tile_for_lonlat(level, 7.68, 45.07), (x, y))


class Classes(unittest.TestCase):

    def test_classify(self):
        name = lambda tags: bldclasses.classify(tags).name   # noqa: E731
        self.assertEqual(name({"building": "house"}), "House")
        self.assertEqual(name({"building": "apartments"}), "Apartments")
        self.assertEqual(name({"building": "yes", "amenity": "place_of_worship"}), "Religious")
        self.assertEqual(name({"building": "yes"}), "Other")
        self.assertEqual(name({"building": "strano"}), "Other")
        self.assertEqual(name({"building:part": "yes"}), "Other")
        self.assertEqual(name({"building:part": "tower"}), "Tower")
        self.assertEqual(name({"building": "roof"}), "Roof")
        self.assertIsNone(bldclasses.classify({"building": "no"}))
        self.assertIsNone(bldclasses.classify({"building": "yes", "location": "underground"}))
        self.assertIsNone(bldclasses.classify({"building:part": "no"}))

    def test_parsing(self):
        self.assertEqual(bldclasses.parse_length_m("12"), 12.0)
        self.assertEqual(bldclasses.parse_length_m("12 m"), 12.0)
        self.assertEqual(bldclasses.parse_length_m("12,5m"), 12.5)
        self.assertAlmostEqual(bldclasses.parse_length_m("40'"), 12.192)
        self.assertIsNone(bldclasses.parse_length_m("alto"))
        self.assertEqual(bldclasses.parse_count("3;4"), 3)
        self.assertEqual(bldclasses.parse_count("2.5"), 2)
        self.assertEqual(bldclasses.parse_colour("#fc8"), (255, 204, 136))
        self.assertEqual(bldclasses.parse_colour("#E8D8B0"), (232, 216, 176))
        self.assertEqual(bldclasses.parse_colour("Light Grey"), (200, 200, 200))
        self.assertIsNone(bldclasses.parse_colour("#zz0000"))

    def test_estimates(self):
        house = bldclasses.estimate({"building": "house"}, bldclasses.BY_NAME["House"])
        self.assertAlmostEqual(house.height_m, 6.5)          # 2 piani x 3 m + 0,5
        self.assertEqual(house.flags, 0)
        tall = bldclasses.estimate({"building": "apartments", "height": "18"}, bldclasses.BY_NAME["Apartments"])
        self.assertEqual((tall.height_m, tall.levels), (18.0, 6))
        self.assertTrue(tall.flags & bldclasses.FLAG_HEIGHT_MEASURED)
        levels = bldclasses.estimate({"building:levels": "4", "roof:levels": "1"}, bldclasses.BY_NAME["Other"])
        self.assertAlmostEqual(levels.height_m, 4 * 3.0 + 0.5 + 2.6)
        self.assertAlmostEqual(levels.roof_height_m, 2.6)
        shed = bldclasses.estimate({"building": "warehouse"}, bldclasses.BY_NAME["Industrial"])
        self.assertEqual(shed.height_m, 8.0)
        canopy = bldclasses.estimate({"building": "roof"}, bldclasses.BY_NAME["Roof"])
        self.assertAlmostEqual(canopy.min_height_m, 3.4)
        part = bldclasses.estimate({"building:part": "yes", "min_height": "20", "height": "40"},
                                   bldclasses.BY_NAME["Other"], is_part=True)
        self.assertEqual((part.min_height_m, part.height_m), (20.0, 40.0))
        self.assertTrue(part.flags & bldclasses.FLAG_PART)
        coloured = bldclasses.estimate({"building:colour": "white", "roof:shape": "half-hipped"}, bldclasses.BY_NAME["Other"])
        self.assertEqual(coloured.roof_shape, bldclasses.ROOF_SHAPES["Hipped"])
        self.assertTrue(coloured.flags & bldclasses.FLAG_WALL_COLOUR)
        self.assertFalse(coloured.flags & bldclasses.FLAG_ROOF_COLOUR)


class Format(unittest.TestCase):

    def test_roundtrip(self):
        tile = bldformat.fixture_tile()
        back = bldformat.decode_tile(bldformat.encode_tile(tile))
        self.assertEqual((back.level, back.x, back.y, len(back.buildings)), (tile.level, tile.x, tile.y, 3))
        for original, decoded in zip(tile.buildings, back.buildings):
            self.assertEqual(original.osm_id, decoded.osm_id)
            self.assertEqual(original.height_dm, decoded.height_dm)
            self.assertEqual(len(original.rings), len(decoded.rings))
            for ring_a, ring_b in zip(original.rings, decoded.rings):
                np.testing.assert_array_equal(ring_a, ring_b)

    def test_quantization(self):
        level, x, y = bldformat.FIXTURE_KEY
        bounds = tiling.tile_bounds(level, x, y)
        lon = np.array([bounds.west + 0.0012345, bounds.east + 0.001, bounds.west - 0.002])
        lat = np.array([bounds.south + 0.0021, bounds.north + 0.0005, bounds.south - 0.001])
        quantized = bldformat.quantize(level, x, y, lon, lat)
        back_lon, back_lat = bldformat.dequantize(level, x, y, quantized)
        error_m = max(np.abs(back_lon - lon).max() * 78700.0, np.abs(back_lat - lat).max() * 111132.0)
        self.assertLess(error_m, 0.03, "un passo di quantizzazione e' ~4 cm")
        far = bldformat.quantize(level, x, y, np.array([bounds.west + 3 * tiling.tile_span_deg(level)]),
                                 np.array([bounds.south]))
        self.assertIsNone(far, "oltre due tile non ci sta")

    def test_rejects_damage(self):
        blob = bldformat.fixture_bytes()
        with self.assertRaises(ValueError):
            bldformat.decode_tile(blob[:20])
        with self.assertRaises(ValueError):
            bldformat.decode_tile(blob[:-2])
        with self.assertRaises(ValueError):
            bldformat.decode_tile(b"XXXX" + blob[4:])

    def test_index(self):
        folder = tempfile.mkdtemp()
        try:
            entries = [bldformat.BuildingTileEntry(5, 9, 30, 900), bldformat.BuildingTileEntry(4, 2, 7, 300)]
            bldformat.write_index(folder, 15, entries)
            back = bldformat.read_index(folder, 15)
            self.assertEqual([(e.x, e.y, e.count, e.size) for e in back], [(4, 2, 7, 300), (5, 9, 30, 900)])
        finally:
            shutil.rmtree(folder)


def _write_osm(path, centre_lon, centre_lat):
    """Un estratto OSM in XML: gli edifici della docstring del modulo."""
    nodes, ways, relations = [], [], []
    counter = [0]
    metres_lon = 111320.0 * math.cos(math.radians(centre_lat))

    def node(east_m, north_m):
        counter[0] += 1
        nodes.append(f'  <node id="{counter[0]}" lat="{centre_lat + north_m / 111132.0:.8f}" '
                     f'lon="{centre_lon + east_m / metres_lon:.8f}" version="1"/>')
        return counter[0]

    def way(way_id, corners, tags):
        ids = [node(e, n) for e, n in corners]
        ids.append(ids[0])
        refs = "".join(f'<nd ref="{i}"/>' for i in ids)
        tag_xml = "".join(f'<tag k="{k}" v="{v}"/>' for k, v in tags.items())
        ways.append(f'  <way id="{way_id}" version="1">{refs}{tag_xml}</way>')

    def box(east_m, north_m, width_m, depth_m):
        return [(east_m, north_m), (east_m + width_m, north_m), (east_m + width_m, north_m + depth_m),
                (east_m, north_m + depth_m)]

    way(101, box(0, 0, 20, 10), {"building": "house", "building:levels": "2", "roof:shape": "gabled"})
    way(102, [(50, 0), (80, 0), (80, 10), (60, 10), (60, 30), (50, 30)], {"building": "apartments", "height": "18"})
    # La chiesa con le sue parti.
    way(103, box(100, 0, 20, 40), {"building": "church", "name": "Chiesa", "wikidata": "Q7"})
    way(104, box(100, 0, 20, 32), {"building:part": "yes", "height": "15", "roof:shape": "gabled"})
    way(105, box(110, 32, 8, 8), {"building:part": "yes", "height": "40", "roof:shape": "pyramidal"})
    # Il palazzo a corte: le due way senza tag, il multipoligono con i tag.
    way(106, box(0, 60, 40, 40), {})
    way(107, box(12, 72, 16, 16), {})
    relations.append('  <relation id="900" version="1"><member type="way" ref="106" role="outer"/>'
                     '<member type="way" ref="107" role="inner"/><tag k="type" v="multipolygon"/>'
                     '<tag k="building" v="apartments"/><tag k="building:levels" v="5"/></relation>')
    way(108, box(60, 60, 15, 6), {"building": "roof"})
    way(109, box(100, 60, 12, 12), {"building": "yes", "building:colour": "#ff0000", "roof:colour": "red",
                                     "wikidata": "Q42"})
    way(110, box(150, 60, 10, 10), {"building": "no"})
    way(111, box(5000, 5000, 10, 10), {"building": "house"})
    with open(path, "w", encoding="utf-8") as handle:
        handle.write("<?xml version='1.0' encoding='UTF-8'?>\n<osm version=\"0.6\">\n")
        handle.write("\n".join(nodes + ways + relations))
        handle.write("\n</osm>\n")


@unittest.skipUnless(HAS_OSM, "serve GDAL con il driver OSM")
class Build(unittest.TestCase):

    LON, LAT = 7.68, 45.07
    BBOX = (7.67, 45.06, 7.70, 45.08)

    @classmethod
    def setUpClass(cls):
        cls.folder = tempfile.mkdtemp()
        cls.osm = os.path.join(cls.folder, "prova.osm")
        _write_osm(cls.osm, cls.LON, cls.LAT)
        cls.output = os.path.join(cls.folder, "edifici")
        cls.result = bldbuild.build(output=cls.output, osm_paths=[cls.osm], bbox=cls.BBOX, name="prova",
                                    report=lambda _: None)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.folder)

    def buildings(self):
        found = {}
        for entry in bldformat.read_index(self.output, bldbuild.DEFAULT_LEVEL):
            tile = bldformat.read_tile(bldformat.tile_path(self.output, bldbuild.DEFAULT_LEVEL, entry.x, entry.y))
            for building in tile.buildings:
                found[building.osm_id] = (tile, building)
        return found

    def test_which_buildings(self):
        found = self.buildings()
        ways = {osm_id & ((1 << 62) - 1) for osm_id in found if osm_id >> 62 == bldformat.OSM_WAY}
        relations = {osm_id & ((1 << 62) - 1) for osm_id in found if osm_id >> 62 == bldformat.OSM_RELATION}
        self.assertEqual(ways, {101, 102, 103, 104, 105, 108, 109})
        self.assertEqual(relations, {900})
        self.assertEqual(self.result["outlines"], 1)

    def test_outline_with_parts(self):
        """La chiesa ha le parti: si disegnano loro; lei resta, marcata, con il suo wikidata, nella stessa tile."""
        found = self.buildings()
        tile_church, church = found[bldformat.pack_osm_id(bldformat.OSM_WAY, 103)]
        self.assertTrue(church.flags & bldclasses.FLAG_OUTLINE)
        self.assertFalse(church.flags & bldclasses.FLAG_PART)
        self.assertEqual(church.wikidata, 7)
        for number in (104, 105):
            tile_part, part = found[bldformat.pack_osm_id(bldformat.OSM_WAY, number)]
            self.assertTrue(part.flags & bldclasses.FLAG_PART)
            self.assertFalse(part.flags & bldclasses.FLAG_OUTLINE)
            self.assertEqual((tile_part.x, tile_part.y), (tile_church.x, tile_church.y), "le parti nella tile del contorno")
        for number in (101, 102, 108, 109):
            self.assertFalse(found[bldformat.pack_osm_id(bldformat.OSM_WAY, number)][1].flags & bldclasses.FLAG_OUTLINE)

    def test_heights_and_tags(self):
        found = self.buildings()
        way = lambda number: found[bldformat.pack_osm_id(bldformat.OSM_WAY, number)][1]   # noqa: E731
        house = way(101)
        self.assertEqual((house.building_class, house.levels, house.height_dm), (1, 2, 65))
        self.assertEqual(house.roof, bldclasses.ROOF_SHAPES["Gabled"])
        self.assertEqual(way(102).height_dm, 180)
        self.assertTrue(way(102).flags & bldclasses.FLAG_HEIGHT_MEASURED)
        tower = way(105)
        self.assertEqual((tower.height_dm, tower.roof), (400, bldclasses.ROOF_SHAPES["Pyramidal"]))
        self.assertTrue(tower.flags & bldclasses.FLAG_PART)
        canopy = way(108)
        self.assertEqual(canopy.building_class, bldclasses.BY_NAME["Roof"].id)
        self.assertEqual(canopy.min_height_dm, 34)
        coloured = way(109)
        self.assertEqual((coloured.wall_colour, coloured.roof_colour), ((255, 0, 0), (170, 60, 50)))
        self.assertEqual(coloured.wikidata, 42)

    def test_courtyard_and_geometry(self):
        found = self.buildings()
        tile, palace = found[bldformat.pack_osm_id(bldformat.OSM_RELATION, 900)]
        self.assertEqual(len(palace.rings), 2)

        def signed_area(ring):
            x, y = ring[:, 0].astype(float), ring[:, 1].astype(float)
            return 0.5 * (np.dot(x, np.roll(y, -1)) - np.dot(np.roll(x, -1), y))
        self.assertGreater(signed_area(palace.rings[0]), 0, "il contorno gira in senso antiorario")
        self.assertLess(signed_area(palace.rings[1]), 0, "il cortile in senso orario")
        # La casa: 20 x 10 m, il primo vertice non ripetuto, misure giuste al decimetro.
        tile_house, house = found[bldformat.pack_osm_id(bldformat.OSM_WAY, 101)]
        lon, lat = bldformat.dequantize(tile_house.level, tile_house.x, tile_house.y, house.rings[0])
        self.assertEqual(len(lon), 4)
        width = (lon.max() - lon.min()) * 111320.0 * math.cos(math.radians(self.LAT))
        depth = (lat.max() - lat.min()) * 111132.0
        self.assertAlmostEqual(width, 20.0, delta=0.1)
        self.assertAlmostEqual(depth, 10.0, delta=0.1)

    def test_manifest_and_verify(self):
        manifest = bldformat.read_manifest(self.output)
        self.assertEqual(manifest["datasetKind"], "buildings")
        self.assertEqual(manifest["buildings"], 7, "il contorno con le parti non si conta: non si disegna")
        self.assertIn("ODbL", manifest["source"]["license"])
        self.assertEqual(bldbuild.verify(self.output, report=lambda _: None), 0)

    def test_resume(self):
        again = bldbuild.build(output=self.output, osm_paths=[self.osm], bbox=self.BBOX, name="prova",
                               report=lambda _: None)
        self.assertEqual(again["written"], 0, "estrazione e blocchi gia' fatti: niente da riscrivere")
        self.assertEqual(again["buildings"], 7)


if __name__ == "__main__":
    unittest.main()

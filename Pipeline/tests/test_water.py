"""
Test della pipeline dell'acqua (Fase 9): classificazione, formato, taglio.

Il taglio vero gira su tre cose SINTETICHE scritte qui:
  - un terreno (tile .ght) che scende verso est, con un lago piano a 230 m, il
    letto di un fiume e il mare a sud;
  - un estratto OSM (XML .osm) con il lago, il fiume (verso est), un torrente
    intermittente e una piscina (da scartare);
  - i poligoni del mare in un GeoPackage, come quelli di osmdata.
Il geoide e' una costante (47 m): senza le griglie di PROJ i test girano lo
stesso, e il conto si controlla a mano.
"""
import json
import os
import re
import sys
import tempfile
import time
import unittest

import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from geoworld import tiling, tileformat, waterclasses, waterformat  # noqa: E402
from geoworld import manifest as terrain_manifest  # noqa: E402

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
FORMAT_HEADER = os.path.join(REPO, "Plugins", "GeoWorld", "Source", "GeoTiles", "Public",
                             "Tiles", "WaterTileFormat.h")
FIXTURE = os.path.join(REPO, "Plugins", "GeoWorld", "Source", "GeoTiles", "TestData",
                       "water_fixture.gwa")


def _gdal_ready():
    try:
        from osgeo import gdal, ogr
    except Exception:                                   # noqa: BLE001
        return False
    if gdal.GetDriverByName("OSM") is None or gdal.GetDriverByName("GPKG") is None:
        return False
    box = ogr.CreateGeometryFromWkt("POLYGON((0 0,1 0,1 1,0 1,0 0))")
    return box.Buffer(0.1) is not None


HAS_GDAL = _gdal_ready()


class Classification(unittest.TestCase):

    def test_areas(self):
        area = waterclasses.classify_area
        self.assertEqual(area({"natural": "water"}).kind, waterclasses.KIND_LAKE)
        self.assertEqual(area({"natural": "water", "water": "reservoir"}).kind, waterclasses.KIND_LAKE)
        self.assertEqual(area({"landuse": "reservoir"}).kind, waterclasses.KIND_LAKE)
        self.assertEqual(area({"natural": "water", "water": "lagoon"}).kind, waterclasses.KIND_LAGOON)
        self.assertEqual(area({"natural": "water", "water": "river"}).kind, waterclasses.KIND_RIVER)
        self.assertEqual(area({"natural": "water", "water": "canal"}).kind, waterclasses.KIND_CANAL)
        self.assertEqual(area({"waterway": "riverbank"}).kind, waterclasses.KIND_RIVER)

    def test_invisible_water_is_dropped(self):
        area = waterclasses.classify_area
        line = waterclasses.classify_line
        self.assertIsNone(area({"natural": "water", "water": "wastewater"}))
        self.assertIsNone(area({"leisure": "swimming_pool", "natural": "water"}))
        self.assertIsNone(area({"natural": "water", "intermittent": "yes"}))
        self.assertIsNone(area({"natural": "wood"}))
        self.assertIsNone(line({"waterway": "river", "intermittent": "yes"}))
        self.assertIsNone(line({"waterway": "river", "tunnel": "culvert"}))
        self.assertIsNone(line({"waterway": "drain"}))

    def test_lines_and_widths(self):
        line = waterclasses.classify_line
        river = line({"waterway": "river"})
        self.assertEqual((river.kind, river.width_m), (waterclasses.KIND_RIVER, 12.0))
        self.assertEqual(line({"waterway": "river", "width": "35 m"}).width_m, 35.0)
        self.assertEqual(line({"waterway": "canal"}).width_m, 8.0)
        # I torrenti solo se larghi davvero.
        self.assertIsNone(line({"waterway": "stream"}))
        self.assertIsNone(line({"waterway": "stream", "width": "2"}))
        self.assertEqual(line({"waterway": "stream", "width": "6"}).width_m, 6.0)
        self.assertIsNone(line({"waterway": "river", "width": "2"}))

    def test_rules(self):
        self.assertEqual(waterclasses.wave_exposure(waterclasses.KIND_SEA), 1.0)
        small = waterclasses.wave_exposure(waterclasses.KIND_LAKE, 0.01)
        garda = waterclasses.wave_exposure(waterclasses.KIND_LAKE, 370.0)
        self.assertAlmostEqual(small, 0.12)
        self.assertAlmostEqual(garda, 0.8)
        self.assertLess(waterclasses.wave_exposure(waterclasses.KIND_RIVER),
                        waterclasses.wave_exposure(waterclasses.KIND_LAGOON))
        self.assertEqual(waterclasses.max_depth(waterclasses.KIND_LAKE, 370.0), 120.0)
        self.assertEqual(waterclasses.max_depth(waterclasses.KIND_RIVER, 0.0, 30.0), 2.0)

        # La rampa del mare: 0,5 m sulla riva, 5%, fino a 60 m.
        shore, slope = waterclasses.ramp(waterclasses.KIND_SEA)
        depths = waterclasses.depth_at(np.array([0.0, 100.0, 5000.0]), shore, slope, 60.0)
        np.testing.assert_allclose(depths, [0.5, 5.5, 60.0])
        self.assertLessEqual(0.5 + 0.05 * waterclasses.MAX_RAMP_DISTANCE_M, 60.5)


class ContractWithCpp(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        with open(FORMAT_HEADER, encoding="utf-8") as handle:
            cls.header = handle.read()

    def constant(self, name):
        match = re.search(rf"{name}\s*=\s*([-0-9.x<> ]+?)[;f]", self.header)
        self.assertIsNotNone(match, name)
        return eval(match.group(1).replace("u", ""))   # noqa: S307 -- costanti dell'header

    def test_flags_and_constants(self):
        for name, value in (("WaterTileSea", waterclasses.TILE_SEA),
                            ("WaterTileLake", waterclasses.TILE_LAKE),
                            ("WaterTileRiver", waterclasses.TILE_RIVER),
                            ("WaterTileFlow", waterclasses.TILE_FLOW),
                            ("WaterTileAllWater", waterclasses.TILE_ALL_WATER)):
            self.assertEqual(self.constant(name), value, name)
        self.assertEqual(self.constant("WaterRaster"), waterformat.RASTER)
        self.assertEqual(self.constant("WaterNoHeight"), waterformat.NO_HEIGHT)
        self.assertEqual(self.constant("WaterSdfZero"), waterformat.SDF_ZERO)
        self.assertEqual(self.constant("WaterSdfScale"), waterformat.SDF_SCALE)
        self.assertEqual(self.constant("WaterFlowScale"), 100.0)

    def test_fixture_is_up_to_date(self):
        with open(FIXTURE, "rb") as handle:
            self.assertEqual(handle.read(), waterformat.fixture_bytes(),
                             "water_fixture.gwa non e' aggiornato: rigeneralo con "
                             "waterformat.fixture_bytes()")


class Format(unittest.TestCase):

    def test_round_trip(self):
        tile = waterformat.fixture_tile()
        decoded = waterformat.decode_tile(waterformat.encode_tile(tile))
        self.assertEqual((decoded.level, decoded.x, decoded.y), waterformat.FIXTURE_KEY)
        self.assertTrue(decoded.flags & waterclasses.TILE_FLOW)
        np.testing.assert_allclose(decoded.surface[:, :65], 230.4, atol=0.05)
        self.assertTrue(np.isnan(decoded.surface[:, 65:]).all())
        np.testing.assert_array_equal(decoded.sdf, tile.sdf)
        np.testing.assert_array_equal(decoded.flow_u, tile.flow_u)

    def test_without_flow_the_planes_are_omitted(self):
        tile = waterformat.fixture_tile()
        tile.flow_u = tile.flow_v = None
        blob = waterformat.encode_tile(tile)
        decoded = waterformat.decode_tile(blob)
        self.assertFalse(decoded.flags & waterclasses.TILE_FLOW)
        self.assertIsNone(decoded.flow_u)

    def test_decimetres_and_sentinel(self):
        values = np.array([230.44, -3300.0, np.nan, 0.05])
        dm = waterformat.heights_to_dm(values)
        np.testing.assert_array_equal(dm, [2304, -32767, waterformat.NO_HEIGHT, 0])
        back = waterformat.dm_to_heights(dm)
        self.assertTrue(np.isnan(back[2]))
        self.assertAlmostEqual(float(back[0]), 230.4, places=4)

    def test_sdf_encoding(self):
        encoded = waterformat.sdf_to_bytes(np.array([0.0, 1.0, -1.0, 100.0, -100.0]))
        np.testing.assert_array_equal(encoded, [128, 136, 120, 255, 0])

    def test_rejects_garbage(self):
        with self.assertRaises(ValueError):
            waterformat.decode_tile(b"GWVT" + bytes(40))
        blob = bytearray(waterformat.fixture_bytes())
        with self.assertRaises(ValueError):
            waterformat.decode_tile(bytes(blob[:-3]))

    def test_metres_per_pixel(self):
        # Torino, livello 14: ~4,1 m (media geometrica di 3,4 m est e 4,8 m nord).
        mpp = waterformat.metres_per_pixel(14, waterformat.FIXTURE_KEY[2])
        self.assertAlmostEqual(mpp, 4.03, delta=0.1)


# =============================================================================
#  Il taglio vero, sui dati sintetici
# =============================================================================

BBOX = (7.600, 44.990, 7.720, 45.100)
LAKE = (7.640, 45.060, 7.662, 45.076)     # dove il terreno e' piano a 230 m
LAKE_POLYGON = (7.642, 45.062, 7.660, 45.074)
RIVER_LAT = 45.040
SEA_NORTH = 45.012
GEOID = 47.0


def terrain_height(lon, lat):
    """Scende verso est di 300 m per grado; il lago, il letto del fiume, il mare."""
    height = 240.0 + (7.72 - lon) * 300.0
    height = np.where((lon > LAKE[0]) & (lon < LAKE[2]) & (lat > LAKE[1]) & (lat < LAKE[3]),
                      230.0, height)
    height = np.where(np.abs(lat - RIVER_LAT) < 0.0001, height - 1.5, height)
    return np.where(lat < SEA_NORTH, GEOID, height)


def write_terrain(root, levels=(12, 13, 14)):
    summaries = []
    for level in levels:
        x0, y0, x1, y1 = tiling.tile_range_for_bbox(level, *BBOX)
        entries = []
        span = tiling.tile_span_deg(level)
        for y in range(y0, y1 + 1):
            for x in range(x0, x1 + 1):
                bounds = tiling.tile_bounds(level, x, y)
                lons = bounds.west + np.arange(tiling.TILE_POSTS) * span / tiling.TILE_CELLS
                lats = bounds.north - np.arange(tiling.TILE_POSTS) * span / tiling.TILE_CELLS
                heights = terrain_height(lons[np.newaxis, :], lats[:, np.newaxis]).astype(np.float32)
                path = os.path.join(root, tileformat.tile_relative_path(level, x, y))
                tileformat.write_tile_atomic(path, tileformat.encode_tile(level, x, y, heights, False))
                entries.append(terrain_manifest.TileEntry(x, y, float(heights.min()), float(heights.max())))
        terrain_manifest.write_level_index(root, level, entries)
        summaries.append(terrain_manifest.LevelSummary(level, len(entries), x0, x1, y0, y1, 0.0, 0.0,
                                                       0.0, 0.0, 0.0))
    document = terrain_manifest.build_manifest(dataset_name="sintetico", bbox=BBOX, levels=summaries,
                                               source={}, vertical={"verticalCrs": "EPSG:3855"},
                                               pipeline_version="test")
    terrain_manifest.write_manifest(root, document)


def write_osm(path):
    nodes, ways = [], []
    counter = [0]

    def node(lon, lat):
        counter[0] += 1
        nodes.append(f'  <node id="{counter[0]}" lat="{lat:.7f}" lon="{lon:.7f}" version="1"/>')
        return counter[0]

    def way(way_id, points, closed=False, **tags):
        ids = [node(lon, lat) for lon, lat in points]
        if closed:
            ids.append(ids[0])
        refs = "".join(f'<nd ref="{i}"/>' for i in ids)
        tag_xml = "".join(f'<tag k="{k}" v="{v}"/>' for k, v in tags.items())
        ways.append(f'  <way id="{way_id}" version="1">{refs}{tag_xml}</way>')

    w, s, e, n = LAKE_POLYGON
    way(1, [(w, s), (e, s), (e, n), (w, n)], closed=True, natural="water", name="Lago di prova")
    # Il fiume scorre verso EST (il verso della way), dove il terreno scende.
    way(2, [(7.605 + 0.01 * i, RIVER_LAT) for i in range(11)], waterway="river", width="20")
    way(3, [(7.61, 45.09), (7.70, 45.09)], waterway="river", intermittent="yes")
    way(4, [(7.700, 45.080), (7.7003, 45.080), (7.7003, 45.0802), (7.700, 45.0802)], closed=True,
        leisure="swimming_pool", natural="water")
    with open(path, "w", encoding="utf-8") as handle:
        handle.write("<?xml version='1.0' encoding='UTF-8'?>\n<osm version=\"0.6\">\n")
        handle.write("\n".join(nodes) + "\n" + "\n".join(ways) + "\n</osm>\n")


def write_sea(path):
    from osgeo import ogr, osr
    ogr.UseExceptions()
    osr.UseExceptions()
    driver = ogr.GetDriverByName("GPKG")
    datasource = driver.CreateDataSource(path)
    srs = osr.SpatialReference()
    srs.ImportFromEPSG(4326)
    layer = datasource.CreateLayer("water_polygons", srs, ogr.wkbPolygon)
    feature = ogr.Feature(layer.GetLayerDefn())
    feature.SetGeometry(ogr.CreateGeometryFromWkt(
        f"POLYGON((7.5 44.9,7.8 44.9,7.8 {SEA_NORTH},7.5 {SEA_NORTH},7.5 44.9))"))
    layer.CreateFeature(feature)
    datasource = None


@unittest.skipUnless(HAS_GDAL, "serve GDAL con i driver OSM e GPKG e GEOS")
class BuildOnSyntheticData(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        from geoworld import waterbuild
        cls.waterbuild = waterbuild
        cls.directory = tempfile.TemporaryDirectory()
        root = cls.directory.name
        cls.terrain = os.path.join(root, "terreno")
        write_terrain(cls.terrain)
        cls.osm = os.path.join(root, "acqua.osm")
        write_osm(cls.osm)
        cls.sea = os.path.join(root, "mare.gpkg")
        write_sea(cls.sea)
        cls.output = os.path.join(root, "acqua")
        cls.messages = []
        started = time.time()
        cls.manifest = waterbuild.build(osm=[cls.osm], sea=cls.sea, terrain_root=cls.terrain,
                                        output=cls.output, name="prova", bbox=BBOX,
                                        geoid=waterbuild.GeoidSampler(constant=GEOID),
                                        report=cls.messages.append)
        cls.seconds = time.time() - started

    @classmethod
    def tearDownClass(cls):
        cls.directory.cleanup()

    def tile_at(self, level, lon, lat):
        x, y = tiling.tile_for_lonlat(level, lon, lat)
        path = os.path.join(self.output, waterformat.tile_relative_path(level, x, y))
        self.assertTrue(os.path.exists(path), f"manca la tile {level}/{x}/{y} ({lon}, {lat})")
        return waterformat.read_tile(path)

    @staticmethod
    def post_of(tile, lon, lat):
        bounds = tiling.tile_bounds(tile.level, tile.x, tile.y)
        span = tiling.tile_span_deg(tile.level)
        return (int(round((bounds.north - lat) / span * tiling.TILE_CELLS)),
                int(round((lon - bounds.west) / span * tiling.TILE_CELLS)))

    @staticmethod
    def pixel_of(tile, lon, lat):
        bounds = tiling.tile_bounds(tile.level, tile.x, tile.y)
        span = tiling.tile_span_deg(tile.level)
        return (min(255, int((bounds.north - lat) / span * 256)), min(255, int((lon - bounds.west) / span * 256)))

    def test_manifest_counts_the_bodies(self):
        bodies = self.manifest["bodies"]
        self.assertEqual(bodies.get("lago"), 1, bodies)       # la piscina no
        self.assertEqual(bodies.get("fiume"), 1, bodies)      # l'intermittente no
        self.assertEqual(bodies.get("mare"), 1, bodies)
        self.assertEqual(self.manifest["datasetKind"], "water")

    def test_lake_level_and_bed(self):
        lon, lat = 0.5 * (LAKE_POLYGON[0] + LAKE_POLYGON[2]), 0.5 * (LAKE_POLYGON[1] + LAKE_POLYGON[3])
        tile = self.tile_at(14, lon, lat)
        self.assertTrue(tile.flags & waterclasses.TILE_LAKE)
        row, column = self.post_of(tile, lon, lat)
        self.assertAlmostEqual(float(tile.surface[row, column]), 230.0, delta=0.11)
        depth = float(tile.surface[row, column] - tile.bed[row, column])
        self.assertGreater(depth, 1.0, "al centro del lago il fondale e' ben sotto la superficie")
        pr, pc = self.pixel_of(tile, lon, lat)
        self.assertEqual(int(tile.sdf[pr, pc]), 255, "al centro del lago: lontano dalla riva")
        self.assertGreater(int(tile.wave[pr, pc]), 0)

    def test_shoreline_is_where_the_polygon_is(self):
        # Sul bordo ovest del lago la distanza passa per zero.
        lat = 0.5 * (LAKE_POLYGON[1] + LAKE_POLYGON[3])
        tile = self.tile_at(14, LAKE_POLYGON[0], lat)
        row, _ = self.pixel_of(tile, LAKE_POLYGON[0], lat)
        sdf = waterformat.bytes_to_sdf(tile.sdf[row])
        bounds = tiling.tile_bounds(14, tile.x, tile.y)
        span = tiling.tile_span_deg(14)
        columns = np.arange(256)
        lons = bounds.west + (columns + 0.5) / 256 * span
        crossing = np.nonzero((sdf[:-1] <= 0) & (sdf[1:] > 0))[0]
        self.assertTrue(len(crossing) > 0, "la riva deve stare in questa riga")
        shore_lon = lons[crossing[0]] + span / 256 * (-sdf[crossing[0]] / (sdf[crossing[0] + 1] - sdf[crossing[0]]))
        metres = abs(shore_lon - LAKE_POLYGON[0]) * tiling.metres_per_degree(lat)[0]
        self.assertLess(metres, 3.0, f"riva a {metres:.2f} m dal poligono")

    def test_river_flows_east_and_downhill(self):
        surfaces = []
        for lon in (7.62, 7.66, 7.70):
            tile = self.tile_at(14, lon, RIVER_LAT)
            self.assertTrue(tile.flags & waterclasses.TILE_RIVER)
            self.assertTrue(tile.flags & waterclasses.TILE_FLOW)
            row, column = self.pixel_of(tile, lon, RIVER_LAT)
            self.assertGreater(int(tile.flow_u[row, column]), 80, "verso est, ~1 m/s")
            self.assertLess(abs(int(tile.flow_v[row, column])), 10)
            prow, pcol = self.post_of(tile, lon, RIVER_LAT)
            surfaces.append(float(tile.surface[prow, pcol]))
        self.assertTrue(surfaces[0] > surfaces[1] > surfaces[2], f"il fiume scende: {surfaces}")
        # Il profilo segue il letto del fiume (terreno - 1,5 m) a meno di mezzo metro.
        expected = float(terrain_height(np.array(7.66), np.array(RIVER_LAT)))
        self.assertAlmostEqual(surfaces[1], expected, delta=0.6)

    def test_sea_at_geoid_and_deeper_offshore(self):
        near = self.tile_at(14, 7.65, SEA_NORTH - 0.001)
        self.assertTrue(near.flags & waterclasses.TILE_SEA)
        row, column = self.post_of(near, 7.65, SEA_NORTH - 0.001)
        self.assertAlmostEqual(float(near.surface[row, column]), GEOID, delta=0.06)
        shallow = float(near.surface[row, column] - near.bed[row, column])
        far = self.tile_at(14, 7.65, SEA_NORTH - 0.015)
        row, column = self.post_of(far, 7.65, SEA_NORTH - 0.015)
        deep = float(far.surface[row, column] - far.bed[row, column])
        self.assertLess(shallow, 10.0)
        self.assertGreater(deep, 50.0, f"a 1,6 km dalla riva: {deep:.1f} m")
        self.assertTrue(far.flags & waterclasses.TILE_ALL_WATER)

    def test_shared_posts_agree(self):
        # Due tile vicine scrivono lo stesso fondale e la stessa superficie sul
        # post che condividono (bordo est della prima = bordo ovest della seconda).
        level = 14
        x, y = tiling.tile_for_lonlat(level, 7.65, SEA_NORTH - 0.004)
        left = waterformat.read_tile(os.path.join(self.output, waterformat.tile_relative_path(level, x, y)))
        right = waterformat.read_tile(os.path.join(self.output, waterformat.tile_relative_path(level, x + 1, y)))
        both = np.isfinite(left.bed[:, -1]) & np.isfinite(right.bed[:, 0])
        self.assertTrue(both.any())
        np.testing.assert_allclose(left.bed[both, -1], right.bed[both, 0], atol=0.11)
        np.testing.assert_allclose(left.surface[both, -1], right.surface[both, 0], atol=0.11)

    def test_dry_land_has_no_tiles(self):
        # Il nord-est dell'area: niente acqua nel raggio di chilometri.
        x, y = tiling.tile_for_lonlat(14, 7.715, 45.098)
        entries = {(e.x, e.y) for e in waterformat.read_level_index(self.output, 14)}
        self.assertNotIn((x, y), entries)

    def test_coarse_levels_exist(self):
        for level in (12, 13):
            self.assertGreater(len(waterformat.read_level_index(self.output, level)), 0)

    def test_geoid_file(self):
        grid, _ = waterformat.read_geoid(self.output)
        np.testing.assert_allclose(grid, GEOID)

    def test_verify_passes(self):
        self.assertEqual(self.waterbuild.verify(self.output), 0)

    def test_rerun_skips_and_matches(self):
        before = {e.x: e for e in waterformat.read_level_index(self.output, 14)}
        messages = []
        started = time.time()
        self.waterbuild.build(osm=[self.osm], sea=self.sea, terrain_root=self.terrain, output=self.output,
                              name="prova", bbox=BBOX, geoid=self.waterbuild.GeoidSampler(constant=GEOID),
                              report=messages.append)
        self.assertTrue(any("gia' fatta" in m for m in messages), messages)
        self.assertLess(time.time() - started, max(5.0, self.seconds / 2))
        after = {e.x: e for e in waterformat.read_level_index(self.output, 14)}
        self.assertEqual(before.keys(), after.keys())


if __name__ == "__main__":
    unittest.main()

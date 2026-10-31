"""
Test della pipeline della vegetazione: classi, formato, nomi dei file
WorldCover, taglio.

Il taglio vero gira su un WorldCover SINTETICO scritto qui: un GeoTIFF con i
codici di WorldCover (10 alberi, 30 prato, 80 acqua), passato con
--worldcover come farebbe chi i file li ha scaricati. Niente rete.
"""
import os
import re
import shutil
import sys
import tempfile
import unittest

import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from geoworld import tiling, vegbuild, vegclasses, vegformat  # noqa: E402

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
TILES_PUBLIC = os.path.join(REPO, "Plugins", "GeoWorld", "Source", "GeoTiles", "Public", "Tiles")
CLASSES_HEADER = os.path.join(TILES_PUBLIC, "VegetationClasses.h")
FORMAT_HEADER = os.path.join(TILES_PUBLIC, "VegetationTileFormat.h")
PLACEMENT_HEADER = os.path.join(REPO, "Plugins", "GeoWorld", "Source", "GeoRender", "Public",
                                "Vegetation", "TreePlacement.h")
FIXTURE = os.path.join(REPO, "Plugins", "GeoWorld", "Source", "GeoTiles", "TestData",
                       "vegetation_fixture.gvg")


def _gdal_ready():
    try:
        from osgeo import gdal
    except Exception:                                   # noqa: BLE001
        return False
    return gdal.GetDriverByName("GTiff") is not None and gdal.GetDriverByName("VRT") is not None


HAS_GDAL = _gdal_ready()


def _read(path):
    with open(path, encoding="utf-8") as handle:
        return handle.read()


class ContractWithCpp(unittest.TestCase):
    """I numeri stanno nei file su disco: Python e C++ devono dire le stesse cose."""

    def test_classes_match_the_enum(self):
        body = re.search(r"enum class ELandCover[^{]*\{(.*?)\};", _read(CLASSES_HEADER), re.S)
        self.assertIsNotNone(body, "enum ELandCover non trovato")
        cpp = {name: int(value) for name, value in re.findall(r"^\s*(\w+)\s*=\s*(\d+),", body.group(1), re.M)}
        python = {cover.name: cover.id for cover in vegclasses.CLASSES}
        self.assertEqual(cpp, python, "VegetationClasses.h e vegclasses.py non coincidono")
        count = re.search(r"LandCoverCount\s*=\s*(\d+);", _read(CLASSES_HEADER))
        self.assertEqual(int(count.group(1)), len(vegclasses.CLASSES))
        self.assertEqual([cover.id for cover in vegclasses.CLASSES], list(range(len(vegclasses.CLASSES))),
                         "i numeri devono essere consecutivi da 0")

    def test_format_constants(self):
        header = _read(FORMAT_HEADER)

        def constant(name):
            match = re.search(rf"{name}\s*=\s*([0-9a-fx<> ]+?)u?;", header)
            self.assertIsNotNone(match, name)
            return eval(match.group(1))                  # noqa: S307 -- costanti dell'header

        self.assertEqual(constant("VegetationTileMagic").to_bytes(4, "little"), vegformat.MAGIC)
        self.assertEqual(constant("VegetationIndexMagic").to_bytes(4, "little"), vegformat.INDEX_MAGIC)
        self.assertEqual(constant("VegetationTileVersion"), vegformat.FORMAT_VERSION)
        self.assertEqual(constant("VegetationTileHeaderBytes"), vegformat.HEADER.size)
        self.assertEqual(constant("VegetationRaster"), vegformat.RASTER)
        self.assertEqual(constant("VegetationTileTrees"), vegclasses.TILE_TREES)
        self.assertEqual(constant("VegetationTileShrubs"), vegclasses.TILE_SHRUBS)
        self.assertEqual(vegformat.INDEX_HEADER.size, 16)
        self.assertEqual(vegformat.INDEX_RECORD.size, 16)

    def test_growing_classes_are_the_ones_with_plants(self):
        """Una tile senza classi GROWING non si scrive: il C++ non ci metterebbe niente comunque."""
        body = re.search(r"PlantsFor\(uint8_t Class\)(.*?)default:", _read(PLACEMENT_HEADER), re.S)
        self.assertIsNotNone(body, "PlantsFor non trovato in TreePlacement.h")
        with_plants = set(re.findall(r"ELandCover::(\w+)", body.group(1)))
        growing = {cover.name for cover in vegclasses.CLASSES if cover.id in vegclasses.GROWING}
        self.assertEqual(with_plants, growing)

    def test_fixture_is_up_to_date(self):
        with open(FIXTURE, "rb") as handle:
            self.assertEqual(handle.read(), vegformat.fixture_bytes(),
                             "vegetation_fixture.gvg non e' aggiornato: rigeneralo con "
                             "vegformat.fixture_bytes()")

    def test_fixture_key_is_turin(self):
        level, x, y = vegformat.FIXTURE_KEY
        self.assertEqual(tiling.tile_for_lonlat(level, 7.68, 45.07), (x, y))


class Classes(unittest.TestCase):

    def test_worldcover_lookup(self):
        table = vegclasses.worldcover_lookup()
        self.assertEqual(table.shape, (256,))
        expected = {10: "Trees", 20: "Shrubs", 30: "Grass", 40: "Crops", 50: "Built", 60: "Bare",
                    70: "Snow", 80: "Water", 90: "Wetland", 95: "Mangrove", 100: "Moss", 0: "NoData"}
        for code, name in expected.items():
            self.assertEqual(table[code], vegclasses.BY_NAME[name].id, code)
        self.assertEqual(table[255], vegclasses.BY_NAME["NoData"].id, "codice sconosciuto")
        self.assertEqual(table[15], vegclasses.BY_NAME["NoData"].id, "codice sconosciuto")

    def test_flags(self):
        classes = np.full((4, 4), vegclasses.BY_NAME["Grass"].id, dtype=np.uint8)
        self.assertEqual(vegclasses.tile_flags(classes), 0)
        classes[0, 0] = vegclasses.BY_NAME["Shrubs"].id
        self.assertEqual(vegclasses.tile_flags(classes), vegclasses.TILE_SHRUBS)
        classes[1, 1] = vegclasses.BY_NAME["Mangrove"].id
        self.assertEqual(vegclasses.tile_flags(classes), vegclasses.TILE_SHRUBS | vegclasses.TILE_TREES)

    def test_what_grows(self):
        for name in ("Water", "Snow", "Bare", "NoData", "Moss"):
            classes = np.full((4, 4), vegclasses.BY_NAME[name].id, dtype=np.uint8)
            self.assertFalse(vegclasses.grows_anything(classes), name)
        classes[2, 3] = vegclasses.BY_NAME["Crops"].id
        self.assertTrue(vegclasses.grows_anything(classes))


class Format(unittest.TestCase):

    def test_roundtrip(self):
        rng = np.random.default_rng(7)
        classes = rng.integers(0, len(vegclasses.CLASSES), (vegformat.RASTER, vegformat.RASTER), dtype=np.uint8)
        tile = vegformat.VegetationTile(13, 8541, 2044, vegclasses.tile_flags(classes), classes)
        blob = vegformat.encode_tile(tile)
        back = vegformat.decode_tile(blob)
        self.assertEqual((back.level, back.x, back.y, back.flags), (13, 8541, 2044, tile.flags))
        np.testing.assert_array_equal(back.classes, classes)

    def test_forest_compresses(self):
        blob = vegformat.fixture_bytes()
        self.assertLess(len(blob), vegformat.RASTER * vegformat.RASTER // 20)

    def test_rejects_damage(self):
        blob = vegformat.fixture_bytes()
        with self.assertRaises(ValueError):
            vegformat.decode_tile(blob[:20])
        with self.assertRaises(ValueError):
            vegformat.decode_tile(blob[:-3])
        with self.assertRaises(ValueError):
            vegformat.decode_tile(b"GWXX" + blob[4:])
        with self.assertRaises(ValueError):
            vegformat.encode_tile(vegformat.VegetationTile(13, 0, 0, 0, np.zeros((10, 10), dtype=np.uint8)))

    def test_index_roundtrip(self):
        folder = tempfile.mkdtemp()
        try:
            entries = [vegformat.VegetationTileEntry(5, 9, 1, 300), vegformat.VegetationTileEntry(4, 9, 3, 200),
                       vegformat.VegetationTileEntry(7, 2, 0, 100)]
            vegformat.write_index(folder, 13, entries)
            back = vegformat.read_index(folder, 13)
            self.assertEqual([(entry.x, entry.y) for entry in back], [(7, 2), (4, 9), (5, 9)], "ordinate per (y, x)")
            self.assertEqual([(entry.flags, entry.size) for entry in back], [(0, 100), (3, 200), (1, 300)])
            # L'indice del 13 messo nella cartella del 12: il livello scritto dentro non torna.
            os.makedirs(os.path.join(folder, "12"))
            shutil.copy(os.path.join(folder, "13", vegformat.INDEX_FILENAME),
                        os.path.join(folder, "12", vegformat.INDEX_FILENAME))
            with self.assertRaises(ValueError):
                vegformat.read_index(folder, 12)
        finally:
            shutil.rmtree(folder)


class WorldCoverNames(unittest.TestCase):

    def test_names(self):
        self.assertEqual(vegbuild.worldcover_tile_name(45, 6), "N45E006")
        self.assertEqual(vegbuild.worldcover_tile_name(-3, -60), "S03W060")
        self.assertEqual(vegbuild.worldcover_tile_name(0, -3), "N00W003")

    def test_bbox(self):
        # Torino: tutto dentro N45E006.
        self.assertEqual(vegbuild.worldcover_tile_names(7.5, 45.0, 7.9, 45.2), ["N45E006"])
        # A cavallo di 9 E e 45 N: quattro file, dal sud.
        self.assertEqual(vegbuild.worldcover_tile_names(8.5, 44.5, 9.5, 45.5),
                         ["N42E006", "N42E009", "N45E006", "N45E009"])
        # Un bordo esatto non tira dentro il file accanto.
        self.assertEqual(vegbuild.worldcover_tile_names(6.0, 45.0, 9.0, 48.0), ["N45E006"])
        # Emisfero sud e ovest.
        self.assertEqual(vegbuild.worldcover_tile_names(-1.0, -1.0, 1.0, 1.0),
                         ["S03W003", "S03E000", "N00W003", "N00E000"])


@unittest.skipUnless(HAS_GDAL, "serve GDAL (osgeo)")
class Build(unittest.TestCase):
    """
    Un WorldCover finto attorno a Torino, a 0,0005 gradi (~50 m):
      - ovest di 7,70 E: bosco (10);
      - est: prato (30);
      - a nord di 45,15 N e a est di 7,80 E: un lago (80), dove non cresce niente.
    Livello 13 come il vero, blocchi del livello 11 (4 x 4 tile) per averne piu' d'uno.
    """

    LEVEL = 13
    BLOCK_LEVEL = 11
    BBOX = (7.62, 45.02, 7.86, 45.19)

    @classmethod
    def setUpClass(cls):
        from osgeo import gdal, osr
        cls.folder = tempfile.mkdtemp()
        cls.source = os.path.join(cls.folder, "worldcover")
        os.makedirs(cls.source)
        west, north, step = 7.5, 45.3, 0.0005
        width, height = 800, 800
        lon = west + (np.arange(width) + 0.5) * step
        lat = north - (np.arange(height) + 0.5) * step
        lon_grid, lat_grid = np.meshgrid(lon, lat)
        codes = np.where(lon_grid < 7.70, 10, 30).astype(np.uint8)
        codes[(lat_grid > 45.15) & (lon_grid > 7.80)] = 80
        path = os.path.join(cls.source, "ESA_WorldCover_10m_2021_v200_N45E006_Map.tif")
        dataset = gdal.GetDriverByName("GTiff").Create(path, width, height, 1, gdal.GDT_Byte)
        dataset.SetGeoTransform((west, step, 0.0, north, 0.0, -step))
        reference = osr.SpatialReference()
        reference.ImportFromEPSG(4326)
        dataset.SetProjection(reference.ExportToWkt())
        dataset.GetRasterBand(1).WriteArray(codes)
        dataset.GetRasterBand(1).SetNoDataValue(0)
        dataset.FlushCache()
        dataset = None
        cls.output = os.path.join(cls.folder, "vegetazione")
        cls.lines = []
        cls.result = vegbuild.build(output=cls.output, bbox=cls.BBOX, worldcover=cls.source, name="prova",
                                    level=cls.LEVEL, block_level=cls.BLOCK_LEVEL, report=cls.lines.append)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.folder)

    def expected_tiles(self):
        x0, y0, x1, y1 = tiling.tile_range_for_bbox(self.LEVEL, *self.BBOX)
        return {(x, y) for x in range(x0, x1 + 1) for y in range(y0, y1 + 1)}

    def test_tiles_and_classes(self):
        trees, grass, water = (vegclasses.BY_NAME[name].id for name in ("Trees", "Grass", "Water"))
        entries = vegformat.read_index(self.output, self.LEVEL)
        written = {(entry.x, entry.y) for entry in entries}
        self.assertTrue(written)
        self.assertTrue(written <= self.expected_tiles())
        for entry in entries:
            tile = vegformat.read_tile(vegformat.tile_path(self.output, self.LEVEL, entry.x, entry.y))
            bounds = tiling.tile_bounds(self.LEVEL, entry.x, entry.y)
            self.assertEqual(tile.flags, entry.flags)
            if bounds.east < 7.70:
                self.assertTrue(np.all(tile.classes == trees), f"{entry.x},{entry.y} tutto bosco")
                self.assertEqual(tile.flags, vegclasses.TILE_TREES)
            elif bounds.west > 7.70 and (bounds.north < 45.15 or bounds.east < 7.80):
                self.assertTrue(np.all(tile.classes == grass), f"{entry.x},{entry.y} tutto prato")
                self.assertEqual(tile.flags, 0)
            elif bounds.west < 7.70 < bounds.east:
                # Il confine bosco/prato: a ovest il bosco (colonna 0 = ovest), a nord come a sud.
                self.assertEqual(tile.classes[128, 0], trees)
                self.assertEqual(tile.classes[128, -1], grass)
        # Le tile tutte lago non ci sono.
        for x, y in self.expected_tiles():
            bounds = tiling.tile_bounds(self.LEVEL, x, y)
            if bounds.south > 45.152 and bounds.west > 7.802:
                self.assertNotIn((x, y), written, "una tile tutta acqua non si scrive")
        # Una tile mezza lago: il lago sta a NORD (riga 0 = nord).
        for entry in entries:
            bounds = tiling.tile_bounds(self.LEVEL, entry.x, entry.y)
            if bounds.south < 45.15 < bounds.north and bounds.west > 7.81:
                tile = vegformat.read_tile(vegformat.tile_path(self.output, self.LEVEL, entry.x, entry.y))
                self.assertEqual(tile.classes[0, 128], water)
                self.assertEqual(tile.classes[-1, 128], grass)

    def test_manifest(self):
        manifest = vegformat.read_manifest(self.output)
        self.assertEqual(manifest["datasetKind"], "vegetation")
        self.assertEqual(manifest["level"], self.LEVEL)
        self.assertEqual(manifest["tiles"], self.result["tiles"])
        self.assertEqual(manifest["classes"]["Trees"], vegclasses.BY_NAME["Trees"].id)
        self.assertIn("CC BY 4.0", manifest["source"]["license"])
        cover = manifest["cover"]
        self.assertAlmostEqual(sum(cover.values()), 1.0, places=2)
        self.assertGreater(cover["Trees"], 0.1)
        self.assertGreater(cover["Grass"], 0.3)

    def test_verify(self):
        self.assertEqual(vegbuild.verify(self.output, report=lambda _: None), 0)

    def test_resume_and_force(self):
        again = vegbuild.build(output=self.output, bbox=self.BBOX, worldcover=self.source, name="prova",
                               level=self.LEVEL, block_level=self.BLOCK_LEVEL, report=lambda _: None)
        self.assertEqual(again["written"], 0, "i blocchi fatti si saltano")
        self.assertEqual(again["tiles"], self.result["tiles"])
        forced = vegbuild.build(output=self.output, bbox=self.BBOX, worldcover=self.source, name="prova",
                                level=self.LEVEL, block_level=self.BLOCK_LEVEL, force=True, report=lambda _: None)
        self.assertEqual(forced["written"], self.result["written"])
        self.assertEqual(vegbuild.verify(self.output, report=lambda _: None), 0)

    def test_verify_finds_damage(self):
        entries = vegformat.read_index(self.output, self.LEVEL)
        path = vegformat.tile_path(self.output, self.LEVEL, entries[0].x, entries[0].y)
        with open(path, "rb") as handle:
            original = handle.read()
        try:
            with open(path, "wb") as handle:
                handle.write(original[:-5])
            self.assertGreater(vegbuild.verify(self.output, report=lambda _: None), 0)
        finally:
            with open(path, "wb") as handle:
                handle.write(original)

    def test_bad_block_level(self):
        with self.assertRaises(ValueError):
            vegbuild.build(output=self.output, bbox=self.BBOX, worldcover=self.source,
                           level=10, block_level=11, report=lambda _: None)


if __name__ == "__main__":
    unittest.main()

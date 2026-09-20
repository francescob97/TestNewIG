"""Formato delle tile di immagine, riduzione della piramide, drappeggio."""

import os
import sys
import tempfile
import unittest

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from geoworld import imagecut, imageformat, imagerymanifest, tiling

# Pillow serve solo a comprimere e decomprimere il payload. Senza, i test che
# toccano un JPEG SALTANO invece di fallire: e' la stessa scelta fatta per la
# griglia geoidica in test_geoid.py. Un test rosso deve voler dire "il codice e'
# sbagliato", non "ti manca una libreria opzionale".
try:
    from PIL import Image as _PillowImage
    HAS_PILLOW = True
except ImportError:
    HAS_PILLOW = False

NEEDS_PILLOW = "serve Pillow: conda install -c conda-forge pillow"


def gradient_tile(seed: int = 0) -> np.ndarray:
    """Un'immagine riconoscibile: rosso verso est, verde verso sud, blu fisso."""
    side = imageformat.TILE_PIXELS
    tile = np.zeros((side, side, 3), dtype=np.uint8)
    columns = (np.arange(side) * 255 // (side - 1)).astype(np.uint8)
    tile[:, :, 0] = columns[np.newaxis, :]
    tile[:, :, 1] = columns[:, np.newaxis]
    tile[:, :, 2] = seed % 256
    return tile


class TestHeaderStructure(unittest.TestCase):
    """Gli invarianti del formato: non serve comprimere niente per provarli."""

    def test_header_is_32_bytes(self):
        self.assertEqual(imageformat.HEADER_STRUCT.size, imageformat.HEADER_SIZE)
        self.assertEqual(imageformat.HEADER_SIZE, 32)

    def test_tile_is_256_pixels(self):
        self.assertEqual(imageformat.TILE_PIXELS, 256)
        self.assertEqual(imageformat.TILE_PIXELS, 2 * tiling.TILE_CELLS)

    def test_fill_tile_needs_only_numpy(self):
        tile = imageformat.fill_tile()
        self.assertEqual(tile.shape, (256, 256, 3))
        self.assertTrue((tile[0, 0] == imageformat.FILL_COLOUR).all())


@unittest.skipUnless(HAS_PILLOW, NEEDS_PILLOW)
class TestHeader(unittest.TestCase):

    def test_round_trip(self):
        pixels = gradient_tile()
        blob = imageformat.encode_tile(13, 4332, 1012, pixels, 100.0)
        header, decoded = imageformat.decode_tile(blob)

        self.assertEqual((header.level, header.tile_x, header.tile_y), (13, 4332, 1012))
        self.assertEqual(header.payload_type, imageformat.PAYLOAD_JPEG)
        self.assertEqual(header.coverage_percent, 100)
        self.assertFalse(header.has_filled_pixels)
        self.assertEqual(decoded.shape, (256, 256, 3))

    def test_jpeg_is_lossy_but_faithful(self):
        """La compressione perde qualcosa, ma non deve stravolgere i colori."""
        pixels = gradient_tile()
        blob = imageformat.encode_tile(10, 1, 1, pixels, 100.0)
        _, decoded = imageformat.decode_tile(blob)

        error = np.abs(decoded.astype(np.int16) - pixels.astype(np.int16))
        self.assertLess(error.mean(), 3.0, "errore medio JPEG troppo alto")
        self.assertLess(np.percentile(error, 99), 12.0)

    def test_partial_coverage_sets_the_flag(self):
        blob = imageformat.encode_tile(10, 1, 1, gradient_tile(), 62.5)
        header = imageformat.decode_header(blob)
        self.assertEqual(header.coverage_percent, 62)
        self.assertTrue(header.has_filled_pixels)

    def test_rejects_wrong_shape(self):
        with self.assertRaises(ValueError):
            imageformat.encode_tile(0, 0, 0, np.zeros((128, 128, 3), np.uint8), 100.0)

    def test_rejects_a_height_tile(self):
        """Un file di quote dato al lettore di immagini deve fallire subito."""
        from geoworld import tileformat
        heights = np.zeros((tiling.TILE_POSTS, tiling.TILE_POSTS), dtype=np.float32)
        blob = tileformat.encode_tile(5, 1, 1, heights, False)
        with self.assertRaises(ValueError) as caught:
            imageformat.decode_header(blob)
        self.assertIn("magic", str(caught.exception))

    def test_atomic_write_leaves_no_temporary(self):
        with tempfile.TemporaryDirectory() as directory:
            path = os.path.join(directory, "3", "1", "2.gim")
            imageformat.write_tile_atomic(
                path, imageformat.encode_tile(3, 1, 2, gradient_tile(), 100.0))
            self.assertTrue(os.path.exists(path))
            self.assertFalse(os.path.exists(path + ".tmp"))


class TestReduction(unittest.TestCase):

    def test_halves_the_side(self):
        self.assertEqual(imageformat.reduce_2x2(gradient_tile()).shape, (128, 128, 3))

    def test_constant_stays_constant(self):
        flat = np.full((8, 8, 3), 200, np.uint8)
        self.assertTrue((imageformat.reduce_2x2(flat) == 200).all())

    def test_no_overflow_on_saturated_pixels(self):
        """Quattro pixel a 255 fanno 1020: in uint8 sarebbe overflow silenzioso."""
        white = np.full((4, 4, 3), 255, np.uint8)
        self.assertTrue((imageformat.reduce_2x2(white) == 255).all())

    def test_average_of_a_known_block(self):
        block = np.zeros((2, 2, 3), np.uint8)
        block[0, 0] = (0, 0, 0)
        block[0, 1] = (100, 100, 100)
        block[1, 0] = (200, 200, 200)
        block[1, 1] = (255, 255, 255)
        self.assertTrue((imageformat.reduce_2x2(block)[0, 0] == 139).all())  # 555/4

    def test_rejects_odd_sides(self):
        with self.assertRaises(ValueError):
            imageformat.reduce_2x2(np.zeros((5, 4, 3), np.uint8))


@unittest.skipUnless(HAS_PILLOW, NEEDS_PILLOW)
class TestPyramidWithoutGdal(unittest.TestCase):
    """
    Costruisce una piramide vera senza toccare GDAL.

    Si scrivono a mano le tile del livello fine, poi si lascia che il codice
    della pipeline riduca verso l'alto. E' il passo 2 della pipeline, cioe'
    tutto quello che non dipende dalla sorgente.
    """

    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.root = self.directory.name
        self.addCleanup(self.directory.cleanup)

        self.fine_level = 6
        self.children = []
        for x in range(10, 14):
            for y in range(4, 8):
                pixels = gradient_tile(seed=x * 16 + y)
                path = os.path.join(
                    self.root, imageformat.tile_relative_path(self.fine_level, x, y))
                imageformat.write_tile_atomic(
                    path, imageformat.encode_tile(self.fine_level, x, y, pixels, 100.0))
                self.children.append((x, y, 100))

    def test_builds_the_parent_level(self):
        stats = imagecut.ImageryStats()
        parents = imagecut.build_level_from_children(
            self.root, self.fine_level - 1, self.children, 85, stats)

        # 4x4 figli -> 2x2 padri
        self.assertEqual(len(parents), 4)
        self.assertEqual(stats.tiles_written, 4)

        for x, y, coverage in parents:
            self.assertEqual(coverage, 100)
            header, pixels = imageformat.read_tile(
                os.path.join(self.root,
                             imageformat.tile_relative_path(self.fine_level - 1, x, y)))
            self.assertEqual(header.level, self.fine_level - 1)
            self.assertEqual(pixels.shape, (256, 256, 3))

    def test_rerunning_does_not_rewrite(self):
        """La pipeline deve essere riavviabile: la seconda passata non riscrive."""
        first = imagecut.ImageryStats()
        imagecut.build_level_from_children(self.root, 5, self.children, 85, first)

        second = imagecut.ImageryStats()
        again = imagecut.build_level_from_children(self.root, 5, self.children, 85, second)

        self.assertEqual(second.tiles_written, 0, "ha riscritto tile gia' presenti")
        self.assertEqual(len(again), 4, "ha perso le voci di indice delle tile esistenti")

    def test_missing_children_become_fill(self):
        partial = [c for c in self.children if (c[0], c[1]) != (10, 4)]
        stats = imagecut.ImageryStats()
        imagecut.build_level_from_children(self.root, 5, partial, 85, stats)

        _, pixels = imageformat.read_tile(
            os.path.join(self.root, imageformat.tile_relative_path(5, 5, 2)))
        corner = pixels[:128, :128].reshape(-1, 3).mean(axis=0)
        self.assertLess(np.abs(corner - imageformat.FILL_COLOUR).max(), 12,
                        "il quadrante del figlio mancante non e' riempimento")

    def test_index_round_trip(self):
        entries = [imagerymanifest.ImageTileEntry(x, y, c) for x, y, c in self.children]
        imagerymanifest.write_level_index(self.root, self.fine_level, entries)
        back = imagerymanifest.read_level_index(self.root, self.fine_level)

        self.assertEqual(len(back), len(entries))
        # L'indice e' ordinato per (y, x), non per (x, y): e' l'ordine che
        # permette al runtime la ricerca binaria riga per riga.
        self.assertEqual([(e.y, e.x) for e in back],
                         sorted((e.y, e.x) for e in entries))

    def test_index_refuses_a_height_index(self):
        from geoworld import manifest as height_manifest
        height_manifest.write_level_index(
            self.root, 9, [height_manifest.TileEntry(1, 1, 0.0, 10.0)])
        with self.assertRaises(ValueError) as caught:
            imagerymanifest.read_level_index(self.root, 9)
        self.assertIn("quote", str(caught.exception))


class TestResolution(unittest.TestCase):

    def test_imagery_is_twice_as_fine_as_terrain_at_the_same_level(self):
        """256 pixel contro 128 celle: e' il fatto su cui poggia il drappeggio."""
        for level in range(4, 18):
            image = imagecut.imagery_pixel_size_deg(level)
            terrain = tiling.post_spacing_deg(level)
            self.assertAlmostEqual(image * 2.0, terrain, places=12)

    def test_sentinel2_is_native_at_level_13(self):
        self.assertEqual(imagecut.recommended_max_level(10.0, 45.0), 13)

    def test_thirty_centimetre_orthophoto_reaches_level_18(self):
        self.assertEqual(imagecut.recommended_max_level(0.30, 45.0), 18)

    def test_finer_source_never_gives_a_coarser_level(self):
        previous = -1
        for resolution in [100.0, 30.0, 10.0, 5.0, 1.0, 0.5, 0.25]:
            level = imagecut.recommended_max_level(resolution, 45.0)
            self.assertGreaterEqual(level, previous)
            previous = level


class TestDrape(unittest.TestCase):
    """La matematica che sceglie e ritaglia l'immagine per una tile di terreno."""

    def test_same_level_is_the_identity(self):
        self.assertEqual(imagecut.uv_transform(12, 4333, 1014, 12), (0.0, 0.0, 1.0))

    def test_worked_example_from_the_design(self):
        self.assertEqual(imagecut.ancestor_for(12, 4333, 1014, 10), (1083, 253))
        offset_u, offset_v, scale = imagecut.uv_transform(12, 4333, 1014, 10)
        self.assertEqual((offset_u, offset_v, scale), (0.25, 0.5, 0.25))

    def test_the_four_children_tile_the_parent_exactly(self):
        """I quattro figli devono coprire il padre senza buchi ne' sovrapposizioni."""
        corners = set()
        for dx in (0, 1):
            for dy in (0, 1):
                u, v, scale = imagecut.uv_transform(9, 100 + dx, 50 + dy, 8)
                self.assertEqual(scale, 0.5)
                corners.add((u, v))
        self.assertEqual(corners, {(0.0, 0.0), (0.5, 0.0), (0.0, 0.5), (0.5, 0.5)})

    def test_uv_stays_inside_the_unit_square(self):
        for depth in range(0, 8):
            level = 10 + depth
            x, y = 12345, 6789
            u, v, scale = imagecut.uv_transform(level, x, y, 10)
            self.assertGreaterEqual(u, 0.0)
            self.assertGreaterEqual(v, 0.0)
            self.assertLessEqual(u + scale, 1.0 + 1e-12)
            self.assertLessEqual(v + scale, 1.0 + 1e-12)

    def test_ancestor_geography_contains_the_tile(self):
        """Prova geometrica: il rettangolo dell'antenato contiene quello della tile."""
        level, x, y = 14, 8901, 3456
        for ancestor_level in range(0, level + 1):
            ax, ay = imagecut.ancestor_for(level, x, y, ancestor_level)
            tile = tiling.tile_bounds(level, x, y)
            ancestor = tiling.tile_bounds(ancestor_level, ax, ay)
            self.assertLessEqual(ancestor.west, tile.west + 1e-9)
            self.assertLessEqual(tile.east, ancestor.east + 1e-9)
            self.assertLessEqual(ancestor.south, tile.south + 1e-9)
            self.assertLessEqual(tile.north, ancestor.north + 1e-9)

    def test_uv_matches_the_geography(self):
        """
        Il ritaglio calcolato con gli interi deve coincidere con quello che si
        otterrebbe misurando i rettangoli in gradi. E' il vero controllo:
        collega l'aritmetica della formula alla geografia del mondo.
        """
        level, x, y, ancestor_level = 13, 4333, 1014, 9
        ax, ay = imagecut.ancestor_for(level, x, y, ancestor_level)
        offset_u, offset_v, scale = imagecut.uv_transform(level, x, y, ancestor_level)

        tile = tiling.tile_bounds(level, x, y)
        ancestor = tiling.tile_bounds(ancestor_level, ax, ay)
        width = ancestor.east - ancestor.west
        height = ancestor.north - ancestor.south

        self.assertAlmostEqual((tile.west - ancestor.west) / width, offset_u, places=12)
        self.assertAlmostEqual((ancestor.north - tile.north) / height, offset_v, places=12)
        self.assertAlmostEqual((tile.east - tile.west) / width, scale, places=12)

    def test_rejects_an_ancestor_below_the_tile(self):
        with self.assertRaises(ValueError):
            imagecut.uv_transform(10, 5, 5, 12)


if __name__ == "__main__":
    unittest.main(verbosity=2)

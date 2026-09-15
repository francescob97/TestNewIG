"""Formato binario delle tile."""
import os
import tempfile
import unittest

import numpy as np

from geoworld import tileformat, tiling


class TestTileFormat(unittest.TestCase):

    def make_heights(self) -> np.ndarray:
        rows = np.arange(tiling.TILE_POSTS, dtype=np.float32)[:, np.newaxis]
        columns = np.arange(tiling.TILE_POSTS, dtype=np.float32)[np.newaxis, :]
        return (rows * 3.25 - columns * 1.5 + 100.0).astype(np.float32)

    def test_round_trip(self):
        heights = self.make_heights()
        header, decoded = tileformat.decode_tile(
            tileformat.encode_tile(14, 17509, 4373, heights, has_filled_posts=True))

        self.assertEqual(header.level, 14)
        self.assertEqual(header.tile_x, 17509)
        self.assertEqual(header.tile_y, 4373)
        self.assertEqual((header.width, header.height),
                         (tiling.TILE_POSTS, tiling.TILE_POSTS))
        self.assertTrue(header.has_filled_posts)
        self.assertAlmostEqual(header.min_height, float(heights.min()), places=4)
        self.assertAlmostEqual(header.max_height, float(heights.max()), places=4)
        np.testing.assert_array_equal(decoded, heights)

    def test_size_is_exactly_as_documented(self):
        blob = tileformat.encode_tile(0, 0, 0, self.make_heights(), has_filled_posts=False)
        self.assertEqual(len(blob), tileformat.TILE_BYTES)
        self.assertEqual(len(blob), 32 + 129 * 129 * 4)

    def test_nan_is_rejected(self):
        """Un NaN che arriva fin qui e' un bug a monte. Deve fermarsi, non
        propagarsi nel runtime dove sarebbe molto piu' difficile da trovare."""
        heights = self.make_heights()
        heights[64, 64] = np.nan
        with self.assertRaises(ValueError):
            tileformat.encode_tile(0, 0, 0, heights, has_filled_posts=False)

    def test_corruption_is_detected(self):
        blob = bytearray(tileformat.encode_tile(0, 0, 0, self.make_heights(), False))
        blob[0:4] = b"XXXX"
        with self.assertRaises(ValueError):
            tileformat.decode_tile(bytes(blob))

        truncated = tileformat.encode_tile(0, 0, 0, self.make_heights(), False)[:-100]
        with self.assertRaises(ValueError):
            tileformat.decode_tile(truncated)

    def test_atomic_write_leaves_no_partial_file(self):
        with tempfile.TemporaryDirectory() as directory:
            path = os.path.join(directory, "3", "1", "2.ght")
            blob = tileformat.encode_tile(3, 1, 2, self.make_heights(), False)
            tileformat.write_tile_atomic(path, blob)

            self.assertTrue(os.path.exists(path))
            self.assertEqual(os.path.getsize(path), tileformat.TILE_BYTES)
            self.assertFalse(os.path.exists(path + ".tmp"))

            header = tileformat.read_tile_header(path)
            self.assertEqual((header.level, header.tile_x, header.tile_y), (3, 1, 2))

    def test_path_layout(self):
        self.assertEqual(tileformat.tile_relative_path(14, 17509, 4373),
                         os.path.join("14", "17509", "4373.ght"))


if __name__ == "__main__":
    unittest.main()

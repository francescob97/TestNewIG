"""
Test della maschera delle nuvole, senza GDAL: la parte che decide QUALI pixel
togliere e come, su array costruiti a mano.
"""
import os
import sys
import unittest

import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from geoworld import cloudmask  # noqa: E402


class Classes(unittest.TestCase):

    def test_clouds_shadows_and_cirrus_go_away(self):
        scl = np.array([[3, 8, 9, 10]], dtype=np.uint8)
        self.assertTrue(cloudmask.cloud_mask(scl, dilation=0).all())

    def test_ground_water_and_snow_stay(self):
        # 4 vegetazione, 5 suolo, 6 acqua, 11 neve: sulle Alpi la neve e' vera.
        scl = np.array([[4, 5, 6, 11, 2, 7]], dtype=np.uint8)
        self.assertFalse(cloudmask.cloud_mask(scl, dilation=0).any())

    def test_nodata_and_defective_go_away(self):
        scl = np.array([[0, 1]], dtype=np.uint8)
        self.assertTrue(cloudmask.cloud_mask(scl, dilation=0).all())


class Dilation(unittest.TestCase):

    def test_a_single_cloud_pixel_grows_into_a_square(self):
        mask = np.zeros((9, 9), dtype=bool)
        mask[4, 4] = True
        grown = cloudmask.dilate(mask, 2)
        self.assertEqual(int(grown.sum()), 25)              # quadrato 5x5
        self.assertTrue(grown[2:7, 2:7].all())
        self.assertFalse(grown[1, 4] or grown[4, 7])

    def test_dilation_reaches_the_borders_without_wrapping(self):
        mask = np.zeros((5, 5), dtype=bool)
        mask[0, 0] = True
        grown = cloudmask.dilate(mask, 1)
        self.assertTrue(grown[:2, :2].all())
        self.assertFalse(grown[4, 4], "lo spostamento non deve rientrare dall'altro lato")

    def test_zero_radius_is_a_copy(self):
        mask = np.eye(4, dtype=bool)
        same = cloudmask.dilate(mask, 0)
        self.assertTrue((same == mask).all())
        self.assertIsNot(same, mask)


class Application(unittest.TestCase):

    def test_upsample_matches_tci_resolution(self):
        mask = np.array([[True, False]])
        up = cloudmask.upsample(mask, 2)
        self.assertEqual(up.shape, (2, 4))
        self.assertTrue(up[:, :2].all() and not up[:, 2:].any())

    def test_masked_pixels_become_nodata_and_valid_black_survives(self):
        rgb = np.full((3, 2, 2), 120, dtype=np.uint8)
        rgb[:, 1, 1] = 0                                      # nero VERO, valido
        mask = np.array([[True, False], [False, False]])
        out = cloudmask.apply_mask(rgb, mask)
        self.assertTrue((out[:, 0, 0] == 0).all(), "il pixel nuvoloso diventa 'nessun dato'")
        self.assertTrue((out[:, 1, 1] == 1).all(), "il nero valido diventa 1, per non sparire")
        self.assertTrue((out[:, 0, 1] == 120).all())
        self.assertTrue((rgb[:, 0, 0] == 120).all(), "l'originale non si tocca")

    def test_fraction(self):
        scl = np.full((10, 10), 4, dtype=np.uint8)
        scl[:5, :] = 9
        self.assertAlmostEqual(cloudmask.masked_fraction(scl, dilation=0), 0.5)


if __name__ == "__main__":
    unittest.main()

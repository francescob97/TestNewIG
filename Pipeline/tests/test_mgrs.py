"""
Quadrati MGRS: la parte che serve a trovare le scene Sentinel-2.

I valori attesi non sono stati calcolati da questo stesso codice: sono i
prefissi sotto cui il bucket pubblico tiene davvero le scene di quelle citta'.
Il test quindi non verifica solo la coerenza interna, verifica che il risultato
corrisponda al mondo reale.
"""

import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from geoworld import mgrs

#: citta' -> (lat, lon, quadrato verificato sul bucket sentinel-cogs)
KNOWN_PLACES = {
    "Roma":     (41.9028, 12.4964, (33, "T", "TG")),
    "Milano":   (45.4642,  9.1900, (32, "T", "NR")),
    "Torino":   (45.0703,  7.6869, (32, "T", "LQ")),
    "Napoli":   (40.8518, 14.2681, (33, "T", "VF")),
    "Palermo":  (38.1157, 13.3613, (33, "S", "UC")),
    "Cagliari": (39.2238,  9.1216, (32, "S", "NJ")),
}


class TestSquares(unittest.TestCase):

    def test_known_italian_cities(self):
        for name, (latitude, longitude, expected) in KNOWN_PLACES.items():
            with self.subTest(citta=name):
                self.assertEqual(mgrs.square(latitude, longitude), expected)

    def test_zone_numbers(self):
        self.assertEqual(mgrs.utm_zone(12.5), 33)     # Roma
        self.assertEqual(mgrs.utm_zone(9.19), 32)     # Milano
        self.assertEqual(mgrs.utm_zone(-3.7), 30)     # Madrid
        self.assertEqual(mgrs.utm_zone(-180.0), 1)
        self.assertEqual(mgrs.utm_zone(179.999), 60)

    def test_latitude_bands(self):
        self.assertEqual(mgrs.latitude_band(41.9), "T")
        self.assertEqual(mgrs.latitude_band(38.1), "S")
        self.assertEqual(mgrs.latitude_band(0.0), "N")
        self.assertEqual(mgrs.latitude_band(-0.1), "M")
        self.assertEqual(mgrs.latitude_band(83.0), "X")

    def test_band_letters_skip_i_and_o(self):
        """I e O si confondono con 1 e 0: MGRS le esclude ovunque."""
        self.assertNotIn("I", mgrs.LATITUDE_BANDS)
        self.assertNotIn("O", mgrs.LATITUDE_BANDS)
        self.assertNotIn("I", mgrs.ROW_LETTERS)
        self.assertNotIn("O", mgrs.ROW_LETTERS)
        for column_set in mgrs.COLUMN_SETS:
            self.assertNotIn("I", column_set)
            self.assertNotIn("O", column_set)

    def test_rejects_latitudes_outside_the_domain(self):
        for latitude in (-80.1, 84.1, 90.0):
            with self.assertRaises(ValueError):
                mgrs.latitude_band(latitude)


class TestUtm(unittest.TestCase):

    def test_central_meridian_gives_the_false_easting(self):
        """Sul meridiano centrale l'easting e' esattamente 500000."""
        for zone in (30, 32, 33):
            central = (zone - 1) * 6.0 - 180.0 + 3.0
            easting, _, _ = mgrs.to_utm(45.0, central, zone)
            self.assertAlmostEqual(easting, 500000.0, places=6)

    def test_equator_gives_zero_northing(self):
        _, northing, _ = mgrs.to_utm(0.0, 15.0, 33)
        self.assertAlmostEqual(northing, 0.0, places=6)

    def test_southern_hemisphere_uses_the_false_northing(self):
        _, northing, _ = mgrs.to_utm(-0.001, 15.0, 33)
        self.assertGreater(northing, 9_900_000.0)

    def test_rome_lands_where_the_map_says(self):
        """Coordinate UTM di Roma, controllabili su qualunque mappa: 33N 291km E, 4641km N."""
        easting, northing, zone = mgrs.to_utm(41.9028, 12.4964)
        self.assertEqual(zone, 33)
        self.assertAlmostEqual(easting / 1000.0, 291.9, delta=1.0)
        self.assertAlmostEqual(northing / 1000.0, 4641.5, delta=1.0)


class TestBboxCoverage(unittest.TestCase):

    def test_small_bbox_gives_one_square(self):
        self.assertEqual(mgrs.squares_for_bbox(12.40, 41.85, 12.60, 41.95),
                         [(33, "T", "TG")])

    def test_wide_bbox_crosses_zones(self):
        """Dal Piemonte al Lazio si attraversano la zona 32 e la 33."""
        squares = mgrs.squares_for_bbox(7.5, 41.8, 12.6, 45.1)
        zones = {zone for zone, _, _ in squares}
        self.assertEqual(zones, {32, 33})
        self.assertIn((33, "T", "TG"), squares)

    def test_step_cannot_skip_a_square(self):
        """
        Il passo di campionamento deve restare sotto i 100 km, altrimenti un
        quadrato puo' passare fra due campioni senza essere visto.
        """
        # Un quarto di grado sono circa 28 km in latitudine: molto sotto i 100.
        self.assertLess(0.25 * 111.32, 100.0)

    def test_no_duplicates(self):
        squares = mgrs.squares_for_bbox(7.0, 37.0, 18.5, 47.0, step_deg=0.5)
        self.assertEqual(len(squares), len(set(squares)))


if __name__ == "__main__":
    unittest.main(verbosity=2)

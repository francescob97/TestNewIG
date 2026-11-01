"""
Test delle stelle (Fase 12): il catalogo di Yale letto a colonne fisse, il
file .gst, il comando senza rete (con un catalogo finto scritto qui).
"""

import gzip
import os
import struct
import tempfile
import unittest

from geoworld import cli, starbuild

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
FIXTURE = os.path.join(REPO, "Plugins", "GeoWorld", "Source", "GeoTiles", "TestData", "stars_fixture.gst")


def catalog_line(hr: int, ra_h: int, ra_m: int, ra_s: float, sign: str, dec_d: int, dec_m: int, dec_s: int,
                 vmag: str, bv: str) -> str:
    """Una riga del catalogo V/50 con i campi dove li mette il ReadMe (colonne da 1)."""
    line = [" "] * 197

    def put(first: int, text: str) -> None:
        for offset, char in enumerate(text):
            line[first - 1 + offset] = char

    put(1, f"{hr:4d}")
    put(5, "Prova     ")
    if ra_h >= 0:
        put(76, f"{ra_h:02d}")
        put(78, f"{ra_m:02d}")
        put(80, f"{ra_s:04.1f}")
        put(84, sign)
        put(85, f"{dec_d:02d}")
        put(87, f"{dec_m:02d}")
        put(89, f"{dec_s:02d}")
    put(103, f"{vmag:>5s}")
    put(110, f"{bv:>5s}")
    return "".join(line)


SIRIUS = catalog_line(2491, 6, 45, 8.9, "-", 16, 42, 58, "-1.46", " 0.00")
VEGA = catalog_line(7001, 18, 36, 56.3, "+", 38, 47, 1, " 0.03", " 0.00")
POLARIS = catalog_line(424, 2, 31, 48.7, "+", 89, 15, 51, " 2.02", " 0.60")
FAINT = catalog_line(9000, 23, 0, 0.0, "+", 10, 0, 0, " 6.20", "")
NO_POSITION = catalog_line(92, -1, 0, 0.0, "+", 0, 0, 0, " 6.00", " 0.10")


class Catalog(unittest.TestCase):

    def test_parse_line(self):
        star = starbuild.parse_catalog_line(SIRIUS)
        self.assertAlmostEqual(star.ra_deg, (6 + 45 / 60 + 8.9 / 3600) * 15, places=6)
        self.assertAlmostEqual(star.dec_deg, -(16 + 42 / 60 + 58 / 3600), places=6)
        self.assertEqual(star.magnitude, -1.46)
        self.assertEqual(star.colour_index, 0.0)

    def test_northern_and_missing_colour(self):
        self.assertGreater(starbuild.parse_catalog_line(POLARIS).dec_deg, 89.0)
        self.assertEqual(starbuild.parse_catalog_line(FAINT).colour_index, starbuild.DEFAULT_COLOUR_INDEX)

    def test_entries_without_position_are_skipped(self):
        self.assertIsNone(starbuild.parse_catalog_line(NO_POSITION))
        self.assertIsNone(starbuild.parse_catalog_line("troppo corta"))

    def test_read_sorts_and_cuts(self):
        with tempfile.TemporaryDirectory() as directory:
            path = os.path.join(directory, "catalog.gz")
            with gzip.open(path, "wt", encoding="latin-1") as handle:
                handle.write("\n".join([POLARIS, FAINT, NO_POSITION, VEGA, SIRIUS]) + "\n")
            stars = starbuild.read_catalog(path)
            self.assertEqual([round(s.magnitude, 2) for s in stars], [-1.46, 0.03, 2.02, 6.2])
            self.assertEqual(len(starbuild.read_catalog(path, max_magnitude=2.5)), 3)


class Format(unittest.TestCase):

    def test_sizes(self):
        self.assertEqual(starbuild.HEADER.size, 16)
        self.assertEqual(starbuild.RECORD.size, 12)

    def test_round_trip(self):
        stars = starbuild.fixture_stars()
        decoded = starbuild.decode(starbuild.encode(stars))
        self.assertEqual(len(decoded), len(stars))
        for original, back in zip(stars, decoded):
            self.assertAlmostEqual(original.ra_deg, back.ra_deg, places=4)
            self.assertAlmostEqual(original.dec_deg, back.dec_deg, places=4)
            self.assertAlmostEqual(original.magnitude, back.magnitude, places=6)
            self.assertAlmostEqual(original.colour_index, back.colour_index, places=6)

    def test_fixture_is_sorted_by_brightness(self):
        magnitudes = [star.magnitude for star in starbuild.fixture_stars()]
        self.assertEqual(magnitudes, sorted(magnitudes))

    def test_rejects_garbage(self):
        with self.assertRaises(ValueError):
            starbuild.decode(b"GWVT" + bytes(12))
        with self.assertRaises(ValueError):
            starbuild.decode(starbuild.fixture_bytes()[:-1])
        bad_version = bytearray(starbuild.fixture_bytes())
        struct.pack_into("<H", bad_version, 4, 9)
        with self.assertRaises(ValueError):
            starbuild.decode(bytes(bad_version))

    def test_fixture_is_up_to_date(self):
        # Il test C++ (geosky_tests) legge questo file: se il formato cambia qui va rigenerato.
        with open(FIXTURE, "rb") as handle:
            self.assertEqual(handle.read(), starbuild.fixture_bytes(),
                             "stars_fixture.gst non e' aggiornato: rigeneralo con starbuild.fixture_bytes()")


class Command(unittest.TestCase):

    def test_build_from_local_catalog(self):
        with tempfile.TemporaryDirectory() as directory:
            catalog = os.path.join(directory, "catalog.gz")
            with gzip.open(catalog, "wt", encoding="latin-1") as handle:
                handle.write("\n".join([VEGA, SIRIUS, POLARIS]) + "\n")
            output = os.path.join(directory, "cielo", "stars.gst")
            code = cli.main(["build-stars", "-o", output, "--catalog", catalog])
            self.assertEqual(code, 0)
            with open(output, "rb") as handle:
                stars = starbuild.decode(handle.read())
            self.assertEqual(len(stars), 3)
            self.assertEqual(stars[0].magnitude, -1.46)

    def test_missing_catalog_is_an_error(self):
        with tempfile.TemporaryDirectory() as directory:
            code = cli.main(["build-stars", "-o", os.path.join(directory, "stars.gst"),
                             "--catalog", os.path.join(directory, "non-c-e.gz")])
            self.assertEqual(code, 1)


if __name__ == "__main__":
    unittest.main()

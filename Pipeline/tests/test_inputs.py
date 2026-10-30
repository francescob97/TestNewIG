"""
I file sorgente dai pattern della riga di comando.

PERCHE' QUESTO TEST ESISTE
I DTED arrivano quasi sempre in una cartella per longitudine
(dted/E016/N38.dt2, dted/E017/N38.dt2...), con lo STESSO nome in cartelle
diverse. Il glob di Python senza recursive=True legge "**" come "*": un
livello solo, e una cartella organizzata piu' a fondo perdeva i file in
silenzio. Il test costruisce tre profondita' e li vuole tutti.
"""

import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from geoworld import cli  # noqa: E402


class SourcePatterns(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        root = self.temp.name
        self.files = [
            os.path.join(root, "N37.dt2"),
            os.path.join(root, "E016", "N38.dt2"),
            os.path.join(root, "E017", "N38.dt2"),          # stesso nome, altra longitudine
            os.path.join(root, "dted", "E018", "N39.dt2"),  # due livelli sotto
        ]
        for path in self.files:
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "wb") as handle:
                handle.write(b"x")
        with open(os.path.join(root, "E016", "leggimi.txt"), "w") as handle:
            handle.write("non e' un DEM")

    def tearDown(self):
        self.temp.cleanup()

    def test_double_star_finds_every_depth(self):
        found = cli.expand_source_patterns([os.path.join(self.temp.name, "**", "*.dt2")])
        self.assertEqual(found, sorted(self.files))

    def test_single_star_stays_one_level(self):
        found = cli.expand_source_patterns([os.path.join(self.temp.name, "*", "*.dt2")])
        self.assertEqual(found, sorted(self.files[1:3]))

    def test_overlapping_patterns_give_each_file_once(self):
        root = self.temp.name
        found = cli.expand_source_patterns([os.path.join(root, "**", "*.dt2"),
                                            os.path.join(root, "E016", "*.dt2")])
        self.assertEqual(found, sorted(self.files))

    def test_nothing_matches(self):
        self.assertEqual(cli.expand_source_patterns([os.path.join(self.temp.name, "*.tif")]), [])


if __name__ == "__main__":
    unittest.main(verbosity=2)

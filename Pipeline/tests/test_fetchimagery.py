"""
Test della scelta delle scene Sentinel-2 e dei suoi dintorni, SENZA rete.

Il caso guida e' quello vero di Torino (estate 2024), letto dal bucket il giorno
in cui il mosaico e' comparso sbagliato: la versione precedente sceglieva per
32TLQ una scena vuota al 37% e per 32TMQ una vuota al 58%, di giorni diversi.
"""
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from geoworld import fetchimagery, mgrs  # noqa: E402
from geoworld.fetchimagery import Scene, choose_scenes  # noqa: E402


def scene(square: str, date: str, cloud: float, nodata: float) -> Scene:
    zone, band, letters = int(square[:2]), square[2], square[3:]
    result = Scene(zone, band, letters, f"S2A_{square}_{date}_0_L2A",
                   int(date[:4]), int(date[4:6]))
    result.cloud_cover = cloud
    result.nodata = nodata
    return result


# I candidati veri di Torino, le cinque meno nuvolose per quadrato.
TORINO = {
    "32TLQ": [scene("32TLQ", "20240729", 5.5, 36.9),
              scene("32TLQ", "20240821", 6.0, 0.0),
              scene("32TLQ", "20240801", 6.5, 0.0),
              scene("32TLQ", "20240808", 7.4, 37.3),
              scene("32TLQ", "20240704", 8.2, 36.8)],
    "32TMQ": [scene("32TMQ", "20240722", 0.3, 57.9),
              scene("32TMQ", "20240729", 0.3, 0.0),
              scene("32TMQ", "20240803", 0.4, 0.0),
              scene("32TMQ", "20240813", 1.3, 0.0),
              scene("32TMQ", "20240801", 3.6, 58.2)],
}


class SceneChoice(unittest.TestCase):

    def test_torino_no_longer_picks_half_empty_scenes(self):
        choices = choose_scenes(TORINO, max_cloud=10.0, max_nodata=1.0)
        principal = {c.scene.square_key: c.scene for c in choices if c.role == "principale"}
        self.assertEqual(set(principal), {"32TLQ", "32TMQ"})
        for chosen in principal.values():
            self.assertLessEqual(chosen.nodata, 1.0,
                                 f"{chosen.name} e' vuota al {chosen.nodata}%")

    def test_old_rule_would_have_picked_the_empty_ones(self):
        # La regola vecchia (solo nuvole) sui dati veri: e' il bug che si vedeva.
        old = {key: min(scenes, key=lambda s: s.cloud_cover) for key, scenes in TORINO.items()}
        self.assertGreater(old["32TLQ"].nodata, 30.0)
        self.assertGreater(old["32TMQ"].nodata, 50.0)

    def test_same_day_is_preferred_when_it_exists(self):
        # Due quadrati con una scena piena lo STESSO giorno, anche se non la piu'
        # serena: deve vincere il giorno comune, che ha la stessa luce.
        candidates = {
            "32TLQ": [scene("32TLQ", "20240701", 1.0, 0.0), scene("32TLQ", "20240715", 4.0, 0.0)],
            "32TMQ": [scene("32TMQ", "20240702", 0.5, 0.0), scene("32TMQ", "20240715", 5.0, 0.0)],
        }
        choices = choose_scenes(candidates, max_cloud=10.0)
        dates = {c.scene.date for c in choices if c.role == "principale"}
        self.assertEqual(dates, {"20240715"})

    def test_filler_goes_below_and_only_when_needed(self):
        candidates = {
            "33TUG": [scene("33TUG", "20240710", 2.0, 45.0),     # mezza vuota
                      scene("33TUG", "20240720", 3.0, 50.0),     # l'altra meta'
                      scene("33TUG", "20240730", 30.0, 0.0)],    # piena ma nuvolosa
        }
        choices = choose_scenes(candidates, max_cloud=10.0, max_nodata=1.0)
        roles = [c.role for c in choices]
        self.assertEqual(roles[-1], "principale", "la principale deve stare SOPRA (ultima)")
        self.assertIn("riempimento", roles)
        self.assertLessEqual(roles.count("riempimento"), 2)
        dates = [c.scene.date for c in choices]
        self.assertEqual(len(dates), len(set(dates)), "niente riempimento dello stesso giorno")

    def test_full_scene_needs_no_filler(self):
        choices = choose_scenes({"32TMQ": TORINO["32TMQ"]}, max_cloud=10.0)
        self.assertEqual([c.role for c in choices], ["principale"])

    def test_empty_square_is_skipped(self):
        choices = choose_scenes({"32SMA": [], "32TMQ": TORINO["32TMQ"]}, max_cloud=10.0)
        self.assertEqual({c.scene.square_key for c in choices}, {"32TMQ"})

    def test_scene_date_comes_from_the_name(self):
        self.assertEqual(scene("32TLQ", "20240821", 0, 0).date, "20240821")


class ItalyArea(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        cls.squares = set(fetchimagery.squares_for_polygons(fetchimagery.AREA_POLYGONS["italia"]))

    def test_every_region_is_covered(self):
        cities = {
            "Torino": (45.07, 7.69), "Aosta": (45.74, 7.32), "Bolzano": (46.50, 11.35),
            "Trieste": (45.65, 13.78), "Venezia": (45.44, 12.33), "Genova": (44.41, 8.93),
            "Roma": (41.90, 12.50), "Napoli": (40.85, 14.27), "Bari": (41.12, 16.87),
            "Lecce": (40.35, 18.17), "Reggio Calabria": (38.11, 15.65),
            "Palermo": (38.12, 13.36), "Pachino": (36.71, 15.09), "Trapani": (38.02, 12.51),
            "Cagliari": (39.22, 9.12), "Olbia": (40.92, 9.50), "Alghero": (40.56, 8.32),
            "Elba": (42.77, 10.25), "Pantelleria": (36.83, 11.95), "Lampedusa": (35.50, 12.60),
        }
        missing = [name for name, (lat, lon) in cities.items()
                   if mgrs.square(lat, lon) not in self.squares]
        self.assertEqual(missing, [])

    def test_open_sea_is_excluded(self):
        # Mezzo Tirreno e mezzo Ionio: niente terra per cento chilometri.
        self.assertNotIn(mgrs.square(40.0, 11.5), self.squares)
        self.assertNotIn(mgrs.square(38.5, 18.5), self.squares)

    def test_size_is_reasonable(self):
        # L'Italia sono ~300.000 km2, un quadrato ~10.000: con le coste, qualche
        # decina in piu'. Meno di 60 vorrebbe dire che manca qualcosa, piu' di
        # 150 che si sta scaricando il mare.
        self.assertGreater(len(self.squares), 60)
        self.assertLess(len(self.squares), 150)


class OrderFile(unittest.TestCase):

    def test_order_file_round_trip(self):
        from geoworld import imagerybuild
        with tempfile.TemporaryDirectory() as directory:
            files = []
            for name in ("b_riempimento_TCI.tif", "a_principale_TCI.tif"):
                path = os.path.join(directory, name)
                open(path, "wb").close()
                files.append(path)
            order = fetchimagery.write_order_file(directory, files)
            expanded = imagerybuild.expand_inputs(["@" + order])
            # L'ordine resta quello scritto, NON quello alfabetico.
            self.assertEqual([os.path.basename(p) for p in expanded],
                             ["b_riempimento_TCI.tif", "a_principale_TCI.tif"])

    def test_vsicurl_entries_survive(self):
        from geoworld import imagerybuild
        with tempfile.TemporaryDirectory() as directory:
            remote = "/vsicurl/https://example.org/S2A_32TMQ_20240729_0_L2A/TCI.tif"
            order = fetchimagery.write_order_file(directory, [remote])
            self.assertEqual(imagerybuild.expand_inputs(["@" + order]), [remote])


class WarpedNames(unittest.TestCase):

    def test_streamed_scenes_get_distinct_names(self):
        # Tutte le scene lette dalla rete si chiamano TCI.tif: prima i loro VRT
        # riproiettati si sovrascrivevano l'uno con l'altro.
        from geoworld import raster
        paths = [f"/vsicurl/https://x/S2A_32TMQ_2024070{i}_0_L2A/TCI.tif" for i in range(4)]
        names = [raster.warped_vrt_name(i, p) for i, p in enumerate(paths)]
        self.assertEqual(len(set(names)), 4)
        self.assertTrue(all(name.endswith("_4326.vrt") for name in names))


if __name__ == "__main__":
    unittest.main()

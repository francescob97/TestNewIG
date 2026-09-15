"""Schema di tiling: proprieta' geometriche. Nessuna dipendenza esterna."""
import unittest

from geoworld import tiling


class TestTilingScheme(unittest.TestCase):

    def test_level0_is_two_by_one(self):
        self.assertEqual(tiling.tiles_x(0), 2)
        self.assertEqual(tiling.tiles_y(0), 1)

    def test_level0_covers_the_globe_exactly(self):
        west = tiling.tile_bounds(0, 0, 0)
        east = tiling.tile_bounds(0, 1, 0)
        self.assertEqual(west.west, -180.0)
        self.assertEqual(west.east, 0.0)
        self.assertEqual(east.east, 180.0)
        self.assertEqual(west.south, -90.0)
        self.assertEqual(west.north, 90.0)

    def test_tiles_are_square_in_degrees(self):
        for level in range(0, 16):
            bounds = tiling.tile_bounds(level, 0, 0)
            self.assertAlmostEqual(bounds.east - bounds.west,
                                   bounds.north - bounds.south, places=12)

    def test_adjacent_tiles_share_their_edge_exactly(self):
        """Il bordo est di una tile e' il bordo ovest della successiva, senza
        tolleranza: e' la condizione per cui i post di bordo coincidono."""
        for level in (0, 5, 14):
            for x in (0, 3, tiling.tiles_x(level) - 2):
                self.assertEqual(tiling.tile_bounds(level, x, 0).east,
                                 tiling.tile_bounds(level, x + 1, 0).west)
            for y in range(min(3, tiling.tiles_y(level) - 1)):
                self.assertEqual(tiling.tile_bounds(level, 0, y).south,
                                 tiling.tile_bounds(level, 0, y + 1).north)

    def test_last_post_of_a_tile_is_first_post_of_the_next(self):
        """E' l'overlap di 1 pixel, espresso in indici globali."""
        for level in (0, 7, 14):
            for x in (0, 11):
                self.assertEqual(tiling.global_post_x(level, x, tiling.TILE_CELLS),
                                 tiling.global_post_x(level, x + 1, 0))

    def test_post_index_doubles_between_levels(self):
        """Il post g al livello L e' il post 2g al livello L+1, e le due
        posizioni geografiche coincidono. E' cio' che rende la riduzione di
        livello una decimazione esatta invece di un ricampionamento."""
        for level in range(0, 14):
            for global_index in (0, 1, 129, 5000):
                if global_index >= tiling.global_posts_x(level):
                    continue
                self.assertAlmostEqual(tiling.post_lon(level, global_index),
                                       tiling.post_lon(level + 1, 2 * global_index),
                                       places=12)

    def test_lookup_round_trips(self):
        for level in (4, 10, 14):
            for lon, lat in [(12.492231, 41.890210), (-179.9, 89.9), (0.0, 0.0)]:
                x, y = tiling.tile_for_lonlat(level, lon, lat)
                bounds = tiling.tile_bounds(level, x, y)
                self.assertTrue(bounds.west <= lon <= bounds.east)
                self.assertTrue(bounds.south <= lat <= bounds.north)

    def test_domain_extremes_stay_inside_the_grid(self):
        """Polo Nord e antimeridiano non devono cadere in una tile inesistente."""
        for level in (0, 8, 14):
            for lon, lat in [(180.0, 90.0), (-180.0, -90.0), (180.0, -90.0)]:
                x, y = tiling.tile_for_lonlat(level, lon, lat)
                self.assertTrue(0 <= x < tiling.tiles_x(level))
                self.assertTrue(0 <= y < tiling.tiles_y(level))

    def test_recommended_level_matches_tinitaly(self):
        """TINITALY e' a 10 m: alle latitudini italiane il livello nativo e' 14."""
        self.assertEqual(tiling.recommended_max_level(10.0, 41.9), 14)
        lon_spacing, lat_spacing = tiling.post_spacing_metres(14, 41.9)
        self.assertLess(lat_spacing, 10.0)
        self.assertGreater(tiling.post_spacing_metres(13, 41.9)[1], 10.0)


if __name__ == "__main__":
    unittest.main()

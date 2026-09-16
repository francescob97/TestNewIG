"""
Regressione: la risoluzione del sorgente va misurata IN METRI SUL TERRENO.

Storia del bug che questo file impedisce di ripetere. La scelta automatica del
livello massimo usava la dimensione del pixel letta dal geotransform, che e'
nelle unita' del CRS sorgente. Con TINITALY (UTM 32N) sono metri e funzionava.
Con il Copernicus DEM (EPSG:4326) sono GRADI: 0.000277 interpretato come metri
faceva chiedere il livello 24, cioe' un raster da 11.930.501 x 11.930.629 post
(518 TB), e GDAL falliva con un messaggio sugli array interni del formato TIFF
che non diceva niente sulla causa.

Il bug era invisibile nei test perche' passavano sempre --max-level esplicito.
Qui il livello NON si passa mai: e' il punto.
"""
import os
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from geoworld import tiling

PIPELINE_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def make_source(directory: str, epsg: int) -> str:
    result = subprocess.run(
        [sys.executable, os.path.join(PIPELINE_DIR, "make_synthetic_source.py"),
         "-o", directory, "--epsg", str(epsg), "--tiles-x", "1", "--tiles-y", "1",
         "--tile-pixels", "400"],
        capture_output=True, text=True,
        env=dict(os.environ, PYTHONPATH=PIPELINE_DIR))
    if result.returncode != 0:
        raise AssertionError(f"generazione sorgente EPSG:{epsg} fallita:\n{result.stderr}")
    return directory


class TestGroundResolution(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        try:
            from osgeo import gdal            # noqa: F401
        except ImportError as error:
            raise unittest.SkipTest(f"GDAL non disponibile ({error})")
        cls._temporary = tempfile.TemporaryDirectory()

    @classmethod
    def tearDownClass(cls):
        cls._temporary.cleanup()

    def resolution_of(self, epsg: int) -> dict:
        from geoworld.raster import source_ground_resolution
        directory = make_source(os.path.join(self._temporary.name, f"src{epsg}"), epsg)
        files = [f for f in os.listdir(directory) if f.endswith(".tif")]
        return source_ground_resolution(os.path.join(directory, files[0]))

    def test_projected_source_in_metres(self):
        """UTM 32N a 10 m: il pixel nativo E' gia' in metri."""
        resolution = self.resolution_of(32632)
        self.assertAlmostEqual(resolution["nativePixelX"], 10.0, places=6)
        self.assertAlmostEqual(resolution["groundResolutionXMetres"], 10.0, delta=0.5)
        self.assertAlmostEqual(resolution["groundResolutionYMetres"], 10.0, delta=0.5)

    def test_geographic_source_is_converted_from_degrees(self):
        """
        EPSG:4326 a 1 arcosecondo: il pixel nativo vale 0.000277 GRADI, ma sul
        terreno sono ~23 m in longitudine e ~31 m in latitudine.
        E' il caso che mandava la pipeline al livello 24.
        """
        resolution = self.resolution_of(4326)

        self.assertAlmostEqual(resolution["nativePixelX"], 1.0 / 3600.0, places=9)
        self.assertLess(resolution["nativePixelX"], 0.001,
                        "il pixel nativo deve restare in gradi")

        self.assertAlmostEqual(resolution["groundResolutionYMetres"], 30.9, delta=1.0)
        self.assertAlmostEqual(resolution["groundResolutionXMetres"], 23.0, delta=2.0)

    def test_level_choice_is_sane_for_both_crs(self):
        """
        E' l'asserzione che avrebbe fermato il bug: da entrambi i sorgenti deve
        uscire un livello plausibile, mai il tetto.
        """
        for epsg, expected in ((32632, 14), (4326, 13)):
            resolution = self.resolution_of(epsg)
            level = tiling.recommended_max_level(
                resolution["finestMetres"], resolution["centreLatitude"])

            self.assertEqual(level, expected, f"EPSG:{epsg} ha scelto il livello {level}")
            self.assertLess(level, tiling.MAX_SUPPORTED_LEVEL,
                            f"EPSG:{epsg} ha toccato il tetto dei livelli")

    def test_infeasible_level_is_refused_with_a_useful_message(self):
        """
        Anche con la risoluzione giusta, un --max-level sbagliato a mano deve
        fermarsi qui e non dentro GDAL.
        """
        from geoworld.raster import check_level_is_feasible, level_grid_for_bbox

        check_level_is_feasible(level_grid_for_bbox(13, (12.0, 41.0, 13.0, 42.0)))

        with self.assertRaises(RuntimeError) as context:
            check_level_is_feasible(level_grid_for_bbox(24, (12.0, 41.0, 13.0, 42.0)))

        message = str(context.exception)
        self.assertIn("--max-level", message)
        self.assertIn("risoluzione", message)


class TestRecommendedLevelGuards(unittest.TestCase):

    def test_rejects_nonsense_resolution(self):
        for bad in (0.0, -1.0):
            with self.assertRaises(ValueError):
                tiling.recommended_max_level(bad, 41.9)

    def test_caps_at_max_supported_level(self):
        """Una risoluzione implausibilmente fine si ferma al tetto invece di
        proseguire fino a livelli irrealizzabili."""
        self.assertEqual(tiling.recommended_max_level(1e-6, 41.9),
                         tiling.MAX_SUPPORTED_LEVEL)


if __name__ == "__main__":
    unittest.main()

"""
Regressione: il passo di campionamento di N va ALLINEATO alla griglia geoidica.

Storia del bug. Il passo di default era 0.01 gradi, scelto "abbastanza fine" e
validato con EGM96, dove dava 0.004 mm di errore. Su EGM2008 lo stesso passo ne
dava 11, e la pipeline si fermava alla soglia di 10 mm.

Il motivo non era la finezza. PROJ interpola bilinearmente dentro la propria
griglia, quindi il campo ha una piega su ogni linea di nodi: ricampionarlo e
re-interpolarlo e' esatto finche' i nostri nodi cadono sui suoi, e sbaglia
appena cadono in mezzo. 0.01 divide 0.25 (EGM96) in 25 parti esatte, ma non
divide 1/24 (EGM2008): 4.1667.

Il default era quindi allineato per caso con la griglia su cui l'avevo provato.
"""
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from geoworld import geoid


class TestSpacingAlignment(unittest.TestCase):
    """Solo aritmetica: non serve nessuna griglia installata."""

    def test_default_spacing_is_aligned_for_known_datums(self):
        for crs in (geoid.VERTICAL_CRS_EGM2008, geoid.VERTICAL_CRS_EGM96):
            spacing = geoid.default_sampling_spacing(crs)
            self.assertIsNotNone(spacing)
            self.assertTrue(geoid.is_spacing_aligned(spacing, crs),
                            f"il passo di default per {crs} non e' allineato")

    def test_egm2008_default_is_one_ninetysixth(self):
        """2.5 primi / 4 = 1/96. E' il valore che risolve il caso reale."""
        self.assertAlmostEqual(
            geoid.default_sampling_spacing(geoid.VERTICAL_CRS_EGM2008), 1.0 / 96.0, places=12)

    def test_the_old_default_was_misaligned_for_egm2008(self):
        """0.01 gradi: allineato con EGM96 per caso, NON con EGM2008."""
        self.assertTrue(geoid.is_spacing_aligned(0.01, geoid.VERTICAL_CRS_EGM96))
        self.assertFalse(geoid.is_spacing_aligned(0.01, geoid.VERTICAL_CRS_EGM2008))

    def test_alignment_survives_further_subdivision(self):
        """Raddoppiare le sottodivisioni deve mantenere l'allineamento, perche'
        e' cosi' che il raffinamento automatico non peggiora le cose."""
        for crs in (geoid.VERTICAL_CRS_EGM2008, geoid.VERTICAL_CRS_EGM96):
            for subdivisions in (4, 8, 16, 32):
                spacing = geoid.default_sampling_spacing(crs, subdivisions)
                self.assertTrue(geoid.is_spacing_aligned(spacing, crs),
                                f"{crs} con {subdivisions} sottodivisioni")

    def test_unknown_datum_has_no_native_spacing(self):
        self.assertIsNone(geoid.default_sampling_spacing("EPSG:9999"))
        self.assertFalse(geoid.is_spacing_aligned(0.01, "EPSG:9999"))


class TestMeasuredError(unittest.TestCase):
    """Richiede una griglia geoidica installata."""

    @classmethod
    def setUpClass(cls):
        from geoworld import environment
        cls.crs = environment.best_available_vertical_crs()
        if cls.crs is None:
            raise unittest.SkipTest("nessuna griglia geoidica: lancia  python run.py check-env")
        # Arco alpino: e' dove il geoide varia di piu' in Italia, quindi dove il
        # ricampionamento sbaglia di piu'. Testare al centro del Tirreno non
        # direbbe niente.
        cls.bbox = (6.5277, 43.6582, 14.0687, 47.1560)

    def measure(self, spacing: float) -> float:
        grid, geotransform = geoid.build_undulation_grid(self.bbox, spacing, self.crs)
        error = geoid.measure_interpolation_error(
            grid, geotransform, self.bbox, self.crs, samples=4000)
        return error["maxErrorM"]

    def test_aligned_spacing_is_orders_of_magnitude_better(self):
        """
        E' l'asserzione centrale: un passo allineato e uno quasi identico ma
        disallineato danno errori che differiscono di ordini di grandezza.
        Se questa proprieta' si perdesse, il default tornerebbe a essere una
        scelta arbitraria.
        """
        native = geoid.GEOID_NATIVE_SPACING_DEG[self.crs]
        aligned = native / 24.0
        misaligned = aligned * 1.04          # ~4% piu' grande: rompe il rapporto intero

        self.assertTrue(geoid.is_spacing_aligned(aligned, self.crs))
        self.assertFalse(geoid.is_spacing_aligned(misaligned, self.crs))

        aligned_error = self.measure(aligned)
        misaligned_error = self.measure(misaligned)

        self.assertLess(aligned_error, 1e-4,
                        f"passo allineato: {aligned_error * 1000:.4f} mm, atteso sotto 0.1 mm")
        self.assertGreater(misaligned_error, aligned_error * 50,
                           f"allineato {aligned_error * 1000:.4f} mm contro "
                           f"disallineato {misaligned_error * 1000:.4f} mm: "
                           "la differenza attesa non c'e' piu'")

    def test_default_spacing_meets_the_threshold(self):
        spacing = geoid.default_sampling_spacing(self.crs)
        self.assertLess(self.measure(spacing), 0.01)

    def test_builder_returns_within_threshold(self):
        _, _, error, used = geoid.build_aligned_undulation_grid(
            self.bbox, self.crs, max_error_m=0.01)
        self.assertLessEqual(error["maxErrorM"], 0.01)
        self.assertTrue(geoid.is_spacing_aligned(used, self.crs))

    def test_builder_refines_when_given_a_bad_starting_point(self):
        """Con una soglia irraggiungibile deve provarci piu' volte e non
        andare in ciclo infinito."""
        _, _, error, used = geoid.build_aligned_undulation_grid(
            self.bbox, self.crs, max_error_m=1e-12, max_attempts=3)
        self.assertGreater(error["maxErrorM"], 0.0)
        self.assertTrue(geoid.is_spacing_aligned(used, self.crs))


if __name__ == "__main__":
    unittest.main()

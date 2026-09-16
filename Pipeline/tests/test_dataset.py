"""
Test di integrazione: genera un dataset da un sorgente sintetico e ne verifica
le proprieta' che contano davvero per il runtime.

Richiede GDAL, pyproj e una griglia geoidica. Usa EGM96 (EPSG:5773) perche' e'
inclusa praticamente ovunque; il dataset di produzione usera' EGM2008.
"""
import os
import subprocess
import sys
import tempfile
import unittest

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from geoworld import tiling, tileformat, geoid, manifest as manifest_module

PIPELINE_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

#: Scelto a tempo di esecuzione fra quelli disponibili: si preferisce EGM2008,
#: si ripiega su EGM96. Fissarne uno a priori faceva fallire la suite su
#: macchine dove quella griglia non c'e', con un errore che non lo diceva.
VERTICAL_CRS: str | None = None


def run_step(arguments: list[str], what: str) -> subprocess.CompletedProcess:
    """
    Esegue un passo della pipeline RIPORTANDO l'errore vero se fallisce.

    Prima questa funzione buttava via stdout e stderr con DEVNULL: qualunque
    problema (GDAL assente, griglia geoidica mancante, percorso sbagliato)
    arrivava come un CalledProcessError nudo dentro setUpClass, cioe' un
    traceback che non diceva niente sulla causa. Un test che nasconde il motivo
    del fallimento e' peggio di un test assente, perche' fa perdere tempo.
    """
    result = subprocess.run(arguments, env=dict(os.environ, PYTHONPATH=PIPELINE_DIR),
                            capture_output=True, text=True)
    if result.returncode != 0:
        raise AssertionError(
            f"{what} e' fallito (codice {result.returncode}).\n"
            f"--- comando ---\n{' '.join(arguments)}\n"
            f"--- stderr ---\n{result.stderr.strip()}\n"
            f"--- stdout ---\n{result.stdout.strip()}")
    return result


def build_once(directory: str) -> tuple[str, str]:
    """Genera sorgente sintetico e dataset. Ritorna (source_dir, dataset_dir)."""
    source = os.path.join(directory, "synth")
    dataset = os.path.join(directory, "dataset")

    run_step([sys.executable, os.path.join(PIPELINE_DIR, "make_synthetic_source.py"),
              "-o", source], "la generazione del sorgente sintetico")

    run_step([sys.executable, os.path.join(PIPELINE_DIR, "run.py"), "build",
              "-i", os.path.join(source, "*.tif"), "-o", dataset,
              "--vertical-crs", VERTICAL_CRS,
              "--min-level", "9", "--max-level", "13", "--jobs", "1"],
             "la pipeline")
    return source, dataset


class TestGeneratedDataset(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        # Prerequisiti: se mancano si SALTA con un messaggio che dice cosa
        # installare, invece di fallire come se il codice fosse rotto.
        try:
            from osgeo import gdal            # noqa: F401
            import pyproj                     # noqa: F401
        except ImportError as error:
            raise unittest.SkipTest(
                f"manca una dipendenza ({error}). Lancia:  python run.py check-env")

        global VERTICAL_CRS
        from geoworld import environment
        VERTICAL_CRS = environment.best_available_vertical_crs()
        if VERTICAL_CRS is None:
            raise unittest.SkipTest(
                "nessuna griglia geoidica utilizzabile (ne' EGM2008 ne' EGM96). "
                "Lancia:  python run.py check-env")

        cls._temporary = tempfile.TemporaryDirectory()
        cls.source_dir, cls.dataset_dir = build_once(cls._temporary.name)
        cls.manifest = manifest_module.read_manifest(cls.dataset_dir)
        cls.levels = [entry["level"] for entry in cls.manifest["levels"]]

    @classmethod
    def tearDownClass(cls):
        cls._temporary.cleanup()

    # --- accesso ---------------------------------------------------------

    def load(self, level: int, x: int, y: int):
        path = os.path.join(self.dataset_dir, tileformat.tile_relative_path(level, x, y))
        if not os.path.exists(path):
            return None
        return tileformat.read_tile(path)

    def existing(self, level: int) -> dict[tuple[int, int], manifest_module.TileEntry]:
        return {(entry.x, entry.y): entry
                for entry in manifest_module.read_level_index(self.dataset_dir, level)}

    # --- giunzioni -------------------------------------------------------

    def test_horizontal_seams_are_bit_identical(self):
        """
        E' LA proprieta' per cui esiste l'overlap di 1 post: due tile affiancate
        devono avere sul bordo comune valori identici BIT PER BIT, non simili.
        Se differissero anche di un ULP, in Fase 5 si aprirebbero crepe fra tile
        dello stesso livello che nessuna skirt puo' nascondere, perche' sarebbero
        buchi a geometria corretta.
        """
        checked = 0
        for level in self.levels:
            tiles = self.existing(level)
            for (x, y) in tiles:
                if (x + 1, y) not in tiles:
                    continue
                _, left = self.load(level, x, y)
                _, right = self.load(level, x + 1, y)
                np.testing.assert_array_equal(
                    left[:, tiling.TILE_CELLS], right[:, 0],
                    err_msg=f"giunzione orizzontale rotta fra {level}/{x}/{y} e {level}/{x+1}/{y}")
                checked += 1
        self.assertGreater(checked, 20, "troppe poche coppie verificate")

    def test_vertical_seams_are_bit_identical(self):
        checked = 0
        for level in self.levels:
            tiles = self.existing(level)
            for (x, y) in tiles:
                if (x, y + 1) not in tiles:
                    continue
                _, upper = self.load(level, x, y)
                _, lower = self.load(level, x, y + 1)
                np.testing.assert_array_equal(
                    upper[tiling.TILE_CELLS, :], lower[0, :],
                    err_msg=f"giunzione verticale rotta fra {level}/{x}/{y} e {level}/{x}/{y+1}")
                checked += 1
        self.assertGreater(checked, 10)

    # --- coerenza fra livelli --------------------------------------------

    def test_parent_posts_coincide_geographically_with_child_posts(self):
        for level in self.levels[:-1]:
            for (x, y) in list(self.existing(level))[:4]:
                parent = tiling.tile_bounds(level, x, y)
                child = tiling.tile_bounds(level + 1, 2 * x, 2 * y)
                self.assertAlmostEqual(parent.west, child.west, places=12)
                self.assertAlmostEqual(parent.north, child.north, places=12)

    def test_parent_value_lies_within_child_neighbourhood(self):
        """
        Il post del padre e' una media PESATA dei post del figlio nel suo
        intorno 5x5. Una media pesata sta sempre fra il minimo e il massimo dei
        valori mediati: se cosi' non fosse, il filtro sarebbe sbagliato (kernel
        non normalizzato, oppure indici sfasati di mezzo passo).
        """
        radius = 2
        violations = 0
        checked = 0

        for level in self.levels[:-1]:
            parents = self.existing(level)
            children = self.existing(level + 1)

            for (x, y) in list(parents)[:6]:
                loaded = self.load(level, x, y)
                if loaded is None:
                    continue
                _, parent_heights = loaded

                # Il quadrante nord-ovest del padre viene dal figlio (2x, 2y).
                if (2 * x, 2 * y) not in children:
                    continue
                _, child_heights = self.load(level + 1, 2 * x, 2 * y)

                for j in range(4, 60, 7):
                    for i in range(4, 60, 7):
                        window = child_heights[2 * j - radius: 2 * j + radius + 1,
                                               2 * i - radius: 2 * i + radius + 1]
                        value = parent_heights[j, i]
                        checked += 1
                        # Tolleranza minima per l'aritmetica float32.
                        if not (window.min() - 1e-3 <= value <= window.max() + 1e-3):
                            violations += 1

        self.assertGreater(checked, 100)
        self.assertEqual(violations, 0,
                         f"{violations} post del padre fuori dall'intorno del figlio su {checked}")

    # --- correttezza delle quote -----------------------------------------

    def test_heights_are_ellipsoidal_not_orthometric(self):
        """
        Confronta le quote delle tile con il valore ANALITICO del terreno
        sintetico piu' l'ondulazione del geoide.

        E' il test che smaschera il fallimento silenzioso di PROJ descritto in
        geoid.py: se la trasformazione verticale non venisse applicata, le quote
        sarebbero ~48 m piu' basse e questo test fallirebbe con uno scarto
        sistematico pari a N.
        """
        from osgeo import osr
        sys.path.insert(0, PIPELINE_DIR)
        from make_synthetic_source import synthetic_height

        wgs84 = osr.SpatialReference()
        wgs84.ImportFromEPSG(4326)
        wgs84.SetAxisMappingStrategy(osr.OAMS_TRADITIONAL_GIS_ORDER)
        utm = osr.SpatialReference()
        utm.ImportFromEPSG(32632)
        utm.SetAxisMappingStrategy(osr.OAMS_TRADITIONAL_GIS_ORDER)
        to_utm = osr.CoordinateTransformation(wgs84, utm)

        transformer = geoid._make_transformer(VERTICAL_CRS)

        level = max(self.levels)

        # Serve una tile INTERAMENTE su terraferma: dove i post sono stati
        # riempiti con il livello del mare il confronto con il terreno analitico
        # non avrebbe senso. Si cerca la prima completa invece di sceglierne una
        # a caso, che sul nostro sorgente sintetico finisce spesso sul mare.
        candidate = next(((x, y) for (x, y) in sorted(self.existing(level))
                          if not self.load(level, x, y)[0].has_filled_posts), None)
        self.assertIsNotNone(candidate, "nessuna tile interamente coperta dal dato sorgente")

        x, y = candidate
        header, heights = self.load(level, x, y)

        bounds = tiling.tile_bounds(level, x, y)
        spacing = tiling.post_spacing_deg(level)

        differences = []
        for j in range(8, 121, 16):
            for i in range(8, 121, 16):
                lon = bounds.west + i * spacing
                lat = bounds.north - j * spacing
                easting, northing, _ = to_utm.TransformPoint(lon, lat)
                orthometric = float(synthetic_height(np.array([easting]), np.array([northing]))[0])
                _, _, undulation = transformer.transform(lon, lat, 0.0)
                differences.append(float(heights[j, i]) - (orthometric + undulation))

        differences = np.array(differences)
        # Lo scarto residuo e' solo ricampionamento bilineare su terreno
        # continuo: deve essere piccolo E centrato su zero. Un bias sistematico
        # di decine di metri sarebbe il sintomo della mancata conversione.
        self.assertLess(abs(differences.mean()), 0.5,
                        f"bias sistematico di {differences.mean():.3f} m: "
                        "la conversione ortometrica -> ellissoidica non e' stata applicata")
        self.assertLess(np.abs(differences).max(), 3.0,
                        f"scarto massimo {np.abs(differences).max():.3f} m troppo grande")

    def test_missing_posts_are_filled_with_sea_level(self):
        """
        I post senza dato sorgente devono valere la quota ELLISSOIDICA del
        livello medio del mare, cioe' N, non zero. Scrivere zero metterebbe il
        mare ~48 m sotto dove sta, con uno scalino netto lungo tutta la costa.
        """
        level = max(self.levels)
        filled = []
        for (x, y) in self.existing(level):
            header, heights = self.load(level, x, y)
            if header.has_filled_posts:
                filled.append((x, y, header, heights))

        self.assertGreater(len(filled), 0, "il sorgente sintetico deve avere una zona di mare")

        x, y, header, heights = filled[0]
        bounds = tiling.tile_bounds(level, x, y)
        transformer = geoid._make_transformer(VERTICAL_CRS)
        _, _, undulation = transformer.transform(
            (bounds.west + bounds.east) / 2.0, (bounds.south + bounds.north) / 2.0, 0.0)

        self.assertGreater(header.min_height, undulation - 1.0)
        self.assertLess(header.min_height, undulation + 1.0)

    # --- indice e manifest ------------------------------------------------

    def test_index_matches_files_on_disk(self):
        for level in self.levels:
            entries = self.existing(level)
            for (x, y), entry in entries.items():
                path = os.path.join(self.dataset_dir, tileformat.tile_relative_path(level, x, y))
                self.assertTrue(os.path.exists(path), f"indice cita {level}/{x}/{y} che non esiste")
                header = tileformat.read_tile_header(path)
                self.assertAlmostEqual(header.min_height, entry.min_height, places=3)
                self.assertAlmostEqual(header.max_height, entry.max_height, places=3)

    def test_index_is_sorted(self):
        for level in self.levels:
            entries = manifest_module.read_level_index(self.dataset_dir, level)
            keys = [(entry.y, entry.x) for entry in entries]
            self.assertEqual(keys, sorted(keys), f"indice del livello {level} non ordinato")

    def test_manifest_declares_what_the_runtime_needs(self):
        scheme = self.manifest["tilingScheme"]
        self.assertEqual(scheme["level0TilesX"], 2)
        self.assertEqual(scheme["level0TilesY"], 1)

        tile_format = self.manifest["tileFormat"]
        self.assertEqual(tile_format["posts"], 129)
        self.assertEqual(tile_format["overlapPosts"], 1)
        self.assertEqual(tile_format["bytesPerTile"], tileformat.TILE_BYTES)

        # Il min/max per tile e' nell'indice, ma il manifest deve comunque dire
        # l'intervallo di ogni livello: serve al bounding volume della radice.
        for entry in self.manifest["levels"]:
            self.assertLessEqual(entry["min_height"], entry["max_height"])

        self.assertIn("verticalDatum", self.manifest)
        self.assertIn("interpolationError", self.manifest["verticalDatum"])

    # --- idempotenza ------------------------------------------------------

    def test_second_run_rewrites_nothing(self):
        """
        Rilanciare la pipeline non deve ricalcolare niente. E' il requisito
        esplicito di riavviabilita': su un dataset vero uno stadio dura ore, e
        ripartire da zero dopo un'interruzione non e' accettabile.
        """
        result = run_step(
            [sys.executable, os.path.join(PIPELINE_DIR, "run.py"), "build",
             "-i", os.path.join(self.source_dir, "*.tif"), "-o", self.dataset_dir,
             "--vertical-crs", VERTICAL_CRS,
             "--min-level", "9", "--max-level", "13", "--jobs", "1"],
            "il secondo lancio della pipeline")

        output = result.stderr
        for stage in ("inventory", "geoid", "warp", "pyramid", "tiles"):
            self.assertIn(f"{stage}  (gia' fatto, salto)", output,
                          f"lo stadio '{stage}' e' stato rieseguito inutilmente")

    def test_deleted_tiles_are_regenerated_and_the_others_are_not(self):
        """
        Simula un'interruzione: si cancellano alcune tile e si rilancia. Devono
        essere riscritte SOLO quelle, non l'intero livello.

        E' la differenza fra "riavviabile" e "ripartire da capo". Su un dataset
        vero il livello piu' fine ha centinaia di migliaia di tile: riscriverle
        tutte per recuperarne dieci renderebbe la riavviabilita' inutile.
        """
        level = max(self.levels)
        tiles = sorted(self.existing(level))[:5]

        for (x, y) in tiles:
            os.remove(os.path.join(self.dataset_dir,
                                   tileformat.tile_relative_path(level, x, y)))

        result = run_step(
            [sys.executable, os.path.join(PIPELINE_DIR, "run.py"), "build",
             "-i", os.path.join(self.source_dir, "*.tif"), "-o", self.dataset_dir,
             "--vertical-crs", VERTICAL_CRS,
             "--min-level", "9", "--max-level", "13", "--jobs", "1",
             "--redo", "tiles"],
            "il rilancio dopo la cancellazione delle tile")

        for (x, y) in tiles:
            self.assertTrue(os.path.exists(os.path.join(
                self.dataset_dir, tileformat.tile_relative_path(level, x, y))),
                f"la tile cancellata {level}/{x}/{y} non e' stata rigenerata")

        # La riga del livello piu' fine deve riportare esattamente 5 scritture.
        for line in result.stderr.splitlines():
            if "tile con dato" in line and f"{len(self.existing(level))}" in line:
                pass
        written_lines = [line for line in result.stderr.splitlines() if "scritte" in line]
        self.assertTrue(any(", 5 scritte" in line for line in written_lines),
                        f"attese 5 tile riscritte, righe trovate: {written_lines}")


if __name__ == "__main__":
    unittest.main()

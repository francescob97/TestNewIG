"""
Coerenza fra moduli della pipeline: i nomi delle chiavi devono corrispondere.

PERCHE' QUESTO TEST ESISTE
`imagerybuild.py` leggeva `resolution["x_metres"]` da un dizionario che
`raster.source_ground_resolution()` costruisce con la chiave
`groundResolutionXMetres`. Nessuno dei due file e' sbagliato da solo: sbagliato
e' il contratto fra i due, e Python se ne accorge solo quando quella riga viene
eseguita davvero -- cioe' dopo che l'utente ha scaricato un gigabyte di scene.

Il controllo e' statico, sull'albero sintattico: non serve GDAL, non serve un
dataset, e da' la stessa risposta ovunque.
"""

import ast
import os
import sys
import unittest

PACKAGE_DIR = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "geoworld")


def dict_keys_in(path: str) -> set[str]:
    """Tutte le stringhe usate come chiave di un dizionario nel file."""
    with open(path, "r", encoding="utf-8") as handle:
        tree = ast.parse(handle.read(), filename=path)

    keys = set()
    for node in ast.walk(tree):
        if isinstance(node, ast.Dict):
            for key in node.keys:
                if isinstance(key, ast.Constant) and isinstance(key.value, str):
                    keys.add(key.value)
        # Anche le assegnazioni del tipo info["chiave"] = ...
        elif isinstance(node, ast.Subscript) and isinstance(node.ctx, ast.Store):
            if isinstance(node.slice, ast.Constant) and isinstance(node.slice.value, str):
                keys.add(node.slice.value)
    return keys


def subscripts_on_calls_to(path: str, module: str) -> list[tuple[str, str, int]]:
    """
    Trova `x = module.qualcosa(...)` seguito da `x["chiave"]`.

    Ritorna (nome della variabile, chiave letta, riga).
    """
    with open(path, "r", encoding="utf-8") as handle:
        tree = ast.parse(handle.read(), filename=path)

    from_module = set()
    for node in ast.walk(tree):
        if isinstance(node, ast.Assign) and isinstance(node.value, ast.Call):
            function = node.value.func
            if (isinstance(function, ast.Attribute)
                    and isinstance(function.value, ast.Name)
                    and function.value.id == module):
                for target in node.targets:
                    if isinstance(target, ast.Name):
                        from_module.add(target.id)

    found = []
    for node in ast.walk(tree):
        if not isinstance(node, ast.Subscript) or isinstance(node.ctx, ast.Store):
            continue
        if not isinstance(node.value, ast.Name) or node.value.id not in from_module:
            continue
        if isinstance(node.slice, ast.Constant) and isinstance(node.slice.value, str):
            found.append((node.value.id, node.slice.value, node.lineno))
    return found


class TestRasterContract(unittest.TestCase):

    def test_imagerybuild_reads_keys_that_raster_produces(self):
        produced = dict_keys_in(os.path.join(PACKAGE_DIR, "raster.py"))
        consumed = subscripts_on_calls_to(
            os.path.join(PACKAGE_DIR, "imagerybuild.py"), "raster")

        self.assertTrue(consumed, "nessuna lettura trovata: il test non sta provando niente")

        for variable, key, line in consumed:
            with self.subTest(chiave=key, riga=line):
                self.assertIn(
                    key, produced,
                    f"imagerybuild.py:{line} legge {variable}['{key}'], ma raster.py "
                    f"non produce mai quella chiave. Chiavi disponibili che le "
                    f"assomigliano: "
                    f"{sorted(k for k in produced if key.lower()[:4] in k.lower()) or 'nessuna'}")

    def test_raster_still_publishes_the_key_the_build_relies_on(self):
        """
        La risoluzione piu' fine ha un nome solo, ed e' quello che decide il
        livello massimo della piramide. Se cambia, deve rompersi qui.
        """
        produced = dict_keys_in(os.path.join(PACKAGE_DIR, "raster.py"))
        self.assertIn("finestMetres", produced)
        self.assertIn("sourcesUsed", produced)


class TestSilentSkipProtection(unittest.TestCase):
    """
    GDAL scarta in silenzio i sorgenti con proiezione diversa: stampa un
    warning e prosegue. Entrambe le vie di mosaicatura devono contarli.
    """

    def test_both_mosaic_functions_verify_the_source_count(self):
        with open(os.path.join(PACKAGE_DIR, "raster.py"), "r", encoding="utf-8") as handle:
            tree = ast.parse(handle.read())

        for name in ("build_source_vrt", "build_reprojected_vrt"):
            with self.subTest(funzione=name):
                function = next(
                    (node for node in ast.walk(tree)
                     if isinstance(node, ast.FunctionDef) and node.name == name), None)
                self.assertIsNotNone(function, f"{name} non esiste piu'")

                calls = [node.func.id for node in ast.walk(function)
                         if isinstance(node, ast.Call) and isinstance(node.func, ast.Name)]
                self.assertIn(
                    "count_vrt_sources", calls,
                    f"{name} non verifica quanti sorgenti il VRT usa davvero: "
                    "un file scartato passerebbe inosservato")


class TestVrtSourceCounting(unittest.TestCase):

    @staticmethod
    def write_vrt(bands: int, sources: list[str]) -> str:
        """Un VRT finto con N bande, ognuna che elenca gli stessi sorgenti."""
        import tempfile

        body = ""
        for band in range(1, bands + 1):
            body += f'  <VRTRasterBand band="{band}">\n'
            for source in sources:
                body += ('    <SimpleSource><SourceFilename relativeToVRT="1">'
                         f'{source}</SourceFilename></SimpleSource>\n')
            body += '  </VRTRasterBand>\n'

        with tempfile.NamedTemporaryFile("w", suffix=".vrt", delete=False,
                                         encoding="utf-8") as handle:
            handle.write('<VRTDataset rasterXSize="10" rasterYSize="10">\n'
                         + body + '</VRTDataset>\n')
            return handle.name

    def test_counts_sources_in_a_single_band_vrt(self):
        from geoworld import raster
        path = self.write_vrt(bands=1, sources=["a.tif", "b.tif"])
        try:
            self.assertEqual(raster.count_vrt_sources(path), 2)
        finally:
            os.unlink(path)

    def test_rgb_vrt_repeats_sources_once_per_band(self):
        """
        LA REGRESSIONE. Un VRT RGB elenca ogni sorgente tre volte, una per
        banda. Contare le occorrenze di <SourceFilename> dava 6 per 2 immagini,
        e la pipeline si fermava dicendo che ne erano state scartate -4.
        Sui DEM, con una banda sola, l'errore era invisibile.
        """
        from geoworld import raster
        path = self.write_vrt(bands=3, sources=["a.tif", "b.tif"])
        try:
            self.assertEqual(raster.count_vrt_sources(path), 2,
                             "sta contando le bande invece dei file")
            self.assertEqual(raster.vrt_source_files(path), {"a.tif", "b.tif"})
        finally:
            os.unlink(path)


if __name__ == "__main__":
    sys.path.insert(0, os.path.dirname(PACKAGE_DIR))
    unittest.main(verbosity=2)

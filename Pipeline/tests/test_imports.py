"""
Gli import pesanti non devono stare in cima ai moduli.

PERCHE' QUESTO TEST ESISTE
Il comando `check-env` serve a dire all'utente cosa manca nel suo ambiente. Se
per funzionare ha bisogno proprio di cio' che deve diagnosticare, non serve a
niente: l'utente riceve una traceback invece della risposta.

E' gia' successo con Pillow, introdotto nella Fase 6: un `from PIL import Image`
in cima a imageformat.py si propagava lungo la catena cli -> imagecut ->
imageformat, e `python run.py check-env` moriva prima di stampare una riga.

Questo test controlla la struttura del sorgente, non il comportamento a runtime,
perche' deve dare la stessa risposta sia che le librerie siano installate sia
che non lo siano.
"""

import ast
import os
import sys
import unittest

PACKAGE_DIR = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "geoworld")

sys.path.insert(0, os.path.dirname(PACKAGE_DIR))

#: Librerie che non devono MAI essere importate a livello di modulo.
#:
#: numpy e' in questa lista solo per cli.py: gli altri moduli della pipeline
#: girano solo quando si sta gia' elaborando dati, e li' numpy c'e' per
#: definizione. cli.py invece viene importato anche da check-env.
HEAVY_MODULES = {"PIL", "osgeo", "pyproj"}
HEAVY_FOR_CLI = HEAVY_MODULES | {"numpy"}


def top_level_imports(path: str) -> set[str]:
    """Nomi di primo livello importati fuori da qualunque funzione o classe."""
    with open(path, "r", encoding="utf-8") as handle:
        tree = ast.parse(handle.read(), filename=path)

    found = set()
    for node in tree.body:                       # solo il corpo del modulo
        if isinstance(node, ast.Import):
            for alias in node.names:
                found.add(alias.name.split(".")[0])
        elif isinstance(node, ast.ImportFrom):
            if node.module and node.level == 0:
                found.add(node.module.split(".")[0])
    return found


class TestHeavyImports(unittest.TestCase):

    def test_no_module_imports_pillow_or_gdal_at_top_level(self):
        for name in sorted(os.listdir(PACKAGE_DIR)):
            if not name.endswith(".py"):
                continue
            path = os.path.join(PACKAGE_DIR, name)
            with self.subTest(modulo=name):
                offenders = top_level_imports(path) & HEAVY_MODULES
                self.assertEqual(
                    offenders, set(),
                    f"{name} importa {sorted(offenders)} in cima al modulo: "
                    "spostalo dentro le funzioni che lo usano, altrimenti "
                    "check-env muore per cio' che deve diagnosticare")

    def test_cli_does_not_even_import_numpy_at_top_level(self):
        offenders = top_level_imports(os.path.join(PACKAGE_DIR, "cli.py")) & HEAVY_FOR_CLI
        self.assertEqual(
            offenders, set(),
            f"cli.py importa {sorted(offenders)} in cima: ogni comando lo "
            "pagherebbe, check-env compreso")

    def test_environment_module_stands_alone(self):
        """
        environment.py e' il modulo della diagnosi: deve poter essere importato
        in un ambiente completamente vuoto.
        """
        imports = top_level_imports(os.path.join(PACKAGE_DIR, "environment.py"))
        self.assertEqual(imports & HEAVY_FOR_CLI, set())


class TestCliRunsWithoutOptionalLibraries(unittest.TestCase):

    def test_check_env_is_importable(self):
        """Se questo fallisce, `python run.py check-env` non parte."""
        from geoworld import cli
        self.assertTrue(hasattr(cli, "main"))

    def test_check_env_reports_pillow(self):
        from geoworld import environment
        text = environment.format_report(environment.collect())
        self.assertIn("Pillow", text,
                      "check-env non dice niente su Pillow: l'utente non saprebbe "
                      "cosa installare")


if __name__ == "__main__":
    unittest.main(verbosity=2)

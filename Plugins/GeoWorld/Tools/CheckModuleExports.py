#!/usr/bin/env python3
"""
Controlla che nessun simbolo pubblico sia invisibile agli altri moduli.

IL PROBLEMA
In Unreal ogni modulo e' una DLL. Un simbolo definito in un .cpp NON e'
visibile agli altri moduli se non viene esportato con la macro API del modulo
(GEOCORE_API, GEOTILES_API, ...). Il sintomo e' un errore di LINK, non di
compilazione, e arriva quindi in fondo alla build:

    unresolved external symbol "GeoWorld::Core::GeodeticToEcef(...)"
    referenced in function "GeoWorld::Quadtree::MakeTileBoundingVolume(...)"

E' successo davvero: GeodeticToEcef era definita in GeoCore/Private e chiamata
da GeoRender.

LE DUE REGOLE CHE LO PREVENGONO

1. Lo STRATO PURO e' header-only. Non si esporta niente perche' non c'e'
   niente da esportare: ogni modulo compila la propria copia. La macro API
   resterebbe comunque fuori discussione li' dentro, perche' e' una macro del
   motore e quello strato non deve sapere di stare dentro Unreal.

2. Nello strato UNREAL, una funzione LIBERA dichiarata in un header pubblico
   deve essere inline oppure portare la macro API del modulo. Le classi vanno
   bene se la macro sta sulla classe: esporta tutti i membri.
"""
import glob
import os
import re
import sys

PURE_DIRECTORIES = [
    ("GeoCore", "Geo"),
    ("GeoTiles", "Tiles"),
    ("GeoRender", "Quadtree"),
]

# Dichiarazione di funzione libera: tipo + nome + parentesi + ";" sulla stessa
# riga, con un solo livello di rientro (dentro il namespace, fuori da una classe).
FREE_FUNCTION = re.compile(
    r'^\t(?!inline\b|template\b|friend\b|using\b|return\b|constexpr\b|static\b)'
    r'([A-Za-z_][\w:]*(?:\s*<[^>]*>)?[\w:&\* ]*?)\s+(\w+)\s*\([^;{]*\)\s*(?:const\s*)?;\s*(?://.*)?$')

API_MACRO = re.compile(r'\b[A-Z][A-Z0-9_]*_API\b')


def check_pure_is_header_only(source_root: str) -> list[str]:
    problems = []
    for module, folder in PURE_DIRECTORIES:
        for base in ("Private", "Public"):
            pattern = os.path.join(source_root, module, base, folder, "*.cpp")
            for path in glob.glob(pattern):
                problems.append(
                    f"{path}: lo strato puro deve essere header-only. "
                    f"Un .cpp qui produce simboli non esportati, invisibili agli altri moduli.")
    return problems


def check_public_free_functions(source_root: str) -> list[str]:
    problems = []
    pure_folders = {folder for _, folder in PURE_DIRECTORIES}

    for path in sorted(glob.glob(os.path.join(source_root, "**", "Public", "**", "*.h"),
                                 recursive=True)):
        # Lo strato puro e' coperto dalla regola precedente ed e' tutto inline.
        if any(os.sep + folder + os.sep in path for folder in pure_folders):
            continue

        # Il conteggio delle graffe e' CONTINUO su tutto il file. La prima
        # versione lo azzerava a ogni "class" o "struct" incontrata, e una
        # struct annidata (FLevelInfo dentro FGeoTileDataset, FActiveViewInfo
        # dentro UGeoreferenceSubsystem) faceva credere di essere usciti dalla
        # classe esterna: tutti i suoi membri successivi venivano segnalati come
        # funzioni libere. Sedici falsi positivi.
        depth = 0
        class_depth = None        # profondita' a cui e' cominciata la classe piu' esterna

        for number, line in enumerate(open(path, encoding="utf-8").read().splitlines(), 1):
            opened = line.count('{')
            closed = line.count('}')

            if class_depth is None and re.match(r'^\s*(class|struct)\s', line) \
                    and not line.rstrip().endswith(';'):
                class_depth = depth      # ci si entra quando arriva la graffa

            depth += opened - closed

            if class_depth is not None:
                if depth <= class_depth and closed:
                    class_depth = None   # chiusa la classe piu' esterna
                continue

            match = FREE_FUNCTION.match(line)
            if match and not API_MACRO.search(line):
                problems.append(
                    f"{path}:{number}: la funzione libera '{match.group(2)}' non e' inline "
                    f"e non ha la macro API del modulo: non sara' visibile agli altri moduli.")
    return problems


def main() -> int:
    root = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "..", "Source")

    problems = check_pure_is_header_only(root) + check_public_free_functions(root)

    if problems:
        print(f"  FALLITO - {len(problems)} simboli a rischio di errore di link:")
        for problem in problems:
            print(f"    {problem}")
        return 1

    print("  ok - strato puro header-only, nessuna funzione libera non esportata")
    return 0


if __name__ == "__main__":
    sys.exit(main())

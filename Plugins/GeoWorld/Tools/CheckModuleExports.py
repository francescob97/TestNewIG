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

3. Nello strato PURO, header-only, ogni funzione DEFINITA a livello di
   namespace deve essere inline. Senza, ogni .cpp che include l'header ne
   ha una copia con collegamento esterno, e il linker trova lo stesso
   simbolo due volte (LNK2005, "already defined"). E' passato inosservato a
   lungo con Detail::PriorityFromError in TileSelector.h: l'header lo
   includeva un solo .cpp, poi la unity build li fondeva. Bastava che un
   secondo .cpp lo includesse fuori dallo stesso blocco unity.
"""
import glob
import os
import re
import sys

PURE_DIRECTORIES = [
    ("GeoCore", "Geo"),
    ("GeoTiles", "Tiles"),
    ("GeoRender", "Quadtree"),
    ("GeoRender", "Mesh"),
]

# Header puri che vivono in una cartella condivisa con codice Unreal: si
# controllano uno per uno (la cartella Imagery contiene anche il subsystem).
PURE_FILES = [
    ("GeoRender", os.path.join("Imagery", "ImageryMapping.h")),
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


# Inizio di una definizione di funzione: "tipo nome(" su una riga che non e'
# un'istruzione. Il resto (inline, template, constexpr) si guarda a parte.
DEFINITION_START = re.compile(
    r'^\s*(?P<head>[A-Za-z_][\w:<>,\*& ]*?)\s+[\*&]*(?P<name>[A-Za-z_]\w*)\s*\(')
NOT_A_TYPE = {"return", "if", "for", "while", "switch", "else", "case", "do", "delete",
              "new", "throw", "using", "namespace", "class", "struct", "enum", "typedef"}


def check_pure_definitions_are_inline(source_root: str) -> list[str]:
    problems = []
    for module, folder in PURE_DIRECTORIES:
        for path in sorted(glob.glob(os.path.join(source_root, module, "Public", folder, "*.h"))):
            problems += check_header_definitions(path)
    for module, relative in PURE_FILES:
        path = os.path.join(source_root, module, "Public", relative)
        if os.path.exists(path):
            problems += check_header_definitions(path)
    return problems


def check_header_definitions(path: str, text: str | None = None) -> list[str]:
    """Funzioni definite a livello di namespace senza inline/template/constexpr."""
    if text is None:
        text = open(path, encoding="utf-8").read()

    # Via commenti e stringhe: le graffe dentro un commento non contano.
    text = re.sub(r'/\*.*?\*/', lambda m: '\n' * m.group(0).count('\n'), text, flags=re.S)
    text = re.sub(r'//[^\n]*', '', text)
    text = re.sub(r'"(?:\\.|[^"\\])*"', '""', text)
    lines = text.splitlines()

    problems = []
    stack = []            # un elemento per graffa aperta: True se e' di un namespace
    pending_namespace = False
    previous = ""

    for number, line in enumerate(lines, 1):
        stripped = line.strip()
        at_namespace_scope = all(stack)

        if at_namespace_scope and stripped and not stripped.startswith('#'):
            match = DEFINITION_START.match(line)
            if match:
                head_words = set(re.findall(r'[A-Za-z_]\w*', match.group("head")))
                is_function = (not head_words & NOT_A_TYPE
                               and "operator" not in head_words
                               and not stripped.endswith(';'))
                # Definizione se la graffa arriva su questa riga o sulla prossima
                # non vuota (dopo eventuali righe di parametri).
                if is_function:
                    rest = lines[number - 1:number + 6]
                    joined = " ".join(rest)
                    brace = joined.find('{')
                    semicolon = joined.find(';')
                    defines = brace != -1 and (semicolon == -1 or brace < semicolon)
                    qualified = head_words & {"inline", "constexpr", "template", "static"} \
                        or previous.startswith("template")
                    if defines and not qualified:
                        problems.append(
                            f"{path}:{number}: '{match.group('name')}' e' definita in un header dello "
                            f"strato puro senza 'inline': due .cpp che lo includono danno LNK2005.")

        if re.search(r'\bnamespace\b', stripped):
            pending_namespace = True
        for char in line:
            if char == '{':
                stack.append(pending_namespace)
                pending_namespace = False
            elif char == '}' and stack:
                stack.pop()
        if stripped:
            previous = stripped
    return problems


def main() -> int:
    root = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "..", "Source")

    problems = (check_pure_is_header_only(root) + check_public_free_functions(root)
                + check_pure_definitions_are_inline(root))

    if problems:
        print(f"  FALLITO - {len(problems)} simboli a rischio di errore di link:")
        for problem in problems:
            print(f"    {problem}")
        return 1

    print("  ok - strato puro header-only e tutto inline, nessuna funzione libera non esportata")
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""
Trova i parametri di funzione che nascondono un membro della stessa classe.

PERCHE' SERVE
Unreal compila con lo shadowing trattato come ERRORE, non come avvertimento.
Un parametro chiamato come un membro non e' quindi uno stile discutibile: e' un
file che non compila. La convenzione del motore e' prefissare il parametro con
"In" (In, InName, bInEnabled), proprio per evitarlo.

Questo controllo esiste perche' il C++ del progetto non viene mai compilato con
Unreal nell'ambiente in cui e' scritto, quindi un errore del genere si
scoprirebbe solo sulla macchina di chi costruisce. E' successo davvero, su
UGeoQuadtreeSubsystem: il membro bEnabled e i parametri bEnabled di
SetDebugOverlayEnabled e SetDebugDrawEnabled.

L'analisi e' per CLASSE, con conteggio delle graffe: un membro di una classe non
puo' essere nascosto da un parametro di un'altra classe nello stesso file.
"""
import glob
import os
import re
import sys

CLASS_START = re.compile(r'^\s*(?:class|struct)\s+[\w_]*\s*[\w:]*\s*(?:final\s*)?(?::[^{]*)?\{?\s*$')
CLASS_NAME = re.compile(r'^\s*(?:class|struct)\s+(?:[A-Z_]+_API\s+)?(\w+)')

# Dichiarazione di variabile membro: ha un tipo e un nome, NON ha parentesi.
MEMBER = re.compile(r'^\s+(?:mutable\s+|static\s+|inline\s+|constexpr\s+|const\s+)*'
                    r'[\w:]+(?:\s*<[^;]*>)?\s*[\*&]?\s+'
                    r'(b?[A-Z]\w*)\s*(?:=[^;]*)?;\s*(?://.*)?$')

# Parametri: tutto cio' che sta fra le parentesi di una firma.
SIGNATURE = re.compile(r'\b\w[\w:<>,\s\*&]*\s+\w+\s*\(([^)]*)\)')


def parameter_names(parameter_text: str) -> list[str] | None:
    """
    Nomi dei parametri, oppure None se non e' una DICHIARAZIONE.

    Il discriminante fra una dichiarazione e una chiamata e' che ogni parametro
    dichiarato ha un tipo E un nome, cioe' almeno due identificatori una volta
    tolti const, riferimenti e puntatori. In una chiamata gli argomenti sono
    espressioni con un identificatore solo. Senza questo controllo,
    "return EcefToEnu(GeodeticToEcef(Geodetic, Ellipsoid))" verrebbe letto come
    una funzione con parametri Geodetic ed Ellipsoid.
    """
    names = []
    for chunk in parameter_text.split(','):
        chunk = chunk.split('=')[0].strip()          # via il valore di default
        if not chunk:
            continue

        # Parametri variadici o senza nome: si ignorano.
        if chunk in ('...', 'void'):
            continue

        bare = chunk.replace('const', ' ').replace('&', ' ').replace('*', ' ')
        bare = re.sub(r'<[^>]*>', ' ', bare)         # via gli argomenti template
        tokens = [token for token in bare.split() if token]

        if len(tokens) < 2:
            return None                              # e' una chiamata, non una firma

        names.append(tokens[-1])

    return names


def scan(path: str) -> list[str]:
    problems = []
    lines = open(path, encoding='utf-8').read().splitlines()

    class_name = None
    depth = 0
    members: set[str] = set()
    pending: list[tuple[int, list[str]]] = []

    def flush():
        for line_number, names in pending:
            for name in names:
                if name in members:
                    problems.append(
                        f"{path}:{line_number}: il parametro '{name}' nasconde il membro "
                        f"'{name}' di {class_name or '?'} "
                        f"(rinominalo in 'In{name.lstrip('b') if name.startswith('b') else name}' "
                        f"o 'b{'In' + name[1:] if name.startswith('b') else name}')")
        pending.clear()

    for index, line in enumerate(lines, start=1):
        stripped = line.strip()
        if stripped.startswith('//') or stripped.startswith('*'):
            continue

        match = CLASS_NAME.match(line)
        if match and ('{' in line or (index < len(lines))):
            if class_name is not None:
                flush()
            class_name = match.group(1)
            members = set()
            depth = 0

        depth += line.count('{') - line.count('}')
        if class_name is not None and depth <= 0 and '}' in line:
            flush()
            class_name = None
            members = set()
            continue

        if class_name is None:
            continue

        member = MEMBER.match(line)
        if member and '(' not in line:
            members.add(member.group(1))
            continue

        # Le righe che cominciano con return o con un'assegnazione sono codice,
        # non dichiarazioni: si saltano prima ancora di provare.
        if stripped.startswith('return ') or stripped.startswith('='):
            continue

        for signature in SIGNATURE.finditer(line):
            names = parameter_names(signature.group(1))
            if names:
                pending.append((index, names))

    flush()
    return problems


def main() -> int:
    root = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        os.path.dirname(os.path.abspath(__file__)), '..', 'Source')

    headers = sorted(glob.glob(os.path.join(root, '**', '*.h'), recursive=True))
    problems = []
    for header in headers:
        problems.extend(scan(header))

    if problems:
        print(f"  FALLITO - {len(problems)} parametri nascondono un membro:")
        for problem in problems:
            print(f"    {problem}")
        print("    -> Unreal compila con lo shadowing come ERRORE. Convenzione: prefisso 'In'.")
        return 1

    print(f"  ok - nessun parametro nasconde un membro ({len(headers)} header)")
    return 0


if __name__ == '__main__':
    sys.exit(main())

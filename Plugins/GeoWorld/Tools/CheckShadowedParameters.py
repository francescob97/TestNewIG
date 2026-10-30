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

ANCHE LE VARIABILI LOCALI (C4458, "declaration of 'X' hides class member").
Nei .cpp, dentro le funzioni membro (Classe::Metodo), una variabile locale, una
variabile di un for o un parametro di una lambda con il nome di un membro e'
lo stesso errore. E' successo con UGeoWeatherSubsystem: il membro Ground
(lo stato del suolo) e' arrivato DOPO due funzioni che avevano gia' una
"double Ground" locale. Il controllo dei soli parametri negli header non lo
vedeva: il nome nuovo era giusto li', e sbagliato in un altro file.
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
                    r'(b?[A-Z]\w*)\s*(?:\[[^\]]*\])?\s*(?:=[^;]*|\{[^;]*\})?;\s*(?://.*)?$')

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


class ClassTracker:
    """
    Chi contiene cosa, riga per riga, con una PILA di classi: una struct
    dichiarata dentro una classe (FObserverWind dentro UGeoWeatherSubsystem)
    non fa perdere la classe esterna. La versione precedente teneva una classe
    sola, e dopo la prima struct annidata smetteva di vedere i membri.
    """

    def __init__(self):
        self.depth = 0
        self.stack: list[dict] = []       # {"name", "body", "members", "lines"}
        self.pending: str | None = None

    def feed(self, line: str):
        """Restituisce (classe piu' interna o None, True se la riga e' DIRETTAMENTE nel corpo)."""
        code = strip_strings(line)
        stripped = code.strip()
        match = CLASS_NAME.match(code)
        if match and not stripped.endswith(';'):
            self.pending = match.group(1)
        innermost = self.stack[-1] if self.stack else None
        direct = innermost is not None and self.depth == innermost["body"]
        closed = []
        for char in code:
            if char == '{':
                self.depth += 1
                if self.pending:
                    self.stack.append({"name": self.pending, "body": self.depth,
                                       "members": set(), "lines": []})
                    self.pending = None
            elif char == '}':
                if self.stack and self.stack[-1]["body"] == self.depth:
                    closed.append(self.stack.pop())
                self.depth -= 1
        return innermost, direct, closed


def scan(path: str) -> list[str]:
    """Gli header: parametri delle firme nel corpo di una classe, e locali dei corpi inline."""
    problems = []
    tracker = ClassTracker()
    for index, line in enumerate(open(path, encoding='utf-8').read().splitlines(), start=1):
        stripped = line.strip()
        if stripped.startswith('//') or stripped.startswith('*') or stripped.startswith('/*'):
            continue
        innermost, direct, closed = tracker.feed(line)
        if innermost is not None:
            code = strip_strings(line)
            if direct:
                member = MEMBER.match(code)
                if member and '(' not in code:
                    innermost["members"].add(member.group(1))
                elif not stripped.startswith('return ') and not stripped.startswith('='):
                    for signature in SIGNATURE.finditer(code):
                        for name in parameter_names(signature.group(1)) or []:
                            innermost["lines"].append((index, name, "il parametro"))
            else:
                for name in local_names(code):
                    innermost["lines"].append((index, name, "la variabile"))
        for finished in closed:
            for line_number, name, what in finished["lines"]:
                if name in finished["members"]:
                    problems.append(f"{path}:{line_number}: {what} '{name}' nasconde il membro "
                                    f"'{name}' di {finished['name']}")
    return problems


# --- Le variabili locali nei .cpp ------------------------------------------------

#: Una dichiarazione locale: un tipo (con la maiuscola, o uno dei tipi base) e un
#: nome, poi "=", ";", "{" o "(". "Ground.Advance(...)" o "return Ground;" no.
LOCAL = re.compile(
    r'^\s*(?:const\s+|static\s+|constexpr\s+)*'
    r'(?:[A-Z]\w*(?:::\w+)*(?:\s*<[^;=()]*>)?|double|float|int|bool|auto|'
    r'int8|int16|int32|int64|uint8|uint16|uint32|uint64|size_t)'
    r'\s*(?:const\s*)?[\*&]*\s+(\w+)\s*(?:=|;|\{|\(|\[)')
#: Le altre variabili della stessa dichiarazione: "double A = 1.0, B = 2.0;".
MORE_LOCALS = re.compile(r',\s*(\w+)\s*(?:=|;)')
FOR_LOCAL = re.compile(r'\bfor\s*\(\s*(?:const\s+)?[\w:]+(?:\s*<[^;:()]*>)?\s*[\*&]*\s+(\w+)\s*[:=]')
LAMBDA = re.compile(r'\]\s*\(([^()]*)\)')
DEFINITION = re.compile(r'^(?:[\w:<>,\*&]+\s+)*?[\*&]?(\w+)::(~?\w+)\s*\(')


def class_members(headers: list[str]) -> dict[str, set[str]]:
    """Classe -> nomi dei membri, da tutti gli header."""
    members: dict[str, set[str]] = {}
    for path in headers:
        tracker = ClassTracker()
        for line in open(path, encoding='utf-8').read().splitlines():
            stripped = line.strip()
            if stripped.startswith('//') or stripped.startswith('*') or stripped.startswith('/*'):
                continue
            innermost, direct, closed = tracker.feed(line)
            if innermost is not None and direct:
                member = MEMBER.match(strip_strings(line))
                if member and '(' not in strip_strings(line):
                    innermost["members"].add(member.group(1))
            for finished in closed:
                members.setdefault(finished["name"], set()).update(finished["members"])
    return members


def local_names(body: str) -> list[str]:
    """Le variabili dichiarate in una riga di codice: locali, del for, parametri di lambda."""
    names = []
    local = LOCAL.match(body)
    if local and not body.lstrip().startswith(('return ', 'else ', 'case ', 'delete ', 'new ')):
        names.append(local.group(1))
        names.extend(MORE_LOCALS.findall(body[local.end(1):].split('(')[0] + ';'))
    names.extend(FOR_LOCAL.findall(body))
    for lambda_parameters in LAMBDA.findall(body):
        names.extend(parameter_names(lambda_parameters) or [])
    return names


def strip_strings(line: str) -> str:
    """Via i letterali e i commenti di fine riga: le graffe e i nomi li' dentro non contano."""
    line = re.sub(r'"(?:[^"\\]|\\.)*"', '""', line)
    return line.split('//', 1)[0]


def scan_source(path: str, members: dict[str, set[str]]) -> list[str]:
    problems = []
    lines = open(path, encoding='utf-8').read().splitlines()
    index = 0
    while index < len(lines):
        line = strip_strings(lines[index])
        match = DEFINITION.match(line)
        if not match or match.group(1) not in members or line.rstrip().endswith(';'):
            index += 1
            continue
        class_name = match.group(1)
        hidden = members[class_name]

        # La firma, anche su piu' righe, fino alla graffa del corpo.
        signature = line
        start = index
        while '{' not in signature and index + 1 < len(lines) and ';' not in signature:
            index += 1
            signature += ' ' + strip_strings(lines[index])
        if '{' not in signature:
            index += 1
            continue
        opening = signature.index('(', match.start(2))
        level = 0
        for position in range(opening, len(signature)):
            level += {'(': 1, ')': -1}.get(signature[position], 0)
            if level == 0:
                break
        for name in parameter_names(signature[opening + 1:position]) or []:
            if name in hidden:
                problems.append(f"{path}:{start + 1}: il parametro '{name}' nasconde il membro "
                                f"'{name}' di {class_name}")

        # Il corpo: dalla prima graffa a quella che la chiude.
        depth = signature.count('{') - signature.count('}')
        index += 1
        while index < len(lines) and depth > 0:
            body = strip_strings(lines[index])
            for name in local_names(body):
                if name in hidden:
                    problems.append(f"{path}:{index + 1}: la variabile '{name}' nasconde il membro "
                                    f"'{name}' di {class_name} (C4458): rinomina la variabile o il membro")
            depth += body.count('{') - body.count('}')
            index += 1
    return problems


def main() -> int:
    root = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        os.path.dirname(os.path.abspath(__file__)), '..', 'Source')

    headers = sorted(glob.glob(os.path.join(root, '**', '*.h'), recursive=True))
    sources = sorted(glob.glob(os.path.join(root, '**', '*.cpp'), recursive=True))
    problems = []
    for header in headers:
        problems.extend(scan(header))
    members = class_members(headers)
    for source in sources:
        problems.extend(scan_source(source, members))

    if problems:
        print(f"  FALLITO - {len(problems)} nomi nascondono un membro:")
        for problem in problems:
            print(f"    {problem}")
        print("    -> Unreal compila con lo shadowing come ERRORE. Convenzione: prefisso 'In' per i parametri.")
        return 1

    print(f"  ok - nessun parametro o variabile nasconde un membro ({len(headers)} header, {len(sources)} .cpp)")
    return 0


if __name__ == '__main__':
    sys.exit(main())

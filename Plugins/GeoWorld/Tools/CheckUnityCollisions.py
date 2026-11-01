#!/usr/bin/env python3
"""
Cerca i nomi che la BUILD UNITY di Unreal farebbe scontrare.

IL PROBLEMA, IN BREVE
Unreal non compila un .cpp alla volta: ne incolla parecchi dello stesso modulo
in un unico file e compila quello ("unity build", o "jumbo build"). Serve a
ridurre di molto i tempi di compilazione.

La conseguenza che sorprende: due namespace ANONIMI di file diversi diventano
LO STESSO namespace. Due funzioni omonime, che in compilazione separata
sarebbero due entita' distinte e invisibili l'una all'altra, all'improvviso si
scontrano:

    C2084: function 'PackKey' already has a body

Vale anche per le funzioni `static` a livello di file, per la stessa ragione.

PERCHE' UNO SCRIPT
Perche' qui non c'e' Unreal e non si compila: questo errore viaggerebbe fino
alla macchina Windows. E' successo con PackKey, definita in quattro file
diversi -- tre dei quali erano stati scritti esattamente cosi' per mesi senza
dare problemi, finche' il quarto non e' finito nello stesso blocco unity.

LA CORREZIONE giusta quasi mai e' rinominare: se lo stesso nome e' servito in
piu' file, quella funzione vuole stare in un header condiviso.

ALTRE DUE TRAPPOLE DELLA STESSA FAMIGLIA (controllate qui sotto)

B. Un nome di file (namespace anonimo, static) diventa "globale" per gli altri
   file del blocco, e un parametro o una variabile con lo stesso nome lo
   nasconde. Il motore compila con gli avvisi come errori:

       C4459: declaration of 'MinSpeedMs' hides global declaration

   (GeoFlyPawn.cpp aveva la costante MinSpeedMs, GeoWeatherSubsystem.cpp un
   parametro con lo stesso nome.) Qui si rinomina il parametro.

C. Un .cpp usa un namespace nostro (SharedMemory::Name) senza includere
   l'header che lo dichiara: compila solo perche' un altro .cpp dello stesso
   blocco lo include prima. Basta un file nuovo nel modulo per cambiare i
   blocchi, e l'errore compare in un file che nessuno ha toccato:

       C2653: 'SharedMemory': is not a class or namespace name

   Il controllo: ogni namespace nostro usato con "::" deve essere dichiarato
   nel file stesso o in un header che il file include (anche indirettamente).
"""

import os
import re
import sys
from collections import defaultdict

SOURCE_DIR = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "Source")

#: Una definizione a livello di file: "TIPO Nome(" oppure "TIPO Nome =" / ";".
#: I tipi possono avere qualificatori, puntatori e template, quindi si accetta
#: qualunque cosa prima del nome purche' la riga non sia una chiamata.
DEFINITION = re.compile(
    r'^\s*(?:FORCEINLINE\s+|inline\s+|constexpr\s+|const\s+|static\s+)*'
    r'[A-Za-z_][A-Za-z0-9_:<>,\s\*&]*?'
    r'\b([A-Za-z_][A-Za-z0-9_]*)\s*\(')

#: Una variabile o costante di file: "TIPO Nome = ...;", "TIPO Nome[3] = {", "TIPO Nome;".
VARIABLE_DEFINITION = re.compile(
    r'^\s*(?:static\s+|constexpr\s+|const\s+|inline\s+)*'
    r'[A-Za-z_][A-Za-z0-9_:<>,\s\*&]*?[\s\*&]'
    r'([A-Za-z_][A-Za-z0-9_]*)\s*(?:\[[^\]]*\])?\s*(?:=|;|\{)')

#: Un tipo di file: struct, class, enum, using.
TYPE_DEFINITION = re.compile(r'^\s*(?:struct|class|enum(?:\s+class)?|using)\s+([A-Za-z_][A-Za-z0-9_]*)\b(?!\s*namespace)')

STATIC_DEFINITION = re.compile(
    r'^\s*static\s+(?:FORCEINLINE\s+|inline\s+|constexpr\s+|const\s+)*'
    r'[A-Za-z_][A-Za-z0-9_:<>,\s\*&]*?'
    r'\b([A-Za-z_][A-Za-z0-9_]*)\s*\(')

#: Nomi che non contano: sono costrutti del linguaggio o del motore, non
#: definizioni di funzione.
IGNORED = {
    "if", "for", "while", "switch", "return", "sizeof", "static_cast",
    "reinterpret_cast", "const_cast", "dynamic_cast", "namespace", "else",
    "catch", "throw", "decltype", "alignas", "operator", "public", "private",
    "protected", "default", "delete",
}

#: Parole che precedono un nome senza dichiararlo ("return MinSpeedMs;").
NOT_A_TYPE = {
    "return", "else", "case", "goto", "delete", "new", "throw", "co_return", "co_yield",
    "sizeof", "typename", "using", "namespace", "struct", "class", "enum", "public",
    "private", "protected", "template", "operator", "friend", "virtual", "override",
}


def strip_cpp(text):
    """
    Il testo senza commenti e senza il contenuto delle stringhe, in UNA passata
    da sinistra a destra (gli a capo restano: i numeri di riga non cambiano).
    L'ordine conta: un "/*" dentro un commento // o una stringa non apre niente
    ("// (Models/*.h)" non e' l'inizio di un commento a blocco).
    """
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            while i < n and text[i] != "\n":
                i += 1
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "*":
            end = text.find("*/", i + 2)
            end = n if end < 0 else end + 2
            out.append("\n" * text.count("\n", i, end))
            i = end
            continue
        if c == "R" and i + 1 < n and text[i + 1] == '"' and (i == 0 or not (text[i - 1].isalnum() or text[i - 1] == "_")):
            open_paren = text.find("(", i + 2)
            delimiter = text[i + 2:open_paren] if open_paren >= 0 else ""
            if open_paren >= 0 and len(delimiter) <= 16 and '"' not in delimiter and " " not in delimiter:
                close = text.find(")" + delimiter + '"', open_paren + 1)
                end = n if close < 0 else close + len(delimiter) + 2
                out.append('""' + "\n" * text.count("\n", i, end))
                i = end
                continue
        if c in "\"'":
            j = i + 1
            while j < n and text[j] != c and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            out.append(c + c)
            i = j + 1
            continue
        out.append(c)
        i += 1
    return "".join(out)


def code_lines(path):
    """Le righe senza commenti e senza il contenuto delle stringhe."""
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        return strip_cpp(handle.read()).split("\n")


def anonymous_namespace_names(path):
    """Nomi definiti dentro un `namespace { ... }` senza nome, e static di file."""
    return set(file_scope_definitions(path))


def file_scope_definitions(path):
    """Nome -> riga, per funzioni, variabili e tipi del namespace anonimo e per gli static di file."""
    lines = code_lines(path)
    found = {}
    depth = 0
    in_anonymous = False
    anonymous_depth = 0
    pending_anonymous = False

    for number, line in enumerate(lines, start=1):
        # `static` a livello di file: stessa trappola, senza namespace.
        if depth == 0:
            match = STATIC_DEFINITION.match(line)
            if match and match.group(1) not in IGNORED:
                found.setdefault(match.group(1), number)

        if re.match(r'^\s*namespace\s*$', line) or re.match(r'^\s*namespace\s*\{', line):
            pending_anonymous = True

        depth_before = depth
        for character in line:
            if character == '{':
                depth += 1
                if pending_anonymous:
                    in_anonymous = True
                    anonymous_depth = depth
                    pending_anonymous = False
            elif character == '}':
                if in_anonymous and depth == anonymous_depth:
                    in_anonymous = False
                depth -= 1

        if in_anonymous and depth_before == anonymous_depth:
            for pattern in (TYPE_DEFINITION, DEFINITION, VARIABLE_DEFINITION):
                match = pattern.match(line)
                if match and match.group(1) not in IGNORED and match.group(1) not in NOT_A_TYPE:
                    found.setdefault(match.group(1), number)
                    break

    return found


def module_sources(module_dir):
    """I .cpp e gli header di un modulo (Private e Public)."""
    paths = []
    for sub in ("Private", "Public"):
        base = os.path.join(module_dir, sub)
        for root, _dirs, files in os.walk(base):
            for name in sorted(files):
                if name.endswith((".cpp", ".h")):
                    paths.append(os.path.join(root, name))
    return paths


def declaration_pattern(name):
    """Una dichiarazione di variabile o parametro chiamata `name` ("double MinSpeedMs)", "const FX& MinSpeedMs =")."""
    return re.compile(
        r'(?:^|[(,;{}]|\s)(?:const\s+|static\s+|constexpr\s+|volatile\s+)*'
        r'([A-Za-z_][A-Za-z0-9_:]*)(<[^;(){}]*>)?[\*&]*\s+' + re.escape(name) + r'\s*(?:=|,|\)|;|\[|\{|:(?!:))')


def scope_map(lines):
    """
    Per ogni riga: il tipo di scope all'inizio ("namespace", "class", "block") e
    quante parentesi tonde sono aperte. Serve a distinguere un parametro o una
    variabile locale (che nascondono un globale: C4459) da un membro di classe o
    da una costante di namespace (che non lo fanno).
    """
    stack = ["namespace"]
    parens = 0
    header = ""
    states = []
    for line in lines:
        states.append((stack[-1], parens, len(stack)))
        for character in line:
            if character == "(":
                parens += 1
            elif character == ")":
                parens = max(0, parens - 1)
            elif character == "{":
                if re.search(r'\bnamespace\b', header):
                    stack.append("namespace")
                elif re.search(r'\b(class|struct|union|enum)\b(?![^(]*\()', header):
                    # (le macro prima, come UCLASS(), non contano: conta che dopo "class" non ci siano parentesi)
                    stack.append("class")
                else:
                    stack.append("block")
                header = ""
                parens = 0
                continue
            elif character == "}":
                if len(stack) > 1:
                    stack.pop()
                header = ""
                continue
            elif character == ";":
                header = ""
                continue
            header += character
        header += " "
    return states


def check_hidden_globals(module, module_dir, definitions):
    """B: parametri e variabili che nascondono un nome di file di un .cpp dello stesso modulo (C4459)."""
    problems = []
    where = {(path, line) for name, places in definitions.items() for path, line in places}
    patterns = {name: declaration_pattern(name) for name in definitions}
    for path in module_sources(module_dir):
        lines = code_lines(path)
        states = scope_map(lines)
        for number, line in enumerate(lines, start=1):
            if (path, number) in where:
                continue
            kind, parens_at_start, _depth = states[number - 1]
            for name, pattern in patterns.items():
                if name not in line:
                    continue
                for match in pattern.finditer(line):
                    template = match.group(2) or ""
                    if match.group(1) in NOT_A_TYPE or "&&" in template or "||" in template:
                        continue
                    before = line[:match.start(1)]
                    in_parens = parens_at_start + before.count("(") - before.count(")") > 0
                    if kind != "block" and not in_parens:
                        continue   # un membro di classe, una costante di namespace: non nascondono niente
                    owners = ", ".join(sorted({os.path.basename(p) for p, _ in definitions[name]}))
                    problems.append((os.path.relpath(path, module_dir), number, name, owners))
                    break
    return problems


INCLUDE = re.compile(r'^\s*#\s*include\s*"([^"]+)"')
NAMESPACE_DECLARATION = re.compile(r'^\s*(?:inline\s+)?namespace\s+([A-Za-z_][A-Za-z0-9_:]*)\s*(?:\{|$|=)')
TYPE_DECLARATION = re.compile(r'\b(?:struct|class|enum(?:\s+class)?|union)\s+(?:[A-Z_]+_API\s+)?([A-Za-z_][A-Za-z0-9_]*)')
QUALIFIER = re.compile(r'\b([A-Za-z_][A-Za-z0-9_]*)\s*::')


def scan_project():
    """Per ogni file nostro: gli include risolti, i namespace e i tipi che dichiara."""
    files = {}
    index = {}
    for module in sorted(os.listdir(SOURCE_DIR)):
        for sub in ("Public", "Private"):
            base = os.path.join(SOURCE_DIR, module, sub)
            for root, _dirs, names in os.walk(base):
                for name in names:
                    if name.endswith((".h", ".cpp", ".inl")):
                        path = os.path.join(root, name)
                        index.setdefault(os.path.relpath(path, base).replace(os.sep, "/"), path)
                        files[path] = None
    for path in files:
        includes, namespaces, types = [], set(), set()
        for line in code_lines(path):
            match = INCLUDE.match(line.replace('""', '"__"')) if False else None
            raw = re.match(r'^\s*#\s*include\s*', line)
            if raw:
                continue
            match = NAMESPACE_DECLARATION.match(line)
            if match:
                namespaces.update(match.group(1).split("::"))
            types.update(TYPE_DECLARATION.findall(line))
        # Gli include si leggono dal testo vero (code_lines toglie il contenuto delle stringhe).
        with open(path, "r", encoding="utf-8", errors="replace") as handle:
            for raw in handle:
                match = INCLUDE.match(raw)
                if match:
                    target = index.get(match.group(1))
                    if not target:
                        local = os.path.normpath(os.path.join(os.path.dirname(path), match.group(1)))
                        target = local if os.path.isfile(local) else None
                    if target:
                        includes.append(target)
        files[path] = (includes, namespaces, types)
    return files


GLOBAL_TYPE = re.compile(r'^\s*(?:template\s*<[^>]*>\s*)?(?:struct|class|enum(?:\s+class)?)\s+(?:[A-Z_]+_API\s+)?([A-Z][A-Za-z0-9_]+)\b')


def global_type_definitions(path):
    """I tipi DEFINITI (con il corpo) fuori da ogni namespace e classe: le classi Unreal del plugin."""
    lines = code_lines(path)
    states = scope_map(lines)
    text = "\n".join(lines)
    offsets = [0]
    for line in lines:
        offsets.append(offsets[-1] + len(line) + 1)
    found = set()
    for number, line in enumerate(lines, start=1):
        _kind, _parens, depth = states[number - 1]
        if depth != 1:
            continue
        match = GLOBAL_TYPE.match(line)
        if not match:
            continue
        rest = text[offsets[number - 1] + match.end():]
        brace, semicolon = rest.find("{"), rest.find(";")
        if brace >= 0 and (semicolon < 0 or brace < semicolon):
            found.add(match.group(1))
    return found


def check_unity_types(files):
    """D: tipi del plugin (fuori dai namespace) usati senza un include che li dichiari, almeno in avanti."""
    defined = set()
    for path in files:
        if path.endswith(".h"):
            defined |= global_type_definitions(path)

    closure_cache = {}

    def visible(path):
        if path in closure_cache:
            return closure_cache[path]
        seen, stack, names = set(), [path], set()
        while stack:
            current = stack.pop()
            if current in seen:
                continue
            seen.add(current)
            includes, _namespaces, types = files[current]
            names |= types
            stack.extend(includes)
        closure_cache[path] = names
        return names

    problems = []
    for path in sorted(files):
        if path.endswith(".inl"):
            continue
        reported = set()
        for number, line in enumerate(code_lines(path), start=1):
            if re.match(r'^\s*#', line):
                continue
            for token in set(re.findall(r'\b([A-Z][A-Za-z0-9_]+)\b', line)):
                if token in defined and token not in reported and token not in visible(path):
                    reported.add(token)
                    problems.append((os.path.relpath(path, SOURCE_DIR), number, token))
    return problems


def check_unity_includes(files):
    """C: namespace nostri usati senza l'header che li dichiara (compilano solo grazie al blocco unity)."""
    ours = set()
    types = set()
    for _includes, namespaces, declared in files.values():
        ours |= namespaces
        types |= declared
    # std (le nostre specializzazioni di std::hash) e UE non sono namespace nostri.
    candidates = ours - types - {"std", "UE"}

    closure_cache = {}

    def closure(path):
        if path in closure_cache:
            return closure_cache[path]
        seen, stack, names = set(), [path], set()
        while stack:
            current = stack.pop()
            if current in seen:
                continue
            seen.add(current)
            includes, namespaces, _types = files[current]
            names |= namespaces
            stack.extend(includes)
        closure_cache[path] = names
        return names

    problems = []
    for path in sorted(files):
        if path.endswith(".inl"):
            continue
        visible = None
        for number, line in enumerate(code_lines(path), start=1):
            if re.match(r'^\s*#', line):
                continue
            for token in QUALIFIER.findall(line):
                if token not in candidates:
                    continue
                if visible is None:
                    visible = closure(path)
                if token not in visible:
                    problems.append((os.path.relpath(path, SOURCE_DIR), number, token))
    return problems


def main():
    print("== Collisioni da build unity ==")
    print("   (due .cpp dello stesso modulo diventano una sola unita' di traduzione)")
    print()

    failures = 0

    for module in sorted(os.listdir(SOURCE_DIR)):
        private = os.path.join(SOURCE_DIR, module, "Private")
        if not os.path.isdir(private):
            continue

        by_name = defaultdict(list)
        definitions = defaultdict(list)
        file_count = 0

        for root, _dirs, files in os.walk(private):
            for name in sorted(files):
                if not name.endswith(".cpp"):
                    continue
                path = os.path.join(root, name)
                file_count += 1
                for symbol, line in file_scope_definitions(path).items():
                    by_name[symbol].append(os.path.relpath(path, private))
                    definitions[symbol].append((path, line))

        clashes = {name: paths for name, paths in by_name.items() if len(paths) > 1}

        if clashes:
            print(f"  {module}: FALLITO")
            for name, paths in sorted(clashes.items()):
                print(f"    '{name}' definito in {len(paths)} file:")
                for path in paths:
                    print(f"        {path}")
            print("    -> mettilo in un header condiviso invece di rinominarlo:")
            print("       se e' servito in piu' file, non appartiene a nessuno di loro")
            failures += 1
        else:
            print(f"  {module}: ok ({file_count} file, {len(by_name)} nomi locali)")

        hidden = check_hidden_globals(module, os.path.join(SOURCE_DIR, module), definitions)
        if hidden:
            print(f"  {module}: FALLITO -- nomi che nascondono un nome di file di un altro .cpp (C4459)")
            for path, number, name, owners in hidden:
                print(f"    {path}:{number}: '{name}' (di file in {owners})")
            print("    -> rinomina il parametro o la variabile: nel blocco unity il nome di file e' globale")
            failures += 1

    print()
    print("== Namespace e tipi usati senza il loro header (compilano solo nel blocco unity) ==")
    project = scan_project()
    missing = check_unity_includes(project)
    for path, number, token in check_unity_types(project):
        print(f"  FALLITO - {path}:{number}: il tipo '{token}' senza un include che lo dichiari")
        failures += 1
    if missing:
        for path, number, token in missing:
            print(f"  FALLITO - {path}:{number}: '{token}::' senza un include che lo dichiari")
        print("  -> includi l'header del namespace nel file che lo usa")
        failures += 1
    else:
        print("  ok - ogni namespace nostro arriva da un include del file")
    if not failures:
        print("  ok - ogni tipo del plugin arriva da un include del file (o da una dichiarazione in avanti)")

    print()
    if failures:
        print(f"{failures} PROBLEMA/I DA BUILD UNITY")
        return 1

    print("NESSUNA COLLISIONE")
    return 0


if __name__ == "__main__":
    sys.exit(main())

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

STATIC_DEFINITION = re.compile(
    r'^\s*static\s+(?:FORCEINLINE\s+|inline\s+|constexpr\s+|const\s+)*'
    r'[A-Za-z_][A-Za-z0-9_:<>,\s\*&]*?'
    r'\b([A-Za-z_][A-Za-z0-9_]*)\s*\(')

#: Nomi che non contano: sono costrutti del linguaggio o del motore, non
#: definizioni di funzione.
IGNORED = {
    "if", "for", "while", "switch", "return", "sizeof", "static_cast",
    "reinterpret_cast", "const_cast", "dynamic_cast", "namespace", "else",
    "catch", "throw", "decltype", "alignas", "operator",
}


def anonymous_namespace_names(path):
    """Nomi definiti dentro un `namespace { ... }` senza nome, e static di file."""
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        lines = handle.readlines()

    found = set()
    depth = 0
    in_anonymous = False
    anonymous_depth = 0
    pending_anonymous = False

    for raw in lines:
        line = re.sub(r'//.*$', '', raw)

        # `static` a livello di file: stessa trappola, senza namespace.
        if depth == 0:
            match = STATIC_DEFINITION.match(line)
            if match and match.group(1) not in IGNORED:
                found.add(match.group(1))

        if re.match(r'^\s*namespace\s*$', line) or re.match(r'^\s*namespace\s*\{', line):
            pending_anonymous = True

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

        if in_anonymous and depth == anonymous_depth:
            match = DEFINITION.match(line)
            if match and match.group(1) not in IGNORED:
                found.add(match.group(1))

    return found


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
        file_count = 0

        for root, _dirs, files in os.walk(private):
            for name in sorted(files):
                if not name.endswith(".cpp"):
                    continue
                path = os.path.join(root, name)
                file_count += 1
                for symbol in anonymous_namespace_names(path):
                    by_name[symbol].append(os.path.relpath(path, private))

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

    print()
    if failures:
        print(f"{failures} MODULO/I CON COLLISIONI")
        return 1

    print("NESSUNA COLLISIONE")
    return 0


if __name__ == "__main__":
    sys.exit(main())

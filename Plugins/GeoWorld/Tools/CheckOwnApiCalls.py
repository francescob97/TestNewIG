#!/usr/bin/env python3
"""
Controlla che i metodi chiamati sulle NOSTRE classi esistano davvero.

PERCHE'
Lo strato puro si compila e si testa qui; lo strato Unreal no. Quando il codice
Unreal chiama un metodo di una classe nostra sbagliando il nome, l'errore viaggia
fino alla macchina Windows:

    C2039: 'SetBudget' is not a member of 'TTileCache<FImageTile>'

E' un errore banale -- il metodo si chiamava SetBudgetBytes -- ma costa un giro
di compilazione a chi sta dall'altra parte. Ed e' proprio il tipo di errore che
si moltiplica quando si scrive una seconda implementazione guardando la prima:
si ricorda il concetto, non il nome esatto.

COME
1. Raccoglie i metodi dichiarati in ogni classe/struct dei nostri header.
2. Trova le variabili (membri e locali) dichiarate con uno di quei tipi.
3. Verifica che ogni chiamata su quelle variabili corrisponda a un metodo noto.

E' volutamente CONSERVATIVO: se non riesce a capire il tipo di qualcosa, sta
zitto. Meglio lasciar passare un errore che segnalarne dieci inesistenti, perche'
un controllo che urla al lupo viene disattivato dopo tre giorni.
"""

import os
import re
import sys

SOURCE_DIR = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "Source")

CLASS_HEADER = re.compile(
    r'(?:^|\n)\s*(?:class|struct)\s+(?:[A-Z_]+_API\s+)?([A-Za-z_][A-Za-z0-9_]*)\s*'
    r'(?::\s*(?:public|private|protected)\s+([A-Za-z_][A-Za-z0-9_:<>, ]*?))?\s*(?=[{;\n])')

#: "Tipo Nome;" oppure "Ns::Tipo<...> Nome = ...;"
#:
#: Il qualificatore di namespace e' facoltativo e viene scartato: le nostre
#: classi si scrivono a volte per nome nudo e a volte per nome completo
#: (GeoWorld::Tiles::TTileCache), e sono la stessa classe.
DECLARATION = re.compile(
    r'^\s*(?:const\s+|static\s+|mutable\s+)*'
    r'(?:[A-Za-z_][A-Za-z0-9_]*::)*'
    r'([A-Z][A-Za-z0-9_]*)\s*(?:<[^;]*>)?\s+'
    r'([A-Za-z_][A-Za-z0-9_]*)\s*(?:=[^;]*)?;')

CALL = re.compile(r'\b([A-Za-z_][A-Za-z0-9_]*)\s*\.\s*([A-Za-z_][A-Za-z0-9_]*)\s*\(')

#: Metodi che arrivano da basi che non analizziamo, o generati dal compilatore.
ALWAYS_ALLOWED = {
    "operator", "IsValid", "Get", "Reset", "Empty", "Num", "Add", "Remove",
    "Find", "Contains", "Set", "Release", "Pin", "ToString",
}


def class_bodies(text):
    """Genera (nome, base, corpo) per ogni classe o struct del file."""
    for match in CLASS_HEADER.finditer(text):
        name = match.group(1)
        base = match.group(2)

        opening = text.find('{', match.end())
        if opening == -1:
            continue
        # Una dichiarazione anticipata ("class X;") non ha corpo: il primo
        # carattere significativo dopo il nome sarebbe un ';'.
        between = text[match.end():opening]
        if ';' in between:
            continue

        depth = 0
        for index in range(opening, len(text)):
            if text[index] == '{':
                depth += 1
            elif text[index] == '}':
                depth -= 1
                if depth == 0:
                    yield name, base, text[opening + 1:index]
                    break


def collect_classes(header_paths):
    """
    Nome della classe -> insieme dei nomi seguiti da '(' nel suo corpo.

    VOLUTAMENTE GENEROSO. Raccoglie ogni identificatore seguito da parentesi,
    quindi anche le chiamate fatte DENTRO i metodi inline finiscono nell'insieme.
    Il risultato e' un sovrainsieme dei metodi veri, e va benissimo: serve a
    rispondere a "questo nome esiste da qualche parte in questa classe?", e un
    nome inventato non c'e' comunque. La prima versione cercava di essere
    precisa, non gestiva le firme su piu' righe, e produceva 48 falsi positivi --
    cioe' era inutilizzabile.
    """
    methods = {}
    bases = {}

    for path in header_paths:
        with open(path, "r", encoding="utf-8", errors="replace") as handle:
            text = handle.read()

        text = re.sub(r'//[^\n]*', '', text)
        text = re.sub(r'/\*.*?\*/', '', text, flags=re.S)

        for name, base, body in class_bodies(text):
            found = set(re.findall(r'\b([A-Za-z_][A-Za-z0-9_]*)\s*\(', body))
            methods.setdefault(name, set()).update(found)
            if base:
                bases[name] = base.split('<')[0].split('::')[-1]

    return methods, bases


def known_methods(class_name, methods, bases, seen=None):
    """Metodi della classe piu' quelli delle basi che conosciamo."""
    seen = seen or set()
    if class_name in seen or class_name not in methods:
        return None            # base sconosciuta: meglio non dire niente
    seen.add(class_name)

    result = set(methods[class_name])
    base = bases.get(class_name)
    if base:
        inherited = known_methods(base, methods, bases, seen)
        if inherited is None:
            return None        # catena che finisce fuori dal nostro codice
        result |= inherited
    return result


def paired_header(path):
    """
    L'header che accompagna un .cpp, secondo la convenzione del progetto:
    Private/X/Y.cpp <-> Public/X/Y.h

    Serve perche' i membri sono dichiarati nell'header e usati nel .cpp. Senza
    questo, il controllo non vedrebbe proprio i casi che contano di piu': e'
    esattamente cosi' che la prima versione non ha trovato l'errore per cui era
    stata scritta.
    """
    if not path.endswith(".cpp") or os.sep + "Private" + os.sep not in path:
        return None
    candidate = path.replace(os.sep + "Private" + os.sep, os.sep + "Public" + os.sep)
    candidate = candidate[:-4] + ".h"
    return candidate if os.path.exists(candidate) else None


def declarations_in(lines, methods):
    declared = {}
    for raw in lines:
        line = re.sub(r'//.*$', '', raw)
        match = DECLARATION.match(line)
        if match:
            # Si registrano ANCHE i tipi che non sono nostri (FVector, FString...).
            # Serve proprio a rendere ambiguo un nome usato sia per un nostro
            # tipo sia per uno del motore: nei test 'Position' e' FEcef in una
            # funzione e FVector in un'altra, e filtrando subito sui nostri tipi
            # la seconda dichiarazione sparirebbe, facendo sembrare il nome
            # univoco.
            declared.setdefault(match.group(2), set()).add(match.group(1))
    return declared


def check_file(path, methods, bases):
    """Ritorna la lista dei problemi trovati in un file."""
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        lines = handle.readlines()

    # Nome -> insieme dei tipi con cui e' dichiarato.
    #
    # Un nome puo' essere riusato in blocchi diversi con tipi diversi: nei test
    # 'Position' e' FEcef in una funzione e FVector in un'altra. Senza tenere
    # conto degli scope non si puo' sapere di quale si parli, quindi un nome
    # ambiguo si scarta. E' la promessa fatta in testa al file: meglio lasciar
    # passare un errore che segnalarne due inesistenti.
    declared = declarations_in(lines, methods)

    header = paired_header(path)
    if header:
        with open(header, "r", encoding="utf-8", errors="replace") as handle:
            for name, types in declarations_in(handle.readlines(), methods).items():
                declared.setdefault(name, set()).update(types)

    variables = {name: next(iter(types))
                 for name, types in declared.items()
                 if len(types) == 1 and next(iter(types)) in methods}

    problems = []
    for number, raw in enumerate(lines, start=1):
        line = re.sub(r'//.*$', '', raw)
        for receiver, method in CALL.findall(line):
            class_name = variables.get(receiver)
            if not class_name or method in ALWAYS_ALLOWED:
                continue
            available = known_methods(class_name, methods, bases)
            if available is None or method in available:
                continue
            suggestion = [name for name in sorted(available)
                          if name.lower().startswith(method.lower()[:6])]
            problems.append((number, receiver, class_name, method, suggestion))

    return problems


def main():
    headers = []
    sources = []
    for root, _dirs, files in os.walk(SOURCE_DIR):
        for name in sorted(files):
            path = os.path.join(root, name)
            if name.endswith(".h"):
                headers.append(path)
            if name.endswith((".h", ".cpp")):
                sources.append(path)

    methods, bases = collect_classes(headers)

    print("== Metodi chiamati sulle nostre classi ==")
    print(f"   ({len(methods)} classi note in {len(headers)} header)")
    print()

    failures = 0
    for path in sources:
        for number, receiver, class_name, method, suggestion in check_file(path, methods, bases):
            relative = os.path.relpath(path, SOURCE_DIR)
            print(f"  FALLITO - {relative}:{number}")
            print(f"    '{method}' non e' un metodo di {class_name} (variabile '{receiver}')")
            if suggestion:
                print(f"    forse intendevi: {', '.join(suggestion)}")
            failures += 1

    if failures:
        print()
        print(f"{failures} CHIAMATA/E A METODI INESISTENTI")
        return 1

    print("  ok - nessuna chiamata a metodi inesistenti")
    return 0


if __name__ == "__main__":
    sys.exit(main())

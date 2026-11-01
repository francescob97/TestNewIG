#!/usr/bin/env python3
"""
Cerca i nomi che in Unreal (o in windows.h) sono MACRO.

IL PROBLEMA
Lo strato puro si compila qui, con g++, senza Unreal: un nome come PI e' un
nome qualsiasi. Ma Unreal definisce

    #define PI (3.1415926535897932f)

e quando lo stesso header si compila dentro il motore, il preprocessore
sostituisce il nome prima che il compilatore lo veda:

    for (size_t PI = 0; PI < N; ++PI)      diventa
    for (size_t (3.14...f) = 0; ...)       C2106: '=': left operand must be l-value

E' successo in GltfReader.h (l'indice delle primitive si chiamava PI). Vale
per tutta una famiglia: le costanti di UnrealMathUtility.h (PI, HALF_PI,
DELTA, SMALL_NUMBER...), check/verify/ensure, TEXT, e le macro di windows.h
che Unreal lascia passare nei file che lo includono (near, far, small,
interface, IN, OUT, ERROR...).

LE REGOLE
1. Nello strato puro (gli header "C++ PURO") questi nomi non compaiono
   proprio, nel codice: nessun uso e' legittimo, perche' lo strato puro non
   conosce Unreal.
2. Nel codice Unreal si possono USARE (PI e' una costante del motore), ma non
   DICHIARARE: una variabile, un parametro o una funzione con quel nome.

I test (Tools/StandaloneTests) non si compilano nel motore: non si guardano.
"""

import os
import re
import sys

SOURCE_DIR = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "Source")

#: Le macro di Unreal (UnrealMathUtility.h, AssertionMacros.h, Platform.h...).
UNREAL_MACROS = {
    "PI", "HALF_PI", "TWO_PI", "INV_PI", "DOUBLE_PI", "DOUBLE_HALF_PI", "DOUBLE_TWO_PI", "DOUBLE_INV_PI",
    "SMALL_NUMBER", "KINDA_SMALL_NUMBER", "BIG_NUMBER", "DELTA", "EULERS_NUMBER", "MAX_FLT",
    "FLOAT_NORMAL_THRESH", "THRESH_POINT_ON_PLANE", "THRESH_VECTOR_NORMALIZED", "INDEX_NONE",
    "check", "checkf", "checkSlow", "verify", "verifyf", "ensure", "ensureMsgf", "TEXT", "UE_LOG",
}

#: Le macro di windows.h (e degli header che tira dentro) che fanno danni come nomi.
WINDOWS_MACROS = {
    "near", "far", "small", "hyper", "interface", "IN", "OUT", "OPTIONAL", "ERROR", "DELETE",
    "CONST", "VOID", "TRUE", "FALSE", "CALLBACK", "PASCAL", "WINAPI",
    "CreateFile", "DeleteFile", "CopyFile", "MoveFile", "CreateDirectory", "RemoveDirectory",
    "GetObject", "LoadImage", "DrawText", "SendMessage", "PostMessage", "GetMessage", "MessageBox",
    "GetClassName", "GetUserName", "GetComputerName", "LoadLibrary", "FormatMessage", "Yield",
    "GetCurrentTime", "OpenFileMapping", "CreateFileMapping", "CreateEvent", "CreateMutex",
}

MACROS = UNREAL_MACROS | WINDOWS_MACROS

#: Le cartelle dello strato puro (come CheckModuleExports.py), piu' gli header che si dichiarano puri.
PURE_DIRECTORIES = [
    ("GeoCore", "Geo"),
    ("GeoTiles", "Tiles"),
    ("GeoRender", "Quadtree"),
    ("GeoRender", "Mesh"),
    ("GeoEntities", "Entities"),
    ("GeoEntities", "Models"),
]

TOKEN = re.compile(r'\b([A-Za-z_][A-Za-z0-9_]*)\b')
DECLARATION = re.compile(
    r'(?:^|[(,;{}]|\s)(?:const\s+|static\s+|constexpr\s+|inline\s+)*'
    r'[A-Za-z_][A-Za-z0-9_:]*(?:<[^;(){}]*>)?[\*&]*\s+'
    r'([A-Za-z_][A-Za-z0-9_]*)\s*(?:=|,|\)|;|\[|\{|\(|:(?!:))')
NOT_A_TYPE = {"return", "else", "case", "goto", "delete", "new", "throw", "sizeof", "using", "namespace",
              "struct", "class", "enum", "typename", "template", "operator", "public", "private", "protected",
              "define", "undef", "ifdef", "ifndef", "if", "elif", "pragma", "include"}


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


def is_pure(path):
    relative = os.path.relpath(path, SOURCE_DIR).replace(os.sep, "/")
    for module, folder in PURE_DIRECTORIES:
        if relative.startswith(f"{module}/Public/{folder}/") or relative.startswith(f"{module}/Private/{folder}/"):
            return True
    if not path.endswith((".h", ".inl")):
        return False
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        head = "".join(handle.readlines()[:40])
    return "C++ PURO" in head


def main():
    print("== Nomi che in Unreal o in windows.h sono macro ==")
    problems = []
    pure_count = 0
    other_count = 0
    for root, _dirs, files in os.walk(SOURCE_DIR):
        for name in sorted(files):
            if not name.endswith((".h", ".cpp", ".inl")):
                continue
            path = os.path.join(root, name)
            pure = is_pure(path)
            pure_count += pure
            other_count += not pure
            for number, line in enumerate(code_lines(path), start=1):
                if line.lstrip().startswith("#"):
                    continue
                if pure:
                    for token in TOKEN.findall(line):
                        if token in MACROS:
                            problems.append((path, number, token, "usato nello strato puro"))
                else:
                    for match in DECLARATION.finditer(line):
                        token = match.group(1)
                        before = line[:match.start(1)].split()
                        if token in MACROS and not (before and before[-1] in NOT_A_TYPE):
                            problems.append((path, number, token, "dichiarato"))
    if problems:
        problems = sorted(set(problems), key=lambda item: (item[0], item[1], item[2]))
        for path, number, token, how in problems:
            print(f"  FALLITO - {os.path.relpath(path, SOURCE_DIR)}:{number}: '{token}' {how}")
        print("  -> rinominalo: in Unreal il preprocessore lo sostituirebbe prima del compilatore")
        return 1
    print(f"  ok - nessuna macro usata come nome ({pure_count} file puri, {other_count} file Unreal)")
    return 0


if __name__ == "__main__":
    sys.exit(main())

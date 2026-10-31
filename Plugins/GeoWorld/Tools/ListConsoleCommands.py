#!/usr/bin/env python3
"""
L'elenco dei comandi console di GeoWorld, letto dal codice: docs/comandi-unreal.md.

PERCHE' UN GENERATORE. La lista di geo.Help era scritta a mano ed era rimasta
a 25 comandi su 96: ogni comando nuovo andava aggiunto in due posti, e il
secondo si dimentica. Qui l'elenco si ricava dalle registrazioni stesse
(static FAutoConsoleCommand... (TEXT("nome"), TEXT("aiuto"), ...)): il nome e
l'aiuto che Unreal mostra sono gli stessi che finiscono nel file.

    python3 Plugins/GeoWorld/Tools/ListConsoleCommands.py           rifa' il file
    python3 Plugins/GeoWorld/Tools/ListConsoleCommands.py --check   dice se e' indietro

Il --check gira in CheckSourceDiscipline.sh (regola 7): un comando aggiunto
senza rifare l'elenco fa fallire i controlli.
"""

from __future__ import annotations

import os
import re
import sys

TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
SOURCE_DIR = os.path.normpath(os.path.join(TOOLS_DIR, "..", "Source"))
REPO_DIR = os.path.normpath(os.path.join(TOOLS_DIR, "..", "..", ".."))
OUTPUT = os.path.join(REPO_DIR, "docs", "comandi-unreal.md")

REGISTRATION = re.compile(
    r'static\s+FAutoConsoleCommand\w*\s+\w+\s*\(\s*'
    r'TEXT\("(?P<name>[^"]+)"\)\s*,\s*'
    r'(?P<help>(?:TEXT\("(?:[^"\\]|\\.)*"\)\s*)+)',
    re.S)
LITERAL = re.compile(r'TEXT\("((?:[^"\\]|\\.)*)"\)')

#: I gruppi, nell'ordine in cui si incontrano lavorando: titolo e una riga.
GROUPS = [
    ("", "Generale", "Dove sei, andare in un posto, l'origine del mondo, la diagnosi."),
    ("Tiles", "Le tile delle quote", "Aprire un dataset del terreno, streaming e cache (Fase 3)."),
    ("Lod", "Cosa disegnare", "Livello di dettaglio, residenza, precarico (Fase 4 e dopo)."),
    ("Terrain", "Il terreno", "Geometria, gonne, ombre, budget, la demo del terreno (Fase 5)."),
    ("Imagery", "Le ortofoto", "Le foto sul terreno, e i materiali di tutto il progetto (Fase 6)."),
    ("Roads", "Strade, ferrovie, piste", "Dipinte sul terreno e 3D da vicino (Fase 8)."),
    ("Water", "L'acqua", "Mare, laghi, fiumi (Fase 9)."),
    ("Weather", "Il meteo", "Vento, pioggia, neve, grandine, fulmini e tuono."),
    ("Vegetation", "La vegetazione", "Alberi e arbusti dalla copertura del suolo, la libreria dei modelli (Fase 11)."),
    ("Buildings", "Gli edifici", "Le sagome di OpenStreetMap estruse, i monumenti dalla libreria (Fase 10)."),
    ("Fly", "La camera di volo", "Solo in Play: velocita' proporzionale alla quota."),
]

#: Moduli i cui comandi esistono solo nell'editor.
EDITOR_ONLY_MODULES = {"GeoWorldEditor"}

#: "Per cominciare": scritto a mano, ma ogni comando citato deve esistere.
START_HERE = [
    ("vedere un dataset del terreno", 'geo.Terrain.Demo "D:\\GeoWorld\\Pipeline\\dataset\\dted"'),
    ("terreno e ortofoto", 'geo.Imagery.Demo "<terreno>" "<ortofoto>"'),
    ("terreno, ortofoto e strade", 'geo.Roads.Demo "<terreno>" "<ortofoto>" "<strade>"'),
    ("terreno, ortofoto, acqua (e strade)", 'geo.Water.Demo "<terreno>" "<ortofoto>" "<acqua>" "<strade>"'),
    ("terreno, ortofoto e alberi", 'geo.Vegetation.Demo "<terreno>" "<ortofoto>" "<vegetazione>"'),
    ("terreno, ortofoto ed edifici", 'geo.Buildings.Demo "<terreno>" "<ortofoto>" "<edifici>"'),
    ("andare in un posto", "geo.Goto 45.07 7.69 2000   oppure   geo.Goto Torino"),
    ("sapere dove sono", "geo.Where   (e le stampe a destra: geo.Hud 1)"),
    ("volare (in Play)", "geo.Fly"),
    ("rifare i materiali dopo un aggiornamento", "geo.Imagery.CreateMaterial"),
    ("pioggia, neve, sereno", "geo.Weather.Rain 4   geo.Weather.Snow 1   geo.Weather.Clear"),
    ("un temporale, o un fulmine subito", "geo.Weather.Storm   oppure   geo.Weather.Strike 1500"),
    ("vento", "geo.Weather.Wind 10 270"),
    ("capire perche' non vedo niente", "geo.Terrain.Diag   (poi viewmode wireframe, r.Fog 0)"),
    ("i comandi di un gruppo, dentro Unreal", "geo.Help Weather"),
]

#: Comandi del motore che la documentazione usa.
ENGINE_COMMANDS = [
    ("viewmode wireframe", "solo i triangoli, senza materiali; `viewmode lit` torna normale"),
    ("r.Fog 0", "spegne la nebbia (il terreno lontano sparisce nella nebbia); `r.Fog 1` la riaccende"),
    ("stat fps", "i fotogrammi al secondo"),
    ("stat unit", "quanto costano game thread, render thread e GPU, in millisecondi"),
    ("stat gpu", "il costo della GPU, voce per voce"),
    ("t.MaxFPS 60", "limita i fotogrammi al secondo (0 = nessun limite)"),
    ("r.ScreenPercentage 50", "disegna a meta' risoluzione: per capire se il limite e' la GPU"),
]


def unescape(text: str) -> str:
    return text.replace('\\"', '"').replace("\\\\", "\\")


def find_commands() -> list[dict]:
    """Ogni registrazione: nome, aiuto, modulo, file, posizione (per l'ordine)."""
    commands = []
    for directory, _, files in os.walk(SOURCE_DIR):
        for filename in sorted(files):
            if not filename.endswith((".cpp", ".h")):
                continue
            path = os.path.join(directory, filename)
            with open(path, encoding="utf-8") as handle:
                text = handle.read()
            module = os.path.relpath(path, SOURCE_DIR).split(os.sep)[0]
            for match in REGISTRATION.finditer(text):
                help_text = "".join(unescape(part) for part in LITERAL.findall(match.group("help")))
                commands.append({
                    "name": match.group("name"),
                    "help": help_text,
                    "module": module,
                    "file": os.path.relpath(path, REPO_DIR).replace(os.sep, "/"),
                    "offset": match.start(),
                })
    commands.sort(key=lambda command: (command["file"], command["offset"]))
    return commands


def group_of(name: str, groups_with_children: set[str]) -> str:
    """geo.Weather.Wind -> Weather; geo.Fly -> Fly (perche' c'e' geo.Fly.Speed); geo.Where -> ''."""
    parts = name.split(".")
    if len(parts) >= 3:
        return parts[1]
    if len(parts) == 2 and parts[1] in groups_with_children:
        return parts[1]
    return ""


def split_usage(name: str, help_text: str) -> tuple[str, str]:
    """'geo.X <a> - fa cosa' -> ('geo.X <a>', 'fa cosa'); senza uso scritto, il nome."""
    if help_text.startswith(name) and " - " in help_text:
        usage, description = help_text.split(" - ", 1)
        return usage.strip(), description.strip()
    return name, help_text.strip()


def code(text: str) -> str:
    return "`" + text.replace("|", "\\|") + "`"


def prose(text: str) -> str:
    return text.replace("|", "\\|").replace("<", "&lt;").replace(">", "&gt;")


def anchor(title: str) -> str:
    """L'ancora che GitHub da' a un titolo: minuscolo, spazi -> trattini, via la punteggiatura."""
    slug = title.strip().lower()
    slug = re.sub(r"[^\w\- ]", "", slug)
    return slug.replace(" ", "-")


def render(commands: list[dict]) -> str:
    names = {command["name"] for command in commands}
    for _, example in START_HERE:
        used = re.findall(r"\bgeo\.[A-Za-z0-9.]+[A-Za-z0-9]", example)
        missing = [name for name in used if name not in names]
        if missing:
            raise SystemExit(f"'Per cominciare' cita comandi che non esistono: {missing}")

    with_children = {command["name"].split(".")[1] for command in commands if command["name"].count(".") >= 2}
    by_group: dict[str, list[dict]] = {}
    for command in commands:
        by_group.setdefault(group_of(command["name"], with_children), []).append(command)

    known = [key for key, _, _ in GROUPS]
    ordered = [group for group in GROUPS if group[0] in by_group]
    ordered += [(key, key, "") for key in sorted(by_group) if key not in known]

    lines = [
        "# I comandi di GeoWorld nella console di Unreal",
        "",
        "> **Generato dal codice**: `python3 Plugins/GeoWorld/Tools/ListConsoleCommands.py`.",
        "> Non modificarlo a mano: i controlli statici (`CheckSourceDiscipline.sh`, regola 7)",
        "> dicono quando e' indietro. In Unreal lo stesso elenco lo da' `geo.Help`.",
        "",
        f"**{len(commands)} comandi** in {len(ordered)} gruppi.",
        "",
        "## Come si usano",
        "",
        "- **La console**: il tasto della console (di solito quello sotto Esc); nell'editor anche il",
        "  campo **Cmd** in basso a sinistra. **Tab** completa i nomi.",
        "- `<...>` va scritto, `[...]` si puo' omettere, `a|b` vuol dire uno dei due.",
        "- **I percorsi con spazi vanno fra virgolette**:",
        '  `geo.Terrain.Demo "C:\\Unreal Projects\\TestNewIG\\Pipeline\\dataset\\dted"`.',
        "- Quasi tutti funzionano sia nell'editor sia in Play. `geo.Fly` solo in Play;",
        "  `geo.Imagery.CreateMaterial` solo nell'editor.",
        "- `<0|1>` senza argomento, di solito, inverte lo stato.",
        "- Dentro Unreal: `geo.Help` elenca i gruppi (e tutto nell'Output Log),",
        "  `geo.Help Weather` i comandi di un gruppo, `geo.Help Open` quelli che",
        "  contengono una parola.",
        "",
        "## Per cominciare",
        "",
        "| Voglio... | Comando |",
        "|---|---|",
    ]
    for wish, example in START_HERE:
        lines.append(f"| {wish} | {code(example)} |")

    lines += ["", "## I gruppi", ""]
    for key, title, _ in ordered:
        prefix = f"geo.{key}.*" if key else "geo.*"
        lines.append(f"- [{title}](#{anchor(title)}) — `{prefix}`, {len(by_group[key])} comandi")

    for key, title, description in ordered:
        lines += ["", f"## {title}", ""]
        if description:
            lines += [description, ""]
        lines += ["| Comando | Cosa fa |", "|---|---|"]
        for command in by_group[key]:
            usage, text = split_usage(command["name"], command["help"])
            if command["module"] in EDITOR_ONLY_MODULES:
                text += " *(solo nell'editor)*"
            lines.append(f"| {code(usage)} | {prose(text)} |")

    lines += [
        "",
        "## Comandi del motore utili",
        "",
        "Non sono di GeoWorld, ma la documentazione li usa spesso.",
        "",
        "| Comando | Cosa fa |",
        "|---|---|",
    ]
    for command, text in ENGINE_COMMANDS:
        lines.append(f"| {code(command)} | {text} |")

    lines += [
        "",
        "## Dove stanno nel codice",
        "",
        "| File | Comandi |",
        "|---|---|",
    ]
    per_file: dict[str, int] = {}
    for command in commands:
        per_file[command["file"]] = per_file.get(command["file"], 0) + 1
    for path in sorted(per_file):
        lines.append(f"| `{path}` | {per_file[path]} |")
    lines.append("")
    return "\n".join(lines)


def main() -> int:
    commands = find_commands()
    if not commands:
        print("Nessun comando trovato: il percorso dei sorgenti e' giusto?", SOURCE_DIR)
        return 2
    text = render(commands)
    if "--check" in sys.argv[1:]:
        try:
            with open(OUTPUT, encoding="utf-8") as handle:
                current = handle.read()
        except FileNotFoundError:
            current = ""
        if current != text:
            print(f"  FALLITO - {os.path.relpath(OUTPUT, REPO_DIR)} non corrisponde ai comandi nel codice "
                  f"({len(commands)}). Rifallo con:")
            print("    python3 Plugins/GeoWorld/Tools/ListConsoleCommands.py")
            return 1
        print(f"  ok - l'elenco dei comandi e' aggiornato ({len(commands)} comandi)")
        return 0
    with open(OUTPUT, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(text)
    print(f"Scritto {os.path.relpath(OUTPUT, REPO_DIR)}: {len(commands)} comandi.")
    return 0


if __name__ == "__main__":
    sys.exit(main())

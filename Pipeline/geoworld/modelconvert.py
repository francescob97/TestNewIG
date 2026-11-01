"""
geoworld convert-models: i modelli nei formati chiusi in formati che GeoWorld legge.

GeoWorld legge da solo OpenFlight (.flt), OBJ e glTF (.gltf, .glb)
(docs/modelli-design.md, sezione 3). Gli altri formati non hanno una
specifica da cui scrivere un lettore, o la loro libreria non si puo'
ridistribuire: si convertono UNA volta, fuori da Unreal, con gli strumenti di
chi li ha fatti.

    .ive .osgb .osgt .osg      -> .flt   con osgconv (OpenSceneGraph): conserva
                                         DOF, switch e LOD
    .fbx                       -> .glb   con FBX2glTF (se c'e'), se no Blender
    .dae .3ds .blend .stl .ply
    .abc .usd* .x3d .wrl       -> .glb   con Blender (in background, senza
                                         finestra), animazioni comprese

Il comando cammina in una cartella (anche il database intero), converte quello
che serve e salta quello che e' gia' aggiornato (il file convertito piu'
recente del sorgente), come un make. Senza --output scrive ACCANTO agli
originali: la cartella dei modelli resta la stessa e le texture si trovano
dove erano. Con --output rifa' la struttura delle cartelle altrove e ci copia
anche le texture.

Gli strumenti si cercano nel PATH (e Blender anche nelle cartelle dove
l'installatore lo mette); --osgconv, --fbx2gltf, --blender li indicano a mano.
Se uno manca, i file che ne avrebbero bisogno si elencano con il modo di
procurarselo: il resto si converte lo stesso.

Dopo osgconv si guardano le texture che il .flt nomina (i record della palette
delle texture): se mancano, lo si dice. Succede quando le immagini erano DENTRO
il .ive.
"""

from __future__ import annotations

import glob
import os
import shutil
import struct
import subprocess
import sys
import tempfile
from dataclasses import dataclass, field
from typing import Callable

OSG_EXTENSIONS = (".ive", ".osgb", ".osgt", ".osg")
FBX_EXTENSIONS = (".fbx",)
BLENDER_EXTENSIONS = (".dae", ".3ds", ".blend", ".stl", ".ply", ".abc", ".usd", ".usda", ".usdc", ".usdz", ".x3d", ".wrl")
#: Quelli che GeoWorld legge da se': non si toccano (con --output si copiano).
READABLE_EXTENSIONS = (".flt", ".obj", ".mtl", ".gltf", ".glb", ".bin")
TEXTURE_EXTENSIONS = (".rgb", ".rgba", ".sgi", ".int", ".inta", ".bw", ".png", ".jpg", ".jpeg", ".bmp", ".tga",
                      ".tif", ".tiff", ".dds")

#: Quanto puo' metterci uno strumento per un modello (s).
TIMEOUT_S = 900

HOW_TO_GET = {
    "osgconv": "osgconv fa parte di OpenSceneGraph (openscenegraph.org): installalo e passalo con "
               "--osgconv <percorso di osgconv.exe>",
    "fbx2gltf": "FBX2glTF: github.com/facebookincubator/FBX2glTF (le release hanno l'eseguibile); "
                "oppure installa Blender, che legge anche gli FBX",
    "blender": "Blender (blender.org): installalo, o passalo con --blender <percorso di blender.exe>",
}

#: Lo script che Blender esegue in background: importa, poi esporta in .glb.
BLENDER_SCRIPT = r'''
import os
import sys

import bpy

argv = sys.argv[sys.argv.index("--") + 1:]
source, target = argv[0], argv[1]
extension = os.path.splitext(source)[1].lower()


def first(*names):
    """Il primo operatore che c'e' (i nomi cambiano fra le versioni di Blender)."""
    for name in names:
        group, op = name.split(".")
        module = getattr(bpy.ops, group, None)
        if module is not None and op in dir(module):
            return getattr(module, op)
    raise RuntimeError("questo Blender non sa importare " + extension + " (" + ", ".join(names) + ")")


try:
    if extension == ".blend":
        bpy.ops.wm.open_mainfile(filepath=source)
    else:
        bpy.ops.wm.read_factory_settings(use_empty=True)
        importers = {
            ".fbx": ("import_scene.fbx",),
            ".dae": ("wm.collada_import",),
            ".3ds": ("import_scene.max3ds", "import_scene.autodesk_3ds"),
            ".stl": ("wm.stl_import", "import_mesh.stl"),
            ".ply": ("wm.ply_import", "import_mesh.ply"),
            ".abc": ("wm.alembic_import",),
            ".usd": ("wm.usd_import",), ".usda": ("wm.usd_import",), ".usdc": ("wm.usd_import",), ".usdz": ("wm.usd_import",),
            ".x3d": ("import_scene.x3d",), ".wrl": ("import_scene.x3d",),
            ".obj": ("wm.obj_import", "import_scene.obj"),
        }
        if extension not in importers:
            raise RuntimeError("formato non previsto: " + extension)
        first(*importers[extension])(filepath=source)
    # glTF binario, Y in alto (lo standard), animazioni comprese, senza compressione Draco.
    bpy.ops.export_scene.gltf(filepath=target, export_format="GLB", export_animations=True,
                              export_draco_mesh_compression_enable=False)
except Exception as error:  # noqa: BLE001 -- qualsiasi errore di Blender va riportato, non mostrato in una finestra
    print("GEOWORLD-ERRORE: " + str(error))
    sys.exit(1)
'''


@dataclass
class Job:
    """Un modello da convertire."""
    source: str
    target: str
    tool: str            # "osgconv", "fbx2gltf", "blender"
    relative: str        # il percorso dalla cartella di partenza (per i messaggi)


@dataclass
class Tools:
    osgconv: str | None = None
    fbx2gltf: str | None = None
    blender: str | None = None


@dataclass
class Report:
    converted: list[str] = field(default_factory=list)
    up_to_date: list[str] = field(default_factory=list)
    failed: list[tuple[str, str]] = field(default_factory=list)
    missing_tool: list[tuple[str, str]] = field(default_factory=list)
    missing_textures: list[tuple[str, list[str]]] = field(default_factory=list)
    copied: int = 0

    @property
    def ok(self) -> bool:
        return not self.failed and not self.missing_tool


def find_tools(osgconv: str | None = None, fbx2gltf: str | None = None, blender: str | None = None) -> Tools:
    """Gli strumenti: quelli indicati, poi il PATH; Blender anche dove lo mette l'installatore."""
    def which(*names: str) -> str | None:
        for name in names:
            found = shutil.which(name)
            if found:
                return found
        return None

    tools = Tools(osgconv=osgconv or which("osgconv"), fbx2gltf=fbx2gltf or which("FBX2glTF", "fbx2gltf",
                  "FBX2glTF-windows-x64", "FBX2glTF-linux-x64", "FBX2glTF-darwin-x64"),
                  blender=blender or which("blender"))
    if not tools.blender:
        patterns = [r"C:\Program Files\Blender Foundation\Blender*\blender.exe",
                    "/Applications/Blender.app/Contents/MacOS/Blender"]
        candidates = sorted(path for pattern in patterns for path in glob.glob(pattern))
        tools.blender = candidates[-1] if candidates else None
    return tools


def tool_for(extension: str, tools: Tools) -> str:
    """Quale strumento serve a un'estensione (FBX: FBX2glTF se c'e', se no Blender)."""
    if extension in OSG_EXTENSIONS:
        return "osgconv"
    if extension in FBX_EXTENSIONS:
        return "fbx2gltf" if tools.fbx2gltf or not tools.blender else "blender"
    return "blender"


def is_inside(path: str, folder: str) -> bool:
    """path e' folder o sta dentro (su Windows, due dischi diversi: no)."""
    try:
        return os.path.commonpath([path, folder]) == folder
    except ValueError:
        return False


def plan(source_dir: str, output_dir: str | None, tools: Tools, osg_format: str = "flt") -> tuple[list[Job], list[str]]:
    """
    Cosa convertire: i job, e (con --output) i file da copiare cosi' come sono
    (texture e modelli che si leggono gia'). In ordine, per ripetibilita'.
    """
    jobs: list[Job] = []
    copies: list[str] = []
    convertible = OSG_EXTENSIONS + FBX_EXTENSIONS + BLENDER_EXTENSIONS
    output_abs = os.path.abspath(output_dir) if output_dir else None
    for root, directories, files in os.walk(source_dir):
        directories.sort()
        if output_abs and is_inside(os.path.abspath(root), output_abs):
            continue   # l'uscita dentro la cartella di partenza: non si riconverte quello che si e' scritto
        for name in sorted(files):
            path = os.path.join(root, name)
            relative = os.path.relpath(path, source_dir)
            stem, extension = os.path.splitext(relative)
            extension = extension.lower()
            if extension in convertible:
                tool = tool_for(extension, tools)
                suffix = "." + osg_format if tool == "osgconv" else ".glb"
                target = os.path.join(output_dir or source_dir, stem + suffix)
                jobs.append(Job(source=path, target=target, tool=tool, relative=relative))
            elif output_dir and (extension in TEXTURE_EXTENSIONS or extension in READABLE_EXTENSIONS):
                copies.append(relative)
    return jobs, copies


def is_up_to_date(source: str, target: str) -> bool:
    return os.path.isfile(target) and os.path.getmtime(target) >= os.path.getmtime(source)


def command_line(job: Job, tools: Tools, script_path: str) -> list[str]:
    """La riga di comando dello strumento (uno script .py si lancia con questo Python: serve ai test)."""
    def launcher(tool_path: str) -> list[str]:
        return [sys.executable, tool_path] if tool_path.lower().endswith(".py") else [tool_path]

    if job.tool == "osgconv":
        return launcher(tools.osgconv or "osgconv") + [job.source, job.target]
    if job.tool == "fbx2gltf":
        # FBX2glTF vuole l'uscita SENZA estensione: con --binary aggiunge .glb.
        return launcher(tools.fbx2gltf or "FBX2glTF") + ["--binary", "--input", job.source,
                                                          "--output", os.path.splitext(job.target)[0]]
    return launcher(tools.blender or "blender") + ["--background", "--factory-startup", "--python", script_path,
                                                    "--", job.source, job.target]


def flt_texture_names(path: str) -> list[str]:
    """Le texture che un .flt nomina: i record 64 (palette delle texture), nome a 4..204."""
    names: list[str] = []
    try:
        with open(path, "rb") as handle:
            data = handle.read()
    except OSError:
        return names
    offset = 0
    while offset + 4 <= len(data):
        opcode, length = struct.unpack_from(">HH", data, offset)
        if length < 4:
            break
        if opcode == 64 and offset + 204 <= len(data):
            raw = data[offset + 4:offset + 204].split(b"\0", 1)[0]
            names.append(raw.decode("latin-1"))
        offset += length
    return names


def missing_textures(model: str, search_dirs: list[str]) -> list[str]:
    """Le texture di un .flt che non si trovano accanto al modello, ne' per nome nelle cartelle indicate."""
    missing = []
    directory = os.path.dirname(model)
    for raw in flt_texture_names(model):
        name = raw.replace("\\", "/").rsplit("/", 1)[-1]
        candidates = [raw, os.path.join(directory, raw), os.path.join(directory, name)]
        candidates += [os.path.join(d, name) for d in search_dirs]
        if not any(os.path.isfile(c) for c in candidates):
            missing.append(name)
    return missing


def convert(source_dir: str, output_dir: str | None = None, tools: Tools | None = None, osg_format: str = "flt",
            force: bool = False, dry_run: bool = False, report: Callable[[str], None] = print) -> Report:
    """Converte la cartella. Ritorna il resoconto; i messaggi passano da report."""
    if not os.path.isdir(source_dir):
        raise FileNotFoundError(f"la cartella dei modelli non c'e': {source_dir}")
    if osg_format not in ("flt", "obj"):
        raise ValueError("--osg-format: flt (conserva DOF, switch, LOD) o obj")
    tools = tools or find_tools()
    jobs, copies = plan(source_dir, output_dir, tools, osg_format)
    result = Report()
    report(f"{len(jobs)} modelli da convertire in {source_dir}"
           + (f" -> {output_dir}" if output_dir else " (accanto agli originali)"))
    report(f"strumenti: osgconv {tools.osgconv or 'NO'}, FBX2glTF {tools.fbx2gltf or 'NO'}, Blender {tools.blender or 'NO'}")

    # Le copie (con --output): texture e modelli gia' leggibili, se mancano o sono vecchie.
    for relative in copies:
        source = os.path.join(source_dir, relative)
        target = os.path.join(output_dir, relative)
        if is_up_to_date(source, target):
            continue
        if not dry_run:
            os.makedirs(os.path.dirname(target), exist_ok=True)
            shutil.copy2(source, target)
        result.copied += 1

    script_path = ""
    with tempfile.TemporaryDirectory(prefix="geoworld-convert-") as scratch:
        if any(job.tool == "blender" for job in jobs):
            script_path = os.path.join(scratch, "geoworld_blender_export.py")
            with open(script_path, "w", encoding="utf-8") as handle:
                handle.write(BLENDER_SCRIPT)
        for job in jobs:
            tool_path = getattr(tools, job.tool)
            if not force and is_up_to_date(job.source, job.target):
                result.up_to_date.append(job.relative)
                continue
            if not tool_path:
                result.missing_tool.append((job.relative, job.tool))
                report(f"  {job.relative}: manca {job.tool}")
                continue
            arguments = command_line(job, tools, script_path)
            if dry_run:
                report(f"  {job.relative} -> {os.path.relpath(job.target, output_dir or source_dir)}  ({job.tool})")
                result.converted.append(job.relative)
                continue
            os.makedirs(os.path.dirname(job.target) or ".", exist_ok=True)
            try:
                completed = subprocess.run(arguments, capture_output=True, text=True, timeout=TIMEOUT_S,
                                           errors="replace", check=False)
            except (OSError, subprocess.TimeoutExpired) as error:
                result.failed.append((job.relative, str(error)))
                report(f"  {job.relative}: ERRORE {error}")
                continue
            if completed.returncode != 0 or not os.path.isfile(job.target):
                output = (completed.stdout or "") + (completed.stderr or "")
                lines = [line for line in output.splitlines() if line.strip()]
                marked = [line.split("GEOWORLD-ERRORE:", 1)[1].strip() for line in lines if "GEOWORLD-ERRORE:" in line]
                reason = marked[-1] if marked else (lines[-1] if lines else f"uscita {completed.returncode}, nessun file scritto")
                result.failed.append((job.relative, reason))
                report(f"  {job.relative}: ERRORE ({job.tool}) {reason}")
                continue
            result.converted.append(job.relative)
            report(f"  {job.relative} -> {os.path.relpath(job.target, output_dir or source_dir)}")
            if job.target.lower().endswith(".flt"):
                lost = missing_textures(job.target, [os.path.dirname(job.source), source_dir])
                if lost:
                    result.missing_textures.append((job.relative, lost))
                    report(f"    texture che il .flt nomina e non ci sono: {', '.join(lost[:8])}"
                           + (" ..." if len(lost) > 8 else "")
                           + " (se erano DENTRO il file OSG vanno estratte a parte, e messe nella cartella dei modelli)")

    report(f"Convertiti {len(result.converted)}, gia' aggiornati {len(result.up_to_date)}, "
           f"errori {len(result.failed)}, senza strumento {len(result.missing_tool)}"
           + (f", file copiati {result.copied}" if output_dir else ""))
    for tool in sorted({tool for _, tool in result.missing_tool}):
        report(f"MANCA {tool}: {HOW_TO_GET[tool]}")
    return result

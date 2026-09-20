"""
Diagnostica dell'ambiente.

Esiste perche' il 90% dei problemi di questa pipeline non sono nel codice ma
nell'installazione: GDAL assente, pyproj installato con pip (che porta un PROJ
con pochissimi dati), griglie geoidiche mancanti, variabili d'ambiente che
puntano altrove. Su Windows succede piu' spesso che altrove, perche' convivono
piu' installazioni di PROJ contemporaneamente (conda, OSGeo4W, la copia dentro
la rotella di pyproj) e vince quella che si trova per prima.

Ogni funzione qui dentro deve poter girare anche quando le dipendenze NON ci
sono: gli import stanno dentro le funzioni e i fallimenti diventano testo, non
eccezioni.
"""

from __future__ import annotations

import glob
import os
import platform
import sys
from dataclasses import dataclass, field


@dataclass
class Report:
    python: dict = field(default_factory=dict)
    gdal: dict = field(default_factory=dict)
    pyproj: dict = field(default_factory=dict)
    imaging: dict = field(default_factory=dict)
    grids: dict = field(default_factory=dict)
    vertical: dict = field(default_factory=dict)

    @property
    def ready(self) -> bool:
        return (self.gdal.get("available", False)
                and self.pyproj.get("available", False)
                and any(entry.get("usable") for entry in self.vertical.values()))


def _python_info() -> dict:
    return {
        "version": sys.version.split()[0],
        "executable": sys.executable,
        "platform": platform.platform(),
        "is64bit": sys.maxsize > 2 ** 32,
        # Su Windows e' quasi sempre la spia del problema: se questo e' un
        # ambiente conda, GDAL e PROJ vanno installati NELLO STESSO ambiente.
        "condaPrefix": os.environ.get("CONDA_PREFIX", ""),
    }


# I driver che vale la pena controllare, con il motivo per cui importano.
#
# PERCHE' ESISTE QUESTA TABELLA. "GDAL legge le ECW" e "la TUA installazione di
# GDAL legge le ECW" sono affermazioni diverse: il driver ECW richiede l'SDK
# proprietario ERDAS e NON e' incluso nelle build di conda-forge. Scoprirlo dopo
# aver lanciato una build su 200 GB di dati e' la cosa da evitare.
INTERESTING_DRIVERS = {
    "GTiff": "GeoTIFF: il formato di lavoro della pipeline",
    "VRT": "mosaici virtuali: indispensabile",
    "DTED": "DTED .dt2: i DEM militari",
    "EHdr": "ESRI BIL/hdr",
    "AAIGrid": "ESRI ASCII grid: i file di TINITALY",
    "JP2OpenJPEG": "JPEG2000: molte ortofoto pubbliche",
    "ECW": "ECW: richiede l'SDK ERDAS, di norma ASSENTE",
    "MrSID": "MrSID: anch'esso con SDK proprietario",
    "JPEG": "JPEG: il payload delle tile di immagine",
    "PNG": "PNG",
    "WMS": "servizi WMS remoti",
}


def _driver_availability(gdal) -> dict:
    return {name: gdal.GetDriverByName(name) is not None
            for name in INTERESTING_DRIVERS}


def _gdal_info() -> dict:
    try:
        from osgeo import gdal
    except Exception as error:                      # noqa: BLE001
        return {"available": False, "error": f"{type(error).__name__}: {error}"}

    try:
        gdal.UseExceptions()
        return {
            "available": True,
            "version": gdal.__version__,
            "driverGTiff": gdal.GetDriverByName("GTiff") is not None,
            "driverVRT": gdal.GetDriverByName("VRT") is not None,
            "drivers": _driver_availability(gdal),
            "dataPath": gdal.GetConfigOption("GDAL_DATA") or "(default interno)",
        }
    except Exception as error:                      # noqa: BLE001
        return {"available": False, "error": f"{type(error).__name__}: {error}"}


def _imaging_info() -> dict:
    """
    numpy e Pillow: servono alla pipeline delle ortofoto (Fase 6).

    Vengono controllati qui e non lasciati fallire all'uso, perche' un
    ModuleNotFoundError a meta' di una build su 200 GB e' il modo peggiore di
    scoprire che manca una libreria.
    """
    report = {}

    try:
        import numpy
        report["numpy"] = numpy.__version__
    except ImportError as error:
        report["numpyError"] = f"{type(error).__name__}: {error}"

    try:
        from PIL import Image
        import PIL
        report["pillow"] = PIL.__version__
    except ImportError as error:
        report["pillowError"] = f"{type(error).__name__}: {error}"

    return report


def _pyproj_info() -> dict:
    try:
        import pyproj
    except Exception as error:                      # noqa: BLE001
        return {"available": False, "error": f"{type(error).__name__}: {error}"}

    data_dirs = pyproj.datadir.get_data_dir().split(os.pathsep)
    return {
        "available": True,
        "version": pyproj.__version__,
        "projVersion": pyproj.proj_version_str,
        "dataDirs": data_dirs,
        "networkEnabled": pyproj.network.is_network_enabled(),
        # PROJ_DATA e' il nome moderno, PROJ_LIB quello storico: se sono
        # impostate e puntano a una cartella sbagliata, PROJ non trova niente.
        "projDataEnv": os.environ.get("PROJ_DATA", ""),
        "projLibEnv": os.environ.get("PROJ_LIB", ""),
        "userWritableDir": _user_grid_dir(),
    }


def _user_grid_dir() -> str:
    """Cartella dove projsync scarica le griglie."""
    try:
        import pyproj
        return pyproj.datadir.get_user_data_dir()
    except Exception:                               # noqa: BLE001
        return ""


def _grid_info() -> dict:
    """Cerca le griglie geoidiche nelle cartelle dati di PROJ."""
    try:
        import pyproj
    except Exception:                               # noqa: BLE001
        return {"searched": [], "found": {}}

    directories = pyproj.datadir.get_data_dir().split(os.pathsep)
    user_dir = _user_grid_dir()
    if user_dir and user_dir not in directories:
        directories.append(user_dir)

    interesting = {
        "EGM2008": ("*egm08*", "*egm2008*"),
        "EGM96": ("*egm96*",),
    }

    found: dict[str, list[str]] = {}
    for name, patterns in interesting.items():
        matches: list[str] = []
        for directory in directories:
            if not os.path.isdir(directory):
                continue
            for pattern in patterns:
                matches.extend(glob.glob(os.path.join(directory, pattern)))
        found[name] = sorted({os.path.basename(path) for path in matches})

    return {"searched": directories, "found": found}


def _vertical_info() -> dict:
    """
    La prova del nove: non basta che il FILE ci sia, deve funzionare la
    trasformazione. Si prova su un punto italiano noto.
    """
    from . import geoid

    results: dict[str, dict] = {}
    for label, crs in (("EGM2008", geoid.VERTICAL_CRS_EGM2008),
                       ("EGM96", geoid.VERTICAL_CRS_EGM96)):
        try:
            info = geoid.check_vertical_transform(crs)
            undulation = info.sample_points[0][3]
            results[label] = {"crs": crs, "usable": True,
                              "undulationColosseo": round(undulation, 3),
                              "pipeline": info.description}
        except Exception as error:                  # noqa: BLE001
            first_line = str(error).strip().splitlines()[0] if str(error).strip() else type(error).__name__
            results[label] = {"crs": crs, "usable": False, "reason": first_line}
    return results


def collect() -> Report:
    report = Report(python=_python_info(), gdal=_gdal_info(), pyproj=_pyproj_info(),
                    imaging=_imaging_info())
    if report.pyproj.get("available"):
        report.grids = _grid_info()
        report.vertical = _vertical_info()
    return report


def format_report(report: Report) -> str:
    lines: list[str] = []
    mark = lambda ok: "OK  " if ok else "NO  "          # noqa: E731

    lines.append("=== Ambiente ===")
    lines.append(f"    Python   : {report.python['version']} "
                 f"({'64 bit' if report.python['is64bit'] else '32 BIT - serve 64 bit'})")
    lines.append(f"    Sistema  : {report.python['platform']}")
    lines.append(f"    Eseguibile: {report.python['executable']}")
    if report.python["condaPrefix"]:
        lines.append(f"    Conda    : {report.python['condaPrefix']}")

    lines.append("")
    lines.append("=== GDAL ===")
    if report.gdal.get("available"):
        lines.append(f"{mark(True)}GDAL {report.gdal['version']}")
        lines.append(f"    driver GTiff: {report.gdal['driverGTiff']}, VRT: {report.gdal['driverVRT']}")

        drivers = report.gdal.get("drivers") or {}
        if drivers:
            lines.append("    formati leggibili:")
            for name, description in INTERESTING_DRIVERS.items():
                present = drivers.get(name, False)
                mark = "si" if present else "NO"
                lines.append(f"      [{mark:>2}] {name:<12} {description}")
            if not drivers.get("ECW", False):
                lines.append("")
                lines.append("    Nota sulle ECW: il driver manca, come nella quasi totalita'")
                lines.append("    delle installazioni. Converti le ECW in GeoTIFF una volta sola")
                lines.append("    (QGIS, o il visualizzatore fornito col database) e poi passa")
                lines.append("    i GeoTIFF a build-imagery: la pipeline non guarda il formato.")
    else:
        lines.append(f"{mark(False)}GDAL non importabile: {report.gdal.get('error')}")

    lines.append("")
    lines.append("=== numpy / Pillow ===")
    if "numpy" in report.imaging:
        lines.append(f"{mark(True)}numpy {report.imaging['numpy']}")
    else:
        lines.append(f"{mark(False)}numpy non importabile: {report.imaging.get('numpyError')}")
    if "pillow" in report.imaging:
        lines.append(f"{mark(True)}Pillow {report.imaging['pillow']}  (tile di ortofoto)")
    else:
        lines.append(f"{mark(False)}Pillow non importabile: {report.imaging.get('pillowError')}")
        lines.append("    Serve solo alle ortofoto (build-imagery). La pipeline delle")
        lines.append("    quote funziona lo stesso.")

    lines.append("")
    lines.append("=== PROJ / pyproj ===")
    if report.pyproj.get("available"):
        lines.append(f"{mark(True)}pyproj {report.pyproj['version']} su PROJ {report.pyproj['projVersion']}")
        for directory in report.pyproj["dataDirs"]:
            exists = os.path.isdir(directory)
            count = len(os.listdir(directory)) if exists else 0
            lines.append(f"    dati : {directory}  ({count} file)" if exists
                         else f"    dati : {directory}  (NON ESISTE)")
        if report.pyproj["projDataEnv"]:
            lines.append(f"    PROJ_DATA = {report.pyproj['projDataEnv']}")
        if report.pyproj["projLibEnv"]:
            lines.append(f"    PROJ_LIB  = {report.pyproj['projLibEnv']}")
        lines.append(f"    cartella scaricabile (projsync): {report.pyproj['userWritableDir']}")
        lines.append(f"    rete PROJ abilitata: {report.pyproj['networkEnabled']}")
    else:
        lines.append(f"{mark(False)}pyproj non importabile: {report.pyproj.get('error')}")

    if report.grids:
        lines.append("")
        lines.append("=== Griglie geoidiche trovate su disco ===")
        for name, files in report.grids["found"].items():
            lines.append(f"{mark(bool(files))}{name}: {', '.join(files) if files else 'nessun file'}")

    if report.vertical:
        lines.append("")
        lines.append("=== Trasformazione verticale (la prova che conta) ===")
        for name, result in report.vertical.items():
            if result["usable"]:
                lines.append(f"{mark(True)}{name} ({result['crs']}): "
                             f"N(Colosseo) = {result['undulationColosseo']} m")
            else:
                lines.append(f"{mark(False)}{name} ({result['crs']}): {result['reason']}")

    lines.append("")
    lines.append("=== Conclusione ===")
    if report.ready:
        usable = [name for name, result in report.vertical.items() if result["usable"]]
        if "EGM2008" in usable:
            lines.append("    Pronto. Puoi lanciare 'build' con le impostazioni di default.")
        else:
            lines.append(f"    Utilizzabile solo con {usable[0]}. Per il dataset definitivo")
            lines.append("    serve EGM2008: vedi le istruzioni qui sotto.")
            lines.append(f"    Per una prova: aggiungi --vertical-crs EPSG:5773")
    else:
        lines.append("    NON pronto. Istruzioni qui sotto.")

    if not report.ready or not report.vertical.get("EGM2008", {}).get("usable"):
        lines.append("")
        lines.append(_install_instructions(report))

    return "\n".join(lines)


def _install_instructions(report: Report) -> str:
    windows = report.python["platform"].lower().startswith("windows")
    conda = bool(report.python["condaPrefix"])

    lines = ["=== Come sistemare ==="]

    # Questo blocco viene PRIMA del ramo su GDAL, che termina con un return:
    # altrimenti, mancando anche GDAL, di Pillow non si leggerebbe mai niente.
    if "pillow" not in report.imaging:
        lines.append("")
        lines.append("  Manca Pillow. Serve solo alle ORTOFOTO (build-imagery,")
        lines.append("  verify-imagery, inspect-imagery); la pipeline delle quote")
        lines.append("  funziona anche senza.")
        lines.append("")
        lines.append("      conda install -c conda-forge pillow")
        lines.append("      # oppure: pip install Pillow")

    if not report.gdal.get("available") or not report.pyproj.get("available"):
        lines.append("")
        lines.append("  Mancano GDAL e/o pyproj.")
        if windows:
            lines.append("  Su Windows la via che funziona senza sorprese e' conda (Miniforge):")
            lines.append("")
            lines.append("      conda create -n geoworld python=3.11")
            lines.append("      conda activate geoworld")
            lines.append("      conda install -c conda-forge gdal pyproj numpy pillow proj-data")
            lines.append("")
            lines.append("  Evita 'pip install gdal': su Windows non esistono rotelle")
            lines.append("  ufficiali su PyPI e la compilazione da sorgente richiede il")
            lines.append("  toolchain C++ piu' le librerie di sistema.")
        else:
            lines.append("      conda install -c conda-forge gdal pyproj numpy pillow proj-data")
            lines.append("      # oppure: apt install gdal-bin python3-gdal python3-pyproj python3-pil proj-data")
        return "\n".join(lines)

    lines.append("")
    lines.append("  GDAL e pyproj ci sono, manca la griglia geoidica EGM2008.")
    lines.append("")
    lines.append("  Opzione 1 - il pacchetto completo delle griglie (consigliata):")
    if conda:
        lines.append("      conda install -c conda-forge proj-data")
    elif windows:
        lines.append("      conda install -c conda-forge proj-data")
        lines.append("      (se non usi conda: OSGeo4W, pacchetto 'proj-data')")
    else:
        lines.append("      conda install -c conda-forge proj-data   # oppure: apt install proj-data")
    lines.append("")
    lines.append("  Opzione 2 - scaricare solo l'area italiana:")
    lines.append("      projsync --bbox 6,35,19,48 --file egm08")
    lines.append(f"      (finisce in {report.pyproj.get('userWritableDir', '?')})")
    lines.append("")
    lines.append("  Opzione 3 - lasciare che PROJ la scarichi al volo:")
    if windows:
        lines.append("      set PROJ_NETWORK=ON            (cmd)")
        lines.append("      $env:PROJ_NETWORK='ON'         (PowerShell)")
    else:
        lines.append("      export PROJ_NETWORK=ON")
    lines.append("      richiede accesso a cdn.proj.org")

    if report.vertical.get("EGM96", {}).get("usable"):
        lines.append("")
        lines.append("  Nel frattempo EGM96 funziona: --vertical-crs EPSG:5773")
        lines.append("  In Italia EGM96 ed EGM2008 differiscono di qualche decimetro.")
        lines.append("  Va bene per provare la pipeline, non per il dataset definitivo.")

    return "\n".join(lines)


def best_available_vertical_crs() -> str | None:
    """Datum verticale utilizzabile, preferendo EGM2008. None se nessuno."""
    from . import geoid

    for crs in (geoid.VERTICAL_CRS_EGM2008, geoid.VERTICAL_CRS_EGM96):
        try:
            geoid.check_vertical_transform(crs)
            return crs
        except Exception:                           # noqa: BLE001
            continue
    return None

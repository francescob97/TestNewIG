"""
Scarica ortofoto Sentinel-2 dal bucket pubblico su AWS.

PERCHE' IL BUCKET E NON L'API STAC
----------------------------------
La via consueta per trovare le scene Sentinel-2 e' l'API STAC di Element 84
(earth-search.aws.element84.com). Qui si legge invece direttamente il bucket S3,
per una ragione precisa: il bucket e' l'unica cosa da cui i dati devono
comunque passare, mentre l'API e' un servizio in piu' che puo' cambiare, essere
irraggiungibile o filtrato da una rete aziendale. Una dipendenza invece di due.

Il costo e' dover calcolare da soli il quadrato MGRS (vedi mgrs.py) e leggere i
metadati scena per scena. Sono trenta righe e una richiesta per scena.

COSA SI SCARICA
---------------
L'asset TCI ("True Colour Image"): le tre bande visibili gia' combinate in un
GeoTIFF a 8 bit, 10 m di risoluzione, circa 230 MB per scena. E' esattamente
cio' che serve per drappeggiare, senza dover comporre le bande a mano.

DUE MODI
--------
Scaricare, oppure NON scaricare e passare alla pipeline gli URL preceduti da
/vsicurl/. In questo secondo caso GDAL legge dalla rete solo le finestre che
servono, sfruttando il fatto che i file sono COG (Cloud Optimized GeoTIFF).
Conviene per provare un'area piccola: si evitano centinaia di megabyte.
"""

from __future__ import annotations

import json
import os
import re
import urllib.request
from dataclasses import dataclass

from . import mgrs

BUCKET = "https://sentinel-cogs.s3.us-west-2.amazonaws.com"
PREFIX = "sentinel-s2-l2a-cogs"

#: Aree di comodo, come per il DEM: ovest, sud, est, nord.
NAMED_AREAS = {
    "test": (12.40, 41.85, 12.60, 41.95),        # Roma, un pezzetto
    "roma": (12.35, 41.78, 12.65, 42.00),
    "torino": (7.55, 45.00, 7.80, 45.15),
    "milano": (9.05, 45.40, 9.30, 45.55),
    "napoli": (14.15, 40.78, 14.40, 40.92),
}

SCENE_PATTERN = re.compile(r"S2[AB]_[A-Z0-9]+_\d{8}_\d+_L2A")


@dataclass
class Scene:
    zone: int
    band: str
    square: str
    name: str
    year: int
    month: int
    cloud_cover: float | None = None
    datetime: str | None = None

    @property
    def directory_url(self) -> str:
        return (f"{BUCKET}/{PREFIX}/{self.zone}/{self.band}/{self.square}"
                f"/{self.year}/{self.month}/{self.name}")

    @property
    def visual_url(self) -> str:
        return f"{self.directory_url}/TCI.tif"

    @property
    def metadata_url(self) -> str:
        return f"{self.directory_url}/{self.name}.json"

    @property
    def vsicurl_path(self) -> str:
        return f"/vsicurl/{self.visual_url}"


def _get(url: str, timeout: float = 60.0) -> bytes:
    request = urllib.request.Request(url, headers={"User-Agent": "geoworld-pipeline"})
    with urllib.request.urlopen(request, timeout=timeout) as response:
        return response.read()


def list_scenes(zone: int, band: str, square: str, year: int, month: int,
                timeout: float = 60.0) -> list[Scene]:
    """Scene presenti sul bucket per un quadrato e un mese."""
    url = (f"{BUCKET}/?list-type=2"
           f"&prefix={PREFIX}/{zone}/{band}/{square}/{year}/{month}/"
           f"&delimiter=/&max-keys=200")
    try:
        body = _get(url, timeout).decode("utf-8", errors="replace")
    except Exception as error:
        raise RuntimeError(
            f"non riesco a elencare {zone}{band}{square} {year}/{month}: {error}. "
            f"Serve accesso a {BUCKET}") from error

    names = []
    for match in SCENE_PATTERN.finditer(body):
        if match.group(0) not in names:
            names.append(match.group(0))

    return [Scene(zone, band, square, name, year, month) for name in names]


def load_metadata(scene: Scene, timeout: float = 60.0) -> Scene:
    """Legge copertura nuvolosa e data dal file JSON della scena."""
    try:
        document = json.loads(_get(scene.metadata_url, timeout))
        properties = document.get("properties", {})
        scene.cloud_cover = properties.get("eo:cloud_cover")
        scene.datetime = properties.get("datetime")
    except Exception:
        # Una scena senza metadati leggibili non e' un errore fatale: resta
        # utilizzabile, semplicemente non si sa quanto sia nuvolosa e finira'
        # in fondo alla classifica.
        scene.cloud_cover = None
    return scene


def find_best_scenes(bbox: tuple[float, float, float, float], *,
                     year: int, months: list[int], max_cloud: float = 10.0,
                     report=print) -> list[Scene]:
    """
    Per ogni quadrato MGRS che tocca il bbox, la scena meno nuvolosa.

    Una scena per quadrato e non tutte: due scene dello stesso quadrato coprono
    lo stesso territorio in giorni diversi, e mosaicarle significherebbe avere
    meta' immagine con le ombre di giugno e meta' con quelle di agosto.
    """
    west, south, east, north = bbox
    squares = mgrs.squares_for_bbox(west, south, east, north)
    report(f"quadrati MGRS da coprire: {', '.join(f'{z}{b}{s}' for z, b, s in squares)}")

    best: list[Scene] = []

    for zone, band, square in squares:
        candidates: list[Scene] = []
        for month in months:
            candidates.extend(list_scenes(zone, band, square, year, month))

        if not candidates:
            report(f"  {zone}{band}{square}: nessuna scena nei mesi richiesti")
            continue

        for scene in candidates:
            load_metadata(scene)

        usable = [s for s in candidates
                  if s.cloud_cover is not None and s.cloud_cover <= max_cloud]
        if not usable:
            clearest = min((s for s in candidates if s.cloud_cover is not None),
                           key=lambda s: s.cloud_cover, default=None)
            if clearest is None:
                report(f"  {zone}{band}{square}: {len(candidates)} scene, metadati illeggibili")
                continue
            report(f"  {zone}{band}{square}: nessuna sotto il {max_cloud:.0f}% di nuvole; "
                   f"la migliore e' al {clearest.cloud_cover:.1f}%")
            best.append(clearest)
            continue

        chosen = min(usable, key=lambda s: s.cloud_cover)
        report(f"  {zone}{band}{square}: {chosen.name}  nuvole {chosen.cloud_cover:.1f}%  "
               f"({len(candidates)} candidate)")
        best.append(chosen)

    return best


def remote_size(url: str, timeout: float = 30.0) -> int | None:
    try:
        request = urllib.request.Request(url, method="HEAD",
                                         headers={"User-Agent": "geoworld-pipeline"})
        with urllib.request.urlopen(request, timeout=timeout) as response:
            length = response.headers.get("Content-Length")
            return int(length) if length else None
    except Exception:
        return None


def download_scene(scene: Scene, directory: str, report=print,
                   timeout: float = 600.0) -> str:
    """Scarica il TCI di una scena. Salta il file se c'e' gia' ed e' completo."""
    os.makedirs(directory, exist_ok=True)
    destination = os.path.join(directory, f"{scene.name}_TCI.tif")

    expected = remote_size(scene.visual_url)
    if os.path.exists(destination):
        if expected is None or os.path.getsize(destination) == expected:
            report(f"  {scene.name}: gia' presente")
            return destination
        report(f"  {scene.name}: incompleto, riscarico")

    megabytes = (expected or 0) / (1024 * 1024)
    report(f"  {scene.name}: scarico {megabytes:.0f} MB")

    temporary = destination + ".part"
    request = urllib.request.Request(scene.visual_url,
                                     headers={"User-Agent": "geoworld-pipeline"})
    with urllib.request.urlopen(request, timeout=timeout) as response, \
            open(temporary, "wb") as handle:
        while True:
            chunk = response.read(1 << 20)
            if not chunk:
                break
            handle.write(chunk)

    os.replace(temporary, destination)
    return destination


def fetch(bbox: tuple[float, float, float, float], directory: str, *,
          year: int, months: list[int], max_cloud: float = 10.0,
          stream: bool = False, report=print) -> list[str]:
    """
    Trova e procura le scene. Ritorna i percorsi da passare a build-imagery.

    Con `stream` non scarica niente: restituisce percorsi /vsicurl/ che GDAL
    legge direttamente dalla rete.
    """
    scenes = find_best_scenes(bbox, year=year, months=months,
                              max_cloud=max_cloud, report=report)
    if not scenes:
        raise RuntimeError(
            "nessuna scena trovata. Prova ad allargare i mesi, ad alzare "
            "--max-cloud, o a controllare che l'area sia sulla terraferma.")

    if stream:
        report("")
        report("Modalita' streaming: niente da scaricare, GDAL leggera' dalla rete.")
        return [scene.vsicurl_path for scene in scenes]

    report("")
    report(f"Scarico {len(scenes)} scene in {directory}")
    return [download_scene(scene, directory, report=report) for scene in scenes]

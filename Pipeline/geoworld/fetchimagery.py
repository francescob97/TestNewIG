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

COME SI SCELGONO LE SCENE (e perche' la prima versione faceva un mosaico)
-----------------------------------------------------------------------
Ogni quadrato MGRS (100 x 100 km) viene fotografato ogni pochi giorni, ma non
sempre per intero: il satellite passa su una striscia larga 290 km, e i
quadrati al bordo della striscia restano coperti a meta'. Il resto della scena
e' "nessun dato". La prima versione sceglieva la scena meno nuvolosa, senza
guardare quanto fosse piena: su Torino aveva preso una scena vuota al 37% e una
vuota al 58%, di giorni diversi. Risultato a schermo: grandi zone grigie (il
colore di riempimento) e pezzi di colore diverso accostati.

Ora, in ordine:

1. si scartano le scene piu' vuote di --max-nodata (1%) o piu' nuvolose di
   --max-cloud;
2. fra quelle rimaste si preferisce lo STESSO GIORNO per quadrati vicini: due
   quadrati fotografati nello stesso passaggio hanno la stessa luce, e il
   confine fra i due non si vede. Si sceglie il giorno che copre piu' quadrati,
   poi il successivo per quelli rimasti, e cosi' via;
3. un quadrato senza nessuna scena piena prende la migliore disponibile piu'
   fino a due scene di RIEMPIMENTO di altri giorni, che vanno SOTTO di lei nel
   mosaico e ne coprono i buchi.

L'ordine conta: nel mosaico virtuale di GDAL chi viene dopo copre chi viene
prima. Per questo fetch-imagery scrive `ordine_scene.txt` (riempimenti prima,
principali dopo), da passare a build-imagery con `-i @cartella/ordine_scene.txt`.

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
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass, field

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
    "italia": (6.6, 35.4, 18.6, 47.1),
}

#: Contorni APPROSSIMATI (lon, lat) per le aree grandi: servono a non scaricare
#: centinaia di quadrati di solo mare. Sono volutamente grossolani e larghi:
#: un quadrato si tiene se un punto qualunque della griglia di campionamento
#: cade dentro, o a meno di COAST_MARGIN_DEG dal bordo.
AREA_POLYGONS = {
    "italia": [
        # penisola e arco alpino, in senso orario da Ventimiglia
        [(7.5, 43.8), (6.6, 45.1), (7.0, 45.9), (8.4, 46.5), (9.3, 46.5), (10.5, 46.9),
         (12.2, 47.1), (13.7, 46.5), (13.9, 45.6), (12.4, 45.4), (12.3, 44.6),
         (13.6, 43.5), (14.7, 42.1), (16.2, 41.9), (18.5, 40.2), (18.3, 39.8),
         (17.0, 40.4), (16.5, 39.6), (17.2, 39.0), (16.1, 37.9), (15.6, 38.0),
         (15.7, 39.8), (14.9, 40.3), (14.0, 40.8), (12.9, 41.3), (11.1, 42.4),
         (10.5, 43.0), (10.2, 43.9), (8.8, 44.4)],
        # Sicilia
        [(12.4, 37.8), (13.3, 38.2), (15.6, 38.3), (15.1, 37.0), (15.1, 36.6),
         (14.3, 37.0), (12.7, 37.6)],
        # Sardegna
        [(8.2, 40.9), (9.2, 41.3), (9.8, 40.9), (9.6, 39.1), (9.0, 39.0),
         (8.4, 38.9), (8.4, 39.9)],
        # isole piccole lontane dalla costa: triangoli attorno all'isola
        [(11.90, 36.72), (12.07, 36.78), (11.95, 36.85)],        # Pantelleria
        [(12.53, 35.48), (12.64, 35.51), (12.55, 35.53)],        # Lampedusa
    ],
}

#: Quanto lontano dal contorno approssimato si tiene ancora un punto, in gradi.
COAST_MARGIN_DEG = 0.15

#: Richieste HTTP in parallelo per elenchi e metadati: sono piccole, e per
#: l'Italia intera sono migliaia. In serie ci vorrebbe mezz'ora.
METADATA_WORKERS = 16

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
    #: Percentuale della scena senza dati (bordo della striscia del satellite).
    nodata: float | None = None

    @property
    def date(self) -> str:
        """Il giorno di acquisizione, AAAAMMGG, dal nome della scena."""
        return self.name.split("_")[2]

    @property
    def square_key(self) -> str:
        return f"{self.zone}{self.band}{self.square}"

    @property
    def directory_url(self) -> str:
        return (f"{BUCKET}/{PREFIX}/{self.zone}/{self.band}/{self.square}"
                f"/{self.year}/{self.month}/{self.name}")

    @property
    def visual_url(self) -> str:
        return f"{self.directory_url}/TCI.tif"

    @property
    def scl_url(self) -> str:
        """La classificazione della scena (20 m): dice dove sono nuvole e ombre."""
        return f"{self.directory_url}/SCL.tif"

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
        scene.nodata = properties.get("s2:nodata_pixel_percentage")
    except Exception:
        # Una scena senza metadati leggibili non e' un errore fatale: resta
        # utilizzabile, semplicemente non si sa quanto sia nuvolosa e finira'
        # in fondo alla classifica.
        scene.cloud_cover = None
    return scene


# =============================================================================
#  Quali quadrati
# =============================================================================

def _point_in_polygon(lon: float, lat: float, polygon: list[tuple[float, float]]) -> bool:
    """Ray casting: quante volte una semiretta verso est attraversa il bordo."""
    inside = False
    count = len(polygon)
    for index in range(count):
        x1, y1 = polygon[index]
        x2, y2 = polygon[(index + 1) % count]
        if (y1 > lat) != (y2 > lat):
            crossing = x1 + (lat - y1) * (x2 - x1) / (y2 - y1)
            if lon < crossing:
                inside = not inside
    return inside


def _distance_to_polygon(lon: float, lat: float, polygon: list[tuple[float, float]]) -> float:
    """Distanza (in gradi, sul piano) dal bordo del poligono. Basta per un margine."""
    best = float("inf")
    count = len(polygon)
    for index in range(count):
        x1, y1 = polygon[index]
        x2, y2 = polygon[(index + 1) % count]
        dx, dy = x2 - x1, y2 - y1
        length2 = dx * dx + dy * dy
        t = 0.0 if length2 == 0 else max(0.0, min(1.0, ((lon - x1) * dx + (lat - y1) * dy) / length2))
        px, py = x1 + t * dx, y1 + t * dy
        best = min(best, ((lon - px) ** 2 + (lat - py) ** 2) ** 0.5)
    return best


def squares_for_polygons(polygons: list, step_deg: float = 0.1,
                         margin_deg: float = COAST_MARGIN_DEG) -> list[tuple[int, str, str]]:
    """I quadrati MGRS che toccano i poligoni (o gli stanno a meno di margin_deg)."""
    west = min(x for polygon in polygons for x, _ in polygon) - margin_deg
    east = max(x for polygon in polygons for x, _ in polygon) + margin_deg
    south = max(-80.0, min(y for polygon in polygons for _, y in polygon) - margin_deg)
    north = min(84.0, max(y for polygon in polygons for _, y in polygon) + margin_deg)

    found: list[tuple[int, str, str]] = []
    seen = set()
    latitude = south
    while latitude <= north + 1e-9:
        longitude = west
        while longitude <= east + 1e-9:
            near = any(_point_in_polygon(longitude, latitude, polygon)
                       or _distance_to_polygon(longitude, latitude, polygon) < margin_deg
                       for polygon in polygons)
            if near:
                key = mgrs.square(latitude, longitude)
                if key not in seen:
                    seen.add(key)
                    found.append(key)
            longitude += step_deg
        latitude += step_deg
    return found


# =============================================================================
#  Quali scene
# =============================================================================

def _parallel(function, items: list, workers: int = METADATA_WORKERS) -> list:
    """map() in parallelo, che conserva l'ordine. Per richieste HTTP piccole."""
    if not items:
        return []
    with ThreadPoolExecutor(max_workers=min(workers, len(items))) as pool:
        return list(pool.map(function, items))


def gather_candidates(squares: list[tuple[int, str, str]], *, year: int, months: list[int],
                      report=print) -> dict[str, list[Scene]]:
    """Tutte le scene dei quadrati nei mesi richiesti, con i metadati letti."""
    jobs = [(zone, band, square, month) for zone, band, square in squares for month in months]
    report(f"elenco le scene: {len(squares)} quadrati x {len(months)} mesi")
    listed = _parallel(lambda job: list_scenes(job[0], job[1], job[2], year, job[3]), jobs)

    scenes = [scene for batch in listed for scene in batch]
    report(f"leggo i metadati di {len(scenes)} scene")
    _parallel(load_metadata, scenes)

    by_square: dict[str, list[Scene]] = {f"{z}{b}{s}": [] for z, b, s in squares}
    for scene in scenes:
        by_square[scene.square_key].append(scene)
    return by_square


@dataclass
class Choice:
    """Una scena scelta, e il suo ruolo nel mosaico."""
    scene: Scene
    role: str                 # "principale" o "riempimento"
    reason: str = ""


def _usable(scene: Scene, max_cloud: float, max_nodata: float) -> bool:
    return (scene.cloud_cover is not None and scene.cloud_cover <= max_cloud
            and (scene.nodata or 0.0) <= max_nodata)


def _badness(scene: Scene) -> float:
    """Quanta parte della scena non serve: nuvole piu' vuoto. Piu' basso e' meglio."""
    cloud = scene.cloud_cover if scene.cloud_cover is not None else 100.0
    return cloud + (scene.nodata or 0.0)


def choose_scenes(candidates: dict[str, list[Scene]], *, max_cloud: float = 10.0,
                  max_nodata: float = 1.0, max_fillers: int = 2,
                  cloud_fillers: int = 0) -> list[Choice]:
    """
    Sceglie le scene del mosaico. Funzione PURA: nessuna rete, testabile.

    Ritorna le scelte nell'ordine in cui vanno messe nel mosaico: prima i
    riempimenti, poi le principali, cosi' che le principali stiano sopra.

    `cloud_fillers`: quante scene in PIU' prendere per ogni quadrato, da
    mettere sotto la principale. Servono quando le nuvole si tolgono con la
    maschera SCL (cloudmask.py): dove la principale aveva una nuvola resta un
    buco, e lo riempie la scena di sotto. Si scelgono le meno nuvolose, di
    giorni diversi, e a parita' le piu' vicine nel tempo alla principale
    (stessa stagione, stessa luce).
    """
    good = {key: [s for s in scenes if _usable(s, max_cloud, max_nodata)]
            for key, scenes in candidates.items()}
    unassigned = {key for key, scenes in candidates.items() if scenes}
    primary: dict[str, Choice] = {}

    # --- Stesso giorno per piu' quadrati possibile ------------------------
    while True:
        by_date: dict[str, dict[str, Scene]] = {}
        for key in unassigned:
            for scene in good[key]:
                current = by_date.setdefault(scene.date, {}).get(key)
                if current is None or _badness(scene) < _badness(current):
                    by_date[scene.date][key] = scene
        if not by_date:
            break

        def rank(date: str):
            chosen = by_date[date]
            mean_badness = sum(_badness(s) for s in chosen.values()) / len(chosen)
            return (len(chosen), -mean_badness)

        best_date = max(sorted(by_date), key=rank)
        for key, scene in by_date[best_date].items():
            primary[key] = Choice(scene, "principale",
                                  f"giorno {best_date}, condiviso da {len(by_date[best_date])} quadrati")
            unassigned.discard(key)

    # --- Quadrati senza una scena piena: la migliore piu' i riempimenti ----
    fillers: list[Choice] = []
    for key in sorted(unassigned):
        # Prima le scene SERENE, anche se vuote a meta': due meta' serene di
        # giorni diversi fanno un'immagine migliore di una scena piena con un
        # terzo di nuvole, che a schermo sono macchie bianche. Fra le serene,
        # la meno vuota fa da principale e le altre riempiono i suoi buchi.
        # Solo se non ce n'e' nessuna serena si ripiega sulla meno peggio.
        with_metadata = [s for s in candidates[key] if s.cloud_cover is not None]
        clear = [s for s in with_metadata if s.cloud_cover <= max_cloud]
        ranked = (sorted(clear, key=lambda s: ((s.nodata or 0.0), s.cloud_cover)) if clear
                  else sorted(with_metadata, key=_badness))
        if not ranked:
            continue
        best = ranked[0]
        primary[key] = Choice(best, "principale",
                              f"nessuna scena piena e serena: la migliore disponibile "
                              f"(nuvole {best.cloud_cover:.1f}%, vuoto {best.nodata or 0:.1f}%)")
        if (best.nodata or 0.0) > max_nodata:
            added = 0
            used_dates = {best.date}
            for other in ranked[1:]:
                if added >= max_fillers:
                    break
                if other.date in used_dates or (other.nodata or 0.0) >= 99.0:
                    continue
                fillers.append(Choice(other, "riempimento", f"copre i buchi di {best.name}"))
                used_dates.add(other.date)
                added += 1

    # --- Riempimenti per i buchi delle nuvole (maschera SCL) -----------------
    if cloud_fillers > 0:
        already = {choice.scene.name for choice in fillers} | {c.scene.name for c in primary.values()}
        for key in sorted(primary):
            main = primary[key].scene
            used_dates = {main.date} | {c.scene.date for c in fillers if c.scene.square_key == key}

            def days_apart(scene: Scene) -> int:
                from datetime import date
                a = date(int(main.date[:4]), int(main.date[4:6]), int(main.date[6:]))
                b = date(int(scene.date[:4]), int(scene.date[4:6]), int(scene.date[6:]))
                return abs((a - b).days)

            ranked = sorted((s for s in candidates[key]
                             if s.cloud_cover is not None and s.name not in already
                             and (s.nodata or 0.0) < 99.0),
                            key=lambda s: (_badness(s), days_apart(s)))
            extra: list[Choice] = []
            for other in ranked:
                if len(extra) >= cloud_fillers:
                    break
                if other.date in used_dates:
                    continue
                extra.append(Choice(other, "riempimento",
                                    f"copre le nuvole di {main.name} ({days_apart(other)} giorni di distanza)"))
                used_dates.add(other.date)
                already.add(other.name)
            # Dal peggiore al migliore: il migliore finisce subito sotto la principale.
            fillers.extend(reversed(extra))

    return fillers + [primary[key] for key in sorted(primary)]


def find_best_scenes(bbox: tuple[float, float, float, float], *,
                     year: int, months: list[int], max_cloud: float = 10.0,
                     max_nodata: float = 1.0, area: str | None = None,
                     cloud_fillers: int = 0, report=print) -> list[Choice]:
    """Quadrati dell'area, loro scene, scelta. Ritorna le scelte in ordine di mosaico."""
    if area in AREA_POLYGONS:
        squares = squares_for_polygons(AREA_POLYGONS[area])
        report(f"area '{area}': {len(squares)} quadrati MGRS sulla terraferma (mare escluso)")
    else:
        west, south, east, north = bbox
        squares = mgrs.squares_for_bbox(west, south, east, north)
        report(f"quadrati MGRS da coprire: {', '.join(f'{z}{b}{s}' for z, b, s in squares)}")

    candidates = gather_candidates(squares, year=year, months=months, report=report)

    for key, scenes in sorted(candidates.items()):
        if not scenes:
            report(f"  {key}: nessuna scena nei mesi richiesti (mare aperto?)")

    choices = choose_scenes(candidates, max_cloud=max_cloud, max_nodata=max_nodata,
                            cloud_fillers=cloud_fillers)
    for choice in choices:
        scene = choice.scene
        report(f"  {scene.square_key}: {choice.role:<12} {scene.name}  "
               f"nuvole {scene.cloud_cover if scene.cloud_cover is not None else -1:5.1f}%  "
               f"vuoto {scene.nodata or 0:5.1f}%  ({choice.reason})")
    return choices


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
                   timeout: float = 600.0, asset: str = "TCI") -> str:
    """
    Scarica un file di una scena (TCI di default, oppure SCL). Salta il file
    se c'e' gia' ed e' completo.
    """
    os.makedirs(directory, exist_ok=True)
    destination = os.path.join(directory, f"{scene.name}_{asset}.tif")
    url = scene.visual_url if asset == "TCI" else f"{scene.directory_url}/{asset}.tif"

    expected = remote_size(url)
    if os.path.exists(destination):
        if expected is None or os.path.getsize(destination) == expected:
            report(f"  {scene.name} {asset}: gia' presente")
            return destination
        report(f"  {scene.name} {asset}: incompleto, riscarico")

    megabytes = (expected or 0) / (1024 * 1024)
    report(f"  {scene.name} {asset}: scarico {megabytes:.0f} MB")

    temporary = destination + ".part"
    request = urllib.request.Request(url,
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


ORDER_FILENAME = "ordine_scene.txt"


def write_order_file(directory: str, paths: list[str]) -> str:
    """Scrive l'ordine del mosaico: una riga per file, dal basso verso l'alto."""
    os.makedirs(directory, exist_ok=True)
    path = os.path.join(directory, ORDER_FILENAME)
    with open(path, "w", encoding="utf-8") as handle:
        handle.write("# Ordine del mosaico per build-imagery: chi viene dopo copre chi viene prima.\n")
        handle.write("# Uso: python run.py build-imagery -i @" + path + " -o dataset/ortofoto\n")
        for entry in paths:
            # Percorsi relativi alla cartella del file: la si puo' spostare.
            relative = entry if entry.startswith("/vsi") else os.path.relpath(entry, directory)
            handle.write(relative + "\n")
    return path


def masked_path(directory: str, scene: Scene) -> str:
    """Dove sta il TCI senza nuvole di una scena."""
    return os.path.join(directory, f"{scene.name}_TCI_senza_nuvole.tif")


def fetch(bbox: tuple[float, float, float, float], directory: str, *,
          year: int, months: list[int], max_cloud: float = 10.0,
          max_nodata: float = 1.0, area: str | None = None,
          stream: bool = False, dry_run: bool = False, download_workers: int = 4,
          cloud_mask: bool = True, cloud_fillers: int = 2,
          keep_originals: bool = False, report=print) -> list[str]:
    """
    Trova e procura le scene. Ritorna i percorsi da passare a build-imagery, in
    ordine di mosaico, e scrive lo stesso elenco in ordine_scene.txt.

    Con `cloud_mask` (default) scarica anche la classificazione SCL di ogni
    scena, toglie nuvole, ombre e cirri (cloudmask.py) e prende
    `cloud_fillers` scene in piu' per quadrato per riempire i buchi.

    Con `stream` non scarica niente: restituisce percorsi /vsicurl/ che GDAL
    legge direttamente dalla rete (e allora niente maschera: servirebbe
    comunque scrivere un file locale). Con `dry_run` dice solo cosa
    scaricherebbe e quanto pesa.
    """
    if stream and cloud_mask:
        report("NOTA: con --stream la maschera delle nuvole non si applica "
               "(servirebbe scrivere comunque un file locale).")
        cloud_mask = False

    choices = find_best_scenes(bbox, year=year, months=months, max_cloud=max_cloud,
                               max_nodata=max_nodata, area=area,
                               cloud_fillers=(cloud_fillers if cloud_mask else 0),
                               report=report)
    if not choices:
        raise RuntimeError(
            "nessuna scena trovata. Prova ad allargare i mesi, ad alzare "
            "--max-cloud, o a controllare che l'area sia sulla terraferma.")

    scenes = [choice.scene for choice in choices]

    if dry_run:
        sizes = _parallel(lambda scene: remote_size(scene.visual_url), scenes)
        known = [size for size in sizes if size]
        total = sum(known) + (len(sizes) - len(known)) * 230 * 1024 * 1024
        report("")
        report(f"Scaricherei {len(scenes)} scene, circa {total / 1024 ** 3:.1f} GB "
               f"({len(sizes) - len(known)} dimensioni stimate a 230 MB).")
        if cloud_mask:
            report("Con la maschera delle nuvole: piu' ~2 MB di SCL a scena, e i file senza "
                   "nuvole occupano circa quanto gli originali"
                   + ("" if keep_originals else " (gli originali si cancellano dopo)") + ".")
        report("Nessun file scaricato: togli --dry-run per procedere.")
        return []

    if stream:
        report("")
        report("Modalita' streaming: niente da scaricare, GDAL leggera' dalla rete.")
        paths = [scene.vsicurl_path for scene in scenes]
        report(f"Ordine del mosaico in {write_order_file(directory, paths)}")
        return paths

    report("")
    report(f"Scarico {len(scenes)} scene in {directory} ({download_workers} alla volta)")

    def procure(scene: Scene) -> str:
        if not cloud_mask:
            return download_scene(scene, directory, report=report)

        # Gia' fatta in un giro precedente: non si riscarica niente.
        result = masked_path(directory, scene)
        if os.path.exists(result):
            report(f"  {scene.name}: senza nuvole, gia' presente")
            return result

        tci = download_scene(scene, directory, report=report)
        scl = download_scene(scene, directory, report=report, asset="SCL")
        from . import cloudmask
        cloudmask.mask_scene(tci, scl, result, report=report)
        if not keep_originals:
            os.remove(tci)
        return result

    paths = _parallel(procure, scenes, workers=max(1, download_workers))
    report(f"Ordine del mosaico in {write_order_file(directory, paths)}")
    return paths

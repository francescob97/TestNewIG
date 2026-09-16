"""
Scaricamento automatico di DEM pubblici.

--------------------------------------------------------------------------
PERCHE' ESISTE, VISTO CHE IL TARGET E' TINITALY
--------------------------------------------------------------------------
TINITALY si scarica a mano: 193 tile da cliccare una per una sul sito INGV.
Non c'e' un endpoint pubblico documentato da cui prenderle in blocco, quindi
non c'e' niente da automatizzare in modo affidabile.

Il Copernicus DEM invece sta su un bucket S3 pubblico, senza credenziali, con
nomi di file deterministici. Questo modulo lo scarica per un'area data. Serve a
due cose concrete:

 1. **sbloccare subito**: si puo' far girare la pipeline completa su dati veri
    oggi, senza aspettare il download manuale di TINITALY;
 2. **fare da copertura di riserva**: TINITALY copre l'Italia e basta. Se un
    giorno il motore dovesse mostrare anche il resto d'Europa, il Copernicus
    copre il globo con lo stesso identico percorso di pipeline.

Confronto onesto fra i due:

  TINITALY 1.1      10 m, solo Italia, CC BY 4.0, download manuale
  Copernicus GLO-30 30 m, globale, download automatico

Tre volte meno dettaglio, ma e' un DSM recente e coerente, ed e' piu' che
sufficiente per verificare tutto quello che c'e' da verificare fino alla Fase 5.
Quando TINITALY arriva, si rilancia la pipeline sui nuovi file: non cambia una
riga di codice.

--------------------------------------------------------------------------
DATUM VERTICALE
--------------------------------------------------------------------------
Le quote del Copernicus DEM sono riferite a EGM2008, esattamente come si
presume per TINITALY. Il default `--vertical-crs EPSG:3855` vale per entrambi.

--------------------------------------------------------------------------
NIENTE DIPENDENZE NUOVE
--------------------------------------------------------------------------
Si usa urllib della standard library invece di `requests`: rispetta le variabili
di proxy, non aggiunge niente da installare, e per scaricare file da un bucket
S3 pubblico non serve altro.
"""

from __future__ import annotations

import math
import os
import time
import urllib.error
import urllib.request
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass

#: (bucket, prefisso dei nomi) per risoluzione nominale in metri.
COPERNICUS_DATASETS = {
    30: ("copernicus-dem-30m", "Copernicus_DSM_COG_10"),
    90: ("copernicus-dem-90m", "Copernicus_DSM_COG_30"),
}

#: Aree predefinite: (ovest, sud, est, nord) in gradi.
NAMED_AREAS = {
    "test":    (12.0, 41.0, 13.0, 42.0),     # una tile sola, su Roma
    "roma":    (12.2, 41.6, 12.9, 42.1),
    "alpi":    (6.5, 45.0, 11.5, 47.1),
    "sicilia": (12.3, 36.6, 15.7, 38.3),
    "italia":  (6.6, 35.4, 18.6, 47.1),
}

ATTRIBUTION = (
    "Copernicus DEM - Global and European Digital Elevation Model.\n"
    "Prodotto da ESA nell'ambito del programma Copernicus dell'Unione Europea.\n"
    "(c) DLR e.V. 2010-2014 e (c) Airbus Defence and Space GmbH 2014-2018,\n"
    "distribuito sotto Copernicus da Unione Europea ed ESA.\n"
    "Quote riferite a EGM2008. Uso libero con attribuzione.\n")


@dataclass
class TileRef:
    latitude: int         # bordo SUD della tile, in gradi interi
    longitude: int        # bordo OVEST della tile, in gradi interi
    name: str
    url: str
    filename: str


def tile_reference(resolution: int, latitude: int, longitude: int) -> TileRef:
    """
    Nome e URL della tile di 1 grado il cui angolo sud-ovest e' (lat, lon).

    La convenzione di nome e' quella verificata sul bucket:
        Copernicus_DSM_COG_10_N41_00_E012_00_DEM
    latitudine a 2 cifre, longitudine a 3, prefisso N/S ed E/W. La tile prende
    il nome dal proprio bordo sud e dal proprio bordo ovest.
    """
    bucket, prefix = COPERNICUS_DATASETS[resolution]

    latitude_part = f"{'N' if latitude >= 0 else 'S'}{abs(latitude):02d}_00"
    longitude_part = f"{'E' if longitude >= 0 else 'W'}{abs(longitude):03d}_00"
    name = f"{prefix}_{latitude_part}_{longitude_part}_DEM"

    return TileRef(
        latitude=latitude, longitude=longitude, name=name,
        url=f"https://{bucket}.s3.amazonaws.com/{name}/{name}.tif",
        filename=f"{name}.tif")


def tiles_for_bbox(resolution: int,
                   bbox: tuple[float, float, float, float]) -> list[TileRef]:
    """Tutte le tile di 1 grado che intersecano il bbox."""
    west, south, east, north = bbox
    references = []
    for latitude in range(math.floor(south), math.ceil(north)):
        for longitude in range(math.floor(west), math.ceil(east)):
            references.append(tile_reference(resolution, latitude, longitude))
    return references


def _remote_size(url: str, timeout: float = 30.0) -> int | None:
    """
    Dimensione remota, o None se la tile non esiste.

    Un 404 NON e' un errore: il Copernicus DEM pubblica solo le tile che
    contengono terraferma. Sul bbox italiano piu' di meta' delle tile di un
    grado sono mare aperto e semplicemente non esistono.
    """
    request = urllib.request.Request(url, method="HEAD")
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            length = response.headers.get("Content-Length")
            return int(length) if length else -1
    except urllib.error.HTTPError as error:
        if error.code == 404:
            return None
        raise


def _download_one(reference: TileRef, directory: str, timeout: float
                  ) -> tuple[str, int]:
    """
    Ritorna (esito, byte) con esito in {"scaricata", "gia presente", "assente"}.
    """
    path = os.path.join(directory, reference.filename)

    size = _remote_size(reference.url, timeout)
    if size is None:
        return ("assente", 0)

    # Riavviabilita': una tile gia' presente e della dimensione giusta non si
    # riscarica. La dimensione basta come controllo perche' la scrittura e'
    # atomica (vedi sotto): un file della dimensione attesa e' completo.
    if os.path.exists(path) and (size < 0 or os.path.getsize(path) == size):
        return ("gia presente", os.path.getsize(path))

    temporary = f"{path}.part"
    with urllib.request.urlopen(reference.url, timeout=timeout) as response, \
            open(temporary, "wb") as handle:
        written = 0
        while True:
            chunk = response.read(1 << 20)
            if not chunk:
                break
            handle.write(chunk)
            written += len(chunk)
        handle.flush()
        os.fsync(handle.fileno())

    # Scrittura atomica: un'interruzione lascia un .part, mai un .tif troncato
    # che al rilancio verrebbe preso per buono.
    os.replace(temporary, path)
    return ("scaricata", written)


def fetch_copernicus(bbox: tuple[float, float, float, float], directory: str,
                     resolution: int = 30, jobs: int = 6, timeout: float = 120.0,
                     dry_run: bool = False, log=print) -> dict:
    """Scarica le tile Copernicus DEM che coprono il bbox."""
    if resolution not in COPERNICUS_DATASETS:
        raise ValueError(f"risoluzione {resolution} non disponibile; "
                         f"scegli fra {sorted(COPERNICUS_DATASETS)}")

    references = tiles_for_bbox(resolution, bbox)
    os.makedirs(directory, exist_ok=True)

    log(f"Area      : {bbox[0]:.2f} {bbox[1]:.2f} {bbox[2]:.2f} {bbox[3]:.2f}")
    log(f"Dataset   : Copernicus DEM GLO-{resolution} (quote EGM2008, EPSG:4326)")
    log(f"Tile di 1 grado da controllare: {len(references)}")
    log("")

    if dry_run:
        total = 0
        available = 0
        for reference in references:
            size = _remote_size(reference.url, timeout)
            if size is None:
                log(f"  -    {reference.name}  (mare, non esiste)")
            else:
                available += 1
                total += max(size, 0)
                log(f"  OK   {reference.name}  {size / 1e6:.1f} MB")
        log("")
        log(f"{available} tile disponibili, {total / 1e6:.0f} MB da scaricare.")
        return {"tiles": available, "bytes": total, "dryRun": True}

    started = time.time()
    counters = {"scaricata": 0, "gia presente": 0, "assente": 0}
    total_bytes = 0
    failures: list[tuple[str, str]] = []

    def worker(reference: TileRef):
        try:
            return reference, _download_one(reference, directory, timeout), None
        except Exception as error:                        # noqa: BLE001
            return reference, ("errore", 0), error

    with ThreadPoolExecutor(max_workers=jobs) as pool:
        for index, (reference, (outcome, size), error) in enumerate(
                pool.map(worker, references), start=1):
            if error is not None:
                failures.append((reference.name, f"{type(error).__name__}: {error}"))
                log(f"  [{index}/{len(references)}] ERRORE {reference.name}: {error}")
                continue

            counters[outcome] = counters.get(outcome, 0) + 1
            total_bytes += size
            if outcome != "assente":
                log(f"  [{index}/{len(references)}] {outcome:<13} {reference.name}"
                    f"  {size / 1e6:.1f} MB")

    elapsed = time.time() - started
    log("")
    log(f"Scaricate {counters['scaricata']}, gia' presenti {counters['gia presente']}, "
        f"non esistenti (mare) {counters['assente']}.")
    log(f"{total_bytes / 1e6:.0f} MB in {elapsed:.0f}s.")

    if failures:
        log("")
        log(f"{len(failures)} tile non scaricate:")
        for name, reason in failures[:10]:
            log(f"  {name}: {reason}")
        log("Rilancia lo stesso comando: le tile gia' presenti non si riscaricano.")

    attribution_path = os.path.join(directory, "ATTRIBUZIONE.txt")
    if not os.path.exists(attribution_path):
        with open(attribution_path, "w", encoding="utf-8") as handle:
            handle.write(ATTRIBUTION)

    return {"tiles": counters["scaricata"] + counters["gia presente"],
            "bytes": total_bytes, "failures": len(failures),
            "directory": directory}

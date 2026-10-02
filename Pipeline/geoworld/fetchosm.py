"""
Scarica gli estratti OpenStreetMap da Geofabrik.

Geofabrik ripubblica ogni giorno l'intero database OSM tagliato per nazione e
per regione, in formato .osm.pbf: e' il modo standard di procurarsi OSM per
un'area grande. Nessun account, nessuna chiave. I dati sono (c) i contributori
di OpenStreetMap, sotto licenza ODbL: chi li usa deve citarli (il comando
scrive ATTRIBUZIONE.txt accanto al file).

Per l'Italia ci sono il file intero (~2 GB) e cinque macro-regioni: per
provare conviene la regione, che si scarica in pochi minuti e si taglia in
pochi minuti. Torino sta in nord-ovest.
"""

from __future__ import annotations

import hashlib
import os
import time
import urllib.request

BASE_URL = "https://download.geofabrik.de/europe"

#: Area -> (file su Geofabrik, bbox suggerito per tagliare, descrizione).
#: Il bbox e' None quando si vuole tutto il file.
AREAS = {
    "italia":     ("italy-latest.osm.pbf", None, "Italia intera (~2 GB)"),
    "nord-ovest": ("italy/nord-ovest-latest.osm.pbf", None,
                   "Piemonte, Valle d'Aosta, Liguria, Lombardia"),
    "nord-est":   ("italy/nord-est-latest.osm.pbf", None,
                   "Trentino-Alto Adige, Veneto, Friuli-Venezia Giulia, Emilia-Romagna"),
    "centro":     ("italy/centro-latest.osm.pbf", None, "Toscana, Umbria, Marche, Lazio"),
    "sud":        ("italy/sud-latest.osm.pbf", None,
                   "Abruzzo, Molise, Campania, Puglia, Basilicata, Calabria"),
    "isole":      ("italy/isole-latest.osm.pbf", None, "Sicilia e Sardegna"),
    # Torino non ha un file suo: si scarica il nord-ovest e si taglia l'area
    # delle prove (la stessa di fetch-imagery --area torino).
    "torino":     ("italy/nord-ovest-latest.osm.pbf", (7.55, 45.00, 7.80, 45.15),
                   "nord-ovest, da tagliare sull'area di Torino"),
}

ATTRIBUTION = (
    "Dati (c) contributori di OpenStreetMap, https://www.openstreetmap.org/copyright\n"
    "Disponibili sotto la Open Database License (ODbL) 1.0.\n"
    "Estratto scaricato da Geofabrik, https://download.geofabrik.de\n")


def url_for(area: str) -> str:
    return f"{BASE_URL}/{AREAS[area][0]}"


def _md5_of(path: str) -> str:
    digest = hashlib.md5()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 22), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _remote_md5(url: str, timeout: float) -> str | None:
    """Geofabrik pubblica accanto a ogni file un .md5. Se manca, si fa a meno."""
    try:
        with urllib.request.urlopen(url + ".md5", timeout=timeout) as response:
            return response.read().decode("ascii", "replace").split()[0].strip().lower()
    except Exception:                                   # noqa: BLE001
        return None


def download(url: str, path: str, timeout: float = 120.0, report=print) -> int:
    """
    Scarica `url` in `path`, riprendendo un download interrotto.

    Il file parziale si chiama .part e si rinomina solo a download completo e
    verificato: un file .osm.pbf presente e' sempre un file intero.
    """
    temporary = path + ".part"
    have = os.path.getsize(temporary) if os.path.exists(temporary) else 0

    request = urllib.request.Request(url)
    if have:
        request.add_header("Range", f"bytes={have}-")

    started = time.time()
    last = started
    with urllib.request.urlopen(request, timeout=timeout) as response:
        # 206 = il server ha accettato di riprendere; 200 = ricomincia da capo.
        resumed = have and response.status == 206
        mode = "ab" if resumed else "wb"
        if not resumed:
            have = 0
        total = response.headers.get("Content-Length")
        total = (int(total) + have) if total else None
        if resumed:
            report(f"  riprendo da {have / 1e6:.0f} MB")

        with open(temporary, mode) as handle:
            while True:
                chunk = response.read(1 << 20)
                if not chunk:
                    break
                handle.write(chunk)
                have += len(chunk)
                now = time.time()
                if now - last >= 5.0:
                    last = now
                    speed = have / max(1e-3, now - started) / 1e6
                    if total:
                        report(f"  {have / 1e6:7.0f} / {total / 1e6:.0f} MB  ({speed:.1f} MB/s)")
                    else:
                        report(f"  {have / 1e6:7.0f} MB  ({speed:.1f} MB/s)")

    expected = _remote_md5(url, timeout)
    if expected:
        actual = _md5_of(temporary)
        if actual != expected:
            os.remove(temporary)
            raise RuntimeError(f"il file scaricato e' corrotto (md5 {actual}, atteso {expected}): "
                               "rilancia il comando")
        report("  controllo md5: ok")
    os.replace(temporary, path)
    return have


def fetch(area: str, directory: str, dry_run: bool = False, timeout: float = 120.0,
          report=print) -> str:
    """Scarica l'estratto dell'area. Ritorna il percorso del file."""
    if area not in AREAS:
        raise ValueError(f"area '{area}' sconosciuta; scegli fra {', '.join(AREAS)}")
    filename, bbox, description = AREAS[area]
    url = url_for(area)
    path = os.path.join(directory, os.path.basename(filename))

    report(f"Area     : {area} ({description})")
    report(f"Sorgente : {url}")
    if bbox:
        report(f"Taglio   : ovest {bbox[0]} sud {bbox[1]} est {bbox[2]} nord {bbox[3]}")

    if dry_run:
        request = urllib.request.Request(url, method="HEAD")
        with urllib.request.urlopen(request, timeout=timeout) as response:
            size = response.headers.get("Content-Length")
        report(f"Da scaricare: {int(size) / 1e6:.0f} MB" if size else "Dimensione non dichiarata")
        return path

    os.makedirs(directory, exist_ok=True)
    if os.path.exists(path):
        report(f"Gia' presente: {path} ({os.path.getsize(path) / 1e6:.0f} MB). "
               "Per aggiornarlo, cancellalo e rilancia.")
    else:
        started = time.time()
        size = download(url, path, timeout=timeout, report=report)
        report(f"Scaricati {size / 1e6:.0f} MB in {time.time() - started:.0f} s: {path}")

    attribution = os.path.join(directory, "ATTRIBUZIONE.txt")
    if not os.path.exists(attribution):
        with open(attribution, "w", encoding="utf-8") as handle:
            handle.write(ATTRIBUTION)
    return path

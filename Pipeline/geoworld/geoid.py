"""
Conversione quote ortometriche -> ellissoidiche.

--------------------------------------------------------------------------
IL PROBLEMA, IN UNA FRASE
--------------------------------------------------------------------------
TINITALY da' quote ORTOMETRICHE: altezze sopra il geoide, cioe' sopra il
livello medio del mare. Il nostro motore lavora su un ELLISSOIDE (WGS84) e ha
bisogno di quote ellissoidiche. Le due differiscono dell'ondulazione del geoide

    N = h_ellissoidica - H_ortometrica

che in Italia vale circa +40 / +52 metri. Ignorarla significa avere tutto il
terreno una cinquantina di metri sotto dove dovrebbe stare.

--------------------------------------------------------------------------
PERCHE' NON LASCIO FARE TUTTO A GDALWARP
--------------------------------------------------------------------------
La via "elegante" sarebbe warpare da EPSG:32632+3855 a EPSG:4979 e lasciare che
PROJ applichi la griglia EGM2008 insieme alla riproiezione orizzontale. Non la
uso, per una ragione misurata su questa macchina:

    Transformer.from_crs("EPSG:32632+3855", "EPSG:4979", allow_ballpark=True)
    -> lon=12.481689 lat=41.859138 h=0.000

Senza la griglia installata, PROJ ricade su una "ballpark vertical
transformation" che semplicemente NON APPLICA lo scostamento verticale. Nessuna
eccezione, nessun warning, nessun codice di errore: restituisce quote
identiche a quelle di partenza. E il ballpark e' il comportamento di DEFAULT di
gdalwarp. Il risultato sarebbe un dataset sbagliato di 48 metri su tutta
l'Italia, plausibile a ogni ispezione superficiale.

Quindi separo i due passi:
  1. riproiezione ORIZZONTALE pura EPSG:32632 -> EPSG:4326 (nessuna griglia,
     nessuna ambiguita', nessun fallback possibile);
  2. somma esplicita di N(lon, lat) preso da una griglia che calcoliamo NOI qui,
     con allow_ballpark=False, e che finisce su disco come GeoTIFF ispezionabile.

Il passo 2 e' una riga di numpy e il suo input e' un file che puoi aprire in
QGIS: se e' pieno di zeri il problema si vede subito. Inoltre la stessa griglia
serve gia' per un altro scopo (riempire i post senza dato con la quota
ellissoidica del livello del mare), quindi non e' un artefatto in piu'.

--------------------------------------------------------------------------
L'ERRORE CHE QUESTO INTRODUCE, E PERCHE' E' TRASCURABILE
--------------------------------------------------------------------------
Campionare N su una griglia e interpolare invece di valutarlo per pixel
introduce un errore. EGM2008 e' uno sviluppo in armoniche sferiche fino al
grado 2190, cioe' ha risoluzione ~9 km: e' un campo liscio. Con passo di 0.01
gradi (~1.1 km) l'errore di interpolazione bilineare e' di frazioni di
millimetro. Non e' un'assunzione: `measure_interpolation_error()` lo MISURA
confrontando i valori interpolati con quelli esatti su punti casuali, e la
pipeline fallisce se supera la soglia.
"""

from __future__ import annotations

import math
from dataclasses import dataclass

import numpy as np

#: Datum verticali supportati. Il primo e' il default richiesto.
VERTICAL_CRS_EGM2008 = "EPSG:3855"
VERTICAL_CRS_EGM96 = "EPSG:5773"

#: Intervallo plausibile per l'ondulazione del geoide in Italia. Serve come
#: controllo di sanita': se il valore calcolato cade fuori, qualcosa non va
#: nella configurazione di PROJ e vale la pena fermarsi invece di produrre
#: 16 GB di dati sbagliati.
ITALY_UNDULATION_RANGE_M = (30.0, 65.0)


@dataclass
class VerticalTransformInfo:
    vertical_crs: str
    description: str
    sample_points: list[tuple[str, float, float, float]]   # (nome, lon, lat, N)

    def as_dict(self) -> dict:
        return {
            "verticalCrs": self.vertical_crs,
            "projPipeline": self.description,
            "samples": [{"name": n, "lon": lo, "lat": la, "undulationM": round(u, 4)}
                        for n, lo, la, u in self.sample_points],
        }


def _make_transformer(vertical_crs: str):
    """
    Trasformatore da (lon, lat, quota ortometrica) a (lon, lat, quota ellissoidica).

    allow_ballpark=False e' il punto centrale di questo modulo: senza, PROJ
    accetta silenziosamente di non fare il lavoro.
    """
    from pyproj import CRS, Transformer

    epsg_code = vertical_crs.split(":")[1]
    source = CRS.from_user_input(f"EPSG:4326+{epsg_code}")
    target = CRS.from_epsg(4979)          # WGS84 3D, quote ellissoidiche

    return Transformer.from_crs(source, target, always_xy=True, allow_ballpark=False)


def check_vertical_transform(vertical_crs: str = VERTICAL_CRS_EGM2008
                             ) -> VerticalTransformInfo:
    """
    Preflight: verifica che la griglia geoidica sia davvero disponibile e che
    produca valori sensati, PRIMA di elaborare gigabyte di dati.

    Solleva RuntimeError con istruzioni concrete se non lo e'.
    """
    probes = [("Colosseo", 12.492231, 41.890210),
              ("Duomo di Milano", 9.191900, 45.464200),
              ("Monte Bianco", 6.865200, 45.832600),
              ("Etna", 14.993400, 37.751000),
              ("Cagliari", 9.114200, 39.223800)]

    try:
        transformer = _make_transformer(vertical_crs)
    except Exception as error:
        raise RuntimeError(_missing_grid_message(vertical_crs, error)) from error

    samples = []
    for name, lon, lat in probes:
        _, _, undulation = transformer.transform(lon, lat, 0.0)

        if not math.isfinite(undulation):
            raise RuntimeError(_missing_grid_message(
                vertical_crs,
                f"la trasformazione ha restituito {undulation} per {name}: PROJ ha "
                "selezionato l'operazione corretta ma non riesce ad accedere alla griglia"))

        low, high = ITALY_UNDULATION_RANGE_M
        if not (low <= undulation <= high):
            raise RuntimeError(
                f"L'ondulazione del geoide calcolata per {name} ({lon}, {lat}) e' "
                f"{undulation:.3f} m, fuori dall'intervallo plausibile per l'Italia "
                f"[{low}, {high}] m.\n"
                f"Datum verticale richiesto: {vertical_crs}.\n"
                "Un valore di 0.000 significa quasi sempre che PROJ ha applicato una "
                "'ballpark vertical transformation', cioe' non ha applicato nulla.")

        samples.append((name, lon, lat, undulation))

    return VerticalTransformInfo(vertical_crs=vertical_crs,
                                 description=transformer.description,
                                 sample_points=samples)


def _missing_grid_message(vertical_crs: str, error: object) -> str:
    name = {VERTICAL_CRS_EGM2008: "EGM2008", VERTICAL_CRS_EGM96: "EGM96"}.get(vertical_crs, vertical_crs)
    return (
        f"Impossibile costruire la trasformazione verticale verso {vertical_crs} ({name}).\n"
        f"Dettaglio: {error}\n\n"
        "La griglia geoidica non e' installata. Rimedi, in ordine di preferenza:\n"
        "  1. scaricare solo l'area che serve:\n"
        "       projsync --bbox 6,35,19,48 --file egm08\n"
        "  2. installare il pacchetto completo delle griglie PROJ:\n"
        "       conda install -c conda-forge proj-data\n"
        "       (oppure, su Debian/Ubuntu: apt install proj-data)\n"
        "  3. abilitare il download automatico da cdn.proj.org:\n"
        "       export PROJ_NETWORK=ON\n"
        "     (richiede accesso a cdn.proj.org; se sei dietro un proxy che lo\n"
        "      blocca questa via non funziona)\n\n"
        f"In alternativa si puo' usare EGM96 con --vertical-crs {VERTICAL_CRS_EGM96}: la\n"
        "griglia egm96_15.gtx e' inclusa in quasi tutte le distribuzioni di PROJ.\n"
        "In Italia EGM96 ed EGM2008 differiscono di qualche decimetro: accettabile\n"
        "per una prova, non per il dataset definitivo.")


def build_undulation_grid(bbox: tuple[float, float, float, float],
                          spacing_deg: float,
                          vertical_crs: str) -> tuple[np.ndarray, tuple[float, float, float]]:
    """
    Calcola N su una griglia regolare che copre il bbox con un margine.

    Ritorna (grid, (origin_lon, origin_lat, spacing)) con grid[0] = riga piu' a NORD.
    """
    west, south, east, north = bbox

    # Un margine di due celle garantisce che l'interpolazione bilineare sui
    # bordi del dataset abbia sempre quattro vicini validi, senza casi speciali.
    margin = spacing_deg * 2.0
    origin_lon = math.floor((west - margin) / spacing_deg) * spacing_deg
    origin_lat = math.ceil((north + margin) / spacing_deg) * spacing_deg

    columns = int(math.ceil((east + margin - origin_lon) / spacing_deg)) + 1
    rows = int(math.ceil((origin_lat - (south - margin)) / spacing_deg)) + 1

    lons = origin_lon + np.arange(columns, dtype=np.float64) * spacing_deg
    lats = origin_lat - np.arange(rows, dtype=np.float64) * spacing_deg
    lon_mesh, lat_mesh = np.meshgrid(lons, lats)

    transformer = _make_transformer(vertical_crs)
    _, _, undulation = transformer.transform(
        lon_mesh.ravel(), lat_mesh.ravel(), np.zeros(lon_mesh.size))

    grid = np.asarray(undulation, dtype=np.float32).reshape(lat_mesh.shape)

    if not np.isfinite(grid).all():
        bad = int((~np.isfinite(grid)).sum())
        raise RuntimeError(
            f"La griglia di ondulazione contiene {bad} valori non finiti su {grid.size}. "
            "La griglia geoidica non copre l'intera area richiesta.")

    return grid, (origin_lon, origin_lat, spacing_deg)


def sample_undulation(grid: np.ndarray, geotransform: tuple[float, float, float],
                      lon: np.ndarray, lat: np.ndarray) -> np.ndarray:
    """
    Interpolazione bilineare di N sui punti dati.

    `lon` e `lat` possono essere array di qualunque forma compatibile; il
    risultato ha la forma del broadcast.
    """
    origin_lon, origin_lat, spacing = geotransform
    rows, columns = grid.shape

    # Coordinate continue nella griglia.
    fx = (np.asarray(lon, dtype=np.float64) - origin_lon) / spacing
    fy = (origin_lat - np.asarray(lat, dtype=np.float64)) / spacing

    # clip a rows-2 / columns-2 cosi' x0+1 e y0+1 restano sempre validi.
    x0 = np.clip(np.floor(fx).astype(np.intp), 0, columns - 2)
    y0 = np.clip(np.floor(fy).astype(np.intp), 0, rows - 2)
    tx = np.clip(fx - x0, 0.0, 1.0)
    ty = np.clip(fy - y0, 0.0, 1.0)

    top = grid[y0, x0] * (1.0 - tx) + grid[y0, x0 + 1] * tx
    bottom = grid[y0 + 1, x0] * (1.0 - tx) + grid[y0 + 1, x0 + 1] * tx
    return (top * (1.0 - ty) + bottom * ty).astype(np.float32)


def measure_interpolation_error(grid: np.ndarray, geotransform: tuple[float, float, float],
                                bbox: tuple[float, float, float, float],
                                vertical_crs: str, samples: int = 4000,
                                seed: int = 20260915) -> dict:
    """
    Misura l'errore introdotto dall'interpolazione, invece di darlo per buono.

    Confronta il valore interpolato dalla griglia con quello esatto calcolato
    da PROJ su punti casuali dentro il bbox.
    """
    west, south, east, north = bbox
    rng = np.random.default_rng(seed)
    lon = rng.uniform(west, east, samples)
    lat = rng.uniform(south, north, samples)

    interpolated = sample_undulation(grid, geotransform, lon, lat)

    transformer = _make_transformer(vertical_crs)
    _, _, exact = transformer.transform(lon, lat, np.zeros(samples))
    exact = np.asarray(exact, dtype=np.float64)

    error = np.abs(interpolated.astype(np.float64) - exact)
    return {
        "samples": samples,
        "maxErrorM": float(error.max()),
        "meanErrorM": float(error.mean()),
        "rmsErrorM": float(np.sqrt((error ** 2).mean())),
        "undulationMinM": float(exact.min()),
        "undulationMaxM": float(exact.max()),
    }

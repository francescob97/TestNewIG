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
L'ERRORE DI RICAMPIONAMENTO DIPENDE DALL'ALLINEAMENTO, NON DALLA FINEZZA
--------------------------------------------------------------------------
Campionare N su una nostra griglia e interpolare, invece di valutarlo per
pixel, introduce un errore. La cosa controintuitiva, misurata e non supposta,
e' che quell'errore NON dipende quasi per niente da quanto fitta e' la nostra
griglia: dipende da come i nostri nodi cadono rispetto a quelli della griglia
geoidica di PROJ.

Il motivo e' che PROJ interpola bilinearmente dentro la propria griglia, quindi
il campo che vediamo e' continuo ma con una "piega" su ogni linea di nodi.
Ricampionarlo e poi re-interpolarlo e' esatto finche' i nostri nodi cadono sui
suoi; appena cadono in mezzo, la piega viene tagliata e l'errore compare.

Misurato su EGM96 (griglia a 15' = 0.25 gradi), stesso bbox, stessi punti:

    passo 0.0104167 = 1/96   ->  0.25/passo = 24.0000 (intero)  ->   0.0036 mm
    passo 0.0104              ->  0.25/passo = 24.0385           ->   7.9409 mm
    passo 0.0110              ->  0.25/passo = 22.7273           ->  17.3452 mm

Diciassette micrometri di differenza nel passo cambiano l'errore di duemila
volte. Percio' il passo NON si sceglie "abbastanza fine": si sceglie come
sottomultiplo intero del passo nativo della griglia geoidica.

    EGM2008: 2.5 primi = 1/24 di grado
    EGM96  : 15  primi = 1/4  di grado

`measure_interpolation_error()` misura comunque il risultato, e
`build_aligned_undulation_grid()` infittisce da sola se non bastasse.
"""

from __future__ import annotations

import math
from dataclasses import dataclass

import numpy as np

#: Datum verticali supportati. Il primo e' il default richiesto.
VERTICAL_CRS_EGM2008 = "EPSG:3855"
VERTICAL_CRS_EGM96 = "EPSG:5773"

#: Passo NATIVO delle griglie geoidiche, in gradi. Campionare su un
#: sottomultiplo intero di questi valori fa cadere i nostri nodi su quelli
#: della griglia sorgente, ed e' cio' che rende il ricampionamento esatto.
GEOID_NATIVE_SPACING_DEG = {
    VERTICAL_CRS_EGM2008: 1.0 / 24.0,   # 2.5 primi d'arco
    VERTICAL_CRS_EGM96: 1.0 / 4.0,      # 15 primi d'arco
}

#: Sottodivisioni di default del passo nativo. 4 tiene la griglia piccola
#: (qualche MB sull'Italia) restando allineata.
DEFAULT_GEOID_SUBDIVISIONS = 4

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


def default_sampling_spacing(vertical_crs: str,
                             subdivisions: int = DEFAULT_GEOID_SUBDIVISIONS) -> float | None:
    """
    Passo di campionamento allineato alla griglia geoidica, o None se il datum
    non e' fra quelli noti (in quel caso decide chi chiama).
    """
    native = GEOID_NATIVE_SPACING_DEG.get(vertical_crs)
    return None if native is None else native / float(max(1, subdivisions))


def is_spacing_aligned(spacing_deg: float, vertical_crs: str,
                       tolerance: float = 1e-9) -> bool:
    """Il passo dato e' un sottomultiplo intero del passo nativo della griglia?"""
    native = GEOID_NATIVE_SPACING_DEG.get(vertical_crs)
    if native is None or spacing_deg <= 0.0:
        return False
    ratio = native / spacing_deg
    return abs(ratio - round(ratio)) < tolerance


def build_aligned_undulation_grid(bbox: tuple[float, float, float, float],
                                  vertical_crs: str, max_error_m: float,
                                  spacing_deg: float | None = None,
                                  max_attempts: int = 4, log=None):
    """
    Costruisce la griglia di N e GARANTISCE che l'errore di interpolazione stia
    sotto la soglia, infittendo da sola se serve.

    Parte dal passo allineato al passo nativo della griglia geoidica; se la
    misura non rispetta comunque la soglia, raddoppia le sottodivisioni (cosi'
    resta allineato) e riprova. La griglia costa pochi MB anche sull'Italia
    intera, quindi infittire e' praticamente gratis: fermare la pipeline
    quando basta raddoppiare un parametro sarebbe solo scortese.

    Ritorna (grid, geotransform, error_info, spacing_usato).
    """
    subdivisions = DEFAULT_GEOID_SUBDIVISIONS
    spacing = spacing_deg if spacing_deg is not None else default_sampling_spacing(
        vertical_crs, subdivisions)

    if spacing is None:
        # Datum verticale non fra quelli noti: non sappiamo il passo nativo,
        # quindi non possiamo allineare. Si parte da un valore ragionevole e ci
        # si affida al raffinamento automatico.
        spacing = 0.01

    if spacing_deg is not None and not is_spacing_aligned(spacing, vertical_crs) \
            and vertical_crs in GEOID_NATIVE_SPACING_DEG and log:
        native = GEOID_NATIVE_SPACING_DEG[vertical_crs]
        log(f"      NOTA: il passo {spacing:.7f} non e' un sottomultiplo intero di "
            f"{native:.7f} (passo nativo di {vertical_crs}).")
        log(f"            L'errore di interpolazione sara' molto piu' alto del "
            f"necessario. Ometti --geoid-spacing per lasciarlo scegliere.")

    last = None
    for attempt in range(max_attempts):
        grid, geotransform = build_undulation_grid(bbox, spacing, vertical_crs)
        error = measure_interpolation_error(grid, geotransform, bbox, vertical_crs)
        last = (grid, geotransform, error, spacing)

        if log:
            aligned = is_spacing_aligned(spacing, vertical_crs)
            log(f"      griglia N: {grid.shape[1]} x {grid.shape[0]} a passo "
                f"{spacing:.7f} gradi{' (allineata)' if aligned else ''}")
            log(f"      errore di interpolazione: max {error['maxErrorM'] * 1000:.4f} mm, "
                f"rms {error['rmsErrorM'] * 1000:.4f} mm")

        if error["maxErrorM"] <= max_error_m:
            return last

        subdivisions *= 2
        refined = default_sampling_spacing(vertical_crs, subdivisions)
        spacing = refined if refined is not None else spacing / 2.0

        if log and attempt + 1 < max_attempts:
            log(f"      sopra la soglia di {max_error_m * 1000:.1f} mm: "
                f"infittisco a {spacing:.7f} gradi e rimisuro.")

    return last

"""
Toglie le nuvole dalle scene Sentinel-2, usando la classificazione della scena.

IL PROBLEMA
-----------
Scegliere la scena "meno nuvolosa" non basta: il 5% di nuvole su un quadrato
di 100 x 100 km sono 500 km2 di macchie bianche, con le loro ombre scure
accanto. A schermo, sopra il terreno, sono la cosa piu' brutta che si vede.

LA SOLUZIONE
------------
Ogni scena L2A ha un file SCL ("Scene Classification Layer"): per ogni pixel
da 20 m, una classe. Fra queste ci sono le nuvole (probabilita' media e alta),
i cirri e le OMBRE delle nuvole. Si mettono a zero (cioe' "nessun dato", la
convenzione di Sentinel e della pipeline) i pixel del TCI che cadono su quelle
classi, e si compone il quadrato con PIU' scene di giorni diversi: dove la
principale ha una nuvola, si vede quella sotto. E' la stessa idea dei mosaici
"cloudless", fatta in piccolo.

Due dettagli che contano:

- DILATAZIONE. I bordi delle nuvole sono sfumati, e l'SCL li classifica come
  terreno: senza allargare la maschera di qualche pixel, ogni nuvola tolta
  lascerebbe un alone biancastro. Si allarga di 2 pixel SCL (40 m).
- NEVE NO. La classe 11 (neve) NON si toglie: sulle Alpi in estate la neve e'
  vera, e toglierla bucherebbe i ghiacciai.

Lo strato puro (numpy) e' testato senza GDAL; la parte che legge e scrive i
file usa GDAL e si importa solo quando serve.
"""

from __future__ import annotations

import os

import numpy as np

#: Classi SCL da togliere. Le altre (vegetazione, suolo, acqua, neve, zone
#: scure, non classificato) restano.
#:   0 nessun dato             1 saturato o difettoso
#:   3 ombra di nuvola         8 nuvola, probabilita' media
#:   9 nuvola, probabilita' alta   10 cirri
MASKED_CLASSES = (0, 1, 3, 8, 9, 10)

#: Di quanti pixel SCL (20 m l'uno) allargare la maschera.
DEFAULT_DILATION = 2

#: Righe del TCI lette e scritte per volta: 1024 righe x 10980 colonne x 3
#: bande sono ~33 MB. Leggere una scena intera in una volta ne servirebbero 360.
BLOCK_ROWS = 1024


def cloud_mask(scl: np.ndarray, dilation: int = DEFAULT_DILATION,
               classes: tuple[int, ...] = MASKED_CLASSES) -> np.ndarray:
    """True dove il pixel SCL va tolto, maschera gia' allargata di `dilation` pixel."""
    mask = np.isin(scl, classes)
    return dilate(mask, dilation)


def dilate(mask: np.ndarray, radius: int) -> np.ndarray:
    """
    Dilatazione binaria con un quadrato di lato 2*radius+1, senza scipy.

    Si fa per righe e poi per colonne (il quadrato e' separabile), con
    spostamenti dell'array: 2*radius operazioni per asse invece di
    (2*radius+1)^2.
    """
    if radius <= 0:
        return mask.copy()
    result = mask.copy()
    for axis in (0, 1):
        source = result.copy()
        for shift in range(1, radius + 1):
            if axis == 0:
                result[shift:, :] |= source[:-shift, :]
                result[:-shift, :] |= source[shift:, :]
            else:
                result[:, shift:] |= source[:, :-shift]
                result[:, :-shift] |= source[:, shift:]
    return result


def upsample(mask: np.ndarray, factor: int) -> np.ndarray:
    """Ripete ogni pixel `factor` volte per lato: 20 m -> 10 m con factor 2."""
    if factor == 1:
        return mask
    return np.repeat(np.repeat(mask, factor, axis=0), factor, axis=1)


def apply_mask(rgb: np.ndarray, mask: np.ndarray) -> np.ndarray:
    """
    Azzera i pixel mascherati. `rgb` ha forma (bande, righe, colonne), `mask`
    (righe, colonne) alla stessa risoluzione. Ritorna una copia.

    Un pixel VALIDO che per caso fosse (0, 0, 0) diventerebbe "nessun dato"
    lo stesso: lo si porta a (1, 1, 1), invisibile a occhio ma distinto.
    """
    out = rgb.copy()
    black = np.all(out == 0, axis=0) & ~mask
    out[:, black] = 1
    out[:, mask] = 0
    return out


def masked_fraction(scl: np.ndarray, dilation: int = DEFAULT_DILATION) -> float:
    """Quanta parte della scena verrebbe tolta (0..1). Per i messaggi."""
    return float(cloud_mask(scl, dilation).mean())


# =============================================================================
#  La parte con GDAL
# =============================================================================

def mask_scene(tci_path: str, scl_path: str, out_path: str, *,
               dilation: int = DEFAULT_DILATION, report=print) -> float:
    """
    Scrive `out_path`: il TCI con le nuvole a zero. Ritorna la frazione tolta.

    Il file si scrive accanto con un nome temporaneo e si rinomina alla fine:
    un'interruzione non lascia mai un file a meta' con il nome giusto (la
    stessa regola delle tile, vedi tileformat.py).
    """
    from osgeo import gdal
    gdal.UseExceptions()

    tci = gdal.Open(tci_path)
    scl = gdal.Open(scl_path)
    if tci is None or scl is None:
        raise RuntimeError(f"non riesco ad aprire {tci_path} o {scl_path}")

    if tci.RasterXSize % scl.RasterXSize or tci.RasterYSize % scl.RasterYSize:
        raise RuntimeError(
            f"{os.path.basename(tci_path)} ({tci.RasterXSize}x{tci.RasterYSize}) non e' un "
            f"multiplo intero di SCL ({scl.RasterXSize}x{scl.RasterYSize})")
    factor = tci.RasterXSize // scl.RasterXSize

    # La maschera intera, a 20 m: ~30 MB. Va calcolata tutta PRIMA di
    # dividere in blocchi, altrimenti la dilatazione si fermerebbe ai bordi
    # dei blocchi e lascerebbe righe di alone.
    mask = cloud_mask(scl.GetRasterBand(1).ReadAsArray(), dilation)
    fraction = float(mask.mean())

    temporary = out_path + ".part.tif"
    driver = gdal.GetDriverByName("GTiff")
    # DEFLATE e non JPEG: il JPEG sporcherebbe lo zero dei pixel tolti
    # (diventerebbero 1, 2, 3...) e ai bordi delle nuvole resterebbe una
    # cornice quasi nera che la pipeline non riconoscerebbe come "nessun dato".
    out = driver.Create(temporary, tci.RasterXSize, tci.RasterYSize, tci.RasterCount,
                        gdal.GDT_Byte, options=["TILED=YES", "COMPRESS=DEFLATE",
                                                "PREDICTOR=2", "BIGTIFF=IF_SAFER"])
    out.SetGeoTransform(tci.GetGeoTransform())
    out.SetProjection(tci.GetProjection())

    for top in range(0, tci.RasterYSize, BLOCK_ROWS):
        rows = min(BLOCK_ROWS, tci.RasterYSize - top)
        rgb = tci.ReadAsArray(0, top, tci.RasterXSize, rows)
        if rgb.ndim == 2:
            rgb = rgb[np.newaxis, :, :]
        block_mask = upsample(mask[top // factor:(top + rows + factor - 1) // factor, :], factor)
        block_mask = block_mask[:rows, :tci.RasterXSize]
        masked = apply_mask(rgb, block_mask)
        for band in range(masked.shape[0]):
            out.GetRasterBand(band + 1).WriteArray(masked[band], 0, top)

    for band in range(tci.RasterCount):
        out.GetRasterBand(band + 1).SetNoDataValue(0)
    out.FlushCache()
    out = None
    tci = None
    scl = None
    os.replace(temporary, out_path)

    report(f"  {os.path.basename(out_path)}: tolto il {fraction * 100:.1f}% (nuvole, ombre, cirri)")
    return fraction

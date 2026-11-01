"""
Il generatore finto di entita' (Fase 7): aerei, un elicottero e qualche auto
che girano sopra un punto, scritti in MEMORIA CONDIVISA perche' GeoWorld li
legga e li disegni.

PERCHE' UN GENERATORE FINTO
---------------------------
docs/fase7-design.md, sezione 12: un protocollo vero (CIGI, DIS) porta con se'
insieme il parsing, la rete, le convenzioni e l'estrapolazione, e quando
un'entita' non si muove come dovrebbe non si sa chi incolpare. Con la memoria
condivisa e questo generatore, parsing e rete non ci sono: resta solo cio' che
sta DOPO l'adattatore (le convenzioni, l'estrapolazione, il disegno).

LA MEMORIA
----------
Si chiama "GeoWorldEntities": e' il nome che si passa a
multiprocessing.shared_memory (su Windows un file mapping senza "Global\\", su
Linux e macOS /dev/shm/GeoWorldEntities). Il formato e' scritto per esteso in
Source/GeoEntities/Public/Entities/SharedMemoryFormat.h; qui gli stessi
numeri:

    intestazione 64 byte:  "GENT", versione, misure, bandiere (bit 0: quote
                           sul livello del mare), capacita', quante,
                           SEQUENZA (u64, offset 24), tempo (f64, offset 32)
    entita'     128 byte:  id (u64), tipo DIS (8 byte), bandiere (bit 0:
                           appoggiala al terreno), algoritmo, fazione,
                           indice di modello (i32), lat, lon, quota (f64),
                           imbardata, beccheggio, rollio (f32, gradi, NED
                           LOCALE come CIGI), velocita' N, E, GIU', accele-
                           razione N, E, GIU' (f32), p, q, r (f32, rad/s,
                           assi del corpo), nome (32 byte)

La SEQUENZA e' un seqlock: dispari mentre si scrive, pari quando e' finito.
Chi legge (GeoWorld) riprova se la trova dispari o cambiata.

IL MOTO
-------
Tutto si calcola nel piano tangente del CENTRO (Nord, Est, Giu' del centro),
dove un cerchio e' un cerchio; poi posizione, velocita' e assetto si portano
nel NED del punto dove l'entita' si trova davvero. A 5 km dal centro il nord
locale e' girato di 0,05 gradi rispetto a quello del centro: poco, ma
abbastanza perche' i test lo vedano. E' proprio la trappola 2 del capitolo.

Una virata coordinata (senza scivolare): rateo di virata v/r, rollio
atan(v²/(r g)) verso l'interno, e nel corpo il rateo si divide fra q
(beccheggio) e r (imbardata): p = 0, q = w sin(rollio), r = w cos(rollio).
"""

from __future__ import annotations

import math
import os
import struct
import sys
import time
from dataclasses import dataclass, field
from typing import Callable

NAME = "GeoWorldEntities"
VERSION = 1
HEADER_BYTES = 64
RECORD_BYTES = 128
LABEL_BYTES = 32
DEFAULT_CAPACITY = 1024
FLAG_ALTITUDE_MSL = 1
RECORD_FLAG_CLAMP = 1
SEQUENCE_OFFSET = 24
GRAVITY = 9.80665

# Torino, piazza Castello.
DEFAULT_CENTRE = (45.07, 7.68)

HEADER = struct.Struct("<4sHHHHIIIQd24x")
RECORD = struct.Struct("<QBBHBBBBBBBBiddd12f32s")
SEQUENCE = struct.Struct("<Q")

# Gli algoritmi di estrapolazione di DIS (IEEE 1278.1).
DR_STATIC, DR_FPW, DR_RPW, DR_RVW, DR_FVW, DR_FPB, DR_RPB, DR_RVB, DR_FVB = range(1, 10)
FORCE_OTHER, FORCE_FRIENDLY, FORCE_OPPOSING, FORCE_NEUTRAL = range(4)


def bytes_for(capacity: int) -> int:
    return HEADER_BYTES + RECORD_BYTES * capacity


# =============================================================================
#  La geodesia minima (WGS84), le basi, gli angoli
# =============================================================================

WGS84_A = 6378137.0
WGS84_F = 1.0 / 298.257223563
WGS84_E2 = WGS84_F * (2.0 - WGS84_F)

Vec = tuple[float, float, float]
Mat = tuple[Vec, Vec, Vec]


def geodetic_to_ecef(lat_deg: float, lon_deg: float, height: float) -> Vec:
    lat, lon = math.radians(lat_deg), math.radians(lon_deg)
    n = WGS84_A / math.sqrt(1.0 - WGS84_E2 * math.sin(lat) ** 2)
    return ((n + height) * math.cos(lat) * math.cos(lon),
            (n + height) * math.cos(lat) * math.sin(lon),
            (n * (1.0 - WGS84_E2) + height) * math.sin(lat))


def ecef_to_geodetic(position: Vec) -> tuple[float, float, float]:
    """Iterativo: converge al decimo di millimetro in quattro o cinque passi."""
    x, y, z = position
    p = math.hypot(x, y)
    lon = math.atan2(y, x)
    lat = math.atan2(z, p * (1.0 - WGS84_E2))
    height = 0.0
    for _ in range(8):
        n = WGS84_A / math.sqrt(1.0 - WGS84_E2 * math.sin(lat) ** 2)
        height = p / math.cos(lat) - n
        lat = math.atan2(z, p * (1.0 - WGS84_E2 * n / (n + height)))
    return math.degrees(lat), math.degrees(lon), height


def ned_basis(lat_deg: float, lon_deg: float) -> Mat:
    """Le righe: Nord, Est, Giu' in ECEF. Destrorsa (det +1)."""
    lat, lon = math.radians(lat_deg), math.radians(lon_deg)
    sl, cl, so, co = math.sin(lat), math.cos(lat), math.sin(lon), math.cos(lon)
    return ((-sl * co, -sl * so, cl), (-so, co, 0.0), (-cl * co, -cl * so, -sl))


def mat_vec(m: Mat, v: Vec) -> Vec:
    return tuple(m[i][0] * v[0] + m[i][1] * v[1] + m[i][2] * v[2] for i in range(3))  # type: ignore[return-value]


def mat_mul(a: Mat, b: Mat) -> Mat:
    return tuple(tuple(sum(a[i][k] * b[k][j] for k in range(3)) for j in range(3)) for i in range(3))  # type: ignore[return-value]


def transpose(m: Mat) -> Mat:
    return tuple(tuple(m[j][i] for j in range(3)) for i in range(3))  # type: ignore[return-value]


def determinant(m: Mat) -> float:
    return (m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1])
            - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0])
            + m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]))


def matrix_from_euler(yaw: float, pitch: float, roll: float) -> Mat:
    """Rz(imbardata) Ry(beccheggio) Rx(rollio), radianti: dal corpo al riferimento (come EntityFrames.h)."""
    cy, sy, cp, sp, cr, sr = math.cos(yaw), math.sin(yaw), math.cos(pitch), math.sin(pitch), math.cos(roll), math.sin(roll)
    return ((cy * cp, cy * sp * sr - sy * cr, cy * sp * cr + sy * sr),
            (sy * cp, sy * sp * sr + cy * cr, sy * sp * cr - cy * sr),
            (-sp, cp * sr, cp * cr))


def euler_from_matrix(r: Mat) -> tuple[float, float, float]:
    sin_pitch = max(-1.0, min(1.0, -r[2][0]))
    pitch = math.asin(sin_pitch)
    if abs(sin_pitch) > 0.999999:
        return math.atan2(-r[0][1], r[1][1]), pitch, 0.0
    return math.atan2(r[1][0], r[0][0]), pitch, math.atan2(r[2][1], r[2][2])


# =============================================================================
#  Le entita' e il loro moto
# =============================================================================

@dataclass
class EntityRecord:
    """Un'entita' come sta nella memoria (le unita' del formato)."""
    id: int
    kind: tuple[int, int, int, int, int, int, int]
    label: str
    lat: float
    lon: float
    alt: float
    yaw: float
    pitch: float
    roll: float
    velocity_ned: Vec
    acceleration_ned: Vec
    rates: Vec
    dead_reckoning: int = 0
    force: int = FORCE_OTHER
    clamp: bool = False
    model_id: int = -1


@dataclass
class LocalState:
    """Lo stato nel piano tangente del CENTRO: posizione N, E, Giu', vettori NED, assetto dal corpo al NED."""
    position: Vec
    velocity: Vec
    acceleration: Vec
    attitude: Mat
    rates: Vec


@dataclass
class Circle:
    """Un cerchio a velocita' costante: sign +1 vira a destra (orario visto dall'alto), -1 a sinistra."""
    radius: float
    speed: float
    altitude: float
    sign: int = 1
    phase: float = 0.0
    pitch_deg: float = 0.0
    bank: bool = True
    centre_north: float = 0.0
    centre_east: float = 0.0

    def at(self, t: float) -> LocalState:
        omega = self.sign * self.speed / self.radius
        alpha = self.phase + omega * t
        heading = alpha + self.sign * math.pi / 2.0
        centripetal = self.speed ** 2 / self.radius
        roll = self.sign * math.atan(centripetal / GRAVITY) if self.bank else 0.0
        pitch = math.radians(self.pitch_deg)
        attitude = matrix_from_euler(heading, pitch, roll)
        # La velocita' angolare del mondo e' w attorno al Giu'; nel corpo: (Ry Rx)^T (0, 0, w).
        tilt = mat_mul(matrix_from_euler(0.0, pitch, 0.0), matrix_from_euler(0.0, 0.0, roll))
        rates = mat_vec(transpose(tilt), (0.0, 0.0, omega))
        return LocalState(
            position=(self.centre_north + self.radius * math.cos(alpha),
                      self.centre_east + self.radius * math.sin(alpha), -self.altitude),
            velocity=(self.speed * math.cos(heading), self.speed * math.sin(heading), 0.0),
            acceleration=(-centripetal * math.cos(alpha), -centripetal * math.sin(alpha), 0.0),
            attitude=attitude,
            rates=rates)


@dataclass
class Entity:
    id: int
    label: str
    kind: tuple[int, int, int, int, int, int, int]
    motion: Circle
    dead_reckoning: int
    force: int
    clamp: bool = False
    model_id: int = -1


@dataclass
class Scenario:
    centre_lat: float
    centre_lon: float
    entities: list[Entity] = field(default_factory=list)

    def __post_init__(self) -> None:
        self._centre = geodetic_to_ecef(self.centre_lat, self.centre_lon, 0.0)
        self._to_ecef = transpose(ned_basis(self.centre_lat, self.centre_lon))

    def record(self, entity: Entity, t: float) -> EntityRecord:
        """Dal piano del centro al NED del punto: posizione, vettori e assetto (la trappola 2)."""
        local = entity.motion.at(t)
        offset = mat_vec(self._to_ecef, local.position)
        ecef = (self._centre[0] + offset[0], self._centre[1] + offset[1], self._centre[2] + offset[2])
        lat, lon, height = ecef_to_geodetic(ecef)
        here = ned_basis(lat, lon)
        centre_to_here = mat_mul(here, self._to_ecef)
        yaw, pitch, roll = euler_from_matrix(mat_mul(centre_to_here, local.attitude))
        return EntityRecord(
            id=entity.id, kind=entity.kind, label=entity.label, lat=lat, lon=lon, alt=height,
            yaw=math.degrees(yaw), pitch=math.degrees(pitch), roll=math.degrees(roll),
            velocity_ned=mat_vec(centre_to_here, local.velocity),
            acceleration_ned=mat_vec(centre_to_here, local.acceleration),
            rates=local.rates, dead_reckoning=entity.dead_reckoning, force=entity.force,
            clamp=entity.clamp, model_id=entity.model_id)

    def records(self, t: float) -> list[EntityRecord]:
        return [self.record(entity, t) for entity in self.entities]


def demo_scenario(centre_lat: float = DEFAULT_CENTRE[0], centre_lon: float = DEFAULT_CENTRE[1], crowd: int = 0) -> Scenario:
    """
    Due aerei (uno vira a destra con l'estrapolazione negli assi del corpo, uno
    a sinistra con quella del mondo), un elicottero, quattro auto appoggiate al
    terreno; piu' 'crowd' auto in giro entro 20 km, per provare il carico.
    Le quote sono sul livello del mare (la bandiera dell'intestazione).
    """
    air, land = 2, 1
    italy = 105
    entities = [
        Entity(1, "AEREO-1", (1, air, italy, 1, 0, 0, 0), Circle(3000.0, 120.0, 1500.0, +1, 0.0), DR_RVB, FORCE_FRIENDLY),
        Entity(2, "AEREO-2", (1, air, italy, 4, 0, 0, 0), Circle(5000.0, 160.0, 2500.0, -1, 2.0), DR_RVW, FORCE_FRIENDLY),
        Entity(3, "ELICOTTERO", (1, air, italy, 21, 0, 0, 0),
               Circle(400.0, 20.0, 450.0, +1, 1.0, pitch_deg=-5.0, centre_north=1500.0, centre_east=-1000.0),
               DR_RPW, FORCE_FRIENDLY),
    ]
    cars = [(800.0, 14.0, +1, 0.3, 0.0, 0.0), (1000.0, 12.0, -1, 1.7, -600.0, 900.0),
            (1200.0, 16.0, +1, 4.0, 700.0, -500.0), (1500.0, 13.0, -1, 5.5, 300.0, 300.0)]
    for index, (radius, speed, sign, phase, north, east) in enumerate(cars):
        entities.append(Entity(10 + index, f"AUTO-{index + 1}", (1, land, italy, 81, 0, 0, 0),
                               Circle(radius, speed, 0.0, sign, phase, bank=False, centre_north=north, centre_east=east),
                               DR_FPW, FORCE_NEUTRAL, clamp=True))
    for index in range(crowd):
        # Un generatore deterministico: la stessa folla a ogni avvio.
        h = (index * 2654435761) & 0xFFFFFFFF
        radius = 300.0 + (h % 1700)
        north = ((h >> 8) % 40000) - 20000.0
        east = ((h >> 4) % 40000) - 20000.0
        entities.append(Entity(1000 + index, f"FOLLA-{index + 1}", (1, land, italy, 81, 0, 0, 0),
                               Circle(radius, 8.0 + (h % 13), 0.0, 1 if h & 1 else -1, (h % 628) / 100.0,
                                      bank=False, centre_north=north, centre_east=east),
                               DR_FPW, FORCE_NEUTRAL, clamp=True))
    return Scenario(centre_lat, centre_lon, entities)


# =============================================================================
#  Il formato
# =============================================================================

def pack_record(record: EntityRecord) -> bytes:
    label = record.label.encode("utf-8")[:LABEL_BYTES - 1]
    kind = record.kind
    return RECORD.pack(
        record.id, kind[0], kind[1], kind[2], kind[3], kind[4], kind[5], kind[6],
        RECORD_FLAG_CLAMP if record.clamp else 0, record.dead_reckoning, record.force, 0, record.model_id,
        record.lat, record.lon, record.alt,
        record.yaw, record.pitch, record.roll, *record.velocity_ned, *record.acceleration_ned, *record.rates,
        label)


def unpack_record(data: bytes) -> EntityRecord:
    (ident, k0, k1, k2, k3, k4, k5, k6, flags, dr, force, _reserved, model_id, lat, lon, alt,
     yaw, pitch, roll, vn, ve, vd, an, ae, ad, p, q, r, label) = RECORD.unpack(data)
    return EntityRecord(id=ident, kind=(k0, k1, k2, k3, k4, k5, k6), label=label.split(b"\0", 1)[0].decode("utf-8", "replace"),
                        lat=lat, lon=lon, alt=alt, yaw=yaw, pitch=pitch, roll=roll,
                        velocity_ned=(vn, ve, vd), acceleration_ned=(an, ae, ad), rates=(p, q, r),
                        dead_reckoning=dr, force=force, clamp=bool(flags & RECORD_FLAG_CLAMP), model_id=model_id)


def pack_header(capacity: int, count: int, sequence: int, sim_time: float, altitude_msl: bool) -> bytes:
    return HEADER.pack(b"GENT", VERSION, HEADER_BYTES, RECORD_BYTES, FLAG_ALTITUDE_MSL if altitude_msl else 0,
                       capacity, count, 0, sequence, sim_time)


def write_snapshot(buffer, capacity: int, sequence: int, sim_time: float, records: list[EntityRecord],
                   altitude_msl: bool = True) -> int:
    """
    Scrive una fotografia nel buffer (la memoria condivisa o un bytearray)
    con il protocollo del seqlock. 'sequence' e' l'ultimo valore pari; ne
    restituisce il nuovo.
    """
    if len(records) > capacity:
        raise ValueError(f"{len(records)} entita' in una memoria da {capacity}")
    SEQUENCE.pack_into(buffer, SEQUENCE_OFFSET, sequence + 1)          # dispari: sto scrivendo
    for index, record in enumerate(records):
        offset = HEADER_BYTES + RECORD_BYTES * index
        buffer[offset:offset + RECORD_BYTES] = pack_record(record)
    header = pack_header(capacity, len(records), sequence + 1, sim_time, altitude_msl)
    buffer[0:HEADER_BYTES] = header                                    # la sequenza resta dispari
    SEQUENCE.pack_into(buffer, SEQUENCE_OFFSET, sequence + 2)          # pari: fatto
    return sequence + 2


def read_snapshot(buffer) -> tuple[float, bool, list[EntityRecord]]:
    """La fotografia (senza il seqlock: per i test e per chi controlla un file)."""
    magic, version, header_bytes, record_bytes, flags, capacity, count, _reserved, sequence, sim_time = \
        HEADER.unpack_from(buffer, 0)
    if magic != b"GENT" or version != VERSION or header_bytes != HEADER_BYTES or record_bytes != RECORD_BYTES:
        raise ValueError("non e' memoria di entita' di GeoWorld")
    if count > capacity:
        raise ValueError("piu' entita' della capacita'")
    records = [unpack_record(bytes(buffer[HEADER_BYTES + RECORD_BYTES * i:HEADER_BYTES + RECORD_BYTES * (i + 1)]))
               for i in range(count)]
    return sim_time, bool(flags & FLAG_ALTITUDE_MSL), records


FIXTURE_TIMES = (10.0, 10.5)
FIXTURE_CAPACITY = 16


def fixture_bytes(centre_lat: float = DEFAULT_CENTRE[0], centre_lon: float = DEFAULT_CENTRE[1]) -> bytes:
    """
    Il file di prova dei test C++ (TestData/entities_fixture.bin): due
    fotografie della scena di prova, a 10,0 e 10,5 secondi, una dietro
    l'altra. Il test C++ estrapola la prima di mezzo secondo e deve ritrovare
    la seconda: se Python e C++ non sono d'accordo su una convenzione (NED,
    il verso degli angoli, gli assi del corpo), non la ritrova.
    """
    scenario = demo_scenario(centre_lat, centre_lon)
    out = bytearray()
    sequence = 0
    for t in FIXTURE_TIMES:
        image = bytearray(bytes_for(FIXTURE_CAPACITY))
        sequence = write_snapshot(image, FIXTURE_CAPACITY, sequence, t, scenario.records(t))
        out += image
    return bytes(out)


# =============================================================================
#  La memoria condivisa vera
# =============================================================================

class SharedMemoryWriter:
    """La memoria con il nome, creata da noi; via quando si chiude."""

    def __init__(self, name: str = NAME, capacity: int = DEFAULT_CAPACITY, altitude_msl: bool = True):
        from multiprocessing import shared_memory
        self.capacity = capacity
        self.altitude_msl = altitude_msl
        size = bytes_for(capacity)
        try:
            self.memory = shared_memory.SharedMemory(name=name, create=True, size=size)
            self._owner = True
        except FileExistsError:
            # Lasciata da un generatore interrotto male (Linux): la si riusa.
            self.memory = shared_memory.SharedMemory(name=name, create=False)
            self._owner = False
            if self.memory.size < size:
                self.memory.close()
                raise RuntimeError(f"la memoria '{name}' esiste gia' ed e' piu' piccola ({self.memory.size} byte): "
                                   "chiudi chi la tiene aperta")
        self.sequence = 0
        if not self._owner:
            previous = SEQUENCE.unpack_from(self.memory.buf, SEQUENCE_OFFSET)[0]
            self.sequence = previous + (previous & 1)
        self.write(0.0, [])

    def write(self, sim_time: float, records: list[EntityRecord]) -> None:
        self.sequence = write_snapshot(self.memory.buf, self.capacity, self.sequence, sim_time, records, self.altitude_msl)

    def close(self) -> None:
        # Una fotografia vuota prima di andarsene: chi legge vede sparire tutti.
        try:
            self.write(0.0, [])
        except (ValueError, TypeError):
            pass
        self.memory.close()
        if self._owner and os.name != "nt":
            try:
                self.memory.unlink()
            except FileNotFoundError:
                pass

    def __enter__(self) -> "SharedMemoryWriter":
        return self

    def __exit__(self, *exc) -> None:
        self.close()


def run_demo(centre_lat: float = DEFAULT_CENTRE[0], centre_lon: float = DEFAULT_CENTRE[1], rate: float = 10.0,
             duration: float | None = None, name: str = NAME, crowd: int = 0,
             report: Callable[[str], None] = print, clock: Callable[[], float] = time.monotonic) -> int:
    """Scrive la scena 'rate' volte al secondo finche' non la si ferma (Ctrl+C) o per 'duration' secondi."""
    if rate <= 0.0 or rate > 1000.0:
        raise ValueError("la frequenza va da 0 a 1000 Hz")
    scenario = demo_scenario(centre_lat, centre_lon, crowd)
    capacity = max(DEFAULT_CAPACITY, len(scenario.entities))
    period = 1.0 / rate
    frames = 0
    with SharedMemoryWriter(name, capacity) as writer:
        report(f"Memoria condivisa '{name}': {len(scenario.entities)} entita' attorno a "
               f"{centre_lat:.4f}, {centre_lon:.4f}, {rate:g} volte al secondo. Ctrl+C per fermare.")
        report("In Unreal: geo.Entities.Listen (e geo.Entities.Follow AEREO-1 per seguirne uno)")
        start = clock()
        next_report = 5.0
        try:
            while True:
                t = clock() - start
                if duration is not None and t >= duration:
                    break
                writer.write(t, scenario.records(t))
                frames += 1
                if t >= next_report:
                    report(f"  t = {t:6.1f} s, {frames} fotografie")
                    next_report += 5.0
                sleep = start + frames * period - clock()
                if sleep > 0.0:
                    time.sleep(sleep)
        except KeyboardInterrupt:
            report("Fermato.")
    return frames


def main_fixture(path: str) -> None:
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, "wb") as handle:
        handle.write(fixture_bytes())


if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] == "--fixture":
        main_fixture(sys.argv[2])
    else:
        run_demo()

"""
Test del generatore di entita' (Fase 7): il formato della memoria condivisa,
la fixture dei test C++, la fisica della scena (le velocita' sono davvero le
derivate delle posizioni, il rollio e' quello di una virata coordinata, le
velocita' angolari sono quelle del corpo), la memoria condivisa vera e il
comando.
"""

import math
import os
import unittest
import uuid

from geoworld import cli, entitygen as g

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
FIXTURE = os.path.join(REPO, "Plugins", "GeoWorld", "Source", "GeoTiles", "TestData", "entities_fixture.bin")


def sub(a, b):
    return tuple(x - y for x, y in zip(a, b))


def norm(v):
    return math.sqrt(sum(x * x for x in v))


def ecef_of(record):
    return g.geodetic_to_ecef(record.lat, record.lon, record.alt)


def ned_to_ecef(record, vector):
    return g.mat_vec(g.transpose(g.ned_basis(record.lat, record.lon)), vector)


def attitude_ecef(record):
    """Dal corpo all'ECEF: la base NED del punto composta con gli angoli locali."""
    local = g.matrix_from_euler(math.radians(record.yaw), math.radians(record.pitch), math.radians(record.roll))
    return g.mat_mul(g.transpose(g.ned_basis(record.lat, record.lon)), local)


class FormatTest(unittest.TestCase):
    def test_sizes_match_the_cpp_header(self):
        # SharedMemoryFormat.h: 64 e 128 byte, la sequenza all'offset 24, il tempo al 32.
        self.assertEqual(g.HEADER.size, 64)
        self.assertEqual(g.RECORD.size, 128)
        header = g.pack_header(16, 3, 42, 12.5, True)
        self.assertEqual(header[:4], b"GENT")
        self.assertEqual(int.from_bytes(header[24:32], "little"), 42)
        self.assertEqual(g.struct.unpack_from("<d", header, 32)[0], 12.5)
        self.assertEqual(g.bytes_for(1024), 64 + 128 * 1024)

    def test_record_round_trip(self):
        record = g.EntityRecord(id=2 ** 40 + 7, kind=(1, 2, 105, 1, 2, 3, 4), label="UN NOME MOLTO PIU' LUNGO DI TRENTUNO BYTE",
                                lat=45.07, lon=7.68, alt=1234.5, yaw=90.0, pitch=-5.0, roll=30.0,
                                velocity_ned=(1.0, 2.0, -3.0), acceleration_ned=(0.5, 0.0, 0.0), rates=(0.0, 0.1, 0.2),
                                dead_reckoning=g.DR_RVB, force=g.FORCE_OPPOSING, clamp=True, model_id=-1)
        back = g.unpack_record(g.pack_record(record))
        self.assertEqual(back.id, record.id)
        self.assertEqual(back.kind, record.kind)
        self.assertEqual(back.model_id, -1)
        self.assertTrue(back.clamp)
        self.assertEqual(back.lat, 45.07)
        self.assertEqual(len(back.label.encode()), 31)
        self.assertAlmostEqual(back.velocity_ned[2], -3.0)

    def test_snapshot_uses_the_seqlock(self):
        buffer = bytearray(g.bytes_for(4))
        records = g.demo_scenario().records(1.0)[:4]
        sequence = g.write_snapshot(buffer, 4, 0, 1.0, records)
        self.assertEqual(sequence, 2)
        self.assertEqual(int.from_bytes(buffer[24:32], "little"), 2, "pari quando ha finito")
        sequence = g.write_snapshot(buffer, 4, sequence, 2.0, records)
        self.assertEqual(sequence, 4)
        sim_time, msl, back = g.read_snapshot(buffer)
        self.assertEqual(sim_time, 2.0)
        self.assertTrue(msl)
        self.assertEqual([r.label for r in back], [r.label for r in records])
        with self.assertRaises(ValueError):
            g.write_snapshot(buffer, 4, sequence, 3.0, g.demo_scenario().records(1.0))

    def test_fixture_is_what_the_generator_writes(self):
        with open(FIXTURE, "rb") as handle:
            self.assertEqual(handle.read(), g.fixture_bytes(),
                             "rigenera con: geoworld demo-entities --fixture <TestData/entities_fixture.bin>")


class FramesTest(unittest.TestCase):
    def test_ned_is_right_handed(self):
        self.assertAlmostEqual(g.determinant(g.ned_basis(45.07, 7.68)), 1.0, places=12)
        down = g.ned_basis(45.07, 7.68)[2]
        up = g.geodetic_to_ecef(45.07, 7.68, 1.0)
        ground = g.geodetic_to_ecef(45.07, 7.68, 0.0)
        self.assertAlmostEqual(sum(d * (u - v) for d, u, v in zip(down, up, ground)), -1.0, places=6)

    def test_geodetic_round_trip(self):
        for lat, lon, h in ((45.07, 7.68, 300.0), (-33.9, 151.2, 5000.0), (89.0, -120.0, -50.0)):
            back = g.ecef_to_geodetic(g.geodetic_to_ecef(lat, lon, h))
            self.assertAlmostEqual(back[0], lat, places=9)
            self.assertAlmostEqual(back[1], lon, places=9)
            self.assertAlmostEqual(back[2], h, places=4)

    def test_euler_round_trip(self):
        for yaw, pitch, roll in ((0.3, 0.2, -0.4), (-2.9, -1.2, 3.0), (1.0, 0.0, 0.0)):
            back = g.euler_from_matrix(g.matrix_from_euler(yaw, pitch, roll))
            for a, b in zip(back, (yaw, pitch, roll)):
                self.assertAlmostEqual(a, b, places=12)


class SceneTest(unittest.TestCase):
    """La fisica della scena: quello che il generatore scrive deve essere coerente con se stesso."""

    def setUp(self):
        self.scene = g.demo_scenario()

    def test_cast(self):
        records = self.scene.records(0.0)
        self.assertEqual([r.label for r in records],
                         ["AEREO-1", "AEREO-2", "ELICOTTERO", "AUTO-1", "AUTO-2", "AUTO-3", "AUTO-4"])
        self.assertEqual(sum(r.clamp for r in records), 4)
        self.assertEqual(len(g.demo_scenario(crowd=50).entities), 57)

    def test_velocity_is_the_derivative_of_position(self):
        dt = 1e-3
        for entity in self.scene.entities:
            before, after, now = self.scene.record(entity, 5.0 - dt), self.scene.record(entity, 5.0 + dt), self.scene.record(entity, 5.0)
            numeric = tuple(x / (2 * dt) for x in sub(ecef_of(after), ecef_of(before)))
            error = norm(sub(numeric, ned_to_ecef(now, now.velocity_ned)))
            self.assertLess(error, 1e-3, f"{entity.label}: la velocita' NED (+giu' scende) e' la derivata della posizione")

    def test_acceleration_is_the_derivative_of_velocity(self):
        dt = 1e-3
        for entity in self.scene.entities:
            before, after, now = self.scene.record(entity, 5.0 - dt), self.scene.record(entity, 5.0 + dt), self.scene.record(entity, 5.0)
            numeric = tuple(x / (2 * dt) for x in sub(ned_to_ecef(after, after.velocity_ned), ned_to_ecef(before, before.velocity_ned)))
            self.assertLess(norm(sub(numeric, ned_to_ecef(now, now.acceleration_ned))), 1e-3, entity.label)

    def test_heading_follows_the_velocity(self):
        for record in self.scene.records(7.0):
            course = math.degrees(math.atan2(record.velocity_ned[1], record.velocity_ned[0]))
            gap = (course - record.yaw + 180.0) % 360.0 - 180.0
            # 1e-3 gradi: il muso dell'elicottero e' inclinato in giu' di 5 gradi, e
            # portato nel NED del punto (inclinato di qualche millesimo di grado su
            # quello del centro) la sua proiezione si sposta di un soffio.
            self.assertLess(abs(gap), 1e-3, f"{record.label}: l'imbardata (da nord verso est) e' la rotta")

    def test_coordinated_turn(self):
        for record in self.scene.records(3.0)[:2]:
            speed = math.hypot(record.velocity_ned[0], record.velocity_ned[1])
            accel = math.hypot(record.acceleration_ned[0], record.acceleration_ned[1])
            bank = math.degrees(math.atan(accel / g.GRAVITY))
            # Vira a destra (rateo r > 0): l'ala destra giu', rollio positivo.
            self.assertAlmostEqual(abs(record.roll), bank, delta=0.1)
            self.assertEqual(record.roll > 0, record.rates[2] > 0)
            self.assertAlmostEqual(record.rates[0], 0.0, places=12)
            self.assertAlmostEqual(math.hypot(record.rates[1], record.rates[2]), accel / speed, places=6)

    def test_rates_are_in_body_axes(self):
        # R(t)^T R(t+dt) = I + [w]x dt: le velocita' angolari si leggono nel corpo.
        dt = 1e-4
        for entity in self.scene.entities[:3]:
            a, b = self.scene.record(entity, 4.0), self.scene.record(entity, 4.0 + dt)
            delta = g.mat_mul(g.transpose(attitude_ecef(a)), attitude_ecef(b))
            omega = ((delta[2][1] - delta[1][2]) / (2 * dt), (delta[0][2] - delta[2][0]) / (2 * dt), (delta[1][0] - delta[0][1]) / (2 * dt))
            self.assertLess(norm(sub(omega, a.rates)), 1e-5, f"{entity.label}: p, q, r negli assi del corpo")

    def test_heights_and_places(self):
        for record in self.scene.records(0.0):
            here = g.geodetic_to_ecef(record.lat, record.lon, 0.0)
            centre = g.geodetic_to_ecef(45.07, 7.68, 0.0)
            self.assertLess(norm(sub(here, centre)), 10000.0)
            if record.clamp:
                self.assertLess(record.alt, 5.0)
            else:
                self.assertGreater(record.alt, 400.0)


class LiveTest(unittest.TestCase):
    def test_shared_memory_and_command(self):
        name = "GeoWorldTest" + uuid.uuid4().hex[:8]
        try:
            writer = g.SharedMemoryWriter(name, capacity=32)
        except (OSError, ImportError) as error:  # pragma: no cover - una macchina senza memoria condivisa
            self.skipTest(f"memoria condivisa non disponibile: {error}")
        from multiprocessing import shared_memory
        try:
            writer.write(1.5, g.demo_scenario().records(1.5))
            reader = shared_memory.SharedMemory(name=name, create=False)
            try:
                sim_time, msl, records = g.read_snapshot(reader.buf)
                self.assertEqual(sim_time, 1.5)
                self.assertTrue(msl)
                self.assertEqual(len(records), 7)
                self.assertEqual(int.from_bytes(bytes(reader.buf[24:32]), "little") % 2, 0)
            finally:
                reader.close()
        finally:
            writer.close()

        messages = []
        frames = g.run_demo(rate=50.0, duration=0.2, name=name + "b", report=messages.append)
        self.assertGreaterEqual(frames, 5)
        self.assertIn("geo.Entities.Listen", "\n".join(messages))
        self.assertEqual(cli.main(["demo-entities", "--duration", "0.1", "--rate", "20", "--name", name + "c"]), 0)
        with self.assertRaises(ValueError):
            g.run_demo(rate=0.0, duration=0.1, name=name + "d")


if __name__ == "__main__":
    unittest.main()

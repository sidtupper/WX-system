"""Hardware-free regression tests of the candidate station's actual calculation code.

AST extraction avoids importing Pi-only INA226/TFT drivers and opening UARTs.
No production code is duplicated: selected definitions come from the source file.
Run from repository root:
  python3 -m unittest discover -s tests -p 'test_wind_statistics.py' -v
"""
from __future__ import annotations

import ast
from collections import deque
from dataclasses import dataclass
import math
from pathlib import Path
import unittest


def load_station_calculations():
    path = Path(__file__).resolve().parents[1] / "station" / "station_wind_gust.py"
    tree = ast.parse(path.read_text(encoding="utf-8"), filename=str(path))
    names = {"WindReading", "RainReading", "WindAccumulator",
             "crc16_modbus", "add_packet_crc", "build_payload"}
    selected = [node for node in tree.body
                if isinstance(node, (ast.ClassDef, ast.FunctionDef))
                and node.name in names]
    actual = {node.name for node in selected}
    if actual != names:
        raise AssertionError(f"Missing station definitions: {names - actual}")
    namespace = {"dataclass": dataclass, "deque": deque, "math": math,
                 "GUST_WINDOW_SAMPLES": 3}
    module = ast.fix_missing_locations(ast.Module(body=selected, type_ignores=[]))
    exec(compile(module, str(path), "exec"), namespace)
    return namespace


S = load_station_calculations()
WindReading = S["WindReading"]
RainReading = S["RainReading"]
WindAccumulator = S["WindAccumulator"]
build_payload = S["build_payload"]
add_packet_crc = S["add_packet_crc"]
crc16_modbus = S["crc16_modbus"]


def reading(speed, direction):
    return WindReading(speed_m_s=speed, direction_deg=direction, gust_m_s=speed,
                       temperature_c=12.5, humidity_percent=72.0,
                       pressure_hpa=1013.2)


class WindStatisticsTests(unittest.TestCase):
    def test_steady_wind(self):
        acc = WindAccumulator(1.0)
        for t in range(600):
            acc.add(reading(5.0, 270.0), float(t))
        result = acc.result()
        self.assertEqual(acc.count, 600)
        self.assertAlmostEqual(result.speed_m_s, 5.0)
        self.assertAlmostEqual(result.direction_deg, 270.0)
        self.assertAlmostEqual(result.gust_m_s, 5.0)

    def test_three_second_gust_excludes_single_sample_spike(self):
        acc = WindAccumulator(1.0)
        speeds = [2, 2, 2, 2, 8, 8, 8, 2, 2, 2]
        for t, speed in enumerate(speeds):
            acc.add(reading(speed, 90), float(t))
        result = acc.result()
        self.assertAlmostEqual(result.speed_m_s, 3.8)
        self.assertAlmostEqual(result.gust_m_s, 8.0)
        self.assertAlmostEqual(acc.peak_sample, 8.0)

    def test_north_crossing(self):
        acc = WindAccumulator(1.0)
        for t, direction in enumerate([359.0, 1.0] * 10):
            acc.add(reading(5.0, direction), float(t))
        direction = acc.result().direction_deg
        self.assertLess(min(direction, 360.0 - direction), 0.01)

    def test_missing_sample_breaks_gust_window(self):
        acc = WindAccumulator(1.0)
        for t, speed in [(0, 10), (1, 10), (4, 2), (5, 2), (6, 2)]:
            acc.add(reading(speed, 180), float(t))
        self.assertAlmostEqual(acc.result().gust_m_s, 2.0)

    def test_invalid_readings(self):
        for speed, direction in [(-1, 90), (float("nan"), 90),
                                 (3, float("inf")), (3, 361)]:
            with self.subTest(speed=speed, direction=direction):
                with self.assertRaises(ValueError):
                    WindAccumulator(1.0).add(reading(speed, direction), 0)

    def test_payload_timestamp_and_crc(self):
        acc = WindAccumulator(1.0)
        for t in range(3):
            acc.add(reading(5.0, 270.0), float(t))
        interval_end_utc = 1800000600  # exact 600-second epoch boundary
        self.assertEqual(interval_end_utc % 600, 0)
        payload = build_payload(epoch=interval_end_utc, wind=acc.result(),
                                rain=RainReading(0.0),
                                sea_temperature_c=10.4,
                                cabinet_temperature_c=18.0,
                                battery_voltage=12.7)
        fields = payload.split(",")
        self.assertEqual(len(fields), 11)
        self.assertEqual(fields[:4], [str(interval_end_utc), "5.00",
                                     "270.0", "5.00"])
        packet = add_packet_crc(payload)
        self.assertTrue(packet.startswith("@"))
        self.assertTrue(packet.endswith("\r\n"))
        body, crc = packet[1:].strip().rsplit("*", 1)
        self.assertEqual(body, payload)
        self.assertEqual(int(crc, 16), crc16_modbus(body.encode("ascii")))


if __name__ == "__main__":
    unittest.main()

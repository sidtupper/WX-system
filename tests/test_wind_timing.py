"""Hardware-free tests for wind_timing.py.

Run: python3 -m unittest discover -s tests -p 'test_wind_timing.py' -v
"""
import pathlib
import sys
import threading
import time
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "station"))
from wind_timing import UTCWindSampler, next_utc_boundary


class FakeAccumulator:
    def __init__(self):
        self.samples = []

    def add(self, reading, timestamp):
        self.samples.append((reading, timestamp))


class FakeSensor:
    def __init__(self):
        self.samples = 0

    def read_all(self):
        self.samples += 1
        return 3.0


class TimingTests(unittest.TestCase):
    def test_boundary_examples(self):
        self.assertEqual(next_utc_boundary(600, 600), 1200)
        self.assertEqual(next_utc_boundary(601, 600), 1200)
        self.assertEqual(next_utc_boundary(1199.99, 600), 1200)
        self.assertEqual(next_utc_boundary(0, 600), 600)
        self.assertEqual(next_utc_boundary(-0.1, 600), 0)

    def test_invalid_intervals(self):
        for interval in (0, -600, 601, 700):
            with self.assertRaises(ValueError):
                next_utc_boundary(1000, interval)

    def test_worker_runs_while_consumer_busy(self):
        sensor = FakeSensor()
        sampler = UTCWindSampler(sensor, FakeAccumulator, threading.Lock(),
                                 interval=1, sample_interval=0.05)
        try:
            sampler.start()
            # Avoid running the consumer for 2.5 seconds: simulate a slow
            # TFT/display or sensor collection while wind keeps polling.
            time.sleep(2.5)
            self.assertGreater(sensor.samples, 20)
            item = sampler.get(timeout=0.25)
            self.assertIsNotNone(item, "No full UTC-boundary interval emitted")
            timestamp, accumulator = item
            self.assertEqual(timestamp, int(timestamp))
            self.assertEqual(timestamp % 1, 0)
            self.assertGreater(len(accumulator.samples), 0)
        finally:
            sampler.stop()


if __name__ == "__main__":
    unittest.main()

"""UTC-aligned 1 Hz wind sampling without Raspberry Pi hardware dependencies.

The sampler is the sole wind reader; other Modbus users share its bus lock.
Completed intervals are queued while the sampler continues observing.
"""
from __future__ import annotations

import math
import queue
import threading
import time


def next_utc_boundary(now: float, interval: int) -> int:
    """Next *strictly future* Unix epoch multiple of interval."""
    if interval <= 0 or 86400 % interval:
        raise ValueError("report interval must be a positive divisor of 86400")
    return (math.floor(now / interval) + 1) * interval


class UTCWindSampler:
    def __init__(self, sensor, accumulator_factory, bus_lock, interval=600,
                 sample_interval=1.0, logger=None):
        if sample_interval <= 0 or sample_interval * 3 > interval:
            raise ValueError("invalid wind sampling interval")
        next_utc_boundary(time.time(), interval)  # Validate calendar alignment.
        self.sensor = sensor
        self.accumulator_factory = accumulator_factory
        self.bus_lock = bus_lock
        self.interval = interval
        self.sample_interval = sample_interval
        self.log = logger
        self.completed = queue.Queue(maxsize=4)
        self._stop = threading.Event()
        self._thread = None

    def start(self):
        if self._thread is not None:
            raise RuntimeError("sampler has already started")
        self._thread = threading.Thread(target=self._run, name="wind-1hz",
                                        daemon=True)
        self._thread.start()

    def get(self, timeout=0.2):
        try:
            return self.completed.get(timeout=timeout)
        except queue.Empty:
            return None

    def stop(self):
        self._stop.set()
        if self._thread is not None:
            self._thread.join(timeout=5)
            if self._thread.is_alive() and self.log:
                self.log.error("Wind thread has not stopped; check blocked Modbus IO")

    def _run(self):
        accumulator = self.accumulator_factory()
        now = time.time()
        boundary = next_utc_boundary(now, self.interval)
        # Discard the startup partial interval, not an actual full 10 minutes.
        full_interval = False
        previous_wall = now
        next_sample = time.monotonic()

        while not self._stop.is_set():
            wall = time.time()
            # GPS/NTP could step the clock. Never label mixed observations
            # as a complete UTC interval after a significant time jump.
            if wall < previous_wall - 2 or wall - previous_wall > max(5, self.interval * 2):
                if self.log:
                    self.log.warning("Clock step detected; discarding partial wind interval")
                accumulator = self.accumulator_factory()
                boundary = next_utc_boundary(wall, self.interval)
                full_interval = False
                next_sample = time.monotonic()
            previous_wall = wall

            if wall >= boundary:
                if full_interval:
                    try:
                        self.completed.put_nowait((boundary, accumulator))
                    except queue.Full:
                        if self.log:
                            self.log.error("Wind result queue full; dropping interval %d",
                                           boundary)
                elif self.log:
                    self.log.info("Discarded startup partial interval ending %d", boundary)
                accumulator = self.accumulator_factory()
                boundary = next_utc_boundary(wall, self.interval)
                full_interval = True

            mono = time.monotonic()
            if mono >= next_sample:
                try:
                    with self.bus_lock:
                        reading = self.sensor.read_all()
                    # If a read straddles the boundary, credit the *ending*
                    # observation to the new interval, not the preceding one.
                    wall = time.time()
                    if wall >= boundary:
                        if full_interval:
                            try:
                                self.completed.put_nowait((boundary, accumulator))
                            except queue.Full:
                                if self.log:
                                    self.log.error("Wind result queue full; dropping %d",
                                                   boundary)
                        accumulator = self.accumulator_factory()
                        boundary = next_utc_boundary(wall, self.interval)
                        full_interval = True
                    accumulator.add(reading, time.monotonic())
                except Exception as exc:
                    if self.log:
                        self.log.warning("Wind sample failed: %s", exc)
                next_sample += self.sample_interval
                if next_sample <= time.monotonic():
                    next_sample = time.monotonic() + self.sample_interval

            self._stop.wait(min(0.05, max(0.0, next_sample - time.monotonic())))

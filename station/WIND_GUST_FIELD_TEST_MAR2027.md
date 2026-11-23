# March 2027 field test — 1 Hz wind gust sampling

**Status:** EXPERIMENTAL / NOT DEPLOYED OR HARDWARE-TESTED. Created 2026-11-22. Do not merge into `main` until tested on the station Raspberry Pi.

## Files
- `station/station.py` — existing version preserved from `main` (not replaced).
- `station/station_wind_gust.py` — candidate version for field testing.\n- `station/wind_timing.py` — independent UTC-boundary sampler thread.\n- `tests/test_wind_timing.py` — hardware-free UTC schedule and concurrent-sampling tests.

## Intended changes
- Poll Mini-C5A RS-485 wind sensor once per second (9600 baud, slave 1).
- Send the usual 11-field station packet once per 600 seconds.
- Report mean speed, speed-weighted circular direction, and largest rolling three-sample mean as wind gust.
- Preserve packet schema for downstream ESP32 / gateway / WeeWX software.
- Log sensor sample count and warn if coverage <90%.\n- Deliver completed intervals only on UTC 10-minute epoch boundaries; discard the first incomplete interval after startup.\n- Keep wind polling on a separate thread during rain, 1-Wire, TFT and ESP32 operations; serialize all Modbus operations with one lock.

## Bench test (before deployment)\nRun `python3 -m unittest discover -s tests -p 'test_wind_timing.py' -v` from the repository root. This covers boundary math and continued polling during delayed consumption, not hardware performance. Verify clock synchronization before evaluating UTC timestamps; a significant clock step causes the current partial interval to be discarded. A full 600-second interval is required after the first UTC boundary.\n\n## Full-program simulation on station2 (no hardware access)
From an experimental worktree on the feature branch, run:

```bash
python3 -m py_compile station/station_wind_gust.py station/wind_timing.py
python3 station/station_wind_gust.py --simulate \
    --sim-output /tmp/wx-wind-sim-packets.txt --sim-count 2 \
    --interval 10 --wind-sample-interval 1 --log-level INFO
cat /tmp/wx-wind-sim-packets.txt
```

The simulation uses deterministic wind/rain, 1-Wire temperatures and battery readings. It **never opens the real UART**, so `emulate_station.py` can continue running. The first partial UTC interval is discarded, and two full 10-second intervals are reported to a local packet file. Ten seconds is exclusively a fast test interval, not the intended production interval (600 seconds). Check that each packet has 11 comma-separated fields, valid CRC16, and a UTC epoch divisible by 10. The simulated speed spikes are periodic, so gusts vary by interval. This tests main-program integration, not RS-485/LoRa delivery or real sensor performance.

## Site procedure (March 2027)
1. Back up the **actual running** station Python script and service configuration, plus relevant configuration/environment. Compare the live station script with both `station/station.py` on `main` and this candidate: the repository version may be older than the deployed code. In particular, check INA226 shunt calibration, GPIO/UART settings, sensor ID mapping, GPS/time, TFT, and 10-minute reporting interval.
2. Do **not** blindly overwrite the deployed `station.py`. Reconcile differences, and verify the candidate can import all dependencies.
3. Stop the existing station service before launching the candidate: only one program can safely access the RS-485 and ESP32 UARTs.
4. Bench/site test the candidate with `--interval 60 --wind-sample-interval 1 --log-level DEBUG` first, then use `--interval 600 --wind-sample-interval 1` for a full 10-minute cycle. Verify readings and sample counts in logs; 60s mode is for diagnostics only.
5. Verify the Mini-C5A's actual update/averaging behavior; the largest moving average of 1 Hz samples is **only an approximation** of a meteorological three-second gust, not guaranteed capture of the true maximum.
6. Confirm that weather packets reach the ESP32, midpoint, gateway, and WeeWX, and that transmitted time stamps are valid.
7. Compare gust and mean readings with a trusted reference where possible. Check station CPU/power and reliability over multiple cycles.
8. Restore the original script/service immediately if failures occur. Merge only after successful testing.

## Known caution
The script is based on an August 2026 `station.py` saved in the ChatGPT Library. It is *not* verified against the March 2027 site configuration. Its existing hardware configuration should be reviewed before any launch. Scheduling now uses UTC epoch boundaries and an independent wind polling thread. On startup, the first partial interval is discarded; the first transmission may therefore be 10–20 minutes after startup. Rain Modbus reads still temporarily hold the shared RS-485 lock and can delay a wind poll. The queue holds at most four completed intervals; prolonged downstream processing can overflow it, and GPS/NTP clock steps must be tested. The code has not been validated against the live field station.

## Branch
`feature/wind-gust-1hz-march-2027` — keep separate from production `main` until verified.

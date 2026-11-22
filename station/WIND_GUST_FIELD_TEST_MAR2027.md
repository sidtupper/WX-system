# March 2027 field test — 1 Hz wind gust sampling

**Status:** EXPERIMENTAL / NOT DEPLOYED. Created 2026-11-22. Do not merge into `main` until tested on the station Raspberry Pi.

## Files
- `station/station.py` — existing version preserved from `main` (not replaced).
- `station/station_wind_gust.py` — candidate version for field testing.

## Intended changes
- Poll Mini-C5A RS-485 wind sensor once per second (9600 baud, slave 1).
- Send the usual 11-field station packet once per 600 seconds.
- Report mean speed, speed-weighted circular direction, and largest rolling three-sample mean as wind gust.
- Preserve packet schema for downstream ESP32 / gateway / WeeWX software.
- Log sensor sample count and warn if coverage <90%.

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
The script is based on an August 2026 `station.py` saved in the ChatGPT Library. It is *not* verified against the March 2027 site configuration. Its existing hardware configuration should be reviewed before any launch. Scheduling of the 10-minute reporting cycle is monotonic from program start, not aligned to absolute wall-clock 10-minute boundaries. During the brief once-per-cycle rain/temperature/display/transmit operations, a few wind polls may be skipped.

## Branch
`feature/wind-gust-1hz-march-2027` — keep separate from production `main` until verified.

# WX-system — November 2026 working baseline

**Baseline tag:** `wx-2026-11-working-baseline`  
**Git commit:** `43ba622`  
**Repository:** `sidtupper/WX-system`  
**Snapshot prepared:** November 22, 2026  
**Purpose:** Preserve the verified bench-test configuration for restoration and March 2027 field deployment.

> **Scope of verification:** The complete communication and reporting chain was demonstrated using a **station Pi emulator**, not the actual field sensors. This is a working integration baseline, **not** certification that the unattended remote installation is production-ready. Do not replace unverified field software solely on the strength of this document.

## 1. System topology

```text
station2 Pi 4B / emulate_station.py
   -- 9600-baud UART3 (GPIO4 TX, GPIO5 RX) --> station ESP32
   -- E22 LoRa --> midpoint ESP32 (relay + battery reading)
   -- E22 LoRa --> gateway ESP32
   -- 9600-baud UART (/dev/serial0 on gateway Pi) --> gateway Pi
   -- gateway.py --> /var/tmp/gateway_latest.json
   -- GatewayJson WeeWX driver --> WeeWX SQLite database
   -- Seasons report --> Apache /weather/
```

## 2. Proven results

- November 2026 end-to-end bench test: radio packets arrived at gateway Pi with **14 fields** and valid **CRC16-MODBUS**.
- `gateway.py` logged repeated successful packets, produced valid JSON, and reported **19 good packets / 0 bad packets** in a captured example.
- The JSON contained relay battery voltage near **3.33 V** and station battery **12.6 V** (the station voltage was dummy data).
- WeeWX **5.6.0** custom `GatewayJson` driver produced valid LOOP packets (METRICWX unit code 17) with temperature, humidity, barometer, wind speed, direction, and gust.
- WeeWX created archive rows in `archive/weewx.sdb` and generated `public_html/index.html` and graphs.
- Apache successfully served the reports at **`http://192.168.1.72/weather/`** on the bench LAN; IP is not a permanent addressing guarantee.
- Both `wx-gateway.service` and `weewx.service` were observed **enabled and active**, and logs showed weather reception, archiving, and report generation. A post-reboot verification was recommended; not established by these results.
- `station2` weather UART stopped working when emulator used `/dev/serial0` (GPIO14/15). Enabling `dtoverlay=uart3` and using `/dev/ttyAMA3` restored ESP32 packet reception on November 22.

## 3. Station-side UART (verified on station2)

| Purpose | Configuration |
|---|---|
| Station Pi TX to ESP32 | GPIO4 (TXD3) → ESP32 GPIO27 (RX) |
| Station Pi RX from ESP32 | GPIO5 (RXD3) ← ESP32 GPIO23 (TX) |
| Common reference | Pi GND ↔ ESP32 GND |
| Serial device | `/dev/ttyAMA3` |
| Format | 9600 baud, 8N1 |
| `/boot/firmware/config.txt` under `[all]` | `enable_uart=1` and `dtoverlay=uart3` |

**Do not substitute `/dev/serial0` on station2:** it was linked to `/dev/ttyS0`, routed to GPIO14/15.

Test command:

```bash
python ~/WX-system/tests/emulate_station.py --port /dev/ttyAMA3 --baud 9600 --interval 600
```

A shorter interval such as `--interval 30` is useful for bench diagnostics. Stop any competing process on the same serial device before testing.

## 4. Radio and packet format

- Radios: E22-900T30D; **fixed addressing**; channel **60**; radio UART **9600 baud**.
- Node addresses used in the verified three-node test: station `00:01`, midpoint `00:02`, gateway `00:03`.
- The station Pi sends **11 comma-separated values**; station ESP32 adds UV and ambient light; midpoint adds relay battery voltage; gateway receives **14 values**.

```text
@epoch,wind_speed,wind_direction,wind_gust,temperature,humidity,
barometric_pressure,rainfall,sea_temperature,cabinet_temperature,
station_battery_voltage,uv,al,relay_battery_voltage*CCCC\r\n
```

- `CCCC` is four hexadecimal digits of **CRC16-MODBUS**, polynomial `0xA001`, initialized to `0xFFFF`, calculated over the ASCII payload **without** `@` or `*CCCC`.
- The midpoint battery ADC calibration was adjusted during testing; example reading ~3.325 V versus ~3.30 V with a meter. Preserve the known-working ESP32 sketches from the tagged commit.

## 5. Gateway Pi deployment

| Element | Location / setting |
|---|---|
| Operating system | Debian 13 (Trixie) |
| Git checkout | `/home/pi/WX-system` |
| Gateway program | `/home/pi/WX-system/gateway/gateway.py` |
| Gateway Python venv | `/home/pi/env` |
| Gateway serial input | `/dev/serial0`, 9600 baud |
| Atomic JSON output | `/var/tmp/gateway_latest.json` |
| WeeWX | **5.6.0** in `/home/pi/weewx-venv` |
| WeeWX root | `/home/pi/weewx-data` |
| WeeWX user driver | `/home/pi/weewx-data/bin/user/gatewayjson.py` |
| WeeWX configuration | `/home/pi/weewx-data/weewx.conf` |
| WeeWX database | `/home/pi/weewx-data/archive/weewx.sdb` |
| Generated reports | `/home/pi/weewx-data/public_html` |
| Apache alias | `/weather` → `/home/pi/weewx-data/public_html` |
| Service units | `/etc/systemd/system/wx-gateway.service`; `/etc/systemd/system/weewx.service` |

**Tagged repository reference files:**

```text
gateway/config/weewx.conf
gateway/config/apache-weewx.conf
gateway/config/wx-gateway.service
gateway/config/weewx.service
gateway/requirements-gateway.txt
gateway/requirements-weewx.txt
gateway/gatewayjson.py
```

`gateway/config/weewx.conf` is a **sanitized GitHub copy** (unused default MySQL password replaced with a placeholder), not a byte-for-byte production configuration file.

### Configured settings in the baseline repository

```ini
[Station]
    station_type = GatewayJson

[GatewayJson]
    driver = user.gatewayjson
    json_file = /var/tmp/gateway_latest.json
    poll_interval = 2
    max_age = 300

[StdConvert]
    target_unit = METRICWX

[StdArchive]
    archive_interval = 600
    record_generation = software
```

**Verification boundary:** the available archive-record test demonstrated archiving, but the 600-second interval change had not been separately verified for robust one-packet-per-interval operation. The current driver generates LOOP packets; it does not emit station-owned archive records.

### Routine checks

```bash
systemctl status wx-gateway.service weewx.service apache2.service --no-pager
journalctl -u wx-gateway.service -n 20 --no-pager
journalctl -u weewx.service -n 20 --no-pager
cat /var/tmp/gateway_latest.json
ls -lh /home/pi/weewx-data/archive/weewx.sdb
ls -l /home/pi/weewx-data/public_html/index.html
```

**Gateway clock:** NTP synchronized; `America/Vancouver` showed UTC−7 (`MST` abbreviation) in the November 2026 test. Unix epoch timestamps are independent of local zone abbreviations.

## 6. Important limitations and March 2027 work

**Not yet demonstrated as complete:**

1. The actual remote station's current `station.py` and GPS time discipline; back up and compare field files before replacing anything.
2. Wind gust sampling at a frequency sufficient to capture peak gusts; design a single-owner RS-485 polling loop or coordinated thread, and compute interval wind statistics.
3. Station-derived, **10-minute boundary-aligned archive records**, with defined delay and robust handling of missing records. Simply setting `archive_interval = 600` does not prove exact alignment.
4. Semantics of rainfall (`rainfall` incremental vs cumulative). Baseline `GatewayJson` deliberately omits rain from LOOP packets until resolved.
5. WeeWX archive schema and display support for sea temperature, cabinet temperature, UV, light level, and battery voltages.
6. Reliable history/replay for packets missed during gateway or RF outages. `gateway_latest.json` only holds the latest accepted observation.
7. Final midpoint energy budget, charging safety and sensor enclosure moisture mitigation at the site.
8. End-to-end field operation and service restart after a real power interruption.

## 7. Preservation / deployment rules

- Treat tag `wx-2026-11-working-baseline` as **immutable**; develop on `march-2027-development` and merge only after validation.
- Back up the field station Pi and installed ESP32 firmware/configuration before replacing or reflashing.
- Keep secrets out of the public repo, even in historical commits; review all config diffs before pushing.
- Record actual station and gateway Git commit IDs, ESP32 firmware hashes, radio configurations, and software test results at the March visit.
- Keep separate offline backups of `weewx.sdb`, custom WeeWX skins, and Pi configuration; do not place the live database or private keys in public GitHub.

## 8. Git reference verification

```bash
cd ~/WX-system
git fetch --tags origin
git show --no-patch wx-2026-11-working-baseline
git status --short
```

**Status wording:** “Verified” means observed in bench testing; “Configured” means present in the saved repository file; “Proposed” means not yet implemented or tested.

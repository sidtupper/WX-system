# WX-system — March 2027 field deployment plan

**Prepared:** 22 November 2026  
**Project:** West Howe Sound Weather Station  
**Repository:** `sidtupper/WX-system`  
**Verified fallback:** `wx-2026-11-working-baseline` (commit `43ba622`)  
**Development branch:** `march-2027-development`

> **Deployment rule:** The tagged November baseline was verified using a **station emulator**, not the real field sensors. Do not overwrite the operational station Pi or reflash hard-to-reach ESP32 nodes until their actual software, radio settings, and wiring have been inventoried and backed up. The site may have limited network access. Download all required files and tools before departure.

## 1. Mission and acceptance criteria

Goal: Update the field station while retaining a recoverable working configuration, and verify weather observations through the station → LoRa midpoint → gateway → WeeWX → Apache website chain.

**Required before declaring success:**

- [ ] The station Pi reads its installed wind, rain, environment, sea/cabinet temperature, battery, and GPS hardware, or each unavailable device is explicitly noted.
- [ ] Station-to-ESP32 UART and three-node LoRa delivery pass with valid 14-field packets and CRC16-MODBUS.
- [ ] Station timestamps are credible UTC Unix epochs; clock discipline works without Internet.
- [ ] Gateway JSON updates from **real station data**, with valid station and midpoint battery values.
- [ ] WeeWX stores observations at the intended cadence **without duplicate or lost archive intervals**; rain totals and optional fields are validated before enabling them.
- [ ] Apache report loads on the local network and shows new observations.
- [ ] Services recover after a controlled reboot and after temporary radio disconnection.
- [ ] Field changes, test results, and the final commit/firmware identifiers are documented.

## 2. Before departure — prepare while in town

### Software and documentation

- [ ] Finish and bench-test planned wind polling, interval timestamping, and WeeWX archiving changes **on `march-2027-development`**, without disturbing the tagged baseline.
- [ ] Do not deploy a proposed change until a specific test demonstrates it works. It is acceptable to deploy the November baseline unchanged and defer unfinished improvements.
- [ ] Keep the complete repository available **offline** (local clone or archive), including the baseline tag and relevant development commit.
- [ ] Save this deployment guide, `docs/NOVEMBER_2026_BASELINE.md`, and `docs/HARDWARE_CONFIGURATION.md` locally.
- [ ] Prepare known-good ESP32 `.ino` files **and compiled firmware images** for the exact ESP32 board type; record ESP32 core and library versions. Verify the three node roles and E22 settings match the hardware.
- [ ] Prepare Raspberry Pi OS package/venv requirements in case the site cannot reach the Internet. Python requirements are in `gateway/requirements-*.txt`; the station's dependencies must also be inventoried.
- [ ] Carry USB serial adapters, ESP32 USB cable, laptop with Arduino IDE/esptool, spare SD card, card reader, multimeter, and appropriate antenna/connector spares.
- [ ] Print/save station and midpoint pin mappings and TinyCAD schematics; verify them against the actual PCB revisions.
- [ ] Ensure a tested way to transfer files between the Pi and laptop without Internet (USB drive, direct LAN, or local Wi-Fi).

### Bench verification before travel

- [ ] Confirm `station2` sends on `/dev/ttyAMA3` (GPIO4/5), **not** `/dev/serial0`, at 9600 8N1.
- [ ] Run `emulate_station.py --port /dev/ttyAMA3 --interval 600`, check E22 forwarding and `gateway_latest.json` updates.
- [ ] Check the gateway systemd services and web page on the local network.
- [ ] Test any new 10-minute archive design across several intervals, including restart close to a boundary, a missing packet, and late delivery. A working 30-second bench test alone is not sufficient.
- [ ] Determine rain sensor semantics before enabling the `rain` field in WeeWX; baseline `gatewayjson.py` intentionally excludes it.

## 3. Site arrival — inspect and record *before changing anything*

Capture physical condition, battery voltages, solar charge state, antenna orientation, water ingress/condensation, and sensor connections. Take photographs of the mast, station enclosure, midpoint enclosure, and cable routing. **Do not disturb power wiring during a transmission or charge event unless safe to do so.**

For each Raspberry Pi:

```bash
hostname
uname -a
cat /etc/os-release
hostname -I
date -u '+%Y-%m-%d %H:%M:%S UTC'
timedatectl
lsblk -f
systemctl --failed --no-pager
```

Record the OS version, clock accuracy, mount/boot state, storage free space (`df -h`), and available interfaces. Compare Pi names and pin mappings: the `station2` bench Pi is **not** necessarily identical to the installed remote station Pi.

**Record the software actually running:**

```bash
ps -ef | grep -E '[s]tation.py|[g]ateway.py|[w]eewxd'
systemctl list-unit-files --type=service | grep -Ei 'wx|station|gps|weewx'
find /home/pi -maxdepth 3 -type f \( -name 'station.py' -o -name '*.ino' \) 2>/dev/null
```

If a local Git checkout exists, record its branch, commit and dirty files. **Do not run `git reset --hard` or `git clean -fd` on a field device.**

```bash
cd ~/WX-system 2>/dev/null && git status --short && git branch --show-current && git rev-parse HEAD
```

## 4. Back up field software and configuration

The known-good field station `station.py` may differ from any copy in GitHub. Back it up **before** updating it. Prefer a dated USB-drive copy or complete SD-card image; retain at least one backup off the Pi.

Example (change paths to match what you actually find):

```bash
mkdir -p ~/wx-field-backup-2027-03
cp -a /boot/firmware/config.txt ~/wx-field-backup-2027-03/
cp -a /etc/systemd/system ~/wx-field-backup-2027-03/systemd-system
# After locating the active station project:
# cp -a /home/pi/wx ~/wx-field-backup-2027-03/wx
# cp -a /home/pi/WX-system ~/wx-field-backup-2027-03/WX-system
```

Also save:

- [ ] The running `station.py`, its configuration, service units, Python environment package list, logs, CSV/weather history, GPS setup, `/boot/firmware/config.txt` and any relevant `cmdline.txt` settings.
- [ ] Firmware source or recorded flashed build/version for each ESP32; radio configuration dumps, if practical.
- [ ] If modifying the gateway, copy live `/home/pi/weewx-data/weewx.conf`, `/home/pi/weewx-data/bin/user`, `/home/pi/weewx-data/skins` customizations, database `archive/weewx.sdb`, Apache config, and service files. **Copy the database with services stopped or use SQLite's backup API** to avoid inconsistent backups.
- [ ] No passwords, SSH private keys, or live weather databases in the public GitHub repository.

Validate the backup exists and can be read. Copy it off the Pi before destructive work.

## 5. Verify station Pi hardware and UART

**Bench-verified `station2` mapping** (confirm against field wiring before applying):

| Signal | Raspberry Pi 4B | ESP32-station |
|---|---|---|
| TX to ESP32 | GPIO4 / TXD3 | GPIO27 / RX |
| RX from ESP32 | GPIO5 / RXD3 | GPIO23 / TX |
| Ground | GND | GND |
| UART | `/dev/ttyAMA3`, 9600 8N1 | `Serial1` |

On station2 the correct boot overlay was:

```ini
[all]
enable_uart=1
dtoverlay=uart3
```

Check the actual field system first:

```bash
ls -l /dev/serial0 /dev/ttyAMA3 2>&1
pinctrl get 4
pinctrl get 5
grep -nE '^\[|dtoverlay|enable_uart' /boot/firmware/config.txt
```

For the expected UART3 wiring, GPIO4 should report TXD3 and GPIO5 RXD3. **Do not change field `config.txt` simply to match station2 if field UART assignments or hardware differ.** Test a short dummy transmission only after ensuring production `station.py` is stopped and no other process owns that UART.

Sensors to verify against field software and schematics:

- Wind Mini-C5A, RS-485 slave 1, 9600 baud; registers for speed, direction, temperature, humidity and pressure.
- Rain SN-3001-GYL-N01, RS-485 slave 2, 9600 baud, with confirmed register setup; distinguish interval rain from running accumulation.
- RS-485 on Pi UART5 (`/dev/ttyAMA5`, GPIO12/13) in the planned station configuration.
- BME280 I²C address `0x76`, INA226 `0x40`, and sea/cabinet DS18B20 devices via 1-Wire GPIO27 (confirm IDs in field).
- GPS receiver/antenna and a time-sync service; do not assume it is synchronized just because the emulator uses `time.time()`.

## 6. Verify ESP32 and E22 network before reflashing

Working bench radio configuration: **E22-900T30D, fixed addressing, channel 60, 9600-baud UART**. Station address `00:01`, midpoint `00:02`, gateway `00:03`.

- [ ] Check supply voltage to each E22 under transmission load; inspect antenna and connector before powering any transmitter.
- [ ] Compare installed firmware with repository source, pin mappings, E22 addressing/network/channel/air-rate settings and expected packet forwarding.
- [ ] Preserve the known-good field radio configuration; only reflash nodes whose intended replacement firmware has been tested.
- [ ] Ensure station sends 11 fields, station ESP32 appends UV/ALS, midpoint appends its battery, gateway receives 14 fields plus CRC.
- [ ] Verify CRC16-MODBUS over the ASCII payload excluding the leading `@`; verify decoded field order.
- [ ] Midpoint: check 1S LiFePO4 battery, solar charging, cold-charge inhibit, deep-sleep/wake behavior, and battery ADC calibration (~3.3 V test reading). Earlier power-budget tests showed the original panel/battery combination was insufficient at a 30-second test rate.
- [ ] Check condensation at the upward-facing UV/quartz window and ensure electronics are protected from drips before returning the enclosure to service.

## 7. Production sampling and time-alignment development (not yet verified)

Do not silently replace `station.py` with an emulator. Implement these only after bench validation and retain a rollback build.

**Proposed behavior:** one controlled owner of the RS-485 bus polls wind roughly every **5 seconds** (adjust after measuring sensor response), while less rapidly changing sensors may be polled every ~60 seconds. Collect statistics over exact **600-second UTC epoch boundaries**. Transmit after the boundary, keeping the **end-of-interval epoch** in the packet. Avoid using repeated `sleep(600)` as the timing reference.

- [ ] Wind speed: define whether `wind_speed` is scalar average; document sample validity requirements.
- [ ] Direction: use speed-weighted vector mean and handle 359°/1° wraparound.
- [ ] Gust: max observed sample (not necessarily a standard 3-second gust if the sensor averages internally); determine the Mini-C5A averaging/output characteristics.
- [ ] Ensure the shared RS-485 bus is accessed serially (one polling thread/owner).
- [ ] Check clock discipline from GPS at the field station; handle missing GPS fix and clock jumps.
- [ ] Define a minimum valid sample count and missing-data behavior.
- [ ] Simulate boundary restart, bad sensor value, delayed LoRa send, midnight rollover, and loss of two consecutive packets.

**WeeWX synchronization:** exact wall-clock execution on both Pis is unnecessary. Timestamp each completed station interval at its UTC end boundary; make WeeWX archive that interval exactly once even if RF delivery is several seconds late. The current `GatewayJson` driver issues LOOP packets and WeeWX performs software archiving; simply setting `archive_interval = 600` does **not** guarantee correct placement of sparse packets. A robust record-generation approach and replay/backfill handling need implementation and testing.

## 8. Gateway baseline configuration and smoke tests

The tagged gateway uses Debian 13 (Trixie), WeeWX **5.6.0**, `/home/pi/env` for `gateway.py`, `/home/pi/weewx-venv` for WeeWX, `/dev/serial0` at 9600 baud, `/var/tmp/gateway_latest.json` and Apache alias `/weather`.

Important saved configuration paths:

```text
gateway/config/wx-gateway.service     -> /etc/systemd/system/wx-gateway.service
gateway/config/weewx.service         -> /etc/systemd/system/weewx.service
gateway/config/apache-weewx.conf     -> /etc/apache2/conf-available/weewx.conf
gateway/config/weewx.conf            -> /home/pi/weewx-data/weewx.conf (sanitized template!)
gateway/gatewayjson.py               -> /home/pi/weewx-data/bin/user/gatewayjson.py
```

**Never blindly overwrite the live WeeWX configuration with the sanitized GitHub copy.** Merge intended settings and preserve the working local database, station location, active units, and any private credentials. Verify `target_unit = METRICWX` before creating a new database; changing that setting for an existing database can corrupt unit consistency.

Smoke tests:

```bash
systemctl is-enabled wx-gateway.service weewx.service apache2.service
systemctl is-active wx-gateway.service weewx.service apache2.service
journalctl -u wx-gateway.service -n 30 --no-pager
journalctl -u weewx.service -n 30 --no-pager
cat /var/tmp/gateway_latest.json
ls -l /home/pi/weewx-data/public_html/index.html
```

Use the current LAN address, not the bench-only `192.168.1.72`, to visit `http://GATEWAY_IP/weather/`. Confirm reports update after the next *real* archive record. Inspect the SQLite archive for duplicates and timing:

```bash
python3 - <<'PY'
import sqlite3
from datetime import datetime, timezone
p='/home/pi/weewx-data/archive/weewx.sdb'
with sqlite3.connect(p) as db:
    for ts, minutes, temp, wind in db.execute(
        'SELECT dateTime, interval, outTemp, windSpeed FROM archive '
        'ORDER BY dateTime DESC LIMIT 10'):
        print(datetime.fromtimestamp(ts, timezone.utc), minutes, temp, wind)
PY
```

The tagged configuration sets `archive_interval = 600`, `record_generation = software`, `poll_interval = 2` and `max_age = 300`. The driver's archive handling at 10-minute station cadence remains an **unresolved verification item**.

## 9. Update procedure and rollback

1. **Inventory → back up → verify the backup → compare versions → update one node at a time → verify → record results.** Do not change all nodes at once.
2. For each Pi, review `git status`, verify the target branch/commit, and save local modifications. Avoid force pulls, hard resets or repository deletion.
3. Reflash an ESP32 only with the correct hardware-target build; save the installed sketch/config and check UART and E22 settings before reconnecting the LoRa chain.
4. Keep the November baseline checkout or tagged source available as a rollback reference. Record any differences between the live field station and that baseline before considering rollback.
5. If a new station build fails, stop its service/process, restore the **backed-up live field files and service configuration**, and reboot/restart only the affected software. If a new gateway driver fails, restore its original `gatewayjson.py` and WeeWX configuration, then restart `weewx.service`.
6. After recovery, re-run end-to-end checks; avoid leaving the site with an unverified change.

For a Git checkout on a **separate test directory** (without disturbing the working tree):

```bash
git clone --branch wx-2026-11-working-baseline --depth 1 \
    https://github.com/sidtupper/WX-system.git WX-system-baseline-reference
```

This requires Internet access; create the reference checkout ahead of the visit if site access is limited.

## 10. Field acceptance log (complete during visit)

| Item | Actual value / result | Pass / needs work |
|---|---|---|
| Date/time, site and operator | | |
| Station Pi OS and backup location | | |
| Station software commit/hash | | |
| GPS / system UTC accuracy | | |
| Station sensor readings | | |
| ESP32-station firmware/version | | |
| ESP32-midpoint firmware/version | | |
| ESP32-gateway firmware/version | | |
| E22 configuration/channel/address | | |
| Station and midpoint battery readings | | |
| 14-field packets and CRC | | |
| Interval timestamps and missing-record test | | |
| Gateway JSON updates | | |
| WeeWX archive interval and records | | |
| Apache weather page | | |
| Boot/restart and recovery test | | |
| Solar, enclosure and antenna inspection | | |
| Follow-up issues created | | |

## 11. At the end of the visit

- [ ] Preserve updated production source, sanitized configuration changes, schematic corrections and test logs in GitHub, using a new descriptive commit or field release tag **only after verification**.
- [ ] Record unresolved tasks as GitHub Issues tagged `March 2027` or in a dated follow-up checklist.
- [ ] Retain an offline backup of the actual field Pi state and weather data.
- [ ] Do not move or retarget the immutable tag `wx-2026-11-working-baseline`.

**Related documentation:** `docs/NOVEMBER_2026_BASELINE.md`, `docs/HARDWARE_CONFIGURATION.md`, gateway config files, TinyCAD hardware files, and the `march-2027-development` branch.

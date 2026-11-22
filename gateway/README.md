# WX-system — Raspberry Pi Gateway

## Configuration snapshot: November 21, 2026

This directory contains the software and configuration files for the Raspberry Pi gateway of the West Howe Sound Weather Station.

The gateway receives weather data transmitted over LoRa from the remote station through an intermediate relay, validates the data, and makes it available to WeeWX for archiving and web reporting.

## 1. System architecture

Data flows through the following components:

```text
Remote weather station
    |
    | Raspberry Pi 4B
    | station.py
    |
    v
Station ESP32 + E22 LoRa
    |
    | LoRa
    v
Midpoint ESP32 + E22 LoRa
    |
    | LoRa
    v
Gateway ESP32 + E22 LoRa
    |
    | UART, 9600 baud
    v
Gateway Raspberry Pi 4B
    |
    | gateway.py
    v
/var/tmp/gateway_latest.json
    |
    | gatewayjson.py
    v
WeeWX 5.x
    |
    v
HTML weather reports
    |
    v
Apache web server
```

## 2. Gateway hardware

- Raspberry Pi 4B
- ESP32 gateway controller
- E22-900T30D LoRa radio
- UART connection between ESP32 and Raspberry Pi

### UART configuration

| Signal | Raspberry Pi | ESP32 |
|---|---|---|
| Pi TX → ESP32 RX | GPIO14 | GPIO22 |
| Pi RX ← ESP32 TX | GPIO15 | GPIO23 |
| Ground | GND | GND |

Serial configuration:

- Device: `/dev/serial0`
- Baud rate: 9600
- Data bits: 8
- Parity: None
- Stop bits: 1

The gateway ESP32 forwards received LoRa packets to the Raspberry Pi.

## 3. Operating environment

The gateway uses Raspberry Pi OS based on Debian 13 (Trixie).

The principal software components are:

- Python 3.13
- WeeWX 5.x
- Apache 2.4
- pyserial
- systemd

Python environments:

```text
/home/pi/env
/home/pi/weewx-venv
```

Installed WeeWX data directory:

```text
/home/pi/weewx-data
```

Exact installed package versions should be recorded separately during system verification.

## 4. Gateway Python software

### gateway.py

Location:

```text
/home/pi/WX-system/gateway/gateway.py
```

Responsibilities:

1. Receive serial packets from the gateway ESP32.
2. Validate CRC16-MODBUS checksums.
3. Parse weather observations.
4. Reject malformed packets.
5. Detect abnormal sensor readings and battery conditions.
6. Write the most recent valid observation to a JSON file.

Output:

```text
/var/tmp/gateway_latest.json
```

Default serial configuration:

```text
/dev/serial0
9600 baud
```

The gateway service uses:

```text
/home/pi/env/bin/python
```

### gatewayjson.py

Repository location:

```text
gateway/gatewayjson.py
```

Installed WeeWX driver location:

```text
/home/pi/weewx-data/bin/user/gatewayjson.py
```

This custom WeeWX driver reads weather observations from:

```text
/var/tmp/gateway_latest.json
```

It converts the observations into WeeWX loop packets.

The driver does not access the serial port directly.

Rainfall is currently omitted from the driver until the measurement's interpretation is verified.

## 5. Weather packet format

Packets contain 14 comma-separated fields:

```text
@epoch,wind_speed,wind_direction,wind_gust,
temperature,humidity,barometric_pressure,
rainfall,sea_temperature,cabinet_temperature,
station_battery_voltage,uv,al,
relay_battery_voltage*CRC
```

The CRC is CRC16-MODBUS, calculated over the characters between `@` and `*`.

Polynomial:

```text
0xA001
```

Initial CRC value:

```text
0xFFFF
```

The normal station reporting interval is 10 minutes.

## 6. WeeWX configuration

Configuration file:

```text
/home/pi/weewx-data/weewx.conf
```

Repository copy:

```text
gateway/weewx/weewx.conf
```

Station driver:

```text
GatewayJson
```

Enabled HTML report:

```text
SeasonsReport
```

Other standard reports are disabled.

WeeWX receives new observations from the JSON driver and generates reports using the Seasons skin.

The repository configuration contains placeholder passwords. Actual database credentials must be configured locally and must not be committed to GitHub.

## 7. Apache configuration

Configuration:

```text
/etc/apache2/sites-available/000-default.conf
```

Repository copy:

```text
gateway/apache/000-default.conf
```

The saved Apache configuration uses:

```text
DocumentRoot /var/www/html
```

WeeWX is configured to generate reports under:

```text
/home/pi/weewx-data/public_html
```

Apache serves the WeeWX-generated weather reports directly from:

`/home/pi/weewx-data/public_html`

The Apache virtual host configuration specifies:

```apache
DocumentRoot /home/pi/weewx-data/public_html

<Directory /home/pi/weewx-data/public_html>
    Options FollowSymLinks
    AllowOverride None
    Require all granted
</Directory>
```

This configuration was verified on November 21, 2026. A request to `http://localhost/` returned exactly the same number of bytes as the WeeWX-generated `index.html` (17,863 bytes).

Apache serves the reports directly; no periodic file copying or symbolic link is required.
## 8. systemd services

Service definitions are preserved in:

```text
gateway/systemd/
```

### wx-gateway.service

Runs:

```text
/home/pi/WX-system/gateway/gateway.py
```

Uses the `/home/pi/env` Python environment.

### weewx.service

Runs:

```text
/home/pi/weewx-venv/bin/weewxd
```

Uses:

```text
/home/pi/weewx-data/weewx.conf
```

Configured to start after and request `wx-gateway.service`.

### gps_logger.service

Runs:

```text
/home/pi/gps_reader.py
```

The GPS reader script must also be preserved if this service is required.

Its use of serial hardware should be checked against the gateway UART configuration.

## 9. Diagnostic commands

Check service status:

```bash
systemctl status wx-gateway.service
systemctl status weewx.service
systemctl status apache2.service
```

View gateway logs:

```bash
journalctl -u wx-gateway.service -n 50
```

View WeeWX logs:

```bash
journalctl -u weewx.service -n 50
```

Check the latest weather observation:

```bash
cat /var/tmp/gateway_latest.json
```

Check generated HTML:

```bash
ls -lh /home/pi/weewx-data/public_html/index.html
```

Check the UART:

```bash
ls -l /dev/serial0
```

## 10. Restoring the gateway

A fresh gateway installation requires:

1. Install a compatible Raspberry Pi operating system.
2. Configure the Raspberry Pi UART.
3. Install Python and the required libraries.
4. Create the Python environments.
5. Install WeeWX.
6. Install Apache.
7. Clone the WX-system repository.
8. Install the custom WeeWX driver.
9. Copy and configure the WeeWX configuration.
10. Restore the systemd service definitions.
11. Configure the Apache document root.
12. Enable and start the services.
13. Verify serial reception, JSON output, WeeWX archiving, and HTML reporting.

This section is a restoration outline, not yet a fully tested installation script.

## 11. Development notes

The November 2026 gateway was developed and tested using simulated weather packets.

The remote weather station normally reports every 10 minutes.

Outstanding work includes:

- Verifying operation with real weather station data.
- Confirming GPS time synchronization.
- Confirming rainfall interpretation.
- Testing long-term unattended operation.
- Recording exact installed software versions.
- Completing and testing a fresh-install restoration procedure.

The next planned opportunity to update the remote station hardware is March 2027.

## 12. Version control

Repository:

`sidtupper/WX-system`

Working gateway configuration committed November 21, 2026:

```text
689fd0f
```

The goal is to preserve a reproducible gateway installation and minimize configuration work when the remote station is next accessible.

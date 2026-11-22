#!/usr/bin/env python3
"""
gateway.py

Read weather packets from esp32_gateway over serial, verify CRC16-MODBUS,
parse fields, detect anomalies, optionally send email alarms, and write the
latest good packet to a JSON file for downstream consumers such as a weeWX driver.

Expected packet format:

@epoch,wind_speed,wind_direction,wind_gust,temperature,humidity,barometric_pressure,
rainfall,sea_temperature,cabinet_temperature,station_battery_voltage,uv,al,
relay_battery_voltage*CRC\r\n

Where CRC is 4 hex digits of CRC16-MODBUS computed over the payload only
(the text between '@' and '*').

Example:
@1713800000,1.2,180,2.1,11.4,84,1013.2,0.0,8.7,15.2,12.6,123,456,12.1*ABCD
"""

from __future__ import annotations

import argparse
import json
import logging
import os
import smtplib
import sys
import time
from dataclasses import asdict, dataclass
from email.message import EmailMessage
from pathlib import Path
from typing import Any

try:
    import serial  # pyserial
    from serial import SerialException
except ImportError:
    print("Missing dependency: pyserial. Install with: pip install pyserial", file=sys.stderr)
    raise


# -----------------------------
# Packet model
# -----------------------------

FIELD_NAMES = [
    "epoch",
    "wind_speed",
    "wind_direction",
    "wind_gust",
    "temperature",
    "humidity",
    "barometric_pressure",
    "rainfall",
    "sea_temperature",
    "cabinet_temperature",
    "station_battery_voltage",
    "uv",
    "al",
    "relay_battery_voltage",
]


@dataclass
class WeatherPacket:
    epoch: int
    wind_speed: float
    wind_direction: float
    wind_gust: float
    temperature: float
    humidity: float
    barometric_pressure: float
    rainfall: float
    sea_temperature: float
    cabinet_temperature: float
    station_battery_voltage: float
    uv: float
    al: float
    relay_battery_voltage: float

    @classmethod
    def from_payload(cls, payload: str) -> "WeatherPacket":
        parts = payload.split(",")
        if len(parts) != len(FIELD_NAMES):
            raise ValueError(
                f"Expected {len(FIELD_NAMES)} fields, got {len(parts)}. Payload={payload!r}"
            )

        try:
            values: dict[str, Any] = {
                "epoch": int(parts[0]),
                "wind_speed": float(parts[1]),
                "wind_direction": float(parts[2]),
                "wind_gust": float(parts[3]),
                "temperature": float(parts[4]),
                "humidity": float(parts[5]),
                "barometric_pressure": float(parts[6]),
                "rainfall": float(parts[7]),
                "sea_temperature": float(parts[8]),
                "cabinet_temperature": float(parts[9]),
                "station_battery_voltage": float(parts[10]),
                "uv": float(parts[11]),
                "al": float(parts[12]),
                "relay_battery_voltage": float(parts[13]),
            }
        except ValueError as exc:
            raise ValueError(f"Failed numeric conversion: {exc}") from exc

        return cls(**values)


# -----------------------------
# CRC16-MODBUS
# -----------------------------

def crc16_modbus(data: bytes) -> int:
    crc = 0xFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            if crc & 1:
                crc = (crc >> 1) ^ 0xA001
            else:
                crc >>= 1
    return crc & 0xFFFF


# -----------------------------
# Packet parsing
# -----------------------------

def parse_packet_line(line: str) -> tuple[str, WeatherPacket]:
    """
    Parse one line of text in the form @payload*ABCD
    Returns (payload, WeatherPacket) if valid.
    """
    text = line.strip()
    if not text:
        raise ValueError("Empty line")

    if not text.startswith("@"):
        raise ValueError(f"Missing '@': {text!r}")

    star = text.rfind("*")
    if star == -1:
        raise ValueError(f"Missing '*': {text!r}")

    payload = text[1:star]
    crc_text = text[star + 1 :]

    if len(crc_text) != 4:
        raise ValueError(f"CRC field must be 4 hex chars, got {crc_text!r}")

    try:
        rx_crc = int(crc_text, 16)
    except ValueError as exc:
        raise ValueError(f"CRC is not valid hex: {crc_text!r}") from exc

    calc_crc = crc16_modbus(payload.encode("ascii", errors="strict"))
    if rx_crc != calc_crc:
        raise ValueError(f"CRC mismatch: rx=0x{rx_crc:04X} calc=0x{calc_crc:04X}")

    packet = WeatherPacket.from_payload(payload)
    return payload, packet


# -----------------------------
# Atomic JSON write
# -----------------------------

def atomic_write_json(path: Path, obj: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(path.suffix + ".tmp")
    tmp.write_text(json.dumps(obj, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    os.replace(tmp, path)


# -----------------------------
# Alarming
# -----------------------------

@dataclass
class AlarmConfig:
    enabled: bool
    smtp_host: str
    smtp_port: int
    smtp_username: str
    smtp_password: str
    use_tls: bool
    from_addr: str
    to_addrs: list[str]
    reminder_seconds: int


class AlarmManager:
    def __init__(self, cfg: AlarmConfig, logger: logging.Logger) -> None:
        self.cfg = cfg
        self.logger = logger
        self.active_since: dict[str, float] = {}
        self.last_sent: dict[str, float] = {}

    def update(self, alarms: list[str], packet: WeatherPacket) -> None:
        now = time.time()
        current = set(alarms)
        previous = set(self.active_since)

        started = current - previous
        cleared = previous - current
        ongoing = current & previous

        for name in started:
            self.active_since[name] = now
            self._send_alarm(f"ALARM START: {name}", self._format_body(name, packet, state="START"))

        for name in ongoing:
            last = self.last_sent.get(name, 0.0)
            if now - last >= self.cfg.reminder_seconds:
                self._send_alarm(f"ALARM REMINDER: {name}", self._format_body(name, packet, state="REMINDER"))

        for name in cleared:
            self._send_alarm(f"ALARM CLEAR: {name}", self._format_body(name, packet, state="CLEAR"))
            self.active_since.pop(name, None)
            self.last_sent.pop(name, None)

    def _format_body(self, name: str, packet: WeatherPacket, state: str) -> str:
        return (
            f"State: {state}\n"
            f"Alarm: {name}\n"
            f"Epoch: {packet.epoch}\n"
            f"Station battery: {packet.station_battery_voltage:.2f} V\n"
            f"Relay battery: {packet.relay_battery_voltage:.2f} V\n"
            f"Temperature: {packet.temperature:.2f} C\n"
            f"Humidity: {packet.humidity:.1f} %\n"
            f"Wind speed: {packet.wind_speed:.2f}\n"
            f"Wind gust: {packet.wind_gust:.2f}\n"
        )

    def _send_alarm(self, subject: str, body: str) -> None:
        if not self.cfg.enabled:
            self.logger.warning("Alarm suppressed (email disabled): %s", subject)
            return

        msg = EmailMessage()
        msg["Subject"] = subject
        msg["From"] = self.cfg.from_addr
        msg["To"] = ", ".join(self.cfg.to_addrs)
        msg.set_content(body)

        try:
            with smtplib.SMTP(self.cfg.smtp_host, self.cfg.smtp_port, timeout=20) as smtp:
                if self.cfg.use_tls:
                    smtp.starttls()
                if self.cfg.smtp_username:
                    smtp.login(self.cfg.smtp_username, self.cfg.smtp_password)
                smtp.send_message(msg)

            self.last_sent[subject.split(": ", 1)[-1]] = time.time()
            self.logger.warning("Alarm email sent: %s", subject)
        except Exception:
            self.logger.exception("Failed to send alarm email: %s", subject)


# -----------------------------
# Anomaly detection
# -----------------------------

@dataclass
class Thresholds:
    low_station_battery: float = 11.8
    low_relay_battery: float = 3.0
    high_cabinet_temperature: float = 45.0
    low_cabinet_temperature: float = -10.0
    max_humidity: float = 100.0
    max_wind_speed: float = 100.0
    max_wind_gust: float = 120.0
    stale_packet_seconds: float = 1800.0  # 30 min


def detect_anomalies(packet: WeatherPacket, thresholds: Thresholds) -> list[str]:
    alarms: list[str] = []
    now = time.time()

    if packet.station_battery_voltage < thresholds.low_station_battery:
        alarms.append(f"station battery low ({packet.station_battery_voltage:.2f} V)")

    if packet.relay_battery_voltage < thresholds.low_relay_battery:
        alarms.append(f"relay battery low ({packet.relay_battery_voltage:.2f} V)")

    if packet.cabinet_temperature > thresholds.high_cabinet_temperature:
        alarms.append(f"cabinet temperature high ({packet.cabinet_temperature:.2f} C)")

    if packet.cabinet_temperature < thresholds.low_cabinet_temperature:
        alarms.append(f"cabinet temperature low ({packet.cabinet_temperature:.2f} C)")

    if not (0.0 <= packet.humidity <= thresholds.max_humidity):
        alarms.append(f"humidity out of range ({packet.humidity:.1f} %)")

    if not (0.0 <= packet.wind_speed <= thresholds.max_wind_speed):
        alarms.append(f"wind speed out of range ({packet.wind_speed:.2f})")

    if not (0.0 <= packet.wind_gust <= thresholds.max_wind_gust):
        alarms.append(f"wind gust out of range ({packet.wind_gust:.2f})")

    age = now - packet.epoch
    if age > thresholds.stale_packet_seconds:
        alarms.append(f"packet stale ({age:.0f} s old)")

    return alarms


# -----------------------------
# Logging
# -----------------------------

def configure_logging(log_level: str, log_file: str | None) -> logging.Logger:
    logger = logging.getLogger("gateway")
    logger.setLevel(getattr(logging, log_level.upper()))
    logger.handlers.clear()

    formatter = logging.Formatter(
        "%(asctime)s %(levelname)s %(name)s: %(message)s",
        "%Y-%m-%d %H:%M:%S",
    )

    sh = logging.StreamHandler(sys.stdout)
    sh.setFormatter(formatter)
    logger.addHandler(sh)

    if log_file:
        fh = logging.FileHandler(log_file)
        fh.setFormatter(formatter)
        logger.addHandler(fh)

    return logger


# -----------------------------
# Main loop
# -----------------------------

def build_serial(port: str, baud: int, timeout: float) -> serial.Serial:
    return serial.Serial(
        port=port,
        baudrate=baud,
        bytesize=serial.EIGHTBITS,
        parity=serial.PARITY_NONE,
        stopbits=serial.STOPBITS_ONE,
        timeout=timeout,
    )


def main() -> int:
    parser = argparse.ArgumentParser(description="Read, validate, and bridge weather packets from esp32_gateway.")
    parser.add_argument("--port", default="/dev/serial0", help="Serial device path")
    parser.add_argument("--baud", type=int, default=9600, help="Serial baud rate")
    parser.add_argument("--timeout", type=float, default=2.0, help="Serial read timeout in seconds")
    parser.add_argument("--latest-json", default="/var/tmp/gateway_latest.json", help="Path to latest good packet JSON")
    parser.add_argument("--log-file", default=None, help="Optional log file path")
    parser.add_argument("--log-level", default="INFO", choices=["DEBUG", "INFO", "WARNING", "ERROR"])
    parser.add_argument("--email-enable", action="store_true", help="Enable email alarms")
    parser.add_argument("--smtp-host", default="")
    parser.add_argument("--smtp-port", type=int, default=587)
    parser.add_argument("--smtp-username", default="")
    parser.add_argument("--smtp-password", default="")
    parser.add_argument("--smtp-no-tls", action="store_true")
    parser.add_argument("--from-addr", default="")
    parser.add_argument("--to-addr", action="append", default=[], help="Alarm recipient; may be given multiple times")
    parser.add_argument("--alarm-reminder-seconds", type=int, default=6 * 3600)
    parser.add_argument("--low-station-battery", type=float, default=11.8)
    parser.add_argument("--low-relay-battery", type=float, default=3.0)
    args = parser.parse_args()

    logger = configure_logging(args.log_level, args.log_file)
    thresholds = Thresholds(
        low_station_battery=args.low_station_battery,
        low_relay_battery=args.low_relay_battery,
    )

    alarm_cfg = AlarmConfig(
        enabled=args.email_enable,
        smtp_host=args.smtp_host,
        smtp_port=args.smtp_port,
        smtp_username=args.smtp_username,
        smtp_password=args.smtp_password,
        use_tls=not args.smtp_no_tls,
        from_addr=args.from_addr,
        to_addrs=args.to_addr,
        reminder_seconds=args.alarm_reminder_seconds,
    )
    alarms = AlarmManager(alarm_cfg, logger)

    latest_json = Path(args.latest_json)

    logger.info("Opening serial port %s at %d baud", args.port, args.baud)

    try:
        ser = build_serial(args.port, args.baud, args.timeout)
    except SerialException:
        logger.exception("Failed to open serial port")
        return 2

    bad_count = 0
    good_count = 0

    try:
        while True:
            try:
                raw = ser.readline()
            except SerialException:
                logger.exception("Serial read failure")
                time.sleep(2)
                continue

            if not raw:
                continue

            try:
                line = raw.decode("ascii", errors="strict")
            except UnicodeDecodeError:
                bad_count += 1
                logger.warning("Discarding non-ASCII line: %r", raw)
                continue

            try:
                payload, packet = parse_packet_line(line)
            except ValueError as exc:
                bad_count += 1
                logger.warning("Discarding bad packet: %s ; raw=%r", exc, line.rstrip("\r\n"))
                continue

            good_count += 1
            packet_dict = asdict(packet)
            packet_dict["received_at"] = time.time()
            packet_dict["raw_payload"] = payload
            packet_dict["raw_line"] = line.rstrip("\r\n")
            packet_dict["good_packets"] = good_count
            packet_dict["bad_packets"] = bad_count

            atomic_write_json(latest_json, packet_dict)

            current_alarms = detect_anomalies(packet, thresholds)
            alarms.update(current_alarms, packet)

            logger.info(
                "OK epoch=%d wind=%.2f gust=%.2f temp=%.2fC hum=%.1f%% press=%.2f "
                "rain=%.2f station_batt=%.2fV uv=%.2f al=%.2f relay_batt=%.2fV alarms=%d",
                packet.epoch,
                packet.wind_speed,
                packet.wind_gust,
                packet.temperature,
                packet.humidity,
                packet.barometric_pressure,
                packet.rainfall,
                packet.station_battery_voltage,
                packet.uv,
                packet.al,
                packet.relay_battery_voltage,
                len(current_alarms),
            )
    except KeyboardInterrupt:
        logger.info("Stopping on keyboard interrupt")
        return 0
    finally:
        ser.close()


if __name__ == "__main__":
    raise SystemExit(main())

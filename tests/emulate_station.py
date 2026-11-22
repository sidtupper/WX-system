#!/usr/bin/env python3
"""Emulate the WX station Pi sensor packet stream without any sensors.

Sends the station's 11-field ASCII packet to ESP32-station on UART3.
ESP32-station adds UV/AL and midpoint adds its battery voltage downstream.

Examples:
  python emulate_station.py --interval 30
  python emulate_station.py --interval 600
  python emulate_station.py --count 1
  python emulate_station.py --dry-run --count 3 --interval 2
"""

import argparse
import math
import time


def crc16_modbus(data: bytes) -> int:
    crc = 0xFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc & 0xFFFF


def make_packet(sequence: int) -> bytes:
    """Build 11 station fields; synthetic measurements vary gently over time."""
    phase = sequence / 5.0
    wind_speed = 5.2 + 0.7 * math.sin(phase)
    wind_direction = (270.0 + 15.0 * math.sin(phase / 2)) % 360
    wind_gust = wind_speed + 3.3
    temperature = 12.5 + 0.5 * math.sin(phase / 3)
    humidity = 75.0 + 2.0 * math.cos(phase / 4)
    pressure = 1013.2 + 0.8 * math.cos(phase / 5)
    fields = [
        str(int(time.time())),          # epoch
        f"{wind_speed:.2f}",           # wind speed
        f"{wind_direction:.1f}",       # wind direction
        f"{wind_gust:.2f}",            # wind gust
        f"{temperature:.2f}",          # air temperature (C)
        f"{humidity:.1f}",             # relative humidity (%)
        f"{pressure:.2f}",             # pressure (hPa)
        "0.0",                         # rainfall
        "11.8",                        # sea temperature (C)
        "15.2",                        # cabinet temperature (C)
        "12.6",                        # station battery voltage (V)
    ]
    body = ",".join(fields)
    return f"@{body}*{crc16_modbus(body.encode('ascii')):04X}\r\n".encode("ascii")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--port", default="/dev/serial0", help="Station Pi UART3 device")
    parser.add_argument("--baud", type=int, default=9600)
    parser.add_argument("--interval", type=float, default=30.0, help="Seconds between packets (default: 30)")
    parser.add_argument("--count", type=int, default=0, help="Packets to send; 0 = continuously")
    parser.add_argument("--dry-run", action="store_true", help="Print packets without opening serial port")
    args = parser.parse_args()
    if args.interval <= 0 or args.count < 0:
        parser.error("--interval must be > 0 and --count must be >= 0")

    # Import serial only if communicating with hardware, so dry-run works anywhere.
    ser = None
    if not args.dry_run:
        try:
            import serial
            ser = serial.Serial(args.port, args.baud, timeout=1)
        except (ImportError, OSError) as exc:
            parser.exit(1, f"Cannot open station UART ({args.port}): {exc}\n")
    print(f"Station emulator: port={args.port}, baud={args.baud}, interval={args.interval}s, dry_run={args.dry_run}", flush=True)
    n = 0
    try:
        while args.count == 0 or n < args.count:
            packet = make_packet(n)
            if ser is not None:
                ser.write(packet)
                ser.flush()
            n += 1
            print(f"[{n}] {'Would send' if args.dry_run else 'Sent'}: {packet.decode('ascii').strip()}", flush=True)
            if args.count and n >= args.count:
                break
            time.sleep(args.interval)
    except KeyboardInterrupt:
        print("Stopped", flush=True)
    finally:
        if ser is not None:
            ser.close()


if __name__ == "__main__":
    main()

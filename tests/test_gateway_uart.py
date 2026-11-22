#!/usr/bin/env python3
"""
WX-system gateway UART reception test.

ESP32-gateway -> Raspberry Pi UART
Device: /dev/serial0
Baud:   9600 8N1

Packet format:
@epoch,wind_speed,wind_direction,wind_gust,
temperature,humidity,barometric_pressure,
rainfall,sea_temperature,cabinet_temperature,
station_battery_voltage,uv,al,relay_battery_voltage*CRC
"""

import serial
import time
import sys

SERIAL_PORT = "/dev/serial0"
BAUD_RATE = 9600
EXPECTED_FIELDS = 14


def crc16_modbus(data):
    """Calculate CRC16-MODBUS."""
    crc = 0xFFFF

    for byte in data:
        crc ^= byte

        for _ in range(8):
            if crc & 1:
                crc = (crc >> 1) ^ 0xA001
            else:
                crc >>= 1

    return crc & 0xFFFF


def validate_packet(packet):
    """Validate packet structure and CRC."""

    if not packet.startswith("@"):
        return False, "Missing @"

    if "*" not in packet:
        return False, "Missing CRC separator"

    payload, received_crc = packet.rsplit("*", 1)

    try:
        expected_crc = int(received_crc, 16)
    except ValueError:
        return False, "Invalid CRC text"

    # CRC covers the packet including '@',
    # but excluding '*' and the CRC characters.
    calculated_crc = crc16_modbus(payload[1:].encode("ascii"))

    if calculated_crc != expected_crc:
        return False, (
            f"CRC mismatch: received {expected_crc:04X}, "
            f"calculated {calculated_crc:04X}"
        )

    fields = payload[1:].split(",")

    if len(fields) != EXPECTED_FIELDS:
        return False, (
            f"Expected {EXPECTED_FIELDS} fields, "
            f"received {len(fields)}"
        )

    return True, fields


def main():

    print("WX Gateway UART Reception Test")
    print("--------------------------------")
    print(f"Port: {SERIAL_PORT}")
    print(f"Baud: {BAUD_RATE}")
    print("Waiting for weather packets...\n")

    try:
        ser = serial.Serial(
            SERIAL_PORT,
            BAUD_RATE,
            bytesize=serial.EIGHTBITS,
            parity=serial.PARITY_NONE,
            stopbits=serial.STOPBITS_ONE,
            timeout=1
        )

    except serial.SerialException as e:
        print(f"Cannot open serial port: {e}")
        sys.exit(1)

    packet_count = 0
    valid_count = 0

    try:
        while True:

            raw = ser.readline()

            if not raw:
                continue

            timestamp = time.strftime("%Y-%m-%d %H:%M:%S")

            packet = raw.decode(
                "ascii", errors="replace"
            ).strip()

            packet_count += 1

            print(f"\n[{timestamp}]")
            print(f"RX: {packet}")

            valid, result = validate_packet(packet)

            if valid:

                valid_count += 1

                print("CRC: PASS")
                print("Fields: 14")
                print(f"Station epoch: {result[0]}")
                print(f"Station battery: {result[10]} V")
                print(f"Relay battery:   {result[13]} V")

            else:

                print(f"INVALID: {result}")

            print(
                f"Packets: {packet_count} | "
                f"Valid: {valid_count} | "
                f"Invalid: {packet_count - valid_count}"
            )

    except KeyboardInterrupt:
        print("\nTest stopped.")

    finally:
        ser.close()


if __name__ == "__main__":
    main()
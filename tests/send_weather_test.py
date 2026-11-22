#!/usr/bin/env python3

import argparse
import time
import serial

PORT = "/dev/serial0"
BAUD = 9600


def crc16_modbus(data):
    crc = 0xFFFF

    for byte in data:
        crc ^= byte

        for _ in range(8):
            if crc & 1:
                crc = (crc >> 1) ^ 0xA001
            else:
                crc >>= 1

    return crc & 0xFFFF


def build_packet():
    fields = [
        str(int(time.time())),  # Unix timestamp
        "5.2",                 # Wind speed
        "270.0",               # Wind direction
        "8.5",                 # Wind gust
        "12.5",                # Air temperature
        "75.0",                # Humidity
        "1013.2",              # Barometric pressure
        "0.0",                 # Rainfall
        "11.8",                # Sea temperature
        "15.2",                # Cabinet temperature
        "12.6",                # Station battery voltage
    ]

    body = ",".join(fields)
    crc = crc16_modbus(body.encode("ascii"))

    return f"@{body}*{crc:04X}\r\n".encode("ascii")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", default=PORT)
    parser.add_argument("--interval", type=float, default=0)
    args = parser.parse_args()

    with serial.Serial(args.port, BAUD, timeout=1) as ser:
        try:
            while True:
                packet = build_packet()

                ser.write(packet)
                ser.flush()

                print("Sent:", packet.decode("ascii").strip())

                if args.interval == 0:
                    break

                time.sleep(args.interval)

        except KeyboardInterrupt:
            print("Stopped")


if __name__ == "__main__":
    main()

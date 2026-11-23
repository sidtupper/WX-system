#!/usr/bin/env python3

# Enable postponed evaluation of type hints.
# This allows cleaner forward references in annotations.
from __future__ import annotations

import argparse
import logging
import math
import threading
from collections import deque
import signal
import sys
import time
from dataclasses import dataclass
from typing import Optional
from datetime import datetime
from typing import Optional, Tuple
from wind_timing import UTCWindSampler

# ============================================================
# Default configuration values
# ============================================================

# Native UART used for RS-485 weather sensors
# Pi4 mapping:
#   GPIO12 = TXD4
#   GPIO13 = RXD4
DEFAULT_RS485_PORT = "/dev/ttyAMA5"

# Native UART used to communicate with ESP32 LoRa bridge
# Pi4 mapping:
#   GPIO19 = TXD3
#   GPIO26 = RXD3
DEFAULT_ESP32_PORT = "/dev/ttyAMA3"

# Shared UART baud rate
DEFAULT_BAUD = 9600

# Sensor polling interval in seconds
# 10 minutes = 600 seconds
DEFAULT_INTERVAL = 600.0
DEFAULT_WIND_SAMPLE_INTERVAL = 1.0  # seconds
GUST_WINDOW_SAMPLES = 3  # 3 samples at 1 Hz = approximately 3 seconds

# Station battery monitor
INA_ADDRESS = 0x40
SHUNT_OHMS = 0.0275                 # calibrated with DMM

# Hardware initialized only in non-simulation mode.
ina = None
tft = None

# ============================================================
# CRC helper functions
# ============================================================

def crc16_modbus(data: bytes) -> int:
    """
    Compute standard Modbus CRC16.

    Polynomial: 0xA001
    Initial value: 0xFFFF

    Used both for:
      - Modbus RTU sensor requests/responses
      - Custom LoRa weather packet integrity checking
    """

    crc = 0xFFFF

    for byte in data:
        crc ^= byte

        for _ in range(8):
            if crc & 1:
                crc = (crc >> 1) ^ 0xA001
            else:
                crc >>= 1

    return crc & 0xFFFF


def add_packet_crc(payload: str) -> str:
    """
    Add station packet framing and CRC.

    Example result:
      @payload*ABCD\r\n

    This is the ASCII packet sent to the ESP32.
    """

    crc = crc16_modbus(payload.encode("ascii"))
    return f"@{payload}*{crc:04X}\r\n"


def modbus_crc(frame: bytes) -> bytes:
    """
    Append Modbus RTU CRC to a raw frame.

    Modbus transmits CRC little-endian:
      low byte first, then high byte.
    """

    crc = crc16_modbus(frame)

    return frame + bytes((
        crc & 0xFF,
        (crc >> 8) & 0xFF
    ))


# ============================================================
# Modbus RTU client
# ============================================================

class ModbusError(Exception):
    """
    Raised for Modbus protocol or transport errors.
    """
    pass


class ModbusRTU:
    """
    Minimal Modbus RTU master/client.

    Provides only the functionality needed by the weather station:
      - Function 0x03 (read holding registers)
    """

    def __init__(self, ser: serial.Serial,
                 inter_request_delay: float = 0.10):

        # Open pyserial Serial object
        self.ser = ser

        # Delay between requests helps avoid collisions and
        # gives slower sensors time to recover.
        self.inter_request_delay = inter_request_delay

    def read_holding_registers(
        self,
        slave: int,
        start: int,
        count: int,
        timeout: float = 1.0
    ) -> list[int]:
        """
        Read holding registers using Modbus function 0x03.

        Returns:
            list of 16-bit register values
        """

        # Modbus valid slave IDs
        if not 1 <= slave <= 247:
            raise ValueError("slave must be 1..247")

        # Maximum registers per request per Modbus spec
        if not 1 <= count <= 125:
            raise ValueError("count must be 1..125")

        # Prevent back-to-back requests too quickly
        time.sleep(self.inter_request_delay)

        # Construct raw Modbus RTU request
        request = bytes([
            slave,
            0x03,  # Function code: read holding registers

            # Register start address (big-endian)
            (start >> 8) & 0xFF,
            start & 0xFF,

            # Register count (big-endian)
            (count >> 8) & 0xFF,
            count & 0xFF,
        ])

        # Append CRC
        request = modbus_crc(request)

        # Clear any stale incoming bytes
        self.ser.reset_input_buffer()

        # Send request
        self.ser.write(request)
        self.ser.flush()

        # Expected response length:
        #   slave + function + bytecount + data + crc
        expected_len = 5 + count * 2

        response = self._read_exact(expected_len, timeout)

        if len(response) != expected_len:
            raise ModbusError(
                f"short response: got {len(response)} bytes, "
                f"expected {expected_len}"
            )

        # Split CRC from body
        body = response[:-2]

        # CRC from received packet
        rx_crc = response[-2] | (response[-1] << 8)

        # CRC computed locally
        calc_crc = crc16_modbus(body)

        if rx_crc != calc_crc:
            raise ModbusError(
                f"bad Modbus CRC: "
                f"rx=0x{rx_crc:04X} "
                f"calc=0x{calc_crc:04X} "
                f"raw={response.hex(' ')}"
            )

        # Verify correct responding slave
        if response[0] != slave:
            raise ModbusError(
                f"wrong slave id: got {response[0]}, expected {slave}"
            )

        # Exception response if high bit set
        if response[1] & 0x80:
            raise ModbusError(
                f"Modbus exception from slave {slave}: "
                f"code={response[2]}"
            )

        # Ensure correct function code echoed back
        if response[1] != 0x03:
            raise ModbusError(
                f"wrong function code: got 0x{response[1]:02X}"
            )

        # Byte count field
        byte_count = response[2]

        if byte_count != count * 2:
            raise ModbusError(
                f"wrong byte count: got {byte_count}, "
                f"expected {count * 2}"
            )

        # Convert register bytes to integers
        regs = []

        data = response[3:3 + byte_count]

        for i in range(0, len(data), 2):
            regs.append((data[i] << 8) | data[i + 1])

        return regs

    def _read_exact(self, n: int, timeout: float) -> bytes:
        """
        Read exactly n bytes or timeout.

        pyserial may return partial reads,
        so we accumulate until complete.
        """

        deadline = time.monotonic() + timeout

        buf = bytearray()

        while len(buf) < n and time.monotonic() < deadline:

            chunk = self.ser.read(n - len(buf))

            if chunk:
                buf.extend(chunk)
            else:
                # Avoid busy-spin
                time.sleep(0.005)

        return bytes(buf)


# ============================================================
# Sensor data models
# ============================================================

@dataclass
class WindReading:
    speed_m_s: float
    direction_deg: float
    gust_m_s: float
    temperature_c: float
    humidity_percent: float
    pressure_hpa: float


@dataclass
class RainReading:
    rainfall_mm: float


# ============================================================
# Wind sensor
# ============================================================

class WindSensor:
    """
    Mini-C5A register map:

      0x0000 wind speed x100
      0x0001 wind direction x10
      0x0002 temperature x10
      0x0003 humidity x10
      0x0004 pressure x10
    """

    def __init__(self, modbus: ModbusRTU, slave: int = 1):
        self.modbus = modbus
        self.slave = slave

    def read_all(self) -> WindReading:
        """
        Read all primary weather registers in one request.
        """

        regs = self.modbus.read_holding_registers(
            self.slave,
            0x0000,
            5
        )

        # Apply scaling factors from sensor documentation
        speed = regs[0] / 100.0
        direction = regs[1] / 10.0

        # Temperature may be signed
        temperature = signed16(regs[2]) / 10.0

        humidity = regs[3] / 10.0
        pressure = regs[4] / 10.0

        # Gust is calculated over the reporting interval by WindAccumulator.
        gust = speed

        return WindReading(
            speed_m_s=speed,
            direction_deg=direction,
            gust_m_s=gust,
            temperature_c=temperature,
            humidity_percent=humidity,
            pressure_hpa=pressure,
        )


# ============================================================
# 1 Hz wind sampling / 3-second gust tracking
# ============================================================

class WindAccumulator:
    """Accumulate an interval of 1 Hz samples from the Mini-C5A.

    Report wind speed as the scalar mean of successful samples, direction
    as a speed-weighted circular mean, and gust as the maximum complete
    3-sample moving average. This is an approximation of a 3-second gust;
    sensor internal averaging and polling frequency determine fidelity.
    """

    def __init__(self, sample_interval: float):
        self.sample_interval = sample_interval
        self.reset()

    def reset(self):
        self.count = 0
        self.speed_sum = 0.0
        self.east = 0.0
        self.north = 0.0
        self.peak_3s = None
        self.peak_sample = None
        self.window = deque(maxlen=GUST_WINDOW_SAMPLES)
        self.previous_sample_time = None
        self.last = None

    def add(self, reading: WindReading, timestamp: float):
        speed = reading.speed_m_s
        direction = reading.direction_deg
        if not (math.isfinite(speed) and speed >= 0 and
                math.isfinite(direction) and 0 <= direction <= 360):
            raise ValueError(f"invalid wind reading: {speed=}, {direction=}")

        # Do not compute a 3-second mean across a communication outage.
        if (self.previous_sample_time is not None and
                timestamp - self.previous_sample_time > self.sample_interval * 1.6):
            self.window.clear()
        self.previous_sample_time = timestamp

        self.count += 1
        self.speed_sum += speed
        angle = math.radians(direction)
        self.east += speed * math.sin(angle)
        self.north += speed * math.cos(angle)
        self.peak_sample = speed if self.peak_sample is None else max(self.peak_sample, speed)
        self.window.append(speed)
        if len(self.window) == GUST_WINDOW_SAMPLES:
            three_sec = sum(self.window) / GUST_WINDOW_SAMPLES
            self.peak_3s = three_sec if self.peak_3s is None else max(self.peak_3s, three_sec)
        self.last = reading

    def result(self) -> WindReading:
        if self.last is None or self.count == 0:
            raise RuntimeError("no valid wind observations during reporting interval")
        direction = (math.degrees(math.atan2(self.east, self.north)) % 360
                     if math.hypot(self.east, self.north) > 1e-9
                     else self.last.direction_deg)
        gust = self.peak_3s if self.peak_3s is not None else self.peak_sample
        return WindReading(
            speed_m_s=self.speed_sum / self.count,
            direction_deg=direction,
            gust_m_s=gust,
            temperature_c=self.last.temperature_c,
            humidity_percent=self.last.humidity_percent,
            pressure_hpa=self.last.pressure_hpa,
        )


# ============================================================
# Rain sensor
# ============================================================

class RainSensor:
    """
    SN-3001-GYL-N01

      0x0000 rainfall x10
    """

    def __init__(self, modbus: ModbusRTU, slave: int = 2):
        self.modbus = modbus
        self.slave = slave

    def read_all(self) -> RainReading:

        regs = self.modbus.read_holding_registers(
            self.slave,
            0x0000,
            1
        )

        return RainReading(
            rainfall_mm=regs[0] / 10.0
        )


def signed16(v: int) -> int:
    """
    Convert unsigned 16-bit integer to signed.
    """

    return v - 0x10000 if v & 0x8000 else v


# ============================================================
# Optional local readings
# ============================================================

def read_ds18b20_temperature(
    device_id: Optional[str] = None
) -> Optional[float]:
    """
    Read DS18B20 temperature from Linux 1-wire interface.

    If device_id is None:
      uses first detected sensor.

    Returns:
      float temperature in Celsius
      or None if unavailable/error
    """

    from pathlib import Path

    base = Path("/sys/bus/w1/devices")

    if device_id:
        candidates = [base / device_id]
    else:
        candidates = sorted(base.glob("28-*"))

    for dev in candidates:

        w1_slave = dev / "w1_slave"

        try:
            text = w1_slave.read_text()
        except OSError:
            continue

        lines = text.strip().splitlines()

        if len(lines) < 2:
            continue

        # First line must end with YES if CRC valid
        if not lines[0].endswith("YES"):
            continue

        marker = "t="

        idx = lines[1].find(marker)

        if idx < 0:
            continue

        try:
            return int(lines[1][idx + len(marker):]) / 1000.0
        except ValueError:
            continue

    return None


def read_station_battery_voltage() -> Optional[float]:
    """
    Read station battery voltage from INA226.

    Returns:
        Battery voltage in volts.

    Returns None on error.
    """
    try:
        return ina.voltage()
    except Exception as e:
        logger.warning(f"INA226 read failed: {e}")
        return None


def update_tft_display(tft, wx, station_battery):
    """
    Write the latest station status to the TFT.

    The TFT backlight is controlled separately by the door switch,
    so this can safely update the screen every cycle even when the
    door is closed and the display is dark.
    """

    from PIL import Image, ImageDraw, ImageFont

    now = datetime.now().strftime("%Y-%m-%d %H:%M:%S")

    image = Image.new("RGB", (480, 320), "black")
    draw = ImageDraw.Draw(image)

    try:
        font_big = ImageFont.truetype("/usr/share/fonts/cour.ttf", 24)
        font = ImageFont.truetype("/usr/share/fonts/cour.ttf", 20)
    except OSError:
        font_big = ImageFont.load_default()
        font = ImageFont.load_default()

    wind_speed_text = (
        f"wind speed:  {wx['wind_speed']:.1f} m/s"
        if wx.get("wind_speed") is not None
        else "N/A"
    )

    wind_dir_text = (
        f"wind dir:    {wx['wind_direction']:.1f} degrees"
        if wx.get("wind_direction") is not None
        else "N/A"
    )

    wind_gust_text = (
        f"wind gust:   {wx['wind_gust']:.1f} m/s"
        if wx.get("wind_gust") is not None
        else "N/A"
    )

    temp_text = (
        f"temperature: {wx['temperature']:.1f} degrees C"
        if wx.get("temperature") is not None
        else "N/A"
    )

    rh_text = (
        f"humidity:    {wx['humidity']:.1f} %"
        if wx.get("humidity") is not None
        else "N/A"
    )

    baro_text = (
        f"baro press:  {wx['barometric_pressure']:.1f} hPa"
        if wx.get("barometric_pressure") is not None
        else "N/A"
    )

    rain_text = (
        f"rainfall:    {wx['rainfall']:.1f} mm"
        if wx.get("rainfall") is not None
        else "N/A"
    )

    sea_temp_text = (
        f"sea temp:    {wx['sea_temperature']:.1f} degrees C"
        if wx.get("sea_temperature") is not None
        else "N/A"
    )

    cab_temp_text = (
        f"cab temp:    {wx['cabinet_temperature']:.1f} degrees C"
        if wx.get("cabinet_temperature") is not None
        else "N/A"
    )

    batt_text = (
        f"battery:     {station_battery:.2f} V"
        if station_battery is not None
        else "N/A"
    )

    lines = [
        "Weather Station",
        now,
        wind_speed_text,
        wind_dir_text,
        wind_gust_text,
        temp_text,
        rh_text,
        baro_text,
        rain_text,
        sea_temp_text,
        cab_temp_text,
        batt_text
    ]

    y = 10
    for i, line in enumerate(lines):
        draw.text((10, y), line, font=font_big if i == 0 else font, fill="white")
        y += 28 if i == 0 else 24

    # Your TFT object may use either display(image) or show(image),
    # depending on the driver used in station.py.
    if hasattr(tft, "display"):
        tft.display(image)
    elif hasattr(tft, "show"):
        tft.show(image)
    else:
        raise RuntimeError("TFT object has no display() or show() method")


# ============================================================
# Payload formatting
# ============================================================

def build_payload(
    *,
    epoch: int,
    wind: WindReading,
    rain: RainReading,
    sea_temperature_c: float,
    cabinet_temperature_c: float,
    battery_voltage: float,
) -> str:
    """
    Construct CSV payload before packet CRC.

    Final format:
      epoch,
      wind_speed,
      wind_direction,
      wind_gust,
      temperature,
      humidity,
      pressure,
      rainfall,
      sea_temp,
      cabinet_temp,
      station_battery
    """

    return (
        f"{epoch},"
        f"{wind.speed_m_s:.2f},"
        f"{wind.direction_deg:.1f},"
        f"{wind.gust_m_s:.2f},"
        f"{wind.temperature_c:.1f},"
        f"{wind.humidity_percent:.1f},"
        f"{wind.pressure_hpa:.1f},"
        f"{rain.rainfall_mm:.1f},"
        f"{sea_temperature_c:.1f},"
        f"{cabinet_temperature_c:.1f},"
        f"{battery_voltage:.2f}"
    )


# ============================================================
# Main program logic
# ============================================================

class StopFlag:
    """
    Simple signal-safe shutdown flag.
    """

    def __init__(self) -> None:
        self.stop = False

    def handler(self, signum, frame) -> None:
        """
        Called on SIGINT or SIGTERM.
        """
        self.stop = True


def open_serial(
    port: str,
    baud: int,
    timeout: float
) -> serial.Serial:
    """
    Open and configure a serial port.
    """

    import serial

    return serial.Serial(
        port=port,
        baudrate=baud,
        bytesize=serial.EIGHTBITS,
        parity=serial.PARITY_NONE,
        stopbits=serial.STOPBITS_ONE,
        timeout=timeout,
        write_timeout=timeout,
    )


class SimulatedWindSensor:
    def __init__(self):
        self.count = 0
    def read_all(self) -> WindReading:
        self.count += 1
        speed = 8.0 if self.count % 20 in (10, 11, 12) else 5.0
        return WindReading(speed, 270.0, speed, 12.5, 72.0, 1013.2)


class SimulatedRainSensor:
    def read_all(self) -> RainReading:
        return RainReading(0.0)


class PacketFile:
    def __init__(self, path):
        self.port, self.baudrate, self.is_open = path, 0, True
        self._file = open(path, "w", encoding="ascii")
    def write(self, data):
        self._file.write(data.decode("ascii"))
        return len(data)
    def flush(self):
        self._file.flush()
    def close(self):
        if self.is_open:
            self._file.close()
            self.is_open = False


def main() -> int:

    # --------------------------------------------------------
    # Command-line argument parsing
    # --------------------------------------------------------

    parser = argparse.ArgumentParser(
        description="Pi 4 weather station sender using native UARTs."
    )

    parser.add_argument("--simulate", action="store_true",
                        help="Use simulated sensors, no hardware access")
    parser.add_argument("--sim-output", default=None,
                        help="Local packet file path, required with --simulate")
    parser.add_argument("--sim-count", type=int, default=1,
                        help="Number of packets to emit in simulation")
    parser.add_argument("--rs485-port", default=DEFAULT_RS485_PORT)
    parser.add_argument("--esp32-port", default=DEFAULT_ESP32_PORT)

    parser.add_argument("--baud", type=int, default=DEFAULT_BAUD)

    parser.add_argument("--interval", type=int,
                        default=DEFAULT_INTERVAL)

    parser.add_argument("--wind-sample-interval", type=float,
                        default=DEFAULT_WIND_SAMPLE_INTERVAL,
                        help="Wind polling interval in seconds (default 1)")

    parser.add_argument("--wind-slave", type=int, default=1)
    parser.add_argument("--rain-slave", type=int, default=2)

    parser.add_argument("--sea-temp-device", default=None)
    parser.add_argument("--cabinet-temp-device", default=None)

    parser.add_argument("--default-sea-temp",
                        type=float, default=0.0)

    parser.add_argument("--default-cabinet-temp",
                        type=float, default=0.0)

    parser.add_argument("--default-station-battery",
                        type=float, default=0.0)

    parser.add_argument(
        "--log-level",
        default="INFO",
        choices=["DEBUG", "INFO", "WARNING", "ERROR"]
    )

    args = parser.parse_args()
    if args.simulate:
        if not args.sim_output or args.sim_count < 1:
            parser.error("--simulate requires --sim-output and positive --sim-count")
    elif args.sim_output is not None:
        parser.error("--sim-output requires --simulate")
    if args.interval <= 0 or 86400 % args.interval:
        parser.error("--interval must be a positive divisor of 86400 for UTC alignment")
    if args.wind_sample_interval <= 0:
        parser.error("--wind-sample-interval must be positive")
    if args.wind_sample_interval * GUST_WINDOW_SAMPLES > args.interval:
        parser.error("report interval must be at least 3 wind sample intervals")

    # --------------------------------------------------------
    # Logging configuration
    # --------------------------------------------------------

    logging.basicConfig(
        level=getattr(logging, args.log_level),

        format="%(asctime)s %(levelname)s station: %(message)s",

        datefmt="%Y-%m-%d %H:%M:%S",
    )

    log = logging.getLogger("station")

    # --------------------------------------------------------
    # Signal handling
    # --------------------------------------------------------

    stop = StopFlag()

    signal.signal(signal.SIGINT, stop.handler)
    signal.signal(signal.SIGTERM, stop.handler)

    rs485 = None
    esp32 = None
    sampler = None

    failed_cycles = 0
    simulated_packets = 0

    try:

        # ----------------------------------------------------
        # Open serial links
        # ----------------------------------------------------

        if args.simulate:
            log.info("SIMULATION: no UART, I2C or TFT access")
            wind_sensor = SimulatedWindSensor()
            rain_sensor = SimulatedRainSensor()
            esp32 = PacketFile(args.sim_output)
        else:
            from ina226 import INA226
            from stationio.ili9488 import ILI9488
            global ina, tft
            ina = INA226(shunt_ohms=SHUNT_OHMS,
                         max_expected_amps=0.8, address=INA_ADDRESS)
            ina.configure()
            tft = ILI9488(rotation=0, spi_speed_hz=4_000_000)
            tft.setup()
            log.info("Opening RS-485 port %s at %d baud",
                     args.rs485_port, args.baud)
            rs485 = open_serial(args.rs485_port, args.baud, timeout=1.0)
            log.info("Opening ESP32 port %s at %d baud",
                     args.esp32_port, args.baud)
            esp32 = open_serial(args.esp32_port, args.baud, timeout=1.0)
            modbus = ModbusRTU(rs485)
            wind_sensor = WindSensor(modbus, slave=args.wind_slave)
            rain_sensor = RainSensor(modbus, slave=args.rain_slave)

        log.info("station.py started")

        # Independent wind thread continues polling while main thread handles
        # rain, 1-Wire, display and ESP32 transmission. Both Modbus clients
        # serialize RS-485 transactions through the same bus lock.
        bus_lock = threading.Lock()
        sampler = UTCWindSampler(
            sensor=wind_sensor,
            accumulator_factory=lambda: WindAccumulator(args.wind_sample_interval),
            bus_lock=bus_lock,
            interval=args.interval,
            sample_interval=args.wind_sample_interval,
            logger=log,
        )
        sampler.start()
        log.info("Wind sampling %.2fs; UTC report boundaries every %ds",
                 args.wind_sample_interval, args.interval)

        while not stop.stop:
            completed = sampler.get(timeout=0.2)
            if completed is None:
                continue
            interval_epoch, wind_accumulator = completed

            try:
                wind = wind_accumulator.result()
                log.info("UTC interval ending %d: %d wind samples, "
                         "mean %.2f m/s, 3-s gust %.2f m/s",
                         interval_epoch, wind_accumulator.count,
                         wind.speed_m_s, wind.gust_m_s)
                if wind_accumulator.count < 0.9 * (args.interval / args.wind_sample_interval):
                    log.warning("Low wind sample coverage: %d successful polls",
                                wind_accumulator.count)

                # Small delay between Modbus requests
                time.sleep(0.05)

                with bus_lock:
                    rain = rain_sensor.read_all()

                # ------------------------------------------------
                # Read optional DS18B20 temperatures
                # ------------------------------------------------

                sea_temp = (10.4 if args.simulate else
                            read_ds18b20_temperature(args.sea_temp_device))

                if sea_temp is None:
                    sea_temp = args.default_sea_temp

                cabinet_temp = (18.0 if args.simulate else
                                read_ds18b20_temperature(args.cabinet_temp_device))

                if cabinet_temp is None:
                    cabinet_temp = args.default_cabinet_temp


                # ------------------------------------------------
                # WX values
                # ------------------------------------------------

                wx = {
                    "wind_speed": wind.speed_m_s,
                    "wind_direction": wind.direction_deg,
                    "wind_gust": wind.gust_m_s,
                    "temperature": wind.temperature_c,
                    "humidity": wind.humidity_percent,
                    "barometric_pressure": wind.pressure_hpa,
                    "rainfall": rain.rainfall_mm,
                    "sea_temperature": sea_temp,
                    "cabinet_temperature": cabinet_temp,
                }
                
                # ------------------------------------------------
                # Battery voltage
                # ------------------------------------------------

                station_batt = (12.7 if args.simulate else
                                read_station_battery_voltage())

                if station_batt is None:
                    station_batt = args.default_station_battery


                # ------------------------------------------------
                # Build payload
                # ------------------------------------------------

                payload = build_payload(
                    epoch=interval_epoch,
                    wind=wind,
                    rain=rain,
                    sea_temperature_c=sea_temp,
                    cabinet_temperature_c=cabinet_temp,
                    battery_voltage=station_batt,
                )

                # Add framing and CRC
                line = add_packet_crc(payload)


                # ------------------------------------------------
                # Send to tft display
                # ------------------------------------------------

                if not args.simulate:
                    update_tft_display(tft=tft, wx=wx,
                                       station_battery=station_batt)

                # ------------------------------------------------
                # Send to ESP32
                # ------------------------------------------------
                print(
                    f"ESP32 port={esp32.port}, open={esp32.is_open}, "
                    f"baud={esp32.baudrate}",
                    flush=True
                )

                written = esp32.write(line.encode("ascii"))
                esp32.flush()
                print(f"Wrote {written} bytes to ESP32", flush=True)
                esp32.flush()

                failed_cycles = 0

                log.info("Sent: %s", line.rstrip())
                if args.simulate:
                    simulated_packets += 1
                    if simulated_packets >= args.sim_count:
                        log.info("Simulation completed %d packets", simulated_packets)
                        break

            except Exception as exc:

                # Count failed polling cycles
                failed_cycles += 1

                log.exception(
                    "Cycle failed, failed_cycles=%d: %s",
                    failed_cycles,
                    exc
                )

        log.info("station.py stopping")

        return 0

    finally:

        if sampler is not None:
            sampler.stop()

        # ----------------------------------------------------
        # Clean serial shutdown
        # ----------------------------------------------------

        if esp32 is not None:
            try:
                esp32.close()
            except Exception:
                pass

        if rs485 is not None:
            try:
                rs485.close()
            except Exception:
                pass


# ============================================================
# Program entry point
# ============================================================

if __name__ == "__main__":
    raise SystemExit(main())

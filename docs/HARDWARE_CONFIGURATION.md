Station Pi UART configuration

Device: /dev/ttyAMA3
Boot overlay: dtoverlay=uart3
Baud rate: 9600

GPIO4 (TXD3) -> ESP32 GPIO27 (RX)
GPIO5 (RXD3) <- ESP32 GPIO23 (TX)

The primary UART (/dev/serial0) is NOT used for
station-to-ESP32 communication.

Verified November 22, 2026 on station2.

# WX-system Hardware Configuration

## Station Raspberry Pi — ESP32 UART

Verified: November 22, 2026
Test platform: station2 (Raspberry Pi 4B)

### Serial configuration

- UART device: `/dev/ttyAMA3`
- Baud rate: 9600, 8N1
- Boot overlay: `dtoverlay=uart3`
- Boot configuration file: `/boot/firmware/config.txt`

### Wiring

| Raspberry Pi | ESP32-station |
|---|---|
| GPIO4 (TXD3) | GPIO27 (RX) |
| GPIO5 (RXD3) | GPIO23 (TX) |
| GND | GND |

### Boot configuration

The following settings must be present under `[all]` in `/boot/firmware/config.txt`:

```ini
enable_uart=1
dtoverlay=uart3

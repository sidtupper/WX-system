\# WX-system



Remote weather monitoring system using Raspberry Pi computers, ESP32 microcontrollers, and Ebyte E22-900T30D LoRa radios.



\## System Architecture



The system consists of three nodes:



\*\*Station\*\*

\- Raspberry Pi 4B collects weather and environmental measurements.

\- ESP32 adds UV and ambient-light measurements.

\- E22 LoRa radio transmits weather packets to the midpoint.



\*\*Midpoint\*\*

\- ESP32 receives and validates station packets.

\- Measures its LiFePO4 battery voltage.

\- Controls battery charging according to temperature.

\- Forwards weather packets to the gateway using LoRa.



\*\*Gateway\*\*

\- ESP32 receives weather packets from the midpoint.

\- Forwards packets to a Raspberry Pi using UART.

\- Raspberry Pi validates packets and makes weather observations available to WeeWX.



\## Communication



Station Pi â†’ Station ESP32 â†’ Midpoint ESP32 â†’ Gateway ESP32 â†’ Gateway Pi



The LoRa network uses fixed addressing:



| Node | Address | Channel |

|---|---|---|

| Station | 00:01 | 60 |

| Midpoint | 00:02 | 60 |

| Gateway | 00:03 | 60 |



Radios: Ebyte E22-900T30D.



Weather packets contain 14 comma-separated fields and a CRC16-MODBUS checksum calculated over the payload, excluding the leading `@`.



\## Repository Structure



\- `station/` â€” Station Raspberry Pi software, ESP32 firmware, and hardware schematics.

\- `midpoint/` â€” Midpoint ESP32 firmware and hardware schematics.

\- `gateway/` â€” Gateway Raspberry Pi software, ESP32 firmware, and hardware schematics.

\- `tests/` â€” Communication and hardware diagnostic programs.

\- `docs/` â€” System documentation.



\## Firmware



ESP32 sketches are developed using the Arduino IDE with the ESP32 Dev Module board configuration.



Production firmware uses direct UART communication with the E22 radios.



\## Gateway



The gateway Raspberry Pi receives weather packets through `/dev/serial0` at 9600 baud.



`gateway.py` validates received packets and writes the latest valid observations to:



`/var/tmp/gateway\_latest.json`



This file is intended for consumption by WeeWX.



\## Verification



On November 21, 2026, the complete communication chain was successfully tested.



The gateway Raspberry Pi received valid 14-field weather packets, and `test\_gateway\_uart.py` confirmed successful CRC validation.



The midpoint battery ADC was calibrated against a digital multimeter.



\## Project Status



End-to-end communication verified.



Further work includes confirming unattended battery-only operation, integrating gateway data with WeeWX, and field deployment.

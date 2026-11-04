// WX-system midpoint — E22 safe-start diagnostics, 2026-11-04
// Unverified candidate; compile and bench-test before deployment.
#include <Arduino.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include "LoRa_E22.h"

// ============================================================
// MIDPOINT RELAY - PRODUCTION CANDIDATE
//
// ESP32 stays awake.
// E22 stays continuously in NORMAL receive mode.
//
// Incoming:
//   @field1,...,field13*CRC
//
// Midpoint:
//   validates CRC
//   appends midpoint battery voltage as field 14
//   recalculates CRC
//   forwards packet in E22 FIXED mode to gateway 0x0003
//
// Charger:
//   temperature >= +2 C -> enabled
//   temperature <  +2 C -> inhibited
//   sensor failure       -> inhibited
// ============================================================


// ---------------- Actual midpoint wiring --------------------

constexpr uint8_t LORA_RX  = 16;  // ESP32 RX  <- E22 TXD
constexpr uint8_t LORA_TX  = 17;  // ESP32 TX  -> E22 RXD
constexpr uint8_t LORA_AUX = 4;
constexpr uint8_t LORA_M0  = 21;
constexpr uint8_t LORA_M1  = 19;

constexpr uint8_t CHARGE_CONTROL = 13;
constexpr uint8_t DS18B20_PIN    = 18;
constexpr uint8_t BATTERY_ADC_PIN = 35;

// Fixed-address radio network
constexpr uint8_t MIDPOINT_ADDH = 0x00;
constexpr uint8_t MIDPOINT_ADDL = 0x02;
constexpr uint8_t GATEWAY_ADDH  = 0x00;
constexpr uint8_t GATEWAY_ADDL  = 0x03;
constexpr uint8_t LORA_CHANNEL = 23;


// ---------------- Charger -----------------------------------

constexpr uint8_t CHARGE_ENABLE  = LOW;
constexpr uint8_t CHARGE_INHIBIT = HIGH;

constexpr float MIN_CHARGE_TEMP_C = 2.0f;


// ---------------- Battery ADC -------------------------------

constexpr float DIVIDER_TOP_OHMS    = 100000.0f;
constexpr float DIVIDER_BOTTOM_OHMS = 220000.0f;

constexpr float ADC_CALIBRATION_FACTOR = 0.7247f;
constexpr float BATTERY_CALIBRATION_FACTOR = 1.0000f;

constexpr float VOLTAGE_MULTIPLIER =
    ADC_CALIBRATION_FACTOR *
    (DIVIDER_TOP_OHMS + DIVIDER_BOTTOM_OHMS) /
    DIVIDER_BOTTOM_OHMS;

constexpr uint8_t ADC_SAMPLES = 32;


// ---------------- Packet parameters -------------------------

constexpr uint8_t EXPECTED_INCOMING_FIELDS = 13;
constexpr size_t MAX_PACKET_CHARS = 220;

// We consider a packet complete when no additional UART
// character has arrived for this long.
constexpr uint32_t INTERBYTE_TIMEOUT_MS = 100;


// ---------------- Temperature interval ----------------------

// Check charging temperature every minute.
constexpr uint32_t TEMP_CHECK_INTERVAL_MS = 60000;


// ---------------- Objects -----------------------------------

HardwareSerial E22Serial(2);
LoRa_E22 e22(&E22Serial, LORA_AUX, LORA_M0, LORA_M1,
             UART_BPS_RATE_9600);

bool e22Ready = false; // True only after configuration readback matches.

OneWire oneWire(DS18B20_PIN);
DallasTemperature temperatureSensor(&oneWire);


// ============================================================
// CRC-16/MODBUS
// ============================================================

uint16_t crc16Modbus(const uint8_t *data, size_t length)
{
    uint16_t crc = 0xFFFF;

    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];

        for (uint8_t bit = 0; bit < 8; ++bit) {
            if (crc & 1) {
                crc = (crc >> 1) ^ 0xA001;
            } else {
                crc >>= 1;
            }
        }
    }

    return crc;
}


String crcToHex(uint16_t crc)
{
    char text[5];

    snprintf(
        text,
        sizeof(text),
        "%04X",
        crc
    );

    return String(text);
}


// ============================================================
// Packet validation
// ============================================================

bool isHexDigitString(const String &text)
{
    if (text.length() != 4)
        return false;

    for (size_t i = 0; i < text.length(); ++i) {

        char c = text[i];

        if (!((c >= '0' && c <= '9') ||
              (c >= 'A' && c <= 'F') ||
              (c >= 'a' && c <= 'f'))) {

            return false;
        }
    }

    return true;
}


uint8_t countCsvFields(const String &body)
{
    if (body.length() == 0)
        return 0;

    uint8_t fields = 1;

    for (size_t i = 0; i < body.length(); ++i) {
        if (body[i] == ',')
            ++fields;
    }

    return fields;
}


bool validatePacket(String packet, String &body)
{
    packet.trim();

    if (packet.length() < 8 ||
        packet.length() > MAX_PACKET_CHARS)
        return false;

    if (packet[0] != '@')
        return false;

    int star = packet.indexOf('*');

    if (star <= 1 ||
        star != packet.lastIndexOf('*'))
        return false;

    String crcText =
        packet.substring(star + 1);

    if (!isHexDigitString(crcText))
        return false;

    body =
        packet.substring(1, star);

    if (countCsvFields(body) !=
        EXPECTED_INCOMING_FIELDS)
        return false;

    uint16_t receivedCRC =
        static_cast<uint16_t>(
            strtoul(
                crcText.c_str(),
                nullptr,
                16
            )
        );

    uint16_t calculatedCRC =
        crc16Modbus(
            reinterpret_cast<const uint8_t *>(
                body.c_str()
            ),
            body.length()
        );

    return receivedCRC == calculatedCRC;
}


// ============================================================
// Battery voltage
// ============================================================

float readBatteryVoltage()
{
    uint32_t sum = 0;

    for (uint8_t i = 0;
         i < ADC_SAMPLES;
         ++i) {

        sum +=
            analogReadMilliVolts(
                BATTERY_ADC_PIN
            );

        delay(2);
    }

    float adcVoltage =
        (sum /
         static_cast<float>(ADC_SAMPLES))
        / 1000.0f;

    return
        adcVoltage *
        VOLTAGE_MULTIPLIER *
        BATTERY_CALIBRATION_FACTOR;
}


// ============================================================
// Temperature
// ============================================================

float readTemperature()
{
    // Retry transient OneWire failures.

    for (uint8_t attempt = 1;
         attempt <= 3;
         ++attempt) {

        temperatureSensor.requestTemperatures();

        float t =
            temperatureSensor.getTempCByIndex(0);

        if (t > -100.0f &&
            t < 80.0f) {

            return t;
        }

        Serial.printf(
            "DS18B20 attempt %u failed: %.2f C\n",
            attempt,
            t
        );

        delay(250);
    }

    return DEVICE_DISCONNECTED_C;
}


void checkChargingTemperature()
{
    float temperatureC =
        readTemperature();

    Serial.printf(
        "Midpoint temperature: %.2f C\n",
        temperatureC
    );

    // Invalid sensor reading -> fail safe.

    if (temperatureC < -100.0f ||
        temperatureC > 80.0f) {

        digitalWrite(
            CHARGE_CONTROL,
            CHARGE_INHIBIT
        );

        Serial.println(
            "Charging INHIBITED - temperature sensor error"
        );

        return;
    }

    if (temperatureC < MIN_CHARGE_TEMP_C) {

        digitalWrite(
            CHARGE_CONTROL,
            CHARGE_INHIBIT
        );

        Serial.println(
            "Charging INHIBITED - temperature below +2 C"
        );

    } else {

        digitalWrite(
            CHARGE_CONTROL,
            CHARGE_ENABLE
        );

        Serial.println(
            "Charging enabled"
        );
    }
}


// ============================================================
// E22
// ============================================================

// Report the actual settings read back from the E22, not just the target values.
void printE22Config(const char *label, const Configuration &c) {
  Serial.printf(
      "%s: addr=%02X:%02X net=%u chan=%u UART=%u parity=%u "
      "air=%u fixed=%u subpacket=%u RSSI=%u LBT=%u repeat=%u "
      "crypt=%02X:%02X power-code=%u\n",
      label, c.ADDH, c.ADDL, c.NETID, c.CHAN,
      c.SPED.uartBaudRate, c.SPED.uartParity, c.SPED.airDataRate,
      c.TRANSMISSION_MODE.fixedTransmission,
      c.OPTION.subPacketSetting, c.TRANSMISSION_MODE.enableRSSI,
      c.TRANSMISSION_MODE.enableLBT,
      c.TRANSMISSION_MODE.enableRepeater,
      c.CRYPT.CRYPT_H, c.CRYPT.CRYPT_L,
      c.OPTION.transmissionPower);
}

// All three nodes must have matching RF/packet settings. Preserve
// transmit power from the existing module configuration (battery budget).
void normalizeE22Config(Configuration &c, uint8_t addrH, uint8_t addrL,
                        uint8_t channel) {
  c.ADDH = addrH;
  c.ADDL = addrL;
  c.NETID = 0;
  c.CHAN = channel;
  c.SPED.uartBaudRate = UART_BPS_9600;
  c.SPED.uartParity = MODE_00_8N1;
  c.SPED.airDataRate = AIR_DATA_RATE_010_24; // 2.4 kbps on air
  c.OPTION.subPacketSetting = SPS_240_00;
  c.OPTION.RSSIAmbientNoise = RSSI_AMBIENT_NOISE_DISABLED;
  c.TRANSMISSION_MODE.fixedTransmission = FT_FIXED_TRANSMISSION;
  c.TRANSMISSION_MODE.enableRSSI = RSSI_DISABLED;
  c.TRANSMISSION_MODE.enableLBT = LBT_DISABLED;
  c.TRANSMISSION_MODE.enableRepeater = REPEATER_DISABLED;
  c.CRYPT.CRYPT_H = 0;
  c.CRYPT.CRYPT_L = 0;
}

bool configureE22() {
  ResponseStructContainer current = e22.getConfiguration();
  if (current.status.code != E22_SUCCESS) {
    Serial.printf("E22 getConfiguration failed: %d (%s)\n",
                  current.status.code,
                  current.status.getResponseDescription().c_str());
    // Library may return before allocating a data buffer on failure.
    return false;
  }
  if (current.data == nullptr) {
    Serial.println("ERROR: E22 returned an empty configuration buffer");
    return false;
  }
  Configuration c = *(Configuration *)current.data;
  printE22Config("E22 before", c);
  current.close();

  normalizeE22Config(c, MIDPOINT_ADDH, MIDPOINT_ADDL, LORA_CHANNEL);
  ResponseStatus status = e22.setConfiguration(c, WRITE_CFG_PWR_DWN_SAVE);
  if (status.code != E22_SUCCESS) {
    Serial.printf("E22 setConfiguration failed: %d (%s)\n",
                  status.code, status.getResponseDescription().c_str());
    return false;
  }

  ResponseStructContainer check = e22.getConfiguration();
  if (check.status.code != E22_SUCCESS) {
    Serial.printf("E22 readback failed: %d (%s)\n",
                  check.status.code,
                  check.status.getResponseDescription().c_str());
    // Do not free a potentially uninitialized pointer on early failure.
    return false;
  }
  if (check.data == nullptr) {
    Serial.println("ERROR: E22 returned an empty configuration buffer");
    return false;
  }
  Configuration actual = *(Configuration *)check.data;
  printE22Config("E22 actual", actual);
  check.close();
  bool ok = (actual.ADDH == MIDPOINT_ADDH && actual.ADDL == MIDPOINT_ADDL
          && actual.CHAN == LORA_CHANNEL && actual.NETID == 0
          && actual.SPED.uartBaudRate == UART_BPS_9600
          && actual.SPED.uartParity == MODE_00_8N1
          && actual.SPED.airDataRate == AIR_DATA_RATE_010_24
          && actual.OPTION.subPacketSetting == SPS_240_00
          && actual.OPTION.RSSIAmbientNoise == RSSI_AMBIENT_NOISE_DISABLED
          && actual.TRANSMISSION_MODE.enableRSSI == RSSI_DISABLED
          && actual.TRANSMISSION_MODE.enableLBT == LBT_DISABLED
          && actual.TRANSMISSION_MODE.enableRepeater == REPEATER_DISABLED
          && actual.TRANSMISSION_MODE.fixedTransmission == FT_FIXED_TRANSMISSION
          && actual.CRYPT.CRYPT_H == 0 && actual.CRYPT.CRYPT_L == 0);
  Serial.println("E22 expected RF link: 9600 8N1 UART; 2.4 kbps air; "
                 "channel 23; NETID 0; fixed mode; key 0000");
  Serial.println(ok ? "E22 configuration VERIFIED" : "ERROR: E22 readback mismatch");
  return ok;
}

// Print even binary junk in a way that can be compared with packets.
void printHexBytes(const String &packet) {
  Serial.print("RX hex:");
  for (size_t i = 0; i < packet.length(); ++i)
    Serial.printf(" %02X", (uint8_t)packet[i]);
  Serial.println();
}

// ============================================================
// Receive one UART packet
// ============================================================

bool receivePacket(String &packet)
{
    if (!E22Serial.available())
        return false;

    packet = "";

    uint32_t lastByteTime = millis();

    while (true) {

        while (E22Serial.available()) {

            char c =
                static_cast<char>(
                    E22Serial.read()
                );

            if (packet.length() <
                MAX_PACKET_CHARS) {

                packet += c;
            }

            lastByteTime = millis();
        }

        if (millis() - lastByteTime >=
            INTERBYTE_TIMEOUT_MS) {

            break;
        }

        delay(1);
    }

    packet.trim();

    return packet.length() > 0;
}


// ============================================================
// Forward packet
// ============================================================

bool forwardPacket(const String &packet)
{
    // FIXED mode adds address/channel on the E22 UART input; the radio
    // strips that routing header before delivering the receiver payload.
    ResponseStatus status = e22.sendFixedMessage(
        GATEWAY_ADDH, GATEWAY_ADDL, LORA_CHANNEL, packet);
    Serial.printf("E22 midpoint->gateway TX: %d (%s)\n",
                  status.code, status.getResponseDescription().c_str());
    return status.code == E22_SUCCESS;
}

// ============================================================
// SETUP
// ============================================================

uint32_t lastTemperatureCheck = 0;


void setup()
{
    Serial.begin(115200);
    delay(500);

    Serial.println();
    Serial.println(
        "Midpoint production relay starting"
    );

    // --------------------------------------------------------
    // Charger defaults OFF until temperature is verified.
    // --------------------------------------------------------

    pinMode(
        CHARGE_CONTROL,
        OUTPUT
    );

    digitalWrite(
        CHARGE_CONTROL,
        CHARGE_INHIBIT
    );

    // --------------------------------------------------------
    // Battery ADC
    // --------------------------------------------------------

    pinMode(
        BATTERY_ADC_PIN,
        INPUT
    );

    analogReadResolution(12);

    analogSetPinAttenuation(
        BATTERY_ADC_PIN,
        ADC_11db
    );

    // --------------------------------------------------------
    // DS18B20
    // --------------------------------------------------------

    temperatureSensor.begin();

    delay(250);

    checkChargingTemperature();

    lastTemperatureCheck =
        millis();

    // --------------------------------------------------------
    // E22 (uses actual midpoint AUX on GPIO4, DS18B20 on GPIO18)
    // --------------------------------------------------------

    E22Serial.begin(9600, SERIAL_8N1, LORA_RX, LORA_TX);
    const bool beginOK = e22.begin();
    Serial.printf("E22 begin: %s\n", beginOK ? "OK (not proof of radio)" : "FAILED");
    Serial.printf("E22 GPIO after begin: AUX=%d M0=%d M1=%d\n",
                  digitalRead(LORA_AUX), digitalRead(LORA_M0), digitalRead(LORA_M1));
    if (beginOK) e22Ready = configureE22();
    if (e22Ready) {
        Serial.println("Midpoint E22 VERIFIED; listening in normal mode");
    } else {
        Serial.println("Midpoint E22 OFFLINE: RF receive/forward disabled."
                       " Expected with bare ESP32; AUX level is meaningless without a radio.");
    }

    Serial.println("Midpoint application running");
}


// ============================================================
// LOOP
// ============================================================

void loop()
{
    // --------------------------------------------------------
    // Periodic temperature / charger control
    // --------------------------------------------------------

    if (millis() -
        lastTemperatureCheck >=
        TEMP_CHECK_INTERVAL_MS) {

        checkChargingTemperature();

        lastTemperatureCheck =
            millis();
    }

    // --------------------------------------------------------
    // Check for received E22 UART data
    // --------------------------------------------------------

    if (!e22Ready) {
        delay(100);
        return; // Charger temperature checks above continue to run.
    }

    if (!E22Serial.available()) {

        // Short delay prevents pointless full-speed polling.
        delay(2);
        return;
    }

    String incoming;

    if (!receivePacket(incoming))
        return;

    Serial.print("RX: ");
    Serial.println(incoming);

    // --------------------------------------------------------
    // Validate station packet
    // --------------------------------------------------------

    String body;

    if (!validatePacket(
            incoming,
            body)) {

        Serial.println(
            "REJECTED: invalid format, field count or CRC"
        );
        printHexBytes(incoming);

        return;
    }

    Serial.println(
        "Valid station packet"
    );

    // --------------------------------------------------------
    // Midpoint battery voltage
    // --------------------------------------------------------

    float batteryVoltage =
        readBatteryVoltage();

    Serial.printf(
        "Midpoint battery: %.3f V\n",
        batteryVoltage
    );

    // --------------------------------------------------------
    // Append field 14
    // --------------------------------------------------------

    String outgoingBody =
        body +
        "," +
        String(
            batteryVoltage,
            3
        );

    uint16_t newCRC =
        crc16Modbus(
            reinterpret_cast<const uint8_t *>(
                outgoingBody.c_str()
            ),
            outgoingBody.length()
        );

    String outgoing =
        "@" +
        outgoingBody +
        "*" +
        crcToHex(newCRC);

    // --------------------------------------------------------
    // Forward
    // --------------------------------------------------------

    Serial.print("TX: ");
    Serial.println(outgoing);

    if (forwardPacket(outgoing)) {

        Serial.println(
            "Forward complete"
        );

    } else {

        Serial.println(
            "ERROR: E22 fixed transmit failed"
        );
    }
}

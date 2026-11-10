// WX-system midpoint -- direct-UART E22 production integration candidate, 2026-11-09
// Derived from successful three-node fixed-address RF bench test.
// NOT field validated; battery monitor and charger logic retained.
#include <Arduino.h>
#include <OneWire.h>
#include <DallasTemperature.h>


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
constexpr uint8_t LORA_CHANNEL = 60; // Bench-verified: 910.125 MHz


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


constexpr uint8_t E22_LOCAL_ADDRESS = MIDPOINT_ADDL;
bool e22Ready = false; // True only when raw UART config verifies.

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
// E22 DIRECT UART DRIVER - based on VERIFIED 3-node bench test
// No xreef LoRa_E22 library. C2 is temporary and re-applied at boot.
// ============================================================
constexpr uint8_t E22_SPEED = 0x62;      // 9600 8N1 UART; 2.4 kbps RF
constexpr uint8_t E22_OPTION = 0x03;     // 240-byte subpacket, lowest TX power
constexpr uint8_t E22_MODE = 0x43;       // fixed addressing; normal RX/TX
constexpr uint32_t E22_REPLY_TIMEOUT_MS = 1500;
constexpr uint32_t E22_RETRY_MS = 30000;
uint32_t lastE22Attempt = 0;

bool waitE22Aux(uint32_t timeoutMs) {
  const uint32_t started = millis();
  do {
    if (digitalRead(LORA_AUX) == HIGH) return true;
    delay(5);
  } while (millis() - started < timeoutMs);
  return false;
}

bool setE22Mode(bool configuration) {
  digitalWrite(LORA_M0, LOW);
  digitalWrite(LORA_M1, configuration ? HIGH : LOW);
  delay(150);
  bool ok = waitE22Aux(3000);
  Serial.printf("E22 %s mode M0=%d M1=%d AUX=%d ready=%s\n",
                configuration ? "CONFIG" : "NORMAL",
                digitalRead(LORA_M0), digitalRead(LORA_M1),
                digitalRead(LORA_AUX), ok ? "YES" : "NO");
  return ok;
}

void printE22Reply(const char *label, const uint8_t *buffer, size_t n) {
  Serial.printf("%s RX %u/12:", label, (unsigned)n);
  for (size_t i = 0; i < n; ++i) Serial.printf(" %02X", buffer[i]);
  Serial.println();
}

bool e22Transaction(const char *label, const uint8_t *request,
                    size_t reqLen, uint8_t response[12]) {
  // Only called in configuration mode; drain stale radio-UART bytes.
  while (E22Serial.available()) (void)E22Serial.read();
  Serial.printf("%s TX:", label);
  for (size_t i = 0; i < reqLen; ++i) Serial.printf(" %02X", request[i]);
  Serial.println();
  E22Serial.write(request, reqLen);
  E22Serial.flush();
  size_t received = 0;
  const uint32_t started = millis();
  while (millis() - started < E22_REPLY_TIMEOUT_MS && received < 12) {
    while (E22Serial.available() && received < 12)
      response[received++] = (uint8_t)E22Serial.read();
    if (received < 12) delay(1);
  }
  printE22Reply(label, response, received);
  return received == 12 && response[0] == 0xC1 &&
         response[1] == 0x00 && response[2] == 0x09;
}

bool readE22Configuration(const char *label, uint8_t response[12]) {
  const uint8_t request[] = {0xC1, 0x00, 0x09};
  for (uint8_t attempt = 1; attempt <= 3; ++attempt) {
    if (!waitE22Aux(1500)) Serial.println("WARN: E22 AUX LOW before read");
    if (e22Transaction(label, request, sizeof(request), response)) return true;
    Serial.printf("E22 configuration read attempt %u failed\n", attempt);
    delay(200);
  }
  return false;
}

bool configureE22() {
  if (!setE22Mode(true)) {
    Serial.println("ERROR: E22 did not enter configuration mode");
    (void)setE22Mode(false);
    return false;
  }

  uint8_t before[12] = {};
  if (!readE22Configuration("READ_BEFORE", before)) {
    Serial.println("ERROR: no valid E22 configuration response");
    (void)setE22Mode(false);
    return false;
  }
  Serial.printf("E22 BEFORE: addr=%02X:%02X net=%u speed=%02X option=%02X "
                "channel=%u mode=%02X\n",
                before[3], before[4], before[5], before[6],
                before[7], before[8], before[9]);

  const uint8_t config[] = {
      0xC2, 0x00, 0x09,  // temporary configuration (not flash/EEPROM)
      0x00, E22_LOCAL_ADDRESS, 0x00,
      E22_SPEED, E22_OPTION, LORA_CHANNEL, E22_MODE, 0x00, 0x00
  };
  uint8_t acknowledgement[12] = {};
  if (!e22Transaction("WRITE_TEMP", config, sizeof(config), acknowledgement)) {
    Serial.println("ERROR: E22 configuration write not acknowledged");
    (void)setE22Mode(false);
    return false;
  }
  delay(120);

  uint8_t after[12] = {};
  if (!readE22Configuration("READ_AFTER", after)) {
    Serial.println("ERROR: E22 configuration readback failed");
    (void)setE22Mode(false);
    return false;
  }
  Serial.printf("E22 AFTER: addr=%02X:%02X net=%u speed=%02X option=%02X "
                "channel=%u mode=%02X crypt=%02X:%02X\n",
                after[3], after[4], after[5], after[6], after[7],
                after[8], after[9], after[10], after[11]);
  const bool verified = after[3] == 0x00 &&
      after[4] == E22_LOCAL_ADDRESS && after[5] == 0x00 &&
      after[6] == E22_SPEED && after[7] == E22_OPTION &&
      after[8] == LORA_CHANNEL && after[9] == E22_MODE &&
      after[10] == 0x00 && after[11] == 0x00;

  if (!verified) Serial.println("ERROR: E22 configuration mismatch");
  const bool normal = setE22Mode(false);
  if (verified && normal)
    Serial.printf("CONFIG VERIFIED: addr=00:%02X channel=%u fixed mode\n",
                  E22_LOCAL_ADDRESS, LORA_CHANNEL);
  return verified && normal;
}

void startE22() {
  pinMode(LORA_M0, OUTPUT);
  pinMode(LORA_M1, OUTPUT);
  digitalWrite(LORA_M0, LOW);
  digitalWrite(LORA_M1, LOW);
  pinMode(LORA_AUX, INPUT);
  E22Serial.begin(9600, SERIAL_8N1, LORA_RX, LORA_TX);
  lastE22Attempt = millis();
  e22Ready = configureE22();
  Serial.println(e22Ready ? "E22 RF ENABLED" : "E22 RF OFFLINE; will retry");
}

void retryE22IfOffline() {
  if (e22Ready || (uint32_t)(millis() - lastE22Attempt) < E22_RETRY_MS)
    return;
  lastE22Attempt = millis();
  Serial.println("Retrying E22 configuration...");
  e22Ready = configureE22();
  Serial.println(e22Ready ? "E22 RF ENABLED" : "E22 RF OFFLINE");
}

bool sendE22Fixed(uint8_t destLow, const String &payload) {
  if (!e22Ready) return false;
  // E22 currently configured for 240-byte subpackets.
  if (payload.length() + 3 > 240) {
    Serial.println("ERROR: E22 payload exceeds one radio subpacket");
    return false;
  }
  if (!waitE22Aux(3000)) {
    Serial.println("ERROR: E22 AUX stayed LOW before TX");
    return false;
  }
  const uint8_t header[] = {0x00, destLow, LORA_CHANNEL};
  const size_t headWritten = E22Serial.write(header, sizeof(header));
  const size_t bodyWritten = E22Serial.write(
      reinterpret_cast<const uint8_t *>(payload.c_str()), payload.length());
  E22Serial.flush();
  const bool queued = headWritten == sizeof(header) &&
                      bodyWritten == payload.length();
  Serial.printf("E22 UART TX 00:%02X CH=%u bytes=%u %s\n",
                destLow, LORA_CHANNEL, (unsigned)payload.length(),
                queued ? "QUEUED" : "FAILED");
  // QUEUED means written to UART, not confirmed over-the-air delivery.
  return queued;
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
    // Forward as one newline-terminated frame for gateway parsing.
    // A UART queue success alone is NOT proof of gateway receipt.
    return sendE22Fixed(GATEWAY_ADDL, packet + "\r\n");
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

    digitalWrite(CHARGE_CONTROL, CHARGE_INHIBIT);
    pinMode(CHARGE_CONTROL, OUTPUT);

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

    startE22();

    Serial.println("Midpoint application running");
}


// ============================================================
// LOOP
// ============================================================

void loop()
{
    retryE22IfOffline();
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
            "Forward queued on radio UART (not gateway ACK)"
        );

    } else {

        Serial.println(
            "ERROR: E22 fixed transmit failed"
        );
    }
}

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
//   forwards packet transparently
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

bool waitForAuxHigh(uint32_t timeoutMs)
{
    uint32_t start = millis();

    while (digitalRead(LORA_AUX) == LOW) {

        if (millis() - start >= timeoutMs)
            return false;

        delay(1);
    }

    delay(2);

    return true;
}


void setE22NormalMode()
{
    // E22-900T30D NORMAL mode
    //
    // M1 = 0
    // M0 = 0

    digitalWrite(LORA_M1, LOW);
    digitalWrite(LORA_M0, LOW);

    delay(10);

    if (!waitForAuxHigh(1000)) {
        Serial.println(
            "WARNING: E22 AUX timeout entering normal mode"
        );
    }
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
    // Current E22 configuration is TRANSPARENT mode.
    //
    // Do not prepend address/channel bytes.

    E22Serial.print(packet);

    E22Serial.flush();

    delay(2);

    if (!waitForAuxHigh(3000)) {
        return false;
    }

    return true;
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
    // E22
    // --------------------------------------------------------

    pinMode(
        LORA_M0,
        OUTPUT
    );

    pinMode(
        LORA_M1,
        OUTPUT
    );

    pinMode(
        LORA_AUX,
        INPUT
    );

    E22Serial.begin(
        9600,
        SERIAL_8N1,
        LORA_RX,
        LORA_TX
    );

    setE22NormalMode();

    Serial.println(
        "E22 listening continuously"
    );

    Serial.println(
        "Midpoint relay ready"
    );
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
            "ERROR: E22 AUX timeout during forwarding"

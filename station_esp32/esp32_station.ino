// WX-system station -- direct-UART E22 production integration candidate, 2026-11-09
// Derived from three-node fixed-address RF test proven on the bench.
// NOT field validated. Verify full weather packets before deployment.
#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_LTR390.h>
#include <ctype.h>

// ============================================================
// Station ESP32
//
// Receives from Raspberry Pi:
//   @epoch,wind_speed,wind_direction,wind_gust,temperature,
//    humidity,pressure,rainfall,sea_temp,cabinet_temp,
//    station_battery*CRC\r\n
// Validates the Pi packet, appends UV and ambient-light readings,
// recalculates the CRC, and sends the completed packet by E22
// FIXED transmission to the midpoint node only.
// ============================================================

constexpr size_t RX_BUF_SIZE = 256;
constexpr uint32_t PI_PACKET_TIMEOUT_MS = 3000;

// ------------------------------------------------------------
// Raspberry Pi <-> station ESP32 UART
// Pi GPIO4 TX  -> ESP32 GPIO27 RX
// Pi GPIO5 RX  <- ESP32 GPIO23 TX
// Pi link uses UART1
// ------------------------------------------------------------
HardwareSerial &WXSerial = Serial1;
constexpr uint8_t WX_RX = 27;
constexpr uint8_t WX_TX = 23;
constexpr uint32_t WX_BAUD = 9600;

// ------------------------------------------------------------
// LTR390 UV/ambient-light sensor
// ------------------------------------------------------------
constexpr uint8_t I2C_SDA = 25;
constexpr uint8_t I2C_SCL = 26;
Adafruit_LTR390 ltr;
bool ltrAvailable = false;

// ------------------------------------------------------------
// Station ESP32 <-> E22 UART
// E22 uses UART2
// ------------------------------------------------------------
HardwareSerial E22Serial(2);
constexpr uint8_t LORA_RX  = 16;  // ESP32 RX <- E22 TXD
constexpr uint8_t LORA_TX  = 17;  // ESP32 TX -> E22 RXD
constexpr uint8_t LORA_AUX = 18;
constexpr uint8_t LORA_M0  = 21;
constexpr uint8_t LORA_M1  = 19;


// ------------------------------------------------------------
// E22 fixed-address network
// ------------------------------------------------------------
constexpr uint8_t STATION_ADDH  = 0x00;
constexpr uint8_t STATION_ADDL  = 0x01;
constexpr uint8_t MIDPOINT_ADDH = 0x00;
constexpr uint8_t MIDPOINT_ADDL = 0x02;
constexpr uint8_t LORA_CHANNEL  = 60; // Bench-verified: 910.125 MHz

constexpr uint8_t E22_LOCAL_ADDRESS = STATION_ADDL;
bool e22Ready = false; // True only when raw UART configuration verifies.

char rxLine[RX_BUF_SIZE];

// ============================================================
// CRC16-MODBUS
// ============================================================
uint16_t crc16Modbus(const uint8_t *data, size_t length) {
  uint16_t crc = 0xFFFF;

  for (size_t i = 0; i < length; ++i) {
    crc ^= data[i];

    for (uint8_t bit = 0; bit < 8; ++bit) {
      crc = (crc & 1U)
                ? static_cast<uint16_t>((crc >> 1U) ^ 0xA001U)
                : static_cast<uint16_t>(crc >> 1U);
    }
  }

  return crc;
}

String makePacket(const String &body) {
  char crcText[5];
  const uint16_t crc = crc16Modbus(
      reinterpret_cast<const uint8_t *>(body.c_str()),
      body.length()
  );

  snprintf(crcText, sizeof(crcText), "%04X", crc);
  return "@" + body + "*" + String(crcText) + "\r\n";
}

// ============================================================
// Raspberry Pi packet reception and checking
// ============================================================
bool readPacketLine(
    char *buf,
    size_t maxLength,
    uint32_t timeoutMs = PI_PACKET_TIMEOUT_MS
) {
  size_t index = 0;
  bool inPacket = false;
  const uint32_t start = millis();

  while (millis() - start < timeoutMs) {
    while (WXSerial.available() > 0) {
      const char c = static_cast<char>(WXSerial.read());

      // Ignore everything until the start marker.
      if (!inPacket) {
        if (c == '@') {
          inPacket = true;
          index = 0;
          buf[index++] = c;
        }
        continue;
      }

      if (index >= maxLength - 1) {
        buf[maxLength - 1] = '\0';
        Serial.println("Pi packet too long; dropped");
        return false;
      }

      buf[index++] = c;

      if (c == '\n') {
        buf[index] = '\0';
        return true;
      }
    }

    delay(1);
  }

  return false;
}

bool extractAndValidateBody(char *packet, String &body) {
  if (packet == nullptr || packet[0] != '@') {
    return false;
  }

  char *star = strchr(packet, '*');
  if (star == nullptr) {
    return false;
  }

  char *lineEnd = strchr(star + 1, '\n');
  if (lineEnd == nullptr) {
    return false;
  }

  // Permit an optional carriage return before newline.
  char *crcEnd = lineEnd;
  if (crcEnd > star + 1 && *(crcEnd - 1) == '\r') {
    --crcEnd;
  }

  if (crcEnd - (star + 1) != 4) {
    return false;
  }

  for (char *p = star + 1; p < crcEnd; ++p) {
    if (!isxdigit(static_cast<unsigned char>(*p))) {
      return false;
    }
  }

  char crcText[5];
  memcpy(crcText, star + 1, 4);
  crcText[4] = '\0';

  *star = '\0';
  body = String(packet + 1);

  const uint16_t received =
      static_cast<uint16_t>(strtoul(crcText, nullptr, 16));

  const uint16_t calculated = crc16Modbus(
      reinterpret_cast<const uint8_t *>(body.c_str()),
      body.length()
  );

  if (received != calculated) {
    Serial.printf(
        "Bad Pi CRC: received=%04X calculated=%04X\n",
        received,
        calculated
    );
    return false;
  }

  return true;
}

// ============================================================
// LTR390 readings
// ============================================================
bool waitForLTRData(uint32_t timeoutMs = 1000) {
  const uint32_t start = millis();

  while (!ltr.newDataAvailable()) {
    if (millis() - start >= timeoutMs) {
      return false;
    }
    delay(5);
  }

  return true;
}

bool readLTR390(uint32_t &uv, uint32_t &al) {
  uv = 0;
  al = 0;

  if (!ltrAvailable) {
    return false;
  }

  // Read raw UV count.
  ltr.setMode(LTR390_MODE_UVS);
  delay(20);
  if (!waitForLTRData()) {
    return false;
  }
  uv = ltr.readUVS();

  // Read raw ambient-light count.
  ltr.setMode(LTR390_MODE_ALS);
  delay(20);
  if (!waitForLTRData()) {
    return false;
  }
  al = ltr.readALS();

  return true;
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

// ============================================================
// Setup
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("ESP32 station E22 direct-UART production candidate");
  Serial.printf("Radio pins: RX=%u TX=%u AUX=%u M0=%u M1=%u\n",
                LORA_RX, LORA_TX, LORA_AUX, LORA_M0, LORA_M1);
  Serial.println("Radio: address 00:01, channel 60, fixed mode, C2 temporary config");

  // Pi serial link. Do not change these pins when configuring E22.
  WXSerial.begin(WX_BAUD, SERIAL_8N1, WX_RX, WX_TX);
  
  Serial.printf(
    "WX UART: Serial1 RX=%u TX=%u baud=%lu\n",
    WX_RX,
    WX_TX,
    static_cast<unsigned long>(WX_BAUD)
  );
  
  Serial.printf(
      "Pi UART1 ready: RX=%u TX=%u baud=%lu\n",
      WX_RX,
      WX_TX,
      static_cast<unsigned long>(WX_BAUD)
  );

  // UV/ambient-light sensor.
  Wire.begin(I2C_SDA, I2C_SCL);
  ltrAvailable = ltr.begin();

  if (ltrAvailable) {
    ltr.setGain(LTR390_GAIN_3);
    ltr.setResolution(LTR390_RESOLUTION_18BIT);
    ltr.setMode(LTR390_MODE_UVS);
    Serial.println("LTR390 ready");
  } else {
    Serial.println("LTR390 not found; UV and AL will be sent as zero");
  }

  // Radio configuration copied from the successful three-node bench test.
  startE22();

  Serial.println("Waiting for Pi weather packets");
}

// ============================================================
// Main loop
// ============================================================
void loop() {
  retryE22IfOffline();

  // Do NOT consume the incoming Pi UART here. readPacketLine() below
  // owns this UART and must see every byte, including the '@' marker.

  String piBody;

  if (!readPacketLine(rxLine, sizeof(rxLine))) {
    return;
  }

  Serial.print("Pi packet: ");
  Serial.print(rxLine);

  if (!extractAndValidateBody(rxLine, piBody)) {
    Serial.println("Invalid Pi packet; not transmitted");
    return;
  }

  uint32_t uv = 0;
  uint32_t al = 0;

  if (!readLTR390(uv, al)) {
    Serial.println("LTR390 read failed; using UV=0, AL=0");
    uv = 0;
    al = 0;
  }

  // Pi sends 11 fields. Station adds UV and AL, producing 13 fields.
  // The midpoint later appends its battery voltage as field 14.
  const String outgoingBody =
      piBody + "," + String(uv) + "," + String(al);

  const String outgoingPacket = makePacket(outgoingBody);

  if (!e22Ready) {
    Serial.println("E22 OFFLINE: valid Pi packet processed but NOT transmitted");
    return;
  }

  if (sendE22Fixed(MIDPOINT_ADDL, outgoingPacket)) {
    Serial.print("Station fixed UART TX: ");
    Serial.print(outgoingPacket);
  } else {
    Serial.println("ERROR: Station E22 transmit queue failed");
  }
}

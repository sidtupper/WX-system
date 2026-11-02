#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_LTR390.h>
#include "LoRa_E22.h"

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
HardwareSerial LoraSerial(2);
constexpr uint8_t LORA_RX  = 16;  // ESP32 RX <- E22 TXD
constexpr uint8_t LORA_TX  = 17;  // ESP32 TX -> E22 RXD
constexpr uint8_t LORA_AUX = 18;
constexpr uint8_t LORA_M0  = 21;
constexpr uint8_t LORA_M1  = 19;

LoRa_E22 e22(
    &LoraSerial,
    LORA_AUX,
    LORA_M0,
    LORA_M1,
    UART_BPS_RATE_9600
);

// ------------------------------------------------------------
// E22 fixed-address network
// ------------------------------------------------------------
constexpr uint8_t STATION_ADDH  = 0x00;
constexpr uint8_t STATION_ADDL  = 0x01;
constexpr uint8_t MIDPOINT_ADDH = 0x00;
constexpr uint8_t MIDPOINT_ADDL = 0x02;
constexpr uint8_t LORA_CHANNEL  = 23;

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
      Serial.printf("Pi RX byte: 0x%02X %c\n", static_cast<uint8_t>(c), (c >= 32 && c <= 126) ? c : '.');

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
// E22 configuration
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
    return false;
  }

  Configuration c = *(Configuration *)current.data;
  printE22Config("E22 before", c);
  current.close();

  normalizeE22Config(c, STATION_ADDH, STATION_ADDL, LORA_CHANNEL);
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
    return false;
  }
  Configuration actual = *(Configuration *)check.data;
  printE22Config("E22 actual", actual);
  check.close();

  bool ok = (actual.ADDH == STATION_ADDH && actual.ADDL == STATION_ADDL
          && actual.CHAN == LORA_CHANNEL && actual.NETID == 0
          && actual.SPED.uartBaudRate == UART_BPS_9600
          && actual.SPED.airDataRate == AIR_DATA_RATE_010_24
          && actual.SPED.uartParity == MODE_00_8N1
          && actual.TRANSMISSION_MODE.fixedTransmission == FT_FIXED_TRANSMISSION
          && actual.CRYPT.CRYPT_H == 0 && actual.CRYPT.CRYPT_L == 0);
  Serial.println(ok ? "E22 configuration VERIFIED" : "ERROR: E22 readback mismatch");
  return ok;
}

// ============================================================
// Setup
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("ESP32 station fixed-address transmitter starting");

  Serial.printf(
    "Pins: RX=%u TX=%u AUX=%u M0=%u M1=%u\n",
    LORA_RX,
    LORA_TX,
    LORA_AUX,
    LORA_M0,
    LORA_M1
  );

  Serial.printf(
    "Before E22 begin: AUX=%d M0=%d M1=%d\n",
    digitalRead(LORA_AUX),
    digitalRead(LORA_M0),
    digitalRead(LORA_M1)
  );

  Serial.println();
  Serial.println("ESP32 station fixed-address transmitter starting");

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

  // E22 UART2 and module initialization.
  LoraSerial.begin(
      9600,
      SERIAL_8N1,
      LORA_RX,
      LORA_TX
  );

  if (!e22.begin()) {
    Serial.println("E22 begin failed");
  } else if (!configureE22()) {
    Serial.println("E22 configuration failed");
  } else {
    Serial.println("E22 ready");
  }

  Serial.println("Waiting for Pi weather packets");
}

// ============================================================
// Main loop
// ============================================================
void loop() {

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

  ResponseStatus status = e22.sendFixedMessage(
      MIDPOINT_ADDH,
      MIDPOINT_ADDL,
      LORA_CHANNEL,
      outgoingPacket
  );

  Serial.printf(
    "E22 send result: code=%d, %s\n",
    status.code,
    status.getResponseDescription().c_str()
  );


  if (status.code == 1) {
    Serial.print("Sent fixed to midpoint: ");
    Serial.print(outgoingPacket);
  } else {
    Serial.printf(
        "E22 fixed send failed: code=%d description=%s\n",
        status.code,
        status.getResponseDescription().c_str()
    );
  }
}

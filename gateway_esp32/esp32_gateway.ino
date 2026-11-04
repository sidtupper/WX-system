// WX-system gateway — E22 safe-start diagnostics, 2026-11-04
// Unverified candidate; compile and bench-test before deployment.
#include <Arduino.h>
#include "LoRa_E22.h"

// Gateway ESP32 <-> Raspberry Pi
HardwareSerial &GWSerial = Serial1;
constexpr uint8_t GW_RX = 22;
constexpr uint8_t GW_TX = 23;

// Gateway ESP32 <-> E22
HardwareSerial LoraSerial(2);
constexpr uint8_t LORA_RX  = 16;  // ESP32 RX <- E22 TXD
constexpr uint8_t LORA_TX  = 17;  // ESP32 TX -> E22 RXD
constexpr uint8_t LORA_AUX = 18;
constexpr uint8_t LORA_M0  = 21;
constexpr uint8_t LORA_M1  = 19;

LoRa_E22 e22(&LoraSerial, LORA_AUX, LORA_M0, LORA_M1,
             UART_BPS_RATE_9600);

constexpr uint8_t GATEWAY_ADDH = 0x00;
constexpr uint8_t GATEWAY_ADDL = 0x03;
constexpr uint8_t LORA_CHANNEL = 23;
bool e22Ready = false; // True only after configuration readback matches.

constexpr size_t MAX_PACKET_CHARS = 220;

bool isHexDigitString(const String &text) {
  if (text.length() != 4) return false;
  for (size_t i = 0; i < text.length(); ++i) {
    if (!isxdigit(static_cast<unsigned char>(text[i]))) return false;
  }
  return true;
}

bool looksLikePacket(String packet) {
  packet.trim();
  if (packet.length() < 8 || packet.length() > MAX_PACKET_CHARS) return false;
  if (packet[0] != '@') return false;

  const int firstStar = packet.indexOf('*');
  const int lastStar = packet.lastIndexOf('*');
  if (firstStar <= 1 || firstStar != lastStar) return false;

  return isHexDigitString(packet.substring(firstStar + 1));
}

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

  if (current.data == nullptr) {
    Serial.println("ERROR: E22 returned an empty configuration buffer");
    return false;
  }
  Configuration c = *(Configuration *)current.data;
  printE22Config("E22 before", c);
  current.close();

  normalizeE22Config(c, GATEWAY_ADDH, GATEWAY_ADDL, LORA_CHANNEL);
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
  if (check.data == nullptr) {
    Serial.println("ERROR: E22 returned an empty configuration buffer");
    return false;
  }
  Configuration actual = *(Configuration *)check.data;
  printE22Config("E22 actual", actual);
  check.close();

  bool ok = (actual.ADDH == GATEWAY_ADDH && actual.ADDL == GATEWAY_ADDL
          && actual.CHAN == LORA_CHANNEL && actual.NETID == 0
          && actual.SPED.uartBaudRate == UART_BPS_9600
          && actual.SPED.airDataRate == AIR_DATA_RATE_010_24
          && actual.SPED.uartParity == MODE_00_8N1
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

void setup() {
  Serial.begin(115200);
  delay(300);

  GWSerial.begin(9600, SERIAL_8N1, GW_RX, GW_TX);
  LoraSerial.begin(9600, SERIAL_8N1, LORA_RX, LORA_TX);
  const bool beginOK = e22.begin();
  Serial.printf("E22 begin: %s\n", beginOK ? "OK (not proof of radio)" : "FAILED");
  Serial.printf("E22 GPIO after begin: AUX=%d M0=%d M1=%d\n",
                digitalRead(LORA_AUX), digitalRead(LORA_M0), digitalRead(LORA_M1));
  if (beginOK) e22Ready = configureE22();
  if (e22Ready) {
    Serial.println("Gateway E22 VERIFIED; receiver enabled");
  } else {
    Serial.println("Gateway E22 OFFLINE: RF receive disabled."
                   " Expected with bare ESP32; AUX level is meaningless without a radio.");
  }
  Serial.println("Gateway application running");
}

void loop() {
  if (!e22Ready) {
    delay(100);
    return;
  }
  if (!e22.available()) {
    delay(1);
    return;
  }

  ResponseContainer response = e22.receiveMessage();
  if (response.status.code != 1) {
    Serial.printf("E22 receive error: %d\n", response.status.code);
    return;
  }

  String received = response.data;

  // The E22/library can occasionally append or deliver an isolated garbage
  // byte (often 0xFF). Find the real packet start and retain exactly the
  // @...*CCCC frame, discarding transport noise before or after it.
  const int packetStart = received.indexOf('@');
  if (packetStart < 0) {
    return;
  }

  const int starPosition = received.indexOf('*', packetStart + 1);
  if (starPosition < 0 || received.length() < starPosition + 5) {
    Serial.println("Rejected incomplete packet: " + received);
    return;
  }

  // Four hexadecimal CRC characters follow '*'. Anything after them is noise.
  String packet = received.substring(packetStart, starPosition + 5);

  if (!looksLikePacket(packet)) {
    Serial.println("Rejected malformed packet: " + packet);
    return;
  }

  Serial.println("LoRa RX: " + packet);

  // gateway.py previously received a newline-terminated packet.
  GWSerial.print(packet);
  GWSerial.print("\r\n");
}

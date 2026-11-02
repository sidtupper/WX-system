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

bool configureE22() {
  ResponseStructContainer container = e22.getConfiguration();
  if (container.status.code != 1) {
    Serial.printf("E22 getConfiguration error: %d\n", container.status.code);
    container.close();
    return false;
  }

  Configuration configuration = *(Configuration *)container.data;
  configuration.ADDH = GATEWAY_ADDH;
  configuration.ADDL = GATEWAY_ADDL;
  configuration.CHAN = LORA_CHANNEL;
  configuration.TRANSMISSION_MODE.fixedTransmission = FT_FIXED_TRANSMISSION;

  ResponseStatus status =
      e22.setConfiguration(configuration, WRITE_CFG_PWR_DWN_SAVE);
  container.close();

  if (status.code != 1) {
    Serial.printf("E22 setConfiguration error: %d\n", status.code);
    return false;
  }
  return true;
}

void setup() {
  Serial.begin(115200);
  delay(300);

  GWSerial.begin(9600, SERIAL_8N1, GW_RX, GW_TX);
  LoraSerial.begin(9600, SERIAL_8N1, LORA_RX, LORA_TX);
  e22.begin();

  if (!configureE22()) {
    Serial.println("WARNING: E22 configuration failed");
  }

  Serial.println("Gateway fixed-address receiver ready");
}

void loop() {
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

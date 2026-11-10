// WX-system gateway -- direct-UART E22 production integration candidate, 2026-11-09
// Based on verified three-node RF bench test; NOT yet end-to-end validated.
#include <Arduino.h>


// Gateway ESP32 <-> Raspberry Pi
HardwareSerial &GWSerial = Serial1;
constexpr uint8_t GW_RX = 22;
constexpr uint8_t GW_TX = 23;

// Gateway ESP32 <-> E22
HardwareSerial E22Serial(2);
constexpr uint8_t LORA_RX  = 16;  // ESP32 RX <- E22 TXD
constexpr uint8_t LORA_TX  = 17;  // ESP32 TX -> E22 RXD
constexpr uint8_t LORA_AUX = 18;
constexpr uint8_t LORA_M0  = 21;
constexpr uint8_t LORA_M1  = 19;



constexpr uint8_t GATEWAY_ADDH = 0x00;
constexpr uint8_t GATEWAY_ADDL = 0x03;
constexpr uint8_t LORA_CHANNEL = 60; // Bench-verified: 910.125 MHz
constexpr uint8_t E22_LOCAL_ADDRESS = GATEWAY_ADDL;
bool e22Ready = false; // True only when raw UART config verifies.

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

// Assemble complete newline-terminated E22 messages. Time-out handles a
// sender that transmits a final frame without CR/LF.
String receivedLine;
uint32_t lastRadioByte = 0;
constexpr uint32_t RADIO_GAP_MS = 150;

bool pollE22Packet(String &packet) {
  while (E22Serial.available()) {
    const char c = static_cast<char>(E22Serial.read());
    lastRadioByte = millis();
    if (c == '\n') {
      if (receivedLine.length() == 0) continue;
      packet = receivedLine;
      receivedLine = "";
      packet.trim();
      return packet.length() > 0;
    }
    if (receivedLine.length() < MAX_PACKET_CHARS + 16) {
      receivedLine += c;
    } else {
      Serial.println("Gateway radio RX overlength: discard");
      receivedLine = "";
    }
  }
  if (receivedLine.length() && millis() - lastRadioByte > RADIO_GAP_MS) {
    packet = receivedLine;
    receivedLine = "";
    packet.trim();
    return packet.length() > 0;
  }
  return false;
}

void setup() {
  Serial.begin(115200);
  delay(300);

  GWSerial.begin(9600, SERIAL_8N1, GW_RX, GW_TX);
  startE22();
  Serial.println("Gateway application running");
}

void loop() {
  retryE22IfOffline();
  if (!e22Ready) {
    delay(100);
    return;
  }
  String received;
  if (!pollE22Packet(received)) {
    delay(1);
    return;
  }

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

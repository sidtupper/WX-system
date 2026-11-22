// WX-system: three-node E22 fixed-address bench test
// 2026-11-09. Read-only at first, followed by TEMPORARY radio configuration.
// Uses the ESP32 HardwareSerial API ONLY (no LoRa_E22 library).
// ESP32 Dev Module; Serial Monitor 115200; E22 UART 9600 8N1.
// Each file differs only in NODE_ROLE. Do not use in production.

#include <Arduino.h>
#include <string.h>

#define NODE_ROLE 2  // 1=STATION 2=MIDPOINT 3=GATEWAY

#if NODE_ROLE == 1
  const char* const NODE_NAME = "STATION";
#elif NODE_ROLE == 2
  const char* const NODE_NAME = "MIDPOINT";
#elif NODE_ROLE == 3
  const char* const NODE_NAME = "GATEWAY";
#else
  #error Invalid NODE_ROLE
#endif

constexpr uint8_t E22_RX = 16;  // ESP32 RX2 <- E22 TXD
constexpr uint8_t E22_TX = 17;  // ESP32 TX2 -> E22 RXD
constexpr uint8_t E22_AUX = (NODE_ROLE == 2) ? 4 : 18;
constexpr uint8_t E22_M0 = 21;
constexpr uint8_t E22_M1 = 19;
constexpr uint8_t MIDPOINT_CHARGE_CONTROL = 13;  // HIGH = inhibit

constexpr uint8_t ADDRESS_HIGH = 0x00;
constexpr uint8_t ADDRESS_LOW = NODE_ROLE; // 01, 02, 03
// E22-900T30D: frequency(MHz) = 850.125 + channel.
// 60 -> 910.125 MHz; do not use production channel 23 (=873.125 MHz) in Canada.
constexpr uint8_t RADIO_CHANNEL = 60;
constexpr uint8_t SERIAL_SETTING = 0x62;  // 9600 8N1, air data rate 2.4 kbps
constexpr uint8_t OPTION_SETTING = 0x03;  // 240-byte subpackets, minimum TX power (~21dBm)
constexpr uint8_t TRANSMISSION_SETTING = 0x43; // fixed address ON, 2000ms WOR period (ignored in normal mode)
constexpr unsigned long RADIO_RESPONSE_TIMEOUT_MS = 1500;
constexpr unsigned long SEND_EVERY_MS = 5000;

HardwareSerial E22Serial(2);
bool radioReady = false;
char rxBuf[96];
size_t rxUsed = 0;
unsigned long lastRxByte = 0;
uint32_t sequenceNumber = 0;
unsigned long lastSend = 0;

void printHex(const uint8_t* data, size_t length) {
  for (size_t i=0; i<length; ++i) {
    Serial.printf("%s%02X", i ? " " : "", data[i]);
  }
  Serial.println();
}

bool waitAux(unsigned long timeoutMs) {
  unsigned long started=millis();
  while (millis()-started < timeoutMs) {
    if (digitalRead(E22_AUX)==HIGH) return true;
    delay(5);
  }
  return false;
}

bool modeConfiguration() {
  digitalWrite(E22_M0, LOW);
  digitalWrite(E22_M1, HIGH);   // M1=1, M0=0
  delay(150);
  const bool ready = waitAux(3000);
  Serial.printf("CONFIG MODE M0=%d M1=%d AUX=%d ready=%s\n",
                digitalRead(E22_M0), digitalRead(E22_M1),
                digitalRead(E22_AUX), ready ? "YES" : "NO");
  return ready;
}

bool modeNormal() {
  digitalWrite(E22_M0, LOW);
  digitalWrite(E22_M1, LOW);   // M1=0, M0=0
  delay(150);
  const bool ready=waitAux(3000);
  Serial.printf("NORMAL MODE M0=%d M1=%d AUX=%d ready=%s\n",
                digitalRead(E22_M0), digitalRead(E22_M1),
                digitalRead(E22_AUX), ready ? "YES" : "NO");
  return ready;
}

// Reads exactly 12 bytes: C1 00 09 + nine registers.
// Prints the raw response even when invalid; particularly useful for diagnosing error 7.
size_t transaction(const char* label, const uint8_t* request,
                   size_t requestLength, uint8_t reply[12]) {
  while (E22Serial.available()) (void)E22Serial.read(); // discard stale bytes
  Serial.print(label);
  Serial.print(" TX: ");
  printHex(request, requestLength);
  E22Serial.write(request, requestLength);
  E22Serial.flush();

  size_t count=0;
  unsigned long started=millis();
  while (millis()-started < RADIO_RESPONSE_TIMEOUT_MS && count < 12) {
    while (E22Serial.available() && count < 12) {
      reply[count++] = (uint8_t)E22Serial.read();
    }
    if (count==12) break;
    delay(1);
  }
  Serial.printf("%s RX %u/12: ", label, (unsigned)count);
  printHex(reply, count);
  return count;
}

bool validConfigurationReply(const uint8_t* data, size_t n) {
  return n==12 && data[0]==0xC1 && data[1]==0x00 && data[2]==0x09;
}

bool readConfiguration(const char* label, uint8_t reply[12]) {
  const uint8_t request[3]={0xC1,0x00,0x09};
  for (uint8_t tryNum=1; tryNum<=3; ++tryNum) {
    if (!waitAux(1500)) Serial.println("WARNING: AUX did not become high before request");
    size_t n=transaction(label, request, sizeof(request), reply);
    if (validConfigurationReply(reply, n)) return true;
    Serial.printf("Configuration read attempt %u failed\n", tryNum);
    delay(200);
  }
  return false;
}

void showSettings(const char* label, const uint8_t* r) {
  Serial.printf("%s: address=%02X:%02X NETID=%u SPEED=%02X OPTION=%02X channel=%u "
                "MODE=%02X FIXED=%s\n", label, r[3],r[4],r[5],r[6],r[7],
                r[8],r[9], (r[9]&0x40) ? "YES":"NO");
}

bool configureRadio() {
  if (!modeConfiguration()) {
    Serial.println("ERROR: E22 not ready in configuration mode; check power and AUX wiring");
    return false;
  }

  uint8_t initial[12]={0};
  if (!readConfiguration("READ_BEFORE", initial)) {
    Serial.println("ERROR: no valid UART configuration response. RF disabled.");
    return false;
  }
  showSettings("BEFORE", initial);

  // C2 means TEMPORARY register changes: does not persist through an E22 power reset.
  // C0 would permanently save; deliberately NOT used in this bench test.
  const uint8_t temporaryWrite[12]={
    0xC2,0x00,0x09,
    ADDRESS_HIGH, ADDRESS_LOW, 0x00,
    SERIAL_SETTING, OPTION_SETTING, RADIO_CHANNEL,
    TRANSMISSION_SETTING, 0x00,0x00
  };
  uint8_t writtenReply[12]={0};
  size_t writtenCount=transaction("WRITE_TEMP", temporaryWrite,
                                  sizeof(temporaryWrite), writtenReply);
  if (!validConfigurationReply(writtenReply, writtenCount)) {
    Serial.println("ERROR: temporary configuration write not acknowledged. RF disabled.");
    return false;
  }
  delay(120);

  uint8_t after[12]={0};
  if (!readConfiguration("READ_AFTER", after)) {
    Serial.println("ERROR: cannot verify configured registers. RF disabled.");
    return false;
  }
  showSettings("AFTER", after);
  bool matches=after[3]==ADDRESS_HIGH && after[4]==ADDRESS_LOW &&
               after[5]==0x00 && after[6]==SERIAL_SETTING &&
               after[7]==OPTION_SETTING && after[8]==RADIO_CHANNEL &&
               after[9]==TRANSMISSION_SETTING;
  if (!matches) {
    Serial.println("ERROR: E22 readback mismatch. RF disabled.");
    return false;
  }
  Serial.printf("CONFIG VERIFIED: %s address=00:%02X channel=%u (%.3f MHz), fixed mode\n",
                NODE_NAME, ADDRESS_LOW, RADIO_CHANNEL, 850.125f+RADIO_CHANNEL);
  return modeNormal();
}

// E22 fixed-address transmission format: ADDH, ADDL, CHAN, followed by ASCII payload.
void sendFixed(uint8_t recipientLow, const char* payload) {
  if (!radioReady) return;
  if (!waitAux(2000)) {
    Serial.println("ERROR: AUX stayed LOW; TX skipped");
    return;
  }
  const uint8_t header[3]={0x00,recipientLow,RADIO_CHANNEL};
  E22Serial.write(header,3);
  E22Serial.print(payload);
  E22Serial.flush();
  Serial.printf("TX TO 00:%02X CH=%u: %s", recipientLow,RADIO_CHANNEL,payload);
  // UART acceptance is not proof of over-the-air reception.
}

void handleRxLine(const char* line) {
  Serial.printf("RX %s: %s\n", NODE_NAME, line);
#if NODE_ROLE == 2
  if (strncmp(line,"S,M,",4)==0) {
    char forwarded[96];
    snprintf(forwarded, sizeof(forwarded), "M,G,%s\n", line);
    sendFixed(0x03, forwarded);
  } else {
    Serial.println("MIDPOINT: unrecognized payload; not forwarded");
  }
#elif NODE_ROLE == 3
  if (strncmp(line,"M,G,S,M,",8)==0)
    Serial.println("GATEWAY: valid test-chain message RECEIVED");
  else
    Serial.println("GATEWAY: unrecognized test payload");
#endif
}

void pollReceive() {
  while (E22Serial.available()) {
    char c=(char)E22Serial.read();
    lastRxByte=millis();
    if (c=='\r') continue;
    if (c=='\n') {
      if (rxUsed>0) {
        rxBuf[rxUsed]='\0';
        handleRxLine(rxBuf);
      }
      rxUsed=0;
    } else if (rxUsed < sizeof(rxBuf)-1) {
      rxBuf[rxUsed++]=c;
    } else {
      Serial.println("RX too long; line discarded");
      rxUsed=0;
    }
  }
  if (rxUsed && millis()-lastRxByte>1200) {
    rxBuf[rxUsed]='\0';
    Serial.printf("RX incomplete (no newline): %s\n", rxBuf);
    rxUsed=0;
  }
}

void setup() {
  Serial.begin(115200);
  delay(400);
#if NODE_ROLE == 2
  // Inhibit charging during bench test; no DS18B20 or charge algorithm used.
  digitalWrite(MIDPOINT_CHARGE_CONTROL, HIGH); // preload output latch HIGH
  pinMode(MIDPOINT_CHARGE_CONTROL, OUTPUT);
  Serial.println("MIDPOINT charging INHIBITED: GPIO13 HIGH");
#endif
  Serial.println();
  Serial.printf("=== WX E22 MINIMAL TEST: %s ===\n", NODE_NAME);
  Serial.printf("PINOUT: RX=%u TX=%u AUX=%u M0=%u M1=%u\n",
                E22_RX,E22_TX,E22_AUX,E22_M0,E22_M1);
  Serial.println("NO Pi, sensors, CRC, or LoRa_E22 library required");
  Serial.println("Configuration is TEMPORARY (C2), not saved to E22 EEPROM");

  pinMode(E22_M0, OUTPUT);
  pinMode(E22_M1, OUTPUT);
  pinMode(E22_AUX, INPUT);
  E22Serial.begin(9600, SERIAL_8N1, E22_RX, E22_TX);
  radioReady=configureRadio();
  if (!radioReady) {
    digitalWrite(E22_M0,LOW);
    digitalWrite(E22_M1,LOW);
    Serial.println("RF DISABLED: inspect UART/POWER/AUX and restart");
  } else {
    Serial.println("RF ENABLED: E22 configured and ready");
  }
  lastSend=millis();
}

void loop() {
  if (!radioReady) {
    delay(250);
    return;
  }
  pollReceive();
#if NODE_ROLE == 1
  if (millis()-lastSend>=SEND_EVERY_MS) {
    lastSend=millis();
    ++sequenceNumber;
    char packet[48];
    snprintf(packet,sizeof(packet),"S,M,%lu\n",(unsigned long)sequenceNumber);
    sendFixed(0x02,packet);
  }
#endif
  delay(2);
}

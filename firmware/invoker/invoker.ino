//   _____  _   _ __      __ ____   _  __ ______  _____  
//  |_   _|| \ | |\ \    / // __ \ | |/ /|  ____||  __ \ 
//    | |  |  \| | \ \  / /| |  | || ' / | |__   | |__) |
//    | |  | . ` |  \ \/ / | |  | ||  <  |  __|  |  _  / 
//   _| |_ | |\  |   \  /  | |__| || . \ | |____ | | \ \ 
//  |_____||_| \_|    \/    \____/ |_|\_\|______||_|  \_\
//                                                      
// SPIRIT board "Invoker": WiFi + OTA + Furby control
//
// - Connects to WiFi "RenegadeScience_Guest"
// - Enables Arduino OTA updates
// - Listens on TCP port 5000
// - Each TCP line is a hex string, starting with a control byte:
//
//   Control byte modes:
//     0x01 = Initialize coprocessor
//     0x02 = Send nibble stream (Furby bus)
//     0x03 = Drive motor forward  (64-bit duration in microseconds)
//     0x04 = Drive motor backward (64-bit duration in microseconds)
//     0x05 = Read last /RTS high-time samples (up to 300 entries)
//     0xFF = Ping command → replies "31337"
//
//   Format examples:
//     01
//     02 7F341F7F71C0000F000
//     03 00000000000F4240
//     FF
// - Non-hex characters ignored.

#include <WiFi.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>

// =========================
// WIFI CONFIG
// =========================
const char* WIFI_SSID     = "FurbyNet";
const char* WIFI_PASSWORD = NULL;

const char* OTA_HOSTNAME  = "TheSPIRITBoard";
const char* OTA_PASSWORD  = "admin";

// =========================
// PIN CONFIG — UPDATE FOR SPIRIT BOARD
// =========================
const int PIN_D1   = 3;
const int PIN_D2   = 4;
const int PIN_D3   = 5;
const int PIN_D4   = 6;
const int PIN_CTS  = 1;
const int PIN_RTS  = 2;

const int PIN_INIT        = 0;
const int PIN_MOTOR_FWD   = 7;
const int PIN_MOTOR_BWD   = 8;

// =========================
// CONTROL BYTES
// =========================
enum ControlMode : uint8_t {
  CTRL_INIT_COPROCESSOR    = 0x01,
  CTRL_SEND_NIBBLE_STREAM  = 0x02,
  CTRL_MOTOR_FORWARD       = 0x03,
  CTRL_MOTOR_BACKWARD      = 0x04,
  CTRL_READ_RTS_HISTORY    = 0x05,  // NEW: read last RTS high-time samples
  CTRL_PING                = 0xFF   // responds "31337"
};

// =========================
// TIMING CONFIG
// =========================
const uint32_t RTS_TIMEOUT_US = 500000;
const uint32_t CTS_PULSE_US   = 5;

const uint32_t INIT_PULSE_MS  = 10;

const uint64_t MAX_MOTOR_US   = 4000000000ULL;

// =========================
// TCP SERVER
// =========================
WiFiServer tcpServer(5000);

// =========================
// RTS HIGH-TIME HISTORY
// =========================
const size_t RTS_HISTORY_LEN = 300;
uint32_t rtsHighDurations[RTS_HISTORY_LEN];
size_t   rtsHistoryIndex = 0;   // Next write position
size_t   rtsHistoryCount = 0;   // Number of valid samples (<= RTS_HISTORY_LEN)

void recordRTSHighDuration(uint32_t duration_us) {
  rtsHighDurations[rtsHistoryIndex] = duration_us;
  rtsHistoryIndex = (rtsHistoryIndex + 1) % RTS_HISTORY_LEN;
  if (rtsHistoryCount < RTS_HISTORY_LEN) {
    rtsHistoryCount++;
  }
}

// =========================
// WIFI + OTA
// =========================
void setupWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.waitForConnectResult() != WL_CONNECTED) {
    delay(5000);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  }
}

void setupOTA() {
  ArduinoOTA.setHostname(OTA_HOSTNAME);
  ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.begin();
}

// =========================
// FURBY BUS HELPERS
// =========================
bool waitForRTSReady(uint32_t timeout_us) {
  uint32_t start = micros();
  bool sawHigh = false;
  uint32_t highStart = 0;

  while ((micros() - start) < timeout_us) {
    int level = digitalRead(PIN_RTS);

    if (level == HIGH) {
      // We are in a "not ready / busy" window.
      if (!sawHigh) {
        sawHigh = true;
        highStart = micros();
      }
    } else { // level == LOW
      // Line is ready. If we previously saw a HIGH window, record its duration.
      if (sawHigh) {
        uint32_t highDuration = micros() - highStart;
        recordRTSHighDuration(highDuration);
      }
      return true;
    }
  }

  // Timed out before seeing RTS go LOW.
  return false;
}

void sendNibble(uint8_t nibble) {
  nibble &= 0x0F;
  digitalWrite(PIN_D1, nibble & 0x01);
  digitalWrite(PIN_D2, nibble & 0x02);
  digitalWrite(PIN_D3, nibble & 0x04);
  digitalWrite(PIN_D4, nibble & 0x08);

  delayMicroseconds(1);
  digitalWrite(PIN_CTS, LOW);
  delayMicroseconds(CTS_PULSE_US);
  digitalWrite(PIN_CTS, HIGH);
}

bool sendNibbleList(const uint8_t* list, size_t count) {
  for (size_t i = 0; i < count; i++) {
    if (!waitForRTSReady(RTS_TIMEOUT_US)) return false;
    sendNibble(list[i]);
  }
  return true;
}

// =========================
// HEX PARSER
// =========================
bool hexCharToNibble(char c, uint8_t &out) {
  if (c >= '0' && c <= '9') { out = c - '0'; return true; }
  if (c >= 'a' && c <= 'f') { out = 10 + (c - 'a'); return true; }
  if (c >= 'A' && c <= 'F') { out = 10 + (c - 'A'); return true; }
  return false;
}

size_t parseHexNibbles(const String &s, uint8_t *out, size_t max) {
  size_t count = 0;
  for (size_t i = 0; i < s.length(); i++) {
    uint8_t nib;
    if (hexCharToNibble(s[i], nib) && count < max) out[count++] = nib;
  }
  return count;
}

// =========================
// HIGH-LEVEL ACTIONS
// =========================

void doInitCoprocessor() {
  digitalWrite(PIN_INIT, LOW);
  delay(INIT_PULSE_MS);
  digitalWrite(PIN_INIT, HIGH);
}

void driveMotorForward(uint64_t us) {
  if (us > MAX_MOTOR_US) us = MAX_MOTOR_US;
  digitalWrite(PIN_MOTOR_BWD, LOW);
  digitalWrite(PIN_MOTOR_FWD, HIGH);
  delayMicroseconds((uint32_t)us);
  digitalWrite(PIN_MOTOR_FWD, LOW);
}

void driveMotorBackward(uint64_t us) {
  if (us > MAX_MOTOR_US) us = MAX_MOTOR_US;
  digitalWrite(PIN_MOTOR_FWD, LOW);
  digitalWrite(PIN_MOTOR_BWD, HIGH);
  delayMicroseconds((uint32_t)us);
  digitalWrite(PIN_MOTOR_BWD, LOW);
}

// =========================
// COMMAND PROCESSOR
// =========================
void processCommand(uint8_t* nibbles, size_t n, WiFiClient &client) {
  if (n < 2) { client.println("[ERROR] Missing control byte"); return; }

  uint8_t control = (nibbles[0] << 4) | nibbles[1];
  uint8_t* payload = nibbles + 2;
  size_t   pcount  = n - 2;

  switch (control) {
    case CTRL_PING:
      client.println("31337");
      break;

    case CTRL_INIT_COPROCESSOR:
      doInitCoprocessor();
      client.println("[OK] INIT coprocessor");
      break;

    case CTRL_SEND_NIBBLE_STREAM:
      if (pcount == 0) { client.println("[ERROR] No payload"); return; }
      if (sendNibbleList(payload, pcount)) client.println("[OK] Nibbles sent");
      else client.println("[ERROR] RTS timeout");
      break;

    case CTRL_READ_RTS_HISTORY: {
      // Dump the last recorded RTS high-time samples in microseconds.
      client.print("[RTS] ");
      client.print(rtsHistoryCount);
      client.print(" samples: ");

      if (rtsHistoryCount == 0) {
        client.println("none");
      } else {
        // Oldest sample first.
        for (size_t i = 0; i < rtsHistoryCount; i++) {
          size_t idx = (rtsHistoryIndex + RTS_HISTORY_LEN - rtsHistoryCount + i) % RTS_HISTORY_LEN;
          uint32_t val = rtsHighDurations[idx];
          client.print(val);
          if (i + 1 < rtsHistoryCount) client.print(",");
        }
        client.println();
      }
      break;
    }

    case CTRL_MOTOR_FORWARD:
    case CTRL_MOTOR_BACKWARD: {
      if (pcount < 16) { client.println("[ERROR] Need 16 nibbles for duration"); return; }
      uint64_t us = 0;
      for (int i = 0; i < 16; i++) us = (us << 4) | payload[i];

      if (control == CTRL_MOTOR_FORWARD) {
        driveMotorForward(us);
        client.println("[OK] Motor forward");
      } else {
        driveMotorBackward(us);
        client.println("[OK] Motor backward");
      }
      break;
    }

    default:
      client.println("[ERROR] Unknown control byte");
      break;
  }
}

// =========================
// TCP HANDLING
// =========================
void handleClient(WiFiClient &client) {
  client.setTimeout(5000);
  String line = client.readStringUntil('\n');
  line.trim();
  if (!line.length()) { client.println("[ERROR] Empty"); return; }

  static uint8_t buf[1024];
  size_t count = parseHexNibbles(line, buf, 1024);
  if (count == 0) { client.println("[ERROR] No hex data"); return; }

  processCommand(buf, count, client);
}

// =========================
// SETUP / LOOP
// =========================
void setup() {
  pinMode(PIN_D1, OUTPUT);
  pinMode(PIN_D2, OUTPUT);
  pinMode(PIN_D3, OUTPUT);
  pinMode(PIN_D4, OUTPUT);

  pinMode(PIN_CTS, OUTPUT);
  digitalWrite(PIN_CTS, HIGH);

  pinMode(PIN_RTS, INPUT_PULLUP);

  pinMode(PIN_INIT, OUTPUT);
  digitalWrite(PIN_INIT, HIGH);

  pinMode(PIN_MOTOR_FWD, OUTPUT);
  pinMode(PIN_MOTOR_BWD, OUTPUT);

  digitalWrite(PIN_MOTOR_FWD, LOW);
  digitalWrite(PIN_MOTOR_BWD, LOW);

  setupWiFi();
  setupOTA();
  tcpServer.begin();
}

void loop() {
  ArduinoOTA.handle();
  WiFiClient client = tcpServer.available();
  if (client) {
    handleClient(client);
    client.stop();
  }
}

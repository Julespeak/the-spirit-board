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

const uint32_t INIT_RTS_GAP_US = 10000;  // 10 ms of LOW on /RTS marks end of init pulse train

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

void clearRTSHistory() {
  rtsHistoryIndex = 0;
  rtsHistoryCount = 0;
}

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
bool waitForRTSReady(uint32_t timeout_us, bool logDuration) {
  uint32_t start     = micros();
  uint32_t highStart = 0;
  bool     sawHigh   = false;

  // Read the initial level
    int level = digitalRead(PIN_RTS);

  // -----------------------------------------------------------
  // Stage 1: Wait for LOW -> HIGH (start of busy window)
  // -----------------------------------------------------------

  if (level == HIGH) {
    // We entered while RTS is already HIGH: treat this as
    // the start of the busy interval.
    sawHigh   = true;
    highStart = micros();
  } else {
    // RTS is LOW: wait for it to go HIGH, but don't block forever.
    while ((micros() - start) < timeout_us) {
      level = digitalRead(PIN_RTS);
    if (level == HIGH) {
        sawHigh   = true;
        highStart = micros();
        break;
      }
    }
  }

  // If we never saw RTS go HIGH within the timeout, we likely
  // missed a very short pulse or there was no busy interval at all.
  // In either case, record 0 (if requested) so the caller sees that
  // "something" happened for this nibble, and treat the bus as ready.
  if (!sawHigh) {
    if (logDuration) {
      recordRTSHighDuration(0);
    }
    // Line is either LOW (already ready) or toggled too fast for us
    // to see; from the caller's perspective, we can continue.
    return true;
  }

  // -----------------------------------------------------------
  // Stage 2: Wait for HIGH -> LOW (end of busy window)
  // -----------------------------------------------------------

  while ((micros() - start) < timeout_us) {
    level = digitalRead(PIN_RTS);
    if (level == LOW) {
      if (logDuration) {
        uint32_t duration = micros() - highStart;
        recordRTSHighDuration(duration);
      }
      return true;
    }
  }

  // -----------------------------------------------------------
  // Timeout: RTS stayed HIGH (or never fell) for too long.
  // We still log whatever duration we observed, but signal
  // failure back to the caller.
  // -----------------------------------------------------------

  if (logDuration) {
    uint32_t duration = micros() - highStart;
    recordRTSHighDuration(duration);
  }
  return false;
}

// Convenience wrapper: default is to log
bool waitForRTSReady(uint32_t timeout_us) {
  return waitForRTSReady(timeout_us, true);
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
  if (count == 0) return true;

  // Make sure bus is ready before we begin, but don't log this interval
  if (!waitForRTSReady(RTS_TIMEOUT_US, false)) return false;

  // Reset the history so it corresponds only to this stream
  clearRTSHistory();

  for (size_t i = 0; i < count; i++) {
    // Send the nibble
    sendNibble(list[i]);

    // Now measure how long RTS stays HIGH until it goes LOW (busy time for this nibble)
    if (!waitForRTSReady(RTS_TIMEOUT_US, true)) {
      return false;
    }
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

size_t doInitCoprocessor() {
//  // Clear any previous RTS history so this capture is only the init pulse train.
//  clearRTSHistory();

  // Pulse /INIT LOW then back HIGH to kick the coprocessor.
  digitalWrite(PIN_INIT, LOW);
  delay(INIT_PULSE_MS);
  digitalWrite(PIN_INIT, HIGH);

  // Clear any previous RTS history so this capture is only the init pulse train.
  clearRTSHistory();

  // Now watch /RTS for a sequence of HIGH pulses. For each pulse, we measure how
  // long /RTS stays HIGH. We stop when /RTS has remained LOW for more than
  // INIT_RTS_GAP_US (10 ms), or when a reasonable overall timeout expires.
  const uint32_t overallTimeoutUs = 500000;  // 0.5 s safety cap
  uint32_t startOverall = micros();

  bool     inHigh      = false;
  uint32_t highStartUs = 0;

  // Track how long we've been continuously LOW between pulses.
  uint32_t lowStartUs = micros();
  int lastLevel = digitalRead(PIN_RTS);

  if (lastLevel == LOW) {
    lowStartUs = micros();
  } else {
    // If we start HIGH, treat as already in a pulse.
    inHigh      = true;
    highStartUs = micros();
  }

  while ((micros() - startOverall) < overallTimeoutUs) {
    int level = digitalRead(PIN_RTS);
    uint32_t now = micros();

    if (!inHigh) {
      // Currently LOW. Look for LOW -> HIGH to start a new pulse.
      if (level == HIGH) {
        inHigh      = true;
        highStartUs = now;
      } else {
        // Still LOW; see if we've been LOW long enough to declare "done".
        if ((now - lowStartUs) >= INIT_RTS_GAP_US) {
          break;
        }
      }
    } else {
      // Currently in a HIGH pulse. Look for HIGH -> LOW to finish the pulse.
      if (level == LOW) {
        uint32_t duration = now - highStartUs;
        recordRTSHighDuration(duration);
        inHigh     = false;
        lowStartUs = now;
      }
    }

    // Small delay to avoid a completely hot spin; still much shorter than the
    // expected ~740 µs HIGH/LOW windows.
    delayMicroseconds(1);
  }

  return rtsHistoryCount;
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

    case CTRL_INIT_COPROCESSOR: {
      // Perform the /INIT pulse and capture the resulting /RTS HIGH pulses.
      size_t count = doInitCoprocessor();

      client.print("[RTS_INIT] ");
      client.print(count);
      client.print(" samples: ");

      if (count == 0) {
        client.println("none");
      } else {
        // Oldest sample first, same ordering as CTRL_READ_RTS_HISTORY.
        for (size_t i = 0; i < count; i++) {
          size_t idx = (rtsHistoryIndex + RTS_HISTORY_LEN - count + i) % RTS_HISTORY_LEN;
          uint32_t val = rtsHighDurations[idx];
          client.print(val);
          if (i + 1 < count) client.print(",");
        }
        client.println();
      }
      break;
    }

    case CTRL_SEND_NIBBLE_STREAM: {
      if (pcount == 0) {
        client.println("[ERROR] No payload");
        return;
      }

      // --- Sanity: echo back exactly what we decoded from the hex line ---
      client.print("[NIBBLES] count=");
      client.print(pcount);
      client.print(" : ");

      for (size_t i = 0; i < pcount; i++) {
        // Print each payload byte as hex; if you know each byte is just a nibble (0–15),
        // this will print a single hex digit per entry.
        client.print(payload[i], HEX);
        if (i + 1 < pcount) client.print(",");
      }
      client.println();  // end the [NIBBLES] line
      // -------------------------------------------------------------------

      if (sendNibbleList(payload, pcount)) {
        client.println("[OK] Nibbles sent");
      } else {
        client.println("[ERROR] RTS timeout");
      }
      break;
    }

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
  digitalWrite(PIN_D1, LOW);
  pinMode(PIN_D2, OUTPUT);
  digitalWrite(PIN_D2, LOW);
  pinMode(PIN_D3, OUTPUT);
  digitalWrite(PIN_D3, LOW);
  pinMode(PIN_D4, OUTPUT);
  digitalWrite(PIN_D4, LOW);

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

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
//     0x05 = Read last /RTS high-time samples
//     0x06 = Debug: directly drive CTS + D1–D4 from a 5-bit pattern
//     0x07 = Poll /RTS + feed/tummy/back buttons and store snapshot
//     0xFF = Ping (responds "31337")
//
//   Format examples:
//     01
//     02 7F341F7F71C0000F000
//     03 00000000000F4240
//     07
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

const int PIN_BTN_FEED  = 15;
const int PIN_BTN_TUMMY = 20;
const int PIN_BTN_BACK  = 21;

// =========================
// CONTROL BYTES
// =========================
enum ControlMode : uint8_t {
  CTRL_INIT_COPROCESSOR    = 0x01,
  CTRL_SEND_NIBBLE_STREAM  = 0x02,
  CTRL_MOTOR_FORWARD       = 0x03,
  CTRL_MOTOR_BACKWARD      = 0x04,
  CTRL_READ_RTS_HISTORY    = 0x05,  // read last RTS high-time samples
  CTRL_DEBUG_BUS_DRIVE     = 0x06,  // directly drive CTS + D1–D4
  CTRL_POLL_INPUTS         = 0x07,  // sample RTS + buttons into snapshot register
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
const size_t RTS_HISTORY_LEN = 3000;
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
// INPUT SNAPSHOT REGISTER
// =========================
//
// Bit layout for the "state" field:
//   bit0 -> RTS   (1 = HIGH, 0 = LOW)
//   bit1 -> FEED  (1 = HIGH, 0 = LOW)
//   bit2 -> TUMMY (1 = HIGH, 0 = LOW)
//   bit3 -> BACK  (1 = HIGH, 0 = LOW)

enum InputBits : uint8_t {
  INPUT_BIT_RTS   = 0x01,
  INPUT_BIT_FEED  = 0x02,
  INPUT_BIT_TUMMY = 0x04,
  INPUT_BIT_BACK  = 0x08
};

struct InputSnapshot {
  uint32_t timestamp_ms;
  uint8_t  state;
};

InputSnapshot g_lastInputSnapshot = {0, 0};

void sampleFurbyInputs() {
  uint8_t s = 0;

  int rts   = digitalRead(PIN_RTS);
  int feed  = digitalRead(PIN_BTN_FEED);
  int tummy = digitalRead(PIN_BTN_TUMMY);
  int back  = digitalRead(PIN_BTN_BACK);

  if (rts   == HIGH) s |= INPUT_BIT_RTS;
  if (feed  == HIGH) s |= INPUT_BIT_FEED;
  if (tummy == HIGH) s |= INPUT_BIT_TUMMY;
  if (back  == HIGH) s |= INPUT_BIT_BACK;

  g_lastInputSnapshot.state        = s;
  g_lastInputSnapshot.timestamp_ms = millis();
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

// Drive CTS + D1–D4 to explicit levels for continuity / wiring tests.
// pattern bit mapping (LSB-first):
//   bit0 -> D1
//   bit1 -> D2
//   bit2 -> D3
//   bit3 -> D4
//   bit4 -> CTS
void driveDebugBus(uint8_t pattern) {
  pattern &= 0x1F;  // keep only 5 bits

  bool d1  = pattern & 0x01;
  bool d2  = pattern & 0x02;
  bool d3  = pattern & 0x04;
  bool d4  = pattern & 0x08;
  bool cts = pattern & 0x10;

  digitalWrite(PIN_D1, d1 ? HIGH : LOW);
  digitalWrite(PIN_D2, d2 ? HIGH : LOW);
  digitalWrite(PIN_D3, d3 ? HIGH : LOW);
  digitalWrite(PIN_D4, d4 ? HIGH : LOW);
  digitalWrite(PIN_CTS, cts ? HIGH : LOW);
}

// New nibble sender: /CTS low, wait /RTS high->low, then /CTS high.
bool sendNibble(uint8_t nibble) {
  nibble &= 0x0F;

  // Put the nibble onto the 4 data lines.
  digitalWrite(PIN_D1, nibble & 0x01);
  digitalWrite(PIN_D2, nibble & 0x02);
  digitalWrite(PIN_D3, nibble & 0x04);
  digitalWrite(PIN_D4, nibble & 0x08);

  delayMicroseconds(10);

  // Start the handshake by pulling /CTS LOW.
  // From your logic trace: /RTS activity happens while /CTS is LOW.
  digitalWrite(PIN_CTS, LOW);

  uint32_t start     = micros();
  uint32_t highStart = 0;
  bool     sawHigh   = false;

  // Read the initial RTS level.
  int level = digitalRead(PIN_RTS);

  // -----------------------------------------------------------
  // Stage 1: Wait for LOW -> HIGH (start of busy window)
  // -----------------------------------------------------------

  if (level == HIGH) {
    // If RTS is already HIGH when we pull CTS low, treat this as
    // the beginning of the busy interval.
    sawHigh   = true;
    highStart = micros();
  } else {
    // RTS is LOW: wait for it to go HIGH, but don't block forever.
    while ((micros() - start) < RTS_TIMEOUT_US) {
      level = digitalRead(PIN_RTS);
      if (level == HIGH) {
        sawHigh   = true;
        highStart = micros();
        break;
      }
    }
  }

  if (!sawHigh) {
    // Never saw RTS go HIGH while CTS was LOW.
    // Log a 0-duration pulse for this nibble so we see the failure.
    recordRTSHighDuration(0);

    // Release CTS so the bus isn't stuck.
    digitalWrite(PIN_CTS, HIGH);
    return false;
  }

  // -----------------------------------------------------------
  // Stage 2: Wait for HIGH -> LOW (end of busy window)
  // -----------------------------------------------------------

  // Now that we have seen the command get latched, we can return /CTS high
  delayMicroseconds(5);
  digitalWrite(PIN_CTS, HIGH);

  while ((micros() - start) < RTS_TIMEOUT_US) {
    level = digitalRead(PIN_RTS);
    if (level == LOW) {
      uint32_t duration = micros() - highStart;
      recordRTSHighDuration(duration);

      return true;
    }
  }

  // If we get here, RTS stayed HIGH for the entire timeout.
  // Record whatever duration we observed and still release CTS.
  {
    uint32_t duration = micros() - highStart;
    recordRTSHighDuration(duration);
  }

  digitalWrite(PIN_CTS, HIGH);
  return false;
}

bool sendNibbleList(const uint8_t* list, size_t count) {
  if (count == 0) return true;

  // This stream’s RTS history should only reflect these nibbles.
  clearRTSHistory();

  for (size_t i = 0; i < count; i++) {
    if (!sendNibble(list[i])) {
      // If any nibble handshake fails (timeout, etc.), abort.
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

    case CTRL_DEBUG_BUS_DRIVE: {
      // Expect at least 1 byte (= 2 nibbles) of payload; we use the lower 5 bits.
      if (pcount < 2) {
        client.println("[ERROR] DEBUG_BUS requires 2 payload nibbles (00–1F)");
        return;
      }

      // Reconstruct a byte from the first two payload nibbles.
      uint8_t pattern = (payload[0] << 4) | payload[1];
      pattern &= 0x1F;

      driveDebugBus(pattern);

      client.print("[DEBUG] CTS,D4,D3,D2,D1 = ");
      client.print((pattern & 0x10) ? 1 : 0); client.print(",");
      client.print((pattern & 0x08) ? 1 : 0); client.print(",");
      client.print((pattern & 0x04) ? 1 : 0); client.print(",");
      client.print((pattern & 0x02) ? 1 : 0); client.print(",");
      client.print((pattern & 0x01) ? 1 : 0);
      client.println();
      client.println("[OK] Debug bus driven");
      break;
    }

    case CTRL_POLL_INPUTS: {
      // Sample RTS + the three buttons and store to the snapshot register.
      sampleFurbyInputs();

      uint8_t s = g_lastInputSnapshot.state;

      client.print("[INPUT] t_ms=");
      client.print(g_lastInputSnapshot.timestamp_ms);
      client.print(" RTS=");
      client.print((s & INPUT_BIT_RTS) ? 1 : 0);
      client.print(" FEED=");
      client.print((s & INPUT_BIT_FEED) ? 1 : 0);
      client.print(" TUMMY=");
      client.print((s & INPUT_BIT_TUMMY) ? 1 : 0);
      client.print(" BACK=");
      client.print((s & INPUT_BIT_BACK) ? 1 : 0);
      client.println();
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

  static uint8_t buf[3000];
  size_t count = parseHexNibbles(line, buf, 3000);
  if (count == 0) { client.println("[ERROR] No hex data"); return; }

  processCommand(buf, count, client);
}

// =========================
// SETUP / LOOP
// =========================
void setup() {
  pinMode(PIN_BTN_FEED,  INPUT);
  pinMode(PIN_BTN_TUMMY, INPUT);
  pinMode(PIN_BTN_BACK,  INPUT);
  
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

  pinMode(PIN_RTS, INPUT);

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

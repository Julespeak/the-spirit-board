//   _____  _   _ __      __ ____   _  __ ______  _____
//  |_   _|| \ | |\ \    / // __ \ | |/ /|  ____||  __ \
//    | |  |  \| | \ \  / /| |  | || ' / | |__   | |__) |
//    | |  | . ` |  \ \/ / | |  | ||  <  |  __|  |  _  /
//   _| |_ | |\  |   \  /  | |__| || . \ | |____ | | \ \
//  |_____||_| \_|    \/    \____/ |_|\_\|______||_|  \_\
//
// SPIRIT board "Invoker": WiFi + OTA + Furby control
//
// - Connects to WiFi (credentials from build flags)
// - Enables Arduino OTA updates
// - Listens on TCP port 5000
// - Each TCP line is a hex string, starting with a control byte:
//
//   Control byte modes:
//     0x01 = Initialize coprocessor (Spirit only)
//     0x02 = Send nibble stream (Furby bus) (Spirit only)
//     0x03 = Drive motor forward (64-bit duration in microseconds) (Spirit only)
//     0x04 = Drive motor backward (64-bit duration in microseconds) (Spirit only)
//     0x05 = Read event timeline (RTS/CTS edges, data nibbles, motor events) (Spirit only)
//     0x06 = Debug: directly drive CTS + D1-D4 from a 5-bit pattern (Spirit only)
//     0x07 = Poll /RTS + feed/tummy/back buttons and store snapshot (Spirit only)
//     0x08 = Blink LED N times (Feather only)
//     0x09 = Set RGB LED color: 09 RR GG BB (Feather only)
//     0x0A = Record 1s of audio from I2S mic (Feather only): returns binary samples
//     0xFF = Ping (responds "31337")
//
//   Format examples:
//     01
//     02 7F341F7F71C0000F000
//     03 00000000000F4240
//     07
//     08 5    (blink 5 times, Feather only)
//     FF
// - Non-hex characters ignored.

#include <WiFi.h>
#include <WiFiMulti.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include <esp_timer.h>

#ifdef TARGET_FEATHER
#include <Adafruit_NeoPixel.h>
#include "driver/i2s.h"
#endif

// =========================
// WIFI CONFIG - From build flags
// =========================
// Stringify macros for converting build flag values to strings
#define XSTR(x) STR(x)
#define STR(x) #x

// Support up to 4 WiFi networks via build flags.
// Define WIFI_SSID_1/WIFI_PASSWORD_1 through WIFI_SSID_4/WIFI_PASSWORD_4
// in platformio_override.ini. The board will try each in sequence.

WiFiMulti wifiMulti;

// Track connection state for reconnection logic
bool wasConnected = false;
unsigned long lastReconnectAttempt = 0;
const unsigned long RECONNECT_INTERVAL_MS = 10000;  // Try reconnecting every 10s

void addConfiguredNetworks() {
  // Add each configured network to WiFiMulti.
  // Networks are tried in the order they're added.

  #ifdef WIFI_SSID_1
    #ifdef WIFI_PASSWORD_1
      wifiMulti.addAP(XSTR(WIFI_SSID_1), XSTR(WIFI_PASSWORD_1));
      Serial.print("[WiFi] Added network: ");
      Serial.println(XSTR(WIFI_SSID_1));
    #endif
  #endif

  #ifdef WIFI_SSID_2
    #ifdef WIFI_PASSWORD_2
      wifiMulti.addAP(XSTR(WIFI_SSID_2), XSTR(WIFI_PASSWORD_2));
      Serial.print("[WiFi] Added network: ");
      Serial.println(XSTR(WIFI_SSID_2));
    #endif
  #endif

  #ifdef WIFI_SSID_3
    #ifdef WIFI_PASSWORD_3
      wifiMulti.addAP(XSTR(WIFI_SSID_3), XSTR(WIFI_PASSWORD_3));
      Serial.print("[WiFi] Added network: ");
      Serial.println(XSTR(WIFI_SSID_3));
    #endif
  #endif

  #ifdef WIFI_SSID_4
    #ifdef WIFI_PASSWORD_4
      wifiMulti.addAP(XSTR(WIFI_SSID_4), XSTR(WIFI_PASSWORD_4));
      Serial.print("[WiFi] Added network: ");
      Serial.println(XSTR(WIFI_SSID_4));
    #endif
  #endif
}

#ifdef TARGET_SPIRIT
const char* OTA_HOSTNAME  = "TheSPIRITBoard";
#else
const char* OTA_HOSTNAME  = "feather";
#endif
const char* OTA_PASSWORD  = "admin";

// =========================
// PIN CONFIG
// =========================
#ifdef TARGET_SPIRIT
// S.P.I.R.I.T. Board pin definitions
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
#endif

#ifdef TARGET_FEATHER
// Feather ESP32-C6 - NeoPixel RGB LED for testing
// PIN_NEOPIXEL (9) and NEOPIXEL_I2C_POWER (20) are defined by the board variant
Adafruit_NeoPixel pixel(1, PIN_NEOPIXEL, NEO_GRB + NEO_KHZ800);

// I2S Microphone configuration
// These pins should be connected to an I2S MEMS microphone (e.g., SPH0645)
const int I2S_BCLK_PIN = 16;   // Bit clock
const int I2S_WS_PIN   = 17;   // Word select / LRCLK
const int I2S_DATA_PIN = 18;   // Data from microphone

const int I2S_PORT = I2S_NUM_0;
const int MIC_SAMPLE_RATE = 16000;
const int MIC_RECORD_SECONDS = 1;
const int MIC_BUFFER_SAMPLES = 256;  // DMA buffer size
const int MIC_TOTAL_SAMPLES = MIC_SAMPLE_RATE * MIC_RECORD_SECONDS;

// Buffer for recording audio (16-bit samples)
int16_t* audioBuffer = nullptr;
bool i2sInitialized = false;

void setupI2SMic() {
  i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = MIC_SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 4,
    .dma_buf_len = MIC_BUFFER_SAMPLES,
    .use_apll = false,
    .tx_desc_auto_clear = false,
    .fixed_mclk = 0
  };

  i2s_pin_config_t pin_config = {
    .bck_io_num = I2S_BCLK_PIN,
    .ws_io_num = I2S_WS_PIN,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num = I2S_DATA_PIN
  };

  esp_err_t err = i2s_driver_install((i2s_port_t)I2S_PORT, &i2s_config, 0, NULL);
  if (err != ESP_OK) {
    Serial.printf("[I2S] Driver install failed: %d\n", err);
    return;
  }

  err = i2s_set_pin((i2s_port_t)I2S_PORT, &pin_config);
  if (err != ESP_OK) {
    Serial.printf("[I2S] Set pin failed: %d\n", err);
    return;
  }

  i2s_zero_dma_buffer((i2s_port_t)I2S_PORT);

  // Allocate audio buffer
  audioBuffer = (int16_t*)malloc(MIC_TOTAL_SAMPLES * sizeof(int16_t));
  if (audioBuffer == nullptr) {
    Serial.println("[I2S] Failed to allocate audio buffer");
    return;
  }

  i2sInitialized = true;
  Serial.println("[I2S] Microphone initialized");
}

// Record audio and return number of samples captured
int recordAudio() {
  if (!i2sInitialized || audioBuffer == nullptr) {
    return -1;
  }

  int32_t rawBuffer[MIC_BUFFER_SAMPLES];
  int samplesRecorded = 0;

  // Clear buffer
  memset(audioBuffer, 0, MIC_TOTAL_SAMPLES * sizeof(int16_t));

  while (samplesRecorded < MIC_TOTAL_SAMPLES) {
    size_t bytesRead = 0;
    esp_err_t result = i2s_read(
      (i2s_port_t)I2S_PORT,
      rawBuffer,
      sizeof(rawBuffer),
      &bytesRead,
      portMAX_DELAY
    );

    if (result != ESP_OK) {
      break;
    }

    int samplesRead = bytesRead / sizeof(int32_t);
    for (int i = 0; i < samplesRead && samplesRecorded < MIC_TOTAL_SAMPLES; i++) {
      // Convert 32-bit to 16-bit by shifting (SPH0645 data is in upper bits)
      audioBuffer[samplesRecorded++] = (int16_t)(rawBuffer[i] >> 14);
    }
  }

  return samplesRecorded;
}
#endif

// =========================
// CONTROL BYTES
// =========================
enum ControlMode : uint8_t {
  CTRL_INIT_COPROCESSOR    = 0x01,
  CTRL_SEND_NIBBLE_STREAM  = 0x02,
  CTRL_MOTOR_FORWARD       = 0x03,
  CTRL_MOTOR_BACKWARD      = 0x04,
  CTRL_READ_RTS_HISTORY    = 0x05,  // read event timeline (replaces old RTS timing)
  CTRL_DEBUG_BUS_DRIVE     = 0x06,  // directly drive CTS + D1-D4
  CTRL_POLL_INPUTS         = 0x07,  // sample RTS + buttons into snapshot register
  CTRL_BLINK_LED           = 0x08,  // blink LED N times (Feather only)
  CTRL_SET_RGB             = 0x09,  // set RGB LED color (Feather only): 09 RR GG BB
  CTRL_RECORD_AUDIO        = 0x0A,  // record 1s of audio (Feather only): returns binary samples
  CTRL_PING                = 0xFF   // responds "31337"
};

#ifdef TARGET_SPIRIT
// =========================
// EVENT TIMELINE TYPES
// =========================
enum EventType : uint8_t {
  EVT_RTS_RISING    = 0x01,  // RTS: LOW -> HIGH
  EVT_RTS_FALLING   = 0x02,  // RTS: HIGH -> LOW
  EVT_CTS_RISING    = 0x03,  // CTS: LOW -> HIGH
  EVT_CTS_FALLING   = 0x04,  // CTS: HIGH -> LOW
  EVT_DATA_NIBBLE   = 0x05,  // D1-D4 changed (nibble value in data field)
  EVT_MOTOR_FWD_ON  = 0x06,  // Motor forward started
  EVT_MOTOR_FWD_OFF = 0x07,  // Motor forward stopped
  EVT_MOTOR_BWD_ON  = 0x08,  // Motor backward started
  EVT_MOTOR_BWD_OFF = 0x09   // Motor backward stopped
};

struct TimelineEvent {
  uint8_t  event_type;     // EventType enum value
  uint8_t  data;           // Event-specific data (nibble value for EVT_DATA_NIBBLE)
  uint64_t timestamp_us;   // Microseconds from esp_timer_get_time()
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
// EVENT TIMELINE BUFFER
// =========================
const size_t TIMELINE_LEN = 5000;
TimelineEvent timeline[TIMELINE_LEN];
size_t timelineIndex = 0;  // Next write position
size_t timelineCount = 0;  // Number of valid events (<= TIMELINE_LEN)

void clearTimeline() {
  timelineIndex = 0;
  timelineCount = 0;
}

void recordEvent(EventType type, uint8_t data = 0) {
  timeline[timelineIndex].event_type = static_cast<uint8_t>(type);
  timeline[timelineIndex].data = data;
  timeline[timelineIndex].timestamp_us = esp_timer_get_time();

  timelineIndex = (timelineIndex + 1) % TIMELINE_LEN;
  if (timelineCount < TIMELINE_LEN) {
    timelineCount++;
  }
}

// Convenience wrappers for common events
inline void recordRTSRising() { recordEvent(EVT_RTS_RISING); }
inline void recordRTSFalling() { recordEvent(EVT_RTS_FALLING); }
inline void recordCTSRising() { recordEvent(EVT_CTS_RISING); }
inline void recordCTSFalling() { recordEvent(EVT_CTS_FALLING); }
inline void recordDataNibble(uint8_t nibble) { recordEvent(EVT_DATA_NIBBLE, nibble & 0x0F); }

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
#endif  // TARGET_SPIRIT

// =========================
// TCP SERVER
// =========================
WiFiServer tcpServer(5000);

// =========================
// WIFI + OTA
// =========================
void setupWiFi() {
  WiFi.mode(WIFI_STA);

  // Add all configured networks
  addConfiguredNetworks();

  Serial.println("[WiFi] Connecting...");

  // Try to connect (WiFiMulti will try each network in sequence)
  while (wifiMulti.run() != WL_CONNECTED) {
    Serial.println("[WiFi] Connection failed, retrying in 5s...");
    delay(5000);
  }

  wasConnected = true;
  Serial.print("[WiFi] Connected to: ");
  Serial.println(WiFi.SSID());
  Serial.print("[WiFi] IP address: ");
  Serial.println(WiFi.localIP());
}

void setupOTA() {
  ArduinoOTA.setHostname(OTA_HOSTNAME);
  ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.begin();
}

#ifdef TARGET_SPIRIT
// =========================
// FURBY BUS HELPERS
// =========================

// Drive CTS + D1-D4 to explicit levels for continuity / wiring tests.
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

// Drive data bus with 4-bit nibble value
void driveDataBus(uint8_t nibble) {
  nibble &= 0x0F;
  digitalWrite(PIN_D1, nibble & 0x01);
  digitalWrite(PIN_D2, nibble & 0x02);
  digitalWrite(PIN_D3, nibble & 0x04);
  digitalWrite(PIN_D4, nibble & 0x08);
}

// New nibble sender: /CTS low, wait /RTS high->low, then /CTS high.
bool sendNibble(uint8_t nibble) {
  nibble &= 0x0F;

  // Put the nibble onto the 4 data lines.
  digitalWrite(PIN_D1, nibble & 0x01);
  digitalWrite(PIN_D2, nibble & 0x02);
  digitalWrite(PIN_D3, nibble & 0x04);
  digitalWrite(PIN_D4, nibble & 0x08);

  // Record the nibble value being transmitted
  recordDataNibble(nibble);

  // Allow data lines to settle before presenting CTS falling edge.
  // Increased from 10us to 50us to improve signal integrity and
  // reduce intermittent transmission errors.
  delayMicroseconds(3);

  // Start the handshake by pulling /CTS LOW.
  // From your logic trace: /RTS activity happens while /CTS is LOW.
  digitalWrite(PIN_CTS, LOW);
  recordCTSFalling();

  // Read RTS immediately after CTS LOW to avoid race condition
  // if Furby responds very quickly.
  int level = digitalRead(PIN_RTS);

  uint32_t start     = micros();
  uint32_t highStart = 0;
  bool     sawHigh   = false;

  // -----------------------------------------------------------
  // Stage 1: Wait for LOW -> HIGH (start of busy window)
  // -----------------------------------------------------------

  if (level == HIGH) {
    // If RTS is already HIGH when we pull CTS low, treat this as
    // the beginning of the busy interval.
    recordRTSRising();
    sawHigh   = true;
    highStart = micros();
  } else {
    // RTS is LOW: wait for it to go HIGH, but don't block forever.
    while ((micros() - start) < RTS_TIMEOUT_US) {
      level = digitalRead(PIN_RTS);
      if (level == HIGH) {
        recordRTSRising();
        sawHigh   = true;
        highStart = micros();
        break;
      }
    }
  }

  if (!sawHigh) {
    // Never saw RTS go HIGH while CTS was LOW.
    // Release CTS so the bus isn't stuck.
    digitalWrite(PIN_CTS, HIGH);
    recordCTSRising();
    return false;
  }

  // -----------------------------------------------------------
  // Stage 2: Wait for HIGH -> LOW (end of busy window)
  // -----------------------------------------------------------

  // Now that we have seen the command get latched, we can return /CTS high
  delayMicroseconds(5);
  digitalWrite(PIN_CTS, HIGH);
  recordCTSRising();
  delayMicroseconds(5);  // Allow CTS to settle after rising edge

  while ((micros() - start) < RTS_TIMEOUT_US) {
    level = digitalRead(PIN_RTS);
    if (level == LOW) {
      recordRTSFalling();
      return true;
    }
  }

  // If we get here, RTS stayed HIGH for the entire timeout.
  // Record the falling edge even on timeout (for diagnostic purposes)
  recordRTSFalling();

  digitalWrite(PIN_CTS, HIGH);
  recordCTSRising();
  return false;
}

bool sendNibbleList(const uint8_t* list, size_t count) {
  if (count == 0) return true;

  // Clear timeline before sending new stream.
  clearTimeline();

  for (size_t i = 0; i < count; i++) {
    if (!sendNibble(list[i])) {
      // If any nibble handshake fails (timeout, etc.), abort.
      return false;
    }
  }
  return true;
}
#endif  // TARGET_SPIRIT

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

#ifdef TARGET_SPIRIT
// =========================
// HIGH-LEVEL ACTIONS
// =========================

size_t doInitCoprocessor() {
  // Pulse /INIT LOW then back HIGH to kick the coprocessor.
  digitalWrite(PIN_INIT, LOW);
  delay(INIT_PULSE_MS);
  digitalWrite(PIN_INIT, HIGH);

  // Clear any previous timeline so this capture is only the init pulse train.
  clearTimeline();

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
    recordRTSRising();
    inHigh      = true;
    highStartUs = micros();
  }

  while ((micros() - startOverall) < overallTimeoutUs) {
    int level = digitalRead(PIN_RTS);
    uint32_t now = micros();

    if (!inHigh) {
      // Currently LOW. Look for LOW -> HIGH to start a new pulse.
      if (level == HIGH) {
        recordRTSRising();
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
        recordRTSFalling();
        inHigh     = false;
        lowStartUs = now;
      }
    }

    // Small delay to avoid a completely hot spin; still much shorter than the
    // expected ~740 us HIGH/LOW windows.
    delayMicroseconds(1);
  }

  return timelineCount;
}

void driveMotorForward(uint64_t us) {
  if (us > MAX_MOTOR_US) us = MAX_MOTOR_US;
  digitalWrite(PIN_MOTOR_BWD, LOW);
  digitalWrite(PIN_MOTOR_FWD, HIGH);
  recordEvent(EVT_MOTOR_FWD_ON);

  delayMicroseconds((uint32_t)us);

  digitalWrite(PIN_MOTOR_FWD, LOW);
  recordEvent(EVT_MOTOR_FWD_OFF);
}

void driveMotorBackward(uint64_t us) {
  if (us > MAX_MOTOR_US) us = MAX_MOTOR_US;
  digitalWrite(PIN_MOTOR_FWD, LOW);
  digitalWrite(PIN_MOTOR_BWD, HIGH);
  recordEvent(EVT_MOTOR_BWD_ON);

  delayMicroseconds((uint32_t)us);

  digitalWrite(PIN_MOTOR_BWD, LOW);
  recordEvent(EVT_MOTOR_BWD_OFF);
}
#endif  // TARGET_SPIRIT

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

#ifdef TARGET_SPIRIT
    case CTRL_INIT_COPROCESSOR: {
      // Perform the /INIT pulse and capture the resulting timeline events.
      size_t count = doInitCoprocessor();

      client.print("[TIMELINE] ");
      client.print(count);
      client.print(" events: ");

      if (count == 0) {
        client.println("none");
      } else {
        // Output format: type,ts_hi,ts_lo[,data];...
        // Oldest event first.
        for (size_t i = 0; i < count; i++) {
          size_t idx = (timelineIndex + TIMELINE_LEN - count + i) % TIMELINE_LEN;
          TimelineEvent &evt = timeline[idx];

          // Format: type,timestamp_hi,timestamp_lo
          client.print(evt.event_type, HEX);
          client.print(",");
          client.print((uint32_t)(evt.timestamp_us >> 32), HEX);
          client.print(",");
          client.print((uint32_t)(evt.timestamp_us & 0xFFFFFFFF), HEX);

          // For data nibble events, append the nibble value
          if (evt.event_type == static_cast<uint8_t>(EVT_DATA_NIBBLE)) {
            client.print(",");
            client.print(evt.data, HEX);
          }

          if (i + 1 < count) client.print(";");
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
        // Print each payload byte as hex; if you know each byte is just a nibble (0-15),
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
      // Return full event timeline (replaces old RTS-only timing data).
      client.print("[TIMELINE] ");
      client.print(timelineCount);
      client.print(" events: ");

      if (timelineCount == 0) {
        client.println("none");
      } else {
        // Output format: type,ts_hi,ts_lo[,data];...
        // Oldest event first.
        for (size_t i = 0; i < timelineCount; i++) {
          size_t idx = (timelineIndex + TIMELINE_LEN - timelineCount + i) % TIMELINE_LEN;
          TimelineEvent &evt = timeline[idx];

          // Format: type,timestamp_hi,timestamp_lo
          client.print(evt.event_type, HEX);
          client.print(",");
          client.print((uint32_t)(evt.timestamp_us >> 32), HEX);
          client.print(",");
          client.print((uint32_t)(evt.timestamp_us & 0xFFFFFFFF), HEX);

          // For data nibble events, append the nibble value
          if (evt.event_type == static_cast<uint8_t>(EVT_DATA_NIBBLE)) {
            client.print(",");
            client.print(evt.data, HEX);
          }

          if (i + 1 < timelineCount) client.print(";");
        }
        client.println();
      }
      break;
    }

    case CTRL_DEBUG_BUS_DRIVE: {
      // Expect at least 1 byte (= 2 nibbles) of payload; we use the lower 5 bits.
      if (pcount < 2) {
        client.println("[ERROR] DEBUG_BUS requires 2 payload nibbles (00-1F)");
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
#endif  // TARGET_SPIRIT

#ifdef TARGET_FEATHER
    case CTRL_BLINK_LED: {
      // Parse count from hex (default 3 blinks)
      int count = 3;
      if (pcount >= 1) {
        count = payload[0];
        if (count > 15) count = 15;  // Max 15 blinks
        if (count < 1) count = 1;
      }
      // Blink white on NeoPixel
      for (int i = 0; i < count; i++) {
        pixel.setPixelColor(0, 255, 255, 255);  // White
        pixel.show();
        delay(200);
        pixel.setPixelColor(0, 0, 0, 0);  // Off
        pixel.show();
        delay(200);
      }
      client.println("OK");
      break;
    }

    case CTRL_SET_RGB: {
      // Parse RGB values from payload: 09 RR GG BB (6 nibbles = 3 bytes)
      if (pcount < 6) {
        client.println("[ERROR] SET_RGB requires 6 nibbles (RRGGBB)");
        break;
      }
      uint8_t r = (payload[0] << 4) | payload[1];
      uint8_t g = (payload[2] << 4) | payload[3];
      uint8_t b = (payload[4] << 4) | payload[5];

      pixel.setPixelColor(0, r, g, b);
      pixel.show();

      client.print("[OK] RGB set to ");
      client.print(r);
      client.print(",");
      client.print(g);
      client.print(",");
      client.println(b);
      break;
    }

    case CTRL_RECORD_AUDIO: {
      if (!i2sInitialized) {
        client.println("[ERROR] I2S microphone not initialized");
        break;
      }

      // Flash LED to indicate recording
      pixel.setPixelColor(0, 255, 0, 0);  // Red = recording
      pixel.show();

      Serial.println("[MIC] Recording 1 second of audio...");
      int samplesRecorded = recordAudio();

      pixel.setPixelColor(0, 0, 0, 0);  // Off when done
      pixel.show();

      if (samplesRecorded < 0) {
        client.println("[ERROR] Recording failed");
        break;
      }

      Serial.printf("[MIC] Recorded %d samples\n", samplesRecorded);

      // Send header: sample rate (4 bytes) + sample count (4 bytes)
      // Then raw 16-bit signed samples as binary
      uint32_t sampleRate = MIC_SAMPLE_RATE;
      uint32_t sampleCount = (uint32_t)samplesRecorded;

      // Write binary header
      client.write((uint8_t*)&sampleRate, 4);
      client.write((uint8_t*)&sampleCount, 4);

      // Write audio samples in chunks to avoid buffer issues
      const int CHUNK_SIZE = 512;  // samples per chunk
      for (int i = 0; i < samplesRecorded; i += CHUNK_SIZE) {
        int remaining = samplesRecorded - i;
        int toSend = (remaining < CHUNK_SIZE) ? remaining : CHUNK_SIZE;
        client.write((uint8_t*)&audioBuffer[i], toSend * sizeof(int16_t));
      }

      Serial.println("[MIC] Audio data sent");
      break;
    }
#endif  // TARGET_FEATHER

#ifndef TARGET_SPIRIT
    // Return error for Furby-specific commands on Feather
    case CTRL_INIT_COPROCESSOR:
    case CTRL_SEND_NIBBLE_STREAM:
    case CTRL_MOTOR_FORWARD:
    case CTRL_MOTOR_BACKWARD:
    case CTRL_READ_RTS_HISTORY:
    case CTRL_DEBUG_BUS_DRIVE:
    case CTRL_POLL_INPUTS:
      client.println("[ERROR] Command not available on this target");
      break;
#endif

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
  Serial.begin(115200);

#ifdef TARGET_SPIRIT
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
#endif

#ifdef TARGET_FEATHER
  // Enable NeoPixel power (NEOPIXEL_I2C_POWER is defined by board variant)
  pinMode(NEOPIXEL_I2C_POWER, OUTPUT);
  digitalWrite(NEOPIXEL_I2C_POWER, HIGH);

  // Initialize NeoPixel
  pixel.begin();
  pixel.setBrightness(50);  // 0-255, start at ~20%
  pixel.setPixelColor(0, 0, 0, 0);  // Start with LED off
  pixel.show();

  // Initialize I2S microphone
  setupI2SMic();
#endif

  setupWiFi();
  setupOTA();
  tcpServer.begin();
}

void loop() {
  // Handle WiFi reconnection if disconnected
  if (WiFi.status() != WL_CONNECTED) {
    if (wasConnected) {
      Serial.println("[WiFi] Connection lost!");
      wasConnected = false;
    }

    unsigned long now = millis();
    if (now - lastReconnectAttempt >= RECONNECT_INTERVAL_MS) {
      lastReconnectAttempt = now;
      Serial.println("[WiFi] Attempting to reconnect...");

      if (wifiMulti.run() == WL_CONNECTED) {
        wasConnected = true;
        Serial.print("[WiFi] Reconnected to: ");
        Serial.println(WiFi.SSID());
        Serial.print("[WiFi] IP address: ");
        Serial.println(WiFi.localIP());
      }
    }
    return;  // Skip normal loop operations while disconnected
  }

  ArduinoOTA.handle();
  WiFiClient client = tcpServer.accept();
  if (client) {
    handleClient(client);
    client.stop();
  }
}

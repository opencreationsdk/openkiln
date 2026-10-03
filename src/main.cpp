// SPDX-License-Identifier: GPL-3.0-or-later
//
// OpenKiln controller firmware.
//
// Task layout (ESP32, dual core):
//   control  core 1, prio 5  thermocouple, PID, SSR time-proportioning (20 ms tick)
//   loopTask core 1, prio 1  web server, NVS checkpoints
//   network  core 0, prio 1  Wi-Fi upkeep, MQTT, InfluxDB, Pushover, firmware update
//   esp_timer                heat watchdog (forces SSR off if the control task stalls)
//
// All shared kiln state lives in `kiln` and is only touched while holding
// stateMutex. Nothing that can block on the network runs in the control task.

#include <Arduino.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <PubSubClient.h>
#include <SPI.h>
#include <Update.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_timer.h>
#include <rom/ets_sys.h>
#include <time.h>

#include <atomic>

#include "Config.h"
#include "Version.h"
#include "WebAssets.h"  // generated from web/ by tools/embed_web.py

namespace {

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
#define OPENKILN_REPO "opencreationsdk/openkiln"
constexpr const char* RELEASE_LATEST_URL = "https://github.com/" OPENKILN_REPO "/releases/latest";
constexpr const char* RELEASE_DOWNLOAD_URL = "https://github.com/" OPENKILN_REPO "/releases/download/";
constexpr const char* USER_AGENT = "OpenKiln/" OPENKILN_VERSION;

constexpr size_t MAX_PROFILE_POINTS = 24;
constexpr size_t MAX_SAVED_PROFILES = 8;
constexpr size_t MAX_PROFILE_NAME = 32;
constexpr time_t VALID_EPOCH = 1700000000;
constexpr uint32_t UPDATE_CHECK_INTERVAL_MS = 24UL * 60UL * 60UL * 1000UL;
constexpr uint32_t FIRST_UPDATE_CHECK_MS = 30000;
constexpr uint32_t CHECKPOINT_MAGIC = 0x4B494C31;  // "KIL1"

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------
struct ProfilePoint {
  float minute;
  float temperatureC;
};

enum class RunState : uint8_t { IDLE, RUNNING, PAUSED, COMPLETE, FAULT };

struct KilnState {
  RunState state = RunState::IDLE;
  float temperatureC = NAN;
  float targetC = 0.0f;
  float duty = 0.0f;
  float rateCPerHour = 0.0f;
  double elapsedSeconds = 0.0;
  bool sensorHealthy = false;
  bool pidResetRequested = true;
  bool checkpointPending = false;
  uint32_t profileRevision = 1;
  uint8_t activeSlot = 0;
  char fault[96] = "";
  char profileName[MAX_PROFILE_NAME + 1] = "Default";
  size_t profileCount = 0;
  ProfilePoint profile[MAX_PROFILE_POINTS];
};

struct UpdateState {
  bool checkRequested = false;
  bool checking = false;
  bool checked = false;
  bool available = false;
  bool installRequested = false;
  bool installing = false;
  int progress = 0;
  const char* stage = "idle";
  char latest[24] = "";
  char checkError[80] = "";
  char message[96] = "";
};

struct Settings {
  String wifiSsid, wifiPassword, apName, apPassword, webUser, webPassword;
  bool authRequired = true;
  bool mqttEnabled = false, mqttControl = false, mqttStart = false;
  String mqttHost, mqttUser, mqttPassword, mqttTopic;
  uint16_t mqttPort = 1883;
  bool influxEnabled = false;
  String influxUrl, influxOrg, influxBucket, influxToken;
  uint32_t influxInterval = 30;
  bool pushoverEnabled = false;
  String pushoverToken, pushoverUser;
  String ntpServer, timezone, hostname, language;
};

// Persisted as a single NVS blob: one flash write per checkpoint instead of three.
struct Checkpoint {
  uint32_t magic;
  uint8_t running;
  uint8_t slot;          // profile slot the firing used
  uint8_t reserved[2];
  double elapsedSeconds;
  int64_t epoch;
};

struct Notification {
  char title[32];
  char body[96];
};

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------
SPIClass thermoSpi(VSPI);
WebServer server(80);
Preferences preferences;
WiFiClient mqttNetwork;
PubSubClient mqttClient(mqttNetwork);

SemaphoreHandle_t stateMutex = nullptr;
QueueHandle_t notifyQueue = nullptr;
KilnState kiln;
UpdateState upd;

// Running configuration: loaded once at boot and treated as read-only afterwards,
// so it is safe to read from every task. Setup changes go to NVS and apply on reboot.
Settings settings;

Checkpoint bootCheckpoint = {};
std::atomic<bool> firmwareBusy(false);       // OTA upload or download in progress
std::atomic<bool> publishRequested(false);   // ask the network task to publish MQTT now
std::atomic<uint32_t> controlHeartbeatMs(0);
std::atomic<bool> recoveryPending(false);    // power-failure recovery not yet decided
bool otaUploadRejected = false;              // loop task only
bool otaUploadOk = false;                    // loop task only
bool otaSessionStarted = false;              // loop task only

class StateLock {
 public:
  StateLock() { xSemaphoreTake(stateMutex, portMAX_DELAY); }
  ~StateLock() { xSemaphoreGive(stateMutex); }
  StateLock(const StateLock&) = delete;
  StateLock& operator=(const StateLock&) = delete;
};

KilnState snapshotKiln() {
  StateLock lock;
  return kiln;
}

UpdateState snapshotUpdate() {
  StateLock lock;
  return upd;
}

bool isActive(RunState s) { return s == RunState::RUNNING || s == RunState::PAUSED; }

const char* stateName(RunState state) {
  switch (state) {
    case RunState::IDLE: return "IDLE";
    case RunState::RUNNING: return "RUNNING";
    case RunState::PAUSED: return "PAUSED";
    case RunState::COMPLETE: return "COMPLETE";
    case RunState::FAULT: return "FAULT";
  }
  return "UNKNOWN";
}

// ---------------------------------------------------------------------------
// Tiny allocation-free JSON writer (avoids String concatenation / heap churn)
// ---------------------------------------------------------------------------
class Json {
 public:
  Json(char* buffer, size_t capacity) : b_(buffer), cap_(capacity) { b_[0] = '\0'; }
  Json& beginObject() { sep(); put('{'); comma_ = false; return *this; }
  Json& endObject() { put('}'); comma_ = true; return *this; }
  Json& beginArray() { sep(); put('['); comma_ = false; return *this; }
  Json& endArray() { put(']'); comma_ = true; return *this; }
  Json& key(const char* k) { sep(); put('"'); puts(k); puts("\":"); comma_ = false; return *this; }
  Json& str(const char* s) {
    sep();
    put('"');
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(s); p && *p; ++p) {
      if (*p == '"' || *p == '\\') { put('\\'); put(static_cast<char>(*p)); }
      else if (*p == '\n') puts("\\n");
      else if (*p < 0x20) { char t[8]; snprintf(t, sizeof(t), "\\u%04x", *p); puts(t); }
      else put(static_cast<char>(*p));
    }
    put('"');
    comma_ = true;
    return *this;
  }
  Json& num(double v, int decimals) {
    sep();
    if (!isfinite(v)) puts("null");
    else { char t[32]; snprintf(t, sizeof(t), "%.*f", decimals, v); puts(t); }
    comma_ = true;
    return *this;
  }
  Json& integer(long long v) { sep(); char t[24]; snprintf(t, sizeof(t), "%lld", v); puts(t); comma_ = true; return *this; }
  Json& boolean(bool v) { sep(); puts(v ? "true" : "false"); comma_ = true; return *this; }
  Json& kvStr(const char* k, const char* v) { return key(k).str(v); }
  Json& kvStr(const char* k, const String& v) { return key(k).str(v.c_str()); }
  Json& kvNum(const char* k, double v, int decimals) { return key(k).num(v, decimals); }
  Json& kvInt(const char* k, long long v) { return key(k).integer(v); }
  Json& kvBool(const char* k, bool v) { return key(k).boolean(v); }
  bool ok() const { return !overflow_; }
  const char* c_str() const { return b_; }
  size_t length() const { return len_; }

 private:
  void sep() { if (comma_) put(','); comma_ = false; }
  void put(char c) {
    if (len_ + 1 < cap_) { b_[len_++] = c; b_[len_] = '\0'; }
    else overflow_ = true;
  }
  void puts(const char* s) { while (*s) put(*s++); }
  char* b_;
  size_t cap_;
  size_t len_ = 0;
  bool comma_ = false;
  bool overflow_ = false;
};

void sendJson(const Json& json, int code = 200) {
  if (!json.ok()) { server.send(500, "text/plain", "Response too large"); return; }
  server.send_P(code, "application/json", json.c_str(), json.length());
}

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------
String prefStr(const char* key, const char* fallback = "") { return preferences.getString(key, fallback); }

void loadSettings(Settings& s) {
  s.wifiSsid = prefStr("wifiSsid", WIFI_SSID);
  s.wifiPassword = prefStr("wifiPass", WIFI_PASSWORD);
  s.apName = prefStr("apName", AP_NAME);
  s.apPassword = prefStr("apPass", AP_PASSWORD);
  s.webUser = prefStr("webUser", WEB_USER);
  s.webPassword = prefStr("webPass", WEB_PASSWORD);
  s.authRequired = preferences.getBool("authReq", true);
  s.mqttEnabled = preferences.getBool("mqttEn", strlen(MQTT_HOST) > 0);
  s.mqttHost = prefStr("mqttHost", MQTT_HOST);
  s.mqttPort = preferences.getUShort("mqttPort", MQTT_PORT);
  s.mqttUser = prefStr("mqttUser", MQTT_USER);
  s.mqttPassword = prefStr("mqttPass", MQTT_PASSWORD);
  s.mqttTopic = prefStr("mqttTopic", MQTT_TOPIC);
  s.mqttControl = preferences.getBool("mqttCtl", MQTT_ALLOW_CONTROL);
  s.mqttStart = preferences.getBool("mqttStart", MQTT_ALLOW_REMOTE_START);
  s.influxEnabled = preferences.getBool("influxEn", false);
  s.influxUrl = prefStr("influxUrl");
  s.influxOrg = prefStr("influxOrg");
  s.influxBucket = prefStr("influxBucket");
  s.influxToken = prefStr("influxToken");
  s.influxInterval = preferences.getUInt("influxInt", 30);
  if (s.influxInterval < 5) s.influxInterval = 5;
  s.pushoverEnabled = preferences.getBool("pushEn", false);
  s.pushoverToken = prefStr("pushToken");
  s.pushoverUser = prefStr("pushUser");
  s.ntpServer = prefStr("ntp", "pool.ntp.org");
  s.timezone = prefStr("tz", "CET-1CEST,M3.5.0,M10.5.0/3");
  s.hostname = prefStr("hostname", "kiln");
  if (s.hostname.isEmpty()) s.hostname = "kiln";
  s.language = prefStr("language", "en");
  if (s.language != "da") s.language = "en";
  while (s.mqttTopic.endsWith("/")) s.mqttTopic.remove(s.mqttTopic.length() - 1);
  if (s.mqttTopic.isEmpty()) s.mqttTopic = MQTT_TOPIC;
}

String sanitizeHostname(String h) {
  h.toLowerCase();
  String out;
  for (char c : h) {
    if (isalnum(static_cast<unsigned char>(c))) out += c;
    else if (c == ' ' || c == '-' || c == '_') out += '-';
    if (out.length() >= 32) break;
  }
  while (out.startsWith("-")) out.remove(0, 1);
  while (out.endsWith("-")) out.remove(out.length() - 1);
  return out.isEmpty() ? String("kiln") : out;
}

// ---------------------------------------------------------------------------
// Notifications (queued; sent by the network task so nothing else blocks on TLS)
// ---------------------------------------------------------------------------
void queueNotification(const char* title, const char* body) {
  if (!notifyQueue) return;
  Notification n;
  strlcpy(n.title, title, sizeof(n.title));
  strlcpy(n.body, body, sizeof(n.body));
  xQueueSend(notifyQueue, &n, 0);  // never block; drop if the queue is full
}

// ---------------------------------------------------------------------------
// Hardware: SSR and thermocouple
// ---------------------------------------------------------------------------
inline void setRelay(bool on) { digitalWrite(PIN_SSR, (on == SSR_ACTIVE_HIGH) ? HIGH : LOW); }

// The functions suffixed "Locked" require stateMutex to be held by the caller.
void heatOffLocked() {
  kiln.duty = 0.0f;
  setRelay(false);
}

void enterFaultLocked(const char* message) {
  heatOffLocked();
  // Re-entering the same fault (e.g. temperature still above the limit on every
  // read) must not spam notifications or rewrite flash twice a second.
  if (kiln.state == RunState::FAULT && strcmp(kiln.fault, message) == 0) return;
  kiln.state = RunState::FAULT;
  strlcpy(kiln.fault, message, sizeof(kiln.fault));
  kiln.checkpointPending = true;  // a faulted firing must never be auto-resumed
  publishRequested = true;
  Serial.printf("FAULT: %s\n", message);
  queueNotification("Kiln fault", message);
}

uint8_t spiReadRegister(uint8_t address) {
  thermoSpi.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE1));
  digitalWrite(PIN_THERMO_CS, LOW);
  thermoSpi.transfer(address & 0x7F);
  uint8_t value = thermoSpi.transfer(0x00);
  digitalWrite(PIN_THERMO_CS, HIGH);
  thermoSpi.endTransaction();
  return value;
}

void spiWriteRegister(uint8_t address, uint8_t value) {
  thermoSpi.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE1));
  digitalWrite(PIN_THERMO_CS, LOW);
  thermoSpi.transfer(address | 0x80);
  thermoSpi.transfer(value);
  digitalWrite(PIN_THERMO_CS, HIGH);
  thermoSpi.endTransaction();
}

void beginThermocouple() {
  pinMode(PIN_THERMO_CS, OUTPUT);
  digitalWrite(PIN_THERMO_CS, HIGH);
  thermoSpi.begin(PIN_THERMO_SCK, PIN_THERMO_MISO, PIN_THERMO_MOSI, PIN_THERMO_CS);
  if (THERMOCOUPLE_CHIP == ThermocoupleChip::MAX31856) {
    // CR0: automatic conversion (0x80), open-circuit detection enabled (OCFAULT=01, 0x10),
    // and mains rejection. Without OCFAULT the MAX31856 does NOT report a broken thermocouple.
    spiWriteRegister(0x00, static_cast<uint8_t>(0x80 | 0x10 | (MAINS_IS_50_HZ ? 0x01 : 0x00)));
    // CR1: 16-sample averaging plus thermocouple type.
    spiWriteRegister(0x01, static_cast<uint8_t>(0x40 | (MAX31856_TYPE & 0x0F)));
  }
}

bool readMax31855(float& temperature, char* error, size_t errorLen) {
  thermoSpi.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
  digitalWrite(PIN_THERMO_CS, LOW);
  delayMicroseconds(2);
  uint32_t raw = 0;
  for (int i = 0; i < 4; ++i) raw = (raw << 8) | thermoSpi.transfer(0x00);
  digitalWrite(PIN_THERMO_CS, HIGH);
  thermoSpi.endTransaction();

  if (raw == 0 || raw == 0xFFFFFFFFUL) {
    strlcpy(error, "Thermocouple amplifier not responding", errorLen);
    return false;
  }
  if (SENSOR_CHECK_ENABLED && (raw & 0x00010000UL)) {
    if (raw & 0x1) strlcpy(error, "Thermocouple open", errorLen);
    else if (raw & 0x2) strlcpy(error, "Thermocouple shorted to ground", errorLen);
    else if (raw & 0x4) strlcpy(error, "Thermocouple shorted to supply", errorLen);
    else strlcpy(error, "MAX31855 fault", errorLen);
    return false;
  }
  int16_t signedValue = static_cast<int16_t>((raw >> 18) & 0x3FFF);
  if (signedValue & 0x2000) signedValue |= 0xC000;
  temperature = signedValue * 0.25f;
  return true;
}

bool readMax31856(float& temperature, char* error, size_t errorLen) {
  uint8_t status = spiReadRegister(0x0F);
  if (SENSOR_CHECK_ENABLED && status != 0) {
    snprintf(error, errorLen, "MAX31856 fault 0x%02X%s", status, (status & 0x01) ? " (open)" : "");
    return false;
  }
  thermoSpi.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE1));
  digitalWrite(PIN_THERMO_CS, LOW);
  thermoSpi.transfer(0x0C);
  uint32_t raw = (static_cast<uint32_t>(thermoSpi.transfer(0)) << 16) |
                 (static_cast<uint32_t>(thermoSpi.transfer(0)) << 8) |
                 thermoSpi.transfer(0);
  digitalWrite(PIN_THERMO_CS, HIGH);
  thermoSpi.endTransaction();
  int32_t signedValue = static_cast<int32_t>(raw >> 5);
  if (signedValue & 0x40000) signedValue |= ~0x7FFFF;
  temperature = signedValue * 0.0078125f;
  return true;
}

bool readThermocouple(float& temperature, char* error, size_t errorLen) {
  bool ok = THERMOCOUPLE_CHIP == ThermocoupleChip::MAX31855
                ? readMax31855(temperature, error, errorLen)
                : readMax31856(temperature, error, errorLen);
  if (!ok) return false;
  temperature += THERMOCOUPLE_OFFSET_C;
  if (!isfinite(temperature) || temperature < -100.0f || temperature > 1800.0f) {
    strlcpy(error, "Implausible temperature", errorLen);
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Profiles (NVS text format "minute,temperature\n" is kept for compatibility)
// ---------------------------------------------------------------------------
const char* defaultProfile() { return "0,20\n10,100\n20,100\n30,20\n"; }

bool parseProfile(const String& text, ProfilePoint* destination, size_t& count, String& error) {
  count = 0;
  int start = 0;
  float previousMinute = -1.0f;
  while (start < static_cast<int>(text.length())) {
    int end = text.indexOf('\n', start);
    if (end < 0) end = text.length();
    String line = text.substring(start, end);
    line.trim();
    start = end + 1;
    if (line.isEmpty() || line.startsWith("#")) continue;
    int comma = line.indexOf(',');
    if (comma < 1 || count >= MAX_PROFILE_POINTS) {
      error = "Invalid line or too many profile points (max 24)";
      return false;
    }
    char* minuteEnd = nullptr;
    char* tempEnd = nullptr;
    String minuteText = line.substring(0, comma);
    String tempText = line.substring(comma + 1);
    float minute = strtof(minuteText.c_str(), &minuteEnd);
    float temperature = strtof(tempText.c_str(), &tempEnd);
    while (*minuteEnd == ' ' || *minuteEnd == '\t') ++minuteEnd;
    while (*tempEnd == ' ' || *tempEnd == '\t' || *tempEnd == '\r') ++tempEnd;
    if (*minuteEnd != '\0' || *tempEnd != '\0' || !isfinite(minute) || !isfinite(temperature) ||
        minute < 0 || minute <= previousMinute || temperature < -50 ||
        temperature >= EMERGENCY_SHUTOFF_C) {
      error = "Each line needs increasing minutes and a safe Celsius temperature";
      return false;
    }
    destination[count++] = {minute, temperature};
    previousMinute = minute;
  }
  if (count < 2 || destination[0].minute != 0.0f) {
    error = "Profile needs at least two points and must start at minute 0";
    return false;
  }
  return true;
}

void profileNameKey(char* out, size_t n, size_t slot) { snprintf(out, n, "pn%u", static_cast<unsigned>(slot)); }
void profileDataKey(char* out, size_t n, size_t slot) { snprintf(out, n, "pd%u", static_cast<unsigned>(slot)); }

String readProfileName(size_t slot) {
  char key[8];
  profileNameKey(key, sizeof(key), slot);
  return preferences.getString(key, "");
}

String readProfileData(size_t slot) {
  char key[8];
  profileDataKey(key, sizeof(key), slot);
  return preferences.getString(key, "");
}

// Converts [[min,temp],...] (from the web editor) into the NVS text format.
String profileTextFromJsonArray(const String& json, String& error) {
  String text;
  int pos = 0;
  while (true) {
    int open = json.indexOf('[', pos);
    if (open < 0) break;
    if (open == 0) { pos = 1; continue; }
    int comma = json.indexOf(',', open + 1);
    int close = comma < 0 ? -1 : json.indexOf(']', comma + 1);
    if (comma < 0 || close < 0) break;
    String a = json.substring(open + 1, comma); a.trim();
    String b = json.substring(comma + 1, close); b.trim();
    if (a.length() && b.length()) text += a + "," + b + "\n";
    pos = close + 1;
  }
  ProfilePoint candidate[MAX_PROFILE_POINTS];
  size_t count = 0;
  if (!parseProfile(text, candidate, count, error)) return "";
  return text;
}

void writeProfilePoints(Json& j, const ProfilePoint* points, size_t count) {
  j.beginArray();
  for (size_t i = 0; i < count; ++i) {
    j.beginArray().num(points[i].minute, 1).num(points[i].temperatureC, 1).endArray();
  }
  j.endArray();
}

// Loads a saved profile into the controller. Refuses while a firing is active.
bool activateProfileSlot(size_t slot, String& error) {
  if (slot >= MAX_SAVED_PROFILES) { error = "Invalid profile slot"; return false; }
  String name = readProfileName(slot);
  String text = readProfileData(slot);
  if (name.isEmpty() || text.isEmpty()) { error = "Profile not found"; return false; }
  ProfilePoint candidate[MAX_PROFILE_POINTS];
  size_t count = 0;
  if (!parseProfile(text, candidate, count, error)) return false;
  {
    StateLock lock;
    if (isActive(kiln.state)) { error = "Stop the kiln before changing profiles"; return false; }
    if (recoveryPending) { error = "Checking for an interrupted firing; try again in a minute"; return false; }
    memcpy(kiln.profile, candidate, sizeof(ProfilePoint) * count);
    kiln.profileCount = count;
    kiln.activeSlot = static_cast<uint8_t>(slot);
    strlcpy(kiln.profileName, name.c_str(), sizeof(kiln.profileName));
    kiln.profileRevision++;
  }
  if (preferences.getUInt("activeSlot", 0) != slot) preferences.putUInt("activeSlot", slot);
  if (preferences.getString("profile", "") != text) preferences.putString("profile", text);
  publishRequested = true;
  return true;
}

void loadProfileAtBoot() {
  // Migrate the original single profile into slot 0 on first boot after upgrade.
  if (readProfileName(0).isEmpty()) {
    String legacy = preferences.getString("profile", "");
    if (legacy.isEmpty()) legacy = defaultProfile();
    preferences.putString("pn0", "Default");
    preferences.putString("pd0", legacy);
    preferences.putUInt("activeSlot", 0);
  }
  String error;
  size_t slot = preferences.getUInt("activeSlot", 0);
  if (!activateProfileSlot(slot, error) && !activateProfileSlot(0, error)) {
    // Last resort: built-in default so the controller always has a valid profile.
    ProfilePoint p[MAX_PROFILE_POINTS];
    size_t count = 0;
    parseProfile(defaultProfile(), p, count, error);
    StateLock lock;
    memcpy(kiln.profile, p, sizeof(ProfilePoint) * count);
    kiln.profileCount = count;
  }
}

float targetForElapsedLocked(double seconds) {
  if (kiln.profileCount == 0) return 0.0f;
  const ProfilePoint* p = kiln.profile;
  float minute = seconds / 60.0;
  if (minute <= p[0].minute) return p[0].temperatureC;
  for (size_t i = 1; i < kiln.profileCount; ++i) {
    if (minute <= p[i].minute) {
      float span = p[i].minute - p[i - 1].minute;
      float fraction = (minute - p[i - 1].minute) / span;
      return p[i - 1].temperatureC + fraction * (p[i].temperatureC - p[i - 1].temperatureC);
    }
  }
  return p[kiln.profileCount - 1].temperatureC;
}

// ---------------------------------------------------------------------------
// Checkpoints (power-failure recovery)
// ---------------------------------------------------------------------------
void writeCheckpoint(bool running, double elapsedSeconds, uint8_t slot) {
  Checkpoint c = {};
  c.magic = CHECKPOINT_MAGIC;
  c.running = running ? 1 : 0;
  c.slot = slot;
  c.elapsedSeconds = elapsedSeconds;
  c.epoch = static_cast<int64_t>(time(nullptr));
  preferences.putBytes("ckpt", &c, sizeof(c));
}

void loadBootCheckpoint() {
  Checkpoint c = {};
  if (preferences.getBytes("ckpt", &c, sizeof(c)) == sizeof(c) && c.magic == CHECKPOINT_MAGIC) {
    bootCheckpoint = c;
  }
  recoveryPending = AUTO_RESTART && bootCheckpoint.running;
}

// Runs in the loop task. Writes on state changes and periodically while firing.
void serviceCheckpoint(uint32_t now) {
  static uint32_t lastCheckMs = 0;
  static uint32_t lastWriteMs = 0;
  if (now - lastCheckMs < 100) return;
  lastCheckMs = now;
  bool active;
  double elapsed;
  uint8_t slot;
  {
    StateLock lock;
    active = isActive(kiln.state);
    slot = kiln.activeSlot;
    const bool periodic = active && now - lastWriteMs >= CHECKPOINT_INTERVAL_MS;
    if (!kiln.checkpointPending && !periodic) return;
    kiln.checkpointPending = false;
    elapsed = kiln.elapsedSeconds;
  }
  lastWriteMs = now;
  writeCheckpoint(active, elapsed, slot);
}

// Runs in the network task: waits for NTP (needed to know how long power was
// off) and then either restores the interrupted firing as PAUSED or discards it.
void serviceAutoRestart() {
  static bool done = false;
  if (done) return;
  if (!recoveryPending) { done = true; return; }
  const time_t now = time(nullptr);
  if (now < VALID_EPOCH && millis() < AUTO_RESTART_TIME_WAIT_MS) return;  // keep waiting for time
  done = true;
  const time_t saved = static_cast<time_t>(bootCheckpoint.epoch);
  bool recoverable = now >= VALID_EPOCH && saved >= VALID_EPOCH && now >= saved &&
                     static_cast<uint32_t>(now - saved) <= AUTO_RESTART_WINDOW_SECONDS;
  StateLock lock;
  recoveryPending = false;
  if (kiln.state != RunState::IDLE) return;  // user already started something else
  recoverable = recoverable && kiln.activeSlot == bootCheckpoint.slot;
  if (recoverable) {
    kiln.elapsedSeconds = bootCheckpoint.elapsedSeconds;
    kiln.targetC = targetForElapsedLocked(kiln.elapsedSeconds);
    kiln.state = RunState::PAUSED;
    strlcpy(kiln.fault, "Power interruption recovered; inspect kiln, then press Resume", sizeof(kiln.fault));
    Serial.println("Recovered interrupted firing (PAUSED)");
  }
  kiln.checkpointPending = true;
  publishRequested = true;
}

// ---------------------------------------------------------------------------
// Real-time control task
// ---------------------------------------------------------------------------
struct Controller {
  float integral = 0.0f;
  float previousPv = NAN;
  uint32_t previousPvMs = 0;
  float derivativeFiltered = 0.0f;
  uint32_t lastGoodMs = 0;
  uint8_t badReads = 0;
  float rateRefTemperature = NAN;
  uint32_t rateRefMs = 0;
  uint32_t windowStartMs = 0;
};

void processReadingLocked(bool ok, float value, const char* error, uint32_t now, Controller& c) {
  if (ok) {
    c.badReads = 0;
    c.lastGoodMs = now;
    kiln.temperatureC = value;
    kiln.sensorHealthy = true;
    // Checked in every state: a welded SSR while IDLE must still raise an alarm.
    if (value >= EMERGENCY_SHUTOFF_C) enterFaultLocked("Emergency temperature limit reached");
    if (c.rateRefMs == 0 || now - c.rateRefMs >= 10000) {
      if (isfinite(c.rateRefTemperature) && c.rateRefMs != 0) {
        kiln.rateCPerHour = (value - c.rateRefTemperature) * 3600000.0f / static_cast<float>(now - c.rateRefMs);
      }
      c.rateRefTemperature = value;
      c.rateRefMs = now;
    }
  } else {
    // The SSR is held off immediately (sensorHealthy=false), but a single glitch
    // (common with SSR switching noise) no longer aborts a multi-hour firing.
    kiln.sensorHealthy = false;
    if (c.badReads < 255) c.badReads++;
    if (SENSOR_CHECK_ENABLED && kiln.state == RunState::RUNNING && c.badReads >= SENSOR_FAULT_COUNT) {
      enterFaultLocked(error);
    }
  }
}

// Advances the profile clock; returns true when the profile has completed.
bool advanceProfileLocked(float dt, bool caughtUp) {
  if (!KILN_MUST_CATCH_UP || caughtUp) kiln.elapsedSeconds += dt;
  if (kiln.elapsedSeconds < kiln.profile[kiln.profileCount - 1].minute * 60.0) return false;
  kiln.state = RunState::COMPLETE;
  heatOffLocked();
  kiln.checkpointPending = true;
  publishRequested = true;
  char body[96];
  snprintf(body, sizeof(body), "%s is complete", kiln.profileName);
  queueNotification("Kiln complete", body);
  return true;
}

void runControlLocked(float dt, uint32_t now, Controller& c) {
  if (kiln.pidResetRequested) {
    kiln.pidResetRequested = false;
    c.integral = 0.0f;
    c.derivativeFiltered = 0.0f;
    c.previousPv = kiln.temperatureC;
    c.previousPvMs = now;
  }
  if (kiln.state != RunState::RUNNING) {
    heatOffLocked();
    return;
  }
  if (SENSOR_CHECK_ENABLED && now - c.lastGoodMs > SENSOR_TIMEOUT_MS) {
    enterFaultLocked("Temperature sensor timeout");
    return;
  }
  kiln.targetC = targetForElapsedLocked(kiln.elapsedSeconds);
  if (!kiln.sensorHealthy) {
    // No usable reading: never heat blind. Normally the profile clock is held too;
    // with SENSOR_CHECK_ENABLED=false it keeps running so a program can be dry-run.
    heatOffLocked();
    if (!SENSOR_CHECK_ENABLED) advanceProfileLocked(dt, true);
    return;
  }

  const float pv = kiln.temperatureC;
  if (advanceProfileLocked(dt, fabsf(pv - kiln.targetC) <= CATCH_UP_WINDOW_C)) return;

  // Derivative on measurement (no setpoint kick on ramp changes) with a low-pass filter.
  // Uses the real time since the last used sample, so a skipped step (transient
  // bad read) doesn't inflate the derivative.
  if (!isfinite(c.previousPv)) { c.previousPv = pv; c.previousPvMs = now; }
  const float dtPv = max((now - c.previousPvMs) / 1000.0f, 0.001f);
  const float dMeasurement = (pv - c.previousPv) / dtPv;
  c.derivativeFiltered += PID_D_FILTER * (dMeasurement - c.derivativeFiltered);
  c.previousPv = pv;
  c.previousPvMs = now;

  const float error = kiln.targetC - pv;
  // Low-temperature power limit applies to every branch, including the PID, so the
  // anti-windup test must use the same ceiling or the integral winds up below it.
  const float maxDuty = pv < THROTTLE_BELOW_C ? THROTTLE_MAX_DUTY : 1.0f;
  float duty;
  if (error > PID_CONTROL_WINDOW_C) {
    c.integral = 0.0f;
    duty = maxDuty;
  } else if (error < -PID_CONTROL_WINDOW_C) {
    c.integral = 0.0f;
    duty = 0.0f;
  } else {
    const float candidateIntegral = constrain(c.integral + PID_KI * error * dt, -INTEGRAL_LIMIT, INTEGRAL_LIMIT);
    const float output = PID_KP * error + candidateIntegral - PID_KD * c.derivativeFiltered;
    duty = constrain(output, 0.0f, maxDuty);
    // Conditional integration prevents wind-up while saturated in the wrong direction.
    if ((output >= 0.0f && output <= maxDuty) || (output > maxDuty && error < 0) || (output < 0.0f && error > 0)) {
      c.integral = candidateIntegral;
    }
  }
  kiln.duty = duty;
}

void driveRelayLocked(uint32_t now, Controller& c) {
  if (now - c.windowStartMs >= SSR_WINDOW_MS) {
    c.windowStartMs += ((now - c.windowStartMs) / SSR_WINDOW_MS) * SSR_WINDOW_MS;
  }
  const bool on = kiln.state == RunState::RUNNING && kiln.sensorHealthy && !firmwareBusy &&
                  (now - c.windowStartMs) < static_cast<uint32_t>(kiln.duty * SSR_WINDOW_MS);
  setRelay(on);
}

void controlTask(void*) {
  Controller c;
  const uint32_t start = millis();
  c.lastGoodMs = start;
  c.windowStartMs = start;
  uint32_t lastSensorMs = start - SENSOR_INTERVAL_MS;
  uint32_t lastControlMs = start;
  TickType_t wake = xTaskGetTickCount();
  for (;;) {
    const uint32_t now = millis();
    controlHeartbeatMs = now;
    if (now - lastSensorMs >= SENSOR_INTERVAL_MS) {
      lastSensorMs = now;
      float value = NAN;
      char error[48] = "";
      const bool ok = readThermocouple(value, error, sizeof(error));  // SPI outside the lock
      StateLock lock;
      processReadingLocked(ok, value, error, now, c);
    }
    if (now - lastControlMs >= CONTROL_INTERVAL_MS) {
      const float dt = (now - lastControlMs) / 1000.0f;
      lastControlMs = now;
      StateLock lock;
      runControlLocked(dt, now, c);
    }
    {
      StateLock lock;
      driveRelayLocked(now, c);
    }
    vTaskDelayUntil(&wake, pdMS_TO_TICKS(CONTROL_TICK_MS));
  }
}

// Runs from the esp_timer task, independent of the control task and the mutex.
void heatWatchdog(void*) {
  // Load the heartbeat first; the control task on the other core may update it
  // between the two reads, so treat a "negative" age as fresh.
  const uint32_t heartbeat = controlHeartbeatMs.load();
  const int32_t age = static_cast<int32_t>(millis() - heartbeat);
  if (age > static_cast<int32_t>(HEAT_WATCHDOG_MS)) {
    setRelay(false);
    if (age > static_cast<int32_t>(HEAT_WATCHDOG_RESET_MS)) {
      ets_printf("Heat watchdog: control task stalled, restarting\n");
      esp_restart();
    }
  }
}

// ---------------------------------------------------------------------------
// Commands (web + MQTT)
// ---------------------------------------------------------------------------
bool executeCommand(const char* command, bool remote, char* error, size_t errorLen) {
  const bool isStart = strcmp(command, "start") == 0;
  const bool isStop = strcmp(command, "stop") == 0;
  if (remote) {
    if (isStart && !settings.mqttStart) { strlcpy(error, "Remote start is disabled", errorLen); return false; }
    if (!isStart && !isStop && !settings.mqttControl) { strlcpy(error, "Remote control is disabled", errorLen); return false; }
  }
  bool ok = true;
  {
    StateLock lock;  // firmwareBusy is only set while holding this lock (see beginFirmwareSession)
    const RunState s = kiln.state;
    if (firmwareBusy && !isStop) {
      strlcpy(error, "Firmware update in progress", errorLen);
      return false;
    }
    if (isStart && (s == RunState::IDLE || s == RunState::COMPLETE)) {
      if (SENSOR_CHECK_ENABLED && !kiln.sensorHealthy) {
        enterFaultLocked("Cannot start: thermocouple is not healthy");
        strlcpy(error, kiln.fault, errorLen);
        ok = false;
      } else {
        kiln.elapsedSeconds = 0;
        kiln.fault[0] = '\0';
        kiln.pidResetRequested = true;
        kiln.targetC = targetForElapsedLocked(0);
        kiln.state = RunState::RUNNING;
        kiln.checkpointPending = true;
        queueNotification("Kiln started", kiln.profileName);
      }
    } else if (strcmp(command, "pause") == 0 && s == RunState::RUNNING) {
      kiln.state = RunState::PAUSED;
      heatOffLocked();
      kiln.checkpointPending = true;
    } else if (strcmp(command, "resume") == 0 && s == RunState::PAUSED && (kiln.sensorHealthy || !SENSOR_CHECK_ENABLED)) {
      kiln.state = RunState::RUNNING;
      kiln.fault[0] = '\0';
      kiln.pidResetRequested = true;
    } else if (isStop) {
      heatOffLocked();
      if (s != RunState::FAULT) kiln.state = RunState::IDLE;  // a fault must be cleared explicitly
      kiln.checkpointPending = true;
      if (isActive(s)) queueNotification("Kiln stopped", kiln.profileName);
    } else if (strcmp(command, "clear") == 0 && s == RunState::FAULT &&
               (kiln.sensorHealthy || !SENSOR_CHECK_ENABLED) &&
               !(kiln.temperatureC >= EMERGENCY_SHUTOFF_C)) {
      kiln.fault[0] = '\0';
      kiln.state = RunState::IDLE;
      kiln.checkpointPending = true;
    } else {
      strlcpy(error, "Command is not valid in the current state", errorLen);
      ok = false;
    }
  }
  publishRequested = true;
  return ok;
}

// Guards both OTA paths (browser upload and GitHub download).
bool beginFirmwareSession(char* error, size_t errorLen) {
  StateLock lock;
  if (firmwareBusy) { strlcpy(error, "Firmware update already in progress", errorLen); return false; }
  if (isActive(kiln.state)) { strlcpy(error, "Stop the kiln before updating firmware", errorLen); return false; }
  firmwareBusy = true;
  heatOffLocked();
  return true;
}

// ---------------------------------------------------------------------------
// Outbound HTTP helpers (network task only)
// ---------------------------------------------------------------------------
void configureHttp(HTTPClient& http) {
  http.setConnectTimeout(HTTP_TIMEOUT_MS);
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.setUserAgent(USER_AGENT);
  http.setReuse(false);
}

String urlEncode(const String& v) {
  String o;
  o.reserve(v.length() + 8);
  char b[4];
  for (char c : v) {
    if (isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.' || c == '~') o += c;
    else { snprintf(b, sizeof(b), "%%%02X", static_cast<unsigned char>(c)); o += b; }
  }
  return o;
}

void sendPushover(const Notification& n) {
  if (!settings.pushoverEnabled || !WiFi.isConnected() || settings.pushoverToken.isEmpty() ||
      settings.pushoverUser.isEmpty()) return;
  WiFiClientSecure net;
  net.setInsecure();
  HTTPClient http;
  configureHttp(http);
  if (!http.begin(net, "https://api.pushover.net/1/messages.json")) return;
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  String body = "token=" + urlEncode(settings.pushoverToken) + "&user=" + urlEncode(settings.pushoverUser) +
                "&title=" + urlEncode(n.title) + "&message=" + urlEncode(n.body);
  int code = http.POST(body);
  http.end();
  Serial.printf("Pushover HTTP %d\n", code);
}

void appendInfluxTag(String& line, const char* value) {
  for (const char* p = value; *p; ++p) {
    if (*p == ',' || *p == '=' || *p == ' ') line += '\\';
    line += *p;
  }
}

void writeInflux() {
  if (!settings.influxEnabled || !WiFi.isConnected() || settings.influxUrl.isEmpty() ||
      settings.influxOrg.isEmpty() || settings.influxBucket.isEmpty() || settings.influxToken.isEmpty()) return;
  const KilnState s = snapshotKiln();
  String line = "kiln,profile=";
  line.reserve(200);
  appendInfluxTag(line, s.profileName);
  char fields[160];
  int n = 0;
  if (isfinite(s.temperatureC)) n = snprintf(fields, sizeof(fields), "temperature=%.2f,", s.temperatureC);
  snprintf(fields + n, sizeof(fields) - n, "target=%.2f,duty=%.1f,elapsed_minutes=%.2f,sensor_ok=%s",
           s.targetC, s.duty * 100.0f, s.elapsedSeconds / 60.0, s.sensorHealthy ? "true" : "false");
  line += ' ';
  line += fields;

  String url = settings.influxUrl;
  while (url.endsWith("/")) url.remove(url.length() - 1);
  url += "/api/v2/write?org=" + urlEncode(settings.influxOrg) + "&bucket=" + urlEncode(settings.influxBucket) +
         "&precision=s";
  WiFiClient plain;      // declared before `http` so they outlive it (~HTTPClient touches the client)
  WiFiClientSecure tls;
  HTTPClient http;
  configureHttp(http);
  bool begun;
  if (url.startsWith("https://")) { tls.setInsecure(); begun = http.begin(tls, url); }
  else begun = http.begin(plain, url);
  if (!begun) return;
  http.addHeader("Authorization", "Token " + settings.influxToken);
  http.addHeader("Content-Type", "text/plain; charset=utf-8");
  int code = http.POST(line);
  http.end();
  if (code != 204) Serial.printf("Influx HTTP %d\n", code);
}

int compareVersions(const char* a, const char* b) {
  if (*a == 'v' || *a == 'V') ++a;
  if (*b == 'v' || *b == 'V') ++b;
  for (int part = 0; part < 4; ++part) {
    char* ea;
    char* eb;
    long va = strtol(a, &ea, 10);
    long vb = strtol(b, &eb, 10);
    if (va != vb) return va < vb ? -1 : 1;
    a = (*ea == '.') ? ea + 1 : ea;
    b = (*eb == '.') ? eb + 1 : eb;
  }
  return 0;
}

// Uses GitHub's /releases/latest redirect instead of the REST API: two HEAD
// requests with no body, instead of downloading and scanning a large JSON
// document (release notes included) into a heap String.
bool fetchLatestRelease(char* tag, size_t tagLen, bool& hasAsset, char* error, size_t errorLen) {
  hasAsset = false;
  WiFiClientSecure net;
  net.setInsecure();
  const char* headerKeys[] = {"Location"};
  String location;
  {
    HTTPClient http;
    configureHttp(http);
    http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
    if (!http.begin(net, RELEASE_LATEST_URL)) { strlcpy(error, "Unable to contact update service", errorLen); return false; }
    http.collectHeaders(headerKeys, 1);
    int code = http.sendRequest("HEAD");
    location = http.header("Location");
    http.end();
    if (code < 300 || code >= 400 || location.isEmpty()) {
      snprintf(error, errorLen, "Update service HTTP %d", code);
      return false;
    }
  }
  int p = location.indexOf("/releases/tag/");
  if (p < 0) { strlcpy(error, "No published release found", errorLen); return false; }
  String t = location.substring(p + 14);
  int q = t.indexOf('?');
  if (q >= 0) t.remove(q);
  if (t.isEmpty() || t.length() >= tagLen) { strlcpy(error, "Invalid release tag", errorLen); return false; }
  strlcpy(tag, t.c_str(), tagLen);

  HTTPClient http;
  configureHttp(http);
  http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
  String assetUrl = String(RELEASE_DOWNLOAD_URL) + t + "/firmware.bin";
  if (http.begin(net, assetUrl)) {
    int code = http.sendRequest("HEAD");
    hasAsset = code == 200 || code == 301 || code == 302 || code == 307;
    http.end();
  }
  return true;
}

void runUpdateCheck() {
  {
    StateLock lock;
    upd.checkRequested = false;
    upd.checking = true;
    upd.checkError[0] = '\0';
  }
  char tag[24] = "";
  char error[80] = "";
  bool hasAsset = false;
  bool ok = false;
  if (WiFi.isConnected()) ok = fetchLatestRelease(tag, sizeof(tag), hasAsset, error, sizeof(error));
  else strlcpy(error, "No Internet connection", sizeof(error));
  StateLock lock;
  upd.checking = false;
  upd.checked = ok;
  if (ok) {
    strlcpy(upd.latest, tag, sizeof(upd.latest));
    upd.available = hasAsset && compareVersions(OPENKILN_VERSION, tag) < 0;
    if (!hasAsset) strlcpy(upd.checkError, "Latest release has no firmware.bin asset", sizeof(upd.checkError));
  } else {
    strlcpy(upd.checkError, error, sizeof(upd.checkError));
  }
  Serial.printf("Firmware check: installed %s, latest %s, update=%s %s\n", OPENKILN_VERSION, upd.latest,
                upd.available ? "yes" : "no", upd.checkError);
}

void setInstallProgress(const char* stage, const char* message, int progress) {
  StateLock lock;
  upd.stage = stage;
  if (message) strlcpy(upd.message, message, sizeof(upd.message));
  if (progress >= 0) upd.progress = progress;
}

bool downloadAndInstall(const char* url, char* error, size_t errorLen) {
  WiFiClientSecure net;
  net.setInsecure();
  HTTPClient http;
  configureHttp(http);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  if (!http.begin(net, url)) { strlcpy(error, "Unable to open firmware download", errorLen); return false; }
  int code = http.GET();
  if (code != HTTP_CODE_OK) { snprintf(error, errorLen, "Firmware download HTTP %d", code); http.end(); return false; }
  const int len = http.getSize();
  if (!Update.begin(len > 0 ? static_cast<size_t>(len) : UPDATE_SIZE_UNKNOWN)) {
    strlcpy(error, "Not enough OTA space", errorLen);
    http.end();
    return false;
  }
  WiFiClient* stream = http.getStreamPtr();
  static uint8_t buf[2048];
  size_t written = 0;
  uint32_t lastDataMs = millis();
  int lastPct = -1;
  while (len < 0 || written < static_cast<size_t>(len)) {
    const size_t avail = stream->available();
    if (avail) {
      const int got = stream->readBytes(buf, min(avail, sizeof(buf)));
      if (got <= 0) break;
      if (Update.write(buf, got) != static_cast<size_t>(got)) {
        snprintf(error, errorLen, "Firmware write failed: %s", Update.errorString());
        Update.abort();
        http.end();
        return false;
      }
      written += got;
      lastDataMs = millis();
      if (len > 0) {
        const int pct = constrain(static_cast<int>(written * 90ULL / len), 1, 90);
        if (pct != lastPct) { lastPct = pct; setInstallProgress("downloading", nullptr, pct); }
      }
    } else {
      if (!stream->connected()) break;
      if (millis() - lastDataMs > 15000) { strlcpy(error, "Firmware download stalled", errorLen); Update.abort(); http.end(); return false; }
      delay(2);
    }
  }
  http.end();
  if (len > 0 && written != static_cast<size_t>(len)) {  // check BEFORE finalising the image
    strlcpy(error, "Incomplete firmware download", errorLen);
    Update.abort();
    return false;
  }
  setInstallProgress("installing", "Installing firmware", 95);
  if (!Update.end(true)) {  // verifies the image checksum/SHA before switching partitions
    snprintf(error, errorLen, "Firmware installation failed: %s", Update.errorString());
    return false;
  }
  return true;
}

void runFirmwareInstall() {
  char url[160];
  {
    StateLock lock;
    upd.installRequested = false;
    snprintf(url, sizeof(url), "%s%s/firmware.bin", RELEASE_DOWNLOAD_URL, upd.latest);
  }
  setInstallProgress("downloading", "Downloading firmware", 1);
  char error[96] = "";
  if (!downloadAndInstall(url, error, sizeof(error))) {
    {
      StateLock lock;
      upd.installing = false;
      upd.stage = "error";
      strlcpy(upd.message, error, sizeof(upd.message));
    }
    firmwareBusy = false;
    Serial.printf("Firmware update failed: %s\n", error);
    return;
  }
  setInstallProgress("rebooting", "Update installed. Rebooting OpenKiln", 100);
  delay(1800);
  ESP.restart();
}

// ---------------------------------------------------------------------------
// MQTT (network task only)
// ---------------------------------------------------------------------------
char mqttHostBuf[64];  // PubSubClient keeps the pointer; must outlive the client
char mqttCommandTopic[80];
uint32_t mqttPublishedRevision = 0;

void mqttTopic(char* out, size_t n, const char* suffix) {
  snprintf(out, n, "%s/%s", settings.mqttTopic.c_str(), suffix);
}

void mqttPublish(const char* suffix, const char* payload, bool retained = true) {
  char topic[96];
  mqttTopic(topic, sizeof(topic), suffix);
  mqttClient.publish(topic, payload, retained);
}

void publishMqttStatus() {
  if (!mqttClient.connected()) return;
  const KilnState s = snapshotKiln();
  char buf[512];
  Json j(buf, sizeof(buf));
  j.beginObject()
      .kvStr("state", stateName(s.state))
      .kvNum("temperature_c", s.temperatureC, 2)
      .kvNum("target_c", s.targetC, 2)
      .kvNum("heating_rate_c_h", s.rateCPerHour, 1)
      .kvNum("duty_percent", s.duty * 100.0f, 1)
      .kvNum("elapsed_minutes", s.elapsedSeconds / 60.0, 2)
      .kvBool("sensor_ok", s.sensorHealthy)
      .kvStr("fault", s.fault)
      .kvStr("profile", s.profileName)
      .endObject();
  mqttPublish("status", buf);

  // The profile rarely changes: publish it on (re)connect and on change only.
  if (s.profileRevision != mqttPublishedRevision) {
    Json p(buf, sizeof(buf));
    p.beginObject().kvStr("name", s.profileName).key("data");
    writeProfilePoints(p, s.profile, s.profileCount);
    p.endObject();
    if (p.ok() && mqttClient.publish("kiln/profile_json", buf, true)) mqttPublishedRevision = s.profileRevision;
  }

  char v[16];
  mqttPublish("state", stateName(s.state));
  if (isfinite(s.temperatureC)) snprintf(v, sizeof(v), "%.2f", s.temperatureC); else strlcpy(v, "nan", sizeof(v));
  mqttPublish("temperature_c", v);
  snprintf(v, sizeof(v), "%.2f", s.targetC);
  mqttPublish("target_c", v);
  snprintf(v, sizeof(v), "%.1f", s.duty * 100.0f);
  mqttPublish("duty_percent", v);
  mqttPublish("fault", s.fault);
}

void onMqttMessage(char* topic, byte* payload, unsigned int length) {
  if (strcmp(topic, mqttCommandTopic) != 0) return;
  char command[16];
  size_t n = 0;
  for (unsigned int i = 0; i < length && n + 1 < sizeof(command); ++i) {
    const char c = static_cast<char>(payload[i]);
    if (!isspace(static_cast<unsigned char>(c))) command[n++] = static_cast<char>(tolower(c));
  }
  command[n] = '\0';
  char error[80] = "";
  const bool ok = executeCommand(command, true, error, sizeof(error));
  char result[100];
  snprintf(result, sizeof(result), ok ? "ok:%s" : "error:%s", ok ? command : error);
  mqttPublish("command_result", result, false);
  publishMqttStatus();
}

void maintainMqtt(uint32_t now) {
  static uint32_t lastAttemptMs = 0;
  static uint32_t lastPublishMs = 0;
  if (!settings.mqttEnabled || settings.mqttHost.isEmpty() || !WiFi.isConnected()) return;
  if (!mqttClient.connected()) {
    if (lastAttemptMs != 0 && now - lastAttemptMs < 5000) return;
    lastAttemptMs = now;
    char clientId[32];
    snprintf(clientId, sizeof(clientId), "esp32-kiln-%08lx", static_cast<unsigned long>(ESP.getEfuseMac()));
    char availability[96];
    mqttTopic(availability, sizeof(availability), "availability");
    const bool connected = settings.mqttUser.isEmpty()
        ? mqttClient.connect(clientId, availability, 1, true, "offline")
        : mqttClient.connect(clientId, settings.mqttUser.c_str(), settings.mqttPassword.c_str(), availability, 1, true, "offline");
    if (!connected) {
      Serial.printf("MQTT connection failed, state=%d\n", mqttClient.state());
      return;
    }
    mqttClient.publish(availability, "online", true);
    mqttClient.subscribe(mqttCommandTopic, 1);
    mqttPublishedRevision = 0;
    publishRequested = true;
  }
  mqttClient.loop();
  if (publishRequested.exchange(false) || now - lastPublishMs >= MQTT_PUBLISH_INTERVAL_MS) {
    lastPublishMs = now;
    publishMqttStatus();
  }
}

// ---------------------------------------------------------------------------
// Network task
// ---------------------------------------------------------------------------
void networkTask(void*) {
  bool timeConfigured = false;
  bool wasConnected = false;
  uint32_t lastWifiAttemptMs = millis();
  uint32_t lastInfluxMs = 0;
  uint32_t lastUpdateCheckMs = 0;
  bool firstUpdateCheckDone = false;

  for (;;) {
    const uint32_t now = millis();
    const bool connected = WiFi.isConnected();

    if (connected && !wasConnected) {
      Serial.printf("Wi-Fi: http://%s (http://%s.local)\n", WiFi.localIP().toString().c_str(), settings.hostname.c_str());
      if (!timeConfigured) {
        configTzTime(settings.timezone.c_str(), settings.ntpServer.c_str(), "time.cloudflare.com");
        timeConfigured = true;
      }
    }
    wasConnected = connected;
    if (!connected && !settings.wifiSsid.isEmpty() && now - lastWifiAttemptMs >= WIFI_RETRY_INTERVAL_MS) {
      lastWifiAttemptMs = now;
      WiFi.reconnect();
    }

    serviceAutoRestart();
    maintainMqtt(now);

    Notification n;
    while (xQueueReceive(notifyQueue, &n, 0) == pdTRUE) sendPushover(n);

    if (settings.influxEnabled && connected && now - lastInfluxMs >= settings.influxInterval * 1000UL) {
      lastInfluxMs = now;
      writeInflux();
    }

    bool active, checkRequested, installRequested;
    {
      StateLock lock;
      active = isActive(kiln.state);
      checkRequested = upd.checkRequested;
      installRequested = upd.installRequested;
    }
    const bool periodicCheckDue = connected && !active &&
        ((!firstUpdateCheckDone && now >= FIRST_UPDATE_CHECK_MS) ||
         (firstUpdateCheckDone && now - lastUpdateCheckMs >= UPDATE_CHECK_INTERVAL_MS));
    if (checkRequested || periodicCheckDue) {
      firstUpdateCheckDone = true;
      lastUpdateCheckMs = now;
      runUpdateCheck();
    }
    if (installRequested) runFirmwareInstall();

    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void startAccessPoint() {
  String apPass = settings.apPassword;
  if (apPass.length() < 8) apPass = "kilnsetup";
  WiFi.softAP(settings.apName.c_str(), apPass.c_str());
}

void startNetwork() {
  WiFi.persistent(false);  // don't rewrite Wi-Fi credentials to flash on every boot
  WiFi.setHostname(settings.hostname.c_str());
  WiFi.mode(WIFI_AP_STA);
  WiFi.setSleep(false);    // mains powered: lower latency for the web UI and MQTT
  WiFi.setAutoReconnect(true);
  startAccessPoint();
  Serial.printf("AP: %s http://%s\n", settings.apName.c_str(), WiFi.softAPIP().toString().c_str());
  if (!settings.wifiSsid.isEmpty()) {
    WiFi.begin(settings.wifiSsid.c_str(), settings.wifiPassword.c_str());  // non-blocking
    Serial.printf("Connecting to %s\n", settings.wifiSsid.c_str());
  }
  if (MDNS.begin(settings.hostname.c_str())) MDNS.addService("http", "tcp", 80);

  strlcpy(mqttHostBuf, settings.mqttHost.c_str(), sizeof(mqttHostBuf));
  mqttTopic(mqttCommandTopic, sizeof(mqttCommandTopic), "command");
  mqttClient.setServer(mqttHostBuf, settings.mqttPort);
  mqttClient.setCallback(onMqttMessage);
  mqttClient.setBufferSize(768);
  mqttClient.setKeepAlive(30);
  mqttClient.setSocketTimeout(5);
}

// ---------------------------------------------------------------------------
// Web server (loop task)
// ---------------------------------------------------------------------------
bool authenticate() {
  if (!settings.authRequired) return true;
  if (server.authenticate(settings.webUser.c_str(), settings.webPassword.c_str())) return true;
  server.requestAuthentication(BASIC_AUTH, "OpenKiln");
  return false;
}

void sendAsset(const WebAsset& asset) {
  if (!authenticate()) return;
  server.sendHeader("Cache-Control", "no-cache");
  server.sendHeader("ETag", asset.etag);
  if (server.header("If-None-Match") == asset.etag) {
    server.send(304);
    return;
  }
  server.sendHeader("Content-Encoding", "gzip");
  server.send_P(200, asset.mime, reinterpret_cast<const char*>(asset.data), asset.length);
}

void handleStatus() {
  if (!authenticate()) return;
  const KilnState s = snapshotKiln();
  const UpdateState u = snapshotUpdate();
  char buf[900];
  Json j(buf, sizeof(buf));
  j.beginObject()
      .kvStr("state", stateName(s.state))
      .kvNum("temp", s.temperatureC, 1)
      .kvNum("target", s.targetC, 1)
      .kvNum("rate", s.rateCPerHour, 1)
      .kvNum("duty", s.duty * 100.0f, 1)
      .kvNum("elapsed", s.elapsedSeconds / 60.0, 1)
      .kvNum("total", s.profileCount ? s.profile[s.profileCount - 1].minute : 0.0f, 1)
      .kvBool("sensor_ok", s.sensorHealthy)
      .kvBool("sensor_check", SENSOR_CHECK_ENABLED)
      .kvStr("fault", s.fault)
      .kvStr("profile", s.profileName)
      .kvInt("slot", s.activeSlot)
      .kvInt("profile_rev", s.profileRevision)
      .kvStr("firmware_version", OPENKILN_VERSION)
      .kvBool("update_available", u.available)
      .kvStr("latest_version", u.latest)
      .kvStr("update_error", u.checkError)
      .kvBool("firmware_busy", firmwareBusy);
  const time_t nowTime = time(nullptr);
  const bool valid = nowTime >= VALID_EPOCH;
  j.kvBool("time_valid", valid);
  if (valid) {
    struct tm tmv;
    localtime_r(&nowTime, &tmv);
    char tb[32];
    strftime(tb, sizeof(tb), "%Y-%m-%d %H:%M:%S", &tmv);
    j.kvStr("local_time", tb);
  }
  j.endObject();
  sendJson(j);
}

void handleProfilesGet() {
  if (!authenticate()) return;
  char buf[1024];
  Json j(buf, sizeof(buf));
  if (server.hasArg("slot")) {
    const int slot = server.arg("slot").toInt();
    if (slot < 0 || slot >= static_cast<int>(MAX_SAVED_PROFILES)) { server.send(400, "text/plain", "Invalid slot"); return; }
    String name = readProfileName(slot);
    String data = readProfileData(slot);
    ProfilePoint points[MAX_PROFILE_POINTS];
    size_t count = 0;
    String error;
    if (name.isEmpty() || data.isEmpty()) { server.send(404, "text/plain", "Profile not found"); return; }
    if (!parseProfile(data, points, count, error)) count = 0;
    j.beginObject().kvInt("slot", slot).kvStr("name", name).key("data");
    writeProfilePoints(j, points, count);
    j.endObject();
    sendJson(j);
    return;
  }
  const uint8_t active = snapshotKiln().activeSlot;
  j.beginArray();
  for (size_t i = 0; i < MAX_SAVED_PROFILES; ++i) {
    String name = readProfileName(i);
    if (name.isEmpty()) continue;
    j.beginObject().kvInt("slot", i).kvStr("name", name).kvBool("active", i == active).endObject();
  }
  j.endArray();
  sendJson(j);
}

void handleProfilesPost() {
  if (!authenticate()) return;
  if (isActive(snapshotKiln().state)) { server.send(409, "text/plain", "Stop the kiln before changing profiles"); return; }
  const String action = server.arg("action");
  int slot = server.hasArg("slot") ? server.arg("slot").toInt() : -1;
  char key[8];
  if (action == "save") {
    if (slot < 0 || slot >= static_cast<int>(MAX_SAVED_PROFILES)) {
      slot = -1;
      for (size_t i = 0; i < MAX_SAVED_PROFILES; ++i) {
        if (readProfileName(i).isEmpty()) { slot = i; break; }
      }
      if (slot < 0) { server.send(409, "text/plain", "Profile library is full (maximum 8)"); return; }
    }
    String name = server.arg("name");
    name.trim();
    if (name.isEmpty()) { server.send(400, "text/plain", "Profile name is required"); return; }
    if (name.length() > MAX_PROFILE_NAME) name.remove(MAX_PROFILE_NAME);
    String error;
    String text = profileTextFromJsonArray(server.arg("data"), error);
    if (text.isEmpty()) { server.send(400, "text/plain", error); return; }
    profileNameKey(key, sizeof(key), slot);
    preferences.putString(key, name);
    profileDataKey(key, sizeof(key), slot);
    preferences.putString(key, text);
    if (static_cast<uint8_t>(slot) == snapshotKiln().activeSlot) activateProfileSlot(slot, error);  // keep RAM copy in sync
    char buf[48];
    Json j(buf, sizeof(buf));
    j.beginObject().kvBool("success", true).kvInt("slot", slot).endObject();
    sendJson(j);
    return;
  }
  if (slot < 0 || slot >= static_cast<int>(MAX_SAVED_PROFILES)) { server.send(400, "text/plain", "Invalid slot"); return; }
  if (action == "select") {
    String error;
    if (!activateProfileSlot(slot, error)) { server.send(400, "text/plain", error); return; }
    server.send(200, "application/json", "{\"success\":true}");
    return;
  }
  if (action == "delete") {
    if (static_cast<uint8_t>(slot) == snapshotKiln().activeSlot) {
      server.send(409, "text/plain", "Select another profile before deleting the active profile");
      return;
    }
    profileNameKey(key, sizeof(key), slot);
    preferences.remove(key);
    profileDataKey(key, sizeof(key), slot);
    preferences.remove(key);
    server.send(200, "application/json", "{\"success\":true}");
    return;
  }
  server.send(400, "text/plain", "Unknown action");
}

// Legacy single-profile API (text format), kept for existing integrations.
void handleLegacyProfilePost() {
  if (!authenticate()) return;
  const String text = server.arg("data");
  ProfilePoint candidate[MAX_PROFILE_POINTS];
  size_t count = 0;
  String error;
  if (!parseProfile(text, candidate, count, error)) { server.send(400, "text/plain", error); return; }
  uint8_t slot = 0;
  bool busy;
  {
    StateLock lock;
    busy = isActive(kiln.state) || recoveryPending;
    if (!busy) {
      memcpy(kiln.profile, candidate, sizeof(ProfilePoint) * count);
      kiln.profileCount = count;
      kiln.profileRevision++;
      slot = kiln.activeSlot;
    }
  }
  if (busy) { server.send(409, "text/plain", "Stop the kiln before changing its profile"); return; }
  char key[8];
  profileDataKey(key, sizeof(key), slot);
  preferences.putString(key, text);
  preferences.putString("profile", text);
  publishRequested = true;
  server.send(200, "application/json", "{\"success\":true}");
}

void handleSettingsGet() {
  if (!authenticate()) return;
  Settings s;
  loadSettings(s);  // report saved values (they may differ from the running config until reboot)
  char buf[1536];
  Json j(buf, sizeof(buf));
  j.beginObject()
      .kvStr("hostname", s.hostname).kvStr("language", s.language)
      .kvStr("auth_required", s.authRequired ? "1" : "0")
      .kvStr("wifi_ssid", s.wifiSsid).kvStr("ap_name", s.apName).kvStr("web_user", s.webUser)
      .kvBool("mqtt_enabled", s.mqttEnabled).kvStr("mqtt_host", s.mqttHost).kvInt("mqtt_port", s.mqttPort)
      .kvStr("mqtt_user", s.mqttUser).kvStr("mqtt_topic", s.mqttTopic)
      .kvBool("mqtt_control", s.mqttControl).kvBool("mqtt_start", s.mqttStart)
      .kvBool("influx_enabled", s.influxEnabled).kvStr("influx_url", s.influxUrl).kvStr("influx_org", s.influxOrg)
      .kvStr("influx_bucket", s.influxBucket).kvInt("influx_interval", s.influxInterval)
      .kvBool("pushover_enabled", s.pushoverEnabled)
      .kvStr("ntp_server", s.ntpServer).kvStr("timezone", s.timezone)
      .endObject();
  sendJson(j);
}

void putTextArg(const char* arg, const char* key, size_t maxLen) {
  if (!server.hasArg(arg)) return;
  String v = server.arg(arg);
  if (v.length() > maxLen) v.remove(maxLen);
  if (preferences.getString(key, "") != v) preferences.putString(key, v);
}

void putSecretArg(const char* arg, const char* key) {  // blank = keep current
  if (server.hasArg(arg) && !server.arg(arg).isEmpty()) preferences.putString(key, server.arg(arg));
}

void putFlagArg(const char* arg, const char* key) {
  const bool v = server.hasArg(arg);
  if (preferences.getBool(key, !v) != v) preferences.putBool(key, v);
}

void handleSettingsPost() {
  if (!authenticate()) return;
  if (snapshotKiln().state == RunState::RUNNING) { server.send(409, "text/plain", "Stop kiln before changing setup"); return; }
  if (server.hasArg("ap_password") && !server.arg("ap_password").isEmpty() && server.arg("ap_password").length() < 8) {
    server.send(400, "text/plain", "AP password must be at least 8 characters");
    return;
  }
  if (server.hasArg("hostname")) preferences.putString("hostname", sanitizeHostname(server.arg("hostname")));
  if (server.hasArg("language")) preferences.putString("language", server.arg("language") == "da" ? "da" : "en");
  if (server.hasArg("auth_required")) preferences.putBool("authReq", server.arg("auth_required") != "0");
  putTextArg("wifi_ssid", "wifiSsid", 32);
  putSecretArg("wifi_password", "wifiPass");
  putTextArg("ap_name", "apName", 32);
  putSecretArg("ap_password", "apPass");
  putTextArg("web_user", "webUser", 32);
  putSecretArg("web_password", "webPass");

  putFlagArg("mqtt_enabled", "mqttEn");
  putTextArg("mqtt_host", "mqttHost", 63);
  if (server.hasArg("mqtt_port")) {
    long port = server.arg("mqtt_port").toInt();
    preferences.putUShort("mqttPort", (port > 0 && port <= 65535) ? static_cast<uint16_t>(port) : 1883);
  }
  putTextArg("mqtt_user", "mqttUser", 64);
  putSecretArg("mqtt_password", "mqttPass");
  putTextArg("mqtt_topic", "mqttTopic", 60);
  putFlagArg("mqtt_control", "mqttCtl");
  putFlagArg("mqtt_start", "mqttStart");

  putFlagArg("influx_enabled", "influxEn");
  putTextArg("influx_url", "influxUrl", 128);
  putTextArg("influx_org", "influxOrg", 64);
  putTextArg("influx_bucket", "influxBucket", 64);
  putSecretArg("influx_token", "influxToken");
  if (server.hasArg("influx_interval")) {
    long interval = constrain(server.arg("influx_interval").toInt(), 5L, 86400L);
    preferences.putUInt("influxInt", static_cast<uint32_t>(interval));
  }

  putFlagArg("pushover_enabled", "pushEn");
  putSecretArg("pushover_token", "pushToken");
  putSecretArg("pushover_user", "pushUser");
  putTextArg("ntp_server", "ntp", 64);
  putTextArg("timezone", "tz", 63);
  server.send(200, "application/json", "{\"success\":true,\"reboot_required\":true}");
}

void handleUpdateStatus() {
  if (!authenticate()) return;
  const UpdateState u = snapshotUpdate();
  char buf[400];
  Json j(buf, sizeof(buf));
  j.beginObject()
      .kvStr("installed", OPENKILN_VERSION)
      .kvBool("checking", u.checking || u.checkRequested)
      .kvBool("checked", u.checked)
      .kvBool("available", u.available)
      .kvStr("latest", u.latest)
      .kvStr("check_error", u.checkError)
      .kvBool("running", u.installing)
      .kvInt("progress", u.progress)
      .kvStr("stage", u.stage)
      .kvStr("message", u.message)
      .endObject();
  sendJson(j);
}

void handleWifiStatus() {
  if (!authenticate()) return;
  const bool connected = WiFi.isConnected();
  const bool apOn = (WiFi.getMode() & WIFI_MODE_AP) != 0;
  char buf[320];
  Json j(buf, sizeof(buf));
  j.beginObject()
      .kvBool("connected", connected)
      .kvStr("ssid", connected ? WiFi.SSID() : String())
      .kvStr("ip", connected ? WiFi.localIP().toString() : String())
      .kvInt("rssi", connected ? WiFi.RSSI() : 0)
      .kvStr("hostname", settings.hostname)
      .kvBool("ap_enabled", apOn)
      .kvStr("ap_ssid", apOn ? settings.apName : String())
      .kvStr("ap_ip", apOn ? WiFi.softAPIP().toString() : String())
      .kvInt("ap_clients", apOn ? WiFi.softAPgetStationNum() : 0)
      .endObject();
  sendJson(j);
}

void handleSystem() {
  if (!authenticate()) return;
  const bool connected = WiFi.isConnected();
  char buf[400];
  Json j(buf, sizeof(buf));
  j.beginObject()
      .kvInt("heapTotal", ESP.getHeapSize())
      .kvInt("heapFree", ESP.getFreeHeap())
      .kvInt("heapMinFree", ESP.getMinFreeHeap())
      .kvInt("heapLargest", ESP.getMaxAllocHeap())
      .kvInt("flashSize", ESP.getFlashChipSize())
      .kvInt("sketchSize", ESP.getSketchSize())
      .kvInt("freeSketch", ESP.getFreeSketchSpace())
      .kvInt("cpuMHz", ESP.getCpuFreqMHz())
      .kvInt("uptime", millis() / 1000UL)
      .kvStr("version", OPENKILN_VERSION)
      .kvBool("connected", connected)
      .kvStr("ssid", connected ? WiFi.SSID() : String())
      .kvStr("ip", connected ? WiFi.localIP().toString() : String())
      .kvInt("rssi", connected ? WiFi.RSSI() : 0)
      .endObject();
  sendJson(j);
}

void installWebRoutes() {
  static const char* headerKeys[] = {"If-None-Match"};
  server.collectHeaders(headerKeys, 1);

  server.on("/", HTTP_GET, [] { sendAsset(WEB_INDEX_HTML); });
  server.on("/profiles", HTTP_GET, [] { sendAsset(WEB_PROFILES_HTML); });
  server.on("/setup", HTTP_GET, [] { sendAsset(WEB_SETUP_HTML); });
  server.on("/system", HTTP_GET, [] { sendAsset(WEB_SYSTEM_HTML); });
  server.on("/app.css", HTTP_GET, [] { sendAsset(WEB_APP_CSS); });
  server.on("/app.js", HTTP_GET, [] { sendAsset(WEB_APP_JS); });
  server.on("/favicon.png", HTTP_GET, [] { sendAsset(WEB_FAVICON_PNG); });

  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/profiles", HTTP_GET, handleProfilesGet);
  server.on("/api/profiles", HTTP_POST, handleProfilesPost);
  server.on("/api/profile", HTTP_GET, [] {
    if (!authenticate()) return;
    server.send(200, "text/plain", readProfileData(snapshotKiln().activeSlot));
  });
  server.on("/api/profile", HTTP_POST, handleLegacyProfilePost);
  server.on("/api/settings", HTTP_GET, handleSettingsGet);
  server.on("/api/settings", HTTP_POST, handleSettingsPost);
  server.on("/api/wifi-status", HTTP_GET, handleWifiStatus);
  server.on("/api/system", HTTP_GET, handleSystem);

  server.on("/api/control", HTTP_POST, [] {
    if (!authenticate()) return;
    char error[96] = "";
    if (!executeCommand(server.arg("cmd").c_str(), false, error, sizeof(error))) {
      server.send(409, "text/plain", error);
      return;
    }
    server.send(200, "application/json", "{\"success\":true}");
  });

  server.on("/api/ap/enable", HTTP_POST, [] {
    if (!authenticate()) return;
    WiFi.mode(WIFI_AP_STA);
    startAccessPoint();
    server.send(200, "application/json", "{\"ok\":true,\"enabled\":true}");
  });
  server.on("/api/ap/disable", HTTP_POST, [] {
    if (!authenticate()) return;
    if (!WiFi.isConnected()) {
      server.send(409, "text/plain", "Connect to Wi-Fi before disabling the access point");
      return;
    }
    server.send(200, "application/json", "{\"ok\":true,\"enabled\":false}");
    delay(50);
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
  });

  server.on("/api/reboot", HTTP_POST, [] {
    if (!authenticate()) return;
    if (snapshotKiln().state == RunState::RUNNING) { server.send(409, "text/plain", "Stop kiln before reboot"); return; }
    server.send(200, "application/json", "{\"success\":true}");
    delay(300);
    ESP.restart();
  });

  server.on("/api/update/check", HTTP_POST, [] {
    if (!authenticate()) return;
    if (isActive(snapshotKiln().state)) { server.send(409, "text/plain", "Update checks are deferred while the kiln is firing"); return; }
    { StateLock lock; upd.checkRequested = true; }
    server.send(202, "application/json", "{\"success\":true,\"started\":true}");
  });
  server.on("/api/update/install", HTTP_POST, [] {
    if (!authenticate()) return;
    if (!snapshotUpdate().available) { server.send(409, "text/plain", "No firmware update is available"); return; }
    char error[80];
    if (!beginFirmwareSession(error, sizeof(error))) { server.send(409, "text/plain", error); return; }
    {
      StateLock lock;
      upd.installRequested = true;
      upd.installing = true;
      upd.progress = 0;
      upd.stage = "starting";
      strlcpy(upd.message, "Preparing firmware update", sizeof(upd.message));
    }
    server.send(202, "application/json", "{\"success\":true,\"started\":true}");
  });
  server.on("/api/update/status", HTTP_GET, handleUpdateStatus);

  // Manual firmware upload (multipart). Heating is forced off for the duration.
  server.on("/update", HTTP_POST,
    [] {
      if (!authenticate()) return;
      const bool ok = otaUploadOk && !otaUploadRejected;
      server.send(ok ? 200 : 500, "text/plain", ok ? "OK - rebooting" : (Update.hasError() ? Update.errorString() : "Update rejected"));
      if (ok) { delay(500); ESP.restart(); }
      if (otaSessionStarted) { otaSessionStarted = false; firmwareBusy = false; }
    },
    [] {
      HTTPUpload& up = server.upload();
      if (up.status == UPLOAD_FILE_START) {
        otaUploadOk = false;
        otaUploadRejected = false;
        if (otaSessionStarted) { otaSessionStarted = false; firmwareBusy = false; }  // previous upload never finished
        char error[80];
        if ((settings.authRequired && !server.authenticate(settings.webUser.c_str(), settings.webPassword.c_str())) ||
            !beginFirmwareSession(error, sizeof(error))) {
          otaUploadRejected = true;
          return;
        }
        otaSessionStarted = true;
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) otaUploadRejected = true;
      } else if (up.status == UPLOAD_FILE_WRITE) {
        if (!otaUploadRejected && Update.write(up.buf, up.currentSize) != up.currentSize) otaUploadRejected = true;
      } else if (up.status == UPLOAD_FILE_END) {
        if (!otaUploadRejected) otaUploadOk = Update.end(true);
      } else if (up.status == UPLOAD_FILE_ABORTED) {
        // WebServer does not call the request handler after an abort, so clean up here.
        if (otaSessionStarted) { Update.abort(); otaSessionStarted = false; firmwareBusy = false; }
        otaUploadRejected = true;
      }
    });

  server.onNotFound([] { server.send(404, "text/plain", "Not found"); });
}

}  // namespace

// ---------------------------------------------------------------------------
// Arduino entry points
// ---------------------------------------------------------------------------
void setup() {
  pinMode(PIN_SSR, OUTPUT);
  setRelay(false);  // Fail-safe state before doing anything else.
  Serial.begin(115200);
  Serial.printf("\nOpenKiln %s\n", OPENKILN_VERSION);
  if (!SENSOR_CHECK_ENABLED) Serial.println("WARNING: thermocouple fault checking is DISABLED (Config.h)");

  stateMutex = xSemaphoreCreateMutex();
  notifyQueue = xQueueCreate(8, sizeof(Notification));
  preferences.begin("kiln", false);
  loadSettings(settings);
  loadProfileAtBoot();   // must run before loadBootCheckpoint() sets recoveryPending
  loadBootCheckpoint();
  beginThermocouple();

  controlHeartbeatMs = millis();
  xTaskCreatePinnedToCore(controlTask, "control", 4096, nullptr, 5, nullptr, 1);

  esp_timer_create_args_t wd = {};
  wd.callback = &heatWatchdog;
  wd.name = "heat-wd";
  esp_timer_handle_t wdTimer = nullptr;
  if (esp_timer_create(&wd, &wdTimer) == ESP_OK) esp_timer_start_periodic(wdTimer, 250000);  // 250 ms

  startNetwork();
  installWebRoutes();
  server.begin();
  xTaskCreatePinnedToCore(networkTask, "network", 12288, nullptr, 1, nullptr, 0);
  Serial.println("Kiln controller ready");
}

void loop() {
  server.handleClient();
  serviceCheckpoint(millis());
  delay(1);
}

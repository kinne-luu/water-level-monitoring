#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <Update.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include "esp_task_wdt.h"
#include "esp_ota_ops.h"
#include "confidential.h"

#define LOG_Q_SIZE 24
#define LOG_LINE_MAX 120

class MirrorPrint : public Print {
 public:
  void begin(unsigned long baud) { Serial.begin(baud); }
  size_t write(uint8_t c) override {
    Serial.write(c);
    capture(c);
    return 1;
  }
  size_t write(const uint8_t* b, size_t n) override {
    Serial.write(b, n);
    for (size_t i = 0; i < n; i++) capture(b[i]);
    return n;
  }
  bool popLine(char* out, size_t cap) {
    bool ok = false;
    portENTER_CRITICAL(&mux);
    if (qCount > 0) {
      strncpy(out, q[qHead], cap - 1);
      out[cap - 1] = '\0';
      qHead = (qHead + 1) % LOG_Q_SIZE;
      qCount--;
      ok = true;
    }
    portEXIT_CRITICAL(&mux);
    return ok;
  }
 private:
  char q[LOG_Q_SIZE][LOG_LINE_MAX];
  int qHead = 0;
  int qCount = 0;
  char cur[LOG_LINE_MAX];
  size_t curLen = 0;
  portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
  void capture(uint8_t c) {
    if (c == '\r') return;
    if (c == '\n') {
      if (curLen == 0) return;
      cur[curLen] = '\0';
      portENTER_CRITICAL(&mux);
      if (qCount == LOG_Q_SIZE) {
        qHead = (qHead + 1) % LOG_Q_SIZE;
        qCount--;
      }
      int tail = (qHead + qCount) % LOG_Q_SIZE;
      memcpy(q[tail], cur, curLen + 1);
      qCount++;
      portEXIT_CRITICAL(&mux);
      curLen = 0;
      return;
    }
    if (curLen < LOG_LINE_MAX - 1) cur[curLen++] = (char)c;
  }
};

MirrorPrint mirrorSerial;
#define Serial mirrorSerial

#define FIRMWARE_VERSION "1.1.2"
#define ENABLE_OTA_VERSION_CHECK 1
#define WDT_TIMEOUT_SEC 25
#define BOOT_CONFIRM_WINDOW_MS 60000UL
#define BOOT_FAIL_ROLLBACK_THRESHOLD 5
#define TRIG_PIN 5
#define ECHO_PIN 18
#define LED_GREEN 25
#define LED_YELLOW 26
#define LED_RED 27
#define BUZZER_PIN 19
#define LED_WIFI 2
#define I2C_SDA 21
#define I2C_SCL 22

LiquidCrystal_I2C lcd(0x27, 16, 2);
float DANGER_THRESHOLD = 15.0;
float WARN_THRESHOLD   = 40.0;
float DETECT_THRESHOLD = 180.0;
const float MAX_DISTANCE = 400.0;
const float DEFAULT_SENSOR_HEIGHT_CM = 200.0;
float SENSOR_HEIGHT_CM = DEFAULT_SENSOR_HEIGHT_CM;
float currentWaterLevel = -1;
const char* ALERT_WORKER_URL    = "https://waterlevelmonitor.luumanhkien08092006.workers.dev/alert";
const char* LOG_WORKER_URL      = "https://waterlevelmonitor.luumanhkien08092006.workers.dev/log";
const char* SETTINGS_WORKER_URL = "https://waterlevelmonitor.luumanhkien08092006.workers.dev/get-settings";
const char* OTA_CHECKIN_URL     = "https://waterlevelmonitor.luumanhkien08092006.workers.dev/ota/checkin";
const char* DEVICE_ID = "HCSR04";
const char* DEVICE_KEY_VALUE = DEVICE_KEY_SECRET;
String lastSentLevel = "";
const float LOG_CHANGE_THRESHOLD = 3.0;
const unsigned long HEARTBEAT_SAFE_MS = 30UL * 60UL * 1000UL;
const unsigned long HEARTBEAT_LIGHT_DETECT_MS = 15UL * 60UL * 1000UL;
const unsigned long HEARTBEAT_DEEP_DETECT_MS = 1UL * 60UL * 1000UL;
const float DEEP_DETECT_MARGIN_CM = 5.0;
const unsigned long ACTIVE_CHANGE_WINDOW_MS = 60UL * 1000UL;
int lastLoggedDistance = -9999;
String lastLoggedLevel = "";
unsigned long lastLogTime = 0;
unsigned long lastChangeTime = 0;
const unsigned long SETTINGS_FETCH_INTERVAL_MS = 20UL * 1000UL;
unsigned long lastSettingsFetch = 0;
const unsigned long OTA_BACKOFF_MS = 10UL * 60UL * 1000UL;
const int OTA_MAX_FAIL = 3;
unsigned long otaBackoffUntil = 0;
volatile bool otaInProgress = false;
int otaFailCount = 0;
String otaLastFailedVersion = "";
String otaLastSkippedVersion = "";
volatile unsigned long lastServerOkMs = 0;
WebServer server(80);
Preferences prefs;
bool bootHealthConfirmed = false;
volatile bool bootWifiOk = false;
volatile bool bootCheckinOk = false;
volatile bool bootSensorOk = false;
volatile bool otaPendingConfirmation = false;
unsigned long lastCheckinTry = 0;
unsigned long bootStartMs = 0;
SemaphoreHandle_t netMutex;
volatile bool alertPending = false;
int alertDistanceShared = -1;
char alertLevelShared[8] = "";

WiFiClientSecure tlsClient;
PubSubClient mqttClient(tlsClient);
const char* MQTT_TOPIC_OTA_STATE = "waterlevel/HCSR04/ota";
const char* MQTT_TOPIC_DEVICE_LOG = "waterlevel/HCSR04/log";
const char* MQTT_TOPIC_INFO = "waterlevel/HCSR04/info";
char pendingOtaStage[12] = "";
unsigned long lastOtaPublishMs = 0;
unsigned long lastMqttRetry = 0;
bool wasMqttConnected = false;

void lcdPrintPadded(uint8_t col, uint8_t row, const char* text) {
  lcd.setCursor(col, row);
  char buf[17];
  snprintf(buf, sizeof(buf), "%-16s", text);
  lcd.print(buf);
}

static inline float r1(float v) { return roundf(v * 10.0f) / 10.0f; }
static inline float r2(float v) { return roundf(v * 100.0f) / 100.0f; }

void triggerAlert(int distance, const char* level) {
  if (xSemaphoreTake(netMutex, pdMS_TO_TICKS(200)) != pdTRUE) return;
  alertPending = true;
  alertDistanceShared = distance;
  strncpy(alertLevelShared, level, sizeof(alertLevelShared) - 1);
  alertLevelShared[sizeof(alertLevelShared) - 1] = '\0';
  xSemaphoreGive(netMutex);
}

const unsigned long READ_INTERVAL = 1000;
unsigned long lastRead = 0;

struct WifiCfg { String ssid, pass, ip, gw; };

WifiCfg loadWifiCfg() {
  WifiCfg c;
  prefs.begin("wifi", true);
  c.ssid = prefs.getString("ssid", WIFI_SSID);
  c.pass = prefs.getString("pass", WIFI_PASSWORD);
  c.ssid.trim();
  c.ip = prefs.getString("ip", "");
  c.gw = prefs.getString("gw", "");
  prefs.end();
  return c;
}

float getSavedSensorHeight() {
  prefs.begin("sensorcfg", true);
  float h = prefs.getFloat("height", DEFAULT_SENSOR_HEIGHT_CM);
  prefs.end();
  if (h <= 0 || h > MAX_DISTANCE) h = DEFAULT_SENSOR_HEIGHT_CM;
  return h;
}

void saveSensorHeight(float heightCm) {
  prefs.begin("sensorcfg", false);
  prefs.putFloat("height", heightCm);
  prefs.end();
}

float computeWaterLevel(int obstacleDistance) {
  if (obstacleDistance < 0) return -1;
  float level = SENSOR_HEIGHT_CM - (float)obstacleDistance;
  if (level < 0) level = 0;
  if (level > SENSOR_HEIGHT_CM) level = SENSOR_HEIGHT_CM;
  return level;
}

bool applyStaticIpIfSet(const WifiCfg& cfg) {
  const String& ipStr = cfg.ip;
  const String& gwStr = cfg.gw;
  if (ipStr.length() == 0) return false;
  IPAddress ip, gw, subnet(255, 255, 255, 0);
  if (!ip.fromString(ipStr)) return false;
  if (gwStr.length() == 0 || !gw.fromString(gwStr)) gw = ip;
  return WiFi.config(ip, gw, subnet);
}

const int FILTER_SIZE_STABLE = 5;
int filterBufStable[FILTER_SIZE_STABLE];
int filterIndexStable = 0;
bool filterFilledStable = false;

int pushAndSmoothStable(int rawValue) {
  filterBufStable[filterIndexStable] = rawValue;
  filterIndexStable = (filterIndexStable + 1) % FILTER_SIZE_STABLE;
  if (filterIndexStable == 0) filterFilledStable = true;
  int count = filterFilledStable ? FILTER_SIZE_STABLE : filterIndexStable;
  long sum = 0;
  for (int i = 0; i < count; i++) sum += filterBufStable[i];
  return (int)(sum / count);
}

int lastRateDistance = -1;
unsigned long lastRateCheckTime = 0;
const unsigned long RATE_WINDOW = 30000;
float currentRateCmPerMin = 0;

void updateRate(int distance) {
  unsigned long now = millis();
  if (lastRateDistance < 0) {
    lastRateDistance = distance;
    lastRateCheckTime = now;
    return;
  }
  if (now - lastRateCheckTime >= RATE_WINDOW) {
    float deltaDistance = distance - lastRateDistance;
    float deltaMinutes = (now - lastRateCheckTime) / 60000.0;
    currentRateCmPerMin = deltaDistance / deltaMinutes;
    lastRateDistance = distance;
    lastRateCheckTime = now;
  }
}

float estimateMinutesToDanger(int currentDistance) {
  if (currentRateCmPerMin >= -0.5) return -1;
  if (currentDistance <= DANGER_THRESHOLD && currentDistance > 0) return 0;
  float remainingDistance = currentDistance - DANGER_THRESHOLD;
  return remainingDistance / (-currentRateCmPerMin);
}

int currentDistance = -1;
int currentFilteredDistance = -1;
float currentEta = -1;
unsigned long lastUpdateMillis = 0;
const int HISTORY_SIZE = 40;
struct Ring {
  int buf[HISTORY_SIZE];
  int count = 0;
  int head = 0;
  void push(int v) {
    buf[head] = v;
    head = (head + 1) % HISTORY_SIZE;
    if (count < HISTORY_SIZE) count++;
  }
};
Ring rawHist, fltHist;

bool isBuzzerOn = false;
const unsigned int BUZZER_FREQ_HZ = 1000;
volatile bool buzzerWanted = false;
unsigned long buzzerPhaseStart = 0;
// Kieu canh bao khi muc nuoc NGUY HIEM:
//   1 = nhip gon 0.5s KEU / 0.5s NGHI (lap lien tuc)
//   2 = coi hu: tan so quet len-xuong lien tuc (can coi thu dong/passive), co khoang nghi ngan de do
#define ALARM_MODE 1
#if ALARM_MODE == 2
const unsigned long ALARM_SOUND_MS = 700;   // thoi gian quet len + xuong
const unsigned long ALARM_CYCLE_MS = 950;   // 700ms hu + 250ms nghi
const unsigned int SIREN_F_LOW = 800;
const unsigned int SIREN_F_HIGH = 1600;
const unsigned long SIREN_STEP_MS = 10;
unsigned long lastSirenStep = 0;
#else
const unsigned long ALARM_SOUND_MS = 500;   // 0.5s kem
const unsigned long ALARM_CYCLE_MS = 1000;  // 0.5s kem + 0.5s nghi
#endif
const unsigned long MEASURE_SETTLE_MS = 25;
const unsigned long MEASURE_LATEST_MS = 150;

void buzzerAlert(unsigned int freq) {
  if (!isBuzzerOn) {
    tone(BUZZER_PIN, freq);
    isBuzzerOn = true;
  }
}

void buzzerStop() {
  if (isBuzzerOn) {
    noTone(BUZZER_PIN);
    digitalWrite(BUZZER_PIN, LOW);
    isBuzzerOn = false;
  }
}

void buzzerSetWanted(bool wanted) {
  if (wanted && !buzzerWanted) buzzerPhaseStart = millis();
  buzzerWanted = wanted;
  if (!wanted) buzzerStop();
}

void buzzerTick() {
  if (!buzzerWanted) {
    buzzerStop();
    return;
  }
  unsigned long phase = (millis() - buzzerPhaseStart) % ALARM_CYCLE_MS;
  if (phase >= ALARM_SOUND_MS) {
    buzzerStop();
    return;
  }
#if ALARM_MODE == 2
  unsigned long now = millis();
  if (isBuzzerOn && now - lastSirenStep < SIREN_STEP_MS) return;
  lastSirenStep = now;
  unsigned long half = ALARM_SOUND_MS / 2;
  unsigned long span = SIREN_F_HIGH - SIREN_F_LOW;
  unsigned int f = (phase < half) ? SIREN_F_LOW + span * phase / half
                                  : SIREN_F_HIGH - span * (phase - half) / half;
  tone(BUZZER_PIN, f);
  isBuzzerOn = true;
#else
  buzzerAlert(BUZZER_FREQ_HZ);
#endif
}

bool readAllowedNow() {
  if (!buzzerWanted) return true;
  if (millis() - lastRead >= READ_INTERVAL + 1500UL) return true;
  unsigned long phase = (millis() - buzzerPhaseStart) % ALARM_CYCLE_MS;
  if (phase < ALARM_SOUND_MS) return false;
  unsigned long off = phase - ALARM_SOUND_MS;
  return off >= MEASURE_SETTLE_MS && off <= MEASURE_LATEST_MS;
}

int measureDistanceRaw() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);
  long duration = pulseIn(ECHO_PIN, HIGH, 30000);
  if (duration == 0) return -1;
  return duration * 0.034 / 2;
}

const int PING_SAMPLES = 3;
int measureDistanceMedianCore() {
  int vals[PING_SAMPLES];
  int n = 0;
  for (int i = 0; i < PING_SAMPLES; i++) {
    int d = measureDistanceRaw();
    if (d >= 2) vals[n++] = d;
    if (i < PING_SAMPLES - 1) delay(30);
  }
  if (n < 2) return -1;
  for (int i = 1; i < n; i++) {
    int key = vals[i];
    int j = i - 1;
    while (j >= 0 && vals[j] > key) {
      vals[j + 1] = vals[j];
      j--;
    }
    vals[j + 1] = key;
  }
  return vals[n / 2];
}

int measureDistanceMedian() {
  return measureDistanceMedianCore();
}

const char* levelFromDistance(int distance) {
  if (distance < 0) return "error";
  if (distance <= DANGER_THRESHOLD) return "danger";
  if (distance <= WARN_THRESHOLD) return "warn";
  if (distance <= DETECT_THRESHOLD) return "detect";
  return "safe";
}

struct OtaOffer {
  bool valid = false;
  bool active = false;
  String version;
  String url;
  size_t size = 0;
};

void fetchThresholdsFromServer(OtaOffer* offer = nullptr) {
  if (WiFi.status() != WL_CONNECTED) return;
  HTTPClient http;
  http.setTimeout(5000);
  String url = String(SETTINGS_WORKER_URL) + "?deviceId=" + String(DEVICE_ID);
  http.begin(url);
  int httpCode = http.GET();
  if (httpCode == 200) {
    lastServerOkMs = millis();
    String payload = http.getString();
    StaticJsonDocument<768> doc;
    DeserializationError err = deserializeJson(doc, payload);
    if (!err && !doc["danger"].isNull() && !doc["warn"].isNull() && !doc["detect"].isNull()) {
      float d = doc["danger"];
      float w = doc["warn"];
      float dt = doc["detect"];
      if (d > 0 && w > d && dt > w) {
        if (d != DANGER_THRESHOLD || w != WARN_THRESHOLD || dt != DETECT_THRESHOLD) {
          DANGER_THRESHOLD = d;
          WARN_THRESHOLD = w;
          DETECT_THRESHOLD = dt;
          Serial.printf("[CFG] nguong %.0f/%.0f/%.0f\n", d, w, dt);
        }
      } else {
        Serial.println("[CFG] nguong khong hop le");
      }
    }
    if (!doc["sensorHeight"].isNull()) {
      float h = doc["sensorHeight"];
      if (h > 0 && h <= MAX_DISTANCE && h != SENSOR_HEIGHT_CM) {
        SENSOR_HEIGHT_CM = h;
        saveSensorHeight(h);
        Serial.printf("[CFG] do cao %.1f cm\n", h);
      }
    }
    if (offer && !err && doc["ota"].is<JsonObject>()) {
      JsonObject o = doc["ota"];
      offer->valid = true;
      offer->active = o["active"] | false;
      offer->version = o["version"] | "";
      offer->url = o["url"] | "";
      offer->size = o["size"] | 0;
    }
  } else if (httpCode > 0) {
    Serial.printf("[CFG] loi HTTP %d\n", httpCode);
  } else {
    Serial.printf("[CFG] loi %s\n", http.errorToString(httpCode).c_str());
  }
  http.end();
}

void flushMqttLogsLocked(int maxLines) {
  if (!mqttClient.connected()) return;
  char line[LOG_LINE_MAX];
  for (int i = 0; i < maxLines; i++) {
    if (!mirrorSerial.popLine(line, sizeof(line))) break;
    mqttClient.publish(MQTT_TOPIC_DEVICE_LOG, line, false);
  }
}

void otaPublish(const char* stage, int percent, size_t written, size_t total, const char* version, const char* msg = "") {
  if (netMutex == NULL) return;
  if (xSemaphoreTake(netMutex, pdMS_TO_TICKS(300)) != pdTRUE) return;
  if (mqttClient.connected()) {
    StaticJsonDocument<320> doc;
    doc["stage"] = stage;
    doc["percent"] = percent;
    doc["written"] = (unsigned long)written;
    doc["total"] = (unsigned long)total;
    doc["version"] = version;
    doc["from"] = FIRMWARE_VERSION;
    if (msg != nullptr && msg[0] != '\0') doc["msg"] = msg;
    char buf[352];
    size_t len = serializeJson(doc, buf);
    mqttClient.publish(MQTT_TOPIC_OTA_STATE, (uint8_t*)buf, len, false);
    flushMqttLogsLocked(6);
    mqttClient.loop();
  }
  xSemaphoreGive(netMutex);
}

void otaServiceMqtt() {
  if (netMutex == NULL) return;
  if (xSemaphoreTake(netMutex, pdMS_TO_TICKS(100)) != pdTRUE) return;
  flushMqttLogsLocked(4);
  mqttClient.loop();
  xSemaphoreGive(netMutex);
}

enum OtaErrorCode {
  OTA_ERR_NONE = 0,
  OTA_ERR_WIFI,
  OTA_ERR_HTTP,
  OTA_ERR_SIZE_MISMATCH,
  OTA_ERR_NO_SPACE,
  OTA_ERR_TIMEOUT,
  OTA_ERR_WRITE_INCOMPLETE,
  OTA_ERR_FINALIZE,
};

struct OtaResult {
  bool ok = false;
  OtaErrorCode errorCode = OTA_ERR_NONE;
  int httpCode = 0;
  size_t written = 0;
  size_t total = 0;
  String errorDetail;
};

bool reportOtaCheckin(bool ok, const char* version, const char* errorDetail = "") {
  if (WiFi.status() != WL_CONNECTED) return false;
  HTTPClient http;
  http.setTimeout(4000);
  http.begin(OTA_CHECKIN_URL);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-Device-Key", DEVICE_KEY_VALUE);
  StaticJsonDocument<384> doc;
  doc["deviceId"] = DEVICE_ID;
  doc["version"] = version;
  doc["ok"] = ok;
  if (errorDetail != nullptr && errorDetail[0] != '\0') {
    doc["error"] = errorDetail;
  }
  String payload;
  serializeJson(doc, payload);
  int httpCode = http.POST(payload);
  http.end();
  return httpCode > 0 && httpCode < 400;
}

OtaResult performOta(const String& url, size_t expectedSize, const String& version) {
  OtaResult res;
  res.total = expectedSize;

  if (WiFi.status() != WL_CONNECTED) {
    res.errorCode = OTA_ERR_WIFI;
    res.errorDetail = "Mat ket noi WiFi";
    Serial.println("OTA: mat ket noi WiFi, huy.");
    return res;
  }

  HTTPClient http;
  http.setTimeout(15000);
  http.begin(url);
  int httpCode = http.GET();
  res.httpCode = httpCode;
  if (httpCode != 200) {
    res.errorCode = OTA_ERR_HTTP;
    res.errorDetail = "Loi tai file, HTTP " + String(httpCode);
    Serial.printf("OTA: khong tai duoc file, HTTP %d\n", httpCode);
    http.end();
    return res;
  }

  int contentLength = http.getSize();
  res.total = contentLength > 0 ? (size_t)contentLength : expectedSize;
  if (contentLength <= 0 || (expectedSize > 0 && (size_t)contentLength != expectedSize)) {
    res.errorCode = OTA_ERR_SIZE_MISMATCH;
    res.errorDetail = "Kich thuoc file khong khop";
    Serial.println("OTA: kich thuoc file khong khop, huy.");
    http.end();
    return res;
  }

  if (!Update.begin(contentLength)) {
    res.errorCode = OTA_ERR_NO_SPACE;
    res.errorDetail = "Khong du bo nho flash";
    Serial.printf("OTA: khong du bo nho flash cho update (%d bytes)\n", contentLength);
    http.end();
    return res;
  }

  lcdPrintPadded(0, 0, ("Cap nhat " + version).c_str());
  Serial.printf("OTA: bat dau tai %d bytes\n", contentLength);
  otaPublish("download", 0, 0, (size_t)contentLength, version.c_str());

  WiFiClient* stream = http.getStreamPtr();
  uint8_t buf[1024];
  size_t written = 0;
  unsigned long lastDataMs = millis();
  unsigned long lastLcdMs = 0;
  int lastShownPercent = -1;
  int lastLoggedTen = -1;
  unsigned long lastMqttMs = 0;

  while (http.connected() && written < (size_t)contentLength) {
    esp_task_wdt_reset();
    size_t avail = stream->available();
    if (avail > 0) {
      int n = stream->readBytes(buf, avail > sizeof(buf) ? sizeof(buf) : avail);
      if (n > 0) {
        Update.write(buf, n);
        written += n;
        lastDataMs = millis();

        int percent = (int)((written * 100UL) / (unsigned long)contentLength);
        unsigned long now = millis();
        if (percent != lastShownPercent || now - lastLcdMs >= 400) {
          lastShownPercent = percent;
          lastLcdMs = now;
          char line[17];
          snprintf(line, sizeof(line), "%3d%% %lu/%luK", percent,
                   (unsigned long)(written / 1024UL),
                   (unsigned long)((size_t)contentLength / 1024UL));
          lcdPrintPadded(0, 1, line);
        }
        if (now - lastMqttMs >= 300 || written >= (size_t)contentLength) {
          lastMqttMs = now;
          int ten = percent / 10;
          if (ten != lastLoggedTen) {
            lastLoggedTen = ten;
            Serial.printf("OTA: %d%% (%lu/%lu bytes)\n", percent, (unsigned long)written, (unsigned long)contentLength);
          }
          otaPublish("download", percent, written, (size_t)contentLength, version.c_str());
        }
      }
    } else {
      if (millis() - lastDataMs > 10000) {
        res.errorCode = OTA_ERR_TIMEOUT;
        res.errorDetail = "Qua thoi gian cho du lieu";
        Serial.println("OTA: qua thoi gian cho du lieu, huy.");
        break;
      }
      delay(2);
    }
  }
  http.end();
  res.written = written;

  if (res.errorCode != OTA_ERR_NONE) {
    Update.abort();
    return res;
  }

  if (written != (size_t)contentLength) {
    res.errorCode = OTA_ERR_WRITE_INCOMPLETE;
    res.errorDetail = "Chi ghi duoc " + String((unsigned long)written) + "/" +
                       String((unsigned long)contentLength) + " bytes";
    Serial.printf("OTA: chi ghi duoc %d/%d bytes, huy.\n", (int)written, contentLength);
    Update.abort();
    return res;
  }

  otaPublish("verify", 100, written, (size_t)contentLength, version.c_str(), "Dang kiem tra firmware");
  if (!Update.end(true)) {
    res.errorCode = OTA_ERR_FINALIZE;
    res.errorDetail = String("Loi finalize: ") + Update.errorString();
    Serial.printf("OTA: loi finalize - %s\n", Update.errorString());
    return res;
  }

  res.ok = true;
  Serial.println("OTA: flash thanh cong, khoi dong lai...");
  return res;
}

String getSeenActiveVersion() {
  Preferences p;
  p.begin("otaseen", true);
  String v = p.getString("ver", "");
  p.end();
  return v;
}

void setSeenActiveVersion(const String& v) {
  Preferences p;
  p.begin("otaseen", false);
  p.putString("ver", v);
  p.end();
}

int compareVersions(const String& a, const String& b) {
  int ai = 0, bi = 0;
  int alen = a.length(), blen = b.length();
  while (ai < alen || bi < blen) {
    int av = 0, bv = 0;
    while (ai < alen && a[ai] != '.') { av = av * 10 + (a[ai] - '0'); ai++; }
    while (bi < blen && b[bi] != '.') { bv = bv * 10 + (b[bi] - '0'); bi++; }
    if (av != bv) return (av > bv) ? 1 : -1;
    if (ai < alen && a[ai] == '.') ai++;
    if (bi < blen && b[bi] == '.') bi++;
  }
  return 0;
}

void checkForOta(const OtaOffer& offer) {
  if (otaInProgress || WiFi.status() != WL_CONNECTED) return;
  if (!offer.active) {
    otaLastSkippedVersion = "";
    if (getSeenActiveVersion() != "none") setSeenActiveVersion("none");
    return;
  }
  const String& remoteVersion = offer.version;
  const String& fwUrl = offer.url;
  size_t remoteSize = offer.size;
  if (remoteVersion.length() == 0 || fwUrl.length() == 0) return;
  int cmp = compareVersions(remoteVersion, FIRMWARE_VERSION);
  String seenVersion = getSeenActiveVersion();
  if (cmp == 0) {
    otaLastSkippedVersion = "";
    if (seenVersion != remoteVersion) setSeenActiveVersion(remoteVersion);
    return;
  }
  bool isRollback = false;
  if (cmp < 0) {
    if (seenVersion.length() == 0) {
      setSeenActiveVersion(remoteVersion);
      seenVersion = remoteVersion;
    }
    if (seenVersion == remoteVersion) {
      if (otaLastSkippedVersion != remoteVersion) {
        otaLastSkippedVersion = remoteVersion;
        Serial.printf("OTA: bo qua vi ban server %s thap hon ban dang chay %s\n",
                      remoteVersion.c_str(), FIRMWARE_VERSION);
      }
      return;
    }
    isRollback = true;
  }
  otaLastSkippedVersion = "";
  if (otaLastFailedVersion == remoteVersion && otaFailCount >= OTA_MAX_FAIL) {
    return;
  }
  if (isRollback) {
    Serial.printf("OTA: nhan yeu cau rollback tu web ve ban %s (dang chay %s), bat dau tai...\n",
                  remoteVersion.c_str(), FIRMWARE_VERSION);
  } else {
    Serial.printf("OTA: phat hien ban mới %s (dang chay %s), bat dau tai...\n",
                  remoteVersion.c_str(), FIRMWARE_VERSION);
  }
  otaInProgress = true;
  lcdPrintPadded(0, 0, ("Cap nhat " + remoteVersion).c_str());
  lcdPrintPadded(0, 1, "Dang ket noi... ");
  buzzerSetWanted(false);
  otaPublish("start", 0, 0, remoteSize, remoteVersion.c_str(), "Dang ket noi de tai firmware");
  OtaResult result = performOta(fwUrl, remoteSize, remoteVersion);
  if (result.ok) {
    lcdPrintPadded(0, 0, "Tai xong 100%   ");
    lcdPrintPadded(0, 1, "Dang khoi dong..");
    prefs.begin("otaboot", false);
    prefs.putBool("pending", true);
    prefs.putString("pendingVer", remoteVersion);
    prefs.putUInt("bootFails", 0);
    prefs.end();
    otaPublish("reboot", 100, result.written, result.total, remoteVersion.c_str(), "Tai xong, dang khoi dong lai");
    for (int i = 0; i < 5; i++) { otaServiceMqtt(); delay(60); }
    delay(300);
    ESP.restart();
  } else {
    otaInProgress = false;
    otaPublish("failed", 0, result.written, result.total, remoteVersion.c_str(), result.errorDetail.c_str());
    reportOtaCheckin(false, remoteVersion.c_str(), result.errorDetail.c_str());
    if (otaLastFailedVersion == remoteVersion) {
      otaFailCount++;
    } else {
      otaLastFailedVersion = remoteVersion;
      otaFailCount = 1;
    }
    Serial.printf("OTA: that bai - %s (da tai %lu/%lu bytes)\n",
                  result.errorDetail.c_str(), (unsigned long)result.written,
                  (unsigned long)result.total);

    lcdPrintPadded(0, 0, "Loi cap nhat!   ");
    lcdPrintPadded(0, 1, result.errorDetail.c_str());
    delay(2500);
    if (otaFailCount >= OTA_MAX_FAIL) {
      lcdPrintPadded(0, 0, "Da tam dung OTA ");
      lcdPrintPadded(0, 1, "Thu lai sau 10ph");
      Serial.printf("OTA: that bai %d lan voi ban %s, tam dung ~10 phut.\n",
                    otaFailCount, remoteVersion.c_str());
      otaBackoffUntil = millis() + OTA_BACKOFF_MS;
    } else {
      lcdPrintPadded(0, 0, "Cap nhat that bai");
      lcdPrintPadded(0, 1, "Van chay ban cu ");
    }
    delay(2000);
  }
}

void sendTelegramAlert(int distance, const char* level) {
  static unsigned long lastFailMs = 0;
  if (lastSentLevel == level) return;
  if (WiFi.status() != WL_CONNECTED) return;
  if (lastFailMs != 0 && millis() - lastFailMs < 15000UL) return;
  HTTPClient http;
  http.setTimeout(4000);
  http.begin(ALERT_WORKER_URL);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-Device-Key", DEVICE_KEY_VALUE);
  StaticJsonDocument<256> doc;
  doc["deviceId"] = DEVICE_ID;
  doc["distance"] = distance;
  doc["level"] = level;
  String payload;
  serializeJson(doc, payload);
  int httpCode = http.POST(payload);
  if (httpCode > 0 && httpCode < 400) {
    Serial.printf("[TG] %s %dcm OK\n", level, distance);
    lastSentLevel = level;
    lastFailMs = 0;
  } else {
    if (httpCode > 0) Serial.printf("[TG] %s %dcm loi %d\n", level, distance, httpCode);
    else Serial.printf("[TG] %s %dcm loi: %s\n", level, distance, http.errorToString(httpCode).c_str());
    lastFailMs = millis();
  }
  http.end();
}

struct PendingLog {
  unsigned long capturedAtMs;
  int distance;
  float rate;
  char level[8];
};
const int PENDING_QUEUE_SIZE = 40;
PendingLog pendingQueue[PENDING_QUEUE_SIZE];
int pendingCount = 0;

void queuePendingLog(int distance, float rate, const char* level) {
  if (pendingCount >= PENDING_QUEUE_SIZE) {
    for (int i = 1; i < PENDING_QUEUE_SIZE; i++) pendingQueue[i - 1] = pendingQueue[i];
    pendingCount--;
    Serial.println("[D1] hang doi day, bo ban cu");
  }
  PendingLog& p = pendingQueue[pendingCount];
  p.capturedAtMs = millis();
  p.distance = distance;
  p.rate = rate;
  strncpy(p.level, level, sizeof(p.level) - 1);
  p.level[sizeof(p.level) - 1] = '\0';
  pendingCount++;
}

bool sendLogPayload(int distance, float rate, const char* level) {
  if (WiFi.status() != WL_CONNECTED) return false;
  HTTPClient http;
  http.setTimeout(4000);
  http.begin(LOG_WORKER_URL);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-Device-Key", DEVICE_KEY_VALUE);
  StaticJsonDocument<256> doc;
  doc["deviceId"] = DEVICE_ID;
  doc["distance"] = distance;
  doc["rate"] = r2(rate);
  doc["level"] = level;
  String payload;
  serializeJson(doc, payload);
  int httpCode = http.POST(payload);
  bool ok = httpCode > 0 && httpCode < 400;
  String distStr = distance >= 0 ? String(distance) + "cm" : String("n/a");
  if (ok) {
    lastServerOkMs = millis();
    Serial.printf("[D1] %s %s OK\n", distStr.c_str(), level);
  } else if (httpCode > 0) {
    Serial.printf("[D1] %s %s loi %d\n", distStr.c_str(), level, httpCode);
  } else {
    Serial.printf("[D1] %s %s loi: %s\n", distStr.c_str(), level, http.errorToString(httpCode).c_str());
  }
  http.end();
  return ok;
}

const int MAX_LOGS_PER_FLUSH = 8;
void flushPendingLogs() {
  if (WiFi.status() != WL_CONNECTED) return;
  xSemaphoreTake(netMutex, portMAX_DELAY);
  bool hasWork = pendingCount > 0;
  xSemaphoreGive(netMutex);
  if (!hasWork) return;
  int processed = 0;
  while (processed < MAX_LOGS_PER_FLUSH) {
    esp_task_wdt_reset();
    xSemaphoreTake(netMutex, portMAX_DELAY);
    if (pendingCount == 0) { xSemaphoreGive(netMutex); break; }
    PendingLog p = pendingQueue[0];
    xSemaphoreGive(netMutex);
    if (!sendLogPayload(p.distance, p.rate, p.level)) break;
    xSemaphoreTake(netMutex, portMAX_DELAY);
    for (int i = 1; i < pendingCount; i++) pendingQueue[i - 1] = pendingQueue[i];
    pendingCount--;
    xSemaphoreGive(netMutex);
    processed++;
  }
}

void sendLogToD1(int distance, const char* level) {
  unsigned long now = millis();
  bool changedEnough = abs(distance - lastLoggedDistance) >= LOG_CHANGE_THRESHOLD;
  bool levelChanged = lastLoggedLevel != level;
  bool isDetecting = strcmp(level, "safe") != 0;
  bool isDeepDetect = isDetecting && (float)distance <= (DETECT_THRESHOLD - DEEP_DETECT_MARGIN_CM);
  bool shouldSend = false;
  if (changedEnough || levelChanged) {
    lastChangeTime = now;
    shouldSend = true;
  } else {
    bool activelyChanging = (now - lastChangeTime) < ACTIVE_CHANGE_WINDOW_MS;
    if (!activelyChanging) {
      unsigned long heartbeatInterval;
      if (!isDetecting) {
        heartbeatInterval = HEARTBEAT_SAFE_MS;
      } else if (isDeepDetect) {
        heartbeatInterval = HEARTBEAT_DEEP_DETECT_MS;
      } else {
        heartbeatInterval = HEARTBEAT_LIGHT_DETECT_MS;
      }
      if (now - lastLogTime >= heartbeatInterval) shouldSend = true;
    }
  }
  if (!shouldSend) return;
  if (xSemaphoreTake(netMutex, pdMS_TO_TICKS(200)) != pdTRUE) return;
  lastLoggedDistance = distance;
  lastLoggedLevel = level;
  lastLogTime = now;
  queuePendingLog(distance, currentRateCmPerMin, level);
  xSemaphoreGive(netMutex);
}

void publishMqttTelemetry(int distance, int stableDist, float rate, const char* level) {
  if (xSemaphoreTake(netMutex, pdMS_TO_TICKS(200)) != pdTRUE) return;
  if (mqttClient.connected()) {
    StaticJsonDocument<384> doc;
    doc["distance"] = distance;
    doc["stableDistance"] = stableDist;
    doc["rate"] = r2(rate);
    doc["level"] = level;
    if (currentEta >= 0) doc["eta"] = r1(currentEta);
    doc["sensorHeight"] = r1(SENSOR_HEIGHT_CM);
    doc["fw"] = FIRMWARE_VERSION;
    if (currentWaterLevel >= 0) doc["waterLevel"] = r1(currentWaterLevel);
    doc["timestamp"] = millis();
    char buf[512];
    size_t len = serializeJson(doc, buf);
    bool pubOk = mqttClient.publish(MQTT_TOPIC_PUB, (uint8_t*)buf, len, false);
    if (!pubOk) {
      Serial.printf("[MQTT] publish loi rc=%d\n", mqttClient.state());
    }
  }
  xSemaphoreGive(netMutex);
}

const char* mqttStateText(int rc) {
  switch (rc) {
    case -4: return "no CONNACK in time";
    case -3: return "connection lost";
    case -2: return "TCP/TLS connect failed";
    case -1: return "disconnected";
    case 1: return "bad protocol";
    case 2: return "bad client id";
    case 3: return "broker unavailable";
    case 4: return "bad user/pass";
    case 5: return "not authorized";
    default: return "other";
  }
}

void handleMqttLoop() {
  if (WiFi.status() != WL_CONNECTED) return;
  unsigned long now = millis();

  xSemaphoreTake(netMutex, portMAX_DELAY);
  bool connected = mqttClient.connected();
  bool shouldRetry = false;
  if (!connected) {
    if (wasMqttConnected) {
      Serial.printf("[MQTT] mat ket noi rc=%d\n", mqttClient.state());
      wasMqttConnected = false;
    }
    if (now - lastMqttRetry >= 5000) {
      lastMqttRetry = now;
      shouldRetry = true;
    }
  }
  xSemaphoreGive(netMutex);

  if (!connected) {
    if (shouldRetry) {
      xSemaphoreTake(netMutex, portMAX_DELAY);
      tlsClient.stop();
      tlsClient.setInsecure();
      tlsClient.setTimeout(3000);
      mqttClient.setSocketTimeout(8);

      Serial.println("[MQTT] connecting");
      bool connOk = mqttClient.connect(DEVICE_ID, MQTT_USER, MQTT_PASS, MQTT_TOPIC_LWT, 1, true, "offline");
      if (connOk) {
        Serial.println("[MQTT] connected");
        mqttClient.publish(MQTT_TOPIC_LWT, "online", true);
        StaticJsonDocument<128> idoc;
        idoc["version"] = FIRMWARE_VERSION;
        idoc["device"] = DEVICE_ID;
        idoc["uptime"] = millis() / 1000;
        char ibuf[160];
        size_t ilen = serializeJson(idoc, ibuf);
        mqttClient.publish(MQTT_TOPIC_INFO, (uint8_t*)ibuf, ilen, true);
        wasMqttConnected = true;
      } else {
        Serial.printf("[MQTT] failed rc=%d (%s)\n", mqttClient.state(), mqttStateText(mqttClient.state()));
        tlsClient.stop();
      }
      xSemaphoreGive(netMutex);
    }
  } else {
    xSemaphoreTake(netMutex, portMAX_DELAY);
    if (pendingOtaStage[0] != '\0') {
      StaticJsonDocument<192> pdoc;
      pdoc["stage"] = pendingOtaStage;
      pdoc["percent"] = 100;
      pdoc["version"] = FIRMWARE_VERSION;
      char pbuf[224];
      size_t plen = serializeJson(pdoc, pbuf);
      if (mqttClient.publish(MQTT_TOPIC_OTA_STATE, (uint8_t*)pbuf, plen, false)) pendingOtaStage[0] = '\0';
    }
    flushMqttLogsLocked(6);
    mqttClient.loop();
    xSemaphoreGive(netMutex);
  }
}

struct LevelStyle { const char* name; const char* text; bool red, yellow, green; };
static const LevelStyle LEVEL_STYLES[] = {
  {"danger", "NGUY HIEM!",     true,  false, false},
  {"warn",   "Nuoc dang cao",  false, true,  false},
  {"detect", "Co nuoc",        false, true,  false},
  {"safe",   "Kho rao",        false, false, true},
};

void updateOutputs(int distance, float etaMinutes) {
  float waterLevel = computeWaterLevel(distance);
  float maxRange = SENSOR_HEIGHT_CM - DANGER_THRESHOLD;
  float percent = 0.0f;
  if (maxRange > 0.0f && waterLevel >= 0.0f) {
    percent = constrain(waterLevel / maxRange * 100.0f, 0.0f, 100.0f);
  }
  int wInt = (int)round(waterLevel);
  if (wInt < 0) wInt = 0;
  char line0[17];
  snprintf(line0, sizeof(line0), "Nuoc: %dcm%s%d%%", wInt, wInt >= 100 ? " " : "  ", (int)round(percent));
  lcdPrintPadded(0, 0, line0);

  const char* level = levelFromDistance(distance);
  for (const LevelStyle& st : LEVEL_STYLES) {
    if (strcmp(level, st.name) != 0) continue;
    lcdPrintPadded(0, 1, st.text);
    digitalWrite(LED_RED, st.red);
    digitalWrite(LED_YELLOW, st.yellow);
    digitalWrite(LED_GREEN, st.green);
    buzzerSetWanted(st.red);
    triggerAlert(distance, st.name);
    return;
  }
}

void showSensorError() {
  lcdPrintPadded(0, 0, "Loi cam bien!   ");
  lcdPrintPadded(0, 1, "Kiem tra day noi");
  digitalWrite(LED_RED, HIGH);
  digitalWrite(LED_YELLOW, HIGH);
  digitalWrite(LED_GREEN, HIGH);
  buzzerSetWanted(false);
  triggerAlert(-1, "error");
}

const char INDEX_HTML[] PROGMEM = R"HTMLPAGE(<!DOCTYPE html><html lang="vi"><head><meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Giám Sát Mực Nước</title>
<style>
:root{--m:#94a3b8;--bd:#ffffff14;--c:#131c2e;--i:#0b1322}
*{box-sizing:border-box;margin:0;padding:0}
body{font:15px system-ui,sans-serif;background:#090e17;color:#f1f5f9;display:flex;justify-content:center;padding:16px}
main{width:100%;max-width:520px;background:var(--c);border:1px solid var(--bd);border-radius:16px;padding:16px;display:grid;gap:12px}
header{display:flex;justify-content:space-between;align-items:center;font-weight:700}
small{color:var(--m);font-size:.75rem}
.b{background:var(--i);border:1px solid var(--bd);border-radius:12px;padding:12px;text-align:center}
#v{font-size:3rem;font-weight:800}
#g{height:8px;background:#1e293b;border-radius:9px;overflow:hidden;margin-top:8px}
#gf{height:100%;width:0}
.r{display:grid;grid-template-columns:1fr 1fr;gap:10px}
#msg{padding:10px;border-radius:10px;font-weight:600;text-align:center}
canvas{width:100%;height:140px;background:var(--i);border:1px solid var(--bd);border-radius:12px}
button,input{width:100%;padding:10px;border-radius:8px;border:1px solid var(--bd);background:var(--i);color:inherit;font:inherit}
button{cursor:pointer;font-weight:700}
#sv{background:#22c55e;color:#000;border:0}
#wb{width:auto;background:none;border:0;font-size:1.2rem;padding:0}
dialog{margin:auto;width:calc(100% - 32px);max-width:400px;background:var(--c);color:inherit;border:1px solid var(--bd);border-radius:16px;padding:16px}
dialog::backdrop{background:#000b}
dialog>*{display:block;margin-bottom:10px}
</style></head><body>
<main>
<header><span>Giám Sát Mực Nước</span><span><small id="cn">Đang kết nối...</small> <button id="wb" title="Đổi WiFi">📶</button></span></header>
<div class="b"><small>MỰC NƯỚC DÂNG</small><div><span id="v">--</span> cm</div><small id="sd">Cách cảm biến -- cm</small><div id="g"><div id="gf"></div></div></div>
<div class="r"><div class="b"><small>Xu hướng</small><div id="td">--</div></div><div class="b"><small>Tốc độ</small><div id="tr">-- cm/p</div></div></div>
<div id="msg">Đang chờ dữ liệu...</div>
<canvas id="ch"></canvas>
<small style="text-align:right">Cập nhật: <span id="ts">--:--:--</span></small>
</main>
<dialog id="w">
<b>Cấu Hình WiFi</b>
<small id="ws"></small>
<input id="k" type="password" placeholder="Device Key (bắt buộc)">
<input id="s" placeholder="Tên WiFi (SSID)">
<input id="p" type="password" placeholder="Mật khẩu WiFi">
<input id="ip" placeholder="IP tĩnh (không bắt buộc)">
<input id="gw" placeholder="Gateway (không bắt buộc)">
<button id="sv">Lưu &amp; Kết Nối Lại</button>
<small id="wm"></small>
<button onclick="w.close()">Đóng</button>
</dialog>
<script>
const $=i=>document.getElementById(i),
L={error:['#94a3b8','Cảm biến lỗi'],danger:['#ef4444','NGUY HIỂM'],warn:['#eab308','NƯỚC DÂNG CAO'],detect:['#f97316','PHÁT HIỆN NƯỚC'],safe:['#22c55e','KHÔ RÁO']};
let H=200,raw=[],flt=[];
function line(a,col,r){
  const c=$('ch'),x=c.getContext('2d'),p=10*r,n=a.length;
  if(n<2)return;
  x.beginPath();
  a.forEach((d,i)=>{
    const l=d<0?0:Math.max(0,Math.min(1,(H-d)/H));
    const X=p+i*(c.width-2*p)/(n-1),Y=c.height-p-l*(c.height-2*p);
    i?x.lineTo(X,Y):x.moveTo(X,Y);
  });
  x.strokeStyle=col;x.lineWidth=2*r;x.stroke();
}
function draw(){
  const c=$('ch'),r=devicePixelRatio||1;
  c.width=c.clientWidth*r;c.height=c.clientHeight*r;
  line(flt,'#38bdf866',r);line(raw,'#38bdf8',r);
}
function add(a,v){a.push(v);if(a.length>40)a.shift()}
async function poll(){
  if(!document.hidden)try{
    const d=await(await fetch('/data',{signal:AbortSignal.timeout(3000)})).json();
    const [col,txt]=L[d.level]||L.error,rt=d.rate||0;
    if(d.sensorHeight)H=+d.sensorHeight;
    $('cn').textContent='● Online';$('cn').style.color='#22c55e';
    $('v').textContent=d.waterLevel==null?'--':Math.round(d.waterLevel);
    $('v').style.color=col;
    $('sd').textContent='Cách cảm biến '+(d.stableDistance>=0?d.stableDistance:'--')+' cm';
    $('msg').textContent=txt;
    $('msg').style.cssText='background:'+col+'22;color:'+col;
    $('gf').style.cssText='width:'+d.pct+'%;background:'+col;
    $('tr').textContent=Math.abs(rt).toFixed(2)+' cm/p';
    const t=rt<-.3?['Đang dâng ↑','#ef4444']:rt>.3?['Đang rút ↓','#22c55e']:['Ổn định •','#94a3b8'];
    $('td').textContent=t[0];$('td').style.color=t[1];
    add(raw,d.distance);add(flt,d.stableDistance);draw();
    $('ts').textContent=new Date().toLocaleTimeString('vi-VN');
  }catch(e){$('cn').textContent='✕ Mất kết nối';$('cn').style.color='#94a3b8'}
  setTimeout(poll,1000);
}
$('wb').onclick=async()=>{
  $('w').showModal();$('wm').textContent='';$('ws').textContent='Đang tải...';
  try{
    const d=await(await fetch('/wifi-status')).json();
    $('ws').textContent=d.connected?'Đã kết nối: '+d.ssid+' ('+d.ip+')':'Chưa kết nối';
  }catch(e){$('ws').textContent='Lỗi kết nối'}
};
$('sv').onclick=async()=>{
  const k=$('k').value.trim(),s=$('s').value.trim(),m=$('wm'),b=$('sv'),o={ssid:s,password:$('p').value};
  if(!k||!s){m.textContent='Cần nhập Device Key và tên mạng WiFi';return}
  if($('ip').value.trim())o.staticIp=$('ip').value.trim();
  if($('gw').value.trim())o.gateway=$('gw').value.trim();
  b.disabled=true;
  try{
    const r=await fetch('/wifi-config',{method:'POST',headers:{'Content-Type':'application/json','X-Device-Key':k},body:JSON.stringify(o)});
    m.textContent=r.ok?'Đã lưu! Thiết bị đang khởi động lại. Hãy tìm IP mới và tải lại trang.':'Cấu hình thất bại (sai Device Key?)';
    if(!r.ok)b.disabled=false;
  }catch(e){m.textContent='Lỗi kết nối';b.disabled=false}
};
poll();
</script></body></html>
)HTMLPAGE";

void handleRoot() {
  server.send_P(200, "text/html", INDEX_HTML);
}

void addCorsHeaders() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type, X-Device-Key");
  server.sendHeader("Access-Control-Allow-Private-Network", "true");
  server.sendHeader("Access-Control-Max-Age", "600");
}

void handleNotFound() {
  addCorsHeaders();
  if (server.method() == HTTP_OPTIONS) {
    server.send(204, "text/plain", "");
    return;
  }
  server.send(404, "text/plain", "Not found");
}

void handleData() {
  addCorsHeaders();
  StaticJsonDocument<384> doc;
  doc["distance"] = currentDistance;
  doc["stableDistance"] = currentFilteredDistance;
  doc["rate"] = r2(currentRateCmPerMin);
  if (currentEta >= 0) doc["eta"] = r1(currentEta);
  else doc["eta"] = nullptr;
  doc["sensorHeight"] = r1(SENSOR_HEIGHT_CM);
  if (currentWaterLevel >= 0) doc["waterLevel"] = r1(currentWaterLevel);
  else doc["waterLevel"] = nullptr;
  doc["pendingLogs"] = pendingCount;
  doc["timestamp"] = lastUpdateMillis;
  doc["level"] = levelFromDistance(currentFilteredDistance);
  float maxRange = SENSOR_HEIGHT_CM - DANGER_THRESHOLD;
  float pct = (maxRange > 0 && currentWaterLevel >= 0) ? currentWaterLevel / maxRange * 100.0f : 0;
  doc["pct"] = (int)(constrain(pct, 0.0f, 100.0f) + 0.5f);
  String json;
  serializeJson(doc, json);
  server.send(200, "application/json", json);
}

void sendHistory(const Ring& r) {
  addCorsHeaders();
  String json = "[";
  json.reserve(r.count * 5 + 2);
  for (int i = 0; i < r.count; i++) {
    if (i) json += ',';
    json += r.buf[(r.head - r.count + i + HISTORY_SIZE) % HISTORY_SIZE];
  }
  json += ']';
  server.send(200, "application/json", json);
}

void handleHistory() { sendHistory(rawHist); }
void handleHistoryFiltered() { sendHistory(fltHist); }

bool constantTimeEquals(const String& a, const String& b) {
  size_t lenA = a.length();
  size_t lenB = b.length();
  uint8_t diff = (lenA == lenB) ? 0 : 1;
  size_t maxLen = (lenA > lenB) ? lenA : lenB;
  for (size_t i = 0; i < maxLen; i++) {
    uint8_t ca = (i < lenA) ? (uint8_t)a[i] : 0;
    uint8_t cb = (i < lenB) ? (uint8_t)b[i] : 0;
    diff |= (ca ^ cb);
  }
  return diff == 0;
}

bool checkDeviceKey() {
  if (!server.hasHeader("X-Device-Key") || !constantTimeEquals(server.header("X-Device-Key"), DEVICE_KEY_VALUE)) {
    server.send(401, "application/json", "{\"error\":\"invalid device key\"}");
    return false;
  }
  return true;
}

void handleWifiStatus() {
  addCorsHeaders();
  StaticJsonDocument<256> doc;
  bool connected = WiFi.status() == WL_CONNECTED;
  doc["connected"] = connected;
  doc["ssid"] = loadWifiCfg().ssid;
  doc["ip"] = connected ? WiFi.localIP().toString() : String("");
  String json;
  serializeJson(doc, json);
  server.send(200, "application/json", json);
}

void handleWifiConfig() {
  addCorsHeaders();
  if (server.method() != HTTP_POST) { server.send(405, "text/plain", "Method not allowed"); return; }
  if (!checkDeviceKey()) return;
  StaticJsonDocument<256> doc;
  if (deserializeJson(doc, server.arg("plain"))) {
    server.send(400, "application/json", "{\"error\":\"bad json\"}");
    return;
  }
  String ssid = doc["ssid"] | "";
  String pass = doc["password"] | "";
  ssid.trim();
  String staticIp = doc["staticIp"] | "";
  String gateway = doc["gateway"] | "";
  if (ssid.length() == 0) {
    server.send(400, "application/json", "{\"error\":\"missing ssid\"}");
    return;
  }
  prefs.begin("wifi", false);
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  prefs.putString("ip", staticIp);
  prefs.putString("gw", gateway);
  prefs.end();
  server.send(200, "application/json", "{\"ok\":true,\"restarting\":true}");
  delay(500);
  ESP.restart();
}

void handleSensorConfigGet() {
  addCorsHeaders();
  StaticJsonDocument<128> doc;
  doc["sensorHeight"] = r1(SENSOR_HEIGHT_CM);
  String json;
  serializeJson(doc, json);
  server.send(200, "application/json", json);
}

void handleSensorConfigSet() {
  addCorsHeaders();
  if (server.method() != HTTP_POST) { server.send(405, "text/plain", "Method not allowed"); return; }
  if (!checkDeviceKey()) return;
  StaticJsonDocument<128> doc;
  if (deserializeJson(doc, server.arg("plain")) || doc["sensorHeight"].isNull()) {
    server.send(400, "application/json", "{\"error\":\"bad json\"}");
    return;
  }
  float h = doc["sensorHeight"].as<float>();
  if (!(h > 0 && h <= MAX_DISTANCE)) {
    server.send(400, "application/json", "{\"error\":\"gia tri khong hop le\"}");
    return;
  }
  SENSOR_HEIGHT_CM = h;
  saveSensorHeight(h);
  server.send(200, "application/json", "{\"ok\":true}");
}

void setupWatchdog() {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  esp_task_wdt_config_t twdtConfig = {
    .timeout_ms = WDT_TIMEOUT_SEC * 1000,
    .idle_core_mask = 0,
    .trigger_panic = true
  };
  esp_task_wdt_init(&twdtConfig);
#else
  esp_task_wdt_init(WDT_TIMEOUT_SEC, true);
#endif
  esp_task_wdt_add(NULL);
}

void rollbackToPreviousFirmware(const char* reason) {
  Serial.printf("ROLLBACK: %s\n", reason);
  lcd.clear();
  lcdPrintPadded(0, 0, "Dang rollback...");
  lcdPrintPadded(0, 1, reason);

  prefs.begin("otaboot", true);
  String pendingVer = prefs.getString("pendingVer", "");
  prefs.end();

  if (WiFi.status() == WL_CONNECTED && pendingVer.length() > 0) {
    reportOtaCheckin(false, pendingVer.c_str());
  }
  otaPublish("rollback", 0, 0, 0, pendingVer.c_str(), reason);

  prefs.begin("otaboot", false);
  prefs.putBool("pending", false);
  prefs.putUInt("bootFails", 0);
  prefs.end();

  const esp_partition_t* target = esp_ota_get_next_update_partition(NULL);
  esp_app_desc_t desc;
  bool valid = target != NULL && esp_ota_get_partition_description(target, &desc) == ESP_OK;
  if (valid) {
    esp_ota_set_boot_partition(target);
    Serial.printf("Da chuyen boot ve partition '%s' (ban %s).\n", target->label, desc.version);
  } else {
    Serial.println("Khong tim thay ban firmware cu hop le de rollback.");
  }
  delay(1000);
  ESP.restart();
}

void markBootHealthy() {
  if (bootHealthConfirmed) return;
  bootHealthConfirmed = true;
  if (otaPendingConfirmation) strncpy(pendingOtaStage, "done", sizeof(pendingOtaStage) - 1);
  esp_ota_mark_app_valid_cancel_rollback();
  prefs.begin("otaboot", false);
  prefs.putBool("pending", false);
  prefs.putUInt("bootFails", 0);
  prefs.end();
  Serial.println("Boot da duoc xac nhan on dinh.");
}

void checkBootConfirmation() {
  if (bootHealthConfirmed) return;
  unsigned long up = millis() - bootStartMs;
  if (otaPendingConfirmation) {
    if (bootWifiOk && bootCheckinOk && bootSensorOk) markBootHealthy();
    else if (up > BOOT_CONFIRM_WINDOW_MS) rollbackToPreviousFirmware("Khong xac nhan duoc ban moi sau 60s");
  } else if (bootSensorOk && up > BOOT_CONFIRM_WINDOW_MS) {
    markBootHealthy();
  }
}

void handleBootRollbackCheck() {
  prefs.begin("otaboot", false);
  uint32_t bootFails = prefs.getUInt("bootFails", 0) + 1;
  otaPendingConfirmation = prefs.getBool("pending", false);
  prefs.putUInt("bootFails", bootFails);
  prefs.end();
  Serial.printf("So lan khoi dong chua duoc xac nhan: %u\n", bootFails);
  if (bootFails > BOOT_FAIL_ROLLBACK_THRESHOLD) {
    rollbackToPreviousFirmware("Qua nhieu lan khoi dong khong on dinh");
  }
}

unsigned long lastHeapLog = 0;
const unsigned long HEAP_LOG_INTERVAL_MS = 60UL * 1000UL;

bool netWasUp = true;
bool backlogDrainPending = false;
unsigned long netDownSince = 0;
unsigned long lastOfflineReport = 0;
const unsigned long OFFLINE_REPORT_INTERVAL_MS = 10000UL;
const unsigned long SERVER_SILENCE_LIMIT_MS = 90000UL;

void printOfflineBacklog() {
  int queued = 0;
  char pendingLevel[8] = "";
  if (xSemaphoreTake(netMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
    queued = pendingCount;
    strncpy(pendingLevel, alertLevelShared, sizeof(pendingLevel) - 1);
    pendingLevel[sizeof(pendingLevel) - 1] = '\0';
    xSemaphoreGive(netMutex);
  }
  bool tgPending = pendingLevel[0] != '\0' && lastSentLevel != pendingLevel;
  Serial.printf("[NET] cho: D1 %d/%d, TG %s\n", queued, PENDING_QUEUE_SIZE, tgPending ? pendingLevel : "-");
}

volatile uint8_t lastWifiReason = 0;
volatile bool wifiReasonPending = false;

const char* wifiReasonText(uint8_t r) {
  switch (r) {
    case 1: return "unspecified";
    case 2: return "auth expired";
    case 3: return "AP deauth";
    case 4: return "assoc expired";
    case 5: return "AP full";
    case 8: return "left AP";
    case 14: return "MIC failure";
    case 15: return "wrong password (4-way handshake timeout)";
    case 16: return "group key timeout";
    case 200: return "beacon timeout, signal lost";
    case 201: return "SSID not found";
    case 202: return "auth failed, wrong password";
    case 203: return "assoc failed";
    case 204: return "handshake timeout, wrong password";
    case 205: return "connection failed";
    default: return "other";
  }
}

const char* wifiStatusText(wl_status_t st) {
  switch (st) {
    case WL_NO_SSID_AVAIL: return "SSID not found";
    case WL_CONNECT_FAILED: return "wrong password or auth failed";
    case WL_CONNECTION_LOST: return "connection lost";
    case WL_DISCONNECTED: return "disconnected or timeout";
    case WL_IDLE_STATUS: return "idle";
    default: return "unknown";
  }
}

bool waitWifiConnected(unsigned long timeoutMs) {
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < timeoutMs) {
    esp_task_wdt_reset();
    delay(300);
  }
  return WiFi.status() == WL_CONNECTED;
}

void logWifiScan(const String& target) {
  esp_task_wdt_reset();
  WiFi.disconnect(false, false);
  delay(500);
  int n = WiFi.scanNetworks(false, true);
  if (n < 0) {
    delay(500);
    esp_task_wdt_reset();
    n = WiFi.scanNetworks(false, true);
  }
  esp_task_wdt_reset();
  if (n < 0) {
    Serial.printf("[WIFI] scan error %d\n", n);
    return;
  }
  bool found = false;
  int similar = -1;
  for (int i = 0; i < n; i++) {
    String s = WiFi.SSID(i);
    if (s == target) found = true;
    else if (s.equalsIgnoreCase(target)) similar = i;
  }
  Serial.printf("[WIFI] scan: %d nets, '%s' %s\n", n, target.c_str(), found ? "visible" : "not seen");
  if (!found && similar >= 0) Serial.printf("[WIFI] similar SSID '%s', check case\n", WiFi.SSID(similar).c_str());
  WiFi.scanDelete();
}

bool connectWifiWithRetry(const String& ssid, const String& pass, int attempts) {
  for (int a = 1; a <= attempts; a++) {
    WiFi.disconnect(false, false);
    delay(200);
    WiFi.begin(ssid.c_str(), pass.c_str());
    Serial.printf("[WIFI] connecting to %s\n", ssid.c_str());
    if (waitWifiConnected(10000)) return true;
    Serial.printf("[WIFI] connect failed: %s (reason %u: %s)\n", wifiStatusText(WiFi.status()), (unsigned)lastWifiReason, wifiReasonText(lastWifiReason));
  }
  logWifiScan(ssid);
  return false;
}

void onWifiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
    lastWifiReason = info.wifi_sta_disconnected.reason;
    wifiReasonPending = true;
  }
}

void reportConnectivity() {
  unsigned long now = millis();
  if (wifiReasonPending) {
    wifiReasonPending = false;
    static uint8_t lastLoggedReason = 0;
    static unsigned long lastReasonLogMs = 0;
    uint8_t r = lastWifiReason;
    if (r != lastLoggedReason || now - lastReasonLogMs >= 30000UL) {
      lastLoggedReason = r;
      lastReasonLogMs = now;
      Serial.printf("[WIFI] disconnected, reason %u: %s\n", r, wifiReasonText(r));
    }
  }
  bool linkUp = WiFi.status() == WL_CONNECTED;
  bool mqttUp = false;
  if (linkUp && xSemaphoreTake(netMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
    mqttUp = mqttClient.connected();
    xSemaphoreGive(netMutex);
  }
  bool up = linkUp && (mqttUp || now - lastServerOkMs < SERVER_SILENCE_LIMIT_MS);
  if (!up) {
    const char* reason = linkUp ? "no internet (WiFi ok, server/MQTT unreachable)" : "WiFi link lost";
    if (netWasUp) {
      netWasUp = false;
      backlogDrainPending = true;
      netDownSince = now;
      lastOfflineReport = now;
      Serial.printf("[NET] offline: %s\n", reason);
      printOfflineBacklog();
    } else if (now - lastOfflineReport >= OFFLINE_REPORT_INTERVAL_MS) {
      lastOfflineReport = now;
      Serial.printf("[NET] offline %lus\n", (now - netDownSince) / 1000UL);
      printOfflineBacklog();
    }
    return;
  }
  if (!netWasUp) {
    netWasUp = true;
    Serial.printf("[NET] online sau %lus ip=%s\n", (now - netDownSince) / 1000UL, WiFi.localIP().toString().c_str());
  }
  if (backlogDrainPending) {
    bool drained = false;
    if (xSemaphoreTake(netMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
      drained = pendingCount == 0;
      xSemaphoreGive(netMutex);
    }
    if (drained) {
      backlogDrainPending = false;
      Serial.println("[D1] da bu xong");
    }
  }
}

void networkTask(void* pvParameters) {
  for (;;) {
    reportConnectivity();
    if (millis() - lastHeapLog >= HEAP_LOG_INTERVAL_MS) {
      lastHeapLog = millis();
      Serial.printf("[SYS] heap %u\n", ESP.getFreeHeap());
    }
    handleMqttLoop();
    if (otaPendingConfirmation && !bootCheckinOk && !otaInProgress && WiFi.status() == WL_CONNECTED &&
        millis() - lastCheckinTry >= 5000) {
      lastCheckinTry = millis();
      bootWifiOk = true;
      bootCheckinOk = reportOtaCheckin(true, FIRMWARE_VERSION);
    }
    if (!otaInProgress) {
      if (millis() - lastSettingsFetch >= SETTINGS_FETCH_INTERVAL_MS) {
        lastSettingsFetch = millis();
        OtaOffer offer;
        fetchThresholdsFromServer(&offer);
#if ENABLE_OTA_VERSION_CHECK
        if (offer.valid && (long)(millis() - otaBackoffUntil) >= 0) checkForOta(offer);
#endif
      }
    }
    if (!otaInProgress) {
      xSemaphoreTake(netMutex, portMAX_DELAY);
      bool doAlert = alertPending;
      int aDist = alertDistanceShared;
      char aLevel[8];
      strncpy(aLevel, alertLevelShared, sizeof(aLevel));
      alertPending = false;
      xSemaphoreGive(netMutex);
      if (doAlert) sendTelegramAlert(aDist, aLevel);
      flushPendingLogs();
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  pinMode(LED_GREEN, OUTPUT);
  pinMode(LED_YELLOW, OUTPUT);
  pinMode(LED_RED, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(LED_WIFI, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(100000);
  delay(150);

  lcd.init();
  delay(50);
  lcd.backlight();
  lcd.clear();
  delay(20);
  lcdPrintPadded(0, 0, "Khoi dong...");

  setupWatchdog();
  bootStartMs = millis();
  handleBootRollbackCheck();

  SENSOR_HEIGHT_CM = getSavedSensorHeight();
  Serial.printf("[SYS] do cao %.1f cm\n", SENSOR_HEIGHT_CM);

  WiFi.onEvent(onWifiEvent);
  WiFi.mode(WIFI_STA);
  WifiCfg cfg = loadWifiCfg();
  String ssid = cfg.ssid;
  String pass = cfg.pass;
  applyStaticIpIfSet(cfg);
  if (ssid.length() == 0) Serial.println("[WIFI] no SSID configured");
  bool wifiOk = ssid.length() > 0 && connectWifiWithRetry(ssid, pass, 2);
  if (!wifiOk && ssid != String(WIFI_SSID)) {
    Serial.println("[WIFI] saved network failed, trying default");
    wifiOk = connectWifiWithRetry(String(WIFI_SSID), String(WIFI_PASSWORD), 1);
  }
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP("ESP32_WaterLevel", AP_PASSWORD, 1, 0, 4);
  wifiReasonPending = false;

  if (WiFi.status() == WL_CONNECTED) {
    digitalWrite(LED_WIFI, HIGH);
    Serial.printf("[WIFI] ok ip=%s ap=%s\n", WiFi.localIP().toString().c_str(), WiFi.softAPIP().toString().c_str());
    lcdPrintPadded(0, 0, "IP nha:         ");
    lcdPrintPadded(0, 1, WiFi.localIP().toString().c_str());
    fetchThresholdsFromServer();
    lastSettingsFetch = millis();
    bootWifiOk = true;
    bootCheckinOk = reportOtaCheckin(true, FIRMWARE_VERSION);
  } else {
    digitalWrite(LED_WIFI, LOW);
    Serial.println("[WIFI] offline, AP mode 192.168.4.1");
    lcdPrintPadded(0, 0, "Dung WiFi AP:   ");
    lcdPrintPadded(0, 1, "192.168.4.1     ");
  }
  delay(2500);

  mqttClient.setServer(MQTT_HOST, MQTT_PORT);
  mqttClient.setBufferSize(512);
  mqttClient.setKeepAlive(30);
  tlsClient.setInsecure();
  tlsClient.setTimeout(5000);

  server.on("/", handleRoot);
  server.on("/data", handleData);
  server.on("/history", handleHistory);
  server.on("/history-filtered", handleHistoryFiltered);
  server.on("/wifi-status", handleWifiStatus);
  server.on("/wifi-config", HTTP_POST, handleWifiConfig);
  server.on("/sensor-config", HTTP_GET, handleSensorConfigGet);
  server.on("/sensor-config", HTTP_POST, handleSensorConfigSet);
  static const char* collectedHeaders[] = {"X-Device-Key"};
  server.collectHeaders(collectedHeaders, 1);
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.println("[SYS] web server san sang");

  netMutex = xSemaphoreCreateMutex();
  xTaskCreatePinnedToCore(networkTask, "networkTask", 12288, NULL, 1, NULL, 0);
  lastRead = millis();
}

void loop() {
  esp_task_wdt_reset();
  server.handleClient();
  buzzerTick();
  if (millis() - lastRead >= READ_INTERVAL && readAllowedNow()) {
    lastRead = millis();
    int raw = measureDistanceMedian();
    if (raw < 0) {
      currentDistance = -1;
      currentFilteredDistance = -1;
      currentWaterLevel = -1;
      currentEta = -1;
      currentRateCmPerMin = 0;
      lastRateDistance = -1;
      filterIndexStable = 0;
      filterFilledStable = false;
      if (!otaInProgress) showSensorError();
      sendLogToD1(-1, "error");
      publishMqttTelemetry(-1, -1, 0, "error");
    } else {
      currentDistance = raw;
#ifdef DEBUG_BUZZER
      Serial.printf("[DBG] raw=%dcm buzzer=%d\n", raw, (int)isBuzzerOn);
#endif
      bootSensorOk = true;
      currentFilteredDistance = pushAndSmoothStable(raw);
      updateRate(currentFilteredDistance);
      currentEta = estimateMinutesToDanger(currentFilteredDistance);
      currentWaterLevel = computeWaterLevel(currentFilteredDistance);
      lastUpdateMillis = millis();
      if (!otaInProgress) updateOutputs(currentFilteredDistance, currentEta);
      rawHist.push(currentDistance);
      fltHist.push(currentFilteredDistance);
      sendLogToD1(currentFilteredDistance, levelFromDistance(currentFilteredDistance));
      publishMqttTelemetry(currentDistance, currentFilteredDistance, currentRateCmPerMin, levelFromDistance(currentFilteredDistance));
    }
  }
  checkBootConfirmation();
}
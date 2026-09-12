#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <Update.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include "esp_task_wdt.h"
#include "esp_ota_ops.h"
#include "secrets.h"
#define FIRMWARE_VERSION "1.0.3"
#define WDT_TIMEOUT_SEC 15
#define BOOT_CONFIRM_WINDOW_MS 36000UL
#define BOOT_FAIL_ROLLBACK_THRESHOLD 5
#define TRIG_PIN 5
#define ECHO_PIN 18
#define LED_GREEN 25
#define LED_YELLOW 26
#define LED_RED 27
#define BUZZER_PIN 19
#define LED_WIFI 2
LiquidCrystal_I2C lcd(0x27, 16, 2);
float DANGER_THRESHOLD = 15.0;
float WARN_THRESHOLD   = 40.0;
float DETECT_THRESHOLD = 180.0;
const float MAX_DISTANCE = 400.0;
const char* ALERT_WORKER_URL    = "https://waterlevelmonitor.luumanhkien08092006.workers.dev/alert";
const char* LOG_WORKER_URL      = "https://waterlevelmonitor.luumanhkien08092006.workers.dev/log";
const char* SETTINGS_WORKER_URL = "https://waterlevelmonitor.luumanhkien08092006.workers.dev/get-settings";
const char* OTA_ACTIVE_URL      = "https://waterlevelmonitor.luumanhkien08092006.workers.dev/ota/active";
const char* OTA_CHECKIN_URL     = "https://waterlevelmonitor.luumanhkien08092006.workers.dev/ota/checkin";
const char* DEVICE_ID = "mahirun";
const char* DEVICE_KEY_VALUE = DEVICE_KEY_SECRET;
String lastSentLevel = "";
const float LOG_CHANGE_THRESHOLD = 3.0;
const unsigned long HEARTBEAT_SAFE_MS = 10UL * 60UL * 1000UL;
const unsigned long HEARTBEAT_DETECT_STABLE_MS = 1UL * 60UL * 1000UL;
const unsigned long ACTIVE_CHANGE_WINDOW_MS = 60UL * 1000UL;
int lastLoggedDistance = -9999;
String lastLoggedLevel = "";
unsigned long lastLogTime = 0;
unsigned long lastChangeTime = 0;
const unsigned long SETTINGS_FETCH_INTERVAL_MS = 30UL * 1000UL;
unsigned long lastSettingsFetch = 0;
const unsigned long OTA_CHECK_INTERVAL_MS = 20UL * 1000UL;
const unsigned long OTA_BACKOFF_MS = 10UL * 60UL * 1000UL;
const int OTA_MAX_FAIL = 3;
unsigned long lastOtaCheck = 0;
bool otaInProgress = false;
int otaFailCount = 0;
String otaLastFailedVersion = "";
WebServer server(80);
Preferences prefs;
bool bootHealthConfirmed = false;
bool bootWifiOk = false;
bool bootCheckinOk = false;
bool bootSensorOk = false;
bool otaPendingConfirmation = false;
unsigned long bootStartMs = 0;
SemaphoreHandle_t netMutex;
volatile bool alertPending = false;
int alertDistanceShared = -1;
char alertLevelShared[8] = "";
void triggerAlert(int distance, const char* level) {
  xSemaphoreTake(netMutex, portMAX_DELAY);
  alertPending = true;
  alertDistanceShared = distance;
  strncpy(alertLevelShared, level, sizeof(alertLevelShared) - 1);
  alertLevelShared[sizeof(alertLevelShared) - 1] = '\0';
  xSemaphoreGive(netMutex);
}
const unsigned long READ_INTERVAL = 1000;
unsigned long lastRead = 0;
String getSavedSSID() {
  prefs.begin("wifi", true);
  String s = prefs.getString("ssid", WIFI_SSID);
  prefs.end();
  return s;
}
String getSavedPassword() {
  prefs.begin("wifi", true);
  String p = prefs.getString("pass", WIFI_PASSWORD);
  prefs.end();
  return p;
}
String getSavedStaticIp() {
  prefs.begin("wifi", true);
  String v = prefs.getString("ip", "");
  prefs.end();
  return v;
}
String getSavedGateway() {
  prefs.begin("wifi", true);
  String v = prefs.getString("gw", "");
  prefs.end();
  return v;
}
bool applyStaticIpIfSet() {
  String ipStr = getSavedStaticIp();
  String gwStr = getSavedGateway();
  if (ipStr.length() == 0) return false;
  IPAddress ip, gw, subnet(255, 255, 255, 0);
  if (!ip.fromString(ipStr)) return false;
  if (gwStr.length() > 0) gw.fromString(gwStr); else gw = ip;
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
const unsigned long RATE_WINDOW = 5000;
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
int historyBuf[HISTORY_SIZE];
int historyCount = 0;
int historyHead = 0;
int filteredHistoryBuf[HISTORY_SIZE];
int filteredHistoryCount = 0;
int filteredHistoryHead = 0;
void pushFilteredHistory(int distance) {
  filteredHistoryBuf[filteredHistoryHead] = distance;
  filteredHistoryHead = (filteredHistoryHead + 1) % HISTORY_SIZE;
  if (filteredHistoryCount < HISTORY_SIZE) filteredHistoryCount++;
}
bool isBuzzerOn = false;
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
void pushHistory(int distance) {
  historyBuf[historyHead] = distance;
  historyHead = (historyHead + 1) % HISTORY_SIZE;
  if (historyCount < HISTORY_SIZE) historyCount++;
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
const char* levelFromDistance(int distance) {
  if (distance < 0) return "error";
  if (distance <= DANGER_THRESHOLD) return "danger";
  if (distance <= WARN_THRESHOLD) return "warn";
  if (distance <= DETECT_THRESHOLD) return "detect";
  return "safe";
}
void fetchThresholdsFromServer() {
  if (WiFi.status() != WL_CONNECTED) return;
  HTTPClient http;
  http.setTimeout(4000);
  String url = String(SETTINGS_WORKER_URL) + "?deviceId=" + String(DEVICE_ID);
  http.begin(url);
  int httpCode = http.GET();
  if (httpCode == 200) {
    String payload = http.getString();
    StaticJsonDocument<256> doc;
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
          Serial.printf("Da cap nhat nguong tu server: danger=%.1f warn=%.1f detect=%.1f\n", d, w, dt);
        }
      } else {
        Serial.println("Nguong tu server khong hop le, bo qua.");
      }
    }
  } else if (httpCode > 0) {
    Serial.printf("Loi lay nguong: HTTP %d\n", httpCode);
  } else {
    Serial.printf("Loi lay nguong: %s\n", http.errorToString(httpCode).c_str());
  }
  http.end();
}
bool reportOtaCheckin(bool ok, const char* version) {
  if (WiFi.status() != WL_CONNECTED) return false;
  HTTPClient http;
  http.setTimeout(4000);
  http.begin(OTA_CHECKIN_URL);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-Device-Key", DEVICE_KEY_VALUE);
  StaticJsonDocument<256> doc;
  doc["deviceId"] = DEVICE_ID;
  doc["version"] = version;
  doc["ok"] = ok;
  String payload;
  serializeJson(doc, payload);
  int httpCode = http.POST(payload);
  http.end();
  return httpCode > 0 && httpCode < 400;
}
bool performOta(const String& url, size_t expectedSize) {
  if (WiFi.status() != WL_CONNECTED) return false;
  HTTPClient http;
  http.setTimeout(15000);
  http.begin(url);
  int httpCode = http.GET();
  if (httpCode != 200) {
    Serial.printf("OTA: khong tai duoc file, HTTP %d\n", httpCode);
    http.end();
    return false;
  }
  int contentLength = http.getSize();
  if (contentLength <= 0 || (expectedSize > 0 && (size_t)contentLength != expectedSize)) {
    Serial.println("OTA: kich thuoc file khong khop, huy.");
    http.end();
    return false;
  }
  if (!Update.begin(contentLength)) {
    Serial.printf("OTA: khong du bo nho flash cho update (%d bytes)\n", contentLength);
    http.end();
    return false;
  }
  WiFiClient* stream = http.getStreamPtr();
  uint8_t buf[1024];
  size_t written = 0;
  unsigned long lastDataMs = millis();
  while (http.connected() && written < (size_t)contentLength) {
    esp_task_wdt_reset();
    size_t avail = stream->available();
    if (avail > 0) {
      int n = stream->readBytes(buf, avail > sizeof(buf) ? sizeof(buf) : avail);
      if (n > 0) {
        Update.write(buf, n);
        written += n;
        lastDataMs = millis();
      }
    } else {
      if (millis() - lastDataMs > 10000) {
        Serial.println("OTA: qua thoi gian cho du lieu, huy.");
        break;
      }
      delay(2);
    }
  }
  http.end();
  if (written != (size_t)contentLength) {
    Serial.printf("OTA: chi ghi duoc %d/%d bytes, huy.\n", (int)written, contentLength);
    Update.abort();
    return false;
  }
  if (!Update.end(true)) {
    Serial.printf("OTA: loi finalize - %s\n", Update.errorString());
    return false;
  }
  Serial.println("OTA: flash thanh cong, khoi dong lai...");
  return true;
}
void checkForOta() {
  if (otaInProgress || WiFi.status() != WL_CONNECTED) return;
  HTTPClient http;
  http.setTimeout(5000);
  String url = String(OTA_ACTIVE_URL) + "?deviceId=" + String(DEVICE_ID);
  http.begin(url);
  int httpCode = http.GET();
  if (httpCode != 200) {
    http.end();
    return;
  }
  String payload = http.getString();
  http.end();
  StaticJsonDocument<512> doc;
  if (deserializeJson(doc, payload)) return;
  if (!doc["active"].as<bool>()) return;
  String remoteVersion = doc["version"].as<String>();
  String fwUrl = doc["url"].as<String>();
  size_t remoteSize = doc["size"].as<size_t>();
  if (remoteVersion == FIRMWARE_VERSION) return;
  if (otaLastFailedVersion == remoteVersion && otaFailCount >= OTA_MAX_FAIL) {
    return;
  }
  Serial.printf("OTA: phat hien ban moi %s (dang chay %s), bat dau tai...\n",
                remoteVersion.c_str(), FIRMWARE_VERSION);
  otaInProgress = true;
  lcd.setCursor(0, 0);
  lcd.print("Dang cap nhat...");
  lcd.setCursor(0, 1);
  lcd.print("Vui long doi    ");
  buzzerStop();
  bool ok = performOta(fwUrl, remoteSize);
  if (ok) {
    prefs.begin("otaboot", false);
    prefs.putBool("pending", true);
    prefs.putString("pendingVer", remoteVersion);
    prefs.putUInt("bootFails", 0);
    prefs.end();
    delay(300);
    ESP.restart();
  } else {
    otaInProgress = false;
    reportOtaCheckin(false, remoteVersion.c_str());
    if (otaLastFailedVersion == remoteVersion) {
      otaFailCount++;
    } else {
      otaLastFailedVersion = remoteVersion;
      otaFailCount = 1;
    }
    lcd.setCursor(0, 0);
    lcd.print("Cap nhat that bai");
    lcd.setCursor(0, 1);
    if (otaFailCount >= OTA_MAX_FAIL) {
      lcd.print("Da tam dung OTA ");
      Serial.printf("OTA: that bai %d lan voi ban %s, tam dung ~10 phut.\n",
                    otaFailCount, remoteVersion.c_str());
      lastOtaCheck = millis() + OTA_BACKOFF_MS - OTA_CHECK_INTERVAL_MS;
    } else {
      lcd.print("Van chay ban cu ");
    }
    delay(2000);
  }
}
void sendTelegramAlert(int distance, const char* level) {
  if (lastSentLevel == level) return;
  if (WiFi.status() != WL_CONNECTED) return;
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
  if (httpCode > 0) {
    Serial.printf("Telegram alert gui: %d - %s\n", httpCode, http.getString().c_str());
    lastSentLevel = level;
  } else {
    Serial.printf("Loi gui alert: %s\n", http.errorToString(httpCode).c_str());
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
    Serial.println("Queue log day, da bo ban ghi cu nhat.");
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
  doc["rate"] = serialized(String(rate, 2));
  doc["level"] = level;
  String payload;
  serializeJson(doc, payload);
  int httpCode = http.POST(payload);
  bool ok = httpCode > 0 && httpCode < 400;
  if (ok) {
    Serial.printf("Da ghi D1: %d\n", httpCode);
  } else if (httpCode > 0) {
    Serial.printf("Loi ghi D1: HTTP %d\n", httpCode);
  } else {
    Serial.printf("Loi ghi D1: %s\n", http.errorToString(httpCode).c_str());
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
  bool shouldSend = false;
  if (changedEnough || levelChanged) {
    lastChangeTime = now;
    shouldSend = true;
  } else {
    bool activelyChanging = (now - lastChangeTime) < ACTIVE_CHANGE_WINDOW_MS;
    if (!activelyChanging) {
      unsigned long heartbeatInterval = isDetecting ? HEARTBEAT_DETECT_STABLE_MS : HEARTBEAT_SAFE_MS;
      if (now - lastLogTime >= heartbeatInterval) shouldSend = true;
    }
  }
  if (!shouldSend) return;
  lastLoggedDistance = distance;
  lastLoggedLevel = level;
  lastLogTime = now;
  xSemaphoreTake(netMutex, portMAX_DELAY);
  queuePendingLog(distance, currentRateCmPerMin, level);
  xSemaphoreGive(netMutex);
}
void updateOutputs(int distance, float etaMinutes) {
  lcd.setCursor(0, 0);
  char line0[17];
  snprintf(line0, sizeof(line0), "Muc nuoc:%4dcm", distance);
  lcd.print(line0);
  lcd.setCursor(0, 1);
  if (distance > 0 && distance <= DANGER_THRESHOLD) {
    lcd.print("NGUY HIEM: DAY! ");
    digitalWrite(LED_RED, HIGH);
    digitalWrite(LED_YELLOW, LOW);
    digitalWrite(LED_GREEN, LOW);
    buzzerAlert(1000);
    triggerAlert(distance, "danger");
  }
  else if (distance > DANGER_THRESHOLD && distance <= WARN_THRESHOLD) {
    lcd.print("Muc trung binh  ");
    digitalWrite(LED_RED, LOW);
    digitalWrite(LED_YELLOW, HIGH);
    digitalWrite(LED_GREEN, LOW);
    buzzerStop();
    triggerAlert(distance, "warn");
  }
  else if (distance > WARN_THRESHOLD && distance <= DETECT_THRESHOLD) {
    lcd.print("Phat hien nuoc  ");
    digitalWrite(LED_RED, LOW);
    digitalWrite(LED_YELLOW, HIGH);
    digitalWrite(LED_GREEN, LOW);
    buzzerStop();
    triggerAlert(distance, "detect");
  }
  else if (distance > DETECT_THRESHOLD) {
    lcd.print("Muc an toan     ");
    digitalWrite(LED_RED, LOW);
    digitalWrite(LED_YELLOW, LOW);
    digitalWrite(LED_GREEN, HIGH);
    buzzerStop();
    triggerAlert(distance, "safe");
  }
}
void showSensorError() {
  lcd.setCursor(0, 0);
  lcd.print("Loi cam bien!   ");
  lcd.setCursor(0, 1);
  lcd.print("Kiem tra day noi");
  digitalWrite(LED_RED, HIGH);
  digitalWrite(LED_YELLOW, HIGH);
  digitalWrite(LED_GREEN, HIGH);
  buzzerStop();
  triggerAlert(-1, "error");
}
const char INDEX_HTML[] PROGMEM = R"HTMLPAGE(
<!DOCTYPE html>
<html lang="vi">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Giám Sát Mực Nước</title>
<style>
  :root{--safe:#22c55e;--warn:#eab308;--danger:#ef4444;--accent:#38bdf8;
    --bg:#090e17;--card:#131c2e;--card-inner:#0b1322;--text:#f1f5f9;
    --muted:#94a3b8;--border:rgba(255,255,255,.08);}
  *{box-sizing:border-box;margin:0;padding:0;}
  body{font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,sans-serif;
    background:radial-gradient(circle at 50% 0%,#1e293b 0%,var(--bg) 80%);
    color:var(--text);min-height:100vh;display:flex;justify-content:center;
    padding:16px;}
  .card{width:100%;max-width:520px;background:var(--card);border-radius:18px;
    border:1px solid var(--border);box-shadow:0 16px 40px rgba(0,0,0,.6);
    padding:18px;display:flex;flex-direction:column;gap:14px;}
  h1{font-size:1.1rem;font-weight:700;border-bottom:1px solid var(--border);
    padding-bottom:10px;display:flex;justify-content:space-between;align-items:center;}
  .conn{font-size:.72rem;color:var(--muted);font-weight:500;}
  .conn.online{color:var(--safe);}
  .metric{background:var(--card-inner);border-radius:14px;border:1px solid var(--border);
    padding:16px;text-align:center;}
  .label{font-size:.72rem;text-transform:uppercase;color:var(--muted);font-weight:600;letter-spacing:.05em;}
  .value{font-size:3rem;font-weight:800;margin:6px 0;transition:color .3s;}
  .unit{font-size:1rem;color:var(--muted);font-weight:600;}
  .gauge-bg{width:100%;height:8px;background:#1e293b;border-radius:999px;overflow:hidden;margin-top:8px;}
  .gauge-fill{height:100%;border-radius:999px;transition:width .4s,background .4s;}
  .grid{display:grid;grid-template-columns:1fr 1fr;gap:10px;}
  .box{background:var(--card-inner);border-radius:12px;border:1px solid var(--border);
    padding:10px;text-align:center;}
  .box-title{font-size:.68rem;color:var(--muted);margin-bottom:2px;}
  .box-val{font-size:1rem;font-weight:700;}
  .status-msg{text-align:center;padding:10px;border-radius:10px;font-weight:600;
    font-size:.8rem;border:1px solid transparent;}
  .chart-wrap{position:relative;width:100%;height:140px;background:var(--card-inner);
    border-radius:12px;border:1px solid var(--border);}
  canvas{width:100%;height:100%;}
  .ts{font-size:.7rem;color:var(--muted);text-align:right;}
  .modal-overlay{display:none;position:fixed;inset:0;background:rgba(0,0,0,0.7);z-index:999;justify-content:center;align-items:center;padding:16px;}
  .modal-content{width:100%;max-width:400px;background:var(--card);border-radius:16px;border:1px solid var(--border);padding:20px;display:flex;flex-direction:column;gap:12px;}
  .modal-header{display:flex;justify-content:space-between;align-items:center;}
  .modal-header h2{font-size:1.1rem;margin:0;}
  .close-btn{background:none;border:none;color:var(--text);font-size:1.5rem;cursor:pointer;}
  .input-field{padding:10px;border-radius:8px;border:1px solid var(--border);background:var(--card-inner);color:var(--text);font-size:0.9rem;width:100%;}
  .btn{padding:10px;border-radius:8px;border:none;cursor:pointer;font-weight:bold;font-size:0.9rem;width:100%;}
  .btn-accent{background:var(--accent);color:#000;}
  .btn-safe{background:var(--safe);color:#000;}
</style>
</head>
<body>
<div class="card">
  <h1>
    <span>Giám Sát Mực Nước</span>
    <div style="display:flex;align-items:center;gap:10px;">
      <span id="conn" class="conn">Đang kết nối...</span>
      <button id="wifiOpenBtn" title="Đổi WiFi" style="background:none;border:none;cursor:pointer;font-size:1.2rem;">📶</button>
    </div>
  </h1>
  <div class="metric">
    <div class="label">Khoảng Cách Tới Mặt Nước</div>
    <div><span id="distVal" class="value" style="color:var(--muted)">--</span><span class="unit"> cm</span></div>
    <div class="gauge-bg"><div id="gaugeFill" class="gauge-fill" style="width:0%;background:var(--muted)"></div></div>
  </div>
  <div class="grid">
    <div class="box"><div class="box-title">Xu Hướng</div><div id="trendDir" class="box-val">--</div></div>
    <div class="box"><div class="box-title">Tốc Độ</div><div id="trendRate" class="box-val">-- cm/p</div></div>
  </div>
  <div id="msgBox" class="status-msg">Đang chờ dữ liệu...</div>
  <div class="chart-wrap"><canvas id="chart"></canvas></div>
  <div class="ts">Cập nhật: <span id="ts">--:--:--</span></div>
</div>
<div id="wifiModal" class="modal-overlay">
  <div class="modal-content">
    <div class="modal-header">
      <h2>Cấu Hình WiFi</h2>
      <button id="closeWifiBtn" class="close-btn">✕</button>
    </div>
    <div style="font-size:0.85rem;color:var(--muted);">
      Trạng thái: <span id="wifiStatusText" style="color:var(--text);font-weight:bold;">Đang kiểm tra...</span>
    </div>
    <hr style="border:1px solid var(--border);width:100%;margin:5px 0;">
    <input type="password" id="devKeyInput" class="input-field" placeholder="Device Key (bắt buộc)">
    <input type="text" id="ssidManualInput" class="input-field" placeholder="Tên WiFi (SSID)">
    <input type="password" id="wifiPassInput" class="input-field" placeholder="Mật khẩu WiFi">
    <input type="text" id="staticIpInput" class="input-field" placeholder="IP tĩnh (không bắt buộc)">
    <input type="text" id="gatewayInput" class="input-field" placeholder="Gateway (không bắt buộc)">
    <button id="saveWifiBtn" class="btn btn-safe">Lưu & Kết Nối Lại</button>
    <div id="wifiMsg" style="font-size:0.85rem;text-align:center;"></div>
  </div>
</div>
<script>
const DANGER = %DANGER%;
const WARN = %WARN%;
const MAXD = %MAXD%;
const distVal = document.getElementById('distVal');
const gaugeFill = document.getElementById('gaugeFill');
const trendDir = document.getElementById('trendDir');
const trendRate = document.getElementById('trendRate');
const msgBox = document.getElementById('msgBox');
const connEl = document.getElementById('conn');
const tsEl = document.getElementById('ts');
const canvas = document.getElementById('chart');
const ctx = canvas.getContext('2d');
const wifiModal = document.getElementById('wifiModal');
const wifiOpenBtn = document.getElementById('wifiOpenBtn');
const closeWifiBtn = document.getElementById('closeWifiBtn');
const wifiStatusText = document.getElementById('wifiStatusText');
const devKeyInput = document.getElementById('devKeyInput');
const wifiPassInput = document.getElementById('wifiPassInput');
const saveWifiBtn = document.getElementById('saveWifiBtn');
const wifiMsg = document.getElementById('wifiMsg');
wifiOpenBtn.addEventListener('click', async () => {
  wifiModal.style.display = 'flex';
  wifiStatusText.textContent = 'Đang tải...';
  wifiMsg.textContent = '';
  try {
    const res = await fetch('/wifi-status');
    const data = await res.json();
    if(data.connected) {
      wifiStatusText.textContent = `Đã kết nối: ${data.ssid} (${data.ip})`;
    } else {
      wifiStatusText.textContent = 'Chưa kết nối';
    }
  } catch(e) {
    wifiStatusText.textContent = 'Lỗi kết nối';
  }
});
closeWifiBtn.addEventListener('click', () => {
  wifiModal.style.display = 'none';
});
const ssidManualInput = document.getElementById('ssidManualInput');
const staticIpInput = document.getElementById('staticIpInput');
const gatewayInput = document.getElementById('gatewayInput');
saveWifiBtn.addEventListener('click', async () => {
  const key = devKeyInput.value.trim();
  const ssid = ssidManualInput.value.trim();
  const pass = wifiPassInput.value;
  const staticIp = staticIpInput.value.trim();
  const gateway = gatewayInput.value.trim();
  if(!key || !ssid) {
    wifiMsg.textContent = 'Cần nhập Device Key và tên mạng WiFi';
    wifiMsg.style.color = 'var(--warn)';
    return;
  }
  saveWifiBtn.disabled = true;
  saveWifiBtn.textContent = 'Đang lưu...';
  try {
    const payload = {ssid: ssid, password: pass};
    if (staticIp) payload.staticIp = staticIp;
    if (gateway) payload.gateway = gateway;
    const res = await fetch('/wifi-config', {
      method: 'POST',
      headers: {
        'Content-Type': 'application/json',
        'X-Device-Key': key
      },
      body: JSON.stringify(payload)
    });
    if(!res.ok) throw new Error('Cấu hình thất bại (Sai Device Key?)');
    wifiMsg.textContent = 'Đã lưu! Thiết bị đang khởi động lại vào mạng mới. Hãy tìm IP mới và tải lại trang.';
    wifiMsg.style.color = 'var(--safe)';
  } catch(e) {
    wifiMsg.textContent = e.message;
    wifiMsg.style.color = 'var(--danger)';
    saveWifiBtn.disabled = false;
    saveWifiBtn.textContent = 'Lưu & Kết Nối Lại';
  }
});
function classify(d){
  if(d<0) return {color:'#94a3b8', label:'Cảm biến lỗi'};
  if(d<=DANGER) return {color:'#ef4444', label:'NGUY HIỂM (GẦN TRÀN)'};
  if(d<=WARN) return {color:'#eab308', label:'MỨC TRUNG BÌNH'};
  return {color:'#22c55e', label:'AN TOÀN'};
}
function drawSeries(history, color){
  const w = canvas.width, h = canvas.height;
  if(history.length < 2) return;
  const pad = 10 * devicePixelRatio;
  const stepX = (w - pad*2) / (history.length - 1);
  ctx.beginPath();
  history.forEach((d,i)=>{
    const level = Math.max(0, Math.min(1, (MAXD - d) / MAXD));
    const x = pad + i*stepX;
    const y = h - pad - level*(h - pad*2);
    if(i===0) ctx.moveTo(x,y); else ctx.lineTo(x,y);
  });
  ctx.strokeStyle = color;
  ctx.lineWidth = 2*devicePixelRatio;
  ctx.stroke();
}
let lastRawHistory = [];
let lastFilteredHistory = [];
function drawChart(){
  const w = canvas.width = canvas.clientWidth * devicePixelRatio;
  const h = canvas.height = canvas.clientHeight * devicePixelRatio;
  ctx.clearRect(0,0,w,h);
  // Duong xanh nhat (mo): gia tri da loc nhieu - doi cham/muot hon co y.
  drawSeries(lastFilteredHistory, '#38bdf866');
  // Duong xanh dam: gia tri thuc te (raw) - doi ngay theo cam bien.
  drawSeries(lastRawHistory, '#38bdf8');
}
async function poll(){
  try{
    const res = await fetch('/data');
    const d = await res.json();
    connEl.textContent = '● Online';
    connEl.classList.add('online');
    const info = classify(d.distance);
    distVal.textContent = d.distance >= 0 ? d.distance.toFixed(0) : '--';
    distVal.style.color = info.color;
    msgBox.textContent = info.label;
    msgBox.style.background = info.color+'22';
    msgBox.style.color = info.color;
    msgBox.style.borderColor = info.color+'55';
    const fillPct = d.distance >= 0 ? Math.max(0,Math.min(100,((MAXD-d.distance)/MAXD)*100)) : 0;
    gaugeFill.style.width = fillPct+'%';
    gaugeFill.style.background = info.color;
    const rate = d.rate || 0;
    trendRate.textContent = Math.abs(rate).toFixed(2)+' cm/p';
    if(rate < -0.3){ trendDir.textContent='Đang dâng ↑'; trendDir.style.color='#ef4444'; }
    else if(rate > 0.3){ trendDir.textContent='Đang rút ↓'; trendDir.style.color='#22c55e'; }
    else { trendDir.textContent='Ổn định •'; trendDir.style.color='#94a3b8'; }
    tsEl.textContent = new Date().toLocaleTimeString('vi-VN');
  }catch(e){
    connEl.textContent = '✕ Mất kết nối';
    connEl.classList.remove('online');
  }
}
async function pollHistory(){
  try{
    const res = await fetch('/history');
    lastRawHistory = await res.json();
    drawChart();
  }catch(e){}
}
async function pollHistoryFiltered(){
  try{
    const res = await fetch('/history-filtered');
    lastFilteredHistory = await res.json();
    drawChart();
  }catch(e){}
}
poll();
pollHistory();
pollHistoryFiltered();
setInterval(poll, 1000);
setInterval(pollHistory, 1000);
setInterval(pollHistoryFiltered, 3000);
</script>
</body>
</html>
)HTMLPAGE";
String buildIndexPage() {
  String page = String(INDEX_HTML);
  page.replace("%DANGER%", String(DANGER_THRESHOLD, 1));
  page.replace("%WARN%", String(WARN_THRESHOLD, 1));
  page.replace("%MAXD%", String(MAX_DISTANCE, 1));
  return page;
}
void handleRoot() {
  server.send(200, "text/html", buildIndexPage());
}
void addCorsHeaders() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "*");
}
void handleData() {
  addCorsHeaders();
  StaticJsonDocument<256> doc;
  doc["distance"] = currentDistance;
  doc["stableDistance"] = currentFilteredDistance;
  doc["rate"] = serialized(String(currentRateCmPerMin, 2));
  if (currentEta >= 0) doc["eta"] = serialized(String(currentEta, 1));
  else doc["eta"] = nullptr;
  doc["pendingLogs"] = pendingCount;
  doc["timestamp"] = lastUpdateMillis;
  String json;
  serializeJson(doc, json);
  server.send(200, "application/json", json);
}
void handleHistory() {
  addCorsHeaders();
  DynamicJsonDocument doc(1024);
  JsonArray arr = doc.to<JsonArray>();
  for (int i = 0; i < historyCount; i++) {
    int idx = (historyHead - historyCount + i + HISTORY_SIZE) % HISTORY_SIZE;
    arr.add(historyBuf[idx]);
  }
  String json;
  serializeJson(doc, json);
  server.send(200, "application/json", json);
}
void handleHistoryFiltered() {
  addCorsHeaders();
  DynamicJsonDocument doc(1024);
  JsonArray arr = doc.to<JsonArray>();
  for (int i = 0; i < filteredHistoryCount; i++) {
    int idx = (filteredHistoryHead - filteredHistoryCount + i + HISTORY_SIZE) % HISTORY_SIZE;
    arr.add(filteredHistoryBuf[idx]);
  }
  String json;
  serializeJson(doc, json);
  server.send(200, "application/json", json);
}
// So sanh hai chuoi theo kieu "constant-time" de tranh timing attack:
// luon duyet het chieu dai toi da thay vi dung som khi gap ky tu sai,
// nen thoi gian chay khong tiet lo key dung toi dau.
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
  // SSID do nguoi dung tu nhap qua /wifi-config nen co the chua ky tu
  // dac biet (vd dau ngoac kep) - de ArduinoJson tu escape thay vi noi
  // chuoi tay, tranh lam hong cau truc JSON tra ve.
  doc["ssid"] = getSavedSSID();
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
  String ssid = doc["ssid"].as<String>();
  String pass = doc["password"].as<String>();
  String staticIp = doc["staticIp"].as<String>();
  String gateway = doc["gateway"].as<String>();
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
  lcd.setCursor(0, 0);
  lcd.print("Dang rollback...");
  lcd.setCursor(0, 1);
  lcd.print(reason);

  prefs.begin("otaboot", true);
  String pendingVer = prefs.getString("pendingVer", "");
  prefs.end();

  if (WiFi.status() == WL_CONNECTED && pendingVer.length() > 0) {
    reportOtaCheckin(false, pendingVer.c_str());
  }

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
  esp_ota_mark_app_valid_cancel_rollback();
  prefs.begin("otaboot", false);
  prefs.putBool("pending", false);
  prefs.putUInt("bootFails", 0);
  prefs.end();
  Serial.println("Boot da duoc xac nhan on dinh.");
}

void checkBootConfirmation() {
  if (bootHealthConfirmed) return;
  if (bootWifiOk && bootCheckinOk && bootSensorOk) {
    markBootHealthy();
    return;
  }
  if (otaPendingConfirmation && millis() - bootStartMs > BOOT_CONFIRM_WINDOW_MS) {
    rollbackToPreviousFirmware("Khong xac nhan duoc ban moi sau 36s");
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

void networkTask(void* pvParameters) {
  esp_task_wdt_add(NULL);
  for (;;) {
    esp_task_wdt_reset();
    if (!otaInProgress) {
      if (millis() - lastSettingsFetch >= SETTINGS_FETCH_INTERVAL_MS) {
        lastSettingsFetch = millis();
        fetchThresholdsFromServer();
      }
      if (millis() - lastOtaCheck >= OTA_CHECK_INTERVAL_MS) {
        lastOtaCheck = millis();
        checkForOta();
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
    vTaskDelay(pdMS_TO_TICKS(300));
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
  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("Khoi dong...");

  setupWatchdog();
  bootStartMs = millis();
  handleBootRollbackCheck();

  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP("ESP32_WaterLevel", AP_PASSWORD, 1, 0, 4);
  String ssid = getSavedSSID();
  String pass = getSavedPassword();
  applyStaticIpIfSet();
  WiFi.begin(ssid.c_str(), pass.c_str());
  Serial.println("\nDang ket noi vao WiFi da luu...");
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 10000) {
    esp_task_wdt_reset();
    delay(300);
    Serial.print(".");
  }
  if (WiFi.status() != WL_CONNECTED && ssid != String(WIFI_SSID)) {
    Serial.println("\nKhong ket noi duoc mang da luu, thu lai voi WiFi mac dinh trong secrets.h...");
    WiFi.disconnect();
    delay(200);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 10000) {
      esp_task_wdt_reset();
      delay(300);
      Serial.print(".");
    }
  }
  lcd.clear();
  if (WiFi.status() == WL_CONNECTED) {
    digitalWrite(LED_WIFI, HIGH);
    Serial.println("\nWiFi nha: Da ket noi!");
    Serial.print("IP mang nha: ");
    Serial.println(WiFi.localIP());
    Serial.print("IP AP du phong: ");
    Serial.println(WiFi.softAPIP());
    lcd.setCursor(0, 0);
    lcd.print("IP nha:         ");
    lcd.setCursor(0, 1);
    lcd.print(WiFi.localIP().toString());
    fetchThresholdsFromServer();
    lastSettingsFetch = millis();
    bootWifiOk = true;
    bootCheckinOk = reportOtaCheckin(true, FIRMWARE_VERSION);
    lastOtaCheck = millis();
  } else {
    digitalWrite(LED_WIFI, LOW);
    Serial.println("\nKhong bat duoc WiFi nha, dung IP AP du phong: 192.168.4.1");
    lcd.setCursor(0, 0);
    lcd.print("Dung WiFi AP:   ");
    lcd.setCursor(0, 1);
    lcd.print("192.168.4.1     ");
  }
  delay(2500);
  server.on("/", handleRoot);
  server.on("/data", handleData);
  server.on("/history", handleHistory);
  server.on("/history-filtered", handleHistoryFiltered);
  server.on("/wifi-status", handleWifiStatus);
  server.on("/wifi-config", HTTP_POST, handleWifiConfig);
  server.begin();
  Serial.println("Web server da khoi dong.");
  netMutex = xSemaphoreCreateMutex();
  xTaskCreatePinnedToCore(networkTask, "networkTask", 8192, NULL, 1, NULL, 0);
  lastRead = millis();
}
void loop() {
  esp_task_wdt_reset();
  server.handleClient();
  if (millis() - lastRead >= READ_INTERVAL) {
    lastRead = millis();
    int raw = measureDistanceRaw();
    if (raw < 0) {
      currentDistance = -1;
      showSensorError();
      sendLogToD1(-1, "error");
    } else {
      currentDistance = raw;
      bootSensorOk = true;
      currentFilteredDistance = pushAndSmoothStable(raw);
      updateRate(currentDistance);
      currentEta = estimateMinutesToDanger(currentDistance);
      lastUpdateMillis = millis();
      updateOutputs(currentDistance, currentEta);
      pushHistory(currentDistance);
      pushFilteredHistory(currentFilteredDistance);
      sendLogToD1(currentFilteredDistance, levelFromDistance(currentFilteredDistance));
    }
  }
  checkBootConfirmation();
}
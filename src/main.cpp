#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <Update.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <ArduinoJson.h>
#include "secrets.h"

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
#define FIRMWARE_VERSION "1.0.0"

String lastSentLevel = "";

const float LOG_CHANGE_THRESHOLD = 3.0;
const unsigned long LOG_HEARTBEAT_MS = 5UL * 60UL * 1000UL;
int lastLoggedDistance = -9999;
String lastLoggedLevel = "";
unsigned long lastLogTime = 0;
const unsigned long SETTINGS_FETCH_INTERVAL_MS = 30UL * 1000UL;
unsigned long lastSettingsFetch = 0;

const unsigned long OTA_CHECK_INTERVAL_MS = 20UL * 1000UL;
unsigned long lastOtaCheck = 0;
bool otaInProgress = false;

WebServer server(80);

const unsigned long READ_INTERVAL = 500;
unsigned long lastRead = 0;

const int FILTER_SIZE_FAST = 2;
int filterBufFast[FILTER_SIZE_FAST];
int filterIndexFast = 0;
bool filterFilledFast = false;

int pushAndSmoothFast(int rawValue) {
  filterBufFast[filterIndexFast] = rawValue;
  filterIndexFast = (filterIndexFast + 1) % FILTER_SIZE_FAST;
  if (filterIndexFast == 0) filterFilledFast = true;
  int count = filterFilledFast ? FILTER_SIZE_FAST : filterIndexFast;
  long sum = 0;
  for (int i = 0; i < count; i++) sum += filterBufFast[i];
  return (int)(sum / count);
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
float currentEta = -1;
unsigned long lastUpdateMillis = 0;

const int HISTORY_SIZE = 40;
int historyBuf[HISTORY_SIZE];
int historyCount = 0;
int historyHead = 0;

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

// ---------------- OTA ----------------

void reportOtaCheckin(bool ok, const char* version) {
  if (WiFi.status() != WL_CONNECTED) return;
  HTTPClient http;
  http.setTimeout(4000);
  http.begin(OTA_CHECKIN_URL);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-Device-Key", DEVICE_KEY_VALUE);

  String payload = "{";
  payload += "\"deviceId\":\"" + String(DEVICE_ID) + "\",";
  payload += "\"version\":\"" + String(version) + "\",";
  payload += "\"ok\":" + String(ok ? "true" : "false");
  payload += "}";

  http.POST(payload);
  http.end();
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
  size_t written = Update.writeStream(*stream);
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
    delay(300);
    ESP.restart();
  } else {
    otaInProgress = false;
    reportOtaCheckin(false, remoteVersion.c_str());
    lcd.setCursor(0, 0);
    lcd.print("Cap nhat that bai");
    lcd.setCursor(0, 1);
    lcd.print("Van chay ban cu ");
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

  String payload = "{";
  payload += "\"deviceId\":\"" + String(DEVICE_ID) + "\",";
  payload += "\"distance\":" + String(distance) + ",";
  payload += "\"level\":\"" + String(level) + "\"";
  payload += "}";

  int httpCode = http.POST(payload);
  if (httpCode > 0) {
    Serial.printf("Telegram alert gui: %d - %s\n", httpCode, http.getString().c_str());
    lastSentLevel = level;
  } else {
    Serial.printf("Loi gui alert: %s\n", http.errorToString(httpCode).c_str());
  }

  http.end();
}

void sendLogToD1(int distance, const char* level) {
  if (WiFi.status() != WL_CONNECTED) return;

  bool changedEnough = abs(distance - lastLoggedDistance) >= LOG_CHANGE_THRESHOLD;
  bool levelChanged = lastLoggedLevel != level;
  bool heartbeatDue = (millis() - lastLogTime) >= LOG_HEARTBEAT_MS;

  if (!changedEnough && !levelChanged && !heartbeatDue) return;

  HTTPClient http;
  http.setTimeout(4000);
  http.begin(LOG_WORKER_URL);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-Device-Key", DEVICE_KEY_VALUE);

  String payload = "{";
  payload += "\"deviceId\":\"" + String(DEVICE_ID) + "\",";
  payload += "\"distance\":" + String(distance) + ",";
  payload += "\"rate\":" + String(currentRateCmPerMin, 2) + ",";
  payload += "\"level\":\"" + String(level) + "\"";
  payload += "}";

  int httpCode = http.POST(payload);
  if (httpCode > 0) {
    lastLoggedDistance = distance;
    lastLoggedLevel = level;
    lastLogTime = millis();
    Serial.printf("Da ghi D1: %d - %s\n", httpCode, http.getString().c_str());
  } else {
    Serial.printf("Loi ghi D1: %s\n", http.errorToString(httpCode).c_str());
  }

  http.end();
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
    sendTelegramAlert(distance, "danger");
  }
  else if (distance > DANGER_THRESHOLD && distance <= WARN_THRESHOLD) {
    lcd.print("Muc trung binh  ");
    digitalWrite(LED_RED, LOW);
    digitalWrite(LED_YELLOW, HIGH);
    digitalWrite(LED_GREEN, LOW);
    buzzerStop();
    sendTelegramAlert(distance, "warn");
  }
  else if (distance > WARN_THRESHOLD && distance <= DETECT_THRESHOLD) {
    lcd.print("Phat hien nuoc  ");
    digitalWrite(LED_RED, LOW);
    digitalWrite(LED_YELLOW, HIGH);
    digitalWrite(LED_GREEN, LOW);
    buzzerStop();
    sendTelegramAlert(distance, "detect");
  }
  else if (distance > DETECT_THRESHOLD) {
    lcd.print("Muc an toan     ");
    digitalWrite(LED_RED, LOW);
    digitalWrite(LED_YELLOW, LOW);
    digitalWrite(LED_GREEN, HIGH);
    buzzerStop();
    sendTelegramAlert(distance, "safe");
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
  sendTelegramAlert(-1, "error");
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
</style>
</head>
<body>
<div class="card">
  <h1><span>Giám Sát Mực Nước</span><span id="conn" class="conn">Đang kết nối...</span></h1>
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

function classify(d){
  if(d<0) return {color:'#94a3b8', label:'Cảm biến lỗi'};
  if(d<=DANGER) return {color:'#ef4444', label:'NGUY HIỂM (GẦN TRÀN)'};
  if(d<=WARN) return {color:'#eab308', label:'MỨC TRUNG BÌNH'};
  return {color:'#22c55e', label:'AN TOÀN'};
}

function drawChart(history){
  const w = canvas.width = canvas.clientWidth * devicePixelRatio;
  const h = canvas.height = canvas.clientHeight * devicePixelRatio;
  ctx.clearRect(0,0,w,h);
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
  ctx.strokeStyle = '#38bdf8';
  ctx.lineWidth = 2*devicePixelRatio;
  ctx.stroke();
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
    const arr = await res.json();
    drawChart(arr);
  }catch(e){}
}

poll();
pollHistory();
setInterval(poll, 600);
setInterval(pollHistory, 2000);
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
  String json = "{";
  json += "\"distance\":" + String(currentDistance) + ",";
  json += "\"rate\":" + String(currentRateCmPerMin, 2) + ",";
  json += "\"eta\":" + (currentEta >= 0 ? String(currentEta, 1) : "null") + ",";
  json += "\"timestamp\":" + String(lastUpdateMillis);
  json += "}";
  server.send(200, "application/json", json);
}

void handleHistory() {
  addCorsHeaders();
  String json = "[";
  for (int i = 0; i < historyCount; i++) {
    int idx = (historyHead - historyCount + i + HISTORY_SIZE) % HISTORY_SIZE;
    json += String(historyBuf[idx]);
    if (i < historyCount - 1) json += ",";
  }
  json += "]";
  server.send(200, "application/json", json);
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

  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP("ESP32_WaterLevel", "12345678", 1, 0, 4);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.println("\nDang ket noi vao WiFi nha...");
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 10000) {
    delay(300);
    Serial.print(".");
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
    reportOtaCheckin(true, FIRMWARE_VERSION);
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
  server.begin();
  Serial.println("Web server da khoi dong.");

  lastRead = millis();
}

void loop() {
  server.handleClient();

  if (!otaInProgress && millis() - lastSettingsFetch >= SETTINGS_FETCH_INTERVAL_MS) {
    lastSettingsFetch = millis();
    fetchThresholdsFromServer();
  }

  if (!otaInProgress && millis() - lastOtaCheck >= OTA_CHECK_INTERVAL_MS) {
    lastOtaCheck = millis();
    checkForOta();
  }

  if (otaInProgress) return; 

  if (millis() - lastRead >= READ_INTERVAL) {
    lastRead = millis();

    int raw = measureDistanceRaw();
    if (raw < 0) {
      currentDistance = -1;
      showSensorError();
      sendLogToD1(-1, "error");
    } else {
      currentDistance = pushAndSmoothFast(raw);           
      int stableDistance = pushAndSmoothStable(raw);  
      updateRate(currentDistance);
      currentEta = estimateMinutesToDanger(currentDistance);
      lastUpdateMillis = millis();

      updateOutputs(currentDistance, currentEta);
      pushHistory(currentDistance);
      sendLogToD1(stableDistance, levelFromDistance(stableDistance));
    }
  }
}

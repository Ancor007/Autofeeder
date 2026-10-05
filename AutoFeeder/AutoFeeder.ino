/*
 * AutoFeeder — автоматическая кормушка для домашних животных
 * Платформа: ESP8266 (Wemos D1 mini), шаговый двигатель 28BYJ-48 + ULN2003
 * Управление: веб-интерфейс, время по NTP (UTC+4), настройки в EEPROM
 *
 * Авторы: Н.А. Глебов, Е.А. Салюков (СамГТУ, 4-ИАИТ-103)
 * Руководитель: О.В. Прохорова
 * Лицензия: MIT
 */

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <EEPROM.h>
#include <time.h>

// ===== Настройки шагового мотора через драйвер =====
const int drvPins[4] = { D5, D6, D7, D8 };
const int STEPS_FRW = 100;
const int STEPS_BKW = 40;
const unsigned long FEED_SPEED = 10;

int feedStep = 0;
bool feedForward = true;
bool feeding = false;
unsigned long feedMillis = 0;
int portionsLeft = 0;

// ===== RTC =====
struct RTC {
  int Hours, minutes, seconds;
  void gettime() {
    time_t now = time(nullptr);
    struct tm *tmPtr = localtime(&now);
    Hours = tmPtr->tm_hour;
    minutes = tmPtr->tm_min;
    seconds = tmPtr->tm_sec;
  }
} rtc;

String last_feed = "Нет данных";

// ===== EEPROM =====
#define EEPROM_SIZE 128
#define WIFI_FLAG_ADDR 0
#define SSID_ADDR 1
#define PASS_ADDR 33
#define TIMER_ADDR 65
#define TIMER_BLOCK_SIZE 4
#define MANUAL_PORTION_ADDR (TIMER_ADDR + 4 * 4)
#define MULTIPLIER_ADDR (TIMER_ADDR + 4 * 4 + 1)

struct FeedTimer {
  int hour;
  int minute;
  int portion;
  bool enabled;
};

FeedTimer timers[4];
int manualFeedPortion = 1;
int globalPortionMultiplier = 1;
int lastTrigMinute[4] = { -1, -1, -1, -1 };

// ===== Web Server =====
ESP8266WebServer server(80);
bool inAPmode = false;

// ===== EEPROM functions =====
void saveWiFiConfig(const String &ssid, const String &pass) {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.write(WIFI_FLAG_ADDR, 1);
  EEPROM.write(SSID_ADDR, ssid.length());
  for (int i = 0; i < ssid.length() && (SSID_ADDR + 1 + i) < PASS_ADDR; i++) EEPROM.write(SSID_ADDR + 1 + i, ssid[i]);
  EEPROM.write(PASS_ADDR, pass.length());
  for (int i = 0; i < pass.length() && (PASS_ADDR + 1 + i) < TIMER_ADDR; i++) EEPROM.write(PASS_ADDR + 1 + i, pass[i]);
  EEPROM.commit();
  EEPROM.end();
}

bool loadWiFiConfig(String &ssid, String &pass) {
  EEPROM.begin(EEPROM_SIZE);
  if (EEPROM.read(WIFI_FLAG_ADDR) != 1) {
    EEPROM.end();
    return false;
  }
  int ssidLen = EEPROM.read(SSID_ADDR);
  ssid = "";
  for (int i = 0; i < ssidLen; i++) ssid += char(EEPROM.read(SSID_ADDR + 1 + i));
  int passLen = EEPROM.read(PASS_ADDR);
  pass = "";
  for (int i = 0; i < passLen; i++) pass += char(EEPROM.read(PASS_ADDR + 1 + i));
  EEPROM.end();
  return true;
}

void loadTimers() {
  EEPROM.begin(EEPROM_SIZE);
  for (int i = 0; i < 4; i++) {
    timers[i].hour = EEPROM.read(TIMER_ADDR + i * TIMER_BLOCK_SIZE);
    timers[i].minute = EEPROM.read(TIMER_ADDR + i * TIMER_BLOCK_SIZE + 1);
    timers[i].portion = EEPROM.read(TIMER_ADDR + i * TIMER_BLOCK_SIZE + 2);
    timers[i].enabled = EEPROM.read(TIMER_ADDR + i * TIMER_BLOCK_SIZE + 3);
    timers[i].hour = constrain(timers[i].hour, 0, 23);
    timers[i].minute = constrain(timers[i].minute, 0, 59);
    timers[i].portion = constrain(timers[i].portion, 1, 20);
  }
  manualFeedPortion = EEPROM.read(MANUAL_PORTION_ADDR);
  if (manualFeedPortion < 1 || manualFeedPortion > 20) manualFeedPortion = 1;
  globalPortionMultiplier = EEPROM.read(MULTIPLIER_ADDR);
  if (globalPortionMultiplier < 1 || globalPortionMultiplier > 20) globalPortionMultiplier = 1;
  EEPROM.end();
}

void saveTimers() {
  EEPROM.begin(EEPROM_SIZE);
  for (int i = 0; i < 4; i++) {
    EEPROM.write(TIMER_ADDR + i * TIMER_BLOCK_SIZE, timers[i].hour);
    EEPROM.write(TIMER_ADDR + i * TIMER_BLOCK_SIZE + 1, timers[i].minute);
    EEPROM.write(TIMER_ADDR + i * TIMER_BLOCK_SIZE + 2, timers[i].portion);
    EEPROM.write(TIMER_ADDR + i * TIMER_BLOCK_SIZE + 3, timers[i].enabled);
  }
  EEPROM.write(MANUAL_PORTION_ADDR, manualFeedPortion);
  EEPROM.write(MULTIPLIER_ADDR, globalPortionMultiplier);
  EEPROM.commit();
  EEPROM.end();
}

// ===== Wi-Fi & NTP =====
const char *ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 14400;
const int daylightOffset_sec = 0;

bool connectToWiFi(const String &ssid, const String &pass, int retries = 20) {
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), pass.c_str());
  Serial.print("Подключение к Wi-Fi: ");
  Serial.println(ssid);
  int count = 0;
  while (WiFi.status() != WL_CONNECTED && count < retries) {
    delay(500);
    Serial.print(".");
    count++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWi-Fi подключен! IP: " + WiFi.localIP().toString());
    return true;
  }
  Serial.println("\nНе удалось подключиться к Wi-Fi");
  return false;
}

void setupWiFiAndTime() {
  String ssid, pass;
  inAPmode = true;
  if (loadWiFiConfig(ssid, pass) && connectToWiFi(ssid, pass, 20)) {
    inAPmode = false;
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
    Serial.println("Ожидание NTP...");
    unsigned long t0 = millis();
    while (millis() - t0 < 5000) {
      if (time(nullptr) > 100000) {
        Serial.println("Время получено!");
        return;
      }
      delay(200);
    }
    Serial.println("NTP таймаут, продолжаем без него.");
  } else {
    inAPmode = true;
    WiFi.mode(WIFI_AP);
    WiFi.softAP("AutoFeeder_Setup", "12345678");
    Serial.println("Создана точка доступа AP! IP: " + WiFi.softAPIP().toString());
  }
}

bool getLocalTimeSafe(struct tm &info) {
  if (inAPmode) return false;
  time_t now = time(nullptr);
  if (now < 100000) return false;
  info = *localtime(&now);
  return true;
}

// ===== Feed functions =====
void startFeed(int portions) {
  if (feeding) return;
  portionsLeft = portions * globalPortionMultiplier;
  if (portionsLeft < 1) portionsLeft = globalPortionMultiplier;
  feeding = true;
  feedForward = true;
  feedStep = 0;
  feedMillis = millis();
  Serial.print("Запуск кормления, циклов: ");
  Serial.println(portionsLeft);
}

void updateFeed() {
  if (!feeding) return;
  if (millis() - feedMillis < FEED_SPEED) return;
  feedMillis = millis();
  if (feedForward) {
    if (feedStep < STEPS_FRW) {
      for (int i = 0; i < 4; i++) digitalWrite(drvPins[i], (i == feedStep % 4) ? HIGH : LOW);
      feedStep++;
    } else {
      feedForward = false;
      feedStep = 0;
    }
  } else {
    if (feedStep < STEPS_BKW) {
      for (int i = 0; i < 4; i++) digitalWrite(drvPins[i], (i == (3 - feedStep % 4)) ? HIGH : LOW);
      feedStep++;
    } else {
      portionsLeft--;
      if (portionsLeft > 0) {
        feedForward = true;
        feedStep = 0;
      } else {
        feeding = false;
        rtc.gettime();
        last_feed = String(rtc.Hours) + ":" + (rtc.minutes < 10 ? "0" : "") + String(rtc.minutes);
        Serial.println("Кормление завершено.");
        for (int i = 0; i < 4; i++) digitalWrite(drvPins[i], LOW);
      }
    }
  }
}

// ===== Web Pages =====
String webPage(struct tm &t, bool hasTime) {
  String s;
  s.reserve(2500);
  s += "<!DOCTYPE html><html><head><meta charset='UTF-8'><meta name='viewport'>";
  s += "<style>body{font-family:Arial;font-size:14px;margin:10px;}h1{font-size:20px;margin:5px 0;}h2{font-size:16px;margin:8px 0;}label{display:inline-block;width:70px;}input{margin:2px 0;padding:3px;} .row{margin:4px 0;} .card{max-width:360px;}</style>";
  s += "</head><body><div class='card'><h1>Автокормушка</h1>";

  if (hasTime) s += String(t.tm_hour) + ":" + (t.tm_min < 10 ? "0" : "") + String(t.tm_min) + " " + String(t.tm_mday) + "." + String(t.tm_mon + 1) + "." + String(t.tm_year + 1900);
  else s += "<div>Время недоступно</div>";

  if (inAPmode) {
    s += "<h2>Wi-Fi</h2><form action='/setwifi'><input name='ssid' placeholder='SSID'><br><input name='pass' placeholder='Pass' type='password'><br><input type='submit' value='Сохранить'></form>";
  } else {
    s += "<form action='/clearwifi'><input type='submit' value='Сбросить Wi-Fi'></form>";
  }

  s += "<h2>Ручное</h2><form action='/feednow'><div class='row'>Порция:<br><input name='manual' type='number' min='1' max='20' value='" + String(manualFeedPortion) + "'></div><input type='submit' value='Покормить'></form>";
  s += "<h2>Коэффициент</h2><form action='/set'><input name='multiplier' type='number' min='1' max='20' value='" + String(globalPortionMultiplier) + "'><br><input type='submit' value='Сохранить'></form>";

  s += "<h2>Таймеры</h2><form action='/settimer'>";
  for (int i = 0; i < 4; i++) {
    s += "<div class='row'>#" + String(i + 1);
    s += "<input name='h" + String(i) + "' type='number' min='0' max='23' value='" + String(timers[i].hour) + "'>";
    s += "<input name='m" + String(i) + "' type='number' min='0' max='59' value='" + String(timers[i].minute) + "'>";
    s += "<input name='p" + String(i) + "' type='number' min='1' max='20' value='" + String(timers[i].portion) + "'>";
    s += "<input name='e" + String(i) + "' type='checkbox'" + (timers[i].enabled ? " checked" : "") + ">";
    s += "</div>";
  }
  s += "<input type='submit' value='Сохранить'></form>";
  s += "<p>Последняя раздача: " + last_feed + "</p>";
  s += "</div></body></html>";
  return s;
}

// ===== Handlers =====
void handleRoot() {
  struct tm t;
  bool hasTime = getLocalTimeSafe(t);
  server.send(200, "text/html", webPage(t, hasTime));
}

void handleSetWiFi() {
  String ssid = server.arg("ssid");
  String pass = server.arg("pass");
  if (ssid.length() > 0) {
    saveWiFiConfig(ssid, pass);
    server.send(200, "text/html", "<h2>Wi-Fi сохранён! Перезагрузка...</h2>");
    delay(200);
    ESP.restart();
  } else server.sendHeader("Location", "/");
  server.send(303);
}

void handleClearWiFi() {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.write(WIFI_FLAG_ADDR, 0);
  EEPROM.commit();
  EEPROM.end();
  server.send(200, "text/html", "<h2>Wi-Fi сброшен. Перезагрузка в AP...</h2>");
  delay(200);
  ESP.restart();
}

void handleFeedNow() {
  int p = server.arg("manual").toInt();
  p = constrain(p, 1, 20);
  if (p != manualFeedPortion) {
    manualFeedPortion = p;
    saveTimers();
  }
  startFeed(manualFeedPortion);
  server.sendHeader("Location", "/");
  server.send(303);
}

void handleSetMultiplier() {
  int m = server.arg("multiplier").toInt();
  globalPortionMultiplier = constrain(m, 1, 20);
  saveTimers();
  server.sendHeader("Location", "/");
  server.send(303);
}

void handleSetTimer() {
  for (int i = 0; i < 4; i++) {
    timers[i].hour = constrain(server.arg("h" + String(i)).toInt(), 0, 23);
    timers[i].minute = constrain(server.arg("m" + String(i)).toInt(), 0, 59);
    timers[i].portion = constrain(server.arg("p" + String(i)).toInt(), 1, 20);
    timers[i].enabled = server.hasArg("e" + String(i));
  }
  saveTimers();
  server.sendHeader("Location", "/");
  server.send(303);
}

// ===== Setup =====
void setup() {
  Serial.begin(115200);
  for (int i = 0; i < 4; i++) pinMode(drvPins[i], OUTPUT);
  loadTimers();
  setupWiFiAndTime();

  // Default timer if EEPROM empty
  bool emptyEEPROM = true;
  for (int i = 0; i < 4; i++)
    if (timers[i].enabled) emptyEEPROM = false;
  if (emptyEEPROM) { timers[0] = { 12, 30, 3, true }; }

  // WebServer
  server.on("/", handleRoot);
  server.on("/setwifi", handleSetWiFi);
  server.on("/clearwifi", handleClearWiFi);
  server.on("/feednow", handleFeedNow);
  server.on("/set", handleSetMultiplier);
  server.on("/settimer", handleSetTimer);
  server.begin();
  Serial.println("Сервер запущен!");
}

// ===== Loop =====
void loop() {
  static unsigned long lastTick = 0;
  unsigned long nowMs = millis();
  if (nowMs - lastTick < 50) return;
  lastTick = nowMs;

  server.handleClient();
  updateFeed();

  struct tm t;
  if (!getLocalTimeSafe(t) || inAPmode) return;
  int h = t.tm_hour;
  int m = t.tm_min;
  for (int i = 0; i < 4; i++) {
    if (!timers[i].enabled) continue;
    if (timers[i].hour == h && timers[i].minute == m) {
      if (lastTrigMinute[i] != m) {
        lastTrigMinute[i] = m;
        startFeed(timers[i].portion);
      }
    } else if (lastTrigMinute[i] == m) lastTrigMinute[i] = -1;
  }
}

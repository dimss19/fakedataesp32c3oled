#include <Arduino.h>
#include <PubSubClient.h>
#include <U8g2lib.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <Wire.h>
#include <ctype.h>

#include "rfid_tags_embedded.h"
#include "secrets.h"

namespace {
constexpr int DEFAULT_OLED_SDA_PIN = 5;
constexpr int DEFAULT_OLED_SCL_PIN = 6;
constexpr int LED_PIN = 8;  // Blue onboard LED on ESP32-C3 0.42" OLED (active LOW)

constexpr uint32_t WIFI_RETRY_INTERVAL_MS = 10000;
constexpr uint32_t MQTT_RETRY_INTERVAL_MS = 5000;
constexpr uint32_t OLED_REFRESH_MS = 250;
constexpr uint32_t STATS_INTERVAL_MS = 5000;
constexpr uint32_t MIN_SEND_INTERVAL_MS = 100;
constexpr uint32_t MAX_SEND_INTERVAL_MS = 60000;

constexpr size_t MAX_EPC_LEN = 65;
constexpr char MQTT_CLIENT_ID[] = "rfid_esp32_simulator";

// 0.42" OLED 72x40 (SSD1306 EastRising, default I2C SDA=GPIO5, SCL=GPIO6)
U8G2_SSD1306_72X40_ER_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE, DEFAULT_OLED_SCL_PIN, DEFAULT_OLED_SDA_PIN);
#if MQTT_PORT == 8883
WiFiClientSecure netClient;
#else
WiFiClient netClient;
#endif
PubSubClient mqttClient(netClient);

char deviceId[64] = "PORTAL-MLG-01";
char mqttCmdTopic[128] = MQTT_TOPIC "/cmd/PORTAL-MLG-01";
char mqttAckTopic[128] = MQTT_TOPIC "/ack/PORTAL-MLG-01";

size_t currentTagIndex = 0;
bool batchMode = false;

bool oledReady = false;
int detectedOledSda = DEFAULT_OLED_SDA_PIN;
int detectedOledScl = DEFAULT_OLED_SCL_PIN;
uint8_t detectedOledAddr = 0x3C;

bool isRunning = false;
uint32_t sendIntervalMs = 1000;  // Default 1 detik

uint32_t lastWifiAttempt = 0;
uint32_t lastMqttAttempt = 0;
uint32_t lastSendAt = 0;
uint32_t lastOledUpdateAt = 0;
uint32_t statsStartedAt = 0;
uint32_t lastLedToggleAt = 0;
uint32_t txFlashUntil = 0;
bool ledBlinkState = false;

uint32_t totalPublished = 0;
uint32_t totalFailed = 0;
char lastEpc[MAX_EPC_LEN] = "-";

bool equalsIgnoreCase(const char *a, const char *b) {
  while (*a != '\0' && *b != '\0') {
    if (tolower(static_cast<unsigned char>(*a)) != tolower(static_cast<unsigned char>(*b))) {
      return false;
    }
    ++a;
    ++b;
  }
  return *a == '\0' && *b == '\0';
}

bool startsWithIgnoreCase(const char *str, const char *prefix) {
  while (*prefix != '\0') {
    if (*str == '\0' ||
        tolower(static_cast<unsigned char>(*str)) != tolower(static_cast<unsigned char>(*prefix))) {
      return false;
    }
    ++str;
    ++prefix;
  }
  return true;
}

void initRfidConfig() {
  snprintf(deviceId, sizeof(deviceId), "%s", RFID_DEFAULT_DEVICE_ID);
  batchMode = RFID_DEFAULT_BATCH_MODE;
  currentTagIndex = 0;

  snprintf(mqttCmdTopic, sizeof(mqttCmdTopic), "%s/cmd/%s", MQTT_TOPIC, deviceId);
  snprintf(mqttAckTopic, sizeof(mqttAckTopic), "%s/ack/%s", MQTT_TOPIC, deviceId);

  Serial.printf("[RFID] Loaded %u tag(s) from rfid_tags.json (device_id=%s, mode=%s)\n",
                static_cast<unsigned int>(RFID_TAG_COUNT),
                deviceId,
                batchMode ? "batch" : "sequential");
}

void setBlueLed(bool on) {
  // Active LOW on ESP32-C3 0.42" OLED (LOW = ON, HIGH = OFF)
  digitalWrite(LED_PIN, on ? LOW : HIGH);
}

void updateStatusLed() {
  const uint32_t now = millis();
  if (now < txFlashUntil) {
    setBlueLed(false);
    return;
  }

  const bool wifiOk = WiFi.status() == WL_CONNECTED;
  const bool mqttOk = mqttClient.connected();

  if (wifiOk && mqttOk) {
    setBlueLed(true);
    return;
  }

  const uint32_t blinkPeriod = wifiOk ? 800 : 200;
  if (now - lastLedToggleAt >= blinkPeriod) {
    lastLedToggleAt = now;
    ledBlinkState = !ledBlinkState;
    setBlueLed(ledBlinkState);
  }
}

bool probeI2cPins(int sda, int scl, uint8_t &foundAddr) {
  Wire.begin(sda, scl, 100000);
  delay(20);
  const uint8_t addrs[] = {0x3C, 0x3D};
  bool found = false;
  for (uint8_t addr : addrs) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      foundAddr = addr;
      found = true;
      break;
    }
  }
  Wire.end();
  return found;
}

void initOledWithAutoDetect() {
  struct PinPair {
    int sda;
    int scl;
  };
  const PinPair candidates[] = {
      {5, 6},  // ESP32-C3 0.42" OLED standard (ABRobot / 01Space)
      {6, 5},  // Swapped variant
      {8, 9},  // ESP32-C3 default I2C
      {4, 5},
      {7, 6},
  };

  bool found = false;
  for (const auto &pair : candidates) {
    uint8_t addr = 0;
    if (probeI2cPins(pair.sda, pair.scl, addr)) {
      detectedOledSda = pair.sda;
      detectedOledScl = pair.scl;
      detectedOledAddr = addr;
      found = true;
      Serial.printf("[OLED] Detected I2C OLED at 0x%02X on SDA=GPIO%d, SCL=GPIO%d\n",
                    detectedOledAddr, detectedOledSda, detectedOledScl);
      break;
    }
  }

  if (!found) {
    Serial.printf("[OLED] WARNING: No I2C device found on probed pins; fallback SDA=GPIO%d, SCL=GPIO%d (0x3C)\n",
                  DEFAULT_OLED_SDA_PIN, DEFAULT_OLED_SCL_PIN);
  }

  u8g2.getU8x8()->pins[U8X8_PIN_I2C_DATA] = static_cast<uint8_t>(detectedOledSda);
  u8g2.getU8x8()->pins[U8X8_PIN_I2C_CLOCK] = static_cast<uint8_t>(detectedOledScl);
  u8g2.setI2CAddress(static_cast<uint8_t>(detectedOledAddr << 1));
  u8g2.setBusClock(400000);
  oledReady = u8g2.begin();
  u8g2.setPowerSave(0);
  u8g2.setContrast(255);
  Serial.printf("[OLED] u8g2.begin() => %s\n", oledReady ? "OK" : "FAILED");
}

void updateOled(bool force = false) {
  const uint32_t now = millis();
  if (!force && now - lastOledUpdateAt < OLED_REFRESH_MS) {
    return;
  }
  lastOledUpdateAt = now;

  const bool wifiOk = WiFi.status() == WL_CONNECTED;
  const bool mqttOk = mqttClient.connected();

  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_5x7_tf);

  // Row 1: Wi-Fi & MQTT status
  char line1[20];
  snprintf(line1, sizeof(line1), "W:%s M:%s", wifiOk ? "OK" : "..", mqttOk ? "OK" : "..");
  u8g2.drawStr(0, 7, line1);
  u8g2.drawHLine(0, 9, 72);

  // Row 2: RUN/STOP badge + interval
  if (isRunning) {
    u8g2.drawBox(0, 12, 23, 9);
    u8g2.setDrawColor(0);
    u8g2.drawStr(2, 19, "RUN");
    u8g2.setDrawColor(1);
  } else {
    u8g2.drawFrame(0, 12, 23, 9);
    u8g2.drawStr(2, 19, "STP");
  }

  char intervalStr[12];
  if (sendIntervalMs >= 1000 && (sendIntervalMs % 1000) == 0) {
    snprintf(intervalStr, sizeof(intervalStr), "%lus",
             static_cast<unsigned long>(sendIntervalMs / 1000));
  } else {
    snprintf(intervalStr, sizeof(intervalStr), "%lums",
             static_cast<unsigned long>(sendIntervalMs));
  }
  u8g2.drawStr(26, 19, intervalStr);

  // Row 3: Total published & tag progress
  char line3[20];
  if (batchMode) {
    snprintf(line3, sizeof(line3), "TX:%lu ALL:%u",
             static_cast<unsigned long>(totalPublished),
             static_cast<unsigned int>(RFID_TAG_COUNT));
  } else {
    const size_t shownIdx = (RFID_TAG_COUNT > 0)
                                ? ((currentTagIndex == 0 && totalPublished > 0) ? RFID_TAG_COUNT : currentTagIndex)
                                : 0;
    snprintf(line3, sizeof(line3), "TX:%lu %u/%u",
             static_cast<unsigned long>(totalPublished),
             static_cast<unsigned int>(shownIdx),
             static_cast<unsigned int>(RFID_TAG_COUNT));
  }
  u8g2.drawStr(0, 30, line3);

  // Row 4: Tail of last EPC or command hint
  char line4[16];
  const size_t epcLen = strlen(lastEpc);
  if (epcLen >= 12) {
    snprintf(line4, sizeof(line4), "..%s", lastEpc + (epcLen - 12));
  } else {
    snprintf(line4, sizeof(line4), "CMD:start/stop");
  }
  u8g2.drawStr(0, 39, line4);

  u8g2.sendBuffer();
}

void publishSingleTag(const char *epc, size_t displayIndex) {
  const uint32_t ts = millis();
  snprintf(lastEpc, sizeof(lastEpc), "%s", epc);

  if (!mqttClient.connected()) {
    ++totalFailed;
    Serial.printf("[%lus] [%u/%u] EPC: %s -> MQTT GAGAL (disconnected)\n",
                  static_cast<unsigned long>(ts / 1000),
                  static_cast<unsigned int>(displayIndex),
                  static_cast<unsigned int>(RFID_TAG_COUNT),
                  epc);
    updateOled(true);
    return;
  }

  char payload[256];
  snprintf(payload, sizeof(payload),
           "{\"device_id\":\"%s\",\"tags\":[\"%s\"]}", deviceId, epc);

  if (mqttClient.publish(MQTT_TOPIC, payload, false)) {
    ++totalPublished;
    txFlashUntil = millis() + 60;
    Serial.printf("[%lus] [%u/%u] EPC: %s -> MQTT OK\n",
                  static_cast<unsigned long>(ts / 1000),
                  static_cast<unsigned int>(displayIndex),
                  static_cast<unsigned int>(RFID_TAG_COUNT),
                  epc);
  } else {
    ++totalFailed;
    Serial.printf("[%lus] [%u/%u] EPC: %s -> MQTT GAGAL\n",
                  static_cast<unsigned long>(ts / 1000),
                  static_cast<unsigned int>(displayIndex),
                  static_cast<unsigned int>(RFID_TAG_COUNT),
                  epc);
  }

  updateOled(true);
}

void publishAllTagsBatch() {
  const uint32_t ts = millis();
  if (RFID_TAG_COUNT == 0) {
    Serial.println("[RFID] Tidak ada tag di rfid_tags.json");
    return;
  }

  snprintf(lastEpc, sizeof(lastEpc), "%s", RFID_TAGS[RFID_TAG_COUNT - 1]);

  if (!mqttClient.connected()) {
    ++totalFailed;
    Serial.printf("[%lus] [BATCH %u tags] -> MQTT GAGAL (disconnected)\n",
                  static_cast<unsigned long>(ts / 1000),
                  static_cast<unsigned int>(RFID_TAG_COUNT));
    updateOled(true);
    return;
  }

  static char payload[3584];
  size_t pos = static_cast<size_t>(
      snprintf(payload, sizeof(payload), "{\"device_id\":\"%s\",\"tags\":[", deviceId));

  size_t added = 0;
  for (size_t i = 0; i < RFID_TAG_COUNT; ++i) {
    const char *tag = RFID_TAGS[i];
    const size_t needed = strlen(tag) + 4;
    if (pos + needed + 4 >= sizeof(payload)) {
      break;
    }
    if (added > 0) {
      payload[pos++] = ',';
    }
    pos += static_cast<size_t>(
        snprintf(payload + pos, sizeof(payload) - pos, "\"%s\"", tag));
    ++added;
  }
  snprintf(payload + pos, sizeof(payload) - pos, "]}");

  if (mqttClient.publish(MQTT_TOPIC, payload, false)) {
    ++totalPublished;
    txFlashUntil = millis() + 60;
    Serial.printf("[%lus] [BATCH %u/%u tags] -> MQTT OK\n",
                  static_cast<unsigned long>(ts / 1000),
                  static_cast<unsigned int>(added),
                  static_cast<unsigned int>(RFID_TAG_COUNT));
  } else {
    ++totalFailed;
    Serial.printf("[%lus] [BATCH %u/%u tags] -> MQTT GAGAL\n",
                  static_cast<unsigned long>(ts / 1000),
                  static_cast<unsigned int>(added),
                  static_cast<unsigned int>(RFID_TAG_COUNT));
  }

  updateOled(true);
}

void sendConfiguredTags() {
  if (RFID_TAG_COUNT == 0) {
    Serial.println("[RFID] Daftar tag kosong di rfid_tags.json");
    return;
  }

  if (batchMode) {
    publishAllTagsBatch();
    return;
  }

  const size_t idx = currentTagIndex;
  currentTagIndex = (currentTagIndex + 1) % RFID_TAG_COUNT;
  publishSingleTag(RFID_TAGS[idx], idx + 1);
}

void startGenerator() {
  isRunning = true;
  lastSendAt = millis() - sendIntervalMs;
  Serial.printf("[CTRL] STARTED (mode=%s, interval=%lu ms, tags=%u)\n",
                batchMode ? "batch" : "sequential",
                static_cast<unsigned long>(sendIntervalMs),
                static_cast<unsigned int>(RFID_TAG_COUNT));
  updateOled(true);
}

void stopGenerator() {
  isRunning = false;
  Serial.println("[CTRL] STOPPED");
  updateOled(true);
}

void setSendInterval(uint32_t ms) {
  if (ms < MIN_SEND_INTERVAL_MS) {
    ms = MIN_SEND_INTERVAL_MS;
  } else if (ms > MAX_SEND_INTERVAL_MS) {
    ms = MAX_SEND_INTERVAL_MS;
  }
  sendIntervalMs = ms;
  Serial.printf("[CTRL] Interval updated to %lu ms\n", static_cast<unsigned long>(sendIntervalMs));
  updateOled(true);
}

void printTagList(bool showAll = false) {
  Serial.printf("=== Daftar RFID dari rfid_tags.json (%u tag, mode=%s) ===\n",
                static_cast<unsigned int>(RFID_TAG_COUNT),
                batchMode ? "batch" : "sequential");
  const size_t limit = (showAll || RFID_TAG_COUNT <= 25) ? RFID_TAG_COUNT : 25;
  for (size_t i = 0; i < limit; ++i) {
    Serial.printf("  %u) %s%s\n",
                  static_cast<unsigned int>(i + 1),
                  RFID_TAGS[i],
                  (!batchMode && i == currentTagIndex) ? "  <-- next" : "");
  }
  if (limit < RFID_TAG_COUNT) {
    Serial.printf("  ... (%u tag lagi; ketik 'list all' untuk tampilkan semua)\n",
                  static_cast<unsigned int>(RFID_TAG_COUNT - limit));
  }
  Serial.println("=========================================================");
}

void scanWifiNetworks() {
  Serial.println("[WIFI] Scanning nearby 2.4GHz networks...");
  const int n = WiFi.scanNetworks();
  if (n <= 0) {
    Serial.println("[WIFI] No networks found");
    return;
  }
  for (int i = 0; i < n; ++i) {
    Serial.printf("  %d) %s (RSSI=%d dBm, ch=%d)\n",
                  i + 1, WiFi.SSID(i).c_str(), WiFi.RSSI(i), WiFi.channel(i));
  }
  WiFi.scanDelete();
}

void printHelp() {
  Serial.println("=== RFID Reader Simulator (ESP32-C3 OLED + JSON) ===");
  Serial.println("  start           : Mulai kirim tag dari rfid_tags.json");
  Serial.println("  stop            : Hentikan pengiriman");
  Serial.println("  once            : Kirim 1x sekarang (tag berikutnya / batch)");
  Serial.println("  list [all]      : Tampilkan daftar tag dari rfid_tags.json");
  Serial.println("  reset           : Reset urutan kembali ke tag #1");
  Serial.println("  mode sequential : Kirim tag 1 per 1 bergiliran");
  Serial.println("  mode batch      : Kirim semua tag sekaligus dalam 1 payload");
  Serial.println("  interval <ms>   : Ubah interval kirim (100 - 60000 ms, default 1000)");
  Serial.println("  scan            : Scan jaringan Wi-Fi 2.4GHz sekitar");
  Serial.println("  status          : Tampilkan status koneksi & simulator");
  Serial.println("  help            : Tampilkan daftar perintah");
  Serial.println("====================================================");
}

void printDetailedStatus() {
  Serial.printf("[STATUS] state=%s mode=%s interval=%lu ms tags=%u next=%u total_pub=%lu total_fail=%lu last_epc=%s\n",
                isRunning ? "START" : "STOP",
                batchMode ? "batch" : "sequential",
                static_cast<unsigned long>(sendIntervalMs),
                static_cast<unsigned int>(RFID_TAG_COUNT),
                static_cast<unsigned int>(RFID_TAG_COUNT > 0 ? currentTagIndex + 1 : 0),
                static_cast<unsigned long>(totalPublished),
                static_cast<unsigned long>(totalFailed),
                lastEpc);
  Serial.printf("[OLED] ready=%d sda=GPIO%d scl=GPIO%d addr=0x%02X\n",
                oledReady ? 1 : 0, detectedOledSda, detectedOledScl, detectedOledAddr);
  Serial.printf("[NET] wifi_status=%d (%s) ip=%s rssi=%d mqtt=%s broker=%s:%u topic=%s device_id=%s client_id=%s\n",
                static_cast<int>(WiFi.status()),
                WiFi.status() == WL_CONNECTED ? "CONNECTED" : "DISCONNECTED",
                WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString().c_str() : "-",
                WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0,
                mqttClient.connected() ? "CONNECTED" : "DISCONNECTED",
                MQTT_BROKER,
                static_cast<unsigned int>(MQTT_PORT),
                MQTT_TOPIC,
                deviceId,
                MQTT_CLIENT_ID);
}

void processSerialCommand(char *rawCmd) {
  char *cmd = rawCmd;
  while (*cmd != '\0' && isspace(static_cast<unsigned char>(*cmd))) {
    ++cmd;
  }

  size_t len = strlen(cmd);
  while (len > 0 && isspace(static_cast<unsigned char>(cmd[len - 1]))) {
    cmd[--len] = '\0';
  }

  if (len == 0) {
    return;
  }

  if (equalsIgnoreCase(cmd, "start") || equalsIgnoreCase(cmd, "1")) {
    startGenerator();
    return;
  }

  if (equalsIgnoreCase(cmd, "stop") || equalsIgnoreCase(cmd, "0")) {
    stopGenerator();
    return;
  }

  if (equalsIgnoreCase(cmd, "once") || equalsIgnoreCase(cmd, "send")) {
    Serial.println("[CTRL] Sending configured RFID tag...");
    sendConfiguredTags();
    return;
  }

  if (equalsIgnoreCase(cmd, "list")) {
    printTagList(false);
    return;
  }

  if (equalsIgnoreCase(cmd, "list all")) {
    printTagList(true);
    return;
  }

  if (equalsIgnoreCase(cmd, "reset")) {
    currentTagIndex = 0;
    Serial.println("[CTRL] Urutan tag di-reset ke #1");
    updateOled(true);
    return;
  }

  if (equalsIgnoreCase(cmd, "mode sequential") || equalsIgnoreCase(cmd, "mode seq")) {
    batchMode = false;
    Serial.println("[CTRL] Mode diubah ke: sequential (1 tag per kirim)");
    updateOled(true);
    return;
  }

  if (equalsIgnoreCase(cmd, "mode batch")) {
    batchMode = true;
    Serial.println("[CTRL] Mode diubah ke: batch (semua tag sekaligus)");
    updateOled(true);
    return;
  }

  if (equalsIgnoreCase(cmd, "scan")) {
    scanWifiNetworks();
    return;
  }

  if (equalsIgnoreCase(cmd, "status")) {
    printDetailedStatus();
    return;
  }

  if (equalsIgnoreCase(cmd, "help") || equalsIgnoreCase(cmd, "?")) {
    printHelp();
    return;
  }

  if (startsWithIgnoreCase(cmd, "interval ") || startsWithIgnoreCase(cmd, "rate ")) {
    const char *arg = strchr(cmd, ' ');
    if (arg != nullptr) {
      const long val = atol(arg + 1);
      if (val > 0) {
        setSendInterval(static_cast<uint32_t>(val));
        return;
      }
    }
    Serial.println("[CTRL] Format salah. Contoh: interval 1000");
    return;
  }

  Serial.printf("[CTRL] Perintah tidak dikenal: '%s' (ketik 'help')\n", cmd);
}

void handleSerialControl() {
  static char lineBuf[64];
  static size_t lineLen = 0;

  while (Serial.available()) {
    const int ch = Serial.read();
    if (ch < 0) {
      break;
    }

    if (ch == '\r' || ch == '\n') {
      if (lineLen > 0) {
        lineBuf[lineLen] = '\0';
        processSerialCommand(lineBuf);
        lineLen = 0;
      }
      continue;
    }

    if (ch == '\b' || ch == 127) {
      if (lineLen > 0) {
        --lineLen;
      }
      continue;
    }

    if (lineLen < sizeof(lineBuf) - 1) {
      lineBuf[lineLen++] = static_cast<char>(ch);
    }
  }
}

void publishAckJson(const char *cmd, bool ok, const char *detail) {
  if (!mqttClient.connected()) {
    return;
  }
  char buf[256];
  snprintf(buf, sizeof(buf),
           "{\"device_id\":\"%s\",\"cmd\":\"%s\",\"ok\":%s,\"detail\":\"%s\"}",
           deviceId, cmd, ok ? "true" : "false", detail);
  mqttClient.publish(mqttAckTopic, buf, false);
}

void publishStatusJson() {
  if (!mqttClient.connected()) {
    return;
  }
  char buf[320];
  snprintf(buf, sizeof(buf),
           "{\"device_id\":\"%s\",\"cmd\":\"get_info\",\"ok\":true,\"mode\":\"%s\","
           "\"running\":%s,\"interval_ms\":%lu,\"tag_count\":%u,\"total_pub\":%lu,"
           "\"total_fail\":%lu,\"last_epc\":\"%s\"}",
           deviceId,
           batchMode ? "batch" : "sequential",
           isRunning ? "true" : "false",
           static_cast<unsigned long>(sendIntervalMs),
           static_cast<unsigned int>(RFID_TAG_COUNT),
           static_cast<unsigned long>(totalPublished),
           static_cast<unsigned long>(totalFailed),
           lastEpc);
  mqttClient.publish(mqttAckTopic, buf, false);
}

void onMqttMessage(char *topic, uint8_t *payload, unsigned int length) {
  if (strcmp(topic, mqttCmdTopic) != 0 || length == 0) {
    return;
  }

  char cmdBuf[128];
  const size_t copyLen = length < sizeof(cmdBuf) - 1 ? length : sizeof(cmdBuf) - 1;
  memcpy(cmdBuf, payload, copyLen);
  cmdBuf[copyLen] = '\0';

  Serial.printf("[MQTT CMD] %s\n", cmdBuf);

  if (equalsIgnoreCase(cmdBuf, "start")) {
    startGenerator();
    publishAckJson("start", true, "simulator started");
    return;
  }

  if (equalsIgnoreCase(cmdBuf, "stop")) {
    stopGenerator();
    publishAckJson("stop", true, "simulator stopped");
    return;
  }

  if (equalsIgnoreCase(cmdBuf, "once")) {
    sendConfiguredTags();
    publishAckJson("once", true, "tag sent");
    return;
  }

  if (equalsIgnoreCase(cmdBuf, "reset")) {
    currentTagIndex = 0;
    publishAckJson("reset", true, "index reset to 1");
    return;
  }

  if (equalsIgnoreCase(cmdBuf, "get_info")) {
    publishStatusJson();
    return;
  }

  int v1 = 0;
  if (sscanf(cmdBuf, "set_interval:%d", &v1) == 1 && v1 > 0) {
    setSendInterval(static_cast<uint32_t>(v1));
    publishAckJson("set_interval", true, "interval updated");
    return;
  }

  publishAckJson("unknown", false, "unknown command");
}

void onWifiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      Serial.printf("[WIFI] Connected! IP=%s RSSI=%d dBm\n",
                    WiFi.localIP().toString().c_str(), WiFi.RSSI());
      updateOled(true);
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      Serial.printf("[WIFI] Disconnected (reason=%u)\n", info.wifi_sta_disconnected.reason);
      break;
    default:
      break;
  }
}

void connectWifi() {
  if (WiFi.status() == WL_CONNECTED || millis() - lastWifiAttempt < WIFI_RETRY_INTERVAL_MS) {
    return;
  }

  lastWifiAttempt = millis();
  Serial.printf("[WIFI] Connecting to %s (tx_power=8.5dBm)...\n", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  // ESP32-C3 0.42" OLED / SuperMini ceramic antenna requires reduced TX power for stable Wi-Fi
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
}

void connectMqtt() {
  if (WiFi.status() != WL_CONNECTED || mqttClient.connected() ||
      millis() - lastMqttAttempt < MQTT_RETRY_INTERVAL_MS) {
    return;
  }

  lastMqttAttempt = millis();
  Serial.printf("[MQTT] Connecting to %s:%u as %s...\n",
                MQTT_BROKER, static_cast<unsigned int>(MQTT_PORT), MQTT_CLIENT_ID);

  bool connected = false;
  if (MQTT_USER[0] != '\0') {
    connected = mqttClient.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASSWORD);
  } else {
    connected = mqttClient.connect(MQTT_CLIENT_ID);
  }

  if (connected) {
    Serial.println("[MQTT] OK");
    mqttClient.subscribe(mqttCmdTopic);
    Serial.printf("[MQTT] Subscribed cmd topic: %s\n", mqttCmdTopic);
    updateOled(true);
  } else {
    Serial.printf("[MQTT] GAGAL (state=%d). Retry %lus...\n",
                  mqttClient.state(), static_cast<unsigned long>(MQTT_RETRY_INTERVAL_MS / 1000));
  }
}

void printPeriodicStats() {
  const uint32_t now = millis();
  if (now - statsStartedAt < STATS_INTERVAL_MS) {
    return;
  }

  Serial.printf("state=%s mode=%s total_pub=%lu total_fail=%lu interval=%lu wifi=%d mqtt=%d epc=%s\n",
                isRunning ? "RUN" : "STOP",
                batchMode ? "batch" : "seq",
                static_cast<unsigned long>(totalPublished),
                static_cast<unsigned long>(totalFailed),
                static_cast<unsigned long>(sendIntervalMs),
                WiFi.status() == WL_CONNECTED ? 1 : 0,
                mqttClient.connected() ? 1 : 0,
                lastEpc);

  statsStartedAt = now;
}
}  // namespace

void setup() {
  pinMode(LED_PIN, OUTPUT);
  // Startup 3x blink on blue LED (GPIO8) to confirm board booted cleanly
  for (int i = 0; i < 3; ++i) {
    setBlueLed(true);
    delay(120);
    setBlueLed(false);
    delay(120);
  }

  Serial.begin(115200);
  delay(1500);

  Serial.println();
  Serial.println("=== RFID Reader Simulator (MQTT + JSON Tag List) ===");
  Serial.println("Board: ESP32-C3 0.42\" OLED");
  Serial.printf("Broker: %s:%u | Topic: %s\n",
                MQTT_BROKER, static_cast<unsigned int>(MQTT_PORT), MQTT_TOPIC);

  initRfidConfig();
  printTagList(false);

  initOledWithAutoDetect();
  updateOled(true);

  WiFi.onEvent(onWifiEvent);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.setSleep(false);

#if MQTT_PORT == 8883
  netClient.setCACert(ROOT_CA_CERT);
#endif
  netClient.setNoDelay(true);
  mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
  mqttClient.setCallback(onMqttMessage);
  mqttClient.setBufferSize(4096);
  mqttClient.setKeepAlive(15);

  lastWifiAttempt = millis() - WIFI_RETRY_INTERVAL_MS;
  lastMqttAttempt = millis() - MQTT_RETRY_INTERVAL_MS;
  statsStartedAt = millis();

  Serial.println("[INFO] Status awal: STOP. Ketik 'start' + Enter di Serial Monitor untuk mulai kirim.");
  printHelp();
}

void loop() {
  handleSerialControl();
  connectWifi();

  if (WiFi.status() == WL_CONNECTED) {
    connectMqtt();
    if (mqttClient.connected()) {
      mqttClient.loop();
    }
  }

  if (isRunning && millis() - lastSendAt >= sendIntervalMs) {
    lastSendAt = millis();
    sendConfiguredTags();
  }

  updateStatusLed();
  updateOled(false);
  printPeriodicStats();
  delay(0);
}

/*
   JAFFINATOR Multi‑Tool – FINAL RELEASE
   - AP‑only (JaffAP / 12345678)
   - Web admin with inline inputs (no pop‑ups)
   - Improved TFT tool UIs (larger text, better layout)
   - All 9 tools fully functional
*/

#include <WiFi.h>
#include <esp_wifi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <BLEDevice.h>
#include <BLEScan.h>
#include <Wire.h>
#include <Adafruit_PN532.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <ctype.h>

// ===== SoftAP Credentials =====
const char* apSSID = "JaffAP";
const char* apPass = "12345678";

// ===== Hardware =====
#define TFT_CS   15
#define TFT_DC   2
#define TFT_RST  4
#define TFT_MOSI 18
#define TFT_SCK  23
#define PN532_IRQ -1
#define PN532_RST -1

Adafruit_ST7789 tft = Adafruit_ST7789(TFT_CS, TFT_DC, TFT_MOSI, TFT_SCK, TFT_RST);
Adafruit_PN532 nfc(PN532_IRQ, PN532_RST);
WebServer server(80);
DNSServer dnsServer;

bool stopFlag = false;
bool exitFlag = false;
uint32_t pktCount = 0;
uint8_t targetMAC[6] = {0};
int targetChannel = 0;
uint8_t storedUID[7];
uint8_t storedUIDLength = 0;

bool featureRunning = false;
String currentFeature = "";
bool webServerStarted = false;
String webTarget = "";

// ===== Log buffer =====
const int MAX_LOG_LINES = 50;
String logLines[MAX_LOG_LINES];
int logIndex = 0;
int logCount = 0;

void addLog(String msg) {
  // Uncomment to see logs in Serial Monitor
  // Serial.println(msg);
  logLines[logIndex] = msg;
  logIndex = (logIndex + 1) % MAX_LOG_LINES;
  if (logCount < MAX_LOG_LINES) logCount++;
}

// ===== Live data for web =====
String featureDataJson = "{}";

// -------------------- Helpers --------------------
void drawCenteredText(int y, const char* text, uint16_t color, uint8_t size) {
  int16_t x1, y1;
  uint16_t w, h;
  tft.setTextSize(size);
  tft.getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
  int x = (320 - w) / 2;
  tft.setCursor(x, y);
  tft.setTextColor(color);
  tft.print(text);
}

void drawSignalBar(int x, int y, int rssi, uint16_t color) {
  int bars = (rssi > -50) ? 4 : (rssi > -65) ? 3 : (rssi > -80) ? 2 : (rssi > -90) ? 1 : 0;
  // Wider, taller bars for better visibility
  tft.fillRect(x, y, 40, 16, ST77XX_BLACK);
  for (int i = 0; i < 4; i++) {
    if (i < bars) tft.fillRect(x + (i * 10), y + (3 - i) * 4, 8, 4, color);
    else tft.drawRect(x + (i * 10), y + (3 - i) * 4, 8, 4, 0x4208);
  }
}

String macToString(uint8_t* mac) {
  char buf[18];
  sprintf(buf, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return String(buf);
}

String getSafeInput() {
  String input = "";
  while (true) {
    while (Serial.available() > 0) {
      char c = Serial.read();
      if (c == '\n' || c == '\r') {
        input.trim();
        if (input.length() > 0) return input;
        input = "";
      } else input += c;
    }
    if (webServerStarted) { server.handleClient(); dnsServer.processNextRequest(); }
    delay(10);
    if (Serial.available() && Serial.peek() == 'm') {
      Serial.read();
      return "m";
    }
  }
}

// ===== Ultra‑fast TFT Home Screen =====
void drawMenu() {
  tft.fillScreen(ST77XX_BLACK);
  tft.fillRect(0, 0, 320, 6, ST77XX_GREEN);
  tft.fillRect(0, 234, 320, 6, ST77XX_GREEN);
  tft.setTextSize(4);
  drawCenteredText(70, "JAFFINATOR", ST77XX_GREEN, 4);
  tft.setTextSize(2);
  tft.setTextColor(0x7BEF);
  drawCenteredText(130, "wireless toolkit", 0x7BEF, 2);
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_CYAN);
  tft.setCursor(10, 210);
  tft.print("AP: "); tft.print(WiFi.softAPIP());
}

// ==================== NFC Magic Card Helpers ====================
bool gen1aUnlock() {
  uint8_t cmd1[] = { 0x40 };
  uint8_t resp[16]; uint8_t respLen = sizeof(resp);
  if (!nfc.inDataExchange(cmd1, 1, resp, &respLen)) return false;
  uint8_t cmd2[] = { 0x43 };
  respLen = sizeof(resp);
  if (!nfc.inDataExchange(cmd2, 1, resp, &respLen)) return false;
  return true;
}

bool gen1aWriteBlock(uint8_t blockNumber, uint8_t *data16) {
  uint8_t cmd[18];
  cmd[0] = 0xA0; cmd[1] = blockNumber;
  memcpy(cmd + 2, data16, 16);
  uint8_t resp[16]; uint8_t respLen = sizeof(resp);
  return nfc.inDataExchange(cmd, 18, resp, &respLen);
}

void buildBlock0(uint8_t *uid4, uint8_t *block0) {
  memset(block0, 0xFF, 16);
  memcpy(block0, uid4, 4);
  block0[4] = uid4[0] ^ uid4[1] ^ uid4[2] ^ uid4[3];
  block0[5] = 0x08;
  block0[6] = 0x04; block0[7] = 0x00;
}

bool writeMagicUID(uint8_t *targetUID, uint8_t targetLen, uint8_t *newUID4) {
  uint8_t block0[16];
  buildBlock0(newUID4, block0);
  if (gen1aUnlock()) {
    if (gen1aWriteBlock(0, block0)) {
      addLog("Write succeeded via Gen1A backdoor");
      return true;
    }
  }
  uint8_t keyA[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
  uint8_t keyB[6] = {0x00,0x00,0x00,0x00,0x00,0x00};
  bool authOK = nfc.mifareclassic_AuthenticateBlock(targetUID, targetLen, 0, 0, keyA)
             || nfc.mifareclassic_AuthenticateBlock(targetUID, targetLen, 0, 1, keyB);
  if (!authOK) {
    addLog("writeMagicUID: auth failed");
    return false;
  }
  if (nfc.mifareclassic_WriteDataBlock(0, block0)) {
    addLog("Write succeeded via Gen2/CUID");
    return true;
  }
  addLog("writeMagicUID: write failed");
  return false;
}

bool verifyUID(uint8_t *expectedUID, uint8_t expectedLen) {
  for (int attempt = 0; attempt < 4; attempt++) {
    delay(400);
    uint8_t vUID[7]; uint8_t vLen;
    if (nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, vUID, &vLen, 1500)) {
      if (vLen == expectedLen) {
        bool match = true;
        for (int i = 0; i < expectedLen; i++) if (vUID[i] != expectedUID[i]) match = false;
        if (match) return true;
      }
    }
  }
  return false;
}

// ==================== Tool implementations (with improved TFT UIs) ====================

void runWiFiScan() {
  stopFlag = false; exitFlag = false;
  Serial.println("[WiFi Scan] started");
  addLog("WiFi Scan started");
  tft.fillScreen(ST77XX_BLACK);
  tft.fillRect(0, 0, 320, 40, ST77XX_GREEN);
  drawCenteredText(8, "WiFi SCAN", ST77XX_WHITE, 3);

  for (int dots = 0; dots < 5 && !stopFlag && !exitFlag; dots++) {
    tft.fillRect(0, 50, 320, 30, ST77XX_BLACK);
    tft.setCursor(10, 55); tft.setTextColor(ST77XX_CYAN); tft.setTextSize(2);
    tft.print("Scanning");
    for (int i = 0; i <= (dots % 4); i++) tft.print(".");
    if (webServerStarted) { server.handleClient(); dnsServer.processNextRequest(); }
    delay(500);
  }
  if (stopFlag || exitFlag) { tft.fillScreen(ST77XX_BLACK); return; }

  int totalNetworks = WiFi.scanNetworks();
  Serial.printf("[WiFi Scan] Found %d networks\n", totalNetworks);
  addLog("Found " + String(totalNetworks) + " networks");
  featureDataJson = "{\"type\":\"scan\",\"total\":" + String(totalNetworks) + ",\"networks\":[";
  for (int i = 0; i < totalNetworks; i++) {
    if (i > 0) featureDataJson += ",";
    featureDataJson += "{\"ssid\":\"" + WiFi.SSID(i) + "\",\"rssi\":" + String(WiFi.RSSI(i)) + ",\"ch\":" + String(WiFi.channel(i)) + "}";
  }
  featureDataJson += "]}";

  int page = 0, perPage = 5;
  int totalPages = (totalNetworks + perPage - 1) / perPage;
  if (totalPages == 0) totalPages = 1;
  unsigned long lastPageChange = 0;
  while (!stopFlag && !exitFlag) {
    if (millis() - lastPageChange > 4000 || page == 0) {
      lastPageChange = millis();
      tft.fillRect(0, 45, 320, 175, ST77XX_BLACK);
      tft.setCursor(10, 50); tft.setTextColor(ST77XX_CYAN); tft.setTextSize(1);
      tft.printf("Page %d/%d  |  %d APs", page+1, totalPages, totalNetworks);
      int y = 75;
      int start = page * perPage;
      int end = min(start + perPage, totalNetworks);
      for (int i = start; i < end; i++) {
        String ssid = WiFi.SSID(i);
        // Show more of the SSID, bigger font
        if (ssid.length() > 18) ssid = ssid.substring(0, 18) + "..";
        tft.setTextSize(2); // bigger
        tft.setCursor(10, y); tft.setTextColor(ST77XX_GREEN); tft.print(ssid);
        tft.setTextSize(1);
        tft.setCursor(10, y + 18); tft.setTextColor(ST77XX_WHITE);
        tft.printf("CH:%2d  %3d dBm", WiFi.channel(i), WiFi.RSSI(i));
        drawSignalBar(230, y + 4, WiFi.RSSI(i), ST77XX_GREEN);
        y += 34;
      }
      page = (page + 1) % totalPages;
    }
    tft.setCursor(10, 220); tft.setTextColor(ST77XX_CYAN); tft.print("Auto‑paging... [HOME]");
    if (webServerStarted) { server.handleClient(); dnsServer.processNextRequest(); }
    delay(100);
  }
  featureDataJson = "{}";
  tft.fillScreen(ST77XX_BLACK);
  Serial.println("[WiFi Scan] exited");
}

void runWiFiBeacon() {
  stopFlag = false; exitFlag = false;
  Serial.println("[Beacon Spam] started");
  addLog("Beacon Spam started");
  tft.fillScreen(ST77XX_BLACK);
  tft.fillRect(0, 0, 320, 40, ST77XX_RED);
  drawCenteredText(8, "BEACON SPAM", ST77XX_WHITE, 3);
  const char* names[] = {"Islington College","Islingt0n College","Islington Co11ege","Isl1ngton College","Islington C0llege","I5lington College","Isl1ngt0n College"};
  const int numSSIDs = sizeof(names)/sizeof(names[0]);
  unsigned long count = 0;
  while (!stopFlag && !exitFlag) {
    for (int i = 0; i < numSSIDs; i++) {
      uint8_t packet[128];
      int ssidLen = strlen(names[i]);
      memset(packet, 0, sizeof(packet));
      packet[0] = 0x80; packet[1] = 0x00;
      packet[2] = 0x00; packet[3] = 0x00;
      memset(&packet[4], 0xFF, 6);
      packet[10] = 0xAA; packet[11] = 0xBB; packet[12] = 0xCC; packet[13] = 0xDD; packet[14] = 0xEE; packet[15] = 0xE0 + i;
      memcpy(&packet[16], &packet[10], 6);
      packet[22] = 0x00; packet[23] = 0x00;
      uint64_t timestamp = esp_timer_get_time();
      memcpy(&packet[24], &timestamp, 8);
      packet[32] = 0x64; packet[33] = 0x00;
      packet[34] = 0x11; packet[35] = 0x04;
      int pos = 36;
      packet[pos++] = 0x00;
      packet[pos++] = ssidLen;
      memcpy(&packet[pos], names[i], ssidLen);
      pos += ssidLen;
      packet[pos++] = 0x01;
      packet[pos++] = 8;
      uint8_t rates[] = {0x82, 0x84, 0x8b, 0x96, 0x24, 0x30, 0x48, 0x6c};
      memcpy(&packet[pos], rates, 8);
      pos += 8;
      packet[pos++] = 0x03;
      packet[pos++] = 0x01;
      packet[pos++] = 1;
      esp_wifi_80211_tx(WIFI_IF_AP, packet, pos, true);
      delay(2);
      count++;
    }
    if (count % 500 == 0) {
      tft.fillRect(10, 80, 300, 100, ST77XX_BLACK);
      tft.setCursor(10, 90); tft.setTextColor(ST77XX_GREEN); tft.setTextSize(3);
      tft.print("Beacons: "); tft.print(count);
      addLog("Beacons sent: " + String(count));
      featureDataJson = "{\"type\":\"beacon\",\"count\":" + String(count) + ",\"ssids\":[";
      for (int i = 0; i < numSSIDs; i++) {
        if (i > 0) featureDataJson += ",";
        featureDataJson += "\"" + String(names[i]) + "\"";
      }
      featureDataJson += "]}";
    }
    if (webServerStarted) { server.handleClient(); dnsServer.processNextRequest(); }
    if (Serial.available()) {
      char c = Serial.read();
      if (c == 's' || c == 'S') stopFlag = true;
    }
  }
  featureDataJson = "{}";
  tft.fillScreen(ST77XX_BLACK);
  Serial.println("[Beacon Spam] stopped");
}

void runBLEWindowsSpam() {
  stopFlag = false; exitFlag = false;
  Serial.println("[BLE Spam] started");
  BLEDevice::init("Jaffinator_Win");
  BLEAdvertising *pAdv = BLEDevice::getAdvertising();
  BLEAdvertisementData oAdv;
  uint8_t swift[19] = {0x06,0x00,0x03,0x00,0x80,'J','A','F','F','I','N','A','T','O','R',0x00,0x00,0x00,0x00};
  String manData = "";
  for (int i = 0; i < 19; i++) manData += (char)swift[i];
  oAdv.setManufacturerData(manData);
  oAdv.setFlags(0x06);
  pAdv->setAdvertisementData(oAdv);
  pAdv->setScanResponse(false);
  pAdv->setMinInterval(0x20);
  pAdv->setMaxInterval(0x20);
  tft.fillScreen(ST77XX_BLACK);
  tft.fillRect(0, 0, 320, 40, ST77XX_RED);
  drawCenteredText(8, "BLE WINDOWS SPAM", ST77XX_WHITE, 3);
  tft.setCursor(10, 70); tft.setTextColor(ST77XX_CYAN); tft.print("Cycles:");
  tft.setTextSize(3);
  pAdv->start();
  unsigned long count = 0;
  while (!stopFlag && !exitFlag) {
    delay(1000);
    count++;
    tft.fillRect(150, 60, 150, 30, ST77XX_BLACK);
    tft.setCursor(150, 65); tft.setTextColor(ST77XX_GREEN); tft.print(count);
    addLog("BLE cycles: " + String(count));
    featureDataJson = "{\"type\":\"ble_spam\",\"cycles\":" + String(count) + ",\"payload\":\"Swift Pair\"}";
    if (webServerStarted) { server.handleClient(); dnsServer.processNextRequest(); }
    if (Serial.available()) {
      char c = Serial.read();
      if (c == 's' || c == 'S') stopFlag = true;
    }
  }
  pAdv->stop();
  BLEDevice::deinit(false);
  featureDataJson = "{}";
  tft.fillScreen(ST77XX_BLACK);
  Serial.println("[BLE Spam] stopped");
}

void runBLETracker() {
  stopFlag = false; exitFlag = false;
  Serial.println("[BLE Tracker] started");
  BLEDevice::init("");
  BLEScan* pBLEScan = BLEDevice::getScan();
  pBLEScan->setActiveScan(true);
  pBLEScan->setInterval(100);
  pBLEScan->setWindow(99);
  addLog("BLE Tracker started");
  tft.fillScreen(ST77XX_BLACK);
  tft.fillRect(0, 0, 320, 36, ST77XX_BLUE);
  drawCenteredText(10, "BLE TRACKER", ST77XX_WHITE, 2);
  int scanCount = 0;
  while (!stopFlag && !exitFlag) {
    BLEScanResults* foundDevices = pBLEScan->start(3, false);
    scanCount++;
    tft.fillRect(0, 38, 320, 182, ST77XX_BLACK);
    tft.setCursor(10, 42); tft.setTextColor(ST77XX_CYAN); tft.setTextSize(1);
    tft.printf("Scan #%d  |  %d devices", scanCount, foundDevices->getCount());
    String json = "{\"type\":\"ble_track\",\"scan\":" + String(scanCount) + ",\"devices\":[";
    int maxDev = min((int)foundDevices->getCount(), 5);
    int y = 60;
    for (int i = 0; i < maxDev; i++) {
      BLEAdvertisedDevice d = foundDevices->getDevice(i);
      String name = d.getName().length() > 0 ? d.getName() : "Unknown";
      if (name.length() > 16) name = name.substring(0, 16) + "..";
      tft.setCursor(10, y); tft.setTextColor(ST77XX_WHITE); tft.setTextSize(2); tft.print(name);
      tft.setTextSize(1);
      tft.setCursor(170, y+2); tft.printf("%d dBm", d.getRSSI());
      drawSignalBar(250, y-2, d.getRSSI(), d.getRSSI() > -70 ? ST77XX_GREEN : ST77XX_RED);
      if (i > 0) json += ",";
      json += "{\"name\":\"" + name + "\",\"rssi\":" + String(d.getRSSI()) + ",\"mac\":\"" + d.getAddress().toString().c_str() + "\"}";
      y += 28;
    }
    json += "]}";
    featureDataJson = json;
    addLog("BLE scan #" + String(scanCount) + " - " + String(foundDevices->getCount()) + " devices");
    if (webServerStarted) { server.handleClient(); dnsServer.processNextRequest(); }
    if (Serial.available()) {
      char c = Serial.read();
      if (c == 's' || c == 'S') stopFlag = true;
    }
  }
  pBLEScan->stop(); BLEDevice::deinit(false);
  featureDataJson = "{}";
  tft.fillScreen(ST77XX_BLACK);
  Serial.println("[BLE Tracker] stopped");
}

// NFC Read / Clone / Manual UID – keep as before (full implementations)
void runNFCRead() {
  stopFlag = false; exitFlag = false;
  Serial.println("[NFC Read] started");
  nfc.begin(); nfc.SAMConfig();
  addLog("NFC Read started");
  tft.fillScreen(ST77XX_BLACK);
  tft.fillRect(0, 0, 320, 40, ST77XX_RED);
  drawCenteredText(8, "NFC READ", ST77XX_WHITE, 3);
  uint8_t lastUID[7]; uint8_t lastUIDLen = 0; bool cardPresent = false;
  while (!stopFlag && !exitFlag) {
    uint8_t uid[7]; uint8_t uidLen = 0;
    if (nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLen, 200)) {
      bool sameCard = (cardPresent && uidLen == lastUIDLen && memcmp(uid, lastUID, uidLen) == 0);
      if (!sameCard) {
        memcpy(lastUID, uid, uidLen); lastUIDLen = uidLen; cardPresent = true;
        memcpy(storedUID, uid, uidLen); storedUIDLength = uidLen;
        String uidHex = "";
        for (int i = 0; i < uidLen; i++) {
          if (uid[i] < 0x10) uidHex += "0";
          uidHex += String(uid[i], HEX);
          if (i < uidLen - 1) uidHex += ":";
        }
        uidHex.toUpperCase();
        Serial.printf("[NFC Read] Tag: %s\n", uidHex.c_str());
        addLog("NFC Tag: " + uidHex);
        featureDataJson = "{\"type\":\"nfc\",\"uid\":\"" + uidHex + "\",\"length\":" + String(uidLen) + "}";
        tft.fillRect(0, 50, 320, 190, ST77XX_BLACK);
        tft.setCursor(10, 60); tft.setTextColor(ST77XX_YELLOW); tft.setTextSize(2); tft.print("TAG DETECTED");
        tft.setCursor(10, 90); tft.setTextColor(ST77XX_WHITE); tft.setTextSize(3); tft.print(uidHex);
        tft.setCursor(10, 130); tft.setTextColor(ST77XX_CYAN); tft.setTextSize(2); tft.print("Len: "); tft.print(uidLen); tft.print(" bytes");
      }
    } else {
      if (cardPresent) {
        cardPresent = false;
        featureDataJson = "{}";
        tft.fillRect(0, 50, 320, 190, ST77XX_BLACK);
        tft.setCursor(10, 60); tft.setTextColor(ST77XX_CYAN); tft.setTextSize(2); tft.print("Waiting for tag...");
      }
    }
    if (webServerStarted) { server.handleClient(); dnsServer.processNextRequest(); }
    if (Serial.available()) {
      char c = toupper(Serial.read());
      if (c == 'S') { stopFlag = true; break; }
    }
    delay(30);
  }
  featureDataJson = "{}";
  tft.fillScreen(ST77XX_BLACK);
  Serial.println("[NFC Read] exited");
}

void runNFCClone() {
  // (identical to previous, kept for completeness – full code present)
  stopFlag = false; exitFlag = false;
  Serial.println("[NFC Clone] started");
  nfc.begin(); nfc.SAMConfig();
  addLog("NFC Clone started");
  tft.fillScreen(ST77XX_BLACK);
  tft.fillRect(0, 0, 320, 45, 0x780F);
  tft.setCursor(10, 8); tft.setTextColor(ST77XX_WHITE); tft.setTextSize(3); tft.print("[NFC CLONE]");
  tft.fillRect(0, 45, 320, 20, 0x1082);
  tft.setCursor(10, 50); tft.setTextColor(ST77XX_CYAN); tft.setTextSize(1); tft.print("STEP 1 of 2");
  tft.fillRect(10, 75, 300, 55, 0x0841);
  tft.drawRect(10, 75, 300, 55, ST77XX_YELLOW);
  tft.setCursor(20, 83); tft.setTextColor(ST77XX_YELLOW); tft.setTextSize(1);
  tft.print("Place ORIGINAL / SOURCE card");
  tft.setCursor(20, 97); tft.setTextColor(ST77XX_WHITE); tft.print("on the reader to scan its UID");
  tft.setCursor(20, 113); tft.setTextColor(ST77XX_CYAN); tft.print("Timeout: 10 seconds");
  tft.setCursor(10, 145); tft.setTextColor(ST77XX_GREEN); tft.setTextSize(2); tft.print("Waiting...");

  if (!nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, storedUID, &storedUIDLength, 10000)) {
    addLog("NFC Clone: Timeout - no source card");
    tft.fillScreen(ST77XX_BLACK);
    tft.fillRect(0, 0, 320, 45, ST77XX_RED);
    tft.setCursor(10, 8); tft.setTextColor(ST77XX_WHITE); tft.setTextSize(3); tft.print("[NFC CLONE]");
    tft.fillRect(30, 90, 260, 60, 0x2000); tft.drawRect(30, 90, 260, 60, ST77XX_RED);
    tft.setCursor(60, 103); tft.setTextColor(ST77XX_RED); tft.setTextSize(2); tft.print("TIMEOUT!");
    tft.setCursor(30, 130); tft.setTextColor(ST77XX_WHITE); tft.setTextSize(1); tft.print("No source tag detected.");
    delay(2000);
    tft.fillScreen(ST77XX_BLACK);
    Serial.println("[NFC Clone] failed (no source)");
    return;
  }
  String uidStr = "";
  for (int i = 0; i < storedUIDLength; i++) {
    if (storedUID[i] < 0x10) uidStr += "0";
    uidStr += String(storedUID[i], HEX);
    if (i < storedUIDLength - 1) uidStr += ":";
  }
  Serial.printf("[NFC Clone] Source UID: %s\n", uidStr.c_str());
  addLog("Source UID: " + uidStr);
  tft.fillScreen(ST77XX_BLACK);
  tft.fillRect(0, 0, 320, 45, 0x780F);
  tft.setCursor(10, 8); tft.setTextColor(ST77XX_WHITE); tft.setTextSize(3); tft.print("[NFC CLONE]");
  tft.fillRect(0, 45, 320, 20, 0x0380);
  tft.setCursor(10, 50); tft.setTextColor(ST77XX_WHITE); tft.setTextSize(1); tft.print("STEP 1 DONE  |  Source UID captured!");
  tft.setCursor(10, 75); tft.setTextColor(ST77XX_YELLOW); tft.setTextSize(1); tft.print("Source UID:");
  tft.setCursor(10, 90); tft.setTextColor(ST77XX_WHITE); tft.setTextSize(2); tft.print(uidStr);
  tft.fillRect(0, 115, 320, 12, 0x1082);
  tft.setCursor(10, 117); tft.setTextColor(ST77XX_CYAN); tft.setTextSize(1);
  tft.print(storedUIDLength * 8); tft.print("-bit  |  ISO14443A");
  tft.drawRect(10, 135, 300, 60, ST77XX_CYAN);
  tft.setCursor(20, 143); tft.setTextColor(ST77XX_CYAN); tft.setTextSize(1);
  tft.print("STEP 2: Place MAGIC / GEN2 card");
  tft.setCursor(20, 157); tft.setTextColor(ST77XX_WHITE); tft.print("Remove source card, then");
  tft.setCursor(20, 171); tft.print("place the writable target card.");
  tft.setCursor(20, 185); tft.setTextColor(ST77XX_YELLOW);
  for (int c = 3; c >= 1; c--) {
    tft.fillRect(0, 205, 320, 20, ST77XX_BLACK);
    tft.setCursor(10, 207); tft.setTextColor(ST77XX_YELLOW); tft.setTextSize(1);
    tft.print("Reading in "); tft.print(c); tft.print("...");
    delay(1000);
  }
  tft.fillRect(0, 200, 320, 40, ST77XX_BLACK);
  tft.setCursor(10, 205); tft.setTextColor(ST77XX_GREEN); tft.setTextSize(2); tft.print("Scanning...");
  uint8_t tUID[7]; uint8_t tLen;
  if (!nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, tUID, &tLen, 10000)) {
    addLog("NFC Clone: No target card");
    tft.fillScreen(ST77XX_BLACK);
    tft.fillRect(0, 0, 320, 45, ST77XX_RED);
    tft.setCursor(10, 8); tft.setTextColor(ST77XX_WHITE); tft.setTextSize(3); tft.print("[NFC CLONE]");
    tft.fillRect(30, 90, 260, 60, 0x2000); tft.drawRect(30, 90, 260, 60, ST77XX_RED);
    tft.setCursor(30, 103); tft.setTextColor(ST77XX_RED); tft.setTextSize(2); tft.print("NO CARD!");
    tft.setCursor(30, 130); tft.setTextColor(ST77XX_WHITE); tft.setTextSize(1); tft.print("No target card detected.");
    delay(2000);
    tft.fillScreen(ST77XX_BLACK);
    Serial.println("[NFC Clone] failed (no target)");
    return;
  }
  String tStr = "";
  for (int i = 0; i < tLen; i++) {
    if (tUID[i] < 0x10) tStr += "0"; tStr += String(tUID[i], HEX);
    if (i < tLen - 1) tStr += ":";
  }
  addLog("Target UID: " + tStr);
  tft.fillScreen(ST77XX_BLACK);
  tft.fillRect(0, 0, 320, 45, 0x780F);
  tft.setCursor(10, 8); tft.setTextColor(ST77XX_WHITE); tft.setTextSize(3); tft.print("[NFC CLONE]");
  tft.fillRect(0, 45, 320, 20, 0x1082);
  tft.setCursor(10, 50); tft.setTextColor(ST77XX_CYAN); tft.setTextSize(1); tft.print("STEP 2  |  Writing to target...");
  tft.setCursor(10, 78); tft.setTextColor(ST77XX_YELLOW); tft.setTextSize(1); tft.print("Source UID:"); tft.setCursor(10, 90); tft.setTextColor(ST77XX_WHITE); tft.setTextSize(1); tft.print(uidStr);
  tft.setCursor(10, 108); tft.setTextColor(ST77XX_YELLOW); tft.setTextSize(1); tft.print("Target UID:"); tft.setCursor(10, 120); tft.setTextColor(ST77XX_WHITE); tft.setTextSize(1); tft.print(tStr);

  if (storedUIDLength != 4) {
    addLog("NFC Clone: source UID not 4 bytes");
    tft.fillScreen(ST77XX_BLACK);
    tft.fillRect(0, 0, 320, 45, ST77XX_RED);
    tft.setCursor(10, 8); tft.setTextColor(ST77XX_WHITE); tft.setTextSize(3); tft.print("[NFC CLONE]");
    tft.fillRect(30, 90, 260, 60, 0x2000); tft.drawRect(30, 90, 260, 60, ST77XX_RED);
    tft.setCursor(40, 103); tft.setTextColor(ST77XX_RED); tft.setTextSize(2); tft.print("UNSUPPORTED");
    tft.setCursor(30, 130); tft.setTextColor(ST77XX_WHITE); tft.setTextSize(1); tft.print("Only 4-byte UID source cards");
    tft.setCursor(30, 143); tft.print("can be cloned.");
    delay(2500);
    tft.fillScreen(ST77XX_BLACK);
    Serial.println("[NFC Clone] failed (unsupported UID length)");
    return;
  }

  bool wrote = writeMagicUID(tUID, tLen, storedUID);
  if (wrote) {
    bool verified = verifyUID(storedUID, storedUIDLength);
    if (verified) {
      addLog("Clone SUCCESS! UID: " + uidStr);
      tft.fillScreen(ST77XX_BLACK);
      tft.fillRect(0, 0, 320, 45, 0x780F);
      tft.setCursor(10, 8); tft.setTextColor(ST77XX_WHITE); tft.setTextSize(3); tft.print("[NFC CLONE]");
      tft.fillRect(0, 45, 320, 20, 0x0380);
      tft.setCursor(10, 50); tft.setTextColor(ST77XX_WHITE); tft.setTextSize(1); tft.print("COMPLETE  |  UID matched & verified");
      tft.fillRect(30, 80, 260, 50, 0x0200); tft.drawRect(30, 80, 260, 50, ST77XX_GREEN);
      tft.setCursor(70, 92); tft.setTextColor(ST77XX_GREEN); tft.setTextSize(3); tft.print("SUCCESS");
      tft.setCursor(10, 145); tft.setTextColor(ST77XX_YELLOW); tft.setTextSize(1); tft.print("Written UID:"); tft.setCursor(10, 158); tft.setTextColor(ST77XX_WHITE); tft.print(uidStr);
      featureDataJson = "{\"type\":\"nfc_clone\",\"status\":\"success\",\"uid\":\"" + uidStr + "\"}";
      Serial.printf("[NFC Clone] success: %s\n", uidStr.c_str());
    } else {
      addLog("Clone write done but verify inconclusive");
      tft.fillScreen(ST77XX_BLACK);
      tft.fillRect(0, 0, 320, 45, 0x780F);
      tft.setCursor(10, 8); tft.setTextColor(ST77XX_WHITE); tft.setTextSize(3); tft.print("[NFC CLONE]");
      tft.fillRect(0, 45, 320, 20, 0x2000);
      tft.setCursor(10, 50); tft.setTextColor(ST77XX_WHITE); tft.setTextSize(1); tft.print("Written, but verify inconclusive");
      tft.fillRect(30, 80, 260, 50, 0x2000); tft.drawRect(30, 80, 260, 50, ST77XX_RED);
      tft.setCursor(25, 92); tft.setTextColor(ST77XX_RED); tft.setTextSize(2); tft.print("RECHECK");
      tft.setCursor(10, 145); tft.setTextColor(ST77XX_WHITE); tft.setTextSize(1); tft.print("Remove card, re-tap to confirm.");
      featureDataJson = "{\"type\":\"nfc_clone\",\"status\":\"verify_fail\",\"uid\":\"" + uidStr + "\"}";
      Serial.println("[NFC Clone] verify inconclusive");
    }
  } else {
    addLog("NFC Clone: Write failed");
    tft.fillScreen(ST77XX_BLACK);
    tft.fillRect(0, 0, 320, 45, ST77XX_RED);
    tft.setCursor(10, 8); tft.setTextColor(ST77XX_WHITE); tft.setTextSize(3); tft.print("[NFC CLONE]");
    tft.fillRect(30, 80, 260, 65, 0x2000); tft.drawRect(30, 80, 260, 65, ST77XX_RED);
    tft.setCursor(30, 93); tft.setTextColor(ST77XX_RED); tft.setTextSize(2); tft.print("WRITE FAIL");
    tft.setCursor(30, 120); tft.setTextColor(ST77XX_WHITE); tft.setTextSize(1); tft.print("Not a writable card.");
    featureDataJson = "{\"type\":\"nfc_clone\",\"status\":\"write_fail\"}";
    Serial.println("[NFC Clone] write failed");
  }
  delay(3000);
  tft.fillScreen(ST77XX_BLACK);
}

void runManualUIDUpdate() {
  stopFlag = false; exitFlag = false;
  Serial.println("[Manual UID] started");
  nfc.begin(); nfc.SAMConfig();
  addLog("Manual UID started");
  tft.fillScreen(ST77XX_BLACK);
  tft.fillRect(0, 0, 320, 45, 0x03EF);
  tft.setCursor(10, 8); tft.setTextColor(ST77XX_WHITE); tft.setTextSize(3); tft.print("[MAN. UID]");
  tft.fillRect(0, 45, 320, 20, 0x1082);
  tft.setCursor(10, 50); tft.setTextColor(ST77XX_CYAN); tft.setTextSize(1); tft.print("Mode 71  |  Manual UID Write");
  tft.drawRect(10, 75, 300, 80, ST77XX_CYAN);
  tft.setCursor(20, 83); tft.setTextColor(ST77XX_CYAN); tft.setTextSize(1); tft.print("Type UID in Serial Monitor");
  tft.setCursor(20, 97); tft.setTextColor(ST77XX_WHITE); tft.print("Format:  DE AD BE EF");
  tft.setCursor(20, 111); tft.print("     or: DE:AD:BE:EF");
  tft.setCursor(20, 125); tft.print("     or: DEADBEEF");
  tft.fillRect(10, 165, 300, 30, 0x1082);
  tft.drawRect(10, 165, 300, 30, ST77XX_YELLOW);
  tft.setCursor(20, 173); tft.setTextColor(ST77XX_YELLOW); tft.setTextSize(1); tft.print("Waiting for Serial input... 's'=stop 'm'=menu");
  bool blink = false;
  tft.setCursor(10, 205); tft.setTextColor(ST77XX_GREEN); tft.setTextSize(2); tft.print("> ");
  Serial.println("\n[Manual UID] Enter UID as hex (space, colon, or no separator).");
  Serial.println("Example: DE AD BE EF   or   DE:AD:BE:EF   or   DEADBEEF");
  Serial.print("> ");
  String input = "";
  unsigned long lastBlink = millis();
  while (true) {
    if (millis() - lastBlink > 400) {
      blink = !blink;
      tft.fillRect(26, 205, 12, 18, ST77XX_BLACK);
      tft.setCursor(26, 205); tft.setTextColor(blink ? ST77XX_GREEN : ST77XX_BLACK); tft.setTextSize(2); tft.print("_");
      lastBlink = millis();
    }
    if (Serial.available()) {
      char c = Serial.read();
      if (c == '\n' || c == '\r') { if (input.length() > 0) break; }
      else { input += c; Serial.print(c); }
    }
    if (Serial.peek() == 's' || Serial.peek() == 'm') {
      Serial.read();
      addLog("Manual UID cancelled");
      tft.fillScreen(ST77XX_BLACK);
      tft.setCursor(10, 120); tft.setTextColor(ST77XX_YELLOW); tft.setTextSize(2); tft.print("Cancelled");
      delay(1000);
      tft.fillScreen(ST77XX_BLACK);
      Serial.println("[Manual UID] cancelled");
      return;
    }
    if (webServerStarted) { server.handleClient(); dnsServer.processNextRequest(); }
    delay(10);
  }
  input.trim();
  if (input.length() == 0) {
    addLog("Manual UID: No input");
    tft.fillScreen(ST77XX_BLACK);
    tft.fillRect(0, 0, 320, 45, ST77XX_RED);
    tft.setCursor(10, 8); tft.setTextColor(ST77XX_WHITE); tft.setTextSize(3); tft.print("[MAN. UID]");
    tft.fillRect(30, 90, 260, 50, 0x2000); tft.drawRect(30, 90, 260, 50, ST77XX_RED);
    tft.setCursor(50, 103); tft.setTextColor(ST77XX_RED); tft.setTextSize(2); tft.print("NO INPUT");
    delay(2000);
    tft.fillScreen(ST77XX_BLACK);
    Serial.println("[Manual UID] no input");
    return;
  }
  uint8_t newUID[7]; int uidLen = 0;
  String token = "";
  for (int i = 0; i <= (int)input.length(); i++) {
    char c = (i < (int)input.length()) ? input[i] : ' ';
    if (c == ' ' || c == ':') {
      if (token.length() > 0 && uidLen < 7) { newUID[uidLen++] = (uint8_t)strtol(token.c_str(), NULL, 16); token = ""; }
    } else token += c;
  }
  if (uidLen == 0 && token.length() >= 2) {
    for (int i = 0; i < (int)token.length() && uidLen < 7; i += 2) {
      if (i + 1 < (int)token.length()) {
        char hex[3] = { token[i], token[i+1], '\0' };
        newUID[uidLen++] = (uint8_t)strtol(hex, NULL, 16);
      }
    }
  }
  if (uidLen == 0) {
    addLog("Manual UID: Parse failed");
    tft.fillScreen(ST77XX_BLACK);
    tft.fillRect(0, 0, 320, 45, ST77XX_RED);
    tft.setCursor(10, 8); tft.setTextColor(ST77XX_WHITE); tft.setTextSize(3); tft.print("[MAN. UID]");
    tft.fillRect(30, 90, 260, 65, 0x2000); tft.drawRect(30, 90, 260, 65, ST77XX_RED);
    tft.setCursor(30, 103); tft.setTextColor(ST77XX_RED); tft.setTextSize(2); tft.print("INVALID");
    tft.setCursor(30, 130); tft.setTextColor(ST77XX_WHITE); tft.setTextSize(1); tft.print("Could not parse hex UID.");
    tft.setCursor(30, 143); tft.print("Try: DE AD BE EF");
    delay(2000);
    tft.fillScreen(ST77XX_BLACK);
    Serial.println("[Manual UID] parse failed");
    return;
  }
  String uidStr = "";
  for (int i = 0; i < uidLen; i++) {
    if (newUID[i] < 0x10) uidStr += "0";
    uidStr += String(newUID[i], HEX);
    if (i < uidLen - 1) uidStr += ":";
  }
  Serial.printf("[Manual UID] writing UID: %s\n", uidStr.c_str());
  addLog("Manual UID to write: " + uidStr);
  tft.fillScreen(ST77XX_BLACK);
  tft.fillRect(0, 0, 320, 45, 0x03EF);
  tft.setCursor(10, 8); tft.setTextColor(ST77XX_WHITE); tft.setTextSize(3); tft.print("[MAN. UID]");
  tft.fillRect(0, 45, 320, 20, 0x0380);
  tft.setCursor(10, 50); tft.setTextColor(ST77XX_WHITE); tft.setTextSize(1); tft.print("UID parsed OK  |  "); tft.print(uidLen * 8); tft.print("-bit");
  tft.setCursor(10, 76); tft.setTextColor(ST77XX_YELLOW); tft.setTextSize(1); tft.print("UID to write:");
  tft.setCursor(10, 90); tft.setTextColor(ST77XX_WHITE); tft.setTextSize(2); tft.print(uidStr);
  tft.drawRect(10, 118, 300, 55, ST77XX_CYAN);
  tft.setCursor(20, 126); tft.setTextColor(ST77XX_CYAN); tft.setTextSize(1); tft.print("Now place MAGIC / GEN2 card");
  tft.setCursor(20, 140); tft.setTextColor(ST77XX_WHITE); tft.print("on the reader to write UID.");
  tft.setCursor(20, 154); tft.setTextColor(ST77XX_YELLOW); tft.print("Timeout: 10 seconds");
  tft.setCursor(10, 185); tft.setTextColor(ST77XX_GREEN); tft.setTextSize(2); tft.print("Scanning...");
  uint8_t tUID[7]; uint8_t tLen;
  if (!nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, tUID, &tLen, 10000)) {
    addLog("Manual UID: Target card timeout");
    tft.fillScreen(ST77XX_BLACK);
    tft.fillRect(0, 0, 320, 45, ST77XX_RED);
    tft.setCursor(10, 8); tft.setTextColor(ST77XX_WHITE); tft.setTextSize(3); tft.print("[MAN. UID]");
    tft.fillRect(30, 90, 260, 60, 0x2000); tft.drawRect(30, 90, 260, 60, ST77XX_RED);
    tft.setCursor(50, 103); tft.setTextColor(ST77XX_RED); tft.setTextSize(2); tft.print("TIMEOUT");
    tft.setCursor(30, 135); tft.setTextColor(ST77XX_WHITE); tft.setTextSize(1); tft.print("No target card detected.");
    delay(2000);
    tft.fillScreen(ST77XX_BLACK);
    Serial.println("[Manual UID] timeout");
    return;
  }
  String tStr = "";
  for (int i = 0; i < tLen; i++) {
    if (tUID[i] < 0x10) tStr += "0"; tStr += String(tUID[i], HEX);
    if (i < tLen - 1) tStr += ":";
  }
  addLog("Manual UID target: " + tStr);

  if (uidLen != 4) {
    addLog("Manual UID: only 4-byte UIDs supported");
    tft.fillScreen(ST77XX_BLACK);
    tft.fillRect(0, 0, 320, 45, ST77XX_RED);
    tft.setCursor(10, 8); tft.setTextColor(ST77XX_WHITE); tft.setTextSize(3); tft.print("[MAN. UID]");
    tft.fillRect(30, 90, 260, 60, 0x2000); tft.drawRect(30, 90, 260, 60, ST77XX_RED);
    tft.setCursor(40, 103); tft.setTextColor(ST77XX_RED); tft.setTextSize(2); tft.print("UNSUPPORTED");
    tft.setCursor(30, 130); tft.setTextColor(ST77XX_WHITE); tft.setTextSize(1); tft.print("Only 4-byte UIDs can be");
    tft.setCursor(30, 143); tft.print("written.");
    delay(2500);
    tft.fillScreen(ST77XX_BLACK);
    Serial.println("[Manual UID] unsupported length");
    return;
  }

  bool wrote = writeMagicUID(tUID, tLen, newUID);
  if (wrote) {
    bool verified = verifyUID(newUID, uidLen);
    if (verified) {
      addLog("Manual UID SUCCESS! UID: " + uidStr);
      tft.fillScreen(ST77XX_BLACK);
      tft.fillRect(0, 0, 320, 45, 0x03EF);
      tft.setCursor(10, 8); tft.setTextColor(ST77XX_WHITE); tft.setTextSize(3); tft.print("[MAN. UID]");
      tft.fillRect(0, 45, 320, 20, 0x0380);
      tft.setCursor(10, 50); tft.setTextColor(ST77XX_WHITE); tft.setTextSize(1); tft.print("COMPLETE  |  UID written & verified");
      tft.fillRect(30, 80, 260, 50, 0x0200); tft.drawRect(30, 80, 260, 50, ST77XX_GREEN);
      tft.setCursor(70, 92); tft.setTextColor(ST77XX_GREEN); tft.setTextSize(3); tft.print("SUCCESS");
      tft.setCursor(10, 145); tft.setTextColor(ST77XX_YELLOW); tft.setTextSize(1); tft.print("Written UID:"); tft.setCursor(10, 158); tft.setTextColor(ST77XX_WHITE); tft.print(uidStr);
      featureDataJson = "{\"type\":\"manual_uid\",\"status\":\"success\",\"uid\":\"" + uidStr + "\"}";
      Serial.printf("[Manual UID] success: %s\n", uidStr.c_str());
    } else {
      addLog("Manual UID write done but verify inconclusive");
      tft.fillScreen(ST77XX_BLACK);
      tft.fillRect(0, 0, 320, 45, 0x03EF);
      tft.setCursor(10, 8); tft.setTextColor(ST77XX_WHITE); tft.setTextSize(3); tft.print("[MAN. UID]");
      tft.fillRect(0, 45, 320, 20, 0x2000);
      tft.setCursor(10, 50); tft.setTextColor(ST77XX_WHITE); tft.setTextSize(1); tft.print("Written, but verify inconclusive");
      tft.fillRect(30, 80, 260, 50, 0x2000); tft.drawRect(30, 80, 260, 50, ST77XX_RED);
      tft.setCursor(25, 92); tft.setTextColor(ST77XX_RED); tft.setTextSize(2); tft.print("RECHECK");
      tft.setCursor(10, 145); tft.setTextColor(ST77XX_WHITE); tft.setTextSize(1); tft.print("Remove card, re-tap to confirm.");
      featureDataJson = "{\"type\":\"manual_uid\",\"status\":\"verify_fail\",\"uid\":\"" + uidStr + "\"}";
      Serial.println("[Manual UID] verify inconclusive");
    }
  } else {
    addLog("Manual UID write failed");
    tft.fillScreen(ST77XX_BLACK);
    tft.fillRect(0, 0, 320, 45, ST77XX_RED);
    tft.setCursor(10, 8); tft.setTextColor(ST77XX_WHITE); tft.setTextSize(3); tft.print("[MAN. UID]");
    tft.fillRect(30, 80, 260, 65, 0x2000); tft.drawRect(30, 80, 260, 65, ST77XX_RED);
    tft.setCursor(30, 93); tft.setTextColor(ST77XX_RED); tft.setTextSize(2); tft.print("WRITE FAIL");
    tft.setCursor(30, 120); tft.setTextColor(ST77XX_WHITE); tft.setTextSize(1); tft.print("Not a writable card.");
    featureDataJson = "{\"type\":\"manual_uid\",\"status\":\"write_fail\"}";
    Serial.println("[Manual UID] write failed");
  }
  delay(3000);
  tft.fillScreen(ST77XX_BLACK);
}

void runSignalTracker() {
  stopFlag = false; exitFlag = false;
  String target = webTarget;
  if (target == "") {
    Serial.println("[Signal Tracker] Enter SSID:");
    target = getSafeInput();
    if (target == "m") { tft.fillScreen(ST77XX_BLACK); drawMenu(); return; }
  }
  Serial.printf("[Signal Tracker] tracking %s\n", target.c_str());
  addLog("Signal Tracker started for: " + target);
  tft.fillScreen(ST77XX_BLACK);
  tft.fillRect(0, 0, 320, 40, ST77XX_GREEN);
  drawCenteredText(10, "SIGNAL TRACKER", ST77XX_BLACK, 2);
  tft.setCursor(10, 70); tft.setTextColor(ST77XX_WHITE); tft.setTextSize(3);
  tft.print(target);
  unsigned long lastRead = 0;
  while (!stopFlag && !exitFlag) {
    if (millis() - lastRead > 1000) {
      lastRead = millis();
      int n = WiFi.scanNetworks();
      int rssi = -100;
      for (int i = 0; i < n; i++) if (WiFi.SSID(i) == target) { rssi = WiFi.RSSI(i); break; }
      String label = rssi > -50 ? "Excellent" : rssi > -65 ? "Good" : rssi > -80 ? "Fair" : "Poor";
      tft.fillRect(0, 110, 320, 90, ST77XX_BLACK);
      tft.setCursor(10, 120); tft.setTextColor(ST77XX_GREEN); tft.setTextSize(3); tft.printf("%d dBm", rssi);
      tft.setCursor(10, 160); tft.setTextColor(ST77XX_WHITE); tft.setTextSize(2); tft.print(label);
      drawSignalBar(200, 155, rssi, ST77XX_GREEN);
      featureDataJson = "{\"type\":\"signal\",\"ssid\":\"" + target + "\",\"rssi\":" + String(rssi) + ",\"label\":\"" + label + "\"}";
      addLog("Signal: " + target + " " + String(rssi) + " dBm - " + label);
    }
    if (webServerStarted) { server.handleClient(); dnsServer.processNextRequest(); }
    delay(100);
  }
  featureDataJson = "{}";
  tft.fillScreen(ST77XX_BLACK);
  Serial.println("[Signal Tracker] exited");
}

void snifferCallback(void* buf, wifi_promiscuous_pkt_type_t type) {
  if (type == WIFI_PKT_MGMT) pktCount++;
}

void runTargetSniffer() {
  stopFlag = false; exitFlag = false;
  pktCount = 0;
  int idx = -1;
  if (webTarget != "") idx = webTarget.toInt() - 1;
  else {
    Serial.println("[Sniffer] No web target – scan & select via serial");
    int n = WiFi.scanNetworks();
    if (n == 0) { addLog("No networks found"); delay(2000); tft.fillScreen(ST77XX_BLACK); drawMenu(); return; }
    for (int i = 0; i < n && i < 10; i++) {
      Serial.printf("%d. %-20s CH:%d MAC:%s\n", i+1, WiFi.SSID(i).c_str(), WiFi.channel(i), WiFi.BSSIDstr(i).c_str());
    }
    Serial.print("Select network number (1-10): ");
    String choice = getSafeInput();
    if (choice == "m") { tft.fillScreen(ST77XX_BLACK); drawMenu(); return; }
    idx = choice.toInt() - 1;
    if (idx < 0 || idx >= n) { addLog("Invalid choice"); tft.fillScreen(ST77XX_BLACK); drawMenu(); return; }
  }
  int n = WiFi.scanNetworks();
  if (idx >= n || idx < 0) { addLog("Invalid index"); return; }
  String bssid = WiFi.BSSIDstr(idx);
  sscanf(bssid.c_str(), "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx", &targetMAC[0], &targetMAC[1], &targetMAC[2], &targetMAC[3], &targetMAC[4], &targetMAC[5]);
  targetChannel = WiFi.channel(idx);
  Serial.printf("[Sniffer] Sniffing %s CH:%d MAC:%s\n", WiFi.SSID(idx).c_str(), targetChannel, bssid.c_str());
  addLog("Sniffing: " + WiFi.SSID(idx) + " CH:" + String(targetChannel) + " MAC:" + bssid);
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_promiscuous_rx_cb(&snifferCallback);
  esp_wifi_set_channel(targetChannel, WIFI_SECOND_CHAN_NONE);
  tft.fillScreen(ST77XX_BLACK);
  tft.fillRect(0, 0, 320, 40, ST77XX_BLUE);
  drawCenteredText(10, "SNIFFER", ST77XX_WHITE, 2);
  tft.setCursor(10, 70); tft.setTextColor(ST77XX_CYAN); tft.setTextSize(2); tft.print(WiFi.SSID(idx));
  tft.setCursor(10, 110); tft.setTextColor(ST77XX_WHITE); tft.setTextSize(2); tft.print("CH:"); tft.print(targetChannel);
  unsigned long lastUpdate = 0;
  while (!stopFlag && !exitFlag) {
    if (millis() - lastUpdate > 1000) {
      lastUpdate = millis();
      tft.fillRect(0, 140, 320, 60, ST77XX_BLACK);
      tft.setCursor(10, 150); tft.setTextColor(ST77XX_GREEN); tft.setTextSize(3); tft.print("Pkts: "); tft.print(pktCount);
      featureDataJson = "{\"type\":\"sniffer\",\"packets\":" + String(pktCount) + ",\"channel\":" + String(targetChannel) + ",\"target\":\"" + WiFi.SSID(idx) + "\",\"real\":true}";
      addLog("Sniffer packets: " + String(pktCount));
    }
    if (webServerStarted) { server.handleClient(); dnsServer.processNextRequest(); }
    delay(50);
  }
  esp_wifi_set_promiscuous(false);
  featureDataJson = "{}";
  tft.fillScreen(ST77XX_BLACK);
  Serial.println("[Sniffer] exited");
}

// ==================== Web Admin (with inline inputs for Signal/Sniffer) ====================
void handleRoot() {
  server.sendHeader("Location", "/admin", true);
  server.send(302, "text/plain", "");
}

void handleAdmin() {
  String page = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<title>JAFFINATOR</title>
<style>
  @import url('https://fonts.googleapis.com/css2?family=Share+Tech+Mono&display=swap');
  :root{
    --bg:#0a0f0a; --panel:#111a11;
    --green:#39ff6a; --green-dim:#1c7a37; --green-faint:#0f3d1c;
    --amber:#ffb000; --red:#ff3b3b;
  }
  *{ box-sizing:border-box; margin:0; padding:0; }
  body{
    background:var(--bg); color:var(--green);
    font-family:'Share Tech Mono', monospace;
    min-height:100vh; display:flex; justify-content:center; align-items:center; padding:20px;
  }
  .container{
    width:100%; max-width:800px;
    background:var(--panel);
    border:1px solid var(--green-dim);
    border-radius:8px; padding:20px;
    box-shadow:0 0 30px rgba(0,255,0,0.1);
  }
  .header{
    display:flex; justify-content:space-between; align-items:center;
    border-bottom:1px solid var(--green-faint);
    padding-bottom:10px; margin-bottom:15px;
  }
  .header h1{
    font-size:28px; letter-spacing:4px;
    color:var(--green); text-shadow:0 0 10px var(--green);
  }
  .status{
    display:flex; align-items:center; gap:8px;
    font-size:14px;
  }
  .dot{
    width:10px; height:10px; border-radius:50%;
    background:var(--green-dim); display:inline-block;
  }
  .dot.live{ background:var(--green); box-shadow:0 0 8px var(--green); animation:pulse 1.5s infinite; }
  @keyframes pulse{ 0%,100%{opacity:1;} 50%{opacity:0.3;} }
  .main-panel{
    display:grid; grid-template-columns:1fr 1fr; gap:15px; margin-bottom:15px;
  }
  .live-data{
    border:1px solid var(--green-faint);
    padding:12px; min-height:150px;
    font-size:13px; overflow-y:auto;
    background:rgba(0,0,0,0.4);
  }
  .live-data::-webkit-scrollbar{ width:6px; }
  .live-data::-webkit-scrollbar-thumb{ background:var(--green-faint); }
  .console{
    border:1px solid var(--green-dim);
    background:rgba(0,0,0,0.5);
    display:flex; flex-direction:column;
  }
  .console-head{
    display:flex; justify-content:space-between;
    padding:5px 10px; border-bottom:1px solid var(--green-faint);
    font-size:11px; color:var(--green-dim);
  }
  .console-body{
    flex:1; padding:10px; overflow-y:auto; max-height:150px;
    font-size:12px; line-height:1.4;
  }
  .console-body::-webkit-scrollbar{ width:6px; }
  .console-body::-webkit-scrollbar-thumb{ background:var(--green-faint); }
  .button-grid{
    display:grid; grid-template-columns:repeat(auto-fill, minmax(100px,1fr)); gap:8px;
    margin-bottom:10px;
  }
  .btn{
    display:flex; align-items:center; justify-content:center; gap:6px;
    padding:10px 5px;
    background:transparent; border:1px solid var(--green-dim);
    color:var(--green); font-family:'Share Tech Mono', monospace;
    font-size:13px; cursor:pointer;
    transition:0.2s;
  }
  .btn svg{ width:20px; height:20px; fill:var(--green); }
  .btn:hover{ background:rgba(57,255,106,0.1); border-color:var(--green); }
  .btn.home{ color:var(--amber); border-color:var(--amber); }
  .btn.home svg{ fill:var(--amber); }
  .btn.home:hover{ background:rgba(255,176,0,0.1); }
  .btn.refresh{ color:var(--amber); border-color:var(--amber); margin-left:auto; }
  .input-group{
    display:flex; align-items:center; gap:4px;
  }
  .input-group input{
    width:100px;
    background:transparent; border:1px solid var(--green-dim);
    color:var(--green); font-family:'Share Tech Mono', monospace;
    padding:4px 6px; font-size:12px;
  }
  .footer{
    display:flex; justify-content:space-between; align-items:center;
    margin-top:10px; font-size:11px; color:var(--green-dim);
  }
</style>
</head>
<body>
<div class="container">
  <div class="header">
    <h1>JAFFINATOR</h1>
    <div class="status">
      <span class="dot" id="statusDot"></span>
      <span id="statusText">IDLE</span>
    </div>
  </div>
  <div class="main-panel">
    <div class="live-data" id="liveData">No tool running</div>
    <div class="console">
      <div class="console-head"><span>console</span><span id="lineCount">0</span></div>
      <div class="console-body" id="consoleBody"></div>
    </div>
  </div>
  <div class="button-grid">
    <button class="btn" onclick="runTool('wifi_scan')">
      <svg viewBox="0 0 24 24"><path d="M12 3C7 3 3 7 3 12s4 9 9 9 9-4 9-9-4-9-9-9zm0 2c3.9 0 7 3.1 7 7s-3.1 7-7 7-7-3.1-7-7 3.1-7 7-7zm0 3a4 4 0 100 8 4 4 0 000-8z"/></svg>
      Scan
    </button>
    <button class="btn" onclick="runTool('beacon')">
      <svg viewBox="0 0 24 24"><path d="M12 2l2 8h8l-6 4.5 2.5 7.5-6.5-5-6.5 5L8.5 14.5 2 10h8z"/></svg>
      Beacon
    </button>
    <button class="btn" onclick="runTool('ble_spam')">
      <svg viewBox="0 0 24 24"><path d="M14.5 2l3 3-1.5 1.5L13 3.5 14.5 2zm-4 0L11 3.5 8 6.5 6.5 5l4-3zm-6 6l1.5 1.5L3 13l-1-4h2.5zm14 0H21l-1 4-3-3.5L18.5 8zM12 10l4 6h-8l4-6z"/></svg>
      BLE
    </button>
    <button class="btn" onclick="runTool('ble_track')">
      <svg viewBox="0 0 24 24"><path d="M12 2C6.5 2 2 6.5 2 12s4.5 10 10 10 10-4.5 10-10S17.5 2 12 2zm0 18c-4.4 0-8-3.6-8-8s3.6-8 8-8 8 3.6 8 8-3.6 8-8 8zm0-6c-1.1 0-2-.9-2-2s.9-2 2-2 2 .9 2 2-.9 2-2 2z"/></svg>
      Track
    </button>
    <button class="btn" onclick="runTool('nfc_read')">
      <svg viewBox="0 0 24 24"><rect x="2" y="4" width="20" height="16" rx="3"/><circle cx="12" cy="12" r="3"/></svg>
      NFC Read
    </button>
    <button class="btn" onclick="runTool('nfc_clone')">
      <svg viewBox="0 0 24 24"><rect x="2" y="4" width="20" height="16" rx="3"/><path d="M16 8h-6v6h6V8z"/></svg>
      Clone
    </button>
    <button class="btn" onclick="runTool('manual_uid')">
      <svg viewBox="0 0 24 24"><path d="M20 2H4c-1.1 0-2 .9-2 2v16c0 1.1.9 2 2 2h16c1.1 0 2-.9 2-2V4c0-1.1-.9-2-2-2zm-7 5h5v2h-5V7zm0 4h5v2h-5v-2zm-6 4h11v2H7v-2z"/></svg>
      UID
    </button>
    <div class="input-group">
      <input type="text" id="signalSSID" placeholder="SSID" maxlength="20">
      <button class="btn" onclick="runSignal()">
        <svg viewBox="0 0 24 24"><path d="M3 14h2v6H3v-6zm4-3h2v9H7v-9zm4-4h2v13h-2V7zm4-3h2v16h-2V4z"/></svg>
        Signal
      </button>
    </div>
    <div class="input-group">
      <input type="number" id="snifferNet" placeholder="#" min="1" max="10">
      <button class="btn" onclick="runSniffer()">
        <svg viewBox="0 0 24 24"><path d="M12 2C6.5 2 2 6.5 2 12s4.5 10 10 10 10-4.5 10-10S17.5 2 12 2zm-2 15l-4-4 1.4-1.4L10 14.2l6.6-6.6L18 9l-8 8z"/></svg>
        Sniffer
      </button>
    </div>
    <button class="btn home" onclick="goHome()">
      <svg viewBox="0 0 24 24"><path d="M10 20v-6h4v6h5v-8h3L12 3 2 12h3v8z"/></svg>
      Home
    </button>
    <button class="btn refresh" onclick="refreshData()">
      <svg viewBox="0 0 24 24"><path d="M17.65 6.35C16.2 4.9 14.2 4 12 4c-4.4 0-8 3.6-8 8s3.6 8 8 8c3.7 0 6.8-2.6 7.7-6h-2.1c-.8 2.3-3 4-5.6 4-3.3 0-6-2.7-6-6s2.7-6 6-6c1.7 0 3.1.7 4.2 1.8L13 11h7V4l-2.35 2.35z"/></svg>
      Refresh
    </button>
  </div>
  <div class="footer">
    <span>AP 192.168.4.1</span>
    <span id="ipDisplay"></span>
  </div>
</div>
<script>
  const consoleBody = document.getElementById('consoleBody');
  const lineCountEl = document.getElementById('lineCount');
  const liveDataEl = document.getElementById('liveData');
  const statusDot = document.getElementById('statusDot');
  const statusText = document.getElementById('statusText');
  let logCache = [];

  async function fetchStatus() {
    try {
      const res = await fetch('/api/status');
      const data = await res.json();
      const status = data.status || 'Idle';
      statusText.textContent = status;
      statusDot.className = 'dot' + (status !== 'Idle' ? ' live' : '');
    } catch(e) {}
  }

  async function fetchLogs() {
    try {
      const res = await fetch('/api/logs');
      const data = await res.json();
      if (data.length !== logCache.length || data.join() !== logCache.join()) {
        consoleBody.innerHTML = '';
        data.forEach(line => {
          const d = document.createElement('div');
          d.textContent = line;
          consoleBody.appendChild(d);
        });
        logCache = data;
        lineCountEl.textContent = data.length + ' lines';
        consoleBody.scrollTop = consoleBody.scrollHeight;
      }
    } catch(e) {}
  }

  async function fetchLiveData() {
    try {
      const res = await fetch('/api/data');
      const json = await res.json();
      if (!json || !json.type) {
        liveDataEl.innerHTML = 'No tool running';
        return;
      }
      let html = '';
      if (json.type === 'scan') {
        html = `<b>WiFi Scan:</b> ${json.total} networks<br>`;
        if (json.networks) json.networks.forEach(n => {
          html += `${n.ssid}  CH:${n.ch}  ${n.rssi} dBm<br>`;
        });
      } else if (json.type === 'beacon') {
        html = `<b>Beacon:</b> ${json.count} pkts<br>`;
        if (json.ssids) html += json.ssids.join(', ');
      } else if (json.type === 'ble_spam') {
        html = `<b>BLE Spam:</b> ${json.cycles} cycles`;
      } else if (json.type === 'ble_track') {
        html = `<b>BLE Track:</b> Scan #${json.scan} - ${json.devices ? json.devices.length : 0} devs<br>`;
        if (json.devices) json.devices.forEach(d => {
          html += `${d.name || '?'}  ${d.rssi}dBm  ${d.mac}<br>`;
        });
      } else if (json.type === 'nfc') {
        html = `<b>NFC Tag:</b> ${json.uid} (${json.length}B)`;
      } else if (json.type === 'signal') {
        html = `<b>Signal:</b> ${json.ssid}  ${json.rssi}dBm  ${json.label}`;
      } else if (json.type === 'sniffer') {
        html = `<b>Sniffer:</b> ${json.packets} real pkts CH ${json.channel} (${json.target})`;
      } else html = JSON.stringify(json);
      liveDataEl.innerHTML = html;
    } catch(e) {}
  }

  function runTool(tool) {
    fetch('/api/run?tool=' + tool);
    setTimeout(() => { fetchStatus(); fetchLiveData(); }, 300);
  }

  function runSignal() {
    const ssid = document.getElementById('signalSSID').value.trim();
    if (ssid) {
      fetch('/api/run?tool=signal_tracker&target=' + encodeURIComponent(ssid));
      setTimeout(() => { fetchStatus(); fetchLiveData(); }, 300);
    }
  }

  function runSniffer() {
    const net = document.getElementById('snifferNet').value;
    if (net && net >= 1 && net <= 10) {
      fetch('/api/run?tool=sniffer&target=' + net);
      setTimeout(() => { fetchStatus(); fetchLiveData(); }, 300);
    }
  }

  function goHome() {
    fetch('/api/home');
    setTimeout(() => { fetchStatus(); liveDataEl.innerHTML = 'No tool running'; }, 300);
  }

  function refreshData() {
    fetchLogs();
    fetchLiveData();
    fetchStatus();
  }

  document.getElementById('ipDisplay').textContent = window.location.hostname;
  fetchStatus(); fetchLogs(); fetchLiveData();
  setInterval(() => { fetchStatus(); fetchLogs(); fetchLiveData(); }, 2000);
</script>
</body>
</html>
)rawliteral";
  server.send(200, "text/html", page);
}

void handleCaptivePortal() {
  server.sendHeader("Location", "/admin", true);
  server.send(302, "text/plain", "");
}

void handlePing() { server.send(200, "text/plain", "pong"); }

void handleApiLogs() {
  String json = "[";
  int start = (logCount < MAX_LOG_LINES) ? 0 : logIndex;
  for (int i = 0; i < logCount; i++) {
    int idx = (start + i) % MAX_LOG_LINES;
    if (i > 0) json += ",";
    json += "\"" + logLines[idx] + "\"";
  }
  json += "]";
  server.send(200, "application/json", json);
}

void handleApiData() { server.send(200, "application/json", featureDataJson); }

void handleApiRun() {
  String tool = server.arg("tool");
  if (tool.length() == 0) { server.send(400, "text/plain", "Missing tool"); return; }
  if (featureRunning) { server.send(409, "text/plain", "A tool is already running"); return; }
  if (server.hasArg("target")) webTarget = server.arg("target");
  else webTarget = "";
  if (tool == "wifi_scan" || tool == "beacon" || tool == "ble_spam" ||
      tool == "ble_track" || tool == "nfc_read" || tool == "nfc_clone" ||
      tool == "manual_uid" || tool == "signal_tracker" || tool == "sniffer") {
    featureRunning = true;
    currentFeature = tool;
    addLog("Web: starting " + tool + (webTarget != "" ? " target=" + webTarget : ""));
    server.send(200, "text/plain", "Started " + tool);
  } else {
    server.send(400, "text/plain", "Unknown tool");
  }
}

void handleApiStop() { stopFlag = true; addLog("Stop"); server.send(200, "text/plain", "OK"); }
void handleApiHome() { exitFlag = true; stopFlag = true; addLog("Home"); server.send(200, "text/plain", "OK"); }
void handleApiStatus() {
  String json = "{\"status\":\"" + (featureRunning ? currentFeature : "Idle") + "\"}";
  server.send(200, "application/json", json);
}

// ==================== Setup ====================
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n\n  ╔═══════════════════════════════════════╗");
  Serial.println("  ║              JAFFINATOR               ║");
  Serial.println("  ╚═══════════════════════════════════════╝\n");
  addLog("=== JAFFINATOR AP ===");

  WiFi.mode(WIFI_AP);
  WiFi.softAP(apSSID, apPass);
  IPAddress apIP = WiFi.softAPIP();
  Serial.printf("  Access Point: %s\n", apSSID);
  Serial.printf("  Password:     %s\n", apPass);
  Serial.printf("  Admin page:   http://%s/admin\n", apIP.toString().c_str());

  dnsServer.start(53, "*", apIP);
  server.on("/", HTTP_GET, handleRoot);
  server.on("/admin", HTTP_GET, handleAdmin);
  server.on("/api/run", HTTP_GET, handleApiRun);
  server.on("/api/stop", HTTP_GET, handleApiStop);
  server.on("/api/home", HTTP_GET, handleApiHome);
  server.on("/api/status", HTTP_GET, handleApiStatus);
  server.on("/api/logs", HTTP_GET, handleApiLogs);
  server.on("/api/data", HTTP_GET, handleApiData);
  server.on("/ping", HTTP_GET, handlePing);
  server.onNotFound(handleCaptivePortal);
  server.begin();
  webServerStarted = true;

  tft.init(240, 320);
  tft.setRotation(1);
  tft.invertDisplay(false);
  drawMenu();

  Serial.println("Ready. Connect to JaffAP and open 192.168.4.1/admin");
}

void loop() {
  dnsServer.processNextRequest();
  if (webServerStarted) server.handleClient();

  if (featureRunning) {
    if (currentFeature == "wifi_scan") runWiFiScan();
    else if (currentFeature == "beacon") runWiFiBeacon();
    else if (currentFeature == "ble_spam") runBLEWindowsSpam();
    else if (currentFeature == "ble_track") runBLETracker();
    else if (currentFeature == "nfc_read") runNFCRead();
    else if (currentFeature == "nfc_clone") runNFCClone();
    else if (currentFeature == "manual_uid") runManualUIDUpdate();
    else if (currentFeature == "signal_tracker") runSignalTracker();
    else if (currentFeature == "sniffer") runTargetSniffer();
    featureRunning = false;
    currentFeature = "";
    featureDataJson = "{}";
    drawMenu();
  }

  if (Serial.available()) {
    char cmd = Serial.read();
    while (Serial.available()) Serial.read();
    if (cmd == 's' || cmd == 'S') stopFlag = true;
    else if (!featureRunning) {
      if (cmd == '1') { currentFeature = "wifi_scan"; featureRunning = true; }
      else if (cmd == '2') { currentFeature = "beacon"; featureRunning = true; }
      else if (cmd == '3') { currentFeature = "ble_spam"; featureRunning = true; }
      else if (cmd == '4') { currentFeature = "ble_track"; featureRunning = true; }
      else if (cmd == '5') { currentFeature = "nfc_read"; featureRunning = true; }
      else if (cmd == '6') { currentFeature = "nfc_clone"; featureRunning = true; }
      else if (cmd == 'u') { currentFeature = "manual_uid"; featureRunning = true; }
      else if (cmd == '7') { currentFeature = "signal_tracker"; featureRunning = true; }
      else if (cmd == '8') { currentFeature = "sniffer"; featureRunning = true; }
    }
  }
}

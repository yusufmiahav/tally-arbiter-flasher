// ArduinoGraphics MUST be included before Arduino_LED_Matrix
// v2.0.1 — built-in WebSocket client, no external library required
#include "ArduinoGraphics.h"
#include "Arduino_LED_Matrix.h"
#include <WiFiS3.h>
#include <ArduinoJson.h>
#include <EEPROM.h>

/* ════════════════════════════════════════
   TALLY ARBITER — Arduino UNO R4 WiFi
   Minimal Socket.IO v2 client over raw TCP
   12×8 red LED matrix display
   ════════════════════════════════════════ */

/* ── Placeholders — patched by web flasher ── */
String networkSSID        = "TALLY_SSID_PLACEHOLDER_00000000000000000000000000000000";
String networkPass        = "TALLY_PASS_PLACEHOLDER_00000000000000000000000000000000";
String tallyarbiter_host  = "TALLY_HOST_PLACEHOLDER_000000000000000";
int    tallyarbiter_port  = 4455;
String listenerDeviceName = "TALLY_NAME_PLACEHOLDER_000000000000000";

/* ── Runtime state ── */
ArduinoLEDMatrix matrix;
WiFiClient       wsClient;
WiFiServer       webServer(80);

StaticJsonDocument<2048> BusOptions;
StaticJsonDocument<4096> Devices;
StaticJsonDocument<2048> DeviceStates;

String DeviceId   = "unassigned";
String DeviceName = "Unassigned";

bool mode_preview     = false;
bool mode_program     = false;
bool networkConnected = false;
bool socketConnected  = false;
bool pendingRegister  = false;

/* ── WebSocket framing state ── */
String  wsRxBuf    = "";
bool    wsInFrame  = false;
uint8_t wsOpcode   = 0;
size_t  wsPayLen   = 0;
size_t  wsPayRead  = 0;
String  wsPayload  = "";

/* ── Socket.IO packet types ── */
#define SIO_CONNECT    "0"
#define SIO_EVENT      "42"
#define EIO_PING       "2"
#define EIO_PONG       "3"

unsigned long lastPing      = 0;
unsigned long pingInterval  = 25000;

/* ── EEPROM layout ── */
#define EE_MAGIC  0      // 2 bytes
#define EE_SSID   2      // 64 bytes
#define EE_PASS   66     // 64 bytes
#define EE_HOST   130    // 40 bytes
#define EE_PORT   170    // 2 bytes
#define EE_LNAME  172    // 32 bytes
#define EE_DEVID  204    // 32 bytes
#define EE_SIZE   236
#define EE_MAGIC_VAL 0x4241

/* ── Log ring buffer ── */
#define LOG_LINES 10
String logBuffer[LOG_LINES];
int    logHead = 0;
void addLog(const String& s) { Serial.println(s); logBuffer[logHead++ % LOG_LINES] = s; }

/* ════════════════════════════════════════
   EEPROM
   ════════════════════════════════════════ */
void eeWriteStr(int addr, int maxLen, const String& s) {
  int len = min((int)s.length(), maxLen - 1);
  for (int i = 0; i < len; i++) EEPROM.write(addr + i, s[i]);
  EEPROM.write(addr + len, 0);
}
String eeReadStr(int addr, int maxLen) {
  String s = "";
  for (int i = 0; i < maxLen - 1; i++) {
    char c = EEPROM.read(addr + i);
    if (!c) break;
    s += c;
  }
  return s;
}
bool loadConfig() {
  uint16_t m = (EEPROM.read(EE_MAGIC) << 8) | EEPROM.read(EE_MAGIC + 1);
  if (m != EE_MAGIC_VAL) return false;
  String ss = eeReadStr(EE_SSID, 64), pw = eeReadStr(EE_PASS, 64);
  String h  = eeReadStr(EE_HOST, 40), ln = eeReadStr(EE_LNAME, 32);
  String di = eeReadStr(EE_DEVID, 32);
  int    po = (EEPROM.read(EE_PORT) << 8) | EEPROM.read(EE_PORT + 1);
  if (ss.length()) networkSSID        = ss;
  if (pw.length()) networkPass        = pw;
  if (h.length())  tallyarbiter_host  = h;
  if (po)          tallyarbiter_port  = po;
  if (ln.length()) listenerDeviceName = ln;
  if (di.length()) DeviceId           = di;
  return true;
}
void saveConfig() {
  EEPROM.write(EE_MAGIC,     (EE_MAGIC_VAL >> 8) & 0xFF);
  EEPROM.write(EE_MAGIC + 1,  EE_MAGIC_VAL & 0xFF);
  eeWriteStr(EE_SSID,  64, networkSSID);
  eeWriteStr(EE_PASS,  64, networkPass);
  eeWriteStr(EE_HOST,  40, tallyarbiter_host);
  EEPROM.write(EE_PORT,     (tallyarbiter_port >> 8) & 0xFF);
  EEPROM.write(EE_PORT + 1,  tallyarbiter_port & 0xFF);
  eeWriteStr(EE_LNAME, 32, listenerDeviceName);
  eeWriteStr(EE_DEVID, 32, DeviceId);
}

/* ════════════════════════════════════════
   MATRIX DISPLAY
   ════════════════════════════════════════ */
void matrixScroll(const char* text, int speedMs = 60) {
  matrix.beginDraw();
  matrix.stroke(0xFFFFFFFF);
  matrix.textScrollSpeed(speedMs);
  matrix.textFont(Font_5x7);
  matrix.beginText(0, 1, 0xFFFFFF);
  matrix.println(text);
  matrix.endText(SCROLL_LEFT);
  matrix.endDraw();
}
void matrixFill() {
  byte f[8][12]; memset(f, 1, sizeof(f));
  matrix.renderBitmap(f, 8, 12);
}
void matrixClear() {
  byte f[8][12]; memset(f, 0, sizeof(f));
  matrix.renderBitmap(f, 8, 12);
}
void matrixBorder() {
  byte f[8][12] = {
    {1,1,1,1,1,1,1,1,1,1,1,1},
    {1,0,0,0,0,0,0,0,0,0,0,1},
    {1,0,0,0,0,0,0,0,0,0,0,1},
    {1,0,0,0,0,0,0,0,0,0,0,1},
    {1,0,0,0,0,0,0,0,0,0,0,1},
    {1,0,0,0,0,0,0,0,0,0,0,1},
    {1,0,0,0,0,0,0,0,0,0,0,1},
    {1,1,1,1,1,1,1,1,1,1,1,1}
  };
  matrix.renderBitmap(f, 8, 12);
}
void showTallyState() {
  if (mode_program) matrixFill();
  else if (mode_preview) matrixBorder();
  else matrixClear();
}

/* ════════════════════════════════════════
   SERIAL CONFIG RECEIVER
   ════════════════════════════════════════ */
String serialLineBuf = "";
void checkSerialConfig() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n') {
      String line = serialLineBuf; serialLineBuf = "";
      line.trim();
      if (!line.startsWith("CFG:")) continue;
      String json = line.substring(4);
      StaticJsonDocument<1024> cfg;
      if (deserializeJson(cfg, json)) continue;
      if (cfg["ssid"].as<String>().length())  networkSSID        = cfg["ssid"].as<String>();
      if (cfg["pass"].as<String>().length())  networkPass        = cfg["pass"].as<String>();
      if (cfg["tahost"].as<String>().length()) tallyarbiter_host = cfg["tahost"].as<String>();
      if (cfg["taport"].as<int>())             tallyarbiter_port = cfg["taport"].as<int>();
      if (cfg["lname"].as<String>().length()) listenerDeviceName = cfg["lname"].as<String>();
      if (cfg["devid"].as<String>().length()) DeviceId           = cfg["devid"].as<String>();
      saveConfig();
      Serial.println("CFG saved. Rebooting...");
      matrixScroll(" CFG SAVED  REBOOTING ", 40);
      delay(500); NVIC_SystemReset();
    } else if (c != '\r') {
      serialLineBuf += c;
      if (serialLineBuf.length() > 1200) serialLineBuf = "";
    }
  }
}

/* ════════════════════════════════════════
   WEBSOCKET CLIENT (minimal, no library)
   ════════════════════════════════════════ */
// Base64 encode for WS handshake key
static const char b64chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
String base64Encode(const uint8_t* data, size_t len) {
  String out = "";
  for (size_t i = 0; i < len; i += 3) {
    uint8_t b0 = data[i], b1 = i+1<len?data[i+1]:0, b2 = i+2<len?data[i+2]:0;
    out += b64chars[b0 >> 2];
    out += b64chars[((b0 & 3) << 4) | (b1 >> 4)];
    out += (i+1 < len) ? b64chars[((b1 & 0xF) << 2) | (b2 >> 6)] : '=';
    out += (i+2 < len) ? b64chars[b2 & 0x3F] : '=';
  }
  return out;
}

bool wsConnect() {
  addLog("WS connecting to " + tallyarbiter_host + ":" + String(tallyarbiter_port));
  if (!wsClient.connect(tallyarbiter_host.c_str(), tallyarbiter_port)) {
    addLog("WS TCP connect failed");
    return false;
  }
  // Random 16-byte key
  uint8_t key[16];
  for (int i = 0; i < 16; i++) key[i] = random(256);
  String keyB64 = base64Encode(key, 16);

  wsClient.print("GET /socket.io/?EIO=3&transport=websocket HTTP/1.1\r\n");
  wsClient.print("Host: " + tallyarbiter_host + "\r\n");
  wsClient.print("Upgrade: websocket\r\n");
  wsClient.print("Connection: Upgrade\r\n");
  wsClient.print("Sec-WebSocket-Key: " + keyB64 + "\r\n");
  wsClient.print("Sec-WebSocket-Version: 13\r\n\r\n");

  // Read HTTP response
  unsigned long t = millis();
  String resp = "";
  while (millis() - t < 3000) {
    while (wsClient.available()) resp += (char)wsClient.read();
    if (resp.indexOf("\r\n\r\n") >= 0) break;
    delay(10);
  }
  if (resp.indexOf("101") < 0) { addLog("WS handshake failed"); wsClient.stop(); return false; }
  addLog("WS connected");
  wsPayload = ""; wsPayLen = 0; wsPayRead = 0; wsInFrame = false;
  return true;
}

// Send a WebSocket text frame (masked, as required by clients)
void wsSend(const String& msg) {
  if (!wsClient.connected()) return;
  size_t len = msg.length();
  uint8_t mask[4];
  for (int i = 0; i < 4; i++) mask[i] = random(256);

  wsClient.write(0x81); // FIN + text opcode
  if (len < 126) {
    wsClient.write((uint8_t)(0x80 | len));
  } else {
    wsClient.write((uint8_t)(0x80 | 126));
    wsClient.write((uint8_t)(len >> 8));
    wsClient.write((uint8_t)(len & 0xFF));
  }
  wsClient.write(mask, 4);
  for (size_t i = 0; i < len; i++) wsClient.write((uint8_t)(msg[i] ^ mask[i % 4]));
}

// Read one complete WebSocket frame — returns true + sets wsPayload when done
bool wsReadFrame() {
  while (wsClient.available()) {
    if (!wsInFrame) {
      if (wsClient.available() < 2) return false;
      uint8_t b0 = wsClient.read();
      uint8_t b1 = wsClient.read();
      wsOpcode  = b0 & 0x0F;
      wsPayLen  = b1 & 0x7F;
      wsPayRead = 0;
      wsPayload = "";
      if (wsPayLen == 126) {
        if (wsClient.available() < 2) return false;
        wsPayLen = ((uint16_t)wsClient.read() << 8) | wsClient.read();
      }
      wsInFrame = true;
    }
    while (wsClient.available() && wsPayRead < wsPayLen) {
      wsPayload += (char)wsClient.read();
      wsPayRead++;
    }
    if (wsPayRead >= wsPayLen) {
      wsInFrame = false;
      return true;
    }
  }
  return false;
}

/* ════════════════════════════════════════
   TALLY LOGIC
   ════════════════════════════════════════ */
String getBusTypeById(const String& busId) {
  for (JsonObject bus : BusOptions.as<JsonArray>())
    if (bus["id"].as<String>() == busId) return bus["type"].as<String>();
  return "invalid";
}
void processTallyData() {
  mode_preview = false; mode_program = false;
  for (JsonObject state : DeviceStates.as<JsonArray>()) {
    if (state["deviceId"].as<String>() != DeviceId) continue;
    String bt = getBusTypeById(state["busId"].as<String>());
    bool   ac = state["sources"].as<JsonArray>().size() > 0;
    if (bt == "preview") mode_preview = ac;
    if (bt == "program") mode_program = ac;
  }
  addLog(String("Tally: ") + (mode_program ? "PROGRAM" : mode_preview ? "PREVIEW" : "CLEAR"));
  showTallyState();
}
void SetDeviceName() {
  for (JsonObject dev : Devices.as<JsonArray>())
    if (dev["id"].as<String>() == DeviceId) { DeviceName = dev["name"].as<String>(); break; }
  addLog("Device: " + DeviceName + " (" + DeviceId + ")");
  saveConfig();
}

/* ════════════════════════════════════════
   SOCKET.IO MESSAGE HANDLER
   ════════════════════════════════════════ */
void sioEmit(const String& event, const String& payload = "") {
  String msg = payload.length()
    ? "42[\"" + event + "\"," + payload + "]"
    : "42[\"" + event + "\"]";
  addLog("TX: " + msg.substring(0, 80));
  wsSend(msg);
}

void doRegister() {
  String p = "{\"deviceId\":\"" + DeviceId + "\","
             "\"listenerType\":\"" + listenerDeviceName + "\","
             "\"canBeReassigned\":true,\"canBeFlashed\":true,\"supportsChat\":false}";
  sioEmit("listenerclient_connect", p);
  addLog("Registered: " + listenerDeviceName);
}

void handleSIOMessage(const String& raw) {
  // EIO packet types: 0=open, 2=ping, 3=pong, 4x=message
  if (raw == EIO_PING) { wsSend(EIO_PONG); return; }
  if (!raw.startsWith("42")) return;

  // Parse ["event", payload]
  int ns = raw.indexOf('"');
  if (ns < 0) return;
  int ne = raw.indexOf('"', ns + 1);
  if (ne < 0) return;
  String event = raw.substring(ns + 1, ne);
  addLog("RX: " + event);

  int ps = raw.indexOf(',', ne);
  if (ps < 0) return;
  String payloadStr = raw.substring(ps + 1);
  if (payloadStr.endsWith("]")) payloadStr.remove(payloadStr.length() - 1);
  payloadStr.trim();

  if (event == "bus_options") {
    BusOptions.clear();
    if (!deserializeJson(BusOptions, payloadStr))
      addLog("bus_options OK (" + String(BusOptions.as<JsonArray>().size()) + ")");

  } else if (event == "devices") {
    Devices.clear();
    if (!deserializeJson(Devices, payloadStr)) SetDeviceName();

  } else if (event == "device_states") {
    DeviceStates.clear();
    if (!deserializeJson(DeviceStates, payloadStr)) processTallyData();

  } else if (event == "reassign") {
    // ["reassign","oldId","newId","listenerId"]
    int pos = 0; String tokens[4]; int found = 0;
    while (found < 4 && pos < (int)raw.length()) {
      int qs = raw.indexOf('"', pos); if (qs < 0) break;
      int qe = raw.indexOf('"', qs + 1); if (qe < 0) break;
      tokens[found++] = raw.substring(qs + 1, qe);
      pos = qe + 1;
    }
    if (found >= 3) {
      DeviceId = tokens[2];
      saveConfig();
      addLog("Reassign -> " + DeviceId);
      String nm = "UNKNOWN";
      for (JsonObject d : Devices.as<JsonArray>())
        if (d["id"].as<String>() == DeviceId) { nm = d["name"].as<String>(); break; }
      matrixScroll((" NOW: " + nm + " ").c_str(), 50);
      showTallyState();
    }

  } else if (event == "flash") {
    addLog("Flash");
    bool pp = mode_preview, pg = mode_program;
    for (int i = 0; i < 3; i++) { matrixClear(); delay(150); showTallyState(); delay(150); }
    mode_preview = pp; mode_program = pg; showTallyState();
  }
}

/* ════════════════════════════════════════
   WEB SERVER
   ════════════════════════════════════════ */
void handleWebClient() {
  WiFiClient client = webServer.available();
  if (!client) return;
  unsigned long t = millis() + 1500;
  String req = "";
  while (client.connected() && millis() < t) {
    if (client.available()) { req += (char)client.read(); if (req.endsWith("\r\n\r\n")) break; }
  }

  if (req.indexOf("GET /save") >= 0) {
    auto dec = [](String s) {
      String r = ""; for (int i = 0; i < (int)s.length(); i++) {
        if (s[i]=='+') r+=' ';
        else if (s[i]=='%' && i+2<(int)s.length()) {
          auto h=[](char c){return c>='0'&&c<='9'?c-'0':c>='A'&&c<='F'?c-'A'+10:c>='a'&&c<='f'?c-'a'+10:0;};
          r+=(char)(h(s[i+1])*16+h(s[i+2])); i+=2;
        } else r+=s[i];
      } return r;
    };
    auto gp = [&](const String& k) {
      int i = req.indexOf(k+"="); if (i<0) return String("");
      i += k.length()+1;
      int e = req.indexOf('&',i); if (e<0) e=req.indexOf(' ',i); if (e<0) e=req.length();
      return dec(req.substring(i,e));
    };
    String ss=gp("ssid"),pw=gp("pass"),h=gp("host"),po=gp("port"),ln=gp("lname"),di=gp("devid");
    if (ss.length()) networkSSID=ss; if (pw.length()) networkPass=pw;
    if (h.length())  tallyarbiter_host=h; if (po.length()) tallyarbiter_port=po.toInt();
    if (ln.length()) listenerDeviceName=ln; if (di.length()) DeviceId=di;
    saveConfig();
    client.println("HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nConnection: close\r\n");
    client.println("Saved! Rebooting...");
    client.stop(); delay(1000); matrixScroll(" REBOOTING ", 50); NVIC_SystemReset();
    return;
  }

  const char* HD = "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nConnection: close\r\n\r\n";
  client.print(HD);
  client.print(F("<!DOCTYPE html><html><head><meta charset=UTF-8><meta name=viewport content='width=device-width,initial-scale=1'>"));
  client.print(F("<title>Tally UNO R4</title><style>body{background:#0d0d0d;color:#e0e0e0;font-family:monospace;padding:1.5rem;max-width:520px;margin:auto}"));
  client.print(F("h1{color:#ff2d2d;font-size:1.3rem}label{display:block;color:#555;font-size:.7rem;text-transform:uppercase;margin:.8rem 0 .2rem}"));
  client.print(F("input{width:100%;background:#111;border:1px solid #2a2a2a;border-radius:3px;color:#e0e0e0;font-family:monospace;padding:.5rem .6rem;box-sizing:border-box}"));
  client.print(F("button{margin-top:1rem;width:100%;background:#ff2d2d;color:#fff;border:none;border-radius:3px;font-family:monospace;padding:.65rem;cursor:pointer}"));
  client.print(F(".row{display:flex;gap:.5rem}#msg{color:#00e676;font-size:.75rem;margin-top:.5rem;text-align:center}"));
  client.print(F(".grid{display:grid;grid-template-columns:1fr 1fr;gap:.3rem 1rem;margin:1rem 0;padding:.75rem;background:#111;border:1px solid #1e1e1e;border-radius:3px}"));
  client.print(F(".sl{color:#555;font-size:.62rem;text-transform:uppercase}.sv{font-size:.85rem}.on{color:#00e676}.off{color:#444}.rd{color:#ff2d2d}"));
  client.print(F("#log{background:#050505;border:1px solid #1a1a1a;border-radius:3px;padding:.6rem;height:150px;overflow-y:auto;font-size:.65rem;color:#888;white-space:pre-wrap}</style></head><body>"));
  client.print(F("<h1>&#9673; TALLY ARBITER</h1>"));
  client.print("<div style='color:#555;font-size:.7rem;letter-spacing:.2em;text-transform:uppercase;margin-bottom:1rem'>UNO R4 &mdash; ");
  client.print(listenerDeviceName); client.print(F("</div>"));
  client.print(F("<div class=grid>"));
  client.print(F("<div><div class=sl>WiFi</div><div class='sv ")); client.print(networkConnected?F("on'>CONNECTED"):F("off'>OFFLINE")); client.print(F("</div></div>"));
  client.print(F("<div><div class=sl>IP</div><div class=sv>")); client.print(WiFi.localIP()); client.print(F("</div></div>"));
  client.print(F("<div><div class=sl>TA Server</div><div class='sv ")); client.print(socketConnected?F("on'>CONNECTED"):F("off'>OFFLINE")); client.print(F("</div></div>"));
  client.print(F("<div><div class=sl>Device</div><div class=sv>")); client.print(DeviceName); client.print(F("</div></div>"));
  client.print(F("<div><div class=sl>Program</div><div class='sv ")); client.print(mode_program?F("rd'>LIVE"):F("off'>OFF")); client.print(F("</div></div>"));
  client.print(F("<div><div class=sl>Preview</div><div class='sv ")); client.print(mode_preview?F("on'>LIVE"):F("off'>OFF")); client.print(F("</div></div></div>"));
  client.print(F("<label>WiFi SSID</label><input id=s value='")); client.print(networkSSID); client.print(F("'>"));
  client.print(F("<label>WiFi Password</label><input type=password id=w placeholder='(unchanged)'>"));
  client.print(F("<label>Tally Arbiter IP</label><div class=row><input id=h value='")); client.print(tallyarbiter_host); client.print(F("'>"));
  client.print(F("<input type=number id=p value='")); client.print(tallyarbiter_port); client.print(F("' style='max-width:80px'></div>"));
  client.print(F("<label>Listener Name</label><input id=l value='")); client.print(listenerDeviceName); client.print(F("'>"));
  client.print(F("<label>Device ID</label><input id=d value='")); client.print(DeviceId); client.print(F("'>"));
  client.print(F("<button onclick=sv()>Save &amp; Reboot</button><div id=msg></div>"));
  client.print(F("<hr style='border:none;border-top:1px solid #1e1e1e;margin:1rem 0'>"));
  client.print(F("<div style='color:#555;font-size:.62rem;text-transform:uppercase;margin-bottom:.4rem'>Log</div><div id=log>"));
  int st = logHead >= LOG_LINES ? logHead : 0;
  for (int i = 0; i < LOG_LINES; i++) { const String& l = logBuffer[(st+i)%LOG_LINES]; if (l.length()) { client.print(l); client.print('\n'); } }
  client.print(F("</div><script>function sv(){var s=document.getElementById('s').value,w=document.getElementById('w').value,"));
  client.print(F("h=document.getElementById('h').value,p=document.getElementById('p').value,"));
  client.print(F("l=document.getElementById('l').value,d=document.getElementById('d').value;"));
  client.print(F("if(!s||!h||!p)return;document.getElementById('msg').textContent='Saving...';"));
  client.print(F("fetch('/save?ssid='+encodeURIComponent(s)+'&pass='+encodeURIComponent(w)+'&host='+encodeURIComponent(h)"));
  client.print(F("+'&port='+p+'&lname='+encodeURIComponent(l)+'&devid='+encodeURIComponent(d))"));
  client.print(F(".then(r=>r.text()).then(t=>document.getElementById('msg').textContent=t)"));
  client.print(F(".catch(()=>document.getElementById('msg').textContent='Error'));}"));
  client.print(F("document.getElementById('log').scrollTop=9999;</script></body></html>"));
  client.stop();
}

/* ════════════════════════════════════════
   SETUP & LOOP
   ════════════════════════════════════════ */
void setup() {
  Serial.begin(115200);
  delay(200);
  matrix.begin();
  matrixClear();

  // Save compiled-in values before EEPROM may overwrite
  String compiledSSID = networkSSID, compiledPass = networkPass, compiledHost = tallyarbiter_host;

  EEPROM.begin();
  bool hadSaved = loadConfig();

  bool compiledIsReal = compiledSSID.length() > 0 && !compiledSSID.startsWith("TALLY_");
  if (compiledIsReal) {
    networkSSID = compiledSSID; networkPass = compiledPass; tallyarbiter_host = compiledHost;
    saveConfig();
    Serial.println("Patched binary — config applied.");
  }

  byte mac[6]; WiFi.macAddress(mac);
  char suf[7]; snprintf(suf, 7, "%02x%02x%02x", mac[3], mac[4], mac[5]);
  if (listenerDeviceName.startsWith("TALLY_") || listenerDeviceName.length() == 0)
    listenerDeviceName = String("unor4-") + suf;

  Serial.println("=== Tally Arbiter UNO R4 WiFi ===");
  Serial.println("Listener : " + listenerDeviceName);
  Serial.println("WiFi SSID: " + networkSSID);
  Serial.println("TA Host  : " + tallyarbiter_host);
  Serial.println("TA Port  : " + String(tallyarbiter_port));
  Serial.println("Device ID: " + DeviceId);
  Serial.println("=================================");

  bool hasConfig = networkSSID.length() > 0 && !networkSSID.startsWith("TALLY_");
  if (!hasConfig) {
    while (true) { matrixScroll(" NO CONFIG  CONNECT USB ", 50); checkSerialConfig();
      if (networkSSID.length() > 0 && !networkSSID.startsWith("TALLY_")) break; }
  }

  WiFi.begin(networkSSID.c_str(), networkPass.c_str());
  unsigned long t = millis();
  while (WiFi.status() != WL_CONNECTED) {
    matrixScroll(" CONNECTING TO WIFI ", 55); checkSerialConfig();
    if (millis() - t > 30000) { matrixScroll(" WIFI TIMEOUT  REBOOT ", 50); delay(500); NVIC_SystemReset(); }
  }
  networkConnected = true;
  String ip = WiFi.localIP().toString();
  addLog("WiFi OK: " + ip);
  matrixScroll((" WIFI OK  " + ip + " ").c_str(), 55);
  matrixClear();
  webServer.begin();

  matrixScroll((" TA: " + tallyarbiter_host + " ").c_str(), 55);
  if (wsConnect()) {
    socketConnected = true;
    pendingRegister = true;
  }
  lastPing = millis();
}

void loop() {
  // WiFi watchdog
  bool wNow = (WiFi.status() == WL_CONNECTED);
  if (wNow != networkConnected) {
    networkConnected = wNow;
    if (!networkConnected) { addLog("WiFi lost."); socketConnected = false; matrixScroll(" WIFI LOST ", 50); }
  }

  // Reconnect socket if dropped
  if (networkConnected && !socketConnected) {
    addLog("Reconnecting to TA...");
    matrixScroll((" TA: " + tallyarbiter_host + " ").c_str(), 55);
    if (wsConnect()) { socketConnected = true; pendingRegister = true; }
    else delay(5000);
  }

  // Register once connected
  if (pendingRegister && socketConnected) { pendingRegister = false; doRegister(); }

  // Send EIO ping to keep connection alive
  if (socketConnected && millis() - lastPing > pingInterval) {
    wsSend(EIO_PING); lastPing = millis();
  }

  // Read incoming WebSocket frames
  if (socketConnected && wsClient.connected()) {
    if (wsReadFrame()) {
      if (wsOpcode == 0x8) { addLog("WS closed by server."); socketConnected = false; wsClient.stop(); }
      else if (wsOpcode == 0x9) { wsSend(String((char)0x8A)); } // pong
      else if (wsOpcode == 0x1) handleSIOMessage(wsPayload);
    }
  } else if (socketConnected) {
    socketConnected = false;
    addLog("WS disconnected.");
    matrixScroll(" TA OFFLINE ", 50);
  }

  handleWebClient();
  checkSerialConfig();
  delay(1);
}

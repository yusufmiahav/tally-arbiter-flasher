// ArduinoGraphics MUST be included before Arduino_LED_Matrix
#include "ArduinoGraphics.h"
#include "Arduino_LED_Matrix.h"
#include <WiFiS3.h>
// Tell WebSockets library to use WiFiNINA network type — prevents Ethernet.h include on R4
#define WEBSOCKETS_NETWORK_TYPE NETWORK_WIFININA
#include <WebSocketsClient.h>
#include <SocketIOclient.h>
#include <ArduinoJson.h>
#include <EEPROM.h>

/* ════════════════════════════════════════
   TALLY ARBITER — Arduino UNO R4 WiFi
   12×8 red LED matrix display
   ════════════════════════════════════════ */

/* ── Placeholders — patched by web flasher at flash time ── */
String networkSSID       = "TALLY_SSID_PLACEHOLDER_00000000000000000000000000000000";
String networkPass       = "TALLY_PASS_PLACEHOLDER_00000000000000000000000000000000";
String tallyarbiter_host = "TALLY_HOST_PLACEHOLDER_000000000000000";
int    tallyarbiter_port = 4455;
String listenerDeviceName = "TALLY_NAME_PLACEHOLDER_000000000000000";

/* ── Runtime state ── */
ArduinoLEDMatrix matrix;
SocketIOclient   socket;

StaticJsonDocument<4096> BusOptions;
StaticJsonDocument<8192> Devices;
StaticJsonDocument<4096> DeviceStates;

String DeviceId   = "unassigned";
String DeviceName = "Unassigned";

bool mode_preview     = false;
bool mode_program     = false;
bool networkConnected = false;
bool socketConnected  = false;
bool pendingRegister  = false;

/* ── EEPROM layout (simple fixed offsets) ── */
#define EE_MAGIC   0      // 2 bytes: 0xTA42 if valid
#define EE_SSID    2      // 64 bytes
#define EE_PASS    66     // 64 bytes
#define EE_HOST    130    // 40 bytes
#define EE_PORT    170    // 2 bytes (int)
#define EE_LNAME   172    // 32 bytes
#define EE_DEVID   204    // 32 bytes
#define EE_SIZE    236

#define EE_MAGIC_VAL 0x4241  // "BA"

/* ── Log ring buffer ── */
#define LOG_LINES 20
String logBuffer[LOG_LINES];
int    logHead = 0;

void addLog(const String& line) {
  Serial.println(line);
  logBuffer[logHead % LOG_LINES] = line;
  logHead++;
}

/* ════════════════════════════════════════
   EEPROM HELPERS
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
    if (c == 0) break;
    s += c;
  }
  return s;
}

bool loadConfig() {
  uint16_t magic = (EEPROM.read(EE_MAGIC) << 8) | EEPROM.read(EE_MAGIC + 1);
  if (magic != EE_MAGIC_VAL) return false;

  String ss = eeReadStr(EE_SSID,  64);
  String pw = eeReadStr(EE_PASS,  64);
  String h  = eeReadStr(EE_HOST,  40);
  int    po = (EEPROM.read(EE_PORT) << 8) | EEPROM.read(EE_PORT + 1);
  String ln = eeReadStr(EE_LNAME, 32);
  String di = eeReadStr(EE_DEVID, 32);

  if (ss.length() > 0) networkSSID        = ss;
  if (pw.length() > 0) networkPass        = pw;
  if (h.length()  > 0) tallyarbiter_host  = h;
  if (po          > 0) tallyarbiter_port  = po;
  if (ln.length() > 0) listenerDeviceName = ln;
  if (di.length() > 0) DeviceId           = di;
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
   12 cols × 8 rows, all red
   ════════════════════════════════════════ */

// Scroll text across the matrix — BLOCKS until complete
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

// Fill entire matrix
void matrixFill() {
  byte frame[8][12];
  memset(frame, 1, sizeof(frame));
  matrix.renderBitmap(frame, 8, 12);
}

// Clear entire matrix
void matrixClear() {
  byte frame[8][12];
  memset(frame, 0, sizeof(frame));
  matrix.renderBitmap(frame, 8, 12);
}

// Border frame only (preview indicator)
void matrixBorder() {
  byte frame[8][12] = {
    {1,1,1,1,1,1,1,1,1,1,1,1},
    {1,0,0,0,0,0,0,0,0,0,0,1},
    {1,0,0,0,0,0,0,0,0,0,0,1},
    {1,0,0,0,0,0,0,0,0,0,0,1},
    {1,0,0,0,0,0,0,0,0,0,0,1},
    {1,0,0,0,0,0,0,0,0,0,0,1},
    {1,0,0,0,0,0,0,0,0,0,0,1},
    {1,1,1,1,1,1,1,1,1,1,1,1}
  };
  matrix.renderBitmap(frame, 8, 12);
}

// Show current tally state on matrix
void showTallyState() {
  if (mode_program && mode_preview) {
    matrixFill(); // both = full (program takes priority)
  } else if (mode_program) {
    matrixFill(); // program = full solid
  } else if (mode_preview) {
    matrixBorder(); // preview = border only
  } else {
    matrixClear(); // clear = off
  }
}

/* ════════════════════════════════════════
   SERIAL CONFIG RECEIVER
   Non-blocking line accumulation
   ════════════════════════════════════════ */
String serialLineBuffer = "";

void checkSerialConfig() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n') {
      String line = serialLineBuffer;
      serialLineBuffer = "";
      line.trim();
      if (!line.startsWith("CFG:")) continue;

      String json = line.substring(4);
      Serial.println("CFG raw (" + String(json.length()) + " bytes): " + json);

      StaticJsonDocument<1024> cfg;
      DeserializationError err = deserializeJson(cfg, json);
      if (err) { Serial.println("CFG parse error: " + String(err.c_str())); continue; }

      if (cfg.containsKey("ssid"))   networkSSID        = cfg["ssid"].as<String>();
      if (cfg.containsKey("pass"))   networkPass        = cfg["pass"].as<String>();
      if (cfg.containsKey("tahost")) tallyarbiter_host  = cfg["tahost"].as<String>();
      if (cfg.containsKey("taport")) tallyarbiter_port  = cfg["taport"].as<int>();
      if (cfg.containsKey("lname"))  listenerDeviceName = cfg["lname"].as<String>();
      if (cfg.containsKey("devid"))  DeviceId           = cfg["devid"].as<String>();

      saveConfig();
      Serial.println("CFG saved. Rebooting...");
      matrixScroll(" CFG SAVED  REBOOTING ", 40);
      delay(500);
      NVIC_SystemReset();
    } else if (c != '\r') {
      serialLineBuffer += c;
      if (serialLineBuffer.length() > 1200) serialLineBuffer = "";
    }
  }
}

/* ════════════════════════════════════════
   TALLY LOGIC
   ════════════════════════════════════════ */
String getBusTypeById(const String& busId) {
  for (JsonObject bus : BusOptions.as<JsonArray>()) {
    if (bus["id"].as<String>() == busId)
      return bus["type"].as<String>();
  }
  return "invalid";
}

void processTallyData() {
  mode_preview = false;
  mode_program = false;
  for (JsonObject state : DeviceStates.as<JsonArray>()) {
    String stateDevId = state["deviceId"].as<String>();
    if (stateDevId != DeviceId) continue;
    String busType = getBusTypeById(state["busId"].as<String>());
    bool   active  = state["sources"].as<JsonArray>().size() > 0;
    if (busType == "preview") mode_preview = active;
    if (busType == "program") mode_program = active;
  }
  addLog(String("Tally: ") + (mode_program ? "PROGRAM" : mode_preview ? "PREVIEW" : "CLEAR"));
  showTallyState();
}

void SetDeviceName() {
  for (JsonObject dev : Devices.as<JsonArray>()) {
    if (dev["id"].as<String>() == DeviceId) {
      DeviceName = dev["name"].as<String>();
      break;
    }
  }
  addLog("Device: " + DeviceName + " (" + DeviceId + ")");
  saveConfig();
}

/* ════════════════════════════════════════
   SOCKET.IO
   ════════════════════════════════════════ */
void ws_emit(const char* event, const char* payload = nullptr) {
  String msg = payload
    ? String("[\"") + event + "\"," + payload + "]"
    : String("[\"") + event + "\"]";
  addLog("TX: " + msg.substring(0, 80));
  socket.sendEVENT(msg);
}

void doRegister() {
  String payload = "{"
    "\"deviceId\":\"" + DeviceId + "\","
    "\"listenerType\":\"" + listenerDeviceName + "\","
    "\"canBeReassigned\":true,"
    "\"canBeFlashed\":true,"
    "\"supportsChat\":false"
    "}";
  ws_emit("listenerclient_connect", payload.c_str());
  addLog("Registered: " + listenerDeviceName);
}

String strip_quot(String str) {
  str.trim();
  if (str.length() > 0 && str[0] == '"')       str.remove(0, 1);
  if (str.length() > 0 && str.endsWith("\""))  str.remove(str.length()-1, 1);
  return str;
}

void socketEvent(socketIOmessageType_t type, uint8_t* payload, size_t length) {
  switch (type) {

    case sIOtype_CONNECT:
      addLog("Socket.IO connected.");
      socketConnected = true;
      pendingRegister = true;
      matrixClear();
      break;

    case sIOtype_DISCONNECT:
      addLog("Socket.IO disconnected.");
      socketConnected = false;
      pendingRegister = false;
      matrixScroll(" TA OFFLINE ", 50);
      break;

    case sIOtype_EVENT: {
      String data = String((char*)payload);

      int nameStart = data.indexOf('"');
      if (nameStart < 0) break;
      int nameEnd = data.indexOf('"', nameStart + 1);
      if (nameEnd < 0) break;
      String event = data.substring(nameStart + 1, nameEnd);
      addLog("RX: " + event);

      int payloadStart = data.indexOf(',', nameEnd);
      if (payloadStart < 0) break;
      payloadStart++;
      int payloadEnd = data.lastIndexOf(']');
      if (payloadEnd <= payloadStart) break;
      String payloadStr = data.substring(payloadStart, payloadEnd);
      payloadStr.trim();

      if (event == "bus_options") {
        BusOptions.clear();
        DeserializationError e = deserializeJson(BusOptions, payloadStr);
        if (!e) addLog("bus_options OK (" + String(BusOptions.as<JsonArray>().size()) + ")");
        else    addLog("bus_options ERR: " + String(e.c_str()));

      } else if (event == "devices") {
        Devices.clear();
        DeserializationError e = deserializeJson(Devices, payloadStr);
        if (!e) SetDeviceName();

      } else if (event == "device_states") {
        DeviceStates.clear();
        DeserializationError e = deserializeJson(DeviceStates, payloadStr);
        if (!e) processTallyData();
        else    addLog("device_states ERR: " + String(e.c_str()));

      } else if (event == "reassign") {
        // data = ["reassign","oldId","newId","listenerId"]
        int pos = 0;
        String tokens[4];
        int found = 0;
        while (found < 4 && pos < (int)data.length()) {
          int qs = data.indexOf('"', pos);
          if (qs < 0) break;
          int qe = data.indexOf('"', qs + 1);
          if (qe < 0) break;
          tokens[found++] = data.substring(qs + 1, qe);
          pos = qe + 1;
        }
        if (found >= 3) {
          String newId = tokens[2];
          addLog("Reassign -> " + newId);
          DeviceId = newId;
          saveConfig();
          // Scroll new device name
          String newName = "UNKNOWN";
          for (JsonObject dev : Devices.as<JsonArray>()) {
            if (dev["id"].as<String>() == newId) {
              newName = dev["name"].as<String>();
              break;
            }
          }
          String msg = " NOW: " + newName + " ";
          matrixScroll(msg.c_str(), 50);
          showTallyState();
        }

      } else if (event == "flash") {
        addLog("Flash command received.");
        bool prev_preview = mode_preview;
        bool prev_program = mode_program;
        // Flash: blink previous state off/on × 3
        for (int i = 0; i < 3; i++) {
          matrixClear();  delay(150);
          showTallyState(); delay(150);
        }
        // Restore
        mode_preview = prev_preview;
        mode_program = prev_program;
        showTallyState();
      }
      break;
    }
    default: break;
  }
}

/* ════════════════════════════════════════
   WIFI
   ════════════════════════════════════════ */
void connectToNetwork() {
  addLog("Connecting to: " + networkSSID);
  WiFi.begin(networkSSID.c_str(), networkPass.c_str());
}

/* ════════════════════════════════════════
   WEB SERVER (simple settings page)
   ════════════════════════════════════════ */
WiFiServer webServer(80);

void handleWebClient() {
  WiFiClient client = webServer.available();
  if (!client) return;

  unsigned long timeout = millis() + 1500;
  String request = "";
  while (client.connected() && millis() < timeout) {
    if (client.available()) {
      char c = client.read();
      request += c;
      if (request.endsWith("\r\n\r\n")) break;
    }
  }

  // Parse GET /save?...
  if (request.indexOf("GET /save") >= 0) {
    auto urlDecode = [](String s) -> String {
      String r = "";
      for (int i = 0; i < (int)s.length(); i++) {
        if (s[i] == '+') { r += ' '; }
        else if (s[i] == '%' && i + 2 < (int)s.length()) {
          char h = s[i+1], l = s[i+2];
          auto hx = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            return 0;
          };
          r += (char)(hx(h) * 16 + hx(l));
          i += 2;
        } else { r += s[i]; }
      }
      return r;
    };

    auto getParam = [&](const String& key) -> String {
      String search = key + "=";
      int idx = request.indexOf(search);
      if (idx < 0) return "";
      idx += search.length();
      int end = request.indexOf('&', idx);
      if (end < 0) end = request.indexOf(' ', idx);
      if (end < 0) end = request.length();
      return urlDecode(request.substring(idx, end));
    };

    String ss = getParam("ssid");
    String pw = getParam("pass");
    String h  = getParam("host");
    String po = getParam("port");
    String ln = getParam("lname");
    String di = getParam("devid");

    if (ss.length() > 0) networkSSID       = ss;
    if (pw.length() > 0) networkPass       = pw;
    if (h.length()  > 0) tallyarbiter_host = h;
    if (po.length() > 0) tallyarbiter_port = po.toInt();
    if (ln.length() > 0) listenerDeviceName = ln;
    if (di.length() > 0) DeviceId          = di;

    saveConfig();
    client.println("HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nConnection: close\r\n");
    client.println("Saved! Rebooting...");
    client.stop();
    delay(1000);
    matrixScroll(" REBOOTING ", 50);
    NVIC_SystemReset();
    return;
  }

  // Serve settings page
  String p = "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nConnection: close\r\n\r\n";
  p += "<!DOCTYPE html><html><head>"
       "<meta charset=UTF-8><meta name=viewport content='width=device-width,initial-scale=1'>"
       "<title>Tally Arbiter UNO R4</title>"
       "<style>"
       "body{background:#0d0d0d;color:#e0e0e0;font-family:monospace;padding:1.5rem;max-width:520px;margin:auto}"
       "h1{color:#ff2d2d;letter-spacing:.1em;font-size:1.3rem}"
       ".sub{color:#555;font-size:.7rem;letter-spacing:.2em;text-transform:uppercase;margin-bottom:1.5rem}"
       "label{display:block;color:#555;font-size:.7rem;text-transform:uppercase;letter-spacing:.15em;margin-top:1rem;margin-bottom:.3rem}"
       "input{width:100%;background:#111;border:1px solid #2a2a2a;border-radius:3px;color:#e0e0e0;font-family:monospace;font-size:.9rem;padding:.5rem .6rem;box-sizing:border-box}"
       "input:focus{outline:none;border-color:#ff2d2d}"
       ".row{display:flex;gap:.5rem}"
       "button{margin-top:1rem;width:100%;background:#ff2d2d;color:#fff;border:none;border-radius:3px;font-family:monospace;font-size:.85rem;padding:.65rem;cursor:pointer;text-transform:uppercase}"
       "button:hover{background:#c02020}"
       "#msg{margin-top:.5rem;color:#00e676;font-size:.75rem;min-height:1em;text-align:center}"
       ".grid{display:grid;grid-template-columns:1fr 1fr;gap:.4rem 1.5rem;margin-top:1.5rem;padding:.75rem 1rem;background:#111;border:1px solid #1e1e1e;border-radius:3px}"
       ".sl{color:#555;font-size:.62rem;text-transform:uppercase;letter-spacing:.12em}"
       ".sv{font-size:.85rem;margin-top:1px}"
       ".on{color:#00e676}.off{color:#444}.rd{color:#ff2d2d}"
       "hr{border:none;border-top:1px solid #1e1e1e;margin:1.25rem 0}"
       "#log{background:#050505;border:1px solid #1a1a1a;border-radius:3px;padding:.6rem;height:180px;overflow-y:auto;font-size:.68rem;color:#888;white-space:pre-wrap;word-break:break-all}"
       ".lt{color:#555;font-size:.62rem;text-transform:uppercase;letter-spacing:.12em;margin-bottom:.4rem}"
       "</style></head><body>";

  p += "<h1>&#9673; TALLY ARBITER</h1><div class=sub>UNO R4 WiFi &mdash; " + listenerDeviceName + "</div>";

  p += "<div class=grid>";
  p += "<div><div class=sl>WiFi</div><div class='sv " + String(networkConnected ? "on'>CONNECTED" : "off'>OFFLINE") + "</div></div>";
  p += "<div><div class=sl>IP</div><div class=sv>" + WiFi.localIP().toString() + "</div></div>";
  p += "<div><div class=sl>Tally Arbiter</div><div class='sv " + String(socketConnected ? "on'>CONNECTED" : "off'>OFFLINE") + "</div></div>";
  p += "<div><div class=sl>Device</div><div class=sv>" + DeviceName + "</div></div>";
  p += "<div><div class=sl>Program</div><div class='sv " + String(mode_program ? "rd'>LIVE" : "off'>OFF") + "</div></div>";
  p += "<div><div class=sl>Preview</div><div class='sv " + String(mode_preview ? "on'>LIVE" : "off'>OFF") + "</div></div>";
  p += "</div>";

  p += "<hr>"
       "<label>WiFi SSID</label><input type=text id=ssid value='" + networkSSID + "'>"
       "<label>WiFi Password</label><input type=password id=wpass placeholder='(unchanged)'>"
       "<label>Tally Arbiter IP</label>"
       "<div class=row><input type=text id=host value='" + tallyarbiter_host + "'>"
       "<input type=number id=port value='" + String(tallyarbiter_port) + "' style='max-width:80px'></div>"
       "<label>Listener Name</label><input type=text id=lname value='" + listenerDeviceName + "'>"
       "<label>Device ID</label><input type=text id=did value='" + DeviceId + "'>"
       "<button onclick=sv()>Save &amp; Reboot</button>"
       "<div id=msg></div>";

  p += "<hr><div class=lt>Debug Log</div><div id=log>";
  int start = (logHead >= LOG_LINES) ? logHead : 0;
  for (int i = 0; i < LOG_LINES; i++) {
    const String& line = logBuffer[(start + i) % LOG_LINES];
    if (line.length()) { p += line; p += "\n"; }
  }
  p += "</div>";

  p += "<script>"
       "function sv(){"
       "var s=document.getElementById('ssid').value,"
       "wp=document.getElementById('wpass').value,"
       "h=document.getElementById('host').value,"
       "pt=document.getElementById('port').value,"
       "ln=document.getElementById('lname').value,"
       "di=document.getElementById('did').value;"
       "if(!s||!h||!pt)return;"
       "document.getElementById('msg').textContent='Saving...';"
       "fetch('/save?ssid='+encodeURIComponent(s)+'&pass='+encodeURIComponent(wp)"
       "+'&host='+encodeURIComponent(h)+'&port='+pt"
       "+'&lname='+encodeURIComponent(ln)+'&devid='+encodeURIComponent(di))"
       ".then(r=>r.text()).then(t=>document.getElementById('msg').textContent=t)"
       ".catch(()=>document.getElementById('msg').textContent='Error');}"
       "document.getElementById('log').scrollTop=9999;"
       "</script></body></html>";

  client.print(p);
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

  // Generate unique listener name from MAC
  byte mac[6];
  WiFi.macAddress(mac);
  char macSuffix[7];
  snprintf(macSuffix, sizeof(macSuffix), "%02x%02x%02x", mac[3], mac[4], mac[5]);

  // Save compiled-in values before EEPROM may overwrite them
  String compiledSSID = networkSSID;
  String compiledPass = networkPass;
  String compiledHost = tallyarbiter_host;

  EEPROM.begin(EE_SIZE);
  bool hadSaved = loadConfig();

  // If binary was patched with real credentials, always use them
  bool compiledIsReal = compiledSSID.length() > 0 && !compiledSSID.startsWith("TALLY_");
  if (compiledIsReal) {
    Serial.println("Patched binary — overriding saved config.");
    networkSSID       = compiledSSID;
    networkPass       = compiledPass;
    tallyarbiter_host = compiledHost;
    saveConfig();
  }

  // Set listener name from MAC if still a placeholder
  if (listenerDeviceName.startsWith("TALLY_") || listenerDeviceName.length() == 0)
    listenerDeviceName = String("unor4-") + macSuffix;

  Serial.println("=== Tally Arbiter UNO R4 WiFi ===");
  Serial.println("Listener : " + listenerDeviceName);
  Serial.println("WiFi SSID: " + networkSSID);
  Serial.println("TA Host  : " + tallyarbiter_host);
  Serial.println("TA Port  : " + String(tallyarbiter_port));
  Serial.println("Device ID: " + DeviceId);
  Serial.println("Config   : " + String(compiledIsReal ? "Patched binary" : hadSaved ? "Loaded from EEPROM" : "Using defaults"));
  Serial.println("=================================");

  // No WiFi config — scroll message and wait for CFG packet
  bool hasConfig = networkSSID.length() > 0 && !networkSSID.startsWith("TALLY_");
  if (!hasConfig) {
    Serial.println("No config — waiting for CFG packet...");
    while (true) {
      matrixScroll(" NO CONFIG  CONNECT USB ", 50);
      checkSerialConfig();
      if (networkSSID.length() > 0 && !networkSSID.startsWith("TALLY_")) break;
    }
  }

  // Connect to WiFi — scroll "CONNECTING" while waiting
  connectToNetwork();
  unsigned long t = millis();
  while (WiFi.status() != WL_CONNECTED) {
    matrixScroll(" CONNECTING TO WIFI ", 55);
    checkSerialConfig();
    if (millis() - t > 30000) {
      matrixScroll(" WIFI TIMEOUT  REBOOT ", 50);
      delay(500);
      NVIC_SystemReset();
    }
  }

  networkConnected = true;
  String ip = WiFi.localIP().toString();
  addLog("WiFi OK: " + ip);
  String ipMsg = " WIFI OK  " + ip + " ";
  matrixScroll(ipMsg.c_str(), 55);
  matrixClear();

  webServer.begin();
  addLog("Web UI: http://" + ip);

  // Connect to Tally Arbiter
  matrixScroll((" TA: " + tallyarbiter_host + " ").c_str(), 55);
  socket.onEvent(socketEvent);
  socket.setReconnectInterval(10000);
  socket.begin(tallyarbiter_host.c_str(), tallyarbiter_port);
}

void loop() {
  // Check WiFi
  bool wifiNow = (WiFi.status() == WL_CONNECTED);
  if (wifiNow != networkConnected) {
    networkConnected = wifiNow;
    if (!networkConnected) {
      addLog("WiFi lost.");
      socketConnected = false;
      matrixScroll(" WIFI LOST ", 50);
    } else {
      addLog("WiFi reconnected: " + WiFi.localIP().toString());
    }
  }

  socket.loop();
  handleWebClient();
  checkSerialConfig();

  if (pendingRegister && socketConnected) {
    pendingRegister = false;
    doRegister();
  }

  delay(1);
}

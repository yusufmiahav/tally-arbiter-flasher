#include <WiFi.h>
#include <WebSocketsClient.h>
#include <SocketIOclient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <WebServer.h>

/* ── Default config (overridden by web flasher or web UI) ── */
#define RED_PIN    23
#define GREEN_PIN  22

bool CUT_BUS = true;

// These defaults are used only if no saved preferences exist
// The web flasher writes real values via CFG: serial packet on first boot
String tallyarbiter_host = "192.168.1.212";
int    tallyarbiter_port = 4455;
String networkSSID       = "";
String networkPass       = "";
bool   USE_STATIC        = false;
IPAddress clientIp(192, 168, 1, 100);
IPAddress subnet(255, 255, 255, 0);
IPAddress gateway(192, 168, 1, 1);

/* ── Runtime state ── */
Preferences    preferences;
WebServer      webServer(80);
SocketIOclient socket;

StaticJsonDocument<4096> BusOptions;
StaticJsonDocument<8192> Devices;
StaticJsonDocument<4096> DeviceStates;

String DeviceId   = "unassigned";
String DeviceName = "Unassigned";
String listenerDeviceName = "esp32-tally";

bool mode_preview     = false;
bool mode_program     = false;
bool networkConnected = false;
bool socketConnected  = false;
bool pendingRegister  = false;
bool socketPaused     = false; // paused while user edits settings in web UI

int pinRed   = RED_PIN;
int pinGreen = GREEN_PIN;

/* ── Log ring buffer ── */
#define LOG_LINES 30
String logBuffer[LOG_LINES];
int    logHead = 0;

void addLog(const String& line) {
  Serial.println(line);
  logBuffer[logHead % LOG_LINES] = line;
  logHead++;
}

/* ── LED helpers ── */
void setLEDs(bool red, bool green) {
  digitalWrite(pinRed,   red   ? HIGH : LOW);
  digitalWrite(pinGreen, green ? HIGH : LOW);
}

void evaluateMode() {
  if (mode_preview && !mode_program) {
    addLog("Tally: PREVIEW");
    setLEDs(false, true);
  } else if (!mode_preview && mode_program) {
    addLog("Tally: PROGRAM");
    setLEDs(true, false);
  } else if (mode_preview && mode_program) {
    addLog("Tally: PREVIEW+PROGRAM");
    setLEDs(true, CUT_BUS ? false : true);
  } else {
    addLog("Tally: CLEAR");
    setLEDs(false, false);
  }
}

/* ═══════════════════════════════════════════════════════
   SERIAL CONFIG RECEIVER
   The web flasher sends: CFG:{json}\n after flashing.
   We parse it here and save to NVS.
   ═══════════════════════════════════════════════════════ */
void checkSerialConfig() {
  if (!Serial.available()) return;
  String line = Serial.readStringUntil('\n');
  line.trim();
  if (!line.startsWith("CFG:")) return;

  String json = line.substring(4);
  StaticJsonDocument<512> cfg;
  DeserializationError err = deserializeJson(cfg, json);
  if (err) {
    Serial.println("CFG parse error: " + String(err.c_str()));
    return;
  }

  Serial.println("CFG received — clearing old config and saving...");

  // Clear ALL old values first so stale WiFi/TA settings don't persist
  preferences.begin("tally-arbiter", false);
  preferences.clear();
  preferences.end();

  preferences.begin("tally-arbiter", false);

  if (cfg.containsKey("ssid"))   preferences.putString("ssid",   cfg["ssid"].as<String>());
  if (cfg.containsKey("pass"))   preferences.putString("pass",   cfg["pass"].as<String>());
  if (cfg.containsKey("tahost")) preferences.putString("tahost", cfg["tahost"].as<String>());
  if (cfg.containsKey("taport")) preferences.putInt   ("taport", cfg["taport"].as<int>());
  if (cfg.containsKey("lname"))  preferences.putString("lname",  cfg["lname"].as<String>());
  if (cfg.containsKey("devid"))  preferences.putString("deviceid", cfg["devid"].as<String>());
  if (cfg.containsKey("cutbus")) preferences.putBool  ("cutbus", cfg["cutbus"].as<int>() == 1);
  if (cfg.containsKey("pinr"))   preferences.putInt   ("pinr",   cfg["pinr"].as<int>());
  if (cfg.containsKey("ping"))   preferences.putInt   ("ping",   cfg["ping"].as<int>());
  if (cfg.containsKey("static")) preferences.putBool  ("static", cfg["static"].as<int>() == 1);
  if (cfg.containsKey("sip"))    preferences.putString("sip",    cfg["sip"].as<String>());
  if (cfg.containsKey("sgw"))    preferences.putString("sgw",    cfg["sgw"].as<String>());
  if (cfg.containsKey("ssn"))    preferences.putString("ssn",    cfg["ssn"].as<String>());

  preferences.end();
  Serial.println("CFG saved. Rebooting...");
  delay(500);
  ESP.restart();
}

/* ── Tally logic ── */
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
    if (state.containsKey("deviceId") && state["deviceId"].as<String>() != DeviceId)
      continue;
    String busType = getBusTypeById(state["busId"].as<String>());
    if (busType == "preview")
      mode_preview = state["sources"].as<JsonArray>().size() > 0;
    if (busType == "program")
      mode_program = state["sources"].as<JsonArray>().size() > 0;
  }
  evaluateMode();
}

void SetDeviceName() {
  for (JsonObject dev : Devices.as<JsonArray>()) {
    if (dev["id"].as<String>() == DeviceId) {
      DeviceName = dev["name"].as<String>();
      break;
    }
  }
  addLog("Device: " + DeviceName + " (" + DeviceId + ")");
  preferences.begin("tally-arbiter", false);
  preferences.putString("devicename", DeviceName);
  preferences.end();
}

/* ── Socket helpers ── */
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
  char buf[512];
  payload.toCharArray(buf, sizeof(buf));
  ws_emit("bus_options");
  ws_emit("listenerclient_connect", buf);
  addLog("Registered: " + listenerDeviceName);
}

void socket_Flash() {
  for (int i = 0; i < 3; i++) {
    setLEDs(true, true);   delay(300);
    setLEDs(false, false); delay(300);
  }
  evaluateMode();
}

String strip_quot(String str) {
  str.trim();
  if (str.length() > 0 && str[0] == '"')      str.remove(0, 1);
  if (str.length() > 0 && str.endsWith("\"")) str.remove(str.length()-1, 1);
  return str;
}

void socket_Reassign(const String& payload) {
  int comma = payload.indexOf(',');
  if (comma < 0) { addLog("Reassign parse error"); return; }
  String oldId = strip_quot(payload.substring(0, comma));
  String newId = strip_quot(payload.substring(comma + 1));
  if (newId.indexOf(',') >= 0) newId = newId.substring(0, newId.indexOf(','));
  newId = strip_quot(newId);

  addLog("Reassign: " + oldId + " -> " + newId);
  String obj = "{\"oldDeviceId\":\"" + oldId + "\",\"newDeviceId\":\"" + newId + "\"}";
  char buf[256]; obj.toCharArray(buf, sizeof(buf));
  ws_emit("listener_reassign_object", buf);
  ws_emit("devices");
  for (int i = 0; i < 2; i++) { setLEDs(true, false); delay(200); setLEDs(false, false); delay(200); }

  DeviceId = newId;
  preferences.begin("tally-arbiter", false);
  preferences.putString("deviceid", newId);
  preferences.end();
  SetDeviceName();
}

void socket_event(socketIOmessageType_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case sIOtype_CONNECT:
      addLog("Socket.IO connected.");
      socketConnected = true;
      setLEDs(false, false);
      pendingRegister = true;
      break;

    case sIOtype_DISCONNECT:
      if (!socketPaused) addLog("Socket.IO disconnected.");
      socketConnected = false;
      pendingRegister = false;
      break;

    case sIOtype_EVENT: {
      String msg = (char*)payload;
      int ns = msg.indexOf('"') + 1;
      int ne = msg.indexOf('"', ns);
      if (ns <= 0 || ne <= ns) break;
      String evtType = msg.substring(ns, ne);
      String content = msg.substring(ne + 2);
      if (content.endsWith("]")) content.remove(content.length() - 1);

      addLog("RX: " + evtType);

      if (evtType == "bus_options") {
        BusOptions.clear();
        auto e = deserializeJson(BusOptions, content);
        if (e) addLog("bus_options ERR: " + String(e.c_str()));
        else addLog("bus_options OK (" + String(BusOptions.as<JsonArray>().size()) + ")");

      } else if (evtType == "devices") {
        Devices.clear();
        auto e = deserializeJson(Devices, content);
        if (!e) SetDeviceName();

      } else if (evtType == "device_states") {
        DeviceStates.clear();
        auto e = deserializeJson(DeviceStates, content);
        if (!e) processTallyData();

      } else if (evtType == "deviceId") {
        DeviceId = strip_quot(content);
        addLog("deviceId: " + DeviceId);
        preferences.begin("tally-arbiter", false);
        preferences.putString("deviceid", DeviceId);
        preferences.end();
        SetDeviceName();

      } else if (evtType == "reassign") {
        socket_Reassign(content);

      } else if (evtType == "flash") {
        socket_Flash();
      }
      break;
    }
    default: break;
  }
}

/* ── WiFi ── */
void WiFiEvent(WiFiEvent_t event) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      addLog("WiFi OK: " + WiFi.localIP().toString());
      networkConnected = true;
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      addLog("WiFi lost.");
      networkConnected = false;
      socketConnected  = false;
      pendingRegister  = false;
      break;
    default: break;
  }
}

void connectToNetwork() {
  addLog("Connecting to: " + networkSSID);
  WiFi.disconnect(true);
  WiFi.onEvent(WiFiEvent);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  if (USE_STATIC) {
    WiFi.config(clientIp, gateway, subnet);
  }
  WiFi.begin(networkSSID.c_str(), networkPass.c_str());
}

void connectToServer() {
  addLog("Connecting to TA: " + tallyarbiter_host + ":" + String(tallyarbiter_port));
  socket.onEvent(socket_event);
  socket.setReconnectInterval(10000); // retry every 10s instead of hammering
  socket.begin(tallyarbiter_host.c_str(), tallyarbiter_port);
}

/* ── Web server ── */
void handleRoot() {
  String p;
  p.reserve(2048);
  p += "<!DOCTYPE html><html><head>"
       "<meta charset=UTF-8><meta name=viewport content='width=device-width,initial-scale=1'>"
       "<title>Tally Arbiter ESP32</title>"
       "<style>"
       "body{background:#0d0d0d;color:#e0e0e0;font-family:monospace;padding:1.5rem;max-width:520px;margin:auto}"
       "h1{color:#ff2d2d;letter-spacing:.1em;font-size:1.3rem;margin-bottom:.2rem}"
       ".sub{color:#555;font-size:.7rem;letter-spacing:.2em;text-transform:uppercase;margin-bottom:1.5rem}"
       "label{display:block;color:#555;font-size:.7rem;text-transform:uppercase;letter-spacing:.15em;margin-top:1rem;margin-bottom:.3rem}"
       "input{width:100%;background:#111;border:1px solid #2a2a2a;border-radius:3px;color:#e0e0e0;font-family:monospace;font-size:.9rem;padding:.5rem .6rem;box-sizing:border-box}"
       "input:focus{outline:none;border-color:#ff2d2d}"
       ".row{display:flex;gap:.5rem}"
       "button{margin-top:1rem;width:100%;background:#ff2d2d;color:#fff;border:none;border-radius:3px;font-family:monospace;font-size:.85rem;padding:.65rem;cursor:pointer;text-transform:uppercase}"
       "button:hover{background:#c02020}"
       ".btn-pause{background:#333;color:#ffab00;border:1px solid #ffab00;margin-bottom:.5rem}"
       ".btn-pause:hover{background:#444}"
       "#msg{margin-top:.5rem;color:#00e676;font-size:.75rem;min-height:1em;text-align:center}"
       ".grid{display:grid;grid-template-columns:1fr 1fr;gap:.4rem 1.5rem;margin-top:1.5rem;padding:.75rem 1rem;background:#111;border:1px solid #1e1e1e;border-radius:3px}"
       ".sl{color:#555;font-size:.62rem;text-transform:uppercase;letter-spacing:.12em}"
       ".sv{font-size:.85rem;margin-top:1px}"
       ".on{color:#00e676}.off{color:#444}.rd{color:#ff2d2d}"
       "hr{border:none;border-top:1px solid #1e1e1e;margin:1.25rem 0}"
       "#log{background:#050505;border:1px solid #1a1a1a;border-radius:3px;padding:.6rem;height:200px;overflow-y:auto;font-size:.68rem;color:#888;white-space:pre-wrap;word-break:break-all}"
       ".lt{color:#555;font-size:.62rem;text-transform:uppercase;letter-spacing:.12em;margin-bottom:.4rem}"
       "</style></head><body>";

  p += "<h1>&#9673; TALLY ARBITER</h1><div class=sub>ESP32 &mdash; " + listenerDeviceName + "</div>";

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
       "<button class=btn-pause id=pbtn onclick=togglePause()>⏸ Pause Reconnect (edit safely)</button>"
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
       "var paused=false;"
       "function togglePause(){"
       "var btn=document.getElementById('pbtn');"
       "if(!paused){"
       "fetch('/pause');paused=true;"
       "btn.textContent='▶ Resume Reconnect';"
       "btn.style.borderColor='#00e676';btn.style.color='#00e676';"
       "document.getElementById('msg').textContent='Reconnect paused — edit freely then save.';"
       "}else{"
       "fetch('/resume');paused=false;"
       "btn.textContent='⏸ Pause Reconnect (edit safely)';"
       "btn.style.borderColor='#ffab00';btn.style.color='#ffab00';"
       "document.getElementById('msg').textContent='';"
       "}}"
       "function sv(){"
       "var s=document.getElementById('ssid').value,"
       "wp=document.getElementById('wpass').value,"
       "h=document.getElementById('host').value,"
       "pt=document.getElementById('port').value,"
       "d=document.getElementById('did').value,"
       "ln=document.getElementById('lname').value;"
       "if(!s||!h||!pt)return;"
       "document.getElementById('msg').textContent='Saving...';"
       "fetch('/save?ssid='+encodeURIComponent(s)+'&pass='+encodeURIComponent(wp)"
       "+'&host='+encodeURIComponent(h)+'&port='+pt"
       "+'&deviceid='+encodeURIComponent(d)+'&lname='+encodeURIComponent(ln))"
       ".then(r=>r.text()).then(t=>document.getElementById('msg').textContent=t)"
       ".catch(()=>document.getElementById('msg').textContent='Error');}"
       "var l=document.getElementById('log');l.scrollTop=l.scrollHeight;"
       "</script></body></html>";

  webServer.send(200, "text/html", p);
}

void handleSave() {
  // Only overwrite ssid/pass if actually provided — prevents blanking WiFi on TA-only changes
  if (webServer.hasArg("ssid") && webServer.arg("ssid").length() > 0)
    networkSSID = webServer.arg("ssid");
  if (webServer.hasArg("pass") && webServer.arg("pass").length() > 0)
    networkPass = webServer.arg("pass");
  if (webServer.hasArg("host"))     tallyarbiter_host  = webServer.arg("host");
  if (webServer.hasArg("port"))     tallyarbiter_port  = webServer.arg("port").toInt();
  if (webServer.hasArg("deviceid")) DeviceId           = webServer.arg("deviceid");
  if (webServer.hasArg("lname"))    listenerDeviceName = webServer.arg("lname");

  // Save everything in one atomic open/close
  preferences.begin("tally-arbiter", false);
  preferences.putString("ssid",     networkSSID);
  preferences.putString("pass",     networkPass);
  preferences.putString("tahost",   tallyarbiter_host);
  preferences.putInt   ("taport",   tallyarbiter_port);
  preferences.putString("deviceid", DeviceId);
  preferences.putString("lname",    listenerDeviceName);
  preferences.end();

  Serial.println("Web save: tahost=" + tallyarbiter_host + " taport=" + String(tallyarbiter_port));

  webServer.send(200, "text/plain", "Saved! Rebooting...");
  delay(1500);
  ESP.restart();
}

/* ── Setup & Loop ── */
void setup() {
  Serial.begin(115200);
  delay(200);

  // Generate unique name from chip ID
  uint64_t chipid = ESP.getEfuseMac();
  listenerDeviceName = "esp32-" + String((uint16_t)(chipid >> 32), HEX)
                                + String((uint32_t)chipid, HEX);

  // Load saved prefs
  preferences.begin("tally-arbiter", true);
  String h   = preferences.getString("tahost",    "");
  int    po  = preferences.getInt   ("taport",    0);
  String d   = preferences.getString("deviceid",  "");
  String n   = preferences.getString("devicename","");
  String ln  = preferences.getString("lname",     "");
  String ss  = preferences.getString("ssid",      "");
  String pw  = preferences.getString("pass",      "");
  bool   st  = preferences.getBool  ("static",    false);
  int    pr  = preferences.getInt   ("pinr",      RED_PIN);
  int    pg  = preferences.getInt   ("ping",      GREEN_PIN);
  bool   cb  = preferences.getBool  ("cutbus",    true);
  String sip = preferences.getString("sip",       "192.168.1.100");
  String sgw = preferences.getString("sgw",       "192.168.1.1");
  String ssn = preferences.getString("ssn",       "255.255.255.0");
  preferences.end();

  if (h.length()  > 0) tallyarbiter_host  = h;
  if (po          > 0) tallyarbiter_port  = po;
  if (d.length()  > 0) DeviceId           = d;
  if (n.length()  > 0) DeviceName         = n;
  if (ln.length() > 0) listenerDeviceName = ln;
  if (ss.length() > 0) networkSSID        = ss;
  if (pw.length() > 0) networkPass        = pw;
  USE_STATIC = st;
  pinRed     = pr;
  pinGreen   = pg;
  CUT_BUS    = cb;
  clientIp.fromString(sip);
  gateway.fromString(sgw);
  subnet.fromString(ssn);

  pinMode(pinRed,   OUTPUT);
  pinMode(pinGreen, OUTPUT);
  setLEDs(true, true); delay(200); setLEDs(false, false);

  setCpuFrequencyMhz(80);
  addLog("Booting: " + listenerDeviceName);

  // ── Listen for CFG packet from web flasher (2s window) ──
  // If no config has been flashed yet, wait briefly for the
  // web flasher to send settings before proceeding
  if (networkSSID.length() == 0) {
    addLog("No WiFi config. Waiting for CFG packet...");
    // Keep blinking and listening forever until config arrives
    while (networkSSID.length() == 0) {
      setLEDs(true, false); delay(300);
      setLEDs(false, true); delay(300);
      checkSerialConfig(); // this reboots automatically after saving
    }
  }

  connectToNetwork();
  unsigned long t = millis();
  while (!networkConnected) {
    setLEDs(true, false); delay(300);
    setLEDs(false, true); delay(300);
    if (millis() - t > 30000) { addLog("WiFi timeout."); ESP.restart(); }
    checkSerialConfig(); // still handle config while waiting
  }
  setLEDs(false, false);

  webServer.on("/",       handleRoot);
  webServer.on("/save",   handleSave);
  webServer.on("/pause",  []() { socketPaused = true;  webServer.send(200, "text/plain", "paused");  });
  webServer.on("/resume", []() { socketPaused = false; webServer.send(200, "text/plain", "resumed"); });
  webServer.begin();
  addLog("Web UI: http://" + WiFi.localIP().toString());

  connectToServer();
}

void loop() {
  webServer.handleClient();
  if (!socketPaused) socket.loop();
  checkSerialConfig();

  if (pendingRegister && socketConnected) {
    pendingRegister = false;
    doRegister();
  }

  delay(1); // yield to RTOS — prevents watchdog crash under heavy load
}

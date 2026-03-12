/*
 * Tally Arbiter ESP8266 Listener
 * ================================
 * Pin mapping (NodeMCU / D1 Mini):
 *   2-pin mode:  D0 (GPIO16) = Red,  D2 (GPIO4) = Green
 *   RGB mode:    D0 (GPIO16) = Red,  D2 (GPIO4) = Green,  D1 (GPIO5) = Blue
 *                3.3V pin = Common (Anode) or GND = Common (Cathode)
 *
 * Config is stored in LittleFS as /config.json
 * Web flasher sends: CFG:{json}\n via serial to write config
 */

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <WebSocketsClient.h>
#include <SocketIOclient.h>
#include <ArduinoJson.h>
#include <LittleFS.h>

/* ── Pin definitions ── */
#define PIN_RED   16   // D0
#define PIN_GREEN  4   // D2
#define PIN_BLUE   5   // D1

/* ── Default config — overwritten by CFG packet or saved config ── */
// TEST credentials — replace via web UI or CFG packet in production
String networkSSID       = "";
String networkPass       = "";
String tallyarbiter_host = "192.168.1.212";
int    tallyarbiter_port = 4455;
String listenerDeviceName = "esp8266-tally";
String DeviceId          = "unassigned";
String DeviceName        = "Unassigned";

bool CUT_BUS   = true;
bool USE_RGB   = true;   // 4-pin RGB LED
bool RGB_ANODE = true;   // common pin on 3.3V = common anode
bool USE_STATIC = false;

IPAddress clientIp(192, 168, 1, 100);
IPAddress gateway(192, 168, 1, 1);
IPAddress subnet(255, 255, 255, 0);

int pinRed   = PIN_RED;
int pinGreen = PIN_GREEN;
int pinBlue  = PIN_BLUE;

/* ── Runtime state ── */
ESP8266WebServer webServer(80);
SocketIOclient   socket;

StaticJsonDocument<4096> BusOptions;
StaticJsonDocument<8192> Devices;
StaticJsonDocument<4096> DeviceStates;

bool mode_preview     = false;
bool mode_program     = false;
bool networkConnected = false;
bool socketConnected  = false;
bool pendingRegister  = false;
bool socketPaused     = false;

/* ── Log ring buffer ── */
#define LOG_LINES 30
String logBuffer[LOG_LINES];
int    logHead = 0;

void addLog(const String& line) {
  Serial.println(line);
  logBuffer[logHead % LOG_LINES] = line;
  logHead++;
}

/* ════════════════════════════════════════
   LED CONTROL
   ════════════════════════════════════════ */

// GPIO16 (D0) on ESP8266 is special — it has no internal pull-up and can
// float high on boot. Always re-assert pinMode before writing to be safe.
void initLEDPins() {
  pinMode(pinRed,   OUTPUT); digitalWrite(pinRed,   LOW);
  pinMode(pinGreen, OUTPUT); digitalWrite(pinGreen, LOW);
  if (USE_RGB) { pinMode(pinBlue, OUTPUT); digitalWrite(pinBlue, USE_RGB && RGB_ANODE ? HIGH : LOW); }
}

void setLEDs(bool red, bool green) {
  if (USE_RGB) {
    if (RGB_ANODE) {
      // Common Anode (3.3V on common): LOW = on, HIGH = off
      digitalWrite(pinRed,   red   ? LOW  : HIGH);
      digitalWrite(pinGreen, green ? LOW  : HIGH);
      digitalWrite(pinBlue,  HIGH);
    } else {
      // Common Cathode (GND on common): HIGH = on, LOW = off
      digitalWrite(pinRed,   red   ? HIGH : LOW);
      digitalWrite(pinGreen, green ? HIGH : LOW);
      digitalWrite(pinBlue,  LOW);
    }
  } else {
    // 2-pin: D0=Red, D2=Green, each with resistor to GND
    digitalWrite(pinRed,   red   ? HIGH : LOW);
    digitalWrite(pinGreen, green ? HIGH : LOW);
  }
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
    // CUT_BUS: if both buses active, show Red only (program takes priority)
    setLEDs(true, CUT_BUS ? false : true);
  } else {
    addLog("Tally: CLEAR");
    setLEDs(false, false);
  }
}

/* ════════════════════════════════════════
   CONFIG — LittleFS JSON storage
   ════════════════════════════════════════ */
#define CONFIG_FILE "/config.json"

void saveConfig() {
  File f = LittleFS.open(CONFIG_FILE, "w");
  if (!f) { Serial.println("Config write failed"); return; }

  StaticJsonDocument<1024> doc;
  doc["ssid"]     = networkSSID;
  doc["pass"]     = networkPass;
  doc["tahost"]   = tallyarbiter_host;
  doc["taport"]   = tallyarbiter_port;
  doc["lname"]    = listenerDeviceName;
  doc["devid"]    = DeviceId;
  doc["cutbus"]   = CUT_BUS   ? 1 : 0;
  doc["ledtype"]  = USE_RGB   ? "rgb" : "2pin";
  doc["rgbanode"] = RGB_ANODE ? 1 : 0;
  doc["pinr"]     = pinRed;
  doc["ping"]     = pinGreen;
  doc["pinb"]     = pinBlue;
  doc["static"]   = USE_STATIC ? 1 : 0;
  doc["sip"]      = clientIp.toString();
  doc["sgw"]      = gateway.toString();
  doc["ssn"]      = subnet.toString();

  serializeJson(doc, f);
  f.close();
  Serial.println("Config saved to LittleFS.");
}

bool loadConfig() {
  if (!LittleFS.exists(CONFIG_FILE)) return false;
  File f = LittleFS.open(CONFIG_FILE, "r");
  if (!f) return false;

  StaticJsonDocument<1024> doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) { Serial.println("Config parse error: " + String(err.c_str())); return false; }

  if (doc.containsKey("ssid"))     networkSSID         = doc["ssid"].as<String>();
  if (doc.containsKey("pass"))     networkPass         = doc["pass"].as<String>();
  if (doc.containsKey("tahost"))   tallyarbiter_host   = doc["tahost"].as<String>();
  if (doc.containsKey("taport"))   tallyarbiter_port   = doc["taport"].as<int>();
  if (doc.containsKey("lname"))    listenerDeviceName  = doc["lname"].as<String>();
  if (doc.containsKey("devid")) {
    String d = doc["devid"].as<String>();
    // Sanity check — valid IDs are hex strings like "9a15a435", reject short/corrupt values
    if (d.length() >= 6) DeviceId = d;
    else DeviceId = "unassigned";
  }
  if (doc.containsKey("cutbus"))   CUT_BUS             = doc["cutbus"].as<int>() == 1;
  if (doc.containsKey("ledtype"))  USE_RGB             = doc["ledtype"].as<String>() == "rgb";
  if (doc.containsKey("rgbanode")) RGB_ANODE           = doc["rgbanode"].as<int>() == 1;
  if (doc.containsKey("pinr"))     pinRed              = doc["pinr"].as<int>();
  if (doc.containsKey("ping"))     pinGreen            = doc["ping"].as<int>();
  if (doc.containsKey("pinb"))     pinBlue             = doc["pinb"].as<int>();
  if (doc.containsKey("static"))   USE_STATIC          = doc["static"].as<int>() == 1;
  if (doc.containsKey("sip"))      clientIp.fromString(doc["sip"].as<String>());
  if (doc.containsKey("sgw"))      gateway.fromString (doc["sgw"].as<String>());
  if (doc.containsKey("ssn"))      subnet.fromString  (doc["ssn"].as<String>());

  return true;
}

/* ════════════════════════════════════════
   SERIAL CONFIG RECEIVER
   Web flasher sends: CFG:{json}\n
   ════════════════════════════════════════ */
void checkSerialConfig() {
  if (!Serial.available()) return;
  Serial.setTimeout(5000);
  String line = Serial.readStringUntil('\n');
  line.trim();
  if (!line.startsWith("CFG:")) return;

  String json = line.substring(4);
  Serial.println("CFG raw (" + String(json.length()) + " bytes): " + json);

  StaticJsonDocument<1024> cfg;
  DeserializationError err = deserializeJson(cfg, json);
  if (err) { Serial.println("CFG parse error: " + String(err.c_str())); return; }

  Serial.println("CFG received — saving...");

  // Apply values
  if (cfg.containsKey("ssid"))     networkSSID        = cfg["ssid"].as<String>();
  if (cfg.containsKey("pass"))     networkPass        = cfg["pass"].as<String>();
  if (cfg.containsKey("tahost"))   tallyarbiter_host  = cfg["tahost"].as<String>();
  if (cfg.containsKey("taport"))   tallyarbiter_port  = cfg["taport"].as<int>();
  if (cfg.containsKey("lname"))    listenerDeviceName = cfg["lname"].as<String>();
  if (cfg.containsKey("devid"))    DeviceId           = cfg["devid"].as<String>();
  if (cfg.containsKey("cutbus"))   CUT_BUS            = cfg["cutbus"].as<int>() == 1;
  if (cfg.containsKey("ledtype"))  USE_RGB            = cfg["ledtype"].as<String>() == "rgb";
  if (cfg.containsKey("rgbanode")) RGB_ANODE          = cfg["rgbanode"].as<int>() == 1;
  if (cfg.containsKey("pinr"))     pinRed             = cfg["pinr"].as<int>();
  if (cfg.containsKey("ping"))     pinGreen           = cfg["ping"].as<int>();
  if (cfg.containsKey("pinb"))     pinBlue            = cfg["pinb"].as<int>();
  if (cfg.containsKey("static"))   USE_STATIC         = cfg["static"].as<int>() == 1;
  if (cfg.containsKey("sip"))      clientIp.fromString(cfg["sip"].as<String>());
  if (cfg.containsKey("sgw"))      gateway.fromString (cfg["sgw"].as<String>());
  if (cfg.containsKey("ssn"))      subnet.fromString  (cfg["ssn"].as<String>());

  saveConfig();
  Serial.println("CFG saved. Rebooting...");
  delay(500);
  ESP.restart();
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
    bool active = state["sources"].as<JsonArray>().size() > 0;
    if (busType == "preview") mode_preview = active;
    if (busType == "program") mode_program = active;
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
  // Persist updated device name
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
}

void socketEvent(socketIOmessageType_t type, uint8_t* payload, size_t length) {
  switch (type) {

    case sIOtype_CONNECT:
      addLog("Socket.IO connected.");
      // Don't manually send namespace packet — TA handles connection on EIO=3
      socketConnected = true;
      pendingRegister = true;
      break;

    case sIOtype_DISCONNECT:
      addLog("Socket.IO disconnected.");
      socketConnected = false;
      setLEDs(false, false);
      break;

    case sIOtype_EVENT: {
      // Raw data looks like: ["event_name", <payload>]
      // We extract the event name cheaply, then deserialize the payload
      // directly into our pre-allocated documents — no tiny envelope doc.
      String data = String((char*)payload);

      // Extract event name: first string after opening ["
      int nameStart = data.indexOf('"');
      if (nameStart < 0) break;
      int nameEnd = data.indexOf('"', nameStart + 1);
      if (nameEnd < 0) break;
      String event = data.substring(nameStart + 1, nameEnd);
      addLog("RX: " + event);

      // Find where the payload starts (after the event name + comma)
      // data = ["event_name",<payload>]
      int payloadStart = data.indexOf(',', nameEnd);
      if (payloadStart < 0) break;
      payloadStart++; // skip the comma
      // Trim the trailing ] from the Socket.IO envelope
      int payloadEnd = data.lastIndexOf(']');
      if (payloadEnd <= payloadStart) break;
      String payloadStr = data.substring(payloadStart, payloadEnd);
      payloadStr.trim();

      // ── bus_options ──
      if (event == "bus_options") {
        BusOptions.clear();
        DeserializationError e = deserializeJson(BusOptions, payloadStr);
        if (!e) addLog("bus_options OK (" + String(BusOptions.as<JsonArray>().size()) + ")");
        else    addLog("bus_options ERR: " + String(e.c_str()));

      // ── devices ──
      } else if (event == "devices") {
        Devices.clear();
        DeserializationError e = deserializeJson(Devices, payloadStr);
        if (!e) { SetDeviceName(); }
        else    addLog("devices ERR: " + String(e.c_str()));

      // ── device_states ──
      } else if (event == "device_states") {
        DeviceStates.clear();
        DeserializationError e = deserializeJson(DeviceStates, payloadStr);
        if (!e) {
          addLog("device_states OK (" + String(DeviceStates.as<JsonArray>().size()) + ")");
          processTallyData();
        } else {
          addLog("device_states ERR: " + String(e.c_str()));
        }

      // ── reassign ──
      // TA sends: ["reassign","oldDeviceId","newDeviceId","listenerClientId"]
      } else if (event == "reassign") {
        addLog("Reassign raw: " + data.substring(0, 80));
        // data = ["reassign","oldId","newId","listenerId"]
        // Find the 4 quoted string values by counting through quote pairs
        // Pair 0 = "reassign", Pair 1 = oldId, Pair 2 = newId, Pair 3 = listenerId
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
          // tokens[0]="reassign", tokens[1]=oldId, tokens[2]=newId
          String oldId = tokens[1];
          String newId = tokens[2];
          addLog("Reassign: " + oldId + " -> " + newId);
          DeviceId = newId;
          saveConfig();
          String rPayload = "{"
            "\"oldDeviceId\":\"" + oldId + "\","
            "\"newDeviceId\":\"" + newId + "\""
            "}";
          ws_emit("listener_reassign_object", rPayload.c_str());
          ws_emit("devices");
        } else {
          addLog("Reassign: could not parse (" + String(found) + " tokens)");
        }

      // ── flash ──
      } else if (event == "flash") {
        addLog("Flash command received.");
        // Save current state before flashing
        bool prev_preview = mode_preview;
        bool prev_program = mode_program;
        for (int i = 0; i < 3; i++) {
          setLEDs(true,  false); delay(150);
          setLEDs(false, true);  delay(150);
        }
        setLEDs(false, false);
        // Restore state explicitly — don't rely on globals being intact
        // after blocking delays (socket may have missed updates)
        mode_preview = prev_preview;
        mode_program = prev_program;
        evaluateMode();

      // ── error ──
      } else if (event == "error") {
        addLog("Server error received.");
      }
      break;
    }

    default: break;
  }
}

void connectToServer() {
  addLog("Connecting to TA: " + tallyarbiter_host + ":" + String(tallyarbiter_port));
  socket.begin(tallyarbiter_host, tallyarbiter_port, "/socket.io/?EIO=3");
  socket.onEvent(socketEvent);
  socket.setReconnectInterval(5000);
}

/* ════════════════════════════════════════
   WEB UI
   ════════════════════════════════════════ */
void handleRoot() {
  String p = "<!DOCTYPE html><html><head>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>Tally " + listenerDeviceName + "</title>"
    "<style>"
    "body{font-family:monospace;background:#0a0a0a;color:#e0e0e0;max-width:480px;margin:0 auto;padding:1rem}"
    "h2{color:#ff2d2d;letter-spacing:.1em}"
    "label{display:block;font-size:.7rem;color:#888;margin:.8rem 0 .2rem;text-transform:uppercase;letter-spacing:.1em}"
    "input{width:100%;background:#1a1a1a;border:1px solid #333;color:#e0e0e0;padding:.5rem;border-radius:3px;box-sizing:border-box;font-family:monospace}"
    "button{margin-top:.75rem;padding:.6rem 1.2rem;background:#ff2d2d;color:#fff;border:none;border-radius:3px;cursor:pointer;font-family:monospace;letter-spacing:.1em}"
    ".row{display:flex;gap:.5rem}"
    ".row input:last-child{max-width:90px}"
    ".sv{display:flex;justify-content:space-between;padding:.35rem 0;border-bottom:1px solid #1f1f1f;font-size:.75rem}"
    ".sl{color:#666;text-transform:uppercase;letter-spacing:.08em}"
    ".on{color:#00e676}.off{color:#ff2d2d}"
    "#log{background:#050505;border:1px solid #1f1f1f;padding:.5rem;height:180px;overflow-y:auto;font-size:.65rem;color:#555;white-space:pre-wrap;margin-top:.5rem}"
    ".lt{font-size:.6rem;color:#ff2d2d;letter-spacing:.2em;text-transform:uppercase;margin-top:1rem}"
    ".btn-pause{background:transparent;border:1px solid #ffab00;color:#ffab00;display:block;width:100%;margin-bottom:.5rem}"
    "</style></head><body>"
    "<h2>Tally Arbiter</h2>"
    "<div class=sv><span class=sl>Device</span><span>" + listenerDeviceName + "</span></div>"
    "<div class=sv><span class=sl>MAC</span><span>" + WiFi.macAddress() + "</span></div>"
    "<div class=sv><span class=sl>IP</span><span>" + WiFi.localIP().toString() + "</span></div>"
    "<div class=sv><span class=sl>WiFi</span><span class='" + String(networkConnected ? "on'>CONNECTED" : "off'>OFFLINE") + "</span></div>"
    "<div class=sv><span class=sl>TA Server</span><span>" + tallyarbiter_host + ":" + String(tallyarbiter_port) + "</span></div>"
    "<div class=sv><span class=sl>Assigned Device</span><span>" + DeviceName + " (" + DeviceId + ")</span></div>"
    "<div class=sv><span class=sl>LED Type</span><span>" + String(USE_RGB ? "RGB 4-pin" : "2-pin") + (USE_RGB ? (RGB_ANODE ? " Anode" : " Cathode") : "") + "</span></div>"
    "<hr>"
    "<label>WiFi SSID</label><input type=text id=ssid value='" + networkSSID + "'>"
    "<label>WiFi Password</label><input type=password id=wpass placeholder='(unchanged)'>"
    "<label>Tally Arbiter IP</label>"
    "<div class=row>"
    "<input type=text id=host value='" + tallyarbiter_host + "'>"
    "<input type=number id=port value='" + String(tallyarbiter_port) + "'>"
    "</div>"
    "<label>Listener Name</label><input type=text id=lname value='" + listenerDeviceName + "'>"
    "<label>Device ID</label><input type=text id=did value='" + DeviceId + "'>"
    "<button class=btn-pause id=pbtn onclick=togglePause()>⏸ Pause Reconnect (edit safely)</button>"
    "<button onclick=sv()>Save &amp; Reboot</button>"
    "<div id=msg></div>"
    "<div class=lt>Debug Log</div>"
    "<div id=log>";

  int start = (logHead >= LOG_LINES) ? logHead : 0;
  for (int i = 0; i < LOG_LINES; i++) {
    const String& line = logBuffer[(start + i) % LOG_LINES];
    if (line.length()) { p += line; p += "\n"; }
  }
  p += "</div>"
    "<script>"
    "var paused=false;"
    "function togglePause(){"
    "var btn=document.getElementById('pbtn');"
    "if(!paused){"
    "fetch('/pause');paused=true;"
    "btn.textContent='▶ Resume Reconnect';"
    "btn.style.borderColor='#00e676';btn.style.color='#00e676';"
    "document.getElementById('msg').textContent='Paused — edit freely then save.';"
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
  if (webServer.hasArg("ssid") && webServer.arg("ssid").length() > 0)
    networkSSID = webServer.arg("ssid");
  if (webServer.hasArg("pass") && webServer.arg("pass").length() > 0)
    networkPass = webServer.arg("pass");
  if (webServer.hasArg("host"))     tallyarbiter_host  = webServer.arg("host");
  if (webServer.hasArg("port"))     tallyarbiter_port  = webServer.arg("port").toInt();
  if (webServer.hasArg("deviceid")) DeviceId           = webServer.arg("deviceid");
  if (webServer.hasArg("lname"))    listenerDeviceName = webServer.arg("lname");

  saveConfig();
  webServer.send(200, "text/plain", "Saved! Rebooting...");
  delay(1500);
  ESP.restart();
}

/* ════════════════════════════════════════
   NETWORK
   ════════════════════════════════════════ */
void WiFiEventConnected(const WiFiEventStationModeConnected& e) {
  // Keep short — called from network stack context
  (void)e;
}

void WiFiEventGotIP(const WiFiEventStationModeGotIP& e) {
  networkConnected = true;
  // Full log happens in setup() after the blocking WiFi wait loop exits
  (void)e;
}

void WiFiEventDisconnected(const WiFiEventStationModeDisconnected& e) {
  networkConnected = false;
  socketConnected  = false;
  pendingRegister  = false;
  (void)e;
  // WiFi library handles auto-reconnect via WiFi.reconnect() called in setup
}

void connectToNetwork() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleepMode(WIFI_NONE_SLEEP);
  if (USE_STATIC) WiFi.config(clientIp, gateway, subnet);
  WiFi.begin(networkSSID.c_str(), networkPass.c_str());
  addLog("WiFi connecting: " + networkSSID);
}

/* ════════════════════════════════════════
   SETUP & LOOP
   ════════════════════════════════════════ */
void setup() {
  Serial.begin(115200);
  delay(200);

  // Generate unique device name from chip ID
  listenerDeviceName = "esp8266-" + String(ESP.getChipId(), HEX);

  // Mount filesystem and load saved config
  if (!LittleFS.begin()) {
    Serial.println("LittleFS mount failed — formatting...");
    LittleFS.format();
    LittleFS.begin();
  }

  bool hadSavedConfig = loadConfig();

  // Set up LED pins — initLEDPins handles GPIO16 quirk
  initLEDPins();

  // Startup flash: both on briefly then off
  setLEDs(true, true); delay(300); setLEDs(false, false); delay(100);

  // Print boot info
  Serial.println("=== Tally Arbiter ESP8266 ===");
  Serial.println("Listener : " + listenerDeviceName);
  Serial.println("MAC Addr : " + WiFi.macAddress());
  Serial.println("WiFi SSID: " + networkSSID);
  Serial.println("TA Host  : " + tallyarbiter_host);
  Serial.println("TA Port  : " + String(tallyarbiter_port));
  Serial.println("Device ID: " + DeviceId);
  Serial.println("LED Type : " + String(USE_RGB ? "RGB" : "2-pin") +
                 (USE_RGB ? (RGB_ANODE ? " Common-Anode" : " Common-Cathode") : ""));
  Serial.println("Config   : " + String(hadSavedConfig ? "Loaded from flash" : "Using defaults"));
  Serial.println("=============================");

  // Register WiFi event handlers
  static WiFiEventHandler h1, h2, h3;
  h1 = WiFi.onStationModeConnected(WiFiEventConnected);
  h2 = WiFi.onStationModeGotIP(WiFiEventGotIP);
  h3 = WiFi.onStationModeDisconnected(WiFiEventDisconnected);

  // If no WiFi config stored, wait forever for CFG packet
  if (networkSSID.length() == 0) {
    Serial.println("No WiFi config. Waiting for CFG packet...");
    while (networkSSID.length() == 0) {
      setLEDs(true, false);  delay(300);
      setLEDs(false, true);  delay(300);
      checkSerialConfig();   // saves & reboots if received
      yield();               // ESP8266 needs explicit yield to avoid WDT reset
    }
  }

  connectToNetwork();

  // Wait for WiFi with 30s timeout, keep checking for CFG while waiting
  unsigned long t = millis();
  while (!networkConnected) {
    setLEDs(true, false);  delay(300);
    setLEDs(false, true);  delay(300);
    checkSerialConfig();
    yield();
    if (millis() - t > 30000) {
      addLog("WiFi timeout — rebooting.");
      ESP.restart();
    }
  }
  // Re-init pins after blink loop — GPIO16 can drift
  initLEDPins();
  setLEDs(false, false);
  addLog("WiFi OK: " + WiFi.localIP().toString());

  // Start web server
  webServer.on("/",             handleRoot);
  webServer.on("/save",         handleSave);
  webServer.on("/pause",        []() { socketPaused = true;  webServer.send(200, "text/plain", "paused");  });
  webServer.on("/resume",       []() { socketPaused = false; webServer.send(200, "text/plain", "resumed"); });
  webServer.on("/resetconfig",  []() {
    LittleFS.remove(CONFIG_FILE);
    webServer.send(200, "text/plain", "Config cleared. Rebooting...");
    delay(1000);
    ESP.restart();
  });
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

  yield(); // ESP8266 must yield regularly — no RTOS, no delay(1) needed
}

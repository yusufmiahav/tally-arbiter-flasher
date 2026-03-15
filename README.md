# Tally Arbiter ESP Flasher

A browser-based flasher for ESP32 and ESP8266 tally listener devices, built for use with [**Tally Arbiter**](https://github.com/josephdadams/TallyArbiter) by [Joseph Adams](https://github.com/josephdadams).

> Tally Arbiter is a free, open-source tally light server that works with virtually any video production software and hardware. This flasher makes it easy to get ESP32 and ESP8266 devices running as Tally Arbiter listeners with no software installation required.

**Live flasher:** https://yusufmiahav.github.io/tally-arbiter-flasher

---

## Supported Boards

| Board | Chip | Display | Notes |
|---|---|---|---|
| ESP32 DevKit | ESP32 | External LED(s) | 30/38 pin board |
| ESP8266 D1 Mini | ESP8266 | External LED(s) | Single binary, LittleFS config |
| NodeMCU v2 | ESP8266 | External LED(s) | Same firmware as D1 Mini |
| Arduino UNO R4 WiFi | Renesas RA4M1 | Built-in 12×8 red LED matrix | **Arduino IDE only** — see below |

---

## Default Wiring

### ESP32 — 2-Pin (Red + Green)

| LED | GPIO | Pin Label | Resistor |
|---|---|---|---|
| Red — Program | GPIO 23 | D23 | 220Ω to GND |
| Green — Preview | GPIO 22 | D22 | 220Ω to GND |

### ESP32 — 4-Pin RGB

| LED Pin | GPIO | Pin Label | Resistor |
|---|---|---|---|
| Red | GPIO 23 | D23 | 220Ω |
| Green | GPIO 22 | D22 | 220Ω |
| Blue | GPIO 21 | D21 | 220Ω |
| Common Anode (+) | 3.3V | 3V3 | — |
| Common Cathode (−) | GND | GND | — |

> Common Anode: 4th pin to 3.3V. Common Cathode: 4th pin to GND.

---

### ESP8266 — 2-Pin (Red + Green)

| LED | GPIO | D1 Mini Label | Resistor |
|---|---|---|---|
| Red — Program | GPIO 16 | D0 | 220Ω to GND |
| Green — Preview | GPIO 4 | D2 | 220Ω to GND |

> GPIO16 (D0) has no internal pull-up — always use a resistor, never leave floating.

### ESP8266 — 4-Pin RGB

| LED Pin | GPIO | D1 Mini Label | Resistor |
|---|---|---|---|
| Red | GPIO 16 | D0 | 220Ω |
| Green | GPIO 4 | D2 | 220Ω |
| Blue | GPIO 5 | D1 | 220Ω |
| Common Anode (+) | 3.3V | 3V3 | — |
| Common Cathode (−) | GND | GND | — |

---

### Arduino UNO R4 WiFi

No external wiring needed. The board has a built-in **12×8 red LED matrix** (96 pixels) used for all tally feedback.

> **The web flasher does not support the UNO R4 WiFi.** The UNO R4 uses a different flashing protocol (USB DFU) that is incompatible with esptool.js. Use Arduino IDE instead — see the [Arduino UNO R4 WiFi](#arduino-uno-r4-wifi-arduino-ide) section below.

| State | Matrix Display |
|---|---|
| No config | Scrolls `" NO CONFIG  CONNECT USB "` |
| WiFi connecting | Scrolls `" CONNECTING TO WIFI "` |
| WiFi connected | Scrolls IP address, then clears |
| TA connecting | Scrolls `" TA: <host> "` |
| Preview | Border outline — hollow rectangle |
| Program | Full matrix solid — all 96 LEDs on |
| Program + Preview | Full matrix solid |
| Flash command | Current state blinks off/on × 3 |
| TA offline | Scrolls `" TA OFFLINE "` |
| Clear / Idle | Matrix off |

---

## LED States — ESP32 / ESP8266

| State | 2-Pin | RGB |
|---|---|---|
| Connecting to WiFi | — | Blue pulsing |
| No config / waiting | Red/Green alternating fast | Red/Green alternating fast |
| Preview | Green solid | Green solid |
| Program | Red solid | Red solid |
| Program + Preview (CUT BUS off) | Both on | Yellow |
| Program + Preview (CUT BUS on) | Red only | Red only |
| Tally Arbiter Flash — Program | Red flashing × 3 | Red → Green → Blue × 3 |
| Tally Arbiter Flash — Preview | Green flashing × 3 | Red → Green → Blue × 3 |
| Tally Arbiter Flash — Clear | Red+Green flashing × 3 | Red → Green → Blue × 3 |
| Idle / Clear | Off | Off |

> **2-pin flash:** flashes whichever colour was active before the flash command. If idle/clear, both LEDs flash together.  
> **RGB flash:** always cycles Red → Green → Blue × 3 regardless of tally state, then returns to previous state.

---

## How to Flash — ESP32 / ESP8266

1. Open the flasher in **Chrome or Edge** (v89+)
2. Select your board type — ESP32 or ESP8266
3. Enter your WiFi SSID/password and Tally Arbiter server IP
4. Choose LED type (2-pin or 4-pin RGB) and confirm pin numbers
5. Click **Connect & Flash** and select the COM port when prompted
6. **ESP32 only:** hold **BOOT** while clicking if it hangs at "Connecting..."
7. WiFi and TA settings are baked directly into the binary at flash time — no serial config step needed

---

## Arduino UNO R4 WiFi — Arduino IDE

The UNO R4 WiFi uses a different flashing protocol that is not supported by the web flasher. Use Arduino IDE instead.

### Setup

1. Install [Arduino IDE 2.x](https://www.arduino.cc/en/software)
2. In Arduino IDE go to **Board Manager** and install **Arduino UNO R4 Boards**
3. Install these libraries via **Library Manager**:
   - `ArduinoJson` (v6.x)
   - `ArduinoGraphics`
   - `Arduino_LED_Matrix` is bundled with the board package — no separate install needed
4. Download the sketch: [`firmware-src/tally-arbiter-unor4wifi/tally-arbiter-unor4wifi.ino`](firmware-src/tally-arbiter-unor4wifi/tally-arbiter-unor4wifi.ino)

### Configure

Open the sketch and edit these lines near the top:

```cpp
String networkSSID        = "YourWiFiName";
String networkPass        = "YourWiFiPassword";
String tallyarbiter_host  = "192.168.1.x";
int    tallyarbiter_port  = 4455;
String listenerDeviceName = "unor4-tally-1";
```

### Upload

1. Connect the UNO R4 via USB
2. Select **Tools → Board → Arduino UNO R4 WiFi**
3. Select the correct port under **Tools → Port**
4. Click **Upload**

### After Upload

The LED matrix will scroll `" CONNECTING TO WIFI "` then show the IP address once connected. Browse to that IP to access the web UI and change settings without re-uploading.

---

## Changing Settings After Flash

### Finding the Device IP

The device IP is printed in the serial monitor on boot. You can access it from:

- **Screen 1** — the ⬡ Serial Monitor button at the bottom of the settings page opens a serial monitor without leaving the config screen
- **Screen 3** (ESP8266) — serial monitor is embedded directly on the post-flash instructions page
- **Screen 4** (ESP32) — connect the serial monitor and press **EN/RST** to see the boot log. The IP appears as `WiFi OK: 192.168.x.x`
- **Screen 5** — the standalone serial monitor, accessible from any screen via the ⬡ Serial Monitor button
- **Your router's DHCP client list** — look for a device named `esp32-xxxxxx` or `esp8266-xxxxxx`

### ESP32

Once you have the IP, open `http://<device-ip>` in a browser to access the web UI — update WiFi, TA server IP, listener name, or device ID and click Save. The device will reboot with the new settings.

Alternatively, reflash from the flasher with updated settings on screen 1 — the CFG packet is sent automatically after flashing.

### ESP8266

Once you have the IP, open `http://<device-ip>` in a browser to access the web UI — update any settings and save. The device reboots with the new settings.

Reflashing from the flasher will also override saved settings automatically — the new WiFi and TA credentials are baked into the binary at flash time.

To factory reset an ESP8266: browse to `http://<device-ip>/resetconfig`

---

## Building from Source

Firmware is compiled automatically by GitHub Actions on every push to `firmware-src/`. Binaries are committed to `firmware/`.

**Required Arduino libraries:**
- `WebSockets` by Markus Sattler (v2.4.1) — ESP32 and ESP8266
- `ArduinoJson` (v6.x)
- `ArduinoGraphics` — UNO R4 only
- `Arduino_LED_Matrix` — UNO R4 only, bundled with the board package
- ESP32 board package: `esp32:esp32@2.0.14`
- ESP8266 board package: `esp8266:esp8266@3.1.2`
- UNO R4 board package: `arduino:renesas_uno@1.2.2` (Arduino IDE, not CI)

---

## Credits

- **[Tally Arbiter](https://github.com/josephdadams/TallyArbiter)** by [Joseph Adams](https://github.com/josephdadams) — the tally management server this firmware connects to. All tally logic, device states, and bus management are handled by Tally Arbiter.
- **[esptool-js](https://github.com/espressif/esptool-js)** by Espressif — Web Serial flashing library used in the browser flasher.
- **[arduinoWebSockets](https://github.com/Links2004/arduinoWebSockets)** by Markus Sattler — Socket.IO library used in the firmware.
- **[ArduinoJson](https://arduinojson.org/)** by Benoit Blanchon — JSON parsing library used in the firmware.

---

## Browser Requirements

- Google Chrome v89+ or Microsoft Edge v89+
- Firefox and Safari do not support the Web Serial API

# Tally Arbiter ESP Flasher

A browser-based flasher for ESP32 and ESP8266 tally listener devices, built for use with [**Tally Arbiter**](https://github.com/josephdadams/TallyArbiter) by [Joseph Adams](https://github.com/josephdadams).

> Tally Arbiter is a free, open-source tally light server that works with virtually any video production software and hardware. This flasher makes it easy to get ESP32 and ESP8266 devices running as Tally Arbiter listeners with no software installation required.

**Live flasher:** https://yusufmiahav.github.io/tally-arbiter-flasher

---

## Supported Boards

| Board | Chip | Notes |
|---|---|---|
| ESP32 DevKit | ESP32 | 30/38 pin board |
| ESP8266 D1 Mini | ESP8266 | Single binary, LittleFS config |
| NodeMCU v2 | ESP8266 | Same firmware as D1 Mini |

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

## LED States

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

## How to Flash

1. Open the flasher in **Chrome or Edge** (v89+)
2. Select your board type — ESP32 or ESP8266
3. Enter your WiFi SSID/password and Tally Arbiter server IP
4. Choose LED type (2-pin or 4-pin RGB) and confirm pin numbers
5. Click **Connect & Flash** and select the COM port when prompted
6. **ESP32 only:** hold **BOOT** while clicking if it hangs at "Connecting..."
7. WiFi and TA settings are baked directly into the binary at flash time — no serial config step needed

---

## Changing Settings After Flash

**ESP32:** Use the Send Config step in the flasher (press EN/RST when prompted), or simply reflash with updated settings.

**ESP8266:** Browse to the device IP in a browser — the built-in web UI lets you update WiFi, TA server IP, listener name, LED pins, and static IP. Find the IP in the serial monitor on screen 3 of the flasher after the device boots. Reflashing with new settings on screen 1 will also override saved config automatically.

To factory reset an ESP8266: browse to `http://<device-ip>/resetconfig`

---

## Building from Source

Firmware is compiled automatically by GitHub Actions on every push to `firmware-src/`. Binaries are committed to `firmware/`.

**Required Arduino libraries:**
- `WebSockets` by Markus Sattler (v2.4.1)
- `ArduinoJson` (v6.x)
- ESP32 board package: `esp32:esp32@2.0.14`
- ESP8266 board package: `esp8266:esp8266@3.1.2`

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

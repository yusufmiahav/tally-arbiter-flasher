# Tally Arbiter ESP Flasher

A browser-based flasher for [Tally Arbiter](https://github.com/josephdadams/TallyArbiter) tally listeners running on ESP32 and ESP8266 devices. No software installation required — works entirely in Chrome or Edge via the Web Serial API.

**Live flasher:** https://yusufmiahav.github.io/tally-arbiter-flasher

---

## Supported Boards

| Board | Chip | Flash Address | Notes |
|---|---|---|---|
| ESP32 DevKit | ESP32 | 0x1000 / 0x8000 / 0x10000 | Bootloader + partitions + firmware |
| ESP8266 D1 Mini | ESP8266 | 0x0 | Single binary, LittleFS config storage |
| NodeMCU v2 | ESP8266 | 0x0 | Same firmware as D1 Mini |

---

## Default Wiring

### ESP32 — 2-Pin (Red + Green)

| LED | GPIO | Pin Label | Resistor |
|---|---|---|---|
| Red (Program) | GPIO 23 | D23 | 220Ω to GND |
| Green (Preview) | GPIO 22 | D22 | 220Ω to GND |

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
| Red (Program) | GPIO 16 | D0 | 220Ω to GND |
| Green (Preview) | GPIO 4 | D2 | 220Ω to GND |

### ESP8266 — 4-Pin RGB

| LED Pin | GPIO | D1 Mini Label | Resistor |
|---|---|---|---|
| Red | GPIO 16 | D0 | 220Ω |
| Green | GPIO 4 | D2 | 220Ω |
| Blue | GPIO 5 | D1 | 220Ω |
| Common Anode (+) | 3.3V | 3V3 | — |
| Common Cathode (−) | GND | GND | — |

> GPIO16 (D0) has no internal pull-up — always connect via resistor, never float.

---

## LED States

| State | 2-Pin | RGB |
|---|---|---|
| Connecting to WiFi | — | Blue pulsing |
| No config / waiting | Red/Green alternating | Red/Green alternating |
| Preview | Green solid | Green solid |
| Program | Red solid | Red solid |
| Program + Preview (CUT BUS off) | Both on | Yellow |
| Program + Preview (CUT BUS on) | Red only | Red only |
| Tally Arbiter Flash | — | Red → Green → Blue × 3 |
| Clear / Idle | Off | Off |

---

## How to Flash

1. Open the flasher in **Chrome or Edge** (v89+)
2. Select your board type (ESP32 or ESP8266)
3. Enter your WiFi credentials and Tally Arbiter server IP
4. Configure LED type and pins
5. Click **Connect & Flash** and select the serial port
6. For ESP32: hold **BOOT** while clicking if it hangs at "Connecting..."
7. Settings are baked into the firmware binary at flash time

---

## Changing Settings After Flash

**ESP32:** Use the Send Config step in the flasher, or reflash with new settings.

**ESP8266:** Browse to the device IP in a browser — the built-in web UI lets you update WiFi, TA server, listener name, LED pins, and static IP. The device IP is shown in the serial monitor on screen 3 of the flasher after it connects.

To reset an ESP8266 to factory defaults: browse to `http://<device-ip>/resetconfig`

---

## Building from Source

Firmware is compiled automatically by GitHub Actions on every push to `firmware-src/`.  
Binaries are committed to the `firmware/` folder.

**Required Arduino libraries:**
- `WebSockets` by Markus Sattler (v2.4.1)
- `ArduinoJson` (v6.x)
- ESP32 board package: `esp32:esp32@2.0.14`
- ESP8266 board package: `esp8266:esp8266@3.1.2`

---

## Browser Requirements

- Google Chrome v89+ or Microsoft Edge v89+
- Firefox and Safari do not support the Web Serial API

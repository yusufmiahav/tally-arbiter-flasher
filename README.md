# Tally Arbiter ESP32 LED Tally Listener

A web-based firmware flasher for ESP32 devices running as LED tally listeners for [Tally Arbiter](https://github.com/josephdadams/TallyArbiter) — the open-source multi-source tally data routing application by Joseph Adams.

**Live flasher → `https://yusufmiahav.github.io/tally-arbiter-flasher`**

---

## What this is

This project turns an ESP32 microcontroller into a physical tally light using two LEDs — one for **Program** (red) and one for **Preview** (green). It connects to a [Tally Arbiter](https://github.com/josephdadams/TallyArbiter) server over WiFi and responds in real time to tally state changes from any connected video production source.

When a camera or source is taken to **Program**, the red LED lights up. When it is placed in **Preview**, the green LED lights up. When both states are active simultaneously, the behaviour is controlled by the CUT_BUS setting — either red only, or both LEDs on.

The device registers itself in the Tally Arbiter **Connected Listeners** panel under a unique name derived from the ESP32 chip ID, and can be reassigned to any device from within the Tally Arbiter UI without reflashing.

---

## How it works

The flasher is a single HTML page hosted on GitHub Pages. It uses **esptool-js** — Espressif's official browser-based flash library — combined with the **Web Serial API** built into Chrome and Edge. No software installation, drivers, or command line tools are required.

When you click Flash, the page fetches the pre-compiled firmware binary directly from this repository and writes it to the connected ESP32 over USB at 921600 baud. The bootloader, partition table, and application binary are all written in a single operation.

After the firmware is written, the page opens a serial connection at 115200 baud and sends a `CFG:{...}` JSON packet containing your WiFi credentials and Tally Arbiter server details. The firmware receives this on first boot and saves everything to the ESP32's non-volatile storage using the Arduino `Preferences` library. The device then reboots, connects to WiFi, and registers with Tally Arbiter automatically — no further configuration needed.

If settings ever need to be changed after flashing, the firmware hosts a small web interface accessible at the device's IP address on the local network.

GitHub Actions automatically recompiles the firmware binary whenever the source code is updated, so the flasher always serves the latest version.

---

## Tally Arbiter

This listener is designed to work with [Tally Arbiter](https://github.com/josephdadams/TallyArbiter) by Joseph Adams — a free, open-source tally data router that aggregates tally information from multiple production switchers and video sources and distributes it to listener clients across a network.

Tally Arbiter supports a wide range of sources including ATEM, vMix, OBS, Tricaster, and many others, and can simultaneously drive multiple listener types including web, mobile, hardware, and networked devices.

---

## Browser support

| Browser | Supported |
|---------|-----------|
| Chrome 89+ | ✅ |
| Edge 89+ | ✅ |
| Firefox | ❌ No Web Serial API |
| Safari | ❌ No Web Serial API |

---

## License

MIT

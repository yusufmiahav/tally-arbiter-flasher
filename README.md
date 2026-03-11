# Tally Arbiter ESP32 Web Flasher

A browser-based flasher for the Tally Arbiter ESP32 listener firmware.  
No Arduino IDE required — plug in your ESP32, fill in your settings, click Flash.

**Live flasher → `https://YOUR-USERNAME.github.io/tally-arbiter-flasher`**

---

## What it does

1. You open the GitHub Pages URL in Chrome or Edge
2. Fill in WiFi SSID/password and Tally Arbiter server IP/port
3. Plug your ESP32 in via USB and click **Connect Device**
4. Click **Flash Firmware** — the pre-compiled binary is written directly to the ESP32 via Web Serial

No drivers, no IDE, no command line needed.

---

## Repository structure

```
tally-arbiter-flasher/
├── index.html                        ← The web flasher UI (GitHub Pages)
├── firmware/
│   ├── tally-arbiter-esp32.bin       ← Compiled firmware (auto-built by CI)
│   ├── bootloader.bin                ← ESP32 bootloader
│   └── partitions.bin                ← Partition table
├── firmware-src/
│   └── tally-arbiter-esp32/
│       └── tally-arbiter-esp32.ino   ← Arduino source
└── .github/
    └── workflows/
        └── build.yml                 ← Auto-compile action
```

---

## Setup instructions

### 1. Create the GitHub repo

```bash
# Create a new repo called: tally-arbiter-flasher
# Then clone it locally
git clone https://github.com/YOUR-USERNAME/tally-arbiter-flasher.git
cd tally-arbiter-flasher
```

### 2. Add your files

Copy the files from this project into the repo:
- `index.html` → root
- `.github/workflows/build.yml` → as shown
- Your `.ino` sketch → `firmware-src/tally-arbiter-esp32/tally-arbiter-esp32.ino`

### 3. Compile firmware manually the first time

You need to pre-compile and commit the `.bin` files once so the flasher has something to use.

**Option A — Arduino IDE:**
1. Open your `.ino` sketch
2. Go to **Sketch → Export Compiled Binary**
3. Find the three `.bin` files in the sketch folder
4. Rename and copy them to `firmware/` in the repo

**Option B — Let GitHub Actions do it:**
- Push your `.ino` to `firmware-src/`
- The `build.yml` workflow will compile and commit the binaries automatically

### 4. Enable GitHub Pages

1. Go to your repo **Settings → Pages**
2. Source: **Deploy from a branch**
3. Branch: `main` / folder: `/ (root)`
4. Save — your flasher will be live at `https://YOUR-USERNAME.github.io/tally-arbiter-flasher`

---

## How flashing works

The flasher uses **esptool-js** (the official Espressif browser-based flash tool) loaded from CDN.  
It communicates with the ESP32 via the **Web Serial API** — built into Chrome/Edge, no plugins needed.

After flashing the binary, the flasher opens the serial port at 115200 baud and sends a `CFG:{...}` JSON packet. The firmware catches this on first boot and saves the settings to NVS (non-volatile storage) via the `Preferences` library — so your WiFi and TA config is baked in without recompiling.

---

## Browser support

| Browser | Supported |
|---------|-----------|
| Chrome 89+ | ✅ Yes |
| Edge 89+   | ✅ Yes |
| Firefox    | ❌ No (no Web Serial API) |
| Safari     | ❌ No (no Web Serial API) |

---

## Firmware features

- Connects to Tally Arbiter as a **listenerclient** (shows in Connected Listeners panel)
- Red LED = Program, Green LED = Preview
- CUT_BUS mode: Program+Preview shows Red only (or both on)
- Web config UI at device IP for adjusting settings without reflashing
- Unique listener name auto-generated from ESP32 chip ID
- Watchdog reboot on WiFi timeout
- 30-line debug log visible in web UI

---

## Updating firmware

When you push changes to `firmware-src/`, GitHub Actions automatically:
1. Compiles the new sketch using Arduino CLI
2. Commits the updated `.bin` files to `firmware/`

Anyone who visits the flasher page will always get the latest version.

---

## License

MIT

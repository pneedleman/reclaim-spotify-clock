# ⏰ Reclaim Spotify Bedside Clock

A distraction-free ESP32-S3 smart bedside clock built for kids and teens. Features physical rotary controls, Spotify Connect streaming, gradual sunrise lighting via Philips Hue, local weather, and school calendar sync—**with 100% zero microphones, cameras, or ad screens**.

[![Reclaim The Day Project Page](https://img.shields.io/badge/Project_Story_%26_Parts_List-reclaimthe.day%2Fspotify--clock-D38865?style=for-the-badge)](https://reclaimthe.day/spotify-clock)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg?style=for-the-badge)](LICENSE)

---

## 🌟 Story & Overview

Modern bedside gadgets fall into two extremes: cheap piezo screamers that cause morning fight-or-flight cortisol spikes, or big-tech smart screens (Echo Show, Nest Hub) with always-listening microphones sitting 3 feet from where your child sleeps.

This project offers a **sane alternative**:
- **Tactile Rotary Dial**: Turn to scroll screens or volume, tap to play/pause/snooze, double-tap to skip tracks.
- **Gentle Blue LCD (16x2)**: Soft retro-blue backlight that automatically dims to a 1% night glow. Zero touchscreens or YouTube rabbit holes.
- **100% Privacy by Design**: Physically zero microphones, zero camera lenses, and zero tracking chips.
- **Sunrise Light Fade**: Slowly ramps up soft amber light on bedroom Philips Hue bulbs before the alarm rings.
- **School Calendar Aware**: Automatically suppresses alarms on teacher workdays, holidays, and snow delays.

👉 **Read the full backstory, kitchen-table build photos, and full parts list at [reclaimthe.day/spotify-clock](https://reclaimthe.day/spotify-clock)**.

---

## 🛠️ Hardware Bill of Materials (BOM)

Built using standard off-the-shelf hobbyist components from Adafruit and a pre-made desktop enclosure:

| Component | Part Name | Details | Approx. Price |
| :--- | :--- | :--- | :---: |
| **Microcontroller** | [Adafruit QT Py ESP32-S3](https://www.adafruit.com/product/5426) | Dual-Core 240MHz Wi-Fi/BLE (PID 5426) | $12.50 |
| **Audio Amp** | [Adafruit I2S Amplifier BFF](https://www.adafruit.com/product/5770) | MAX98357A I2S Class-D Amp (PID 5770) | $4.95 |
| **Speaker** | [40mm 4-Ohm 3W Speaker](https://www.adafruit.com/product/3968) | Compact internal speaker (PID 3968) | $4.95 |
| **Display** | [16x2 Character LCD (Blue)](https://www.adafruit.com/product/181) | Retro white-on-blue display (PID 181) | $9.95 |
| **LCD Adapter** | [Adafruit I2C LCD Backpack](https://www.adafruit.com/product/292) | 4-wire STEMMA QT adapter (PID 292) | $9.95 |
| **Dial Control** | [Adafruit Rotary Encoder Breakout](https://www.adafruit.com/product/5880) | I2C Seesaw encoder board (PID 5880) | $7.95 |
| **Dial Knob** | [Soft-Grip Rubber Knob](https://www.adafruit.com/product/5093) | Tactile rubber knob cap (PID 5093) | $0.75 |
| **Cables** | [STEMMA QT Cables 100mm](https://www.adafruit.com/product/4210) | Plug-and-play jumper cables (PID 4210) | $1.90 |
| **Enclosure** | [Wezhape 16x2 Desktop Case](https://wezhape.com/products/arduino-uno-case-with-16x2-lcd) | Molded desktop case (or 3D print) | $14.00 |

*Total Hardware Cost: ~$67.85 ($53.85 if 3D printing your case)*

---

## ⚡ Hardware Pinout & Architecture

```text
               +----------------------------------+
               |    Adafruit QT Py ESP32-S3       |
               +----------------------------------+
                                 |
           +---------------------+---------------------+
           | STEMMA QT I2C Bus   | I2S Audio Bus       |
           | (SCL: 40 / SDA: 41) | (DIN: 18 / LRC: 17  |
           |                     |  BCLK: 9)           |
           +----------+----------+----------+----------+
                      |                     |
              +-------v-------+     +-------v-------+
              | 1602 LCD      |     | I2S Amp BFF   |
              | Backpack 0x27 |     | (MAX98357A)   |
              +-------+-------+     +-------+-------+
                      |                     |
              +-------v-------+     +-------v-------+
              | Rotary Dial   |     | 40mm Bedside  |
              | Seesaw 0x36   |     | Speaker       |
              +---------------+     +---------------+
```

---

## 🚀 Getting Started & Setup

### 1. Clone & Setup Configuration
```bash
git clone https://github.com/pneedleman/reclaim-spotify-clock.git
cd reclaim-spotify-clock
cp src/secrets.h.example src/secrets.h
```

### 2. Enter Your Credentials in `src/secrets.h`
Edit `src/secrets.h` with your local network and API tokens:
```cpp
#define SECRET_WIFI_SSID "Your_Home_WiFi"
#define SECRET_WIFI_PASSWORD "Your_WiFi_Password"

#define SECRET_SPOTIFY_CLIENT_ID "your_spotify_client_id"
#define SECRET_SPOTIFY_CLIENT_SECRET "your_spotify_client_secret"
#define SECRET_SPOTIFY_REFRESH_TOKEN "your_spotify_refresh_token"
```

### 3. Build & Flash via PlatformIO
Open the project folder in VSCode with PlatformIO installed, then build and flash:
```bash
# Upload Firmware
pio run -t upload

# Upload SPIFFS Audio Assets
pio run -t uploadfs
```

---

## 🔌 Optional Integrations

The clock works out-of-the-box with just Wi-Fi & Spotify credentials. You can optionally enable these integrations in `src/secrets.h`:

- **Philips Hue Sunrise Lighting**: Add your Hue Bridge IP and username to `SECRET_HUE_BRIDGE_USERNAME` to automatically fade up bedroom lights 15 minutes before wake time.
- **Sinric Pro (Alexa Control)**: Add your Sinric Pro App Key & Switch ID to `SECRET_SINRICPRO_APP_KEY` if you want to toggle the clock or trigger alarms via Alexa routines.

---

## 🌐 Zero-App Web Dashboard

Access the local web dashboard from any browser on your home network at `http://spotify-alarm.local` (or via the clock's IP address):

- **Live Alarm Configuration**: Adjust wake times, gradual volume ramp rates, and school calendar sync.
- **Sunrise Simulation**: Configure Philips Hue bridge connection and light brightness curves.
- **API Config Tab**: Update credentials wirelessly over Wi-Fi without re-flashing.

---

## 📄 License & Acknowledgments

Distributed under the **MIT License**. See `LICENSE` for more information.

Created by Paul Needleman & daughter at [Reclaim The Day](https://reclaimthe.day). Special thanks to the open-source community behind `ESP32-audioI2S`, `LiquidCrystal_I2C`, `Adafruit Seesaw`, and `ArduinoJson`.




# CoffeeGrinder

**Smart coffee grinder controller** with MQTT, OTA updates, and full Home Assistant integration.  
Includes 3D-printable housing, custom PCB (KiCad), and open-source firmware for ESP32.

---

## ✨ Features

- ☕️ Weight-based preset grinding
- 📡 MQTT auto-discovery (Home Assistant integration)
- 🌐 Web interface for configuration
- 🔧 Touch buttons for presets and calibration
- 📊 Live sensor telemetry and control
- 📦 STL files for 3D-printed case
- 🔌 KiCad schematics and PCB

---

## 📁 Project Structure

```text
CoffeeGrinder/
├── src/                # Firmware source code
├── include/            # Header files
├── lib/                # Custom libraries (optional)
├── data/               # Web interface (SPIFFS)
├── hardware/
│   ├── stl/            # 3D-printable case parts
│   ├── kicad/          # KiCad PCB project
│   └── docs/           # Rendered schematics (PDF/PNG)
├── docs/               # General project documentation
├── LICENSE.md          # License (AGPL + CC BY-NC-SA)
└── README.md           # This file
```

---

## 🚀 Getting Started

## 🧾 Bill of Materials (BOM)

> **Note**: Some of the links below are affiliate links. If you make a purchase through them, I may receive a small commission at no extra cost to you. Thank you for your support!

- Manual Coffee Grinder [🛒 Amazon](https://amzn.to/4o6IfON)
- ESP32 Dev Board [🛒 Amazon](https://amzn.to/40CVtJ8)
- OLED Display [🛒 Amazon](https://amzn.to/44N8UZx)
- Step Down Converter [🛒 Amazon](https://amzn.to/4o4rhAz)
- HX711 Weight Sensor [🛒 Amazon](https://amzn.to/4lEH1bS)
- Scale [🛒 Amazon](https://amzn.to/3Uswbtt)
- 3x TTP223 Touch Sensors [🛒 Amazon](https://amzn.to/4133kje)
- NEMA 17 Stepper Motor [🛒 Amazon](https://amzn.to/4lTh3BO)
- Stepper Driver (e.g., TMC2209) [🛒 Amazon](https://amzn.to/45koFHp)
- 2x Bearing 30x37x4 [🛒 Amazon](https://amzn.to/46XXUtu)
- 2x Bearing 8x12x3.5 [🛒 Amazon](https://amzn.to/3UtORsR)
- 2x Bearing 7x13x4 [🛒 Amazon](https://amzn.to/470tUxc)
- 6x Spacer [🛒 Amazon](https://amzn.to/4lLxm3v)
- 12x Magnets 7x2 [🛒 Amazon](https://amzn.to/4f6ZivV)
- Power Supply [🛒 Amazon](https://amzn.to/4o3DJ3y)
- Elko [🛒 Amazon](https://amzn.to/3Uhur6B)
- DC Connector [🛒 Amazon](https://amzn.to/4f7ja2e)
- some cables
- some M3 screws
- some M3 nuts

### Requirements

- MQTT Broker (e.g., Mosquitto)
- Home Assistant (optional, for integration)

### Flashing

Use [PlatformIO](https://platformio.org/) in VSCode:

---

## 🔧 Web Interface

Accessible after WiFi configuration.  
Use it to set:

- MQTT server credentials
- Calibration
- Preset values
- Start grinding manually

---

## 🧠 Home Assistant Integration

Entities auto-discovered via MQTT:

- Sensors: weight, selected preset, scale factor, total weight
- Numbers: block threshold, preset weights
- Buttons: start, calibrate, tare, run preset

Make sure `discovery` is enabled in your MQTT integration.

---

## 🔐 License

- Firmware: [AGPL-3.0](https://www.gnu.org/licenses/agpl-3.0.html)
- Hardware + STL: [CC BY-NC-SA 4.0](https://creativecommons.org/licenses/by-nc-sa/4.0/)

**© 2025 Danny Smolinsky**

For commercial licensing inquiries, contact the author.
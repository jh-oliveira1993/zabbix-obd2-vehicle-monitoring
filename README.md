# zabbix-obd2-vehicle-monitoring

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![Python Version](https://img.shields.io/badge/python-3.8%2B-blue.svg)](https://www.python.org/)
[![C++ Version](https://img.shields.io/badge/C%2B%2B-ESP32-green.svg)](https://www.arduino.cc/)
[![Zabbix Version](https://img.shields.io/badge/zabbix-7.0%2B-red.svg)](https://www.zabbix.com/)

A lightweight suite of tools (Python daemon and ESP32 C++ firmware) and a Zabbix template designed to monitor real-time vehicle telemetry via OBD-II using an ELM327 Bluetooth adapter. It streams numeric and textual telemetry at 1Hz, and utilizes Zabbix calculated items to compute advanced metrics such as fuel consumption (km/L) and engine power.

## Features

- **Dual-Client Support:** Choose between running a Python daemon on an Android phone via Termux, or using a dedicated ESP32 microcontroller for an embedded plug-and-play experience.
- **Dynamic LLD Discovery (Python & ESP32):** Both clients automatically query the ECU's supported PIDs bitmasks and register them in Zabbix using Low-Level Discovery (LLD). The ESP32 natively parses 22+ standard SAE formulas in C++.
- **Zabbix Sender Protocol:** Both clients employ a lightweight implementation of the Zabbix Sender TCP protocol to bypass the need for Zabbix Agents.
- **Advanced Virtual Metrics:** Uses Zabbix's `CALCULATED` item type to compute:
  - Real-time fuel consumption in km/L (via Speed-Density calculation using MAP, RPM, Intake Temp, and Lambda).
  - Estimated Engine Power (kW/HP) & Torque (N·m).
  - Total Fuel Trim (Short Term + Long Term).
- **ESP32 Dual-Core Architecture:** The C++ firmware utilizes FreeRTOS to run connection handling and data parsing on Core 1, while providing highly responsive visual LED status feedback on Core 0.
- **Auto-Recovery:** Built-in smart reconnection logic handles Bluetooth disconnects or Wi-Fi drops transparently in both clients.

## Architecture

This project supports two different architectures depending on your hardware availability:

### 1. Gateway Approach (Termux / Android)
Designed to run on an unrooted Android device acting as a gateway inside the car.
```text
[Vehicle ECU] -> (OBD-II / CAN) -> [ELM327 BT Adapter]
                                          | (Bluetooth SPP)
                            [Android Gateway (TCP Bridge App)]
                                          | (TCP Socket 127.0.0.1:35000)
                     [Python Collector Script (clients/python-termux)]
                                          | (Mobile Data / Zabbix Trapper TCP)
                                   [Zabbix Server]
```
**Note:** To bypass Android security restrictions without root, an Android app (like *Bluetooth to TCP Bridge*) is used to redirect the Bluetooth SPP stream to a local TCP socket that the Python script can read.

### 2. Embedded Approach (ESP32)
A dedicated, headless hardware solution. The ESP32 connects directly to the ELM327 as a Bluetooth Master, processes the hexadecimal PIDs, and transmits the JSON payload to Zabbix using a smartphone's Wi-Fi Hotspot.
```text
[Vehicle ECU] -> (OBD-II / CAN) -> [ELM327 BT Adapter]
                                          | (Bluetooth Classic)
                           [ESP32 Microcontroller (clients/esp32)]
                                          | (Wi-Fi 2.4GHz via Smartphone Hotspot)
                                   [Zabbix Server]
```

## Repository Structure

```text
zabbix-obd2-vehicle-monitoring/
├── README.md
├── zabbix/
│   └── generic_telemetry_by_obd2.json # Universal Zabbix 7.0 template
└── clients/
    ├── python-termux/        # Python telemetry collector daemon for smartphones
    │   ├── telemetry.py
    │   └── requirements.txt
    └── esp32/                # ESP32 C++ OBD2 collector
        └── zabbix_obd2
            └── zabbix_obd2.ino
```

## Requirements

### Zabbix Server
- **Zabbix Server or Proxy 7.0+** (supporting the exported JSON template format).

### Client Option A: Python on Android
- **Python 3.8+** (via Termux)
- **python-obd** library
- [Bluetooth to TCP Bridge](https://play.google.com/store/apps/details?id=masar.bb) App

### Client Option B: ESP32
- ESP32 Development Board (e.g., WROOM-32, NodeMCU-32S)
- Arduino IDE (with ESP32 core `esp32:esp32` installed)
- Smartphone with Wi-Fi Hotspot (2.4 GHz band enabled)

## Setup & Installation

### 1. Zabbix Template Configuration
1. Go to Zabbix Web UI -> **Configuration** -> **Templates** -> **Import**.
2. Upload `zabbix/generic_telemetry_by_obd2.json`.
3. Create a Host for your vehicle (e.g., `My Vehicle`) and link the template to it.
4. Go to the Host's **Macros** tab to configure your engine parameters:
   - `{$OBD.ENGINE.DISPLACEMENT}`: Engine size in liters (default: `1.0`).
   - `{$OBD.ENGINE.VE}`: Volumetric Efficiency (default: `0.80`).

### 2. Python Collector Installation (Option A)
Clone this repository in Termux and install dependencies:
```bash
git clone https://github.com/jh-oliveira1993/zabbix-obd2-vehicle-monitoring.git
cd zabbix-obd2-vehicle-monitoring/clients/python-termux
pip install -r requirements.txt
```
Run the collector by setting the environment variables:
```bash
export ZABBIX_HOST="<YOUR_ZABBIX_HOST>"
export ZABBIX_SERVER="<YOUR_ZABBIX_SERVER_IP>"
export OBD_URI="socket://127.0.0.1:35000"

python3 telemetry.py
```

### 3. ESP32 Collector Installation (Option B)
1. Open `clients/esp32/zabbix_obd2/zabbix_obd2.ino` in the Arduino IDE.
2. Edit the **GENERAL SETTINGS** block at the top of the file:
   - Update `WIFI_SSID` and `WIFI_PASS` (must be a 2.4GHz network).
   - Update `ZABBIX_SERVER` and `ZABBIX_HOST` (ensure the host matches your Zabbix Hostname exactly).
   - Update `ELM327_MAC_BYTES` with your adapter's MAC address in hex format (e.g., `{0x01, 0x23, 0x45, 0x67, 0x89, 0xBA}`).
3. Install the **ArduinoJson** library via the Library Manager.
4. In the Arduino IDE, go to **Tools -> Partition Scheme** and select **Huge APP (3MB No OTA/1MB SPIFFS)**. This is mandatory as both the Wi-Fi and Bluetooth Classic stacks are very large.
5. Compile and Upload.

#### ESP32 Visual Feedback (Built-in LED)
The firmware uses FreeRTOS to provide non-blocking visual feedback via the built-in LED (GPIO 2):
- **Fast Continuous Blink (250ms):** Searching for Wi-Fi.
- **Strobe (3 ultra-fast blinks every 2s):** Wi-Fi connected, but searching for the ELM327 Bluetooth.
- **Solid ON:** Fully connected to both Wi-Fi and the vehicle.
- **Brief OFF Blink:** Data successfully transmitted to Zabbix.

## License
This project is licensed under the MIT License - see the LICENSE file for details.

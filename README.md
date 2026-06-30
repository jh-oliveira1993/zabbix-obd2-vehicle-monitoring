# zabbix-obd2-vehicle-monitoring

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![Python Version](https://img.shields.io/badge/python-3.8%2B-blue.svg)](https://www.python.org/)
[![Zabbix Version](https://img.shields.io/badge/zabbix-7.0%2B-red.svg)](https://www.zabbix.com/)

A lightweight Python daemon and Zabbix template designed to monitor real-time vehicle telemetry via OBD-II (using an ELM327 adapter and a Bluetooth/Wi-Fi TCP bridge). It dynamically discovers supported ECU PIDs, streams numeric and textual telemetry at 1Hz, and utilizes Zabbix calculated items to compute advanced metrics such as fuel consumption (km/L) and engine power.

## Features

- **Dynamic LLD Discovery:** Automatically queries the ECU's supported PIDs and registers them in Zabbix using Low-Level Discovery (LLD).
- **Auto-injected Descriptions:** Dynamically registers explanatory descriptions and units for each OBD2 metric directly into Zabbix items.
- **Data Usage Optimization:** Descriptions are sent only on the first connection session. Subsequent periodic updates are stripped of description payloads to minimize mobile data consumption.
- **Pure Python Zabbix Sender:** Employs a lightweight, pure-Python implementation of the Zabbix Sender protocol, eliminating binary dependencies on restricted environments (e.g., Android/Bionic).
- **Advanced Virtual Metrics:** Uses Zabbix's `CALCULATED` item type to compute:
  - Real-time fuel consumption in km/L (via Speed-Density calculation using MAP, RPM, Intake Temperature, and Lambda/Equivalence Ratio).
  - Estimated Engine Power (kW/HP) & Torque (N·m).
  - Manifold Vacuum (mmHg).
  - Total Fuel Trim (Short Term + Long Term).
  - Exhaust Gas Thermal Rise.
- **Flexible Configuration:** Fully configurable via environment variables (`ZABBIX_HOST`, `ZABBIX_SERVER`, `OBD_URI`, etc.) to monitor multiple vehicles using a single daemon.

## Architecture

Because this project is designed to run on resource-constrained edge gateways (like an unrooted Android device running Termux inside a car), the architecture is split into a localized collector and a central monitor:

```
[Vehicle ECU]
      │ (OBD-II / CAN Bus)
[ELM327 OBD2 Adapter (Bluetooth/WiFi)]
      │ (Bluetooth SPP Profile)
[Android Gateway (Termux / Bluetooth to TCP Bridge App)]
      │ (Python-OBD queries socket://127.0.0.1:35000)
[Python Collector Script (clients/python-termux/telemetry.py)]
      │ (TCP Zabbix Sender Protocol via Mobile Data)
[Zabbix Server / Proxy]
```

### No-Root Android Solution
By default, Android isolates hardware access. A standard, non-rooted **Termux** environment cannot access the device's Bluetooth stack directly. To bypass this security restriction without rooting the phone (preserving device integrity and security), we use an Android application to act as a serial-to-TCP bridge.

The Android app reads the SPP (Serial Port Profile) stream from the paired Bluetooth ELM327 adapter and redirects it to a local port (e.g., `127.0.0.1:35000`). The Python daemon running inside Termux then easily communicates with the adapter via standard network sockets.

## Repository Structure

```
zabbix-obd2-vehicle-monitoring/
├── README.md
├── .gitignore
├── docs/
│   └── architecture.md       # High-level architecture and hardware details
├── zabbix/
│   └── generic_telemetry_by_obd2.json # Universal Zabbix 7.0 template
└── clients/
    ├── python-termux/        # Python telemetry collector daemon for smartphones
    │   ├── telemetry.py
    │   └── requirements.txt
    └── esp32/                # ESP32 C++ OBD2 collector (WIP)
```

## Requirements

### Gateway (e.g., Android Device with Termux)
- **Python 3.8+**
- **python-obd** library (handles connection and command encoding)
- An ELM327 Bluetooth/WiFi adapter
- **Bluetooth to TCP Bridge app:** The recommended app to bridge the Bluetooth adapter to a local port without root is [Bluetooth to TCP Bridge](https://play.google.com/store/apps/details?id=masar.bb) (exposing the serial port on a local port like `127.0.0.1:35000`).

### Monitoring
- **Zabbix Server or Proxy 7.0+** (supporting the exported JSON template format).

## Setup & Installation

### 1. Zabbix Template Configuration
1. Go to Zabbix Web UI -> **Configuration** -> **Templates** -> **Import**.
2. Upload `zabbix/generic_telemetry_by_obd2.json`.
3. Create a Host for your vehicle (e.g. `My Vehicle`) and link the `Generic Telemetry by OBD2` template to it.
4. Go to the Host's **Macros** tab to configure your engine parameters:
   - `{$OBD.ENGINE.DISPLACEMENT}`: Engine size in liters (default: `1.0`). Change to `1.6` or others if needed.
   - `{$OBD.ENGINE.VE}`: Volumetric Efficiency (default: `0.80`). For turbocharged engines, adjust to `1.15` - `1.20`.

### 2. Python Collector Installation
On your Android/Termux or Linux terminal, clone this repository and install dependencies:

```bash
git clone https://github.com/jh-oliveira1993/zabbix-obd2-vehicle-monitoring.git
cd zabbix-obd2-vehicle-monitoring/clients/python-termux
pip install -r requirements.txt
```

### 3. Run the Collector
Set the configuration environment variables and run the script:

```bash
export ZABBIX_HOST="My Vehicle"
export ZABBIX_SERVER="your-zabbix-server-ip-or-dns"
export OBD_URI="socket://127.0.0.1:35000"

python3 telemetry.py
```

### Environment Variables
| Variable | Description | Default |
|----------|-------------|---------|
| `ZABBIX_HOST` | Hostname of the vehicle as configured in Zabbix | `My Vehicle` |
| `ZABBIX_SERVER` | Zabbix Server or Proxy IP/DNS address | `your-zabbix-server-ip-or-dns` |
| `ZABBIX_PORT` | Zabbix Trapper Port | `10051` |
| `OBD_URI` | Connection URI for python-obd (`socket://IP:PORT` or `/dev/ttyUSBX`) | `socket://127.0.0.1:35000` |

## License

This project is licensed under the MIT License - see the LICENSE file for details.

# Project Context: Edge Automotive Telemetry Gateway (OBD2 to Zabbix)

## 1. Overview and Objective
This project implements a real-time automotive telemetry ingestion pipeline. Data extraction occurs via the CAN bus (OBD2) using a Bluetooth/WiFi ELM327 adapter, which is processed at the edge by an Android device running Termux, and sent asynchronously to a Zabbix server.

## 2. Infrastructure Architecture (Constraints and Solutions)
The execution environment (Termux on Android) **does not have Root access**. Therefore, the architecture designed to bypass the hardware block of the Android kernel was:
1. **Physical Layer:** ELM327 natively paired with the Android Bluetooth stack.
2. **Serial-to-TCP Proxy:** A non-root Android app ([Bluetooth to TCP Bridge](https://play.google.com/store/apps/details?id=masar.bb)) receives SPP (Serial Port Profile) data and exposes it over a local TCP socket at `127.0.0.1:35000`.
3. **Application Layer:** A Python daemon running inside Termux utilizing the `python-obd` library.
4. **Transport Layer (Zabbix):** Metrics are sent using a pure-Python implementation of the Zabbix Sender protocol over TCP to port `10051`. This bypasses the need for the native `zabbix_sender` binary on Android, ensuring compatibility with the Bionic libc environment.

## 3. Validated Physical and Logical Tests
End-to-end communication was validated using the following tests during development:
- **Telnet/Socket Test:** A raw socket connection via `telnet 127.0.0.1 35000` executing AT commands. Running command `010C` successfully returned the CAN frame `7E804410C0D74`, confirming the extraction of engine RPM from the ECU.
- **Python-OBD Test:** Auto-handshake of the library succeeded when target URI was set to `socket://127.0.0.1:35000`.
- **Mapped Protocol:** The test vehicle natively negotiates via the `ISO 15765-4 (CAN 11/500)` protocol and supports standard OBD2 commands (PIDs).

## 4. Edge Telemetry Collector Daemon
The final telemetry daemon (`clients/python-termux/telemetry.py`) uses a pure-Python Zabbix Sender protocol implementation to send metrics to Zabbix. This approach avoids spawning external binaries, reducing overhead and improving reliability on Android platforms.

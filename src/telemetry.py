import obd
import time
import socket
import struct
import json
import logging
import logging.handlers
import sys
import os
import ssl
try:
    import sslpsk3
except ImportError:
    sslpsk3 = None

# ==========================================
# LOGGING CONFIGURATION
# File rotation: max 5 MB, 3 backups.
# Ensures the log doesn't consume the limited
# storage of the Android device (Termux).
# ==========================================
LOG_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "obd2_telemetry.log")

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(message)s",
    datefmt="%Y-%m-%dT%H:%M:%S",
    handlers=[
        logging.handlers.RotatingFileHandler(
            LOG_FILE, maxBytes=5 * 1024 * 1024, backupCount=3, encoding="utf-8"
        ),
        logging.StreamHandler(sys.stdout),
    ],
)

log = logging.getLogger(__name__)

# ==========================================
# ZABBIX CONFIGURATION
# ==========================================
ZABBIX_SERVER  = os.environ.get("ZABBIX_SERVER", "localhost")
ZABBIX_PORT    = int(os.environ.get("ZABBIX_PORT", 10051))
ZABBIX_HOST    = os.environ.get("ZABBIX_HOST", "Generic OBD2")
ZABBIX_TIMEOUT = 5                           # TCP connection timeout in seconds
ZABBIX_TLS_CONNECT = os.environ.get("ZABBIX_TLS_CONNECT", "unencrypted").lower()
ZABBIX_TLS_PSK_IDENTITY = os.environ.get("ZABBIX_TLS_PSK_IDENTITY", "")
ZABBIX_TLS_PSK = os.environ.get("ZABBIX_TLS_PSK", "")

# ==========================================
# RECONNECTION SETTINGS (BACKOFF)
# ==========================================
OBD_URI                  = os.environ.get("OBD_URI", "socket://127.0.0.1:35000")
BACKOFF_BASE_SECONDS    = 5    # Initial wait between reconnection attempts
BACKOFF_MAX_SECONDS  = 300  # Wait ceiling (5 minutes)
BACKOFF_FACTOR            = 2    # Exponential growth of the interval
POLLING_INTERVAL        = 1.0  # Polling interval in seconds (1 Hz)

# ==========================================
# LLD DISCOVERY
# Interval to resend the discovery payload
# to Zabbix. On the first connection
# it is sent immediately; then every
# DISCOVERY_INTERVAL seconds.
# ==========================================
DISCOVERY_INTERVAL = 3600  # 1 hour

# ==========================================
# IGNORED PIDS IN DYNAMIC DISCOVERY
# Commands returning non-numeric objects
# (DTCs, status, bitmasks) are excluded.
# ==========================================
IGNORED_COMMANDS = {
    "PIDS_A", "PIDS_B", "PIDS_C",           # Support bitmasks
    "PIDS_9A",                               # Support bitmask (mode 9)
    "STATUS", "STATUS_DRIVE_CYCLE",          # Complex Status objects
    "FREEZE_DTC", "GET_DTC",                # DTC lists
    "CLEAR_DTC", "GET_CURRENT_DTC",         # DTC lists
    "O2_SENSORS", "O2_SENSORS_ALT",         # O2 sensor bitmasks
    "AUX_INPUT_STATUS",                     # Boolean status
    # Monitor IDs — bitmasks internos sem value operacional
    "MIDS_A", "MIDS_B", "MIDS_C",
    "MIDS_D", "MIDS_E", "MIDS_F",
    # Note: MONITOR_* were removed from here — they return .complete (1/0)
    # and are useful for emission alerts. See extract_value().
}

# ==========================================
# PIDS RETURNING TEXTUAL VALUES
# Discovered via obd.discovery.text and
# sent as obd.text[{suffix}].
# ==========================================
TEXT_COMMANDS = {
    "VIN",             # Vehicle Identification Number
    "CALIBRATION_ID",  # ECU calibration ID
    "CVN",             # Calibration Verification Number
    "FUEL_STATUS",     # Fuel system status
    "OBD_COMPLIANCE",  # OBD compliance standard
    "ELM_VERSION",     # ELM327 adapter version
}

# ==========================================
# UNIT MAP PER PID
# Used in the LLD discovery payload
# ({#OBD_UNIT}) for display in Zabbix.
# ==========================================
OBD_UNITS = {
    "RPM":                          "rpm",
    "SPEED":                        "km/h",
    "COOLANT_TEMP":                 "°C",
    "INTAKE_TEMP":                  "°C",
    "AMBIENT_AIR_TEMP":             "°C",
    "OIL_TEMP":                     "°C",
    "CATALYST_TEMP_B1S1":           "°C",
    "CATALYST_TEMP_B1S2":           "°C",
    "CATALYST_TEMP_B2S1":           "°C",
    "CATALYST_TEMP_B2S2":           "°C",
    "ENGINE_LOAD":                  "%",
    "ABSOLUTE_LOAD":                "%",
    "THROTTLE_POS":                 "%",
    "THROTTLE_POS_B":               "%",
    "RELATIVE_THROTTLE_POS":        "%",
    "COMMANDED_EGR":                "%",
    "COMMANDED_EVAPORATIVE_PURGE":  "%",
    "EGR_ERROR":                    "%",
    "FUEL_LEVEL":                   "%",
    "SHORT_FUEL_TRIM_1":            "%",
    "LONG_FUEL_TRIM_1":             "%",
    "SHORT_FUEL_TRIM_2":            "%",
    "LONG_FUEL_TRIM_2":             "%",
    "ACCELERATOR_POS_D":            "%",
    "ACCELERATOR_POS_E":            "%",
    "ACCELERATOR_POS_F":            "%",
    "COMMANDED_THROTTLE_ACTUATOR":  "%",
    "CONTROL_MODULE_VOLTAGE":       "V",
    "FUEL_PRESSURE":                "kPa",
    "INTAKE_PRESSURE":              "kPa",
    "BAROMETRIC_PRESSURE":          "kPa",
    "FUEL_RAIL_PRESSURE_DIRECT":    "kPa",
    "FUEL_RAIL_PRESSURE_VAC":       "kPa",
    "EVAPORATIVE_PURGE":            "Pa",
    "TIMING_ADVANCE":               "°",
    "FUEL_INJECT_TIMING":           "°",
    "MAF":                          "g/s",
    "FUEL_RATE":                    "L/h",
    "RUN_TIME":                     "s",
    "DISTANCE_SINCE_DTC_CLEAR":     "km",
    "DISTANCE_W_MIL":               "km",
    "WARMUPS_SINCE_DTC_CLEAR":      "",
    "ELM_VOLTAGE":                  "V",
    "AMBIANT_AIR_TEMP":              "°C",
    "THROTTLE_ACTUATOR":             "%",
    "EVAPORATION_PURGE":             "%",
    "ETHANOL_PERCENT":               "%",
    "COMMANDED_EQUIV_RATIO":         "λ",
    "O2_B1S1":                       "V",
    "O2_B1S2":                       "V",
}

# ==========================================
# DESCRIPTIONS PER PID
# Included in the LLD payload as {#OBD_DESC}.
# Each item created by the LLD receives its
# specific description automatically.
# ==========================================
OBD_DESCRIPTIONS = {
    "RPM":                    "Engine rotational speed in revolutions per minute.\nNormal idle: 750-950 RPM. Red line: ~6500 RPM.",
    "SPEED":                  "Vehicle speed in km/h, reported by the ABS wheel speed sensors via ECU.\n0 = vehicle stationary.",
    "ENGINE_LOAD":            "Calculated engine load as a percentage of maximum capacity.\nIdle: 15-35%. Full throttle: 80-100%.",
    "ABSOLUTE_LOAD":          "Absolute throttle body airflow as a percentage of maximum airflow at standard conditions.\nNormalized to sea-level — useful for comparing load independent of altitude.",
    "COOLANT_TEMP":           "Engine coolant temperature in degrees C.\nNormal operating range: 85-105C. Above 110C indicates overheating risk.",
    "INTAKE_TEMP":            "Intake air temperature in degrees C, measured at the air filter/throttle body.\nAffects air density and engine power. Normal range: ambient to ~70C.",
    "AMBIANT_AIR_TEMP":       "Ambient (outside) air temperature in degrees C, as reported by the ECU.\nUsed for cold-start enrichment and climate control logic.",
    "INTAKE_PRESSURE":        "Manifold Absolute Pressure (MAP) in kPa — air pressure inside the intake manifold.\nIdle (throttle closed): 20-40 kPa. Full throttle: ~95-100 kPa.\nPrimary load signal on this engine (Speed-Density, no MAF).",
    "BAROMETRIC_PRESSURE":    "Atmospheric pressure in kPa, measured by the ECU.\nSea level: ~101 kPa. High altitude reduces this value.\nUsed to compensate fuel delivery for altitude.",
    "TIMING_ADVANCE":         "Spark ignition timing in degrees relative to Top Dead Center (TDC).\nPositive = advance (before TDC). Negative = retard (after TDC).\nECU retards when knock is detected. Normal idle: 5-15 deg BTDC.",
    "THROTTLE_POS":           "Absolute throttle position sensor (TPS) value, Bank 1 (0-100%).\nIdle baseline is typically 14-16% due to electronic throttle design.",
    "THROTTLE_POS_B":         "Absolute throttle position sensor (TPS) value, Bank 2 — redundant safety sensor.\nShould closely track THROTTLE_POS. Large divergence indicates sensor fault.",
    "THROTTLE_ACTUATOR":      "Commanded electronic throttle actuator position in %.\nWhat the ECU requests from the throttle motor. Compare with THROTTLE_POS for response issues.",
    "RELATIVE_THROTTLE_POS":  "Relative throttle position, zeroed at closed throttle.\n0% = foot off pedal. 100% = wide open throttle.\nMore intuitive than absolute TPS for reading driver input.",
    "ACCELERATOR_POS_D":      "Accelerator pedal position sensor D (%).\nOne of two redundant drive-by-wire pedal sensors. Should match ACCELERATOR_POS_E within ~1-2%.",
    "ACCELERATOR_POS_E":      "Accelerator pedal position sensor E (%).\nSecond redundant drive-by-wire pedal sensor. ECU cross-validates with D; divergence triggers throttle fault.",
    "RUN_TIME":               "Time in seconds the engine has been running since last key-on event.\nResets to 0 on every engine start.",
    "FUEL_LEVEL":             "Fuel tank fill level as a percentage of total capacity.\nA typical 35-liter tank size. 1% ~= 0.35 L.",
    "ETHANOL_PERCENT":        "Ethanol content in the current fuel blend (Flex-Fuel sensor).\n0% = pure gasoline (E0). 100% = pure ethanol (E100).\nBrazilian common gasoline (E22-E27) reads 20-27%.\nECU uses this to adjust fuel injection and ignition timing.",
    "EVAPORATIVE_PURGE":      "Commanded opening of the EVAP purge valve (%).\nAllows fuel vapors from the charcoal canister to be burned in the engine.\n0% = closed. 100% = fully open. Active when engine is warm at cruise.",
    "LONG_FUEL_TRIM_1":       "Long-Term Fuel Trim (LTFT) Bank 1, in %.\nLearned persistent ECU correction to fuel injection to maintain stoichiometry.\nPositive = adding fuel (lean). Negative = reducing fuel (rich).\n+-5% is normal. Beyond +-10% suggests vacuum leak, injector fault, or failing O2 sensor.",
    "SHORT_FUEL_TRIM_1":      "Short-Term Fuel Trim (STFT) Bank 1, in %.\nReal-time correction based on O2 sensor feedback. Oscillates around 0% in healthy operation.\nSustained values > +-10% become absorbed into LTFT.",
    "COMMANDED_EQUIV_RATIO":  "Commanded equivalence ratio (lambda) — ECU target air/fuel ratio.\n1.000 = stoichiometric. < 1.0 = rich (acceleration). > 1.0 = lean (deceleration/cruise).",
    "O2_B1S1":                "Pre-catalyst upstream oxygen sensor voltage, Bank 1 Sensor 1.\nIn closed-loop, oscillates rapidly between 0.1V (lean) and 0.9V (rich) at ~1Hz.\nSlow switching or stuck voltage = failing O2 sensor or mixture problem.",
    "O2_B1S2":                "Post-catalyst downstream oxygen sensor voltage, Bank 1 Sensor 2.\nMonitors catalytic converter efficiency.\nHealthy cat: stable ~0.5-0.7V. Oscillating like upstream = degraded catalyst.",
    "CONTROL_MODULE_VOLTAGE": "Battery/charging system voltage as measured by the ECU (V).\nEngine off: ~12.4-12.7V. Engine running: ~13.8-14.5V (alternator charging).\nBelow 13.5V while running may indicate alternator or wiring issues.",
    "ELM_VOLTAGE":            "Battery voltage measured directly by the ELM327 OBD adapter (V).\nShould closely match CONTROL_MODULE_VOLTAGE.\nSignificant divergence may indicate wiring resistance or sensor drift.",
    "CATALYST_TEMP_B1S1":     "Catalytic converter temperature, Bank 1 Sensor 1, in degrees C.\nNormal operating range: 400-900C. Cold start light-off: ~300-400C.\nAbove 950C = risk of catalyst damage (misfire or excess fuel in exhaust).",
    "DISTANCE_SINCE_DTC_CLEAR": "Total distance driven in km since Diagnostic Trouble Codes were last cleared.\nTracks how long the vehicle has been running clean after a repair.",
    "DISTANCE_W_MIL":         "Distance driven in km while the MIL (Check Engine Light) was on.\n0 km = no active faults. Any value > 0 = fault was active while driving.",
    "WARMUPS_SINCE_DTC_CLEAR": "Warmup cycles completed since DTCs were last cleared.\nA warmup = engine starts cold and reaches operating temperature.\n255 = counter saturated (maximum value).",
    "MAX_MAF":                "Maximum mass airflow sensor capacity in g/s — a fixed engineering constant.\nNot a real-time reading. Value of 0 = no MAF sensor (this engine uses MAP/Speed-Density).",
    "MONITOR_CATALYST_B1":       "Catalytic converter efficiency readiness monitor, Bank 1.\n1 = all sub-tests passed. 0 = sub-test failed or monitor not yet complete.",
    "MONITOR_MISFIRE_CYLINDER_1": "Misfire detection monitor, Cylinder 1.\n1 = passed (no misfire detected). 0 = misfire detected in Cylinder 1.",
    "MONITOR_MISFIRE_CYLINDER_2": "Misfire detection monitor, Cylinder 2.\n1 = passed (no misfire detected). 0 = misfire detected in Cylinder 2.",
    "MONITOR_MISFIRE_CYLINDER_3": "Misfire detection monitor, Cylinder 3.\n1 = passed (no misfire detected). 0 = misfire detected in Cylinder 3.",
    "MONITOR_O2_B1S1":           "Upstream O2 sensor (B1S1) circuit and response readiness monitor.\n1 = sensor passed all tests (heater, response, range). 0 = test failed.",
    "MONITOR_O2_HEATER_B1S1":    "Heater circuit test for upstream O2 sensor, Bank 1 Sensor 1.\n1 = heater resistance and current within spec. 0 = heater circuit fault.",
    "MONITOR_O2_HEATER_B1S2":    "Heater circuit test for downstream O2 sensor, Bank 1 Sensor 2.\n1 = heater resistance and current within spec. 0 = heater circuit fault.",
    "MONITOR_VVT_B1":            "Variable Valve Timing (VVT) system readiness monitor, Bank 1.\n1 = VVT actuator operating within expected parameters. 0 = VVT fault detected.",
}


# ------------------------------------------
# ZABBIX SENDER PROTOCOL (PURE PYTHON IMPLEMENTATION)
# Eliminates dependency on the zabbix_sender binary,
# incompatible with the Android/Bionic kernel.
#
# Protocol:
#   1. Connects TCP to Zabbix Server/Proxy
#   2. Sends ZBXD\x01 header + size
#      (uint64 little-endian) + payload JSON
#   3. Lê a response (registrada em debug)
# ------------------------------------------
_ZBXD_HEADER  = b"ZBXD\x01"   # Magic + Zabbix protocol version
_ZBXD_LEN_FMT = "<Q"           # uint64 little-endian


def dispatch_zabbix_batch(items: list) -> None:
    """
    Envia múltiplos items ao Zabbix em uma única conexão TCP.
    Each item is a dict with 'key' and 'value' keys.
    Any failure is logged and discarded — the main loop
    is NEVER interrupted by sending failures.
    """
    payload = json.dumps(
        {
            "request": "sender data",
            "data": [
                {
                    "host":  ZABBIX_HOST,
                    "key":   item["key"],
                    "value": str(item["value"]),
                }
                for item in items
            ],
        },
        ensure_ascii=False,
    ).encode("utf-8")

    packet = (
        _ZBXD_HEADER
        + struct.pack(_ZBXD_LEN_FMT, len(payload))
        + payload
    )

    try:
        with socket.create_connection(
            (ZABBIX_SERVER, ZABBIX_PORT), timeout=ZABBIX_TIMEOUT
        ) as sock:
            if ZABBIX_TLS_CONNECT == "psk":
                if not sslpsk3:
                    log.error("ZABBIX_TLS_CONNECT is set to 'psk' but 'sslpsk3' module is not installed.")
                    return
                if not ZABBIX_TLS_PSK_IDENTITY or not ZABBIX_TLS_PSK:
                    log.error("ZABBIX_TLS_PSK_IDENTITY and ZABBIX_TLS_PSK must be provided when using PSK encryption.")
                    return
                try:
                    psk_bytes = bytes.fromhex(ZABBIX_TLS_PSK)
                except ValueError:
                    log.error("ZABBIX_TLS_PSK must be a valid hex string.")
                    return

                # Zabbix requires exactly this format for PSK tuple in sslpsk
                # Wrap socket *after* connecting to the remote host (since it's already connected via create_connection)
                with sslpsk3.wrap_socket(
                    sock,
                    psk=(psk_bytes, ZABBIX_TLS_PSK_IDENTITY.encode("utf-8")),
                    ssl_version=ssl.PROTOCOL_TLSv1_2,
                    ciphers='PSK-AES128-CBC-SHA',
                    server_side=False,
                    # Zabbix PSK typically uses these ciphers; sslpsk3 should negotiate automatically
                    # but we allow any supported by default.
                    cert_reqs=ssl.CERT_NONE # Not using certificates
                ) as tls_sock:
                    tls_sock.sendall(packet)
                    response = tls_sock.recv(4096)
                    log.debug("Batch sent (%d items) | Response: %s", len(items), response)
            else:
                sock.sendall(packet)
                response = sock.recv(4096)
                log.debug("Batch sent (%d items) | Response: %s", len(items), response)
    except OSError as exc:
        log.error("Erro de rede ao enviar batch ao Zabbix: %s", exc)
    except Exception as exc:  # pylint: disable=broad-except
        log.error("Erro inesperado ao enviar batch ao Zabbix: %s", exc)


def dispatch_zabbix(key: str, value) -> None:
    """Sends a single item to Zabbix (convenience wrapper over dispatch_zabbix_batch)."""
    dispatch_zabbix_batch([{"key": key, "value": value}])


# ------------------------------------------
# FUNCTION: discover_metrics
# Queries the ECU about all PIDs it
# supports and builds the polling list.
# Returns a list of tuples:
#   (cmd, suffix, name, unit)
# where suffix is used in the key: obd.{suffix}
# ------------------------------------------
def discover_metrics(conn) -> list:
    metrics = []
    supported_cmds = sorted(conn.supported_commands, key=lambda c: c.name)
    for cmd in supported_cmds:
        if cmd.name in IGNORED_COMMANDS or cmd.name in TEXT_COMMANDS:
            log.debug("PID ignored in numeric discovery: %s", cmd.name)
            continue
        suffix  = cmd.name.lower()
        name    = getattr(cmd, "desc", cmd.name)
        unit = OBD_UNITS.get(cmd.name, "")
        description = OBD_DESCRIPTIONS.get(cmd.name, "OBD2 PID: " + cmd.name)
        metrics.append((cmd, suffix, name, unit, description))

    names = [s for _, s, _, _, _ in metrics]
    log.info("PIDs supported by ECU (%d): %s", len(metrics), ", ".join(names))
    return metrics


# ------------------------------------------
# FUNCTION: send_discovery
# Sends the LLD payload to Zabbix so that the
# items are automatically created from
# the Zabbix templates.
#
# Payload enviado à key 'obd.discovery':
#   {"data": [
#     {"{#OBD_KEY}": "rpm",
#      "{#OBD_NAME}": "Engine RPM",
#      "{#OBD_UNIT}": "rpm"},
#     ...
#   ]}
# ------------------------------------------
def send_discovery(metrics: list, with_description: bool = True) -> None:
    lld_data = []
    for _, suffix, name, unit, description in metrics:
        item = {
            "{#OBD_KEY}":   suffix,
            "{#OBD_NAME}":  name,
            "{#OBD_UNIT}":  unit,
        }
        if with_description:
            item["{#OBD_DESC}"] = description
        lld_data.append(item)

    lld_json = json.dumps({"data": lld_data}, ensure_ascii=False)
    log.info("Sending LLD discovery to Zabbix (%d PIDs, with_description=%s)...", len(lld_data), with_description)
    dispatch_zabbix("obd.discovery", lld_json)


# ------------------------------------------
# FUNCTION: discover_text_metrics
# Returns a list of tuples (cmd, suffix, name)
# for PIDs returning textual values.
# ------------------------------------------
OBD_TEXT_DESCRIPTIONS = {
    "VIN":            "Vehicle Identification Number — unique 17-character chassis identifier.\nEncodes manufacturer, model, engine, plant and serial number.",
    "CALIBRATION_ID": "ECU software calibration ID — identifies the firmware version on the Engine Control Unit.\nUsed by technicians to verify correct calibration for the vehicle variant.",
    "CVN":            "Calibration Verification Number — a checksum of the ECU calibration data.\nUsed by emissions inspectors to confirm the ECU software has not been tampered with.",
    "FUEL_STATUS":    "Fuel system operating mode reported by the ECU.\n'Closed loop' = using O2 sensor feedback (normal warm operation).\n'Open loop' = ignoring O2 sensors (normal during cold start or wide-open throttle).",
    "OBD_COMPLIANCE": "OBD compliance standard reported by the ECU.\n'Brazil OBD Phase 2 (OBDBr-2)' = compliance with PROCONVE L6 emissions regulation.",
    "ELM_VERSION":    "Firmware version of the ELM327 OBD-to-Bluetooth adapter.\nTranslates between the vehicle OBD-II CAN bus and the TCP interface used by this monitoring script.",
}


def discover_text_metrics(conn) -> list:
    metrics = []
    supported_cmds = sorted(conn.supported_commands, key=lambda c: c.name)
    for cmd in supported_cmds:
        if cmd.name not in TEXT_COMMANDS:
            continue
        suffix    = cmd.name.lower()
        name      = getattr(cmd, "desc", cmd.name)
        description = OBD_TEXT_DESCRIPTIONS.get(cmd.name, "Textual OBD2 PID: " + cmd.name)
        metrics.append((cmd, suffix, name, description))
    names = [s for _, s, _, _ in metrics]
    log.info("Textual PIDs supported by ECU (%d): %s", len(metrics), ", ".join(names))
    return metrics


# ------------------------------------------
# FUNCTION: send_text_discovery
# Sends LLD payload for the
# discovery of textual items in Zabbix.
# ------------------------------------------
def send_text_discovery(text_metrics: list, with_description: bool = True) -> None:
    if not text_metrics:
        return
    lld_data = []
    for _, suffix, name, description in text_metrics:
        item = {
            "{#OBD_KEY}":  suffix,
            "{#OBD_NAME}": name,
        }
        if with_description:
            item["{#OBD_DESC}"] = description
        lld_data.append(item)

    lld_json = json.dumps({"data": lld_data}, ensure_ascii=False)
    log.info("Sending textual LLD discovery to Zabbix (%d PIDs, with_description=%s)...", len(lld_data), with_description)
    dispatch_zabbix("obd.discovery.text", lld_json)


# ------------------------------------------
# FUNCTION: extract_text_value
# Converts non-numeric OBD response to
# a readable string. Supports strings,
# bytes, lists/tuples, and objects with __str__.
# ------------------------------------------
def _decode_bytes(val):
    if isinstance(val, (bytes, bytearray)):
        return val.decode("utf-8", errors="replace")
    return str(val)

def extract_text_value(response) -> str | None:
    value = response.value
    if value is None:
        log.debug("Resposta nula para PID '%s'", response.command.name)
        return None
    if isinstance(value, (bytes, bytearray)):
        return _decode_bytes(value).strip() or None
    if isinstance(value, str):
        return value.strip() or None
    if isinstance(value, (list, tuple)):
        parts = [_decode_bytes(v).strip() for v in value if v is not None]
        return ", ".join(parts) if parts else None
    return str(value).strip() or None


# ------------------------------------------
# FUNCTION: extract_value
# Attempts to extract a scalar value from the OBD response.
# Supports Pint quantities (magnitude),
# numbers, and booleans. Returns None if the
# type is not convertible.
# ------------------------------------------
def extract_value(response):
    value = response.value
    # Grandeza com unit (ex.: RPM, km/h, °C)
    if hasattr(value, "magnitude"):
        return round(float(value.magnitude), 2)
    # Objeto Monitor do python-obd (testes de emissão — modo 06)
    # Monitor.tests → lista de MonitorTest com dados reais da ECU
    # MonitorTest.passed → True se value está entre min e max
    # Retorna 1.0 se todos os testes passaram, 0.0 se algum falhou,
    # None se a ECU não retornou subtestes (monitor sem dados).
    if hasattr(value, "tests") and hasattr(value, "_tests"):
        subtestes = value.tests  # só os não-nulos
        if not subtestes:
            return None          # ECU não forneceu dados para este monitor
        return 1.0 if all(t.passed for t in subtestes) else 0.0
    # Número puro ou booleano
    if isinstance(value, (int, float, bool)):
        return round(float(value), 2)
    # Unsupported type (list, complex object, etc.)
    return None


# ------------------------------------------
# FUNCTION: connect_obd
# Attempts to establish connection with the ECU via
# TCP bridge. Returns the connected OBD object
# or None if the attempt fails.
# ------------------------------------------
def connect_obd():
    log.info("Attempting connection with the ECU via %s ...", OBD_URI)
    try:
        conn = obd.OBD(OBD_URI, fast=False)
        if conn.is_connected():
            log.info("[OK] Conectado à ECU — Protocol: %s", conn.protocol_name())
            return conn
        else:
            log.warning("python-obd library returned object without active connection.")
            conn.close()
            return None
    except obd.OBDException as exc:
        log.warning("OBD failure in connection attempt: %s", exc)
        return None
    except ConnectionRefusedError:
        log.warning(
            "Connection refused at %s — TCP bridge offline or Bluetooth app inactive.", OBD_URI
        )
        return None
    except OSError as exc:
        log.warning("Socket error in connection attempt: %s", exc)
        return None
    except Exception as exc:  # pylint: disable=broad-except
        log.warning("Unexpected error in connection attempt: %s", exc)
        return None


def collect_and_dispatch_text(conn, text_metrics: list) -> bool:
    text_batch = []
    for cmd, suffix, _name, _description in text_metrics:
        key = f"obd.text[{suffix}]"
        try:
            response = conn.query(cmd)
            if response.is_null():
                continue
            value = extract_text_value(response)
            if value is None:
                continue
            log.info("%-35s = %s", key, value)
            text_batch.append({"key": key, "value": value})
        except OSError as exc:
            log.error("Connection loss during textual polling of '%s': %s", key, exc)
            return False
        except Exception as exc:  # pylint: disable=broad-except
            log.error("Unexpected error querying '%s': %s", key, exc)

    if text_batch:
        dispatch_zabbix_batch(text_batch)

    return True

# ------------------------------------------
# FUNCTION: collect_and_dispatch
# Performs polling of all discovered
# PIDs, collects values and sends them
# to Zabbix in a single TCP batch.
# Returns False if connection is lost.
# ------------------------------------------
def collect_and_dispatch(conn, metrics: list) -> bool:
    batch = []
    for cmd, suffix, _name, _unit, _description in metrics:
        key = f"obd[{suffix}]"
        try:
            response = conn.query(cmd)

            if response.is_null():
                log.debug("Null response for PID '%s' — ignoring.", key)
                continue

            value = extract_value(response)
            if value is None:
                log.debug("PID '%s' returned non-numeric type — ignoring.", key)
                continue

            log.info("%-35s = %s", key, value)
            batch.append({"key": key, "value": value})

        except obd.OBDException as exc:
            log.warning("OBD error querying '%s': %s", key, exc)
            # Error in one PID doesn't interrupt others; loop continues.

        except OSError as exc:
            # Socket loss during read — signals necessary reconnection
            log.error("Connection loss during polling of '%s': %s", key, exc)
            return False  # <-- triggers the reconnection loop

        except Exception as exc:  # pylint: disable=broad-except
            log.error("Unexpected error querying '%s': %s", key, exc)

    if batch:
        dispatch_zabbix_batch(batch)

    return True  # Polling completed without connection loss


# ------------------------------------------
# MAIN LOOP WITH EXPONENTIAL BACKOFF
# Lifecycle:
#   1. Tries to connect.
#   2. If fails → waits backoff and doubles
#      o intervalo (até BACKOFF_MAXIMO).
#   3. If connects → discovers PIDs, sends
#      LLD payload and starts polling at 1 Hz.
#   4. Every DISCOVERY_INTERVAL → resends
#      o payload LLD (items novos/removidos).
#   5. If connection drops during polling →
#      returns to step 1.
# ------------------------------------------
def main() -> None:
    log.info("=" * 60)
    log.info("OBD2 Telemetry Pipeline — Starting")
    log.info("OBD URI  : %s", OBD_URI)
    log.info("Zabbix   : %s:%s  Host='%s'", ZABBIX_SERVER, ZABBIX_PORT, ZABBIX_HOST)
    log.info("=" * 60)

    backoff           = BACKOFF_BASE_SECONDS
    connection        = None
    metrics          = []
    text_metrics    = []
    last_discovery = 0.0  # Unix timestamp of the last LLD send
    first_discovery_session = True

    try:
        while True:
            # ── Connection Phase ──────────────────────────────────────
            if connection is None or not connection.is_connected():
                connection = connect_obd()

                if connection is None:
                    log.warning(
                        "Reconnection failed. Waiting %ds before next attempt "
                        "(max backoff: %ds).",
                        backoff,
                        BACKOFF_MAX_SECONDS,
                    )
                    time.sleep(backoff)
                    # Exponential backoff with ceiling
                    backoff = min(backoff * BACKOFF_FACTOR, BACKOFF_MAX_SECONDS)
                    continue  # tries again

                # Successful connection: discovers PIDs and resets backoff
                metrics          = discover_metrics(connection)
                text_metrics    = discover_text_metrics(connection)
                backoff           = BACKOFF_BASE_SECONDS
                last_discovery = 0.0  # Forces immediate send of LLD discovery
                first_discovery_session = True

            # ── Periodic send of LLD discovery ────────────────────
            now = time.time()
            if now - last_discovery >= DISCOVERY_INTERVAL:
                send_discovery(metrics, with_description=first_discovery_session)
                send_text_discovery(text_metrics, with_description=first_discovery_session)
                
                # Collects and dispatches textual metrics only during the discovery phase (once per hour)
                connection_ok = collect_and_dispatch_text(connection, text_metrics)
                if not connection_ok:
                    log.warning("Connection with ECU lost during textual polling. Closing session and reconnecting...")
                    try:
                        connection.close()
                    except Exception:  # pylint: disable=broad-except
                        pass
                    connection = None
                    log.info("Waiting %ds before reconnecting...", backoff)
                    time.sleep(backoff)
                    backoff = min(backoff * BACKOFF_FACTOR, BACKOFF_MAX_SECONDS)
                    continue
                
                first_discovery_session = False
                last_discovery = now

            # ── Polling Phase ───────────────────────────────────────
            connection_ok = collect_and_dispatch(connection, metrics)

            if not connection_ok:
                log.warning("Connection with ECU lost. Closing session and reconnecting...")
                try:
                    connection.close()
                except Exception:  # pylint: disable=broad-except
                    pass
                connection = None
                # Applies immediate backoff before reconnecting
                log.info("Waiting %ds before reconnecting...", backoff)
                time.sleep(backoff)
                backoff = min(backoff * BACKOFF_FACTOR, BACKOFF_MAX_SECONDS)
                continue

            time.sleep(POLLING_INTERVAL)

    except KeyboardInterrupt:
        log.info("Polling interrupted by user (Ctrl+C). Exiting.")
    finally:
        if connection is not None:
            try:
                connection.close()
                log.info("OBD connection successfully closed.")
            except Exception:  # pylint: disable=broad-except
                pass
        log.info("Pipeline terminated.")


if __name__ == "__main__":
    main()

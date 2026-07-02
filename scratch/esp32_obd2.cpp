#include <WiFi.h>
#include <BluetoothSerial.h>

// ==========================================
// GENERAL SETTINGS
// ==========================================
#define LED_PIN 2

const char* WIFI_SSID = "Galaxy S10+";
const char* WIFI_PASS = "Blister169";

const char* ZABBIX_SERVER = "jholiv-zabbix.ddns.net"; 
const int ZABBIX_PORT = 10051;
const char* ZABBIX_HOST = "Volkswagen UP ESP32";

uint8_t ELM327_MAC_BYTES[6] = {0x01, 0x23, 0x45, 0x67, 0x89, 0xBA};
const char* ELM327_PIN = "1234";

BluetoothSerial SerialBT;
WiFiClient client;
volatile int appState = 0; 
unsigned long lastDiscoveryTime = 0;
const unsigned long DISCOVERY_INTERVAL = 3600000; // 1 hour

struct PIDDef {
  byte pid;
  String suffix;
  String name;
  String unit;
};

// Known Numeric PIDs with standard SAE formulas
const PIDDef KNOWN_PIDS[] = {
  {0x04, "engine_load", "Engine Load", "%"},
  {0x05, "coolant_temp", "Engine Coolant Temp", "°C"},
  {0x06, "short_fuel_trim_1", "Short Term Fuel Trim 1", "%"},
  {0x07, "long_fuel_trim_1", "Long Term Fuel Trim 1", "%"},
  {0x0B, "intake_pressure", "Intake Manifold Pressure", "kPa"},
  {0x0C, "rpm", "Engine RPM", "rpm"},
  {0x0D, "speed", "Vehicle Speed", "km/h"},
  {0x0E, "timing_advance", "Timing Advance", "°"},
  {0x0F, "intake_temp", "Intake Air Temp", "°C"},
  {0x10, "maf", "MAF Air Flow Rate", "g/s"},
  {0x11, "throttle_pos", "Throttle Position", "%"},
  {0x14, "o2_b1s1", "O2 Sensor B1S1 Voltage", "V"},
  {0x15, "o2_b1s2", "O2 Sensor B1S2 Voltage", "V"},
  {0x1F, "run_time", "Engine Run Time", "s"},
  {0x2C, "commanded_egr", "Commanded EGR", "%"},
  {0x2F, "fuel_level", "Fuel Level", "%"},
  {0x31, "distance_since_dtc_clear", "Distance Since DTCs Cleared", "km"},
  {0x33, "barometric_pressure", "Barometric Pressure", "kPa"},
  {0x42, "control_module_voltage", "Control Module Voltage", "V"},
  {0x46, "ambient_air_temp", "Ambient Air Temp", "°C"},
  {0x5A, "accelerator_pos_d", "Accelerator Pedal Position D", "%"},
  {0x5C, "oil_temp", "Engine Oil Temp", "°C"}
};
const int NUM_KNOWN_PIDS = sizeof(KNOWN_PIDS)/sizeof(PIDDef);

PIDDef activePIDs[30];
int numActivePIDs = 0;
bool isDiscoveryDone = false;

// Task declarations...

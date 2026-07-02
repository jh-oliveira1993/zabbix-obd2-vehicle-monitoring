#include <WiFi.h>
#include <BluetoothSerial.h>

// ==========================================
// GENERAL SETTINGS (USER CONFIGURABLE)
// ==========================================
// Built-in LED Pin (Usually GPIO 2 on generic ESP32 boards)
#define LED_PIN 2

const char* WIFI_SSID = "YOUR_WIFI_SSID";
const char* WIFI_PASS = "YOUR_WIFI_PASSWORD";

const char* ZABBIX_SERVER = "YOUR_ZABBIX_IP_OR_DNS"; // Zabbix Server IP/DNS
const int ZABBIX_PORT = 10051;
const char* ZABBIX_HOST = "YOUR_ZABBIX_HOSTNAME";      // Hostname configured in Zabbix

// Hardcode the MAC address of your ELM327 adapter
uint8_t ELM327_MAC_BYTES[6] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
const char* ELM327_PIN = "1234";

// ==========================================
// GLOBAL VARIABLES
// ==========================================
BluetoothSerial SerialBT;
WiFiClient client;

// App State for the FreeRTOS LED Task
// 0 = Searching Wi-Fi, 1 = Searching BT, 2 = Connected, 3 = Transmitting
volatile int appState = 0; 

// Discovery and polling intervals
unsigned long lastDiscoveryTime = 0;
const unsigned long DISCOVERY_INTERVAL = 3600000; // 1 hour in ms

struct PIDDef {
  byte pid;
  String suffix;
  String name;
  String unit;
};

// Known Numeric PIDs with standard SAE formulas
const PIDDef KNOWN_PIDS[] = {
  {0x04, "engine_load", "Engine Load", "%"},
  {0x05, "coolant_temp", "Engine Coolant Temp", "C"},
  {0x06, "short_fuel_trim_1", "Short Term Fuel Trim 1", "%"},
  {0x07, "long_fuel_trim_1", "Long Term Fuel Trim 1", "%"},
  {0x0B, "intake_pressure", "Intake Manifold Pressure", "kPa"},
  {0x0C, "rpm", "Engine RPM", "rpm"},
  {0x0D, "speed", "Vehicle Speed", "km/h"},
  {0x0E, "timing_advance", "Timing Advance", "deg"},
  {0x0F, "intake_temp", "Intake Air Temp", "C"},
  {0x10, "maf", "MAF Air Flow Rate", "g/s"},
  {0x11, "throttle_pos", "Throttle Position", "%"},
  {0x14, "o2_b1s1", "O2 Sensor B1S1 Voltage", "V"},
  {0x15, "o2_b1s2", "O2 Sensor B1S2 Voltage", "V"},
  {0x1F, "run_time", "Engine Run Time", "s"},
  {0x2C, "commanded_egr", "Commanded EGR", "%"},
  {0x2F, "fuel_level", "Fuel Level", "%"},
  {0x31, "distance_since_dtc_clear", "Distance Since DTCs Cleared", "km"},
  {0x33, "barometric_pressure", "Barometric Pressure", "kPa"},
  {0x3C, "catalyst_temp_b1s1", "Catalyst Temp B1S1", "C"},
  {0x42, "control_module_voltage", "Control Module Voltage", "V"},
  {0x43, "absolute_load", "Absolute Load", "%"},
  {0x44, "commanded_equiv_ratio", "Commanded Equiv Ratio", ""},
  {0x45, "relative_throttle_pos", "Relative Throttle Pos", "%"},
  {0x46, "ambiant_air_temp", "Ambient Air Temp", "C"},
  {0x47, "throttle_pos_b", "Throttle Position B", "%"},
  {0x49, "accelerator_pos_d", "Accelerator Pedal Pos D", "%"},
  {0x4A, "accelerator_pos_e", "Accelerator Pedal Pos E", "%"},
  {0x4C, "throttle_actuator", "Commanded Throttle Actuator", "%"},
  {0x52, "ethanol_percent", "Ethanol Percent", "%"},
  {0x5C, "oil_temp", "Engine Oil Temp", "C"}
};
const int NUM_KNOWN_PIDS = sizeof(KNOWN_PIDS)/sizeof(PIDDef);

PIDDef activePIDs[30];
int numActivePIDs = 0;
bool isDiscoveryDone = false;

// Function prototypes
void initOBD2();
String sendCommand(String cmd);
void discoverPIDs();
void sendZabbixLLD();
void sendZabbixBatch(String payloadJson);
float queryAndParsePID(byte pid);

// FreeRTOS Task for Asynchronous LED Blinking (Runs on Core 0)
void ledTaskCode(void * parameter) {
  for(;;) {
    if (appState == 0) {
      digitalWrite(LED_PIN, !digitalRead(LED_PIN));
      vTaskDelay(250 / portTICK_PERIOD_MS);
    } 
    else if (appState == 1) {
      for(int i=0; i<3; i++) {
        digitalWrite(LED_PIN, HIGH); vTaskDelay(100 / portTICK_PERIOD_MS);
        digitalWrite(LED_PIN, LOW);  vTaskDelay(100 / portTICK_PERIOD_MS);
      }
      vTaskDelay(1400 / portTICK_PERIOD_MS); 
    } 
    else if (appState == 2) {
      digitalWrite(LED_PIN, HIGH);
      vTaskDelay(500 / portTICK_PERIOD_MS);
    } 
    else if (appState == 3) {
      digitalWrite(LED_PIN, LOW);
      vTaskDelay(50 / portTICK_PERIOD_MS);
      digitalWrite(LED_PIN, HIGH);
      appState = 2;
    }
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  xTaskCreatePinnedToCore(ledTaskCode, "LEDTask", 10000, NULL, 1, NULL, 0);

  Serial.println("\n--- Starting Dynamic Zabbix OBD2 Telemetry on ESP32 ---");
  
  appState = 0;
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("Connecting to Wi-Fi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\n[OK] Wi-Fi Connected! IP: " + WiFi.localIP().toString());

  SerialBT.begin("ESP32_OBD2", true);
  SerialBT.setPin(ELM327_PIN);
  Serial.println("Starting Bluetooth... Connecting to ELM327 (MAC 01:23:45:67:89:BA)...");
  
  bool connected = SerialBT.connect(ELM327_MAC_BYTES);
  if(connected) {
    Serial.println("[OK] Connected to ELM327 successfully!");
    appState = 2;
    initOBD2();
  } else {
    Serial.println("[ERROR] Failed to connect to ELM327.");
    appState = 1;
  }
}

void loop() {
  if (SerialBT.connected() && WiFi.status() == WL_CONNECTED) {
    appState = 2;
    unsigned long now = millis();
    
    // Perform discovery if not done, or if 1 hour has passed
    if (!isDiscoveryDone || (now - lastDiscoveryTime >= DISCOVERY_INTERVAL)) {
      discoverPIDs();
      sendZabbixLLD();
      lastDiscoveryTime = now;
      isDiscoveryDone = true;
    }
    
    // Polling Phase
    if (numActivePIDs > 0) {
      String batchPayload = "{\"request\":\"sender data\",\"data\":[";
      bool firstItem = true;
      int successfulReads = 0;
      
      for (int i = 0; i < numActivePIDs; i++) {
        float value = queryAndParsePID(activePIDs[i].pid);
        if (value != -999.0) {
          if (!firstItem) batchPayload += ",";
          batchPayload += "{\"host\":\"" + String(ZABBIX_HOST) + "\",";
          batchPayload += "\"key\":\"obd[" + activePIDs[i].suffix + "]\",";
          batchPayload += "\"value\":\"" + String(value, 2) + "\"}";
          firstItem = false;
          successfulReads++;
          Serial.printf("%-20s = %.2f %s\n", activePIDs[i].name.c_str(), value, activePIDs[i].unit.c_str());
        }
      }
      batchPayload += "]}";
      
      if (successfulReads > 0) {
        appState = 3; // Transmitting
        sendZabbixBatch(batchPayload);
      }
    }
    
    Serial.println("----------------------------------------");
    delay(1000); // Wait 1 second before next polling cycle (matches 1Hz python script)
    
  } else {
    Serial.println("[WARNING] Connection lost. Attempting to reconnect...");
    
    if (WiFi.status() != WL_CONNECTED) {
      appState = 0;
      Serial.print("-> Reconnecting Wi-Fi...");
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASS);
      int retries = 0;
      while (WiFi.status() != WL_CONNECTED && retries < 15) {
        delay(500); Serial.print("."); retries++;
      }
      if(WiFi.status() == WL_CONNECTED) {
        Serial.println("\n[OK] Wi-Fi Reconnected!");
      }
    }
    
    if (WiFi.status() == WL_CONNECTED && !SerialBT.connected()) {
      appState = 1;
      Serial.println("-> Reconnecting Bluetooth to ELM327...");
      bool connected = SerialBT.connect(ELM327_MAC_BYTES);
      if(connected) {
        Serial.println("[OK] Bluetooth reconnected successfully!");
        appState = 2;
        initOBD2();
      }
    }
    delay(2000);
  }
}

// ==========================================
// HELPER FUNCTIONS
// ==========================================

void initOBD2() {
  sendCommand("ATZ");   delay(1000);
  sendCommand("ATE0");  delay(200);
  sendCommand("ATL0");  delay(200);
  sendCommand("ATH0");  delay(200);
  sendCommand("ATSP0"); delay(200);
  Serial.println("[OK] ELM327 Initialized and ready to read PIDs.");
}

String sendCommand(String cmd) {
  SerialBT.print(cmd + "\r");
  String response = "";
  long timeout = millis() + 2000;
  
  while(millis() < timeout) {
    if(SerialBT.available()) {
      char c = SerialBT.read();
      if(c == '>') break;
      response += c;
    }
  }
  response.replace("\r", "");
  response.replace("\n", "");
  response.replace(" ", "");
  return response;
}

// Checks if a bit is set in a 32-bit integer (bit 0 is MSB in OBD2 bitmask)
bool isPidSupported(uint32_t bitmask, int pidOffset) {
  if (pidOffset < 1 || pidOffset > 32) return false;
  return (bitmask & (1ULL << (32 - pidOffset))) != 0;
}

void discoverPIDs() {
  Serial.println("Starting Dynamic PID Discovery...");
  bool supported[256] = {false};
  
  // We check 0100, 0120, 0140, 0160
  for (int base = 0x00; base <= 0x60; base += 0x20) {
    char cmd[5];
    sprintf(cmd, "01%02X", base);
    Serial.print("Querying: "); Serial.println(cmd);
    String resp = sendCommand(cmd);
    Serial.print("Response: "); Serial.println(resp);
    
    char prefix[5];
    sprintf(prefix, "41%02X", base);
    
    int idx = resp.indexOf(prefix);
    if (idx != -1 && resp.length() >= idx + 12) {
      String hexMask = resp.substring(idx + 4, idx + 12); // 8 hex chars = 32 bits
      Serial.print("Bitmask hex: "); Serial.println(hexMask);
      uint32_t bitmask = strtoul(hexMask.c_str(), NULL, 16);
      
      for (int i = 1; i <= 32; i++) {
        if (isPidSupported(bitmask, i)) {
          int pid = base + i;
          if (pid < 256) supported[pid] = true;
          Serial.printf("Supported PID: %02X\n", pid);
        }
      }
      
      // If the last PID in this group (e.g. 0x20) is not supported, stop polling further
      if (!isPidSupported(bitmask, 32)) {
        break;
      }
    } else {
      Serial.println("Invalid response, stopping discovery.");
      break; // Invalid response, stop discovery
    }
  }
  
  // Map supported PIDs to KNOWN_PIDS
  numActivePIDs = 0;
  for (int i = 0; i < NUM_KNOWN_PIDS; i++) {
    if (supported[KNOWN_PIDS[i].pid]) {
      activePIDs[numActivePIDs] = KNOWN_PIDS[i];
      numActivePIDs++;
      if (numActivePIDs >= 30) break; // Array limit
    }
  }
  
  Serial.printf("Discovery complete! %d known PIDs supported by ECU.\n", numActivePIDs);
}

void sendZabbixLLD() {
  if (numActivePIDs == 0) return;
  
  String lldValue = "{\"data\":[";
  bool first = true;
  for (int i = 0; i < numActivePIDs; i++) {
    if (!first) lldValue += ",";
    lldValue += "{\"{#OBD_KEY}\":\"" + activePIDs[i].suffix + "\",";
    lldValue += "\"{#OBD_NAME}\":\"" + activePIDs[i].name + "\",";
    lldValue += "\"{#OBD_UNIT}\":\"" + activePIDs[i].unit + "\",";
    lldValue += "\"{#OBD_DESC}\":\"OBD2 PID: " + activePIDs[i].name + "\"}";
    first = false;
  }
  lldValue += "]}";
  
  // Escape quotes in lldValue for inclusion in outer JSON
  lldValue.replace("\"", "\\\"");
  
  String batchPayload = "{\"request\":\"sender data\",\"data\":[{\"host\":\"" + String(ZABBIX_HOST) + "\",\"key\":\"obd.discovery\",\"value\":\"" + lldValue + "\"}]}";
  
  Serial.println("Sending LLD Discovery to Zabbix...");
  sendZabbixBatch(batchPayload);
}

void sendZabbixBatch(String payloadJson) {
  if (!client.connect(ZABBIX_SERVER, ZABBIX_PORT)) {
    Serial.println("[ERROR] TCP connection to Zabbix failed.");
    return;
  }

  int jsonLen = payloadJson.length();
  uint8_t header[13] = {
    'Z', 'B', 'X', 'D', 0x01,
    (uint8_t)(jsonLen & 0xFF), (uint8_t)((jsonLen >> 8) & 0xFF), 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00
  };

  client.write(header, 13);
  client.print(payloadJson);
  
  Serial.print("-> Zabbix: ");
  long timeout = millis() + 2000;
  while((client.connected() || client.available()) && millis() < timeout) {
    if(client.available()) {
      Serial.write(client.read());
    }
  }
  Serial.println("");
  client.stop();
}

float queryAndParsePID(byte pid) {
  char cmd[5];
  sprintf(cmd, "01%02X", pid);
  String resp = sendCommand(cmd);
  
  char prefix[5];
  sprintf(prefix, "41%02X", pid);
  
  int idx = resp.indexOf(prefix);
  if(idx != -1 && resp.length() >= idx + 6) {
    int A = 0, B = 0, C = 0, D = 0;
    
    if (resp.length() >= idx + 6) A = strtol(resp.substring(idx + 4, idx + 6).c_str(), NULL, 16);
    if (resp.length() >= idx + 8) B = strtol(resp.substring(idx + 6, idx + 8).c_str(), NULL, 16);
    if (resp.length() >= idx + 10) C = strtol(resp.substring(idx + 8, idx + 10).c_str(), NULL, 16);
    if (resp.length() >= idx + 12) D = strtol(resp.substring(idx + 10, idx + 12).c_str(), NULL, 16);
    
    switch(pid) {
      case 0x04: return (A * 100.0) / 255.0; // Engine Load
      case 0x05: return A - 40; // Coolant Temp
      case 0x06: return ((A - 128) * 100.0) / 128.0; // Short Fuel Trim
      case 0x07: return ((A - 128) * 100.0) / 128.0; // Long Fuel Trim
      case 0x0B: return A; // Intake Pressure
      case 0x0C: return ((A * 256.0) + B) / 4.0; // RPM
      case 0x0D: return A; // Speed
      case 0x0E: return (A / 2.0) - 64.0; // Timing Advance
      case 0x0F: return A - 40; // Intake Temp
      case 0x10: return ((A * 256.0) + B) / 100.0; // MAF
      case 0x11: return (A * 100.0) / 255.0; // Throttle Pos
      case 0x14: return A / 200.0; // O2 B1S1 Voltage
      case 0x15: return A / 200.0; // O2 B1S2 Voltage
      case 0x1F: return (A * 256.0) + B; // Run Time
      case 0x2C: return (A * 100.0) / 255.0; // Commanded EGR
      case 0x2F: return (A * 100.0) / 255.0; // Fuel Level
      case 0x31: return (A * 256.0) + B; // Distance Since Cleared
      case 0x33: return A; // Barometric Pressure
      case 0x3C: return (((A * 256.0) + B) / 10.0) - 40.0; // Catalyst Temp B1S1
      case 0x42: return ((A * 256.0) + B) / 1000.0; // Control Module Voltage
      case 0x43: return ((A * 256.0) + B) * 100.0 / 255.0; // Absolute Load
      case 0x44: return ((A * 256.0) + B) / 32768.0; // Commanded Equivalence Ratio
      case 0x45: return (A * 100.0) / 255.0; // Relative Throttle Pos
      case 0x46: return A - 40; // Ambient Air Temp
      case 0x47: return (A * 100.0) / 255.0; // Throttle Pos B
      case 0x49: return (A * 100.0) / 255.0; // Accelerator Pedal Pos D
      case 0x4A: return (A * 100.0) / 255.0; // Accelerator Pedal Pos E
      case 0x4C: return (A * 100.0) / 255.0; // Commanded Throttle Actuator
      case 0x52: return (A * 100.0) / 255.0; // Ethanol Percent
      case 0x5C: return A - 40; // Oil Temp
      default: return -999.0;
    }
  }
  return -999.0;
}

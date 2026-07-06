#include <WiFi.h>
#include <BluetoothSerial.h>

// ==========================================
// GENERAL SETTINGS (USER CONFIGURABLE)
// ==========================================
// Built-in LED Pin (Usually GPIO 2 on generic ESP32 boards)
#define LED_PIN 2

// Set to false to disable ESP32 self-monitoring metrics (saves Flash/RAM)
const bool ENABLE_SELF_MONITORING = true;

#include "secrets.h"

// ==========================================
// GLOBAL VARIABLES
// ==========================================
BluetoothSerial SerialBT;
WiFiClient client;

// App State for the FreeRTOS LED Task
// 0 = Searching Wi-Fi, 1 = Searching BT, 2 = Connected, 3 = Transmitting
volatile int appState = 0; 

// Discovery, polling and monitor intervals
unsigned long lastDiscoveryTime = 0;
unsigned long lastMonitorTime   = 0;
const unsigned long DISCOVERY_INTERVAL = 3600000; // 1 hour in ms
const unsigned long MONITOR_INTERVAL   = 10000;   // 10 seconds in ms

// Application-level telemetry counters (reset only on hardware reboot)
uint32_t obd2FailedReads    = 0; // PID reads that returned no valid data
uint32_t obd2ReconnectCount = 0; // Total BT + Wi-Fi reconnection attempts since boot
uint32_t zabbixSendErrors   = 0; // Failed TCP connections to Zabbix server

struct PIDDef {
  byte pid;
  String suffix;
  String name;
  String unit;
};

// Known Numeric PIDs with standard SAE formulas
const PIDDef KNOWN_PIDS[] = {
  {0x03, "fuel_status", "Fuel System Status", ""},
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
  {0x21, "distance_w_mil", "Distance w/ MIL", "km"},
  {0x2C, "commanded_egr", "Commanded EGR", "%"},
  {0x2E, "evaporative_purge", "Evaporative Purge", "%"},
  {0x2F, "fuel_level", "Fuel Level", "%"},
  {0x30, "warmups_since_dtc_clear", "Warm-ups Since DTC Clear", ""},
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
  {0x50, "max_maf", "Max MAF Rate", "g/s"},
  {0x52, "ethanol_percent", "Ethanol Percent", "%"},
  {0x5C, "oil_temp", "Engine Oil Temp", "C"}
};
const int NUM_KNOWN_PIDS = sizeof(KNOWN_PIDS)/sizeof(PIDDef);

PIDDef activePIDs[40];
int numActivePIDs = 0;
bool isDiscoveryDone = false;

// Function prototypes
void initOBD2();
String sendCommand(String cmd);
void discoverPIDs();
void sendZabbixLLD();
void sendZabbixBatch(String payloadJson, const char* host);
void collectAndSendMonitorMetrics();
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
  unsigned long now = millis();

  // ── Self-monitoring: runs whenever Wi-Fi is available ────────────────────
  // Independent of the Bluetooth connection to the ELM327. Allows monitoring
  // the ESP32 health even when the vehicle is off or the adapter is absent.
  if (ENABLE_SELF_MONITORING && WiFi.status() == WL_CONNECTED && now - lastMonitorTime >= MONITOR_INTERVAL) {
    collectAndSendMonitorMetrics();
    lastMonitorTime = now;
  }

  // ── OBD2: requires both Wi-Fi and Bluetooth (ELM327) to be active ────────
  if (SerialBT.connected() && WiFi.status() == WL_CONNECTED) {
    appState = 2;

    // Perform discovery if not done, or if 1 hour has passed
    if (!isDiscoveryDone || (now - lastDiscoveryTime >= DISCOVERY_INTERVAL)) {
      discoverPIDs();
      sendZabbixLLD();
      lastDiscoveryTime = now;
      isDiscoveryDone = true;
    }
    
    // Polling Phase: OBD2 metrics -> ZABBIX_HOST
    if (numActivePIDs > 0) {
      String batchPayload = "{\"request\":\"sender data\",\"data\":[";
      batchPayload.reserve(4096);
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
        } else {
          obd2FailedReads++; // Count invalid / no-response PID reads
        }
      }
      batchPayload += "]}";
      
      if (successfulReads > 0) {
        appState = 3; // Transmitting
        sendZabbixBatch(batchPayload, ZABBIX_HOST);
      }
    }

    Serial.println("----------------------------------------");
    delay(1000); // Wait 1 second before next polling cycle (matches 1Hz python script)
    
  } else {
    Serial.println("[WARNING] Connection lost. Attempting to reconnect...");
    
    if (WiFi.status() != WL_CONNECTED) {
      appState = 0;
      obd2ReconnectCount++;
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
      obd2ReconnectCount++;
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
    
    int idx = 0;
    uint32_t combinedBitmask = 0;
    bool foundAny = false;
    
    // Find all occurrences of the response prefix (to handle multiple ECUs)
    while ((idx = resp.indexOf(prefix, idx)) != -1) {
      if (resp.length() >= idx + 12) {
        String hexMask = resp.substring(idx + 4, idx + 12);
        uint32_t bitmask = strtoul(hexMask.c_str(), NULL, 16);
        combinedBitmask |= bitmask;
        foundAny = true;
      }
      idx += 12; // Advance to find next ECU response
    }
    
    if (foundAny) {
      Serial.printf("Combined Bitmask hex: %08X\n", combinedBitmask);
      
      for (int i = 1; i <= 32; i++) {
        if (isPidSupported(combinedBitmask, i)) {
          int pid = base + i;
          if (pid < 256) supported[pid] = true;
          Serial.printf("Supported PID: %02X\n", pid);
        }
      }
      
      // If the last PID in this group (e.g. 0x20) is not supported, stop polling further
      if (!isPidSupported(combinedBitmask, 32)) {
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
      if (numActivePIDs >= 40) break; // Array limit
    }
  }
  
  Serial.printf("Discovery complete! %d known PIDs supported by ECU.\n", numActivePIDs);
}

void sendZabbixLLD() {
  if (numActivePIDs == 0) return;
  
  String lldValue;
  lldValue.reserve(4096);
  lldValue = "{\"data\":[";
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
  
  String batchPayload;
  batchPayload.reserve(4096);
  batchPayload = "{\"request\":\"sender data\",\"data\":[{\"host\":\"" + String(ZABBIX_HOST) + "\",\"key\":\"obd.discovery\",\"value\":\"" + lldValue + "\"}]}";
  
  Serial.println("Sending LLD Discovery to Zabbix...");
  sendZabbixBatch(batchPayload, ZABBIX_HOST);
}

// ------------------------------------------
// FUNCTION: collectAndSendMonitorMetrics
// Collects ESP32 internal health metrics and
// sends them to the ZABBIX_MONITOR_HOST.
// ------------------------------------------
void collectAndSendMonitorMetrics() {
  Serial.println("[MONITOR] Collecting ESP32 self-monitoring metrics...");

  // --- Hardware metrics ---
  uint32_t heapFree     = ESP.getFreeHeap();
  uint32_t heapTotal    = ESP.getHeapSize();
  uint32_t heapMinFree  = ESP.getMinFreeHeap();
  uint32_t heapMaxAlloc = ESP.getMaxAllocHeap();
  float    heapUsedPct  = 100.0 * (1.0 - ((float)heapFree / (float)heapTotal));
  int32_t  rssi         = WiFi.RSSI();
  int32_t  channel      = WiFi.channel();
  uint32_t uptime       = millis() / 1000;
  uint32_t cpuFreq      = ESP.getCpuFreqMHz();
  uint32_t taskCount    = uxTaskGetNumberOfTasks();
  float    chipTemp     = temperatureRead();
  int      resetReason  = (int)esp_reset_reason();

  // --- Flash metrics ---
  uint32_t flashTotal      = ESP.getFlashChipSize();
  uint32_t sketchSize      = ESP.getSketchSize();
  uint32_t sketchFreeSpace = ESP.getFreeSketchSpace();

  // --- PSRAM metrics (0 if not available) ---
  uint32_t psramTotal = ESP.getPsramSize();
  uint32_t psramFree  = ESP.getFreePsram();

  // --- Application metrics ---
  uint8_t  btConnected = SerialBT.connected() ? 1 : 0;

  Serial.printf("  Heap Free:      %u bytes (%.1f%% used)\n", heapFree, heapUsedPct);
  Serial.printf("  Heap Min Free:  %u bytes\n", heapMinFree);
  Serial.printf("  Wi-Fi RSSI:     %d dBm  (Ch %d)\n", rssi, channel);
  Serial.printf("  Uptime:         %u s\n", uptime);
  Serial.printf("  CPU Freq:       %u MHz\n", cpuFreq);
  Serial.printf("  FreeRTOS Tasks: %u\n", taskCount);
  Serial.printf("  Chip Temp:      %.1f C\n", chipTemp);
  Serial.printf("  Flash Total:    %u bytes | Sketch: %u | Free: %u\n", flashTotal, sketchSize, sketchFreeSpace);
  Serial.printf("  PSRAM:          total=%u free=%u\n", psramTotal, psramFree);
  Serial.printf("  BT Connected:   %u | Active PIDs: %d\n", btConnected, numActivePIDs);
  Serial.printf("  Failed Reads:   %u | Reconnects: %u | Zabbix Errors: %u\n",
                obd2FailedReads, obd2ReconnectCount, zabbixSendErrors);

  String h = String(ZABBIX_MONITOR_HOST);
  String payload;
  payload.reserve(2048);
  payload  = "{\"request\":\"sender data\",\"data\":[";

  // Heap
  payload += "{\"host\":\"" + h + "\",\"key\":\"esp32.heap.free\",\"value\":\""        + String(heapFree)        + "\"},";
  payload += "{\"host\":\"" + h + "\",\"key\":\"esp32.heap.total\",\"value\":\""       + String(heapTotal)       + "\"},";
  payload += "{\"host\":\"" + h + "\",\"key\":\"esp32.heap.min_free\",\"value\":\""   + String(heapMinFree)     + "\"},";
  payload += "{\"host\":\"" + h + "\",\"key\":\"esp32.heap.max_alloc\",\"value\":\""  + String(heapMaxAlloc)    + "\"},";
  payload += "{\"host\":\"" + h + "\",\"key\":\"esp32.heap.used_pct\",\"value\":\""   + String(heapUsedPct, 1)  + "\"},";

  // Wi-Fi
  payload += "{\"host\":\"" + h + "\",\"key\":\"esp32.wifi.rssi\",\"value\":\""       + String(rssi)            + "\"},";
  payload += "{\"host\":\"" + h + "\",\"key\":\"esp32.wifi.channel\",\"value\":\""    + String(channel)         + "\"},";

  // System
  payload += "{\"host\":\"" + h + "\",\"key\":\"esp32.uptime\",\"value\":\""           + String(uptime)          + "\"},";
  payload += "{\"host\":\"" + h + "\",\"key\":\"esp32.cpu.freq\",\"value\":\""         + String(cpuFreq)         + "\"},";
  payload += "{\"host\":\"" + h + "\",\"key\":\"esp32.tasks\",\"value\":\""            + String(taskCount)       + "\"},";
  payload += "{\"host\":\"" + h + "\",\"key\":\"esp32.chip.temp\",\"value\":\""       + String(chipTemp, 1)     + "\"},";
  payload += "{\"host\":\"" + h + "\",\"key\":\"esp32.reset_reason\",\"value\":\""    + String(resetReason)     + "\"},";

  // Flash
  payload += "{\"host\":\"" + h + "\",\"key\":\"esp32.flash.total\",\"value\":\""     + String(flashTotal)      + "\"},";
  payload += "{\"host\":\"" + h + "\",\"key\":\"esp32.flash.sketch_size\",\"value\":\"" + String(sketchSize)    + "\"},";
  payload += "{\"host\":\"" + h + "\",\"key\":\"esp32.flash.free_space\",\"value\":\"" + String(sketchFreeSpace) + "\"},";

  // PSRAM
  payload += "{\"host\":\"" + h + "\",\"key\":\"esp32.psram.total\",\"value\":\""     + String(psramTotal)      + "\"},";
  payload += "{\"host\":\"" + h + "\",\"key\":\"esp32.psram.free\",\"value\":\""      + String(psramFree)       + "\"},";

  // Application
  payload += "{\"host\":\"" + h + "\",\"key\":\"esp32.obd2.bt_connected\",\"value\":\"" + String(btConnected)     + "\"},";
  payload += "{\"host\":\"" + h + "\",\"key\":\"esp32.obd2.active_pids\",\"value\":\"" + String(numActivePIDs)   + "\"},";
  payload += "{\"host\":\"" + h + "\",\"key\":\"esp32.obd2.failed_reads\",\"value\":\"" + String(obd2FailedReads) + "\"},";
  payload += "{\"host\":\"" + h + "\",\"key\":\"esp32.obd2.reconnects\",\"value\":\""  + String(obd2ReconnectCount) + "\"},";
  payload += "{\"host\":\"" + h + "\",\"key\":\"esp32.zabbix.send_errors\",\"value\":\"" + String(zabbixSendErrors) + "\"}";

  payload += "]}";

  sendZabbixBatch(payload, ZABBIX_MONITOR_HOST);
}

void sendZabbixBatch(String payloadJson, const char* host) {
  if (!client.connect(ZABBIX_SERVER, ZABBIX_PORT)) {
    Serial.println("[ERROR] TCP connection to Zabbix failed.");
    zabbixSendErrors++;
    return;
  }

  int jsonLen = payloadJson.length();
  uint8_t header[13] = {
    'Z', 'B', 'X', 'D', 0x01,
    (uint8_t)(jsonLen & 0xFF), (uint8_t)((jsonLen >> 8) & 0xFF), 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00
  };

  client.write(header, 13);
  
  // Send JSON in chunks to avoid overflowing the LWIP buffer (TCP Fragmentation)
  int bytesSent = 0;
  int totalBytes = payloadJson.length();
  const char* payloadPtr = payloadJson.c_str();
  
  Serial.printf("Sending %d bytes...\n", totalBytes);
  
  // Disable Nagle's algorithm to force immediate delivery of each chunk.
  // This prevents "TCP MTU Black Holes" on 4G mobile networks, where packets
  // larger than the mobile MTU (~1420 bytes) are silently dropped.
  client.setNoDelay(true);
  
  while(bytesSent < totalBytes) {
    if (!client.connected()) {
      Serial.println("[ERROR] Connection dropped mid-send!");
      break;
    }
    int chunk = totalBytes - bytesSent;
    if (chunk > 256) chunk = 256;
    int written = client.write((const uint8_t*)(payloadPtr + bytesSent), chunk);
    if (written > 0) {
      client.flush(); // Force immediate send and wait for ACK
      bytesSent += written;
    } else {
      delay(10); // Wait for the buffer to drain
    }
  }
  Serial.printf("Sent %d / %d bytes.\n", bytesSent, totalBytes);
  
  Serial.print("-> Zabbix: ");
  long timeout = millis() + 3000;
  bool responsePrinted = false;
  while(millis() < timeout) {
    if(client.available()) {
      Serial.write(client.read());
      responsePrinted = true;
    } else if (!client.connected() && responsePrinted) {
      break; // connection closed and we read the data
    } else if (!client.connected() && !responsePrinted && (millis() + 100 > timeout)) {
      // connection closed and no data yet, wait a tiny bit just in case it's in the buffer
      break;
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
      case 0x03: return A; // Fuel System Status
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
      case 0x21: return (A * 256.0) + B; // Distance w/ MIL
      case 0x2C: return (A * 100.0) / 255.0; // Commanded EGR
      case 0x2E: return (A * 100.0) / 255.0; // Evaporative Purge
      case 0x2F: return (A * 100.0) / 255.0; // Fuel Level
      case 0x30: return A; // Warm-ups since DTC clear
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
      case 0x50: return A * 10.0; // Max MAF Rate
      case 0x52: return (A * 100.0) / 255.0; // Ethanol Percent
      case 0x5C: return A - 40; // Oil Temp
      default: return -999.0;
    }
  }
  return -999.0;
}

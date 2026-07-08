#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>

// ==========================================
// SECRETS & CONFIGURATION
// ==========================================

// Wi-Fi Configuration
const char *WIFI_SSID = "YOUR_WIFI_SSID";
const char *WIFI_PASS = "YOUR_WIFI_PASSWORD";

// Zabbix Server Configuration
const char *ZABBIX_SERVER = "0.0.0.0"; // Zabbix IP or DNS
const int ZABBIX_PORT = 10051; // Zabbix port
const char *ZABBIX_HOST = "Volkswagen UP"; // Host for OBD2 vehicle metrics

// Set to false to disable ESP32 self-monitoring metrics (saves Flash/RAM)
const bool ENABLE_SELF_MONITORING = true;

// Self-monitoring Zabbix host
const char *ZABBIX_MONITOR_HOST = "ESP32-UP";

// ELM327 Bluetooth Adapter Configuration
// Replace with the MAC address of your ELM327 adapter
uint8_t ELM327_MAC_BYTES[6] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
const char *ELM327_PIN = "1234";

#endif

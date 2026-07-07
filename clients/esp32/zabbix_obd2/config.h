#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>

// ==========================================
// SECRETS & CONFIGURATION
// ==========================================

// Wi-Fi Configuration
const char *WIFI_SSID = "iPhone";
const char *WIFI_PASS = "10203090";

// Zabbix Server Configuration
const char *ZABBIX_SERVER = "jholiv-zabbix.ddns.net"; // Zabbix IP or DNS
const int ZABBIX_PORT = 10051;
const char *ZABBIX_HOST = "Volkswagen UP"; // Host for OBD2 vehicle metrics
// const char* ZABBIX_HOST = "Hyundai HB20";     // Host for OBD2 vehicle
// metrics
const char *ZABBIX_MONITOR_HOST =
    "ESP32-UP"; // Host for ESP32 self-monitoring metrics
// const char* ZABBIX_MONITOR_HOST = "ESP32-HB20";  // Host for ESP32
// self-monitoring metrics

// Set to false to disable ESP32 self-monitoring metrics (saves Flash/RAM)
const bool ENABLE_SELF_MONITORING = true;

// ELM327 Bluetooth Adapter Configuration
// Replace with the MAC address of your ELM327 adapter
uint8_t ELM327_MAC_BYTES[6] = {0x4C, 0x2C, 0x8A,
                               0x15, 0x27, 0x5E}; // ELM327 Preto
// uint8_t ELM327_MAC_BYTES[6] = {0x01, 0x23, 0x45, 0x67, 0x89, 0xBA}; // ELM327
// Azul
const char *ELM327_PIN = "1234";

#endif

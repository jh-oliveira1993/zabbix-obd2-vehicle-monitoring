#include "BluetoothSerial.h"

#if !defined(CONFIG_BT_ENABLED) || !defined(CONFIG_BLUEDROID_ENABLED)
#error Bluetooth is not enabled! Please run `make menuconfig` to and enable it
#endif

#if !defined(CONFIG_BT_SPP_ENABLED)
#error Serial Bluetooth not available or not enabled. It is only available for the ESP32 chip.
#endif

BluetoothSerial SerialBT;

void setup() {
  Serial.begin(115200);
  Serial.println("Starting Bluetooth Device Scanner...");
  SerialBT.begin("ESP32_Scanner", true); // Master mode
  
  Serial.println("Scanning for 15 seconds...");
  BTScanResults* results = SerialBT.discover(15000);
  
  if (results) {
    int count = results->getCount();
    Serial.printf("Found %d devices\n", count);
    for (int i = 0; i < count; i++) {
      BTAddress addr = results->getDevice(i)->getAddress();
      String name = results->getDevice(i)->getName();
      Serial.printf("Device %d: %s - MAC: %s\n", i+1, name.c_str(), addr.toString().c_str());
    }
  } else {
    Serial.println("Error discovering devices.");
  }
}

void loop() {
  delay(1000);
}

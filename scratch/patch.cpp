void discoverPIDs() {
  Serial.println("Starting Dynamic PID Discovery...");
  bool supported[256] = {false};
  
  for (int base = 0x00; base <= 0x60; base += 0x20) {
    char cmd[5];
    sprintf(cmd, "01%02X", base);
    Serial.print("Querying: "); Serial.println(cmd);
    String resp = sendCommand(cmd);
    Serial.print("Response: "); Serial.println(resp);
    
    char prefix[5];
    sprintf(prefix, "41%02X", base);
    
    if (resp.startsWith(prefix) && resp.length() >= 12) {
      String hexMask = resp.substring(4, 12);
      Serial.print("Bitmask hex: "); Serial.println(hexMask);
      uint32_t bitmask = strtoul(hexMask.c_str(), NULL, 16);
      
      for (int i = 1; i <= 32; i++) {
        if (isPidSupported(bitmask, i)) {
          int pid = base + i;
          if (pid < 256) supported[pid] = true;
          Serial.printf("Supported PID: %02X\n", pid);
        }
      }
      
      if (!isPidSupported(bitmask, 32)) {
        break;
      }
    } else {
      Serial.println("Invalid response, stopping discovery.");
      break; 
    }
  }

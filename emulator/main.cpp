#include "Arduino.h"
#include "EEPROM.h"

// ESP32 define so dacWrite path is taken
#define ESP32

// Enable web server in emulator
#define WEB_ENABLED 1

#include "WebServer.h"

// Forward declare the web command handler (defined after .ino include)
void handleWebCommand(const char* cmd);

// Include the firmware directly
#include "../alkalinity_monitor_v2_1.ino"

void handleWebCommand(const char* cmd) {
  std::string line(cmd);
  // Process same as serial input
  if (line.rfind("CAL ", 0) == 0) {
    float v = std::strtof(line.c_str() + 4, nullptr);
    if (v >= MIN_VALID_DKH && v <= MAX_VALID_DKH) saveCalibration(v);
  }
  else if (line == "STATUS") {
    Serial.print("Anchor: "); Serial.print(cal.referenceAlk_dKH, 2); Serial.println(" dKH");
    Serial.print("Sim mode: "); Serial.println(SIMULATION_MODE ? "YES" : "NO");
  }
  else if (line == "PUMPS_OFF") {
    ensurePumpsOff();
    g_abortCycle = true;
    Serial.println("EMERGENCY STOP — all pumps off.");
  }
  else if (line == "RUN") {
    g_forceRun = true;
    Serial.println("RUN queued.");
  }
}

int main() {
  webServer_begin();
  setup();
  g_webState.anchor_dKH = cal.referenceAlk_dKH;
  while (true) {
    loop();
  }
  return 0;
}

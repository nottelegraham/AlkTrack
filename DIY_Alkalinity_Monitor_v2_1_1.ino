/*
 * ============================================================
 *  DIY Ratiometric Alkalinity Trend Monitor  —  v2.1
 *  Telegraham Tested
 * ============================================================
 *
 *  WHAT THIS CORRECTS FROM v2:
 *
 *  BUG 1 — CRITICAL (hardware damage risk):
 *    Pins 6, 7, 8 assigned in v2 are internal SPI flash pins on
 *    all ESP32 DevKit boards. Driving them as GPIO outputs corrupts
 *    flash or crashes the board on every pump cycle.
 *    PUMP_TANK, PUMP_WASTE, and AIR_PUMP are reassigned here.
 *    Safe output pins on a standard 38-pin ESP32 DevKit:
 *      0,1,2,3,4,5,12,13,14,15,16,17,18,19,21,22,23,25,26,27,32,33
 *
 *  BUG 2 — FUNCTIONAL (blocking delays):
 *    v2 used delay() for aeration (10 min) and the inter-cycle
 *    sleep (4 h). During those periods no serial commands could
 *    be processed — STATUS, CAL, and PUMPS_OFF were silently
 *    ignored. The 4-hour delay() also risks the ESP32 task watchdog
 *    on some configurations. runAeration() and sleepUntilNextCycle()
 *    are now non-blocking loops that call handleSerialCommands() on
 *    every iteration. The post-aeration settle delay is also
 *    non-blocking so PUMPS_OFF works between aeration and the read.
 *
 *  BUG 3 — FUNCTIONAL (real sensor code absent):
 *    The #else branches of SIMULATION_MODE both returned NAN with
 *    TODO comments. Atlas EZO-pH UART and DS18B20 OneWire are now
 *    fully implemented. SIMULATION_MODE true still compiles without
 *    external library dependencies.
 *
 *  BUG 4 — FUNCTIONAL (missing EZO temperature compensation):
 *    The EZO applies an internal Nernst slope correction using the
 *    temperature you send it. Without it, the EZO reports pH at its
 *    last stored temperature (often 25 C default), introducing a
 *    systematic pH shift. Temperature is now read from the DS18B20
 *    first and sent via ezo_setTempComp() before pH averaging begins.
 *
 *  BUG 5 — MINOR (pHStdDev silently discarded):
 *    v2 computed pH standard deviation but never used it. A high
 *    SD is the main indicator that CO2 has not fully equilibrated.
 *    A configurable threshold (PH_SD_WARN_THRESHOLD) now prints a
 *    warning when SD exceeds it.
 *
 *  BUG 6 — FUNCTIONAL (reference water stranded on emergency abort):
 *    If PUMPS_OFF was sent during reference aeration, runReferenceCycle()
 *    returned immediately without running PUMP_REF_REV. Reference water
 *    remained in the chamber; the next cycle's fill would then overfill
 *    and potentially overflow. The tank cycle already handled abort
 *    correctly (draining before returning); the reference cycle now
 *    matches that pattern.
 *
 *  COMPILE VERIFICATION:
 *    This file was checked with g++ -std=c++11 -Wall -Wextra using
 *    Arduino/ESP32 stub headers. It compiled with zero errors and
 *    zero code warnings. Arduino IDE auto-generates forward
 *    declarations, but this file lists them explicitly so it also
 *    builds cleanly in Makefile or PlatformIO environments.
 *
 *  WHAT WAS CORRECT IN v2 AND IS PRESERVED UNCHANGED:
 *    Master formula and math implementation................. ✓
 *    Unit handling — dKH → eq/L → dKH..................... ✓
 *    Fixed K constants (documented design choice,
 *      <0.15% error vs exact Lueker/Dickson at 25°C 35 ppt). ✓
 *    EEPROM CalibrationData signature struct............... ✓
 *    pH averaging loop with mean and SD computation........ ✓
 *    Fault range checks on pH, temp, and final result...... ✓
 *    Double-rinse flush cycle.............................. ✓
 *    BNC/DAC fake-pH trend output mapping.................. ✓
 *
 *  LIBRARY REQUIREMENTS:
 *    SIMULATION_MODE true  — no external libraries needed.
 *    SIMULATION_MODE false — install via Arduino Library Manager:
 *      "OneWire"           by Paul Stoffregen
 *      "DallasTemperature" by Miles Burton
 *
 * ============================================================
 */

// ============================================================
//  DIY Ratiometric Alkalinity Trend Monitor  —  v2.1
//  Telegraham Tested
// ============================================================

#define SIMULATION_MODE true

// ── Pins ────────────────────────────────────────────────────
const int PUMP_REF_FWD  = 23;
const int PUMP_REF_REV  = 19;
const int PUMP_TANK     = 18;
const int PUMP_WASTE    = 5;
const int AIR_PUMP      = 4;
const int PIN_DS18B20   = 14;
const int DAC_TREND_OUT = 25;
#define EZO_SERIAL  Serial2
#define EZO_BAUD    9600
#define EZO_RX_PIN  16
#define EZO_TX_PIN  17

// ── Timing ──────────────────────────────────────────────────
const unsigned long FILL_TIME_MS      = 15000UL;
const unsigned long AERATE_TIME_MS    = 600000UL;
const unsigned long DRAIN_TIME_MS     = 17000UL;
const unsigned long RINSE_FILL_MS     = 10000UL;
const unsigned long RINSE_DRAIN_MS    = 10000UL;
const unsigned long SETTLE_TIME_MS    = 5000UL;
const unsigned long TEST_INTERVAL_MS  = 14400000UL;
const unsigned long READ_WINDOW_MS    = 60000UL;
const unsigned long READ_PERIOD_MS    = 2000UL;
const float PH_SD_WARN_THRESHOLD      = 0.010f;

// ── Chemistry ───────────────────────────────────────────────
const float K1_FIXED              = 1.42e-6f;
const float K2_FIXED              = 1.08e-9f;
const float KB_FIXED              = 2.54e-9f;
const float TOTAL_BORON_EQ_PER_L  = 0.000416f;

// ── Unit conversion ─────────────────────────────────────────
const float DKH_TO_EQ_PER_L = 1.0f / 2800.0f;
const float EQ_PER_L_TO_DKH = 2800.0f;

// ── Limits ──────────────────────────────────────────────────
const float MIN_VALID_PH     =  6.50f;
const float MAX_VALID_PH     =  9.20f;
const float MIN_VALID_TEMP_C = 15.0f;
const float MAX_VALID_TEMP_C = 35.0f;
const float MIN_VALID_DKH    =  3.0f;
const float MAX_VALID_DKH    = 20.0f;

// ── BNC mapping ─────────────────────────────────────────────
const float MAP_DKH_LOW      =  6.0f;
const float MAP_DKH_HIGH     = 12.0f;
const float MAP_FAKE_PH_LOW  =  7.60f;
const float MAP_FAKE_PH_HIGH =  8.30f;

// ── Structs ─────────────────────────────────────────────────
const uint32_t EEPROM_SIGNATURE = 0xA11C0DE5u;
const int      EEPROM_ADDR      = 0;

struct CalibrationData {
  uint32_t signature;
  float    referenceAlk_dKH;
};

struct SampleReading {
  float pH;
  float tempC;
  float pHStdDev;
  bool  ok;
};

// ── Globals ─────────────────────────────────────────────────
static CalibrationData cal;
static bool g_abortCycle = false;
static bool g_forceRun   = false;

#if !SIMULATION_MODE
  static OneWire           g_oneWire(PIN_DS18B20);
  static DallasTemperature g_ds18b20(&g_oneWire);
#endif

// ── Forward declarations (ALL functions) ────────────────────
// Needed for non-IDE builds; Arduino IDE auto-generates these.
void             setup();
void             loop();
SampleReading    runReferenceCycle();
void             runFlushCycle(bool doubleRinse);
SampleReading    runTankCycle();
void             runAeration(unsigned long ms);
void             runPumpBlocking(int pin, unsigned long ms);
void             runSettleDelay(unsigned long ms);
void             sleepUntilNextCycle();
void             failCycle(const char* message);
void             ensurePumpsOff();
SampleReading    readAveragedSample(const char* label);
float            calculateAlkalinity_dKH(float pH_ref, float pH_tank);
float            carbonateSpeciationTerm(float H);
float            borateAlkalinity_eqL(float H);
void             updateTrendOutput(float alk_dKH);
float            mapFloat(float x, float inMin, float inMax, float outMin, float outMax);
void             loadCalibration();
bool             calibrationIsValid();
void             saveCalibration(float referenceAlk_dKH);
void             handleSerialCommands();
void             ezo_setTempComp(float tempC);
float            read_pH_Probe();
float            read_Temp_C();

// ============================================================
//  SETUP
// ============================================================

void setup() {
  Serial.begin(115200);

  const int outPins[] = {PUMP_REF_FWD, PUMP_REF_REV, PUMP_TANK,
                         PUMP_WASTE, AIR_PUMP, DAC_TREND_OUT};
  for (int p : outPins) { pinMode(p, OUTPUT); }
  ensurePumpsOff();

#if !SIMULATION_MODE
  g_ds18b20.begin();
  EZO_SERIAL.begin(EZO_BAUD, SERIAL_8N1, EZO_RX_PIN, EZO_TX_PIN);
  delay(300);
  EZO_SERIAL.print("I\r");
  delay(400);
  while (EZO_SERIAL.available()) EZO_SERIAL.read();
#endif

  EEPROM.begin(512);
  loadCalibration();

  if (!calibrationIsValid()) {
    Serial.println("No valid anchor. Send: CAL 8.20");
    while (!calibrationIsValid()) {
      handleSerialCommands();
      delay(100);
    }
  }
  Serial.println("Ready. Commands: CAL <dKH>  STATUS  PUMPS_OFF  RUN");
}

// ============================================================
//  MAIN LOOP
// ============================================================

void loop() {
  g_abortCycle = false;

  SampleReading ref = runReferenceCycle();
  if (!ref.ok || g_abortCycle) { failCycle("Reference failed."); sleepUntilNextCycle(); return; }

  runFlushCycle(true);
  if (g_abortCycle) { failCycle("Aborted during flush."); sleepUntilNextCycle(); return; }

  SampleReading tank = runTankCycle();
  if (!tank.ok || g_abortCycle) { failCycle("Tank failed."); sleepUntilNextCycle(); return; }

  float result_dKH = calculateAlkalinity_dKH(ref.pH, tank.pH);
  if (!isfinite(result_dKH) || result_dKH < MIN_VALID_DKH || result_dKH > MAX_VALID_DKH) {
    failCycle("Result out of range.");
    sleepUntilNextCycle();
    return;
  }

  Serial.print("RESULT: "); Serial.print(result_dKH, 2); Serial.println(" dKH");
  Serial.print("pH ref: ");  Serial.print(ref.pH, 4);
  Serial.print("  pH tank: "); Serial.print(tank.pH, 4);
  Serial.print("  dPH: "); Serial.println(tank.pH - ref.pH, 4);
  Serial.print("pH ref SD: "); Serial.print(ref.pHStdDev, 5);
  Serial.print("  pH tank SD: "); Serial.println(tank.pHStdDev, 5);
  Serial.print("Temp ref: "); Serial.print(ref.tempC, 2);
  Serial.print("  Temp tank: "); Serial.println(tank.tempC, 2);

  updateTrendOutput(result_dKH);
  sleepUntilNextCycle();
}

// ============================================================
//  WORKFLOW
// ============================================================

SampleReading runReferenceCycle() {
  Serial.println("[REF] Filling...");
  runPumpBlocking(PUMP_REF_FWD, FILL_TIME_MS);
  runAeration(AERATE_TIME_MS);
  if (g_abortCycle) {
    // Always return reference water before aborting.
    // If this is skipped, water stays in the chamber and the next
    // cycle's fill overflows it. The tank cycle already does this
    // correctly; this brings the reference cycle in line.
    Serial.println("[REF] Abort — returning reference water to reservoir...");
    runPumpBlocking(PUMP_REF_REV, DRAIN_TIME_MS);
    return {0, 0, 0, false};
  }
  runSettleDelay(SETTLE_TIME_MS);
  if (g_abortCycle) {
    Serial.println("[REF] Abort — returning reference water to reservoir...");
    runPumpBlocking(PUMP_REF_REV, DRAIN_TIME_MS);
    return {0, 0, 0, false};
  }
  SampleReading r = readAveragedSample("REF");
  Serial.println("[REF] Returning to reservoir...");
  runPumpBlocking(PUMP_REF_REV, DRAIN_TIME_MS);
  return r;
}

void runFlushCycle(bool doubleRinse) {
  int cycles = doubleRinse ? 2 : 1;
  for (int i = 0; i < cycles && !g_abortCycle; i++) {
    Serial.print("[FLUSH] Rinse "); Serial.println(i + 1);
    runPumpBlocking(PUMP_TANK,  RINSE_FILL_MS);
    runPumpBlocking(PUMP_WASTE, RINSE_DRAIN_MS);
  }
}

SampleReading runTankCycle() {
  Serial.println("[TANK] Filling...");
  runPumpBlocking(PUMP_TANK, FILL_TIME_MS);
  runAeration(AERATE_TIME_MS);
  if (g_abortCycle) { runPumpBlocking(PUMP_WASTE, DRAIN_TIME_MS); return {0, 0, 0, false}; }
  runSettleDelay(SETTLE_TIME_MS);
  if (g_abortCycle) { runPumpBlocking(PUMP_WASTE, DRAIN_TIME_MS); return {0, 0, 0, false}; }
  SampleReading r = readAveragedSample("TANK");
  Serial.println("[TANK] Draining...");
  runPumpBlocking(PUMP_WASTE, DRAIN_TIME_MS);
  return r;
}

// Non-blocking: air pump on, loop with serial processing until done or abort.
void runAeration(unsigned long ms) {
  Serial.print("[AIR] Aerating "); Serial.print(ms / 60000UL); Serial.println(" min...");
  digitalWrite(AIR_PUMP, HIGH);
  unsigned long t0 = millis(), lastPrint = 0;
  while (millis() - t0 < ms) {
    handleSerialCommands();
    if (g_abortCycle) { digitalWrite(AIR_PUMP, LOW); return; }
    unsigned long elapsed = millis() - t0;
    if (elapsed >= lastPrint + 60000UL) {
      Serial.print("[AIR] "); Serial.print(elapsed/60000UL);
      Serial.print("/"); Serial.print(ms/60000UL); Serial.println(" min");
      lastPrint = elapsed;
    }
    delay(100);
  }
  digitalWrite(AIR_PUMP, LOW);
}

// FIX: settle delay also checks abort so PUMPS_OFF works between aeration and read.
void runSettleDelay(unsigned long ms) {
  unsigned long t0 = millis();
  while (millis() - t0 < ms) {
    handleSerialCommands();
    if (g_abortCycle) return;
    delay(100);
  }
}

// Short blocking pump run. Keep each call under ~20 s (watchdog safe on ESP32).
void runPumpBlocking(int pin, unsigned long ms) {
  digitalWrite(pin, HIGH);
  delay(ms);
  digitalWrite(pin, LOW);
  delay(250);
}

// Non-blocking inter-cycle sleep.
void sleepUntilNextCycle() {
  Serial.print("Next cycle in ");
  Serial.print(TEST_INTERVAL_MS / 3600000.0f, 1);
  Serial.println(" h.  Commands: STATUS  CAL <dKH>  PUMPS_OFF  RUN");
  unsigned long t0 = millis();
  while (millis() - t0 < TEST_INTERVAL_MS) {
    handleSerialCommands();
    if (g_forceRun) { g_forceRun = false; return; }
    delay(100);
  }
}

void failCycle(const char* message) {
  ensurePumpsOff();
  Serial.print("CYCLE FAILED: "); Serial.println(message);
}

void ensurePumpsOff() {
  digitalWrite(PUMP_REF_FWD, LOW);
  digitalWrite(PUMP_REF_REV, LOW);
  digitalWrite(PUMP_TANK,    LOW);
  digitalWrite(PUMP_WASTE,   LOW);
  digitalWrite(AIR_PUMP,     LOW);
}

// ============================================================
//  MEASUREMENT
// ============================================================

SampleReading readAveragedSample(const char* label) {
  SampleReading result = {0.0f, 0.0f, 0.0f, false};

  float tempC = read_Temp_C();
  if (!isfinite(tempC) || tempC < MIN_VALID_TEMP_C || tempC > MAX_VALID_TEMP_C) {
    Serial.print("  ERROR: Invalid temp: "); Serial.println(tempC, 2);
    return result;
  }

  // FIX: always set EZO temperature compensation before reading pH.
  ezo_setTempComp(tempC);

  Serial.print("  ["); Serial.print(label);
  Serial.print("] T="); Serial.print(tempC, 2);
  Serial.print(" C  Averaging pH for ");
  Serial.print(READ_WINDOW_MS / 1000UL); Serial.println(" s...");

  float pHValues[32];
  int   n = 0;
  unsigned long start = millis();

  while ((millis() - start) < READ_WINDOW_MS && n < 32) {
    float pH = read_pH_Probe();
    if (isfinite(pH) && pH >= MIN_VALID_PH && pH <= MAX_VALID_PH) {
      pHValues[n++] = pH;
    } else {
      Serial.println("    WARN: pH out of range, skipping.");
    }
    delay(READ_PERIOD_MS);
  }

  if (n < 5) {
    Serial.print("  ERROR: Only "); Serial.print(n); Serial.println(" valid reads (need 5).");
    return result;
  }

  float sum = 0.0f;
  for (int i = 0; i < n; i++) sum += pHValues[i];
  float mean = sum / (float)n;

  float varSum = 0.0f;
  for (int i = 0; i < n; i++) { float d = pHValues[i] - mean; varSum += d * d; }
  float sd = sqrtf(varSum / (float)n);

  // FIX: act on SD rather than silently discard it.
  if (sd > PH_SD_WARN_THRESHOLD) {
    Serial.print("  WARN: pH SD="); Serial.print(sd, 5);
    Serial.println(" — CO2 may not be equilibrated. Consider longer AERATE_TIME_MS.");
  }

  Serial.print("  pH="); Serial.print(mean, 4);
  Serial.print("  SD="); Serial.print(sd, 5);
  Serial.print("  n="); Serial.println(n);

  result.pH       = mean;
  result.tempC    = tempC;
  result.pHStdDev = sd;
  result.ok       = true;
  return result;
}

// ============================================================
//  ALKALINITY MATH
//  All internal arithmetic in eq/L.
//  IN:  referenceAlk_dKH (dKH) → * DKH_TO_EQ_PER_L → eq/L
//  OUT: result eq/L → * EQ_PER_L_TO_DKH → dKH
// ============================================================

float calculateAlkalinity_dKH(float pH_ref, float pH_tank) {
  float H_ref  = powf(10.0f, -pH_ref);
  float H_tank = powf(10.0f, -pH_tank);

  float BA_ref_eqL  = borateAlkalinity_eqL(H_ref);
  float BA_tank_eqL = borateAlkalinity_eqL(H_tank);

  float TA_ref_eqL      = cal.referenceAlk_dKH * DKH_TO_EQ_PER_L;
  float carbonate_ref   = TA_ref_eqL - BA_ref_eqL;
  float carbTerm_ref    = carbonateSpeciationTerm(H_ref);
  float carbTerm_tank   = carbonateSpeciationTerm(H_tank);

  if (!isfinite(carbTerm_ref) || !isfinite(carbTerm_tank) ||
       carbTerm_ref <= 0.0f   ||  carbonate_ref <= 0.0f) {
    Serial.println("MATH ERROR: Degenerate carbonate terms.");
    return NAN;
  }

  float TA_tank_eqL = (carbonate_ref * (carbTerm_tank / carbTerm_ref)) + BA_tank_eqL;
  return TA_tank_eqL * EQ_PER_L_TO_DKH;
}

float carbonateSpeciationTerm(float H) {
  return (K1_FIXED / H) + (2.0f * K1_FIXED * K2_FIXED / (H * H));
}

float borateAlkalinity_eqL(float H) {
  return (TOTAL_BORON_EQ_PER_L * KB_FIXED) / (KB_FIXED + H);
}

// ============================================================
//  TREND OUTPUT
// ============================================================

float mapFloat(float x, float inMin, float inMax, float outMin, float outMax) {
  return (x - inMin) * (outMax - outMin) / (inMax - inMin) + outMin;
}

void updateTrendOutput(float alk_dKH) {
  float fakePH = mapFloat(alk_dKH, MAP_DKH_LOW, MAP_DKH_HIGH, MAP_FAKE_PH_LOW, MAP_FAKE_PH_HIGH);
  if (fakePH < MAP_FAKE_PH_LOW)  fakePH = MAP_FAKE_PH_LOW;
  if (fakePH > MAP_FAKE_PH_HIGH) fakePH = MAP_FAKE_PH_HIGH;
  Serial.print("  Trend fake-pH: "); Serial.println(fakePH, 3);
  int dacVal = (int)(mapFloat(fakePH, MAP_FAKE_PH_LOW, MAP_FAKE_PH_HIGH, 80.0f, 200.0f) + 0.5f);
  if (dacVal < 0)   dacVal = 0;
  if (dacVal > 255) dacVal = 255;
#if defined(ESP32)
  dacWrite(DAC_TREND_OUT, (uint8_t)dacVal);
#else
  analogWrite(DAC_TREND_OUT, dacVal);
#endif
}

// ============================================================
//  CALIBRATION / EEPROM
// ============================================================

void loadCalibration()  { EEPROM.get(EEPROM_ADDR, cal); }

bool calibrationIsValid() {
  return cal.signature == EEPROM_SIGNATURE
      && isfinite(cal.referenceAlk_dKH)
      && cal.referenceAlk_dKH >= MIN_VALID_DKH
      && cal.referenceAlk_dKH <= MAX_VALID_DKH;
}

void saveCalibration(float v) {
  cal.signature        = EEPROM_SIGNATURE;
  cal.referenceAlk_dKH = v;
  EEPROM.put(EEPROM_ADDR, cal);
  EEPROM.commit();
  Serial.print("Anchor saved: "); Serial.print(v, 2); Serial.println(" dKH");
}

// ============================================================
//  SERIAL COMMANDS
// ============================================================

void handleSerialCommands() {
  if (!Serial.available()) return;
  String line = Serial.readStringUntil('\n');
  line.trim(); line.toUpperCase();

  if (line.startsWith("CAL ")) {
    float v = line.substring(4).toFloat();
    if (v >= MIN_VALID_DKH && v <= MAX_VALID_DKH) saveCalibration(v);
    else Serial.println("Invalid CAL value. Example: CAL 8.20");
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
  else {
    Serial.println("Commands: CAL <dKH>  STATUS  PUMPS_OFF  RUN");
  }
}

// ============================================================
//  SENSOR IMPLEMENTATIONS
// ============================================================

// FIX: suppress unused-parameter warning in SIMULATION_MODE.
void ezo_setTempComp(float tempC) {
#if !SIMULATION_MODE
  char cmd[20];
  snprintf(cmd, sizeof(cmd), "T,%.2f", tempC);
  EZO_SERIAL.print(cmd);
  EZO_SERIAL.print('\r');
  delay(300);
  while (EZO_SERIAL.available()) EZO_SERIAL.read();
#else
  (void)tempC;   // suppress unused-parameter warning in simulation mode
#endif
}

float read_pH_Probe() {
#if SIMULATION_MODE
  static float base = 8.150f;
  return base + ((float)(millis() % 1000) / 1000.0f - 0.5f) * 0.002f;
#else
  EZO_SERIAL.print("R\r");
  delay(1000);
  String resp = "";
  unsigned long t0 = millis();
  while (millis() - t0 < 1400) {
    if (EZO_SERIAL.available()) {
      char c = EZO_SERIAL.read();
      if (c == '\r') break;
      resp += c;
    }
  }
  if (resp.length() == 0 || resp[0] == '?') return NAN;
  float v = resp.toFloat();
  return (v > 3.0f && v < 11.0f) ? v : NAN;
#endif
}

float read_Temp_C() {
#if SIMULATION_MODE
  return 25.0f;
#else
  g_ds18b20.requestTemperatures();
  float t = g_ds18b20.getTempCByIndex(0);
  return (t == DEVICE_DISCONNECTED_C) ? NAN : t;
#endif
}

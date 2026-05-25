/*
 * ============================================================
 *  DIY Ratiometric Alkalinity Trend Monitor  —  v3.0
 *  TelegrahamTested
 * ============================================================
 *
 *  WHAT IS NEW IN v3.0:
 *
 *  MAJOR 1 — STEPPER MOTOR ARCHITECTURE:
 *    All four peristaltic pumps are now driven by stepper motor
 *    drivers (A4988 / DRV8825). Each pump uses a STEP pin and
 *    a DIR pin. Volume is controlled by step count, replacing
 *    timing-based control from all prior versions. This provides
 *    the precision required for acid titration and improves
 *    repeatability in pH differential mode.
 *    Relay module is no longer used for pump control.
 *    Air pump remains on a relay or MOSFET (on/off only).
 *
 *  MAJOR 2 — DUAL OPERATING MODES (user-selectable):
 *    MODE PHDI — pH Differential (v2.x ratiometric method, unchanged)
 *    MODE TITR — Acid Titration  (new in v3.0)
 *    Active mode persists in EEPROM across power cycles.
 *
 *    Titration sequence:
 *      1. Fill chamber with precise sample volume (Pump B, step-counted)
 *      2. Pre-aerate 10 min to drive off dissolved CO2
 *      3. Record starting pH with EZO temperature compensation
 *      4. Dose 0.1N HCl in small increments (Pump D, slow/precise)
 *      5. Mix with 1.5-second air pulse after each dose
 *      6. Wait for pH to stabilise (< 0.003 pH change between reads)
 *      7. Detect endpoint: fixed pH 4.5, refined by Gran function
 *      8. TA (dKH) = (N_acid x Ve_mL) / V_sample_mL x 2800
 *      9. Drain acidic waste to waste container (Pump C)
 *     10. Log result, update DAC trend output
 *
 *  MAJOR 3 — EXPANDED EEPROM CALIBRATION:
 *    New EEPROM signature (0xA11C0003) forces re-initialisation
 *    on upgrade. New fields: acid normality, steps/mL for sample
 *    and acid pumps, sample volume, dose increment, mode.
 *
 *  CARRIED FORWARD FROM v2.2 UNCHANGED:
 *    pH differential ratiometric formula
 *    Fixed K constants (Lueker 2000 / Dickson 1990)
 *    EZO-pH UART with temperature compensation
 *    DS18B20 OneWire temperature sensor
 *    Non-blocking aeration and inter-cycle sleep loops
 *    pH averaging with SD warning
 *    DAC trend output on GPIO 25
 *
 *  LIBRARY REQUIREMENTS:
 *    SIMULATION_MODE true  — no external libraries needed
 *    SIMULATION_MODE false — install via Arduino Library Manager:
 *      "OneWire"           by Paul Stoffregen
 *      "DallasTemperature" by Miles Burton
 *
 *  STEPPER DRIVER WIRING:
 *    Each A4988 / DRV8825: connect STEP and DIR to ESP32 GPIO.
 *    Connect ENABLE to GND (always enabled) or to a GPIO if
 *    coil de-energisation between cycles is desired.
 *    MS1/MS2/MS3 jumpers set microstepping resolution.
 *    Recommended: 1/16 step for water pumps, 1/32 for acid pump.
 *    VMOT: 12V supply. GND: shared with ESP32 GND (with care).
 *    Logic VCC: 3.3V or 5V per driver board spec.
 *
 * ============================================================
 */
// SPDX-License-Identifier: CC-BY-NC-SA-4.0
// TelegrahamTested, 2025.
// https://creativecommons.org/licenses/by-nc-sa/4.0/

// ============================================================
//  DIY Ratiometric Alkalinity Trend Monitor  —  v3.0
//  TelegrahamTested
// ============================================================

#define SIMULATION_MODE true

// ── Operating mode ───────────────────────────────────────────
enum OperatingMode : uint8_t { MODE_PH_DIFF = 0, MODE_TITRATION = 1 };

// ── Stepper pump pins ────────────────────────────────────────
// NEVER use GPIO 6-11 (internal SPI flash).
// Stepper driver ENABLE pins: tie to GND (always-on) or manage externally.
const int PUMP_A_STEP = 23;   // Pump A — reference reservoir (reversible)
const int PUMP_A_DIR  = 22;
const int PUMP_B_STEP = 21;   // Pump B — tank/sump water (reversible)
const int PUMP_B_DIR  = 19;
const int PUMP_C_STEP = 18;   // Pump C — waste drain (forward only)
const int PUMP_C_DIR  = 5;
const int PUMP_D_STEP = 27;   // Pump D — acid injection (forward only, slow)
const int PUMP_D_DIR  = 26;
const int AIR_PUMP      = 4;  // Air pump — relay or MOSFET, on/off
const int PIN_DS18B20   = 14;
const int DAC_TREND_OUT = 25;
#define EZO_SERIAL  Serial2
#define EZO_BAUD    9600
#define EZO_RX_PIN  16
#define EZO_TX_PIN  17

// ── Stepper speed ────────────────────────────────────────────
const float STEP_SPEED_WATER = 800.0f;   // steps/sec — Pumps A, B, C
const float STEP_SPEED_ACID  = 200.0f;   // steps/sec — Pump D (slow = precise)

// ── Timing ───────────────────────────────────────────────────
const unsigned long AERATE_TIME_MS   = 600000UL;  // 10 min CO2 equilibration
const unsigned long SETTLE_TIME_MS   = 5000UL;
const unsigned long TEST_INTERVAL_MS = 14400000UL; // 4-hour auto cycle
const unsigned long READ_WINDOW_MS   = 60000UL;
const unsigned long READ_PERIOD_MS   = 2000UL;
const float         RINSE_FILL_ML    = 20.0f;     // mL per flush rinse pass
const float         PH_SD_WARN_THRESHOLD = 0.010f;

// ── Titration constants ──────────────────────────────────────
const float          ENDPOINT_PH    = 4.50f;  // fixed-endpoint pH (fallback)
const float          ABORT_PH       = 3.50f;  // abort if pH drops below this
const float          MAX_ACID_ML    = 5.00f;  // safety cutoff — max acid before abort
const unsigned long  MIX_PULSE_MS   = 1500UL; // air-pump mixing burst after each dose
const unsigned long  STABILIZE_MS   = 8000UL; // max wait for pH to stabilise
const float          PH_STABLE_DELTA = 0.003f;// pH "stable" threshold

// ── Chemistry (pH differential mode) ────────────────────────
const float K1_FIXED             = 1.42e-6f;
const float K2_FIXED             = 1.08e-9f;
const float KB_FIXED             = 2.54e-9f;
const float TOTAL_BORON_EQ_PER_L = 0.000416f;
const float DKH_TO_EQ_PER_L     = 1.0f / 2800.0f;
const float EQ_PER_L_TO_DKH     = 2800.0f;

// ── Validity limits ──────────────────────────────────────────
const float MIN_VALID_PH     =  6.50f;
const float MAX_VALID_PH     =  9.20f;
const float MIN_VALID_TEMP_C = 15.0f;
const float MAX_VALID_TEMP_C = 35.0f;
const float MIN_VALID_DKH    =  3.0f;
const float MAX_VALID_DKH    = 20.0f;

// ── BNC/DAC trend output mapping ────────────────────────────
const float MAP_DKH_LOW      =  6.0f;
const float MAP_DKH_HIGH     = 12.0f;
const float MAP_FAKE_PH_LOW  =  7.60f;
const float MAP_FAKE_PH_HIGH =  8.30f;

// ── EEPROM ───────────────────────────────────────────────────
const uint32_t EEPROM_SIGNATURE = 0xA11C0003u; // v3 — forces re-init from v2.x
const int      EEPROM_ADDR      = 0;

struct CalibrationData {
  uint32_t     signature;
  // pH differential
  float        referenceAlk_dKH;
  // Titration calibration
  float        acidNormality;     // mol/L (= N for monoprotic HCl)
  float        sampleStepsPerML;  // Pump B calibrated steps per mL
  float        acidStepsPerML;    // Pump D calibrated steps per mL
  float        sampleML;          // target sample volume in mL
  float        doseMl;            // acid increment per dose in mL
  // Mode
  OperatingMode mode;
  uint8_t      _pad[3];           // struct alignment
};

// ── Sensor reading ───────────────────────────────────────────
struct SampleReading { float pH; float tempC; float pHStdDev; bool ok; };

// ── Gran function storage ────────────────────────────────────
struct GranPoint { float acidML; float pH; };
static GranPoint granData[200];
static int       granCount = 0;

// ── Globals ──────────────────────────────────────────────────
static CalibrationData cal;
static bool g_abortCycle = false;
static bool g_forceRun   = false;

#if !SIMULATION_MODE
  static OneWire           g_oneWire(PIN_DS18B20);
  static DallasTemperature g_ds18b20(&g_oneWire);
#endif

// ── Forward declarations ─────────────────────────────────────
void          setup();
void          loop();
void          stepMotor(int stepPin, int dirPin, long steps, bool forward, float stepsPerSec);
void          stepML(int stepPin, int dirPin, float mL, bool forward, float stepsPerML, float stepsPerSec);
SampleReading runReferenceCycle();
void          runFlushCycle(bool doubleRinse);
SampleReading runTankCycle();
float         runTitrationCycle();
float         waitForPHStability();
float         fitGranEquivalence();
void          drainWaste();
void          runAeration(unsigned long ms);
void          runSettleDelay(unsigned long ms);
void          sleepUntilNextCycle();
void          failCycle(const char* message);
void          ensurePumpsOff();
SampleReading readAveragedSample(const char* label);
float         calculateAlkalinity_dKH(float pH_ref, float pH_tank);
float         carbonateSpeciationTerm(float H);
float         borateAlkalinity_eqL(float H);
void          updateTrendOutput(float alk_dKH);
float         mapFloat(float x, float inMin, float inMax, float outMin, float outMax);
void          loadCalibration();
bool          calibrationIsValid();
void          initDefaultCalibration();
void          saveCalibration();
void          handleSerialCommands();
void          ezo_setTempComp(float tempC);
float         read_pH_Probe();
float         read_Temp_C();

// ============================================================
//  SETUP
// ============================================================

void setup() {
  Serial.begin(115200);

  const int outPins[] = {
    PUMP_A_STEP, PUMP_A_DIR,
    PUMP_B_STEP, PUMP_B_DIR,
    PUMP_C_STEP, PUMP_C_DIR,
    PUMP_D_STEP, PUMP_D_DIR,
    AIR_PUMP, DAC_TREND_OUT
  };
  for (int p : outPins) { pinMode(p, OUTPUT); digitalWrite(p, LOW); }

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
    initDefaultCalibration();
    Serial.println("First boot or v2.x upgrade detected — defaults loaded.");
    Serial.println("pH diff mode: send CAL <dKH> to set anchor.");
    Serial.println("Titration mode: see STATUS for calibration values to verify.");
  }

  Serial.print("Mode: ");
  Serial.println(cal.mode == MODE_PH_DIFF ? "pH Differential" : "Acid Titration");
  Serial.println("Commands: STATUS  MODE PHDI  MODE TITR  CAL  PRIME  RUN  PUMPS_OFF");
}

// ============================================================
//  MAIN LOOP
// ============================================================

void loop() {
  g_abortCycle = false;

  if (cal.mode == MODE_PH_DIFF) {

    SampleReading ref = runReferenceCycle();
    if (!ref.ok || g_abortCycle) { failCycle("Reference cycle failed."); sleepUntilNextCycle(); return; }

    runFlushCycle(true);
    if (g_abortCycle) { failCycle("Aborted during flush."); sleepUntilNextCycle(); return; }

    SampleReading tank = runTankCycle();
    if (!tank.ok || g_abortCycle) { failCycle("Tank cycle failed."); sleepUntilNextCycle(); return; }

    float result_dKH = calculateAlkalinity_dKH(ref.pH, tank.pH);
    if (!isfinite(result_dKH) || result_dKH < MIN_VALID_DKH || result_dKH > MAX_VALID_DKH) {
      failCycle("Result out of range."); sleepUntilNextCycle(); return;
    }

    Serial.print("RESULT: "); Serial.print(result_dKH, 2); Serial.println(" dKH  [pH-diff]");
    Serial.print("pH ref: "); Serial.print(ref.pH, 4);
    Serial.print("  pH tank: "); Serial.print(tank.pH, 4);
    Serial.print("  dPH: "); Serial.println(tank.pH - ref.pH, 4);
    Serial.print("Temp ref: "); Serial.print(ref.tempC, 2);
    Serial.print("  Temp tank: "); Serial.println(tank.tempC, 2);
    updateTrendOutput(result_dKH);

  } else {

    float result_dKH = runTitrationCycle();
    if (isfinite(result_dKH)) updateTrendOutput(result_dKH);

  }

  sleepUntilNextCycle();
}

// ============================================================
//  STEPPER MOTOR CONTROL
// ============================================================

void stepMotor(int stepPin, int dirPin, long steps, bool forward, float stepsPerSec) {
  if (steps <= 0) return;
#if SIMULATION_MODE
  unsigned long simMs = (unsigned long)(steps * 1000.0f / stepsPerSec);
  if (simMs > 15000) simMs = 15000;
  unsigned long t0 = millis();
  while (millis() - t0 < simMs) {
    handleSerialCommands();
    if (g_abortCycle) return;
    delay(50);
  }
#else
  digitalWrite(dirPin, forward ? HIGH : LOW);
  delayMicroseconds(5);
  unsigned long halfUs = (unsigned long)(500000.0f / stepsPerSec);
  if (halfUs < 100) halfUs = 100;
  for (long i = 0; i < steps; i++) {
    if (g_abortCycle) break;
    if (i % 100 == 0) handleSerialCommands();
    digitalWrite(stepPin, HIGH);
    delayMicroseconds(halfUs);
    digitalWrite(stepPin, LOW);
    delayMicroseconds(halfUs);
  }
#endif
}

void stepML(int stepPin, int dirPin, float mL, bool forward, float stepsPerML, float stepsPerSec = STEP_SPEED_WATER) {
  long steps = (long)(mL * stepsPerML + 0.5f);
  stepMotor(stepPin, dirPin, steps, forward, stepsPerSec);
}

// ============================================================
//  pH DIFFERENTIAL WORKFLOW  (MODE_PH_DIFF)
// ============================================================

SampleReading runReferenceCycle() {
  Serial.println("[REF] Filling from reservoir...");
  stepML(PUMP_A_STEP, PUMP_A_DIR, cal.sampleML, true, cal.sampleStepsPerML);
  runAeration(AERATE_TIME_MS);
  if (g_abortCycle) {
    Serial.println("[REF] Abort — returning reference water to reservoir...");
    stepML(PUMP_A_STEP, PUMP_A_DIR, cal.sampleML * 1.1f, false, cal.sampleStepsPerML);
    return {0, 0, 0, false};
  }
  runSettleDelay(SETTLE_TIME_MS);
  if (g_abortCycle) {
    Serial.println("[REF] Abort — returning reference water to reservoir...");
    stepML(PUMP_A_STEP, PUMP_A_DIR, cal.sampleML * 1.1f, false, cal.sampleStepsPerML);
    return {0, 0, 0, false};
  }
  SampleReading r = readAveragedSample("REF");
  Serial.println("[REF] Returning to reservoir...");
  stepML(PUMP_A_STEP, PUMP_A_DIR, cal.sampleML * 1.1f, false, cal.sampleStepsPerML);
  return r;
}

void runFlushCycle(bool doubleRinse) {
  int cycles = doubleRinse ? 2 : 1;
  for (int i = 0; i < cycles && !g_abortCycle; i++) {
    Serial.print("[FLUSH] Rinse "); Serial.println(i + 1);
    stepML(PUMP_B_STEP, PUMP_B_DIR, RINSE_FILL_ML, true,  cal.sampleStepsPerML);
    stepML(PUMP_B_STEP, PUMP_B_DIR, RINSE_FILL_ML * 1.1f, false, cal.sampleStepsPerML);
  }
}

SampleReading runTankCycle() {
  Serial.println("[TANK] Filling from sump...");
  stepML(PUMP_B_STEP, PUMP_B_DIR, cal.sampleML, true, cal.sampleStepsPerML);
  runAeration(AERATE_TIME_MS);
  if (g_abortCycle) {
    stepML(PUMP_B_STEP, PUMP_B_DIR, cal.sampleML * 1.1f, false, cal.sampleStepsPerML);
    return {0, 0, 0, false};
  }
  runSettleDelay(SETTLE_TIME_MS);
  if (g_abortCycle) {
    stepML(PUMP_B_STEP, PUMP_B_DIR, cal.sampleML * 1.1f, false, cal.sampleStepsPerML);
    return {0, 0, 0, false};
  }
  SampleReading r = readAveragedSample("TANK");
  Serial.println("[TANK] Returning water to sump...");
  stepML(PUMP_B_STEP, PUMP_B_DIR, cal.sampleML * 1.1f, false, cal.sampleStepsPerML);
  return r;
}

// ============================================================
//  ACID TITRATION WORKFLOW  (MODE_TITRATION)
// ============================================================

float runTitrationCycle() {

  // 1. Fill sample from tank
  Serial.print("[TITR] Filling "); Serial.print(cal.sampleML, 1);
  Serial.println(" mL from sump...");
  stepML(PUMP_B_STEP, PUMP_B_DIR, cal.sampleML, true, cal.sampleStepsPerML);
  if (g_abortCycle) { drainWaste(); return NAN; }

  // 2. Pre-aerate to drive off dissolved CO2
  Serial.println("[TITR] Pre-aerating to remove CO2 (10 min)...");
  runAeration(AERATE_TIME_MS);
  if (g_abortCycle) { drainWaste(); return NAN; }

  // 3. Settle, read starting pH
  runSettleDelay(SETTLE_TIME_MS);
  if (g_abortCycle) { drainWaste(); return NAN; }

  float tempC = read_Temp_C();
  ezo_setTempComp(tempC);
  SampleReading startRead = readAveragedSample("TITR-START");
  if (!startRead.ok) {
    failCycle("[TITR] Cannot read starting pH. Check probe.");
    drainWaste(); return NAN;
  }
  Serial.print("[TITR] Start pH: "); Serial.print(startRead.pH, 4);
  Serial.print("  T="); Serial.print(startRead.tempC, 1); Serial.println(" C");

  // 4. Titration loop
  float acidML = 0.0f;
  float pH     = startRead.pH;
  granCount    = 0;

  while (!g_abortCycle) {
    handleSerialCommands();

    // Dose acid
    stepML(PUMP_D_STEP, PUMP_D_DIR, cal.doseMl, true, cal.acidStepsPerML, STEP_SPEED_ACID);
    acidML += cal.doseMl;

    // Mix — brief air pulse (air pump stayed off since pre-aeration ended)
    digitalWrite(AIR_PUMP, HIGH);
    delay(MIX_PULSE_MS);
    digitalWrite(AIR_PUMP, LOW);

    // Wait for pH stability
    pH = waitForPHStability();

    // Store Gran data point
    if (granCount < 200) {
      granData[granCount].acidML = acidML;
      granData[granCount].pH    = pH;
      granCount++;
    }

    Serial.print("[TITR] +"); Serial.print(acidML, 3);
    Serial.print(" mL   pH="); Serial.println(pH, 4);

    // Safety — probe lost
    if (!isfinite(pH)) {
      failCycle("[TITR] pH probe lost mid-titration. Check wiring and isolation.");
      drainWaste(); return NAN;
    }
    // Safety — max acid
    if (acidML >= MAX_ACID_ML) {
      failCycle("[TITR] Max acid volume reached. Check: probe calibration, HCl normality, sample volume.");
      drainWaste(); return NAN;
    }
    // Safety — over-acidified
    if (pH < ABORT_PH) {
      failCycle("[TITR] pH below abort limit. Overacidification prevented.");
      drainWaste(); return NAN;
    }
    // Endpoint
    if (pH <= ENDPOINT_PH) break;
  }

  if (g_abortCycle) { drainWaste(); return NAN; }

  // 5. Refine equivalence volume with Gran function
  float ve_used = acidML;   // fallback: fixed-endpoint volume
  float ve_gran = fitGranEquivalence();
  if (isfinite(ve_gran) && ve_gran > 0.0f && ve_gran <= acidML * 1.20f) {
    ve_used = ve_gran;
    Serial.print("[TITR] Gran Ve="); Serial.print(ve_gran, 4); Serial.println(" mL (used)");
  } else {
    Serial.print("[TITR] Gran fit unavailable — fixed endpoint: ");
    Serial.print(acidML, 4); Serial.println(" mL");
  }

  // 6. Calculate total alkalinity
  //    N_acid is in equivalents/L and both volumes are in mL.
  //    (N_acid x Ve_mL / V_sample_mL) gives eq/L.
  //    Convert eq/L to dKH with 2800 dKH per eq/L.
  //    Equivalent form: meq/L = eq/L x 1000, then dKH = meq/L x 2.8.
  float ta_dKH = (cal.acidNormality * ve_used / cal.sampleML) * 2800.0f;

  if (!isfinite(ta_dKH) || ta_dKH < MIN_VALID_DKH || ta_dKH > MAX_VALID_DKH) {
    failCycle("[TITR] Result out of range. Verify acid normality and sample volume calibration.");
    drainWaste(); return NAN;
  }

  // 7. Report
  Serial.println("----------------------------------------");
  Serial.print("RESULT: "); Serial.print(ta_dKH, 2); Serial.println(" dKH  [titration]");
  Serial.print("Acid used (Ve): ");    Serial.print(ve_used, 4);         Serial.println(" mL");
  Serial.print("Sample volume:  ");    Serial.print(cal.sampleML, 1);    Serial.println(" mL");
  Serial.print("Acid normality: ");    Serial.println(cal.acidNormality, 4);
  Serial.println("----------------------------------------");

  // 8. Drain acidic waste
  drainWaste();
  return ta_dKH;
}

// Wait until consecutive pH reads differ by less than PH_STABLE_DELTA,
// or STABILIZE_MS elapses. Returns last reading.
float waitForPHStability() {
  float prevPH = NAN;
  unsigned long t0 = millis();
  while (millis() - t0 < STABILIZE_MS) {
    handleSerialCommands();
    if (g_abortCycle) return NAN;
    float pH = read_pH_Probe();
    if (isfinite(pH) && pH > 2.0f && pH < 10.0f) {
      if (isfinite(prevPH) && fabsf(pH - prevPH) < PH_STABLE_DELTA) return pH;
      prevPH = pH;
    }
    delay(1000);
  }
  return prevPH;
}

// Gran function: F(Va) = (Vs + Va) x 10^(-pH) is linear near the
// equivalence volume. Fit a line to the pH 4.0-6.5 region and
// extrapolate to F=0 to find Ve.
float fitGranEquivalence() {
  float sumX=0, sumY=0, sumXY=0, sumX2=0;
  int n = 0;
  for (int i = 0; i < granCount; i++) {
    float pH = granData[i].pH;
    if (pH < 4.0f || pH > 6.5f) continue;
    float Va = granData[i].acidML;
    float F  = (cal.sampleML + Va) * powf(10.0f, -pH);
    sumX += Va;  sumY += F;
    sumXY += Va * F;  sumX2 += Va * Va;
    n++;
  }
  if (n < 4) return NAN;
  float D = (float)n * sumX2 - sumX * sumX;
  if (fabsf(D) < 1e-12f) return NAN;
  float slope     = ((float)n * sumXY - sumX * sumY) / D;
  float intercept = (sumY - slope * sumX) / (float)n;
  if (slope >= 0.0f || fabsf(slope) < 1e-12f) return NAN;
  return -intercept / slope;
}

// Drain chamber to waste container via Pump C.
// Pumps 1.5x sample volume to ensure full evacuation.
void drainWaste() {
  Serial.println("[WASTE] Draining to waste container...");
  stepML(PUMP_C_STEP, PUMP_C_DIR, cal.sampleML * 1.5f, true, cal.sampleStepsPerML);
}

// ============================================================
//  SHARED UTILITIES
// ============================================================

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

void runSettleDelay(unsigned long ms) {
  unsigned long t0 = millis();
  while (millis() - t0 < ms) {
    handleSerialCommands();
    if (g_abortCycle) return;
    delay(100);
  }
}

void sleepUntilNextCycle() {
  Serial.print("Next cycle in "); Serial.print(TEST_INTERVAL_MS / 3600000.0f, 1);
  Serial.println(" h.");
  Serial.println("Commands: STATUS  MODE PHDI  MODE TITR  CAL  PRIME  RUN  PUMPS_OFF");
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
  g_abortCycle = true;
  // Setting g_abortCycle stops any active stepMotor() loop within ~125 ms.
  // Air pump has an explicit GPIO:
  digitalWrite(AIR_PUMP, LOW);
  // Note: stepper coils remain energised (holding position) unless ENABLE
  // pins are driven HIGH. Add digitalWrite(ENABLE_PIN, HIGH) here if wired.
}

// ============================================================
//  MEASUREMENT
// ============================================================

SampleReading readAveragedSample(const char* label) {
  SampleReading result = {0.0f, 0.0f, 0.0f, false};

  float tempC = read_Temp_C();
  if (!isfinite(tempC) || tempC < MIN_VALID_TEMP_C || tempC > MAX_VALID_TEMP_C) {
    Serial.print("  ERROR: Invalid temperature: "); Serial.println(tempC, 2);
    return result;
  }
  ezo_setTempComp(tempC);

  Serial.print("  ["); Serial.print(label);
  Serial.print("] T="); Serial.print(tempC, 2);
  Serial.print(" C  Averaging pH for "); Serial.print(READ_WINDOW_MS/1000UL);
  Serial.println(" s...");

  float pHValues[32]; int n = 0;
  unsigned long start = millis();
  while ((millis() - start) < READ_WINDOW_MS && n < 32) {
    float pH = read_pH_Probe();
    if (isfinite(pH) && pH >= MIN_VALID_PH && pH <= MAX_VALID_PH) pHValues[n++] = pH;
    else Serial.println("    WARN: pH out of range, skipping.");
    delay(READ_PERIOD_MS);
  }

  if (n < 5) {
    Serial.print("  ERROR: Only "); Serial.print(n); Serial.println(" valid reads (need 5).");
    return result;
  }

  float sum = 0; for (int i=0;i<n;i++) sum+=pHValues[i];
  float mean = sum/(float)n;
  float var  = 0; for (int i=0;i<n;i++) { float d=pHValues[i]-mean; var+=d*d; }
  float sd   = sqrtf(var/(float)n);

  if (sd > PH_SD_WARN_THRESHOLD) {
    Serial.print("  WARN: pH SD="); Serial.print(sd, 5);
    Serial.println(" — CO2 may not be equilibrated. Consider longer AERATE_TIME_MS.");
  }

  Serial.print("  pH="); Serial.print(mean,4);
  Serial.print("  SD="); Serial.print(sd,5);
  Serial.print("  n="); Serial.println(n);

  result.pH=mean; result.tempC=tempC; result.pHStdDev=sd; result.ok=true;
  return result;
}

// ============================================================
//  ALKALINITY MATH (pH differential mode)
// ============================================================

float calculateAlkalinity_dKH(float pH_ref, float pH_tank) {
  float H_ref  = powf(10.0f, -pH_ref);
  float H_tank = powf(10.0f, -pH_tank);
  float BA_ref  = borateAlkalinity_eqL(H_ref);
  float BA_tank = borateAlkalinity_eqL(H_tank);
  float TA_ref  = cal.referenceAlk_dKH * DKH_TO_EQ_PER_L;
  float carb    = TA_ref - BA_ref;
  float ct_ref  = carbonateSpeciationTerm(H_ref);
  float ct_tank = carbonateSpeciationTerm(H_tank);
  if (!isfinite(ct_ref)||!isfinite(ct_tank)||ct_ref<=0||carb<=0) {
    Serial.println("MATH ERROR: Degenerate carbonate terms."); return NAN;
  }
  return (carb * (ct_tank/ct_ref) + BA_tank) * EQ_PER_L_TO_DKH;
}

float carbonateSpeciationTerm(float H) {
  return (K1_FIXED/H) + (2.0f*K1_FIXED*K2_FIXED/(H*H));
}

float borateAlkalinity_eqL(float H) {
  return (TOTAL_BORON_EQ_PER_L * KB_FIXED) / (KB_FIXED + H);
}

// ============================================================
//  DAC TREND OUTPUT
// ============================================================

float mapFloat(float x, float inMin, float inMax, float outMin, float outMax) {
  return (x-inMin)*(outMax-outMin)/(inMax-inMin)+outMin;
}

void updateTrendOutput(float alk_dKH) {
  float fakePH = mapFloat(alk_dKH, MAP_DKH_LOW, MAP_DKH_HIGH, MAP_FAKE_PH_LOW, MAP_FAKE_PH_HIGH);
  if (fakePH < MAP_FAKE_PH_LOW)  fakePH = MAP_FAKE_PH_LOW;
  if (fakePH > MAP_FAKE_PH_HIGH) fakePH = MAP_FAKE_PH_HIGH;
  int dacVal = (int)(mapFloat(fakePH, MAP_FAKE_PH_LOW, MAP_FAKE_PH_HIGH, 80.0f, 200.0f)+0.5f);
  if (dacVal < 0) dacVal = 0;
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
      && isfinite(cal.sampleStepsPerML) && cal.sampleStepsPerML > 0
      && isfinite(cal.acidStepsPerML)   && cal.acidStepsPerML   > 0
      && isfinite(cal.acidNormality)    && cal.acidNormality     > 0
      && isfinite(cal.sampleML)         && cal.sampleML          > 0
      && isfinite(cal.doseMl)           && cal.doseMl            > 0;
}

void initDefaultCalibration() {
  cal.signature        = EEPROM_SIGNATURE;
  cal.referenceAlk_dKH = 0.0f;     // set with: CAL <dKH>
  cal.mode             = MODE_PH_DIFF;
  cal.acidNormality    = 0.1000f;  // 0.1 N HCl — verify with: CAL ACID <N>
  cal.sampleStepsPerML = 3200.0f;  // calibrate with: CAL SAMPLESPM <steps>
  cal.acidStepsPerML   = 6400.0f;  // calibrate with: CAL ACIDSPM <steps>
  cal.sampleML         = 25.0f;    // adjust with: CAL SAMPLEML <mL>
  cal.doseMl           = 0.020f;   // 20 µL per dose; adjust with: CAL DOSE <mL>
  saveCalibration();
}

void saveCalibration() {
  EEPROM.put(EEPROM_ADDR, cal);
  EEPROM.commit();
}

// ============================================================
//  SERIAL COMMAND HANDLER
// ============================================================

void handleSerialCommands() {
  if (!Serial.available()) return;
  String line = Serial.readStringUntil('\n');
  line.trim(); line.toUpperCase();

  // ── pH differential anchor ───────────────────────────────
  if (line.startsWith("CAL ") &&
      !line.startsWith("CAL A") && !line.startsWith("CAL S") && !line.startsWith("CAL D")) {
    float v = line.substring(4).toFloat();
    if (v >= MIN_VALID_DKH && v <= MAX_VALID_DKH) {
      cal.referenceAlk_dKH = v; saveCalibration();
      Serial.print("pH-diff anchor saved: "); Serial.print(v, 2); Serial.println(" dKH");
    } else Serial.println("Invalid. Range 3-20 dKH. Example: CAL 8.50");
  }
  // ── Acid normality ───────────────────────────────────────
  else if (line.startsWith("CAL ACID ")) {
    float v = line.substring(9).toFloat();
    if (v > 0 && v < 2.0f) {
      cal.acidNormality = v; saveCalibration();
      Serial.print("Acid normality: "); Serial.print(v, 4); Serial.println(" N saved");
    } else Serial.println("Example: CAL ACID 0.1000");
  }
  // ── Acid pump steps/mL ───────────────────────────────────
  else if (line.startsWith("CAL ACIDSPM ")) {
    float v = line.substring(12).toFloat();
    if (v > 0) {
      cal.acidStepsPerML = v; saveCalibration();
      Serial.print("Acid pump steps/mL: "); Serial.println(v, 1);
    } else Serial.println("Example: CAL ACIDSPM 6400.0");
  }
  // ── Sample pump steps/mL ────────────────────────────────
  else if (line.startsWith("CAL SAMPLESPM ")) {
    float v = line.substring(14).toFloat();
    if (v > 0) {
      cal.sampleStepsPerML = v; saveCalibration();
      Serial.print("Sample pump steps/mL: "); Serial.println(v, 1);
    } else Serial.println("Example: CAL SAMPLESPM 3200.0");
  }
  // ── Sample volume ────────────────────────────────────────
  else if (line.startsWith("CAL SAMPLEML ")) {
    float v = line.substring(13).toFloat();
    if (v > 5 && v < 200) {
      cal.sampleML = v; saveCalibration();
      Serial.print("Sample volume: "); Serial.print(v, 1); Serial.println(" mL saved");
    } else Serial.println("Example: CAL SAMPLEML 25.0");
  }
  // ── Dose per increment ───────────────────────────────────
  else if (line.startsWith("CAL DOSE ")) {
    float v = line.substring(9).toFloat();
    if (v > 0.001f && v < 0.5f) {
      cal.doseMl = v; saveCalibration();
      Serial.print("Dose per increment: "); Serial.print(v, 4); Serial.println(" mL saved");
    } else Serial.println("Example: CAL DOSE 0.020  (= 20 uL)");
  }
  // ── Mode switching ───────────────────────────────────────
  else if (line == "MODE PHDI") {
    cal.mode = MODE_PH_DIFF; saveCalibration();
    Serial.println("Mode: pH Differential (ratiometric)");
    if (cal.referenceAlk_dKH < MIN_VALID_DKH)
      Serial.println("  WARNING: no anchor set — send CAL <dKH>");
  }
  else if (line == "MODE TITR") {
    cal.mode = MODE_TITRATION; saveCalibration();
    Serial.println("Mode: Acid Titration");
    Serial.print("  Acid normality: "); Serial.print(cal.acidNormality, 4); Serial.println(" N");
    Serial.print("  Sample volume:  "); Serial.print(cal.sampleML, 1); Serial.println(" mL");
    Serial.print("  Dose per step:  "); Serial.print(cal.doseMl, 4); Serial.println(" mL");
  }
  // ── Status ───────────────────────────────────────────────
  else if (line == "STATUS") {
    Serial.print("Mode: "); Serial.println(cal.mode==MODE_PH_DIFF?"pH Differential":"Acid Titration");
    Serial.print("Sim mode: ");         Serial.println(SIMULATION_MODE?"YES":"NO");
    Serial.print("pH-diff anchor: ");   Serial.print(cal.referenceAlk_dKH,2); Serial.println(" dKH");
    Serial.print("Acid normality: ");   Serial.print(cal.acidNormality,4);    Serial.println(" N");
    Serial.print("Sample volume:  ");   Serial.print(cal.sampleML,1);         Serial.println(" mL");
    Serial.print("Dose per step:  ");   Serial.print(cal.doseMl,4);           Serial.println(" mL");
    Serial.print("Sample steps/mL: ");  Serial.println(cal.sampleStepsPerML,1);
    Serial.print("Acid steps/mL:   ");  Serial.println(cal.acidStepsPerML,1);
  }
  // ── Emergency stop ───────────────────────────────────────
  else if (line == "PUMPS_OFF") {
    ensurePumpsOff();
    Serial.println("EMERGENCY STOP — steppers halted, air pump off.");
    Serial.println("Run PRIME to flush chamber before next cycle.");
  }
  // ── Immediate run ────────────────────────────────────────
  else if (line == "RUN") {
    g_forceRun = true;
    Serial.println("RUN queued.");
  }
  // ── Prime all lines ──────────────────────────────────────
  else if (line == "PRIME") {
    Serial.println("PRIME: Priming all pump lines...");
    Serial.println("  1/6 — Pump A fwd: reservoir to chamber (30 mL)");
    stepML(PUMP_A_STEP, PUMP_A_DIR, 30.0f, true,  cal.sampleStepsPerML);
    Serial.println("  2/6 — Pump A rev: chamber to reservoir (30 mL)");
    stepML(PUMP_A_STEP, PUMP_A_DIR, 30.0f, false, cal.sampleStepsPerML);
    Serial.println("  3/6 — Pump B fwd: sump to chamber (30 mL)");
    stepML(PUMP_B_STEP, PUMP_B_DIR, 30.0f, true,  cal.sampleStepsPerML);
    Serial.println("  4/6 — Pump B rev: chamber to sump (30 mL)");
    stepML(PUMP_B_STEP, PUMP_B_DIR, 30.0f, false, cal.sampleStepsPerML);
    Serial.println("  5/6 — Pump C: waste drain line (20 mL)");
    stepML(PUMP_C_STEP, PUMP_C_DIR, 20.0f, true,  cal.sampleStepsPerML);
    Serial.println("  6/6 — Pump D: acid injection line (5 mL, slow)");
    stepML(PUMP_D_STEP, PUMP_D_DIR,  5.0f, true,  cal.acidStepsPerML, STEP_SPEED_ACID);
    Serial.println("PRIME complete. Verify flow and check for leaks before first RUN.");
    Serial.println("Calibrate steps/mL with: CAL SAMPLESPM <n>  and  CAL ACIDSPM <n>");
  }
  // ── Help ─────────────────────────────────────────────────
  else {
    Serial.println("=== Commands ===");
    Serial.println("CAL <dKH>          pH-diff anchor (e.g. CAL 8.50)");
    Serial.println("CAL ACID <N>       acid normality (e.g. CAL ACID 0.1000)");
    Serial.println("CAL SAMPLEML <mL>  sample volume (e.g. CAL SAMPLEML 25.0)");
    Serial.println("CAL DOSE <mL>      dose/step (e.g. CAL DOSE 0.020)");
    Serial.println("CAL SAMPLESPM <n>  sample pump steps/mL (calibration)");
    Serial.println("CAL ACIDSPM <n>    acid pump steps/mL (calibration)");
    Serial.println("MODE PHDI          switch to pH differential mode");
    Serial.println("MODE TITR          switch to acid titration mode");
    Serial.println("STATUS             show all current settings");
    Serial.println("PRIME              prime all pump lines");
    Serial.println("RUN                trigger immediate cycle");
    Serial.println("PUMPS_OFF          emergency stop");
  }
}

// ============================================================
//  SENSOR IMPLEMENTATIONS
// ============================================================

void ezo_setTempComp(float tempC) {
#if !SIMULATION_MODE
  char cmd[20];
  snprintf(cmd, sizeof(cmd), "T,%.2f", tempC);
  EZO_SERIAL.print(cmd);
  EZO_SERIAL.print('\r');
  delay(300);
  while (EZO_SERIAL.available()) EZO_SERIAL.read();
#else
  (void)tempC;
#endif
}

float read_pH_Probe() {
#if SIMULATION_MODE
  static float base = 8.150f;
  return base + ((float)(millis()%1000)/1000.0f - 0.5f) * 0.002f;
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

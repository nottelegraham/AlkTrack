# AlkTrack — DIY Ratiometric Alkalinity Monitor

To do - GUI mode selection, pH probe calibration at 4.01, 7.01, and 10.01.

**TelegrahamTested** — Firmware v3.0

AlkTrack is an open, non-commercial DIY alkalinity monitor for reef aquariums. Version 3.0 supports two user-selectable operating modes:

1. **pH differential mode** — reagent-free trend monitoring using a known reference sample and a tank sample measured in the same reaction chamber.
2. **Acid titration mode** — true acid-demand alkalinity testing using a fixed tank-water sample volume and a known-strength acid reagent.

This project is intended for education, experimentation, and hobby use. It is not a commercial product, a laboratory instrument, or a replacement for independent judgment.

---

## What changed in v3.0

Version 3.0 is no longer only a reagent-free pH differential trend monitor. It now includes a second operating mode for traditional acid titration.

Major changes:

- Stepper-driven peristaltic pumps replace relay-timed pump control.
- Volume is controlled by calibrated steps per mL instead of pump runtime.
- User-selectable modes are available from Serial Monitor:
  - `MODE PHDI` for pH differential/ratiometric mode.
  - `MODE TITR` for acid titration mode.
- Titration mode uses Pump D for acid reagent dosing.
- Titration mode calculates alkalinity from acid normality, endpoint volume, and sample volume.
- EEPROM calibration now stores operating mode, sample volume, acid normality, pump steps/mL, and dose increment.
- The titration dKH calculation uses the corrected conversion:

```text
dKH = (acid_normality × acid_mL / sample_mL) × 2800
```

---

## What it does

### pH differential mode

Draws a known reference sample into a shared reaction chamber, aerates it, records pH and temperature, then removes it. A tank sample is then drawn into the same chamber, aerated under the same general conditions, and measured with the same pH and temperature probes.

The result is a **reference-anchored alkalinity trend estimate**. This mode is reagent-free and best used to watch whether alkalinity is rising, falling, or stable.

### Acid titration mode

Draws a fixed volume of tank water into the reaction chamber, adds known-strength acid in measured increments using a stepper-driven dosing pump, monitors pH, detects an endpoint near the total alkalinity region, and calculates alkalinity from the acid volume consumed.

This mode is a traditional chemical titration. It consumes reagent and produces acidic waste.

---

## Hardware summary

Minimum v3.0 hardware:

- ESP32 DevKit C, 38-pin
- Atlas Scientific EZO-pH circuit on an electrically isolated carrier
- Lab-grade double-junction pH probe with BNC connector
- DS18B20 waterproof temperature probe
- Four stepper-driven peristaltic pumps:
  - Pump A: reference reservoir to/from reaction chamber
  - Pump B: tank/sample fill
  - Pump C: chamber drain/waste or controlled evacuation
  - Pump D: acid reagent dosing for titration mode
- Four A4988 or DRV8825 stepper motor drivers
- Micro air pump and ceramic fine-bubble airstone
- Reference reservoir
- Acid reagent reservoir for titration mode
- Waste collection container for titration mode
- 12V supply rated at 5A minimum
- 5V/USB supply for ESP32
- Optional isolated DAC/BNC output for controller-readable trend logging

---

## Firmware

Open:

```text
alkalinity_monitor_v3_0/alkalinity_monitor_v3_0.ino
```

Libraries required when `SIMULATION_MODE false`:

- OneWire by Paul Stoffregen
- DallasTemperature by Miles Burton

First run:

1. Upload firmware with `SIMULATION_MODE true` for bench testing.
2. Open Serial Monitor at 115200 baud.
3. Confirm commands respond with `STATUS`.
4. Set mode:
   - `MODE PHDI` for pH differential mode.
   - `MODE TITR` for acid titration mode.
5. For pH differential mode, enter the reference alkalinity anchor, for example: `CAL 8.50`.
6. For titration mode, set acid normality, sample volume, and pump calibration values.
7. Prime tubing with `PRIME`.
8. Set `SIMULATION_MODE false`, re-upload, and run `RUN`.

---

## Serial commands

| Command | What it does |
|---|---|
| `STATUS` | Show current mode, calibration values, and last result |
| `RUN` | Trigger immediate measurement cycle |
| `MODE PHDI` | Switch to pH differential/ratiometric mode |
| `MODE TITR` | Switch to acid titration mode |
| `CAL 8.50` | Set pH differential reference anchor in dKH |
| `CAL ACID 0.1000` | Set acid reagent normality for titration mode |
| `CAL SAMPLEML 25.0` | Set titration sample volume in mL |
| `CAL DOSE 0.020` | Set acid dose increment in mL |
| `CAL SAMPLESPM 3200` | Set sample pump calibration in steps/mL |
| `CAL ACIDSPM 6400` | Set acid pump calibration in steps/mL |
| `PRIME` | Prime fluid lines |
| `PUMPS_OFF` | Emergency stop and cycle abort |

---

## How the two methods work

### pH differential/ratiometric mode

```text
TA_tank = (TA_ref - BA_ref) × carbonate_speciation_ratio + BA_tank
```

Where:

```text
carbonate_speciation_ratio =
[(K1_tank / H_tank) + (2 × K1_tank × K2_tank / H_tank²)] /
[(K1_ref / H_ref) + (2 × K1_ref × K2_ref / H_ref²)]
```

All internal alkalinity math must use compatible units. Inputs and outputs are shown in dKH.

### Acid titration mode

```text
dKH = (N_acid × V_acid_mL / V_sample_mL) × 2800
```

Where `N_acid` is acid normality, `V_acid_mL` is the endpoint acid volume, and `V_sample_mL` is the tank-water sample volume.

---

## Documents

| File | Description |
|---|---|
| `DIY_Alkalinity_Monitor_Build_Guide_v3_0.docx` | Hardware, wiring, assembly, and calibration guide |
| `DIY_Alkalinity_Monitor_User_Guide_v3_0.docx` | Operating instructions for both modes |
| `DIY_Alkalinity_Monitor_Price_Sheet_v3_0.xlsx` | Updated component price sheet |
| `DIY_Alkalinity_Monitor_Changelog_v3_0.docx` | Version history through v3.0 |

---

## License

This project is released under the **Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International License (CC BY-NC-SA 4.0)**.

You may fork it, modify it, improve it, share it, and build on it for personal, educational, or hobby use. You may not sell the project files, kits, assembled devices, or commercial products based on this design. Modified versions must be shared under the same license.

Full license:
https://creativecommons.org/licenses/by-nc-sa/4.0/

---

## Disclaimer

This project is provided as-is for educational, experimental, and hobby use. It includes water handling, electrical components, and optional acid reagent handling. Build and use it at your own risk. Verify all results with independent testing before making aquarium dosing decisions.

# DIY Ratiometric Alkalinity Monitor
**Telegraham Tested** — Firmware v2.1.2

A reagent-free automated alkalinity trend monitor for reef aquariums. Uses carbonate chemistry to track whether your dKH is rising, falling, or stable — automatically, every 4 hours, without titration or chemicals.

## What it does

Draws a small water sample from your tank, aerates it alongside a sealed reference water sample under the same room air, then compares their stabilised pH readings to estimate alkalinity. Room CO₂ cancels out mathematically — it does not affect the result.

**This is a trend monitor, not a lab instrument.** Target repeatability: ±0.1–0.2 dKH. Cross-check with a manual titration kit every 1–2 weeks.

## Hardware

- ESP32 DevKit C (38-pin)
- Atlas Scientific EZO-pH circuit + electrically isolated carrier board
- Lab-grade double-junction pH probe (BNC)
- DS18B20 waterproof temperature probe
- 3× 12V peristaltic pumps (Pump A reversible, Pump B tank in, Pump C drain)
- Micro air pump + ceramic fine-bubble airstone
- PTFE hydrophobic membrane vent plug (for reference reservoir)
- 4-channel opto-isolated relay module

See the full **Build Guide** and **Price Sheet** for the complete component list and wiring instructions.

## Firmware

Open `alkalinity_monitor_v2_1_1/alkalinity_monitor_v2_1_1.ino` in Arduino IDE.

**Libraries required** (install via Library Manager when `SIMULATION_MODE false`):
- OneWire by Paul Stoffregen
- DallasTemperature by Miles Burton

**First run:**
1. Upload firmware (ships with `SIMULATION_MODE true` for bench testing)
2. Open Serial Monitor at 115200 baud
3. Enter your Day 1 titration result: `CAL 8.50`
4. Prime all tubing lines: `PRIME`
5. Set `SIMULATION_MODE false`, re-upload, run: `RUN`

**Serial commands:**

| Command | What it does |
|---|---|
| `CAL 8.50` | Set reference anchor in dKH |
| `STATUS` | Show current anchor and mode |
| `RUN` | Trigger immediate cycle |
| `PRIME` | Prime all tubing on first install |
| `PUMPS_OFF` | Emergency stop |

## Documents

| File | Description |
|---|---|
| `Build_Guide.docx` | Full build instructions and shopping list |
| `User_Guide.docx` | Step-by-step operating instructions |
| `Price_Sheet.xlsx` | Itemised component cost spreadsheet |
| `Changelog.docx` | Full version history v1 → v2.1.2 |

## How it works

After aerating both samples with the same room air, both reach the same atmospheric CO₂ equilibrium. The master equation:

```
TA_tank = (TA_ref − BA_ref) × ratio + BA_tank
```

Where `ratio = f(K1, K2, H_tank) / f(K1, K2, H_ref)` and `f(K1, K2, H) = K1/H + 2·K1·K2/H²`

Room CO₂ (pCO₂) appears in both numerator and denominator and cancels completely. Borate alkalinity (BA) is stripped from the reference and added back for the tank using BT and KB constants. All arithmetic runs internally in eq/L; input and output are in dKH.

Equilibrium constants: K1 and K2 from Lueker et al. (2000), KB from Dickson (1990), fixed at 25 °C / 35 ppt for trend-only use.

## License

**CC0 1.0 Universal — No rights reserved.**  
Firmware, documentation, schematics, and all associated files are dedicated to the public domain. Copy, modify, sell, redistribute — no permission, attribution, or conditions required.  
See [LICENSE](LICENSE) or [creativecommons.org/publicdomain/zero/1.0](https://creativecommons.org/publicdomain/zero/1.0/)

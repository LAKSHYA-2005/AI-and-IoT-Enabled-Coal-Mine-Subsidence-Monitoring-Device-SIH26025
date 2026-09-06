# AI-and-IoT-Enabled-Coal-Mine-Subsidence-Monitoring-Device-SIH26025

# SENSE-MINE

**Distributed Edge-AI Surface Subsidence Monitoring & Early Warning System**
Smart India Hackathon 2026 — Problem Statement 26025

> Development of an AI-enabled Low Cost Real Time Mine Subsidence Monitoring, Prediction and Early Warning System for Underground Coal Mines in India
> Organization: Ministry of Coal | Department: Coal India Limited | Category: Hardware | Theme: Smart Automation

---

## 1. What this project is (and isn't)

SENSE-MINE is a 3-node distributed surface-deformation sensing network. Each node is an **ESP32-S3 with local edge AI**; a plain **ESP32 Command Centre** aggregates all three nodes over LoRa, runs spatial fusion and a rule engine on top of the ML-derived scores, and drives GSM-based always-on alerting plus a dual-mode (online/offline) dashboard.

**This is a sensor / network / AI validation platform, not a geomechanical replica of an underground coal mine.** That distinction governs every claim in this document:

- No official or field dataset exists for PS 26025. Training data is physics-informed synthetic (Probability Integral Method + Knothe time function), not real mine sensor data.
- The wireless mesh requirement in the PS is a stated baseline, not this project's innovation — every team addressing this PS is expected to build some form of it.
- The system does not output absolute multi-hour displacement forecasts. It outputs short-horizon trend/state classification, justified by comparison to peer-reviewed InSAR studies that needed years of real satellite data for millimeter-level forecasts.
- Mine/geological parameters used in this prototype (depth, seam thickness, extraction ratio, etc.) are **published literature values**, explicitly tagged as such — never presented as measured by this hardware.

---

## 2. System Architecture

```
   NODE 1                NODE 2                NODE 3
 ESP32-S3+EdgeAI       ESP32-S3+EdgeAI       ESP32-S3+EdgeAI
 2x potentiometer,       (same sensors)         (same sensors)
 MPU6050, DS18B20,
 OLED (5 pages)
        \                    |                    /
         \___________________|___________________/
                        LoRa 433MHz
                             |
                             v
                 +--------------------------+
                 |     COMMAND CENTRE       |
                 |     Plain ESP32          |
                 |  - LoRa RX               |
                 |  - Spatial fusion        |
                 |  - Rule engine (on top   |
                 |    of ML/Z-score/PIM)    |
                 |  - Fixed mine parameters |
                 |  - SIM800L (SMS + Call)  |
                 +------+---------+---------+
                        |         |
             USB-Serial |         | Wi-Fi (best-effort)
            (always on) |         |
                        v         v
          +-------------------+  +---------------------------+
          |  Node-RED (local) |  |  FastAPI + SQLite +        |
          |  offline dashboard|  |  React/TS + schematic GIS  |
          +-------------------+  +---------------------------+
   (online and offline dashboards run independently, by design, not synced)
```

---

## 3. Hardware

### 3.1 Sensor Node (×3) — Final BOM

| Component | Function | Interface | Approx. Price (INR) |
|---|---|---|---|
| ESP32-S3-DevKitC-1 (N8R8/N16R8) | Node MCU + local edge AI (per-node anomaly scoring) | — | 700–900 |
| WH20-1-0 potentiometer #1 | Crack width | Analog ADC (GPIO1) | 80–150 |
| WH20-1-0 potentiometer #2 | Vertical displacement | Analog ADC (GPIO2) | 80–150 |
| MPU6050 (clone chip) | Tilt X/Y, vibration RMS/peak, dominant frequency | I2C (GPIO8/9) | ~150 |
| DS18B20 | Temperature — drift compensation | 1-Wire (GPIO4) | 80–150 |
| SSD1306 OLED | Local debug display, 5 auto-cycling pages | I2C (shared bus) | ~150–250 |
| SX1278 (Ra-02) LoRa | Radio link to Command Centre | SPI (GPIO10–15) | 479 |

**PSRAM variant of the ESP32-S3 is mandatory**, not optional — the local edge-AI inference needs it.

### 3.2 Node pin map

| GPIO | Function | Component |
|---|---|---|
| GPIO1 | ADC1 | Crack-width potentiometer |
| GPIO2 | ADC1 | Vertical-displacement potentiometer |
| GPIO4 | 1-Wire | DS18B20 |
| GPIO8 | I2C SDA | MPU6050 + OLED (shared bus) |
| GPIO9 | I2C SCL | MPU6050 + OLED (shared bus) |
| GPIO10 | SPI CS/NSS | SX1278 |
| GPIO11 | SPI MOSI | SX1278 |
| GPIO12 | SPI SCK | SX1278 |
| GPIO13 | SPI MISO | SX1278 |
| GPIO14 | LoRa RESET | SX1278 |
| GPIO15 | LoRa DIO0 | SX1278 |

### 3.3 Command Centre — Final BOM

| Component | Function | Approx. Price (INR) |
|---|---|---|
| ESP32-WROOM-32 (plain ESP32) | Aggregation, spatial fusion, rule engine, alert dispatch | 280–530 |
| SX1278 (Ra-02) | LoRa RX from 3 nodes | 479 |
| SIM800L | SMS + voice-call alerts (Command Centre only, not per-node) | 184–400 |
| External ~4.0V/2A regulator | Dedicated SIM800L power supply | 150–250 |
| UART level-shifter (if needed) | ESP32 TX → SIM800L RX level matching | ~50–100 |

### 3.4 Command Centre pin map

| GPIO | Connected to | Function |
|---|---|---|
| GPIO5 | SX1278 NSS/CS | LoRa chip select |
| GPIO18 | SX1278 SCK | SPI clock |
| GPIO19 | SX1278 MISO | SPI data |
| GPIO23 | SX1278 MOSI | SPI data |
| GPIO14 | SX1278 RESET | LoRa reset |
| GPIO26 | SX1278 DIO0 | LoRa interrupt |
| GPIO16 | SIM800L TXD | ESP32 RX2 |
| GPIO17 | SIM800L RXD | ESP32 TX2 |
| GPIO27 | SIM800L RST | Optional reset |

### 3.5 Wiring cautions — read before powering anything

- **SIM800L VCC → external regulated ~4.0V/2A supply, never the ESP32 3.3V pin.** SIM800L draws high-current pulses during transmission that will brown out a 3.3V rail.
- **SIM card must be Airtel, Vodafone Idea, or BSNL.** Reliance Jio has no 2G network at all — a Jio SIM will not work in the SIM800L.
- **Check your exact SIM800L breakout's UART electrical spec** before wiring TX directly; some require level-shifting for the 3.3V↔SIM800L interface.
- **Bring-up order** (per node): (1) ESP32-S3 + Pot #1, (2) add Pot #2, (3) add DS18B20, (4) add MPU6050, (5) add OLED, (6) add SX1278, (7) integrate edge AI, (8) test full LoRa link to Command Centre. Wiring everything at once makes faults hard to isolate.

---

## 4. Firmware

Repository files: `firmware/sense_mine_node.ino`, `firmware/sense_mine_command_centre.ino`

**Status: bring-up/integration test firmware.** These sketches confirm every sensor reads correctly and every link works end-to-end. They do not yet include the full production pipeline (FreeRTOS task/core split, watchdog timers, adaptive-baseline freeze/unfreeze, PIM classifier inference) described in Section 6 — add those once the hardware is confirmed working.

### 4.1 Node firmware

```
Read crack pot + displacement pot (ADC)
Read MPU6050 (bare I2C, no library — clone-chip compatible)
   -> tilt X/Y from gravity vector
   -> 128-sample vibration burst -> RMS, peak, FFT dominant frequency
Read DS18B20 (1-Wire)
Compute derived: growth/velocity/acceleration/tilt-rate, cumulative growth
Display on OLED (5 auto-cycling debug pages)
Transmit compact LoRa packet (NODE=.. key=value pairs)
```

**No `STATE` field is transmitted from the node.** A single node cannot validly judge whether a reading is genuine deformation or local interference — that decision requires all 3 nodes together, so it's made only at the Command Centre.

MPU6050 uses **bare register access, no library** — this board uses a non-original/clone chip whose `WHO_AM_I` response is unreliable with standard libraries; the code proceeds regardless of what that register returns.

**Libraries required:** OneWire, DallasTemperature, Adafruit_GFX, Adafruit_SSD1306, LoRa (sandeepmistry), arduinoFFT.

**Known caveat:** derived rates (`crack_growth_rate_mm_day`, etc.) are extrapolated from a short bench-test interval to a "per day" unit — on a multi-second test cycle this produces large, noisy numbers. Expected behavior on a short test, not a bug; treat as instantaneous-rate estimates until running over real multi-hour/day timescales. Potentiometer ADC→mm calibration in the code uses placeholder endpoints — recalibrate against your actual WH20-1-0 travel before trusting displacement values.

### 4.2 Command Centre firmware

```
LoRa RX -> parse KEY=VALUE packet -> store per-node latest reading
      | (on regular fusion interval, all 3 nodes fresh)
      v
Spatial fusion: spatial_displacement_gradient, spatial_coherence
      v
Rule engine (consumes ML-derived scores as inputs, decides on top of them):
   - deformation_state (STABLE/INITIATION/PROGRESSIVE/ACCELERATING/RAPID/STABILIZING)
   - risk_class (NORMAL/WATCH/WARNING/CRITICAL)
      v
Dispatch: USB-serial (Node-RED, always) + Wi-Fi POST (FastAPI, best-effort)
          + SIM800L SMS/call if risk_class == CRITICAL
```

A node's packet is treated **stale** if not refreshed within `NODE_TIMEOUT_MS` (15s default) — fusion is skipped for that cycle rather than run on outdated data silently.

`ACCELERATING` and `RAPID` states require **high spatial coherence** before firing — a sudden change on a single node alone is routed to sensor-fault handling, not escalated as a real event. This is the core defense against a false alarm from one glitchy sensor.

**Libraries required:** LoRa (sandeepmistry). SIM800L is driven via raw AT commands over `HardwareSerial` — no library, for transparency.

---

## 5. Communication Protocol

| Link | Protocol |
|---|---|
| Node ↔ Command Centre | LoRa (433MHz), custom `KEY=VALUE` packet format |
| Command Centre ↔ laptop (offline) | USB Serial, always-tethered |
| Command Centre ↔ backend (online) | Wi-Fi, HTTP POST, best-effort |
| Command Centre ↔ alert recipient | GSM (SIM800L), SMS + voice call via AT commands |
| Backend ↔ frontend | REST + WebSocket |

**Example node→gateway packet:**
```
NODE=N02,CRACK=4.82,DISP=12.40,TILT_X=1.24,TILT_Y=0.86,VRMS=0.021,VPEAK=0.084,
FREQ=7.8,TEMP=31.2,CRATE=0.9,VEL=1.82,ACC=0.41,TRATE=0.15,CUMCRK=2.1,CUMSUB=8.4
```

---

## 6. Data Schema — by computation location

| Location | Fields |
|---|---|
| **Node (raw)** | `crack_width_mm, vertical_displacement_mm, tilt_x_deg, tilt_y_deg, vibration_rms_g, vibration_peak_g, dominant_frequency_hz, temperature_C` |
| **Node (derived)** | `crack_growth_rate_mm_day, vertical_displacement_velocity_mm_day, displacement_acceleration_mm_day2, tilt_rate_deg_day, cumulative_crack_growth_mm, cumulative_subsidence_mm, rolling_mean, rolling_std, z_score, sensor_missing, sensor_stuck, sensor_spike` |
| **Command Centre (cross-node)** | `spatial_displacement_gradient, spatial_coherence, communication_loss, deformation_state, risk_class` |
| **Training-only conditioning** (never a live device input) | `mining_depth_m, seam_thickness_m, mining_height_m, panel_width_m, panel_length_m, face_advance_rate_m_day, extraction_ratio, mining_method, overburden_thickness_m, rock_density_kg_m3, cohesion_kPa, internal_friction_angle_deg, elastic_modulus_GPa, bulk_modulus_GPa, shear_modulus_GPa, poisson_ratio, UCS_MPa, tensile_strength_MPa, GSI, seam_dip_deg, fault_present, fault_distance_m, geological_disturbance_index, water_table_depth_m, soil_moisture, rainfall_mm` |
| **Dataset bookkeeping only** | `run_id, data_source` — must distinguish `testbed_measured` / `published_literature` / `synthetic_PIM`; never mislabel one as another |
| **Dropped from anything demoed** | `future_vertical_displacement_mm, future_displacement_velocity_mm_day` — absolute forecast; testbed data doesn't span the timescale needed to justify this |

### Fixed mine parameters used in this prototype (Command Centre)

| Parameter | Value | Source |
|---|---|---|
| mining_depth_m | 584.08 | Published literature |
| seam_thickness_m | 3.53 | Published literature |
| mining_height_m | 3.32 | Published literature |
| panel_width_m | 284.24 | Published literature |
| panel_length_m | 1420.16 | Published literature |
| face_advance_rate_m_day | 6.02 | Published literature |
| extraction_ratio | 0.94 | Published literature |

These are commissioning-time constants entered once, **not sensor readings** — every dataset row using them must carry `data_source = published_literature`, never `measured`.

---

## 7. AI / ML Architecture

**Design principle:** the rule engine is a transparent decision layer sitting **on top of** machine-learned/statistical scores, not a replacement for them — this preserves both a genuine AI/ML claim and an explainable final decision ("why was this warning raised").

### Layer 1 — Per-node adaptive baseline (primary, on-device)
Rolling mean/std (Welford's streaming algorithm) per node, per feature, computing a Z-score. Requires **zero pretrained dataset** — calibrates on-site after deployment.

- **Calibration**: run across multiple sessions spanning different times of day/temperature — a single 30–60 minute window will miscalibrate against real thermal drift.
- **Freeze**: sustained `|Z|` above threshold over several cycles (not one noisy sample) freezes baseline updates, preventing the system from learning a developing event as "normal."
- **Unfreeze**: only after readings stabilize for a sustained window, then a **new** baseline is recalibrated from the post-event stable readings (never silently resumes the old one) — or a manual reset from the dashboard.

### Layer 2 — PIM-trained spatial-shape classifier (secondary, physics-informed)
A small feedforward network (BPNN-class, matching architectures in peer-reviewed subsidence literature), trained offline on synthetic ground truth from the Probability Integral Method, answering: *does this N1/N2/N3 spatial pattern resemble a real subsidence bowl, or random noise?*

### Rule engine (Command Centre)
Consumes both layers' scores plus spatial coherence to assign `deformation_state` and `risk_class`. Key safeguard: `ACCELERATING`/`RAPID` require high spatial coherence — a lone spike routes to sensor-fault handling instead.

### Deformation states
`STABLE → INITIATION → PROGRESSIVE → ACCELERATING → RAPID → STABILIZING`
(`PROGRESSIVE`/`ACCELERATING` boundary uses hysteresis on acceleration sign to avoid flicker on noisy MPU6050 data.)

### Risk classes
`NORMAL → WATCH → WARNING → CRITICAL` (prototype bands, not DGMS statutory limits — calibrate against your own testbed runs.)

---

## 8. Data & Training Strategy

No dataset exists matching this PS's exact modality (low-cost IMU/potentiometer sensors, India-specific underground coal geology, LoRa-network sampling rate). Strategy:

### Tier 1 — Physics-informed synthetic ground truth
Using the **Probability Integral Method (PIM)** — an official, widely-used mining-subsidence prediction model, with Indian precedent (P.R. Sheorey, CMRI/CIMFR):

```
Maximum subsidence:  W0 = mc x q x cos(alpha)
Subsidence profile:  S-shaped curve via error function, r = H / tan(beta)
Tilt at a point:     local slope (1st derivative) of the subsidence profile
Time dynamics:       Knothe time function (subsidence velocity over time)
```

Script: `generate_synthetic_data.py` — generates multiple parameterized "virtual mines," samples the resulting field at each node's position, and outputs a labeled CSV plus sanity-check plots.

### Tier 2 — Sensor-realistic noise injection
Clean PIM signal corrupted with noise magnitudes drawn from actual component datasheets (placeholder values in the current script — replace with your MPU6050/potentiometer datasheet numbers before final submission).

### Tier 3 — Physical testbed validation
Small number of real screw-actuator runs, cross-checked against an independent displacement reference (dial gauge/vernier) — confirms the hardware pipeline works and that assumed noise roughly matches real behavior. This tier validates the pipeline; it is not the primary training source.

**Jury Q&A ready answers:**
- *"Why not real InSAR data?"* — InSAR operates at satellite revisit cadence (days–weeks) over km-scale pixels; this network operates at second-scale resolution over meter spacing.
- *"How do you know the synthetic data resembles reality?"* — PIM parameters are taken from published, measured Indian and Chinese case studies, not invented; qualitative curve shape is checked against those published profiles.

---

## 9. Software Architecture

### Online mode
```
ESP32 Command Centre --Wi-Fi HTTP POST--> FastAPI (Python) --> SQLite DB
      --> REST/WebSocket --> React+TypeScript frontend
      --> schematic GIS-style map (image overlay of testbed, NOT satellite tiles)
```

### Offline mode
```
ESP32 Command Centre --USB Serial (permanently tethered)--> Node-RED (same laptop)
      --> node-red-dashboard: live gauges, time-series charts, risk banner
```

**Design decision on record:** offline and online data paths are **independent by design**, not synced — this is a deliberate scoping choice against the PS's literal "offline capability with periodic cloud synchronization" wording. Documented here so it's explained, not discovered.

---

## 10. Alert System

| Trigger | Channel | Works offline? | Notes |
|---|---|---|---|
| risk_class = CRITICAL | SIM800L SMS | Yes (cellular) | Airtel/Vi/BSNL SIM only |
| risk_class = CRITICAL | SIM800L voice call | Yes | Rings only, no spoken message |
| Any mode | On-screen dashboard banner | Yes (both dashboards) | Always active |

---

## 11. Complete Technology Stack

| Layer | Technology |
|---|---|
| Node MCU (×3) | ESP32-S3-DevKitC-1 (N8R8/N16R8) |
| Command Centre MCU | ESP32-WROOM-32 |
| Displacement sensing | WH20-1-0 linear potentiometers (×2/node) |
| Tilt + vibration | MPU6050 (clone) |
| Temperature | DS18B20 |
| Local display | SSD1306 OLED |
| Radio link | SX1278/Ra-02 LoRa, 433MHz |
| Cellular alerting | SIM800L |
| Firmware language | C/C++ (Arduino framework) |
| On-device inference | TensorFlow Lite Micro (int8 quantized) |
| Online backend | FastAPI (Python) |
| Database | SQLite |
| Online frontend | React + TypeScript |
| GIS | Leaflet, schematic image-overlay |
| Offline dashboard | Node-RED + node-red-dashboard |
| Data science / training | Python, NumPy, SciPy, pandas, scikit-learn/Keras |

---

## 12. Known Limitations (stated for jury Q&A, not left to be discovered)

- Scaled physical testbed, not a geomechanical replica of an underground coal mine.
- No official/field dataset exists for PS 26025; mine/geological parameters are published-literature values, clearly tagged, not measured by this hardware.
- Vibration, tilt, and dominant frequency all derive from the same MPU6050 chip — documented, not presented as independent modalities.
- Model outputs short-horizon trend/state classification only, not absolute multi-hour forecasts.
- Offline and online data paths are independent, not synced.
- GIS view is a schematic image overlay, not satellite-grade GIS.
- SIM800L requires a 2G-capable SIM (Airtel/Vi/BSNL); Jio SIMs will not work.
- Self-calibrating baseline and mesh networking are converged-upon approaches across teams addressing this PS, not unique to this solution — real differentiation rests on spatial fusion, explainable risk output, and the physics-grounded data strategy.

---

## 13. Future Upgrades

| Upgrade | What it adds |
|---|---|
| Independent geophone (SM-24) per node | True independent vibration channel |
| Piezometer / pore-water-pressure sensing | Real leading indicator currently unmeasured |
| GNSS-RTK reference stations | ±1–2cm real-time field validation |
| InSAR/SBAS integration | km-scale mm-precision context for a real mine |
| 4G/LTE module (SIM7600) vs 2G SIM800L | Removes dependency on shrinking 2G coverage |
| True multi-hop mesh relay | Matches PS's mesh terminology literally |
| Offline→online data sync | Fully satisfies PS's offline+sync requirement |
| Solar + battery autonomy per node | Field-deployable without external power |
| Field validation with a mine/research institution | Real ground-truth validation vs. survey benchmarks |

---

## 14. Repository Structure

```
/
├── README.md                              (this file)
├── firmware/
│   ├── sense_mine_node.ino                (sensor node bring-up firmware)
│   └── sense_mine_command_centre.ino      (Command Centre bring-up firmware)
├── data/
│   ├── generate_synthetic_data.py         (PIM+Knothe synthetic dataset generator)
│   ├── synthetic_subsidence_dataset.csv   (generated output)
│   └── synthetic_scenarios_preview.png    (sanity-check plots)
└── docs/
    └── SENSE-MINE_Final_Presentation_Architecture.pdf
```

---

## 15. Team

**Problem Statement:** SIH26025 — Ministry of Coal / Coal India Limited

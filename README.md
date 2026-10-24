# SENSE-MINE

**AI-Enabled Wireless Surface Mesh Network for Real-Time Mine Subsidence Monitoring, Prediction and Early Warning**

Smart India Hackathon 2026 — Problem Statement **SIH26025**
Organization: Ministry of Coal | Department: Coal India Limited

<img width="1536" height="1024" alt="sense-mine png" src="https://github.com/user-attachments/assets/3461f1cd-4020-4db0-94a9-2c0591b4005c" />


## Overview

SENSE-MINE is a low-cost, six-node distributed surface-deformation sensing network deployed over an underground coal panel. Nodes communicate over a **self-healing multi-hop LoRa mesh** to a command centre, which runs local safety logic and an independent GSM alert path, while a connected operator PC hosts the AI pipeline, database, and both an offline and online dashboard.

The system does **not** claim to replace conventional surveying, GNSS, or InSAR. It is positioned as a **dense, continuous, local surface-sensing layer** that complements them — providing high-frequency local deformation observations and immediate local alerts between periodic survey campaigns.

📄 Full technical detail lives in [`docs/SENSE-MINE_Detailed_Project_Report.docx`](SENSE-MINE_Detailed_Project_Report.pdf) — this README is the quick-reference summary.

---

## Problem Statement

> Development of an AI-enabled Low Cost Real Time Mine Subsidence Monitoring, Prediction and Early Warning System for Underground Coal Mines in India.

The PS calls for: continuous tilt/relative-distance/crack/vibration monitoring, AI-based anomaly detection and prediction, GIS risk-zone visualization, alerting, offline operation with sync, and scalability.

---

## Key Features

- 🛰️ **multi-nodes self-healing LoRa mesh** (433 MHz) with neighbour discovery, routing, ACK/retry, and automatic route recovery on node failure.
- 📏 **Dual displacement sensing per node** — crack opening + vertical displacement, via individually calibrated low-cost potentiometric transducers.
- 📐 **Tilt + vibration** from MPU6050 (gravity-vector tilt, complementary/Kalman-filtered; on-node RMS/peak/dominant-frequency feature extraction for vibration).
- 🤖 **Three-layer explainable AI pipeline** — Isolation Forest (anomaly detection) → Random Forest (deformation-state classification) → XGBoost (short-horizon forecasting).
- 🧭 **Spatial fusion engine** — distinguishes a genuinely coherent multi-node deformation event from an isolated faulty sensor before any alert is raised.
- 🚨 **Independent, redundant alerting** — local OLED/buzzer, cellular SIM800L SMS + voice call (works without internet), and online dashboard push.
- 🗺️ **GIS risk visualization** — Leaflet + IDW-interpolated risk surface over real GPS-registered node locations.
- 🔌 **Offline-first** — Node-RED local dashboard operates independently of the Python/cloud backend; internet loss never disables core monitoring or alerting.
- ☀️ **Solar + battery powered nodes**, sized against a calculated per-node energy budget rather than an arbitrary panel size.

---

## System Architecture

```
                  SURFACE OVER UNDERGROUND PANEL
 +----------------------------------------------------------+
 |  N1 <----------> N2 <----------> N3                        |
 |  ^                ^                ^                       |
 |  v                v                v                       |
 |  N6 <----------> N5 <----------> N4                        |
 +---------------------------+--------------------------------+
                             |
                        LoRa MESH (multi-hop / ACK / retry)
                             v
                  +-----------------------+
                  | ESP32 COMMAND CENTRE  |
                  | mesh sink + node      |
                  | health + safety logic |
                  +-----------+-----------+
                              |
                +-------------+-------------+
                |                           |
             SIM800L                   USB / Serial
           SMS + VOICE                       |
                                     Python Backend (PC)
                                              |
                              PostgreSQL / TimescaleDB
                                              |
                              +---------------+---------------+
                          Node-RED (offline)          Web Dashboard (online)
                              |                               |
                             GIS                       AI / History
```

Full block diagrams, firmware task pipeline, and calibration protocols are in the [Detailed Project Report](SENSE-MINE_Detailed_Project_Report.pdf).

---

## Hardware (per sensor node × 6)

| Component | Role |
|---|---|
| ESP32-S3 DevKitC-1 | Node controller |
| WH20 linear potentiometer ×2 | Crack opening + vertical displacement |
| MPU6050 | Tilt (X/Y) + vibration |
| DS18B20 | Temperature (drift compensation) |
| GPS/GNSS module | Installation-time location registration only |
| SX1278 / RA-02 (433 MHz) | LoRa mesh radio |
| Solar panel + Li-ion battery | Autonomous power |
| OLED + LED + buzzer | Local status interface |

**Command centre:** ESP32-S3 + SX1278 + SIM800L (independent SMS/voice alert channel, requires an Airtel/Vi/BSNL SIM — Jio has no 2G) + dedicated GSM power regulator, permanently USB-tethered to the operator PC.

> ⚠️ Potentiometric transducers here are **low-cost transducers with calibrated mechanical linkage**, not survey-grade geotechnical instruments. Accuracy is established per node via calibration (see docs), not assumed.

---

## AI Pipeline

```
Sensor data → Calibration → Feature extraction
        │
   ┌────┴────┐
   ▼         ▼
Isolation   Random Forest
 Forest    (deformation state:
(anomaly    STABLE → INITIATION → PROGRESSIVE
 score)     → ACCELERATING → RAPID → STABILIZING)
   │         │
   └────┬────┘
        ▼
 Spatial fusion (gradient + neighbour coherence)
        ▼
 XGBoost short-horizon forecast (10 min / 30 min / 1h)
        ▼
     Risk Engine (rules, not ML alone)
        ▼
  GREEN / YELLOW / ORANGE / RED
```

The risk engine — not the ML models directly — decides whether to escalate, cross-checking sensor health and multi-node spatial agreement first. This prevents a single noisy sensor from triggering a false panel-wide alert.

---

## Tech Stack

| Layer | Technology |
|---|---|
| Node firmware | ESP32-S3 (Arduino / ESP-IDF), FreeRTOS |
| Mesh radio | LoRa (SX1278/RA-02), custom HELLO/ROUTE/DATA/ACK/RETRY/HEARTBEAT protocol |
| Command centre firmware | ESP32-S3, packet management, local safety logic |
| Backend | Python, FastAPI |
| Database | PostgreSQL + TimescaleDB |
| Offline dashboard | Node-RED + node-red-dashboard |
| Online dashboard | React + TypeScript |
| GIS | Leaflet + OpenStreetMap, IDW interpolation |
| AI/ML | scikit-learn (Isolation Forest, Random Forest), XGBoost |
| Alerts | SIM800L (SMS + voice call) |

----

## Risk Levels

| Level | Meaning |
|---|---|
| 🟢 GREEN | Normal |
| 🟡 YELLOW | Early abnormality, single-node/low confidence |
| 🟠 ORANGE | Persistent, spatially coherent deformation |
| 🔴 RED | Rapid/accelerating deformation, multi-node confirmed |

---

## Known Limitations

- Scaled prototype/testbed validation, not a geomechanical replica of an underground mine.
- No real mine field dataset used for AI training at this stage — clearly-labeled synthetic and testbed data only.
- Short-horizon (10 min–1h) trend forecasting only; no multi-hour/day-scale absolute forecast claimed.
- GIS is a Leaflet overlay with surveyed local coordinates, not satellite-grade GIS; the IDW risk surface is an estimate between sensor points, not a measurement everywhere.
- SIM800L requires a 2G-capable SIM (Airtel/Vi/BSNL) — not compatible with Jio.
- No OTA model deployment for this build; model updates are validated and flashed via USB.

Full list with rationale: see §19 of the [Detailed Project Report](SENSE-MINE_Detailed_Project_Report.pdf).

---

## Roadmap

- [ ] Phase 1 — One production-quality node validated continuously
- [ ] Phase 2 — Replicate to all 6 nodes
- [ ] Phase 3 — Mesh protocol + route-recovery test
- [ ] Phase 4 — Per-node sensor calibration
- [ ] Phase 5 — Command-centre firmware + SIM800L integration
- [ ] Phase 6 — Python backend + PostgreSQL/TimescaleDB
- [ ] Phase 7 — AI layer (Isolation Forest + Random Forest + XGBoost)
- [ ] Phase 8 — Risk engine (AI + spatial coherence + sensor health)
- [ ] Phase 9 — Node-RED offline dashboard
- [ ] Phase 10 — Online web dashboard
- [ ] Phase 11 — GIS integration
- [ ] Phase 12 — 24–72h endurance test

Future upgrades (independent geophone, GNSS-RTK, InSAR/SBAS integration, 4G/LTE module, OTA deployment, field validation with a mine/research institution) are tracked in the DPR §20.

---
## References

1. SIH 2026 — Problem Statement SIH26025, Ministry of Coal / Coal India Limited
2. Underground coal mine monitoring with wireless sensor networks — HKUST Research Portal
3. Integrated high-precision surface subsidence monitoring using D-InSAR, SBAS, and UAV — *Scientific Reports* (Nature)
4. Land surface deformation monitoring by microwave SAR data analysis in the coal mines of Chhattisgarh — ISTI Portal
5. Monitoring 3D surface deformation in mining areas combining SBAS-InSAR, GNSS and the Probability Integral Method — PMC
6. Application of wireless sensor networks for environmental monitoring in underground coal mines: A systematic review — ScienceDirect
7. DGMS Annual Report 2024, Directorate General of Mines Safety

---
## Team

**Team-Nirvana**

**Institution:** Guru Gobind Singh Indraprastha University, Delhi — University School of Automation & Robotics (USAR)
**Institute Code (AISHE):** U-0099
**Event:** Smart India Hackathon 2026 Nomination

| Name | Position | Branch | Batch | Role |
|---|---|---|---|---|
| Paras Bhatia | Team Leader | B.Tech IIOT | 2024 | Backend Dvelopement |
| Lakshya | Team Member | B.Tech IIOT | 2024 | Embedded and IoT Developement |
| Aditya | Team Member | B.Tech AIML | 2024 | Edge AI Developement |
| Navneet | Team Member | B.Tech IIOT | 2024 | Frontend Developement |
| Kundan | Team Member | B.Tech IIOT | 2024 | Research And Developement |
| Deepti | Team Member | B.Tech AIML | 2026 | Research And Developement |

**Mentor:** Dr. Khyati Chopra Assistant Professor GGSIPU, USAR

**Problem Statement:** SIH26025 — Ministry of Coal / Coal India Limited

## Folder Structure 

```text
SENSE-MINE/
│
├── sennse_mine_node_final/
│   ├── sense_mine_node_final.ino      //Final Sensor Node-code
│   ├── secrets.h
│   ├── model_rf_compact.cpp
│   ├── model_rf_compact.h
│   └── preprocessing_rf.h
│
├── README.md
├── SENSE-MINE_Detailed_Project_Report
├── sennse_mine_command_centre_final003.ino   //Final Command Centre Code
├── sennse_mine_command_centre_test.ino       // Test Code 
└── sennse_mine_node_test.ino                 // Test Code 
```

## CURRENT VERSION 
/* ============================================================
   SENSE-MINE  |  SENSOR NODE — FINAL FIRMWARE (edge AI) fixed code not update it 
   Board   : ESP32-S3-DevKitC-1
   Purpose : Read all sensors, assemble the 23-feature vector,
             run the on-device Random Forest, and transmit the
             classification + raw features to the Command Centre
             over LoRa. OLED shows live debug pages.
   ============================================================

   ARCHITECTURE DECISION (per your instructions):
   - Single-node deployment for now. spatial_displacement_gradient
     and spatial_coherence genuinely need >=2 nodes to compute -
     with only one node, they are HARDCODED placeholders below.
     Swap in the real multi-node calculation when you add nodes.
   - Inference runs HERE, on the node (ESP32-S3), matching the
     board the model was exported/sized for.

   FROZEN PIN MAP (unchanged from hardware-test build)
   ----------------------------------------------------------
   GPIO 1  : ADC  - Crack-width potentiometer (wiper)
   GPIO 2  : ADC  - Vertical-displacement potentiometer (wiper)
   GPIO 4  : 1-Wire - DS18B20 temperature
   GPIO 8  : I2C SDA - MPU6050 (clone) + OLED (shared bus)
   GPIO 9  : I2C SCL - MPU6050 (clone) + OLED (shared bus)
   GPIO 10 : SPI NSS  - SX1278 LoRa
   GPIO 11 : SPI MOSI - SX1278 LoRa
   GPIO 12 : SPI SCK  - SX1278 LoRa
   GPIO 13 : SPI MISO - SX1278 LoRa
   GPIO 14 : LoRa RESET
   GPIO 15 : LoRa DIO0

   FILES THAT MUST SIT IN THIS SAME SKETCH FOLDER
   ----------------------------------------------------------
   - model_rf_compact.cpp   <-- use the FIXED version I generated,
                                 not your original export. Your
                                 original file has ~36,889 malformed
                                 float literals ("0f" instead of
                                 "0.0f") from the export script,
                                 which will not compile. The fixed
                                 version only changes those literal
                                 tokens - no structural/tree changes.
   - model_rf_compact.h     <-- unchanged from your export
   - preprocessing_rf.h     <-- FIXED version (same "0f" bug, one
                                 instance, on the acceleration
                                 feature's median value)

   DO NOT add feature_config_compact.h to this sketch folder.
   It declares its own `rf_compact::kClassNames`, which conflicts
   with the one already defined in model_rf_compact.h/.cpp -
   including both in the same build will not compile. This
   firmware gets class names from model_rf_compact.h only, and
   defines its own small local feature-name list (below) purely
   for Serial debug printing.

   ARDUINO IDE SETTINGS REQUIRED
   ----------------------------------------------------------
   The compact model is ~1MB of flash-resident data. In
   Tools > Partition Scheme, pick a scheme with a large enough
   app partition - e.g. "Huge APP (3MB No OTA/1MB SPIFFS)" - and
   make sure your board variant actually has >=4MB flash (8MB/16MB
   preferred). The default/minimal partition scheme WILL overflow.

   LIBRARIES REQUIRED (Library Manager)
   ----------------------------------------------------------
   - LoRa by sandeepmistry
   - OneWire
   - DallasTemperature
   - Adafruit GFX Library
   - Adafruit SSD1306
   MPU6050 uses NO library - raw I2C register driver (clone chip).
   ============================================================ */


   /* ============================================================
   SENSE-MINE  |  COMMAND CENTRE — PHASE-1 PROTOTYPE FIRMWARE
   Board   : ESP32 DevKitC / WROOM-32
   Purpose : Receive each node's classification + raw features
             over LoRa, and fan the data out over TWO INDEPENDENT
             paths (neither one waits on the other):

   DATA PATH (updated — supersedes the older "Node-RED owns
   Firestore" comment from the previous revision of this file)
   ----------------------------------------------------------
   Sensor Node --LoRa--> Command Centre
                             |
                             +--USB Serial (JSON, always)--> Node-RED (local dashboard)
                             |
                             +--WiFi/HTTPS (best-effort)-----> Firestore directly --> Website

   - The USB-Serial -> Node-RED path is UNCONDITIONAL. It never
     waits on WiFi, Firestore auth, or anything network-related.
     If WiFi/Firebase is down, this path is completely unaffected.
   - The WiFi -> Firestore path is BEST-EFFORT. If there's no
     WiFi, or Firebase auth hasn't completed yet, the push is
     silently skipped (logged to Serial only) and the loop moves
     on immediately — it never blocks LoRa reception.
   - This is a genuine phase-1 prototype simplification vs. the
     DPR's final architecture (FastAPI + PostgreSQL/TimescaleDB).
     Firestore here plays the role that Postgres plays in the
     final design. Swap it out later without touching the node
     firmware or the LoRa protocol.

   FIREBASE AUTH MODEL (Option B — dedicated device account)
   ----------------------------------------------------------
   This board signs in to Firebase Auth with a FIXED email/
   password created just for it (not a real mailbox — just a
   fixed identity string). Firestore rules then check
   request.auth.token.email == that fixed address, so only this
   board (or anything else holding those credentials) can write.
   Anonymous website visitors get their own anonymous auth
   session (see the frontend prompt) which has auth != null but
   NO email claim, so the rule correctly lets them read but not
   write. See the "Firestore rules" note delivered alongside this
   file for the exact rules text to paste into the Firebase
   console.

   BEFORE FLASHING — create the device account once:
     Firebase Console -> Authentication -> Sign-in method
       -> enable "Email/Password"  AND  enable "Anonymous"
     Firebase Console -> Authentication -> Users -> Add user
       -> email:    (same as DEVICE_EMAIL below)
       -> password: (same as DEVICE_PASSWORD below)

   INVALID-READING HANDLING (the actual bug this revision fixes)
   ----------------------------------------------------------
   The node firmware can legitimately send NaN for a feature
   (e.g. tilt_x/tilt_y when the MPU read fails, temperature_C
   when the DS18B20 disconnects). Arduino's float printing turns
   NaN into the literal text "nan", which is NOT valid JSON —
   that is what was making Node-RED's JSON.parse() throw in
   debug. Every numeric value this board emits (over BOTH the
   USB path and the Firestore path) now goes through jsonNum(),
   which converts NaN/Inf into a proper JSON `null` instead. A
   top-level "data_quality" field ("OK" / "PARTIAL") is also
   added so Node-RED/the website can flag a reading without the
   parse itself ever failing.

   FROZEN PIN MAP (unchanged from hardware-test build)
   ----------------------------------------------------------
   GPIO 18 : SPI SCK   - SX1278 LoRa (VSPI)
   GPIO 19 : SPI MISO  - SX1278 LoRa (VSPI)
   GPIO 23 : SPI MOSI  - SX1278 LoRa (VSPI)
   GPIO 5  : SPI NSS   - SX1278 LoRa
   GPIO 14 : LoRa RESET
   GPIO 26 : LoRa DIO0
   GPIO 21 : I2C SDA   - OLED
   GPIO 22 : I2C SCL   - OLED

   - WiFiClientSecure below uses setInsecure() (no certificate
     pinning) to keep this phase-1 build simple. That's an
     accepted, stated shortcut for the prototype — pin Google's
     root CA (or use the ESP32 core's cert bundle) before this
     goes anywhere near a real deployment.

   LIBRARIES REQUIRED (Library Manager + built-in ESP32 core)
   ----------------------------------------------------------
   - LoRa by sandeepmistry
   - Adafruit GFX Library
   - Adafruit SSD1306
   - WiFi, WiFiClientSecure, HTTPClient  (bundled with ESP32 core)
   ============================================================ */

#include <SPI.h>
#include <LoRa.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <time.h>

// ---------------- PIN MAP ----------------
#define PIN_LORA_SCK   18
#define PIN_LORA_MISO  19
#define PIN_LORA_MOSI  23
#define PIN_LORA_NSS   5
#define PIN_LORA_RST   14
#define PIN_LORA_DIO0  26

#define PIN_I2C_SDA    21
#define PIN_I2C_SCL    22

#define LORA_FREQ      433E6

// ======================================================================
// TODO: EDIT THESE FOUR BLOCKS BEFORE FLASHING
// ======================================================================
// 1) Your WiFi (best-effort — the USB->Node-RED path works with none of this filled in)
#define WIFI_SSID            "WIFI NAME HERE"
#define WIFI_PASSWORD         "WIFI PASSWORD HERE"

// 2) From your firebaseConfig (apiKey + projectId) — already filled in from what you shared
#define FIREBASE_API_KEY     "FIREBASE_API_KEY"
#define FIREBASE_PROJECT_ID  "FIREBASE_PROJECT_ID"

// 3) The fixed device account you create once in the Firebase console (Option B)
#define DEVICE_EMAIL         "Device Email "
#define DEVICE_PASSWORD      "Device Password"

// 4) GIS — no GPS hardware yet, so each node's install location is
//    hardcoded here instead. IMPORTANT: this is set to the geographic
//    centroid of the Pachhwara North coal block, Pakur District,
//    Jharkhand (bounding box 24°32'12.771"N-24°33'53.683"N,
//    87°25'18.208"E-87°29'55.417"E, centroid computed below) - a real,
//    named Coal India-linked site, used here as a REFERENCE/TARGET
//    deployment site for the demo narrative.
//    This is NOT where the physical testbed actually sits (that's your
//    lab/college). "location_source" and "site_label" below both say
//    so explicitly, so nothing downstream - Node-RED, the website, or
//    a jury member reading the raw Firestore data - can mistake this
//    for a real GPS fix from hardware physically installed at
//    Pachhwara North. If you ever want the pin to instead show where
//    the testbed itself sits, swap in real coordinates for that and
//    change location_source/site_label accordingly.

   

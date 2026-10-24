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

COMPLETE FILE/FOLDER STRUCTURE 
    
  └── sennse_mine_node_final/
      ├── sense_mine_node_final.ino                
      ├── secrets.h
      ├── model_rf_compact.cpp
      ├── model_rf_compact.h
      └── preprocessing_rf.h
  └── READEME.md
  └── SENSE-MINE_Detailed_Project_Report
  └── sennse_mine_command_centre_final003.ino       
  └── sennse_mine_command_centre_test.ino
  └── sennse_mine_node_test.ino

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

#include <Wire.h>
#include <SPI.h>
#include <LoRa.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#include "model_rf_compact.h"
#include "preprocessing_rf.h"

// MPU DATA STRUCTURE
struct MpuData { float ax, ay, az, gx, gy, gz, roll, pitch; };

// ---------------- PIN MAP ----------------
#define PIN_POT_CRACK      1
#define PIN_POT_VERT       2
#define PIN_DS18B20        4
#define PIN_I2C_SDA        8
#define PIN_I2C_SCL        9
#define PIN_LORA_NSS       10
#define PIN_LORA_MOSI      11
#define PIN_LORA_SCK       12
#define PIN_LORA_MISO      13
#define PIN_LORA_RST       14
#define PIN_LORA_DIO0      15

#define LORA_FREQ          433E6
#define NODE_ID            1        // <-- change per physical node

// ---------------- MPU6050 bare register map (clone-tolerant) ----------------
#define MPU_ADDR           0x68
#define REG_PWR_MGMT_1     0x6B
#define REG_WHO_AM_I       0x75
#define REG_ACCEL_CONFIG   0x1C
#define REG_GYRO_CONFIG    0x1B
#define REG_ACCEL_XOUT_H   0x3B

// ---------------- OLED ----------------
#define OLED_W 128
#define OLED_H 64
Adafruit_SSD1306 oled(OLED_W, OLED_H, &Wire, -1);

// ---------------- DS18B20 ----------------
OneWire oneWire(PIN_DS18B20);
DallasTemperature ds18b20(&oneWire);

// ---------------- FIXED SITE / PANEL CONFIG (features 17-23) ----------------
const float mining_depth_m          = 584.0785429f;
const float seam_thickness_m        = 3.527589707f;
const float mining_height_m         = 3.321241755f;
const float panel_width_m           = 284.2377001f;
const float panel_length_m          = 1420.158918f;
const float face_advance_rate_m_day = 6.02314398f;
const float extraction_ratio        = 0.940441635f;

// ---------------- HARDCODED spatial placeholders (features 15-16) ----------------
// Single-node deployment: there is no second node to compute a real
// spatial gradient/coherence against. 0.0 gradient ("no measurable
// difference from neighbours") and 1.0 coherence ("perfectly self-
// consistent") are the most defensible stand-ins until a second node
// is online. Replace with the real multi-node calculation then.
const float SPATIAL_GRADIENT_PLACEHOLDER  = 0.0f;
const float SPATIAL_COHERENCE_PLACEHOLDER = 1.0f;

// names for Serial debug only - kept local so we don't need
// feature_config_compact.h (see conflict note above)
static const char* kFeatureNamesLocal[rf_compact::kNumFeatures] = {
  "crack_width_mm", "vertical_displacement_mm", "tilt_x_deg", "tilt_y_deg",
  "vibration_rms_g", "vibration_peak_g", "dominant_frequency_hz", "temperature_C",
  "crack_growth_rate_mm_day", "vertical_displacement_velocity_mm_day",
  "displacement_acceleration_mm_day2", "tilt_rate_deg_day",
  "cumulative_crack_growth_mm", "cumulative_subsidence_mm",
  "spatial_displacement_gradient", "spatial_coherence",
  "mining_depth_m", "seam_thickness_m", "mining_height_m",
  "panel_width_m", "panel_length_m", "face_advance_rate_m_day", "extraction_ratio"
};

// ---------------- live + derived readings ----------------
struct Readings {
  float crack_width_mm;
  float vertical_displacement_mm;
  float tilt_x_deg;
  float tilt_y_deg;
  float vibration_rms_g;
  float vibration_peak_g;
  float dominant_frequency_hz;   // approximate - zero-crossing based, no FFT
  float temperature_C;

  float crack_growth_rate_mm_day;
  float vertical_displacement_velocity_mm_day;
  float displacement_acceleration_mm_day2;
  float tilt_rate_deg_day;
  float cumulative_crack_growth_mm;
  float cumulative_subsidence_mm;
} R;

float prev_crack = NAN, prev_disp = NAN, prev_vel = NAN, prev_tilt_mag = NAN;
unsigned long prev_t = 0;

float baseline_crack = NAN, baseline_disp = NAN;
bool  baselineSet = false;

#define VIB_SAMPLES 50
float vibBuf[VIB_SAMPLES];

// ---------------- AI inference result (updated once per loop) ----------------
float raw_features[rf_compact::kNumFeatures];
float normalized_features[rf_compact::kNumFeatures];
uint16_t votes[rf_compact::kNumClasses];
uint8_t g_predictedClass = 0;
uint16_t g_maxVotes = 0;
float g_confidence = 0;

// ---------------- OLED circular page cycling ----------------
#define NUM_PAGES 5
uint8_t currentPage = 0;
unsigned long lastPageSwitch = 0;
const unsigned long PAGE_INTERVAL_MS = 3000;

bool mpuOK = false, dsOK = false, loraOK = false, oledOK = false;
unsigned long lastTxTime = 0;
uint32_t packetCounter = 0;

// ================= MPU6050 BARE I2C DRIVER (no library) =================
void mpuWrite(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(reg);
  Wire.write(val);
  Wire.endTransmission();
}

uint8_t mpuReadByte(uint8_t reg) {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(reg);
  Wire.endTransmission(false);
  Wire.requestFrom((int)MPU_ADDR, 1);
  return Wire.available() ? Wire.read() : 0xFF;
}

bool mpuInit() {
  Wire.beginTransmission(MPU_ADDR);
  if (Wire.endTransmission() != 0) return false;

  uint8_t whoami = mpuReadByte(REG_WHO_AM_I);
  mpuWrite(REG_PWR_MGMT_1, 0x00);
  delay(50);
  mpuWrite(REG_ACCEL_CONFIG, 0x00);  // +-2g
  mpuWrite(REG_GYRO_CONFIG, 0x00);   // +-250 dps
  delay(20);
  Serial.printf("[MPU] WHO_AM_I = 0x%02X (clone-tolerant init, not hard-checked)\n", whoami);
  return true;
}

// struct MpuData { float ax, ay, az, gx, gy, gz, roll, pitch; };

bool mpuRead(MpuData &d) {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(REG_ACCEL_XOUT_H);
  if (Wire.endTransmission(false) != 0) return false;
  Wire.requestFrom((int)MPU_ADDR, 14);
  if (Wire.available() < 14) return false;

  int16_t axr = (Wire.read() << 8) | Wire.read();
  int16_t ayr = (Wire.read() << 8) | Wire.read();
  int16_t azr = (Wire.read() << 8) | Wire.read();
  Wire.read(); Wire.read();
  int16_t gxr = (Wire.read() << 8) | Wire.read();
  int16_t gyr = (Wire.read() << 8) | Wire.read();
  int16_t gzr = (Wire.read() << 8) | Wire.read();

  d.ax = axr / 16384.0f;
  d.ay = ayr / 16384.0f;
  d.az = azr / 16384.0f;
  d.gx = gxr / 131.0f;
  d.gy = gyr / 131.0f;
  d.gz = gzr / 131.0f;

  d.roll  = atan2(d.ay, d.az) * 180.0 / PI;
  d.pitch = atan2(-d.ax, sqrt(d.ay * d.ay + d.az * d.az)) * 180.0 / PI;
  return true;
}

void sampleVibration(float &rms, float &peak, float &domFreqHz) {
  MpuData d;
  int zeroCrossings = 0;
  float prevMag = 0;
  float sumSq = 0;
  peak = 0;
  unsigned long t0 = millis();

  for (int i = 0; i < VIB_SAMPLES; i++) {
    if (mpuRead(d)) {
      float mag = sqrt(d.ax * d.ax + d.ay * d.ay + d.az * d.az) - 1.0f;
      vibBuf[i] = mag;
      sumSq += mag * mag;
      if (fabs(mag) > peak) peak = fabs(mag);
      if (i > 0 && ((mag > 0 && prevMag <= 0) || (mag < 0 && prevMag >= 0))) zeroCrossings++;
      prevMag = mag;
    } else {
      vibBuf[i] = 0;
    }
    delay(4);
  }
  unsigned long dt = millis() - t0;
  rms = sqrt(sumSq / VIB_SAMPLES);
  domFreqHz = (dt > 0) ? (zeroCrossings / 2.0f) / (dt / 1000.0f) : 0;
}

float adcToCrackMM(int raw)        { return (raw / 4095.0f) * 50.0f; }
float adcToDisplacementMM(int raw) { return (raw / 4095.0f) * 100.0f; }

// ================= AI INFERENCE =================
void buildFeatureVector() {
  raw_features[0]  = R.crack_width_mm;
  raw_features[1]  = R.vertical_displacement_mm;
  raw_features[2]  = R.tilt_x_deg;
  raw_features[3]  = R.tilt_y_deg;
  raw_features[4]  = R.vibration_rms_g;
  raw_features[5]  = R.vibration_peak_g;
  raw_features[6]  = R.dominant_frequency_hz;
  raw_features[7]  = R.temperature_C;
  raw_features[8]  = R.crack_growth_rate_mm_day;
  raw_features[9]  = R.vertical_displacement_velocity_mm_day;
  raw_features[10] = R.displacement_acceleration_mm_day2;
  raw_features[11] = R.tilt_rate_deg_day;
  raw_features[12] = R.cumulative_crack_growth_mm;
  raw_features[13] = R.cumulative_subsidence_mm;
  raw_features[14] = SPATIAL_GRADIENT_PLACEHOLDER;
  raw_features[15] = SPATIAL_COHERENCE_PLACEHOLDER;
  raw_features[16] = mining_depth_m;
  raw_features[17] = seam_thickness_m;
  raw_features[18] = mining_height_m;
  raw_features[19] = panel_width_m;
  raw_features[20] = panel_length_m;
  raw_features[21] = face_advance_rate_m_day;
  raw_features[22] = extraction_ratio;
  // NOTE: any NAN here (e.g. before baseline/derived values exist yet, or a
  // failed sensor read) is handled automatically - rf_compact::preprocess()
  // performs the model's training-time median imputation on NaN inputs.
}

void runInference() {
  buildFeatureVector();
  rf_compact::preprocess(raw_features, normalized_features);
  rf_compact::predict(normalized_features, &g_predictedClass, votes);

  g_maxVotes = 0;
  for (uint32_t i = 0; i < rf_compact::kNumClasses; i++) {
    if (votes[i] > g_maxVotes) g_maxVotes = votes[i];
  }
  g_confidence = (float)g_maxVotes / (float)rf_compact::kNumTrees;
}

// ================= OLED PAGES =================
void drawHeader(const char* title) {
  oled.setTextSize(1);
  oled.setCursor(0, 0);
  oled.printf("N%d %s", NODE_ID, title);
  oled.drawLine(0, 10, 128, 10, SSD1306_WHITE);
}

void page1_displacement() {
  drawHeader("DISPLACEMENT");
  oled.setCursor(0, 16);
  oled.printf("Crack : %.2f mm\n", R.crack_width_mm);
  oled.printf("Vert. : %.2f mm\n", R.vertical_displacement_mm);
  oled.printf("Cr.gr : %.3f mm/d\n", R.crack_growth_rate_mm_day);
  oled.printf("V.vel : %.3f mm/d\n", R.vertical_displacement_velocity_mm_day);
}

void page2_tilt_vibration() {
  drawHeader("TILT/VIB");
  oled.setCursor(0, 16);
  oled.printf("TiltX : %.2f deg\n", R.tilt_x_deg);
  oled.printf("TiltY : %.2f deg\n", R.tilt_y_deg);
  oled.printf("Vrms  : %.3f g\n", R.vibration_rms_g);
  oled.printf("Vpk   : %.3f g\n", R.vibration_peak_g);
  oled.printf("Freq~ : %.1f Hz\n", R.dominant_frequency_hz);
}

void page3_temp_cumulative() {
  drawHeader("TEMP/CUM");
  oled.setCursor(0, 16);
  oled.printf("Temp  : %.2f C\n", R.temperature_C);
  oled.printf("CumCr : %.2f mm\n", R.cumulative_crack_growth_mm);
  oled.printf("CumSub: %.2f mm\n", R.cumulative_subsidence_mm);
  oled.printf("TiltRt: %.3f d/d\n", R.tilt_rate_deg_day);
}

void page4_system_status() {
  drawHeader("SYS STATUS");
  oled.setCursor(0, 16);
  oled.printf("MPU  : %s\n", mpuOK ? "OK" : "FAIL");
  oled.printf("DS18 : %s\n", dsOK ? "OK" : "FAIL");
  oled.printf("LoRa : %s\n", loraOK ? "OK" : "FAIL");
  oled.printf("TX count: %lu\n", packetCounter);
  oled.printf("LastTX:%lus ago\n", (millis() - lastTxTime) / 1000);
}

void page5_ai_risk() {
  drawHeader("AI RISK");
  oled.setCursor(0, 16);
  oled.printf("State:\n");
  oled.printf("%s\n", rf_compact::kClassNames[g_predictedClass]);
  oled.printf("Conf : %d%%\n", (int)(g_confidence * 100));
  oled.printf("Votes: %u/%u\n", g_maxVotes, rf_compact::kNumTrees);
}

void showPage(uint8_t p) {
  oled.clearDisplay();
  switch (p % NUM_PAGES) {
    case 0: page1_displacement();    break;
    case 1: page2_tilt_vibration();  break;
    case 2: page3_temp_cumulative(); break;
    case 3: page4_system_status();   break;
    case 4: page5_ai_risk();         break;
  }
  oled.display();
}

// ---------------- LoRa packet ----------------
// CSV format: NODE_ID,millis,predicted_class_id,confidence,f1,f2,...,f23
// That's 4 + 23 = 27 comma-separated fields, f1..f23 in the EXACT
// order documented in ESP32_RF_DEPLOYMENT_INSTRUCTIONS.md.
void sendPacket() {
  if (!loraOK) return;

  if (!LoRa.beginPacket()) {
    Serial.println("[LoRa] beginPacket() failed - radio busy/not ready, skipping this TX.");
    return;
  }
  LoRa.print(NODE_ID); LoRa.print(",");
  LoRa.print(millis()); LoRa.print(",");
  LoRa.print(g_predictedClass); LoRa.print(",");
  LoRa.print(g_confidence, 3);
  for (uint32_t i = 0; i < rf_compact::kNumFeatures; i++) {
    LoRa.print(",");
    LoRa.print(raw_features[i], (i == 4 || i == 5) ? 4 : 3);  // extra precision for vibration
  }

  if (LoRa.endPacket()) {
    packetCounter++;
    lastTxTime = millis();
    Serial.println("[LoRa] Packet sent OK.");
  } else {
    Serial.println("[LoRa] endPacket() reported failure - packet may not have been sent.");
  }
}

// ================= SETUP =================
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== SENSE-MINE NODE (EDGE AI) BOOT ===");
  Serial.printf("Model: %u trees, %u nodes, %u features, %u classes\n",
                rf_compact::kNumTrees, rf_compact::kNumNodes,
                rf_compact::kNumFeatures, rf_compact::kNumClasses);

  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  Wire.setClock(100000);

  oledOK = oled.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  if (oledOK) {
    oled.clearDisplay();
    oled.setTextColor(SSD1306_WHITE);
    oled.setCursor(0, 0);
    oled.println("SENSE-MINE NODE");
    oled.println("Edge AI Boot...");
    oled.display();
  } else {
    Serial.println("[OLED] init FAILED - check wiring / I2C address (0x3C or 0x3D)");
  }

  mpuOK = mpuInit();
  Serial.printf("[MPU] init %s\n", mpuOK ? "OK" : "FAIL (check I2C wiring on GPIO8/9)");

  ds18b20.begin();
  dsOK = (ds18b20.getDeviceCount() > 0);
  Serial.printf("[DS18B20] devices found: %d\n", ds18b20.getDeviceCount());

  SPI.begin(PIN_LORA_SCK, PIN_LORA_MISO, PIN_LORA_MOSI, PIN_LORA_NSS);
  LoRa.setPins(PIN_LORA_NSS, PIN_LORA_RST, PIN_LORA_DIO0);
  loraOK = LoRa.begin(LORA_FREQ);
  Serial.printf("[LoRa] init %s\n", loraOK ? "OK" : "FAIL (check SPI wiring / antenna)");

  analogReadResolution(12);
  prev_t = millis();

  delay(1000);
}

// ================= LOOP =================
void loop() {
  int rawCrack = analogRead(PIN_POT_CRACK);
  int rawVert  = analogRead(PIN_POT_VERT);
  R.crack_width_mm           = adcToCrackMM(rawCrack);
  R.vertical_displacement_mm = adcToDisplacementMM(rawVert);

  MpuData d;
  if (mpuOK && mpuRead(d)) {
    R.tilt_x_deg = d.roll;
    R.tilt_y_deg = d.pitch;
  } else {
    R.tilt_x_deg = NAN;
    R.tilt_y_deg = NAN;
  }
  sampleVibration(R.vibration_rms_g, R.vibration_peak_g, R.dominant_frequency_hz);

  if (dsOK) {
    ds18b20.requestTemperatures();
    float t = ds18b20.getTempCByIndex(0);
    if (t == DEVICE_DISCONNECTED_C) {
      Serial.println("[DS18B20] Sensor disconnected mid-run - flagging temperature invalid.");
      R.temperature_C = NAN;
      dsOK = false;
    } else {
      R.temperature_C = t;
    }
  } else {
    R.temperature_C = NAN;
  }

  unsigned long sampleTime = millis();
  float dt_days = (sampleTime - prev_t) / 1000.0f / 86400.0f;
  if (dt_days < 1e-9) dt_days = 1e-9;

  if (!baselineSet) {
    baseline_crack = R.crack_width_mm;
    baseline_disp  = R.vertical_displacement_mm;
    baselineSet = true;
    Serial.printf("[BASELINE] crack=%.3f mm  disp=%.3f mm\n", baseline_crack, baseline_disp);
  }

  if (!isnan(prev_crack)) {
    R.crack_growth_rate_mm_day = (R.crack_width_mm - prev_crack) / dt_days;
  }
  if (!isnan(prev_disp)) {
    R.vertical_displacement_velocity_mm_day = (R.vertical_displacement_mm - prev_disp) / dt_days;
    if (!isnan(prev_vel)) {
      R.displacement_acceleration_mm_day2 =
        (R.vertical_displacement_velocity_mm_day - prev_vel) / dt_days;
    }
    prev_vel = R.vertical_displacement_velocity_mm_day;
  }
  float tiltMag = sqrt(R.tilt_x_deg * R.tilt_x_deg + R.tilt_y_deg * R.tilt_y_deg);
  if (!isnan(prev_tilt_mag)) {
    R.tilt_rate_deg_day = (tiltMag - prev_tilt_mag) / dt_days;
  }

  R.cumulative_crack_growth_mm = max(0.0f, R.crack_width_mm - baseline_crack);
  R.cumulative_subsidence_mm   = max(0.0f, R.vertical_displacement_mm - baseline_disp);

  prev_crack    = R.crack_width_mm;
  prev_disp     = R.vertical_displacement_mm;
  prev_tilt_mag = tiltMag;
  prev_t        = sampleTime;

  // ---- run the on-device Random Forest ----
  runInference();

  // ---- serial debug dump ----
  Serial.println("---- NODE READING ----");
  for (uint32_t i = 0; i < rf_compact::kNumFeatures; i++) {
    Serial.printf("%-38s: %.4f\n", kFeatureNamesLocal[i], raw_features[i]);
  }
  Serial.printf("PREDICTED STATE: %s  (confidence %.0f%%, votes %u/%u)\n",
                rf_compact::kClassNames[g_predictedClass],
                g_confidence * 100, g_maxVotes, rf_compact::kNumTrees);

  // ---- OLED circular page cycling ----
  if (oledOK && sampleTime - lastPageSwitch > PAGE_INTERVAL_MS) {
    currentPage = (currentPage + 1) % NUM_PAGES;
    lastPageSwitch = sampleTime;
  }
  if (oledOK) showPage(currentPage);

  // ---- periodic LoRa transmission ----
  if (sampleTime - lastTxTime > 5000) sendPacket();

  delay(500);
}

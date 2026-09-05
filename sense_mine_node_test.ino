/* ============================================================
   SENSE-MINE  |  SENSOR NODE — HARDWARE TEST FIRMWARE
   Board   : ESP32-S3-DevKitC-1
   Purpose : Verify every sensor + the LoRa uplink are alive,
             and print/display every parameter this node CAN
             produce from the dataset schema.
   SCOPE   : Hardware-level test only.
             NO AI model. NO dashboard. NO cloud logic here.
   ============================================================

   FROZEN PIN MAP (node)
   ----------------------------------------------------------
   GPIO 1  : ADC  - Crack-width potentiometer (wiper)
   GPIO 2  : ADC  - Vertical-displacement potentiometer (wiper)
   GPIO 4  : 1-Wire - DS18B20 temperature (4.7k pull-up to 3V3)
   GPIO 8  : I2C SDA - MPU6050 (clone) + OLED (shared bus)
   GPIO 9  : I2C SCL - MPU6050 (clone) + OLED (shared bus)
   GPIO 10 : SPI NSS  - SX1278 LoRa
   GPIO 11 : SPI MOSI - SX1278 LoRa
   GPIO 12 : SPI SCK  - SX1278 LoRa
   GPIO 13 : SPI MISO - SX1278 LoRa
   GPIO 14 : LoRa RESET
   GPIO 15 : LoRa DIO0

   LIBRARIES REQUIRED (Library Manager)
   ----------------------------------------------------------
   - LoRa by sandeepmistry
   - OneWire
   - DallasTemperature
   - Adafruit GFX Library
   - Adafruit SSD1306
   MPU6050 uses NO library — this board's MPU6050 is a
   non-original/clone chip, so it is driven with raw I2C
   register reads/writes only (see "bare MPU6050 driver" below).
   ============================================================ */

#include <Wire.h>
#include <SPI.h>
#include <LoRa.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

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
#define NODE_ID            1        // <-- change to 2, 3... per physical node

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

// ---------------- FIXED SITE / PANEL CONFIG (not sensor-measured) ----------------
const float mining_depth_m          = 584.0785429f;
const float seam_thickness_m        = 3.527589707f;
const float mining_height_m         = 3.321241755f;
const float panel_width_m           = 284.2377001f;
const float panel_length_m          = 1420.158918f;
const float face_advance_rate_m_day = 6.02314398f;
const float extraction_ratio        = 0.940441635f;

// ---------------- live + derived readings ----------------
struct Readings {
  float crack_width_mm;
  float vertical_displacement_mm;
  float tilt_x_deg;
  float tilt_y_deg;
  float vibration_rms_g;
  float vibration_peak_g;
  float dominant_frequency_hz;   // approximate - see note below
  float temperature_C;

  // derived (need a previous sample to compute a rate)
  float crack_growth_rate_mm_day;
  float vertical_displacement_velocity_mm_day;
  float displacement_acceleration_mm_day2;
  float tilt_rate_deg_day;
  float cumulative_crack_growth_mm;
  float cumulative_subsidence_mm;
} R;

float prev_crack = NAN, prev_disp = NAN, prev_vel = NAN, prev_tilt_mag = NAN;
unsigned long prev_t = 0;

// baselines captured on the very first valid reading - cumulative values are
// defined as net movement away from this baseline, NOT sum of absolute deltas
float baseline_crack = NAN, baseline_disp = NAN;
bool  baselineSet = false;

#define VIB_SAMPLES 50
float vibBuf[VIB_SAMPLES];

// ---------------- OLED circular page cycling ----------------
#define NUM_PAGES 4
uint8_t currentPage = 0;
unsigned long lastPageSwitch = 0;
const unsigned long PAGE_INTERVAL_MS = 3000;

bool mpuOK = false, dsOK = false, loraOK = false, oledOK = false;
unsigned long lastTxTime = 0;
uint32_t packetCounter = 0;
// NOTE: RSSI is deliberately NOT tracked here - a transmitting node never
// receives a packet, so a node-side "RSSI" would always read 0/stale.
// RSSI/SNR belong on the command centre, which actually calls
// LoRa.packetRssi() / LoRa.packetSnr() on receive.

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
  if (Wire.endTransmission() != 0) return false;   // no ACK -> chip not present

  uint8_t whoami = mpuReadByte(REG_WHO_AM_I);
  // Genuine InvenSense parts answer 0x68 at WHO_AM_I. Clone chips are
  // inconsistent here, so we log it but do NOT hard-fail on a mismatch.
  mpuWrite(REG_PWR_MGMT_1, 0x00);     // wake from sleep, internal clock
  delay(50);
  mpuWrite(REG_ACCEL_CONFIG, 0x00);  // +-2g
  mpuWrite(REG_GYRO_CONFIG, 0x00);   // +-250 dps
  delay(20);
  Serial.printf("[MPU] WHO_AM_I = 0x%02X (clone-tolerant init, not hard-checked)\n", whoami);
  return true;
}

struct MpuData { float ax, ay, az, gx, gy, gz, roll, pitch; };

bool mpuRead(MpuData &d) {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(REG_ACCEL_XOUT_H);
  if (Wire.endTransmission(false) != 0) return false;
  Wire.requestFrom((int)MPU_ADDR, 14);
  if (Wire.available() < 14) return false;

  int16_t axr = (Wire.read() << 8) | Wire.read();
  int16_t ayr = (Wire.read() << 8) | Wire.read();
  int16_t azr = (Wire.read() << 8) | Wire.read();
  Wire.read(); Wire.read();              // discard chip's internal temp reg (DS18B20 used instead)
  int16_t gxr = (Wire.read() << 8) | Wire.read();
  int16_t gyr = (Wire.read() << 8) | Wire.read();
  int16_t gzr = (Wire.read() << 8) | Wire.read();

  d.ax = axr / 16384.0f;   // +-2g sensitivity
  d.ay = ayr / 16384.0f;
  d.az = azr / 16384.0f;
  d.gx = gxr / 131.0f;     // +-250 dps sensitivity
  d.gy = gyr / 131.0f;
  d.gz = gzr / 131.0f;

  d.roll  = atan2(d.ay, d.az) * 180.0 / PI;
  d.pitch = atan2(-d.ax, sqrt(d.ay * d.ay + d.az * d.az)) * 180.0 / PI;
  return true;
}

// naive vibration RMS/peak + zero-crossing frequency estimate.
// NOTE: this is an approximation for hardware bring-up only.
// A real dominant_frequency_hz value needs an FFT (e.g. arduinoFFT) —
// intentionally left out here since this is hardware-test-only code.
void sampleVibration(float &rms, float &peak, float &domFreqHz) {
  MpuData d;
  int zeroCrossings = 0;
  float prevMag = 0;
  float sumSq = 0;
  peak = 0;
  unsigned long t0 = millis();

  for (int i = 0; i < VIB_SAMPLES; i++) {
    if (mpuRead(d)) {
      float mag = sqrt(d.ax * d.ax + d.ay * d.ay + d.az * d.az) - 1.0f; // remove gravity
      vibBuf[i] = mag;
      sumSq += mag * mag;
      if (fabs(mag) > peak) peak = fabs(mag);
      if (i > 0 && ((mag > 0 && prevMag <= 0) || (mag < 0 && prevMag >= 0))) zeroCrossings++;
      prevMag = mag;
    } else {
      vibBuf[i] = 0;
    }
    delay(4);   // ~250 Hz sample rate
  }
  unsigned long dt = millis() - t0;
  rms = sqrt(sumSq / VIB_SAMPLES);
  domFreqHz = (dt > 0) ? (zeroCrossings / 2.0f) / (dt / 1000.0f) : 0;
}

// ---------------- ADC -> mm placeholders ----------------
// TODO: replace with real calibration curves from your mechanical rig.
// Using floating-point math here (NOT Arduino's integer map()) so the
// output isn't quantized to ~1 mm steps.
float adcToCrackMM(int raw) {
  return (raw / 4095.0f) * 50.0f;     // 0-50 mm demo range
}
float adcToDisplacementMM(int raw) {
  return (raw / 4095.0f) * 100.0f;    // 0-100 mm demo range
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

// pages wrap around (0->1->2->3->0->...) - this is the "circular" cycling
void showPage(uint8_t p) {
  oled.clearDisplay();
  switch (p % NUM_PAGES) {
    case 0: page1_displacement();    break;
    case 1: page2_tilt_vibration();  break;
    case 2: page3_temp_cumulative(); break;
    case 3: page4_system_status();   break;
  }
  oled.display();
}

// ---------------- LoRa test packet ----------------
// CSV format: NODE_ID,millis,crack,vert,tiltx,tilty,vibrms,vibpeak,domfreq,temp,depth,seam,extraction
void sendPacket() {
  if (!loraOK) return;

  if (!LoRa.beginPacket()) {
    Serial.println("[LoRa] beginPacket() failed - radio busy/not ready, skipping this TX.");
    return;
  }
  LoRa.print(NODE_ID); LoRa.print(",");
  LoRa.print(millis()); LoRa.print(",");
  LoRa.print(R.crack_width_mm, 3); LoRa.print(",");
  LoRa.print(R.vertical_displacement_mm, 3); LoRa.print(",");
  LoRa.print(R.tilt_x_deg, 3); LoRa.print(",");
  LoRa.print(R.tilt_y_deg, 3); LoRa.print(",");
  LoRa.print(R.vibration_rms_g, 4); LoRa.print(",");
  LoRa.print(R.vibration_peak_g, 4); LoRa.print(",");
  LoRa.print(R.dominant_frequency_hz, 2); LoRa.print(",");
  LoRa.print(R.temperature_C, 2); LoRa.print(",");
  LoRa.print(mining_depth_m, 3); LoRa.print(",");
  LoRa.print(seam_thickness_m, 3); LoRa.print(",");
  LoRa.print(extraction_ratio, 4);

  if (LoRa.endPacket()) {
    packetCounter++;
    lastTxTime = millis();
    Serial.println("[LoRa] Test packet sent OK.");
  } else {
    Serial.println("[LoRa] endPacket() reported failure - packet may not have been sent.");
  }
}

// ================= SETUP =================
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== SENSE-MINE NODE HARDWARE TEST ===");

  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  Wire.setClock(100000);

  oledOK = oled.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  if (oledOK) {
    oled.clearDisplay();
    oled.setTextColor(SSD1306_WHITE);
    oled.setCursor(0, 0);
    oled.println("SENSE-MINE NODE");
    oled.println("Hardware Test Boot...");
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

  analogReadResolution(12);   // ESP32-S3 ADC1: 12-bit
  prev_t = millis();

  delay(1000);
}

// ================= LOOP =================
void loop() {
  // 1) displacement potentiometers
  int rawCrack = analogRead(PIN_POT_CRACK);
  int rawVert  = analogRead(PIN_POT_VERT);
  R.crack_width_mm           = adcToCrackMM(rawCrack);
  R.vertical_displacement_mm = adcToDisplacementMM(rawVert);

  // 2) MPU6050 tilt + vibration
  MpuData d;
  if (mpuOK && mpuRead(d)) {
    R.tilt_x_deg = d.roll;
    R.tilt_y_deg = d.pitch;
  } else {
    // Don't let a failed read silently keep showing the last-good tilt value -
    // an unreadable sensor should show up as invalid data, not stale data.
    R.tilt_x_deg = NAN;
    R.tilt_y_deg = NAN;
  }
  sampleVibration(R.vibration_rms_g, R.vibration_peak_g, R.dominant_frequency_hz);  // this alone takes ~200ms

  // 3) DS18B20 temperature
  if (dsOK) {
    ds18b20.requestTemperatures();   // this also takes real time (conversion delay)
    float t = ds18b20.getTempCByIndex(0);
    if (t == DEVICE_DISCONNECTED_C) {
      Serial.println("[DS18B20] Sensor disconnected mid-run - flagging temperature invalid.");
      R.temperature_C = NAN;
      dsOK = false;   // downgrade status so OLED/SYS STATUS page reflects it
    } else {
      R.temperature_C = t;
    }
  } else {
    R.temperature_C = NAN;
  }

  // Take the timing snapshot HERE, only after every blocking sensor read is
  // done (vibration sampling + DS18B20 conversion both take real, non-trivial
  // time). Using a timestamp from the top of loop() would understate the
  // true elapsed interval and skew every *_day rate below.
  unsigned long sampleTime = millis();
  float dt_days = (sampleTime - prev_t) / 1000.0f / 86400.0f;  // elapsed time as a fraction of a day
  if (dt_days < 1e-9) dt_days = 1e-9;

  // capture baseline once, from the first valid reading
  if (!baselineSet) {
    baseline_crack = R.crack_width_mm;
    baseline_disp  = R.vertical_displacement_mm;
    baselineSet = true;
    Serial.printf("[BASELINE] crack=%.3f mm  disp=%.3f mm\n", baseline_crack, baseline_disp);
  }

  // 4) derived rates (extrapolated to "per day" units from this short interval —
  //    for a real deployment these should be computed over hours/days, not seconds)
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

  // cumulative values = net movement AWAY from baseline, not sum of absolute
  // sample-to-sample deltas (a value that returns to baseline should not
  // keep accumulating "growth")
  R.cumulative_crack_growth_mm = max(0.0f, R.crack_width_mm - baseline_crack);
  R.cumulative_subsidence_mm   = max(0.0f, R.vertical_displacement_mm - baseline_disp);

  prev_crack    = R.crack_width_mm;
  prev_disp     = R.vertical_displacement_mm;
  prev_tilt_mag = tiltMag;
  prev_t        = sampleTime;   // use the post-acquisition timestamp, not loop-start time

  // 5) full serial debug dump
  Serial.println("---- NODE READING ----");
  Serial.printf("crack_width_mm: %.3f\n", R.crack_width_mm);
  Serial.printf("vertical_displacement_mm: %.3f\n", R.vertical_displacement_mm);
  Serial.printf("tilt_x_deg: %.3f\n", R.tilt_x_deg);
  Serial.printf("tilt_y_deg: %.3f\n", R.tilt_y_deg);
  Serial.printf("vibration_rms_g: %.4f\n", R.vibration_rms_g);
  Serial.printf("vibration_peak_g: %.4f\n", R.vibration_peak_g);
  Serial.printf("dominant_frequency_hz (approx, no FFT): %.2f\n", R.dominant_frequency_hz);
  Serial.printf("temperature_C: %.2f\n", R.temperature_C);
  Serial.printf("crack_growth_rate_mm_day: %.4f\n", R.crack_growth_rate_mm_day);
  Serial.printf("vertical_displacement_velocity_mm_day: %.4f\n", R.vertical_displacement_velocity_mm_day);
  Serial.printf("displacement_acceleration_mm_day2: %.4f\n", R.displacement_acceleration_mm_day2);
  Serial.printf("tilt_rate_deg_day: %.4f\n", R.tilt_rate_deg_day);
  Serial.printf("cumulative_crack_growth_mm: %.3f\n", R.cumulative_crack_growth_mm);
  Serial.printf("cumulative_subsidence_mm: %.3f\n", R.cumulative_subsidence_mm);
  Serial.printf("mining_depth_m (fixed): %.4f\n", mining_depth_m);
  Serial.printf("seam_thickness_m (fixed): %.4f\n", seam_thickness_m);
  Serial.printf("mining_height_m (fixed): %.4f\n", mining_height_m);
  Serial.printf("panel_width_m (fixed): %.4f\n", panel_width_m);
  Serial.printf("panel_length_m (fixed): %.4f\n", panel_length_m);
  Serial.printf("face_advance_rate_m_day (fixed): %.4f\n", face_advance_rate_m_day);
  Serial.printf("extraction_ratio (fixed): %.4f\n", extraction_ratio);
  Serial.println("spatial_displacement_gradient / spatial_coherence: computed at Command Centre from multiple nodes, not by a single node.");

  // 6) OLED circular page cycling
  if (oledOK && sampleTime - lastPageSwitch > PAGE_INTERVAL_MS) {
    currentPage = (currentPage + 1) % NUM_PAGES;   // wraps back to page 0 -> circular
    lastPageSwitch = sampleTime;
  }
  if (oledOK) showPage(currentPage);

  // 7) periodic LoRa test transmission
  if (sampleTime - lastTxTime > 5000) sendPacket();

  delay(500);
}



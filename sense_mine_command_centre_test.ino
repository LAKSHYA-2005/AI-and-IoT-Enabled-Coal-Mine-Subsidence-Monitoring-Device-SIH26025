/* ============================================================
   SENSE-MINE  |  COMMAND CENTRE — HARDWARE TEST FIRMWARE
   Board   : ESP32 DevKitC / WROOM-32
   Purpose : Confirm the LoRa receive link, the SIM800L GSM
             modem, and the OLED debug pages all work together.
   SCOPE   : Hardware-level test only.
             NO AI model. NO dashboard. NO cloud logic here.
   ============================================================

   FROZEN PIN MAP (command centre)
   ----------------------------------------------------------
   GPIO 18 : SPI SCK   - SX1278 LoRa (VSPI)
   GPIO 19 : SPI MISO  - SX1278 LoRa (VSPI)
   GPIO 23 : SPI MOSI  - SX1278 LoRa (VSPI)
   GPIO 5  : SPI NSS   - SX1278 LoRa
   GPIO 14 : LoRa RESET
   GPIO 26 : LoRa DIO0
   GPIO 16 : UART2 RX  <- SIM800L TXD
   GPIO 17 : UART2 TX  -> SIM800L RXD
   GPIO 27 : SIM800L RESET (optional)
   GPIO 21 : I2C SDA   - OLED (default ESP32 I2C pins, unused elsewhere)
   GPIO 22 : I2C SCL   - OLED

   IMPORTANT (repeated from architecture notes):
   - SIM800L VCC must come from an external ~4.0V regulated
     supply capable of high current pulses — NOT the ESP32
     3.3V pin.
   - Confirm your exact SIM800L breakout's UART logic level
     (3.3V-tolerant boards are common, but check yours).

   LIBRARIES REQUIRED (Library Manager)
   ----------------------------------------------------------
   - LoRa by sandeepmistry
   - Adafruit GFX Library
   - Adafruit SSD1306
   (HardwareSerial/Wire/SPI are built in to the ESP32 core.)
   ============================================================ */

#include <SPI.h>
#include <LoRa.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <HardwareSerial.h>

// ---------------- PIN MAP ----------------
#define PIN_LORA_SCK   18
#define PIN_LORA_MISO  19
#define PIN_LORA_MOSI  23
#define PIN_LORA_NSS   5
#define PIN_LORA_RST   14
#define PIN_LORA_DIO0  26

#define PIN_SIM_RX     16   // ESP32 RX  <- SIM800L TXD
#define PIN_SIM_TX     17   // ESP32 TX  -> SIM800L RXD
#define PIN_SIM_RST    27   // optional hardware reset line

#define PIN_I2C_SDA    21
#define PIN_I2C_SCL    22

#define LORA_FREQ      433E6

HardwareSerial sim800(2);   // UART2
Adafruit_SSD1306 oled(128, 64, &Wire, -1);

bool oledOK = false, loraOK = false, simOK = false;

// ---------------- per-node record table ----------------
#define MAX_NODES 5
struct NodeRecord {
  bool     seen = false;
  int      nodeId = -1;
  unsigned long lastSeenMs = 0;
  int      rssi = 0;
  float    snr = 0;
  float    crack_width_mm, vert_disp_mm, tilt_x, tilt_y;
  float    vib_rms, vib_peak, dom_freq, temp_c;
  float    mining_depth_m, seam_thickness_m, extraction_ratio;
  uint32_t packetCount = 0;
};
NodeRecord nodes[MAX_NODES];
int knownNodeCount = 0;
uint32_t totalPacketsReceived = 0;
int lastRxNodeIdx = -1;

// ---------------- OLED circular page cycling ----------------
// page 0 = overview, 1 = last node detail, 2 = LoRa radio stats, 3 = GSM/SIM800L
#define NUM_PAGES 4
uint8_t currentPage = 0;
unsigned long lastPageSwitch = 0;
const unsigned long PAGE_INTERVAL_MS = 3000;

// ---------------- helpers ----------------
int getNodeIndex(int id) {
  for (int i = 0; i < knownNodeCount; i++) if (nodes[i].nodeId == id) return i;
  if (knownNodeCount < MAX_NODES) {
    nodes[knownNodeCount].nodeId = id;
    nodes[knownNodeCount].seen = true;
    return knownNodeCount++;
  }
  Serial.println("[WARN] Node table full, discarding new node ID.");
  return -1;
}

// CSV format from node:
// NODE_ID,millis,crack,vert,tiltx,tilty,vibrms,vibpeak,domfreq,temp,depth,seam,extraction
void parsePacket(String pkt, int rssi, float snr) {
  int idx = -1;
  float vals[11];
  int fieldCount = 0;
  int start = 0;
  for (int i = 0; i <= (int)pkt.length(); i++) {
    if (i == (int)pkt.length() || pkt[i] == ',') {
      String tok = pkt.substring(start, i);
      if (fieldCount == 0) idx = tok.toInt();
      else if (fieldCount >= 2 && fieldCount <= 12) vals[fieldCount - 2] = tok.toFloat();
      fieldCount++;
      start = i + 1;
    }
  }
  if (idx < 0) { Serial.println("[LoRa] Malformed packet, discarded."); return; }

  int ni = getNodeIndex(idx);
  if (ni < 0) return;

  nodes[ni].lastSeenMs       = millis();
  nodes[ni].rssi             = rssi;
  nodes[ni].snr              = snr;
  nodes[ni].crack_width_mm   = vals[0];
  nodes[ni].vert_disp_mm     = vals[1];
  nodes[ni].tilt_x           = vals[2];
  nodes[ni].tilt_y           = vals[3];
  nodes[ni].vib_rms          = vals[4];
  nodes[ni].vib_peak         = vals[5];
  nodes[ni].dom_freq         = vals[6];
  nodes[ni].temp_c           = vals[7];
  nodes[ni].mining_depth_m   = vals[8];
  nodes[ni].seam_thickness_m = vals[9];
  nodes[ni].extraction_ratio = vals[10];
  nodes[ni].packetCount++;
  totalPacketsReceived++;
  lastRxNodeIdx = ni;

  Serial.printf("---- PACKET FROM NODE %d ----\n", idx);
  Serial.printf("RSSI: %d dBm   SNR: %.1f dB\n", rssi, snr);
  Serial.printf("crack_width_mm: %.3f\n", nodes[ni].crack_width_mm);
  Serial.printf("vertical_displacement_mm: %.3f\n", nodes[ni].vert_disp_mm);
  Serial.printf("tilt_x_deg: %.3f   tilt_y_deg: %.3f\n", nodes[ni].tilt_x, nodes[ni].tilt_y);
  Serial.printf("vibration_rms_g: %.4f   vibration_peak_g: %.4f\n", nodes[ni].vib_rms, nodes[ni].vib_peak);
  Serial.printf("dominant_frequency_hz (approx): %.2f\n", nodes[ni].dom_freq);
  Serial.printf("temperature_C: %.2f\n", nodes[ni].temp_c);
  Serial.printf("mining_depth_m (fixed): %.4f\n", nodes[ni].mining_depth_m);
  Serial.printf("seam_thickness_m (fixed): %.4f\n", nodes[ni].seam_thickness_m);
  Serial.printf("extraction_ratio (fixed): %.4f\n", nodes[ni].extraction_ratio);
  Serial.println("(spatial_displacement_gradient / spatial_coherence require >=2 live nodes - not computed in this hardware test build)");
}

// ---------------- SIM800L bare AT test ----------------
bool simSendAT(const char* cmd, const char* expect, unsigned long timeoutMs) {
  while (sim800.available()) sim800.read();   // flush stale bytes
  sim800.print(cmd);
  sim800.print("\r\n");
  unsigned long t0 = millis();
  String resp = "";
  while (millis() - t0 < timeoutMs) {
    while (sim800.available()) resp += (char)sim800.read();
    if (resp.indexOf(expect) != -1) return true;
  }
  Serial.printf("[SIM800L] '%s' -> expected '%s', got: %s\n", cmd, expect, resp.c_str());
  return false;
}

// ================= OLED PAGES =================
void drawHeader(const char* title) {
  oled.setTextSize(1);
  oled.setCursor(0, 0);
  oled.print("CMD  ");
  oled.println(title);
  oled.drawLine(0, 10, 128, 10, SSD1306_WHITE);
}

void page0_overview() {
  drawHeader("OVERVIEW");
  oled.setCursor(0, 16);
  oled.printf("Nodes known : %d\n", knownNodeCount);
  oled.printf("Packets tot : %lu\n", totalPacketsReceived);
  for (int i = 0; i < knownNodeCount && i < 3; i++) {
    unsigned long age = (millis() - nodes[i].lastSeenMs) / 1000;
    oled.printf("N%d age:%lus\n", nodes[i].nodeId, age);
  }
}

void page1_last_node_detail() {
  drawHeader("LAST NODE");
  oled.setCursor(0, 16);
  if (lastRxNodeIdx < 0) { oled.println("No packet yet"); return; }
  NodeRecord &n = nodes[lastRxNodeIdx];
  oled.printf("N%d Crk:%.1fmm\n", n.nodeId, n.crack_width_mm);
  oled.printf("Vrt:%.1fmm T:%.1fC\n", n.vert_disp_mm, n.temp_c);
  oled.printf("TltX:%.1f TltY:%.1f\n", n.tilt_x, n.tilt_y);
  oled.printf("Vrms:%.2fg Pk:%.2fg\n", n.vib_rms, n.vib_peak);
}

void page2_radio_stats() {
  drawHeader("LORA RADIO");
  oled.setCursor(0, 16);
  oled.printf("Status: %s\n", loraOK ? "OK" : "FAIL");
  if (lastRxNodeIdx >= 0) {
    oled.printf("RSSI: %d dBm\n", nodes[lastRxNodeIdx].rssi);
    oled.printf("SNR : %.1f dB\n", nodes[lastRxNodeIdx].snr);
  }
  oled.println("Freq: 433 MHz");
}

void page3_gsm_status() {
  drawHeader("GSM/SIM800L");
  oled.setCursor(0, 16);
  oled.printf("Modem: %s\n", simOK ? "OK (AT)" : "NO RESPONSE");
  oled.println("Check CSQ/CREG");
  oled.println("on Serial monitor");
}

// pages wrap around (0->1->2->3->0->...) - this is the "circular" cycling
void showPage(uint8_t p) {
  oled.clearDisplay();
  switch (p % NUM_PAGES) {
    case 0: page0_overview();        break;
    case 1: page1_last_node_detail();break;
    case 2: page2_radio_stats();     break;
    case 3: page3_gsm_status();      break;
  }
  oled.display();
}

// ================= SETUP =================
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== SENSE-MINE COMMAND CENTRE HARDWARE TEST ===");

  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  oledOK = oled.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  if (oledOK) {
    oled.clearDisplay();
    oled.setTextColor(SSD1306_WHITE);
    oled.setCursor(0, 0);
    oled.println("SENSE-MINE CMD CTR");
    oled.println("Hardware Test Boot...");
    oled.display();
  } else {
    Serial.println("[OLED] init FAILED - check wiring / I2C address (0x3C or 0x3D)");
  }

  SPI.begin(PIN_LORA_SCK, PIN_LORA_MISO, PIN_LORA_MOSI, PIN_LORA_NSS);
  LoRa.setPins(PIN_LORA_NSS, PIN_LORA_RST, PIN_LORA_DIO0);
  loraOK = LoRa.begin(LORA_FREQ);
  Serial.printf("[LoRa] init %s\n", loraOK ? "OK" : "FAIL (check VSPI wiring / antenna)");

  pinMode(PIN_SIM_RST, OUTPUT);
  digitalWrite(PIN_SIM_RST, HIGH);

  sim800.begin(9600, SERIAL_8N1, PIN_SIM_RX, PIN_SIM_TX);
  delay(3000);   // allow modem to finish booting
  simOK = simSendAT("AT", "OK", 2000);
  Serial.printf("[SIM800L] AT test: %s\n", simOK ? "OK" : "FAIL (check external power / UART levels)");
  if (simOK) {
    simSendAT("AT+CSQ", "OK", 2000);     // signal quality - read result on Serial monitor
    simSendAT("AT+CREG?", "OK", 2000);   // network registration - read result on Serial monitor
  }

  delay(500);
}

// ================= LOOP =================
void loop() {
  // 1) check for an incoming LoRa packet from any node
  int packetSize = LoRa.parsePacket();
  if (packetSize) {
    String pkt = "";
    while (LoRa.available()) pkt += (char)LoRa.read();
    int rssi   = LoRa.packetRssi();
    float snr  = LoRa.packetSnr();
    parsePacket(pkt, rssi, snr);
  }

  // 2) periodic SIM800L liveness re-check (every 30s, not every loop)
  static unsigned long lastSimCheck = 0;
  if (millis() - lastSimCheck > 30000) {
    simOK = simSendAT("AT", "OK", 1500);
    lastSimCheck = millis();
  }

  // 3) circular OLED page cycling
  if (oledOK && millis() - lastPageSwitch > PAGE_INTERVAL_MS) {
    currentPage = (currentPage + 1) % NUM_PAGES;   // wraps back to page 0 -> circular
    lastPageSwitch = millis();
  }
  if (oledOK) showPage(currentPage);

  delay(50);
}

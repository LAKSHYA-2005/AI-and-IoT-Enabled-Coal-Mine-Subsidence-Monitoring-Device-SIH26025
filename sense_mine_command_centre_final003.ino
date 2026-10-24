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
#define WIFI_SSID            "Airtel_Lakshya"
#define WIFI_PASSWORD         "Lakshya@2005"

// 2) From your firebaseConfig (apiKey + projectId) — already filled in from what you shared
#define FIREBASE_API_KEY     "AIzaSyBwdXQLuA4vffr9BPH9egR6FIRTyd7bgyA"
#define FIREBASE_PROJECT_ID  "mine-subsidence-monitoring"

// 3) The fixed device account you create once in the Firebase console (Option B)
#define DEVICE_EMAIL         "indicmindofficial@gmail.com"
#define DEVICE_PASSWORD      "Lakshya@@@2005"

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
struct NodeLocation { int nodeId; double lat; double lon; const char* siteLabel; };
const NodeLocation kNodeLocations[] = {
  { 1, 24.550896, 87.460226, "Pachhwara North Coal Block, Pakur, Jharkhand (reference site - testbed not physically deployed here)" },
};
const int kNodeLocationCount = sizeof(kNodeLocations) / sizeof(kNodeLocations[0]);

bool getNodeLocation(int nodeId, double &lat, double &lon, const char* &siteLabel) {
  for (int i = 0; i < kNodeLocationCount; i++) {
    if (kNodeLocations[i].nodeId == nodeId) {
      lat = kNodeLocations[i].lat;
      lon = kNodeLocations[i].lon;
      siteLabel = kNodeLocations[i].siteLabel;
      return true;
    }
  }
  return false;
}
// ======================================================================

Adafruit_SSD1306 oled(128, 64, &Wire, -1);

bool oledOK = false, loraOK = false;

// local copy of class names/order - matches model_rf_compact.cpp exactly.
// (command centre doesn't compile the model, so it keeps its own small
// lookup table instead of needing the model files at all.)
static const char* kClassNames[6] = {
  "ACCELERATING", "INITIATION", "PROGRESSIVE", "RAPID", "STABILIZING", "STABLE"
};
bool isHighRisk(uint8_t classId) {
  // ACCELERATING(0), PROGRESSIVE(2), RAPID(3) -> alert-worthy
  return classId == 0 || classId == 2 || classId == 3;
}

// feature names, in the exact order the node sends them, for JSON keys
static const char* kFeatureNames[23] = {
  "crack_width_mm", "vertical_displacement_mm", "tilt_x_deg", "tilt_y_deg",
  "vibration_rms_g", "vibration_peak_g", "dominant_frequency_hz", "temperature_C",
  "crack_growth_rate_mm_day", "vertical_displacement_velocity_mm_day",
  "displacement_acceleration_mm_day2", "tilt_rate_deg_day",
  "cumulative_crack_growth_mm", "cumulative_subsidence_mm",
  "spatial_displacement_gradient", "spatial_coherence",
  "mining_depth_m", "seam_thickness_m", "mining_height_m",
  "panel_width_m", "panel_length_m", "face_advance_rate_m_day", "extraction_ratio"
};

// ---------------- per-node record table ----------------
#define MAX_NODES 5
struct NodeRecord {
  bool     seen = false;
  int      nodeId = -1;
  unsigned long lastSeenMs = 0;
  int      rssi = 0;
  float    snr = 0;
  uint8_t  predictedClass = 0;
  float    confidence = 0;
  float    features[23];
  uint32_t packetCount = 0;
};
NodeRecord nodes[MAX_NODES];
int knownNodeCount = 0;
uint32_t totalPacketsReceived = 0;
int lastRxNodeIdx = -1;

// ---------------- OLED circular page cycling ----------------
#define NUM_PAGES 4
uint8_t currentPage = 0;
unsigned long lastPageSwitch = 0;
const unsigned long PAGE_INTERVAL_MS = 3000;

// ---------------- WiFi / Firebase state (all best-effort) ----------------
unsigned long lastWifiAttemptMs = 0;
unsigned long wifiConnectStartedMs = 0;
const unsigned long WIFI_RETRY_INTERVAL_MS = 15000;
const unsigned long WIFI_CONNECT_TIMEOUT_MS = 12000;
bool wifiConnecting = false;

String g_idToken = "";
String g_refreshToken = "";
unsigned long g_tokenExpiryMs = 0;
unsigned long g_lastAuthAttemptMs = 0;
const unsigned long AUTH_RETRY_INTERVAL_MS = 30000;

bool g_timeSynced = false;
bool g_ntpStarted = false;

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

// Converts a float to a JSON-safe string: NaN/Inf (e.g. a failed sensor
// read from the node) becomes the JSON literal null instead of the
// non-JSON text "nan" that Arduino's default float printing produces.
// This is the fix for the "Node-RED debug shows a parse error" issue.
String jsonNum(float v, int decimals) {
  if (isnan(v) || isinf(v)) return "null";
  return String(v, decimals);
}

bool anyInvalidFeature(const float* f) {
  for (int i = 0; i < 23; i++) {
    if (isnan(f[i]) || isinf(f[i])) return true;
  }
  return false;
}

// CSV format from node: NODE_ID,millis,predicted_class_id,confidence,f1..f23
#define EXPECTED_FIELD_COUNT 27   // 4 header fields + 23 raw features

void printNodeJSON(int ni);
void pushReadingAndLatest(int ni);

void parsePacket(String pkt, int rssi, float snr) {
  // TEMPORARY LoRa RX DIAGNOSTIC: print exactly what the radio received
  // before parsing. This does NOT change the node firmware or packet format.
  Serial.println("[LoRa] RAW RX:");
  Serial.println(pkt);
  Serial.printf("[LoRa] RX length = %d bytes\n", pkt.length());

  int idx = -1;
  int classId = -1;
  float confidence = 0;
  float feats[23];
  int fieldCount = 0;
  int start = 0;

  for (int i = 0; i <= (int)pkt.length(); i++) {
    if (i == (int)pkt.length() || pkt[i] == ',') {
      String tok = pkt.substring(start, i);
      if (fieldCount == 0) idx = tok.toInt();
      else if (fieldCount == 1) { /* millis from node - not currently used here */ }
      else if (fieldCount == 2) classId = tok.toInt();
      else if (fieldCount == 3) confidence = tok.toFloat();
      else if (fieldCount >= 4 && fieldCount <= 26) feats[fieldCount - 4] = tok.toFloat();
      fieldCount++;
      start = i + 1;
    }
  }

  if (idx < 0 || fieldCount != EXPECTED_FIELD_COUNT || classId < 0 || classId > 5) {
    Serial.printf("[LoRa] Malformed packet, discarded (fields=%d, expected=%d).\n",
                  fieldCount, EXPECTED_FIELD_COUNT);
    Serial.println("[LoRa] Node firmware is expected to send 27 CSV fields.");
    return;
  }

  int ni = getNodeIndex(idx);
  if (ni < 0) return;

  nodes[ni].lastSeenMs     = millis();
  nodes[ni].rssi           = rssi;
  nodes[ni].snr            = snr;
  nodes[ni].predictedClass = (uint8_t)classId;
  nodes[ni].confidence     = confidence;
  memcpy(nodes[ni].features, feats, sizeof(feats));
  nodes[ni].packetCount++;
  totalPacketsReceived++;
  lastRxNodeIdx = ni;

  Serial.printf("---- PACKET FROM NODE %d ----\n", idx);
  Serial.printf("RSSI: %d dBm   SNR: %.1f dB\n", rssi, snr);
  Serial.printf("STATE: %s  (confidence %.0f%%)\n",
                kClassNames[nodes[ni].predictedClass], nodes[ni].confidence * 100);

  printNodeJSON(ni);        // <-- Node-RED reads this line (unconditional, USB)
  pushReadingAndLatest(ni); // <-- best-effort WiFi->Firestore path (skips instantly if offline)
}

// one compact JSON object per line, e.g.:
// {"node_id":1,"rssi":-82,"snr":9.5,"deformation_state":"STABLE","confidence":0.90,
//  "data_quality":"OK","features":{"crack_width_mm":12.340,...}}
// All numeric fields are JSON-safe (see jsonNum) - Node-RED's JSON.parse()
// will never throw on a NaN/Inf reading from the node.
void printNodeJSON(int ni) {
  NodeRecord &n = nodes[ni];
  bool dataOk = !anyInvalidFeature(n.features);
  Serial.print("{\"node_id\":"); Serial.print(n.nodeId);
  Serial.print(",\"rssi\":"); Serial.print(n.rssi);
  Serial.print(",\"snr\":"); Serial.print(jsonNum(n.snr, 1));
  Serial.print(",\"deformation_state\":\""); Serial.print(kClassNames[n.predictedClass]); Serial.print("\"");
  Serial.print(",\"confidence\":"); Serial.print(jsonNum(n.confidence, 3));
  Serial.print(",\"data_quality\":\""); Serial.print(dataOk ? "OK" : "PARTIAL"); Serial.print("\"");
  Serial.print(",\"features\":{");
  for (int i = 0; i < 23; i++) {
    Serial.print("\""); Serial.print(kFeatureNames[i]); Serial.print("\":");
    Serial.print(jsonNum(n.features[i], 4));
    if (i < 22) Serial.print(",");
  }
  Serial.println("}}");
}

// ================= WiFi / TIME (all non-blocking, best-effort) =================
void maintainWiFi() {
  wl_status_t status = WiFi.status();

  if (status == WL_CONNECTED) {
    if (wifiConnecting) {
      wifiConnecting = false;
      Serial.print("[WiFi] Connected. IP: ");
      Serial.println(WiFi.localIP());
    }
    return;
  }

  // We already called WiFi.begin(). Do NOT call WiFi.begin() again while
  // the ESP32 is still trying to associate; that is what caused:
  // "sta is connecting, cannot set config".
  if (wifiConnecting) {
    if (millis() - wifiConnectStartedMs >= WIFI_CONNECT_TIMEOUT_MS) {
      Serial.println("[WiFi] Connection attempt timed out; restarting cleanly.");
      WiFi.disconnect(false, false);
      wifiConnecting = false;
      lastWifiAttemptMs = millis();
    }
    return;
  }

  if (millis() - lastWifiAttemptMs < WIFI_RETRY_INTERVAL_MS) return;

  lastWifiAttemptMs = millis();
  wifiConnectStartedMs = millis();
  wifiConnecting = true;

  Serial.printf("[WiFi] Starting connection to %s...\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

void syncTimeIfNeeded() {
  if (g_timeSynced || WiFi.status() != WL_CONNECTED) return;
  if (!g_ntpStarted) {
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    g_ntpStarted = true;
    Serial.println("[NTP] Sync requested...");
  }
  time_t now = time(nullptr);
  if (now > 1700000000) {  // sanity check: after ~Nov 2023, i.e. a real sync happened
    g_timeSynced = true;
    Serial.println("[NTP] Time synced.");
  }
}

String isoTimestampNow() {
  if (!g_timeSynced) return "";   // caller omits the timestamp field if empty
  time_t now = time(nullptr);
  struct tm timeinfo;
  gmtime_r(&now, &timeinfo);
  char buf[25];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &timeinfo);
  return String(buf);
}

// ================= FIREBASE AUTH (Option B — fixed device account) =================
// Minimal hand-rolled extractor for the flat string fields Firebase's
// REST auth endpoints return - avoids pulling in a full JSON library
// for two small responses.
String extractJsonString(const String& json, const String& key) {
  // Small dependency-free JSON string extractor for flat Firebase Auth fields.
  // Accepts optional whitespace around the colon, e.g. "idToken":"..."
  // and "idToken" : "...".
  String needle = "\"" + key + "\"";
  int idx = json.indexOf(needle);
  if (idx == -1) return "";

  idx += needle.length();
  while (idx < (int)json.length() && isspace((unsigned char)json[idx])) idx++;
  if (idx >= (int)json.length() || json[idx] != ':') return "";
  idx++;
  while (idx < (int)json.length() && isspace((unsigned char)json[idx])) idx++;
  if (idx >= (int)json.length() || json[idx] != '"') return "";
  idx++;

  String out = "";
  bool escaped = false;
  for (int i = idx; i < (int)json.length(); i++) {
    char c = json[i];
    if (escaped) {
      // Firebase token strings normally need no escaping, but preserve the
      // common JSON escapes rather than terminating early.
      if (c == '"' || c == '\\' || c == '/') out += c;
      else if (c == 'n') out += '\\n';
      else if (c == 'r') out += '\\r';
      else if (c == 't') out += '\\t';
      else out += c;
      escaped = false;
    } else if (c == '\\') {
      escaped = true;
    } else if (c == '"') {
      return out;
    } else {
      out += c;
    }
  }
  return "";
}

bool firebaseSignIn() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[Firebase] Sign-in skipped - WiFi not connected.");
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();  // Phase-1 prototype only; pin CA before deployment.
  client.setTimeout(15000);

  HTTPClient http;
  String url = String("https://identitytoolkit.googleapis.com/v1/accounts:signInWithPassword?key=") + FIREBASE_API_KEY;

  if (!http.begin(client, url)) {
    Serial.println("[Firebase] HTTP begin failed.");
    return false;
  }

  http.setTimeout(15000);
  http.setReuse(false);
  http.useHTTP10(true);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Connection", "close");

  String body = String("{\"email\":\"") + DEVICE_EMAIL +
                String("\",\"password\":\"") + DEVICE_PASSWORD +
                String("\",\"returnSecureToken\":true}");

  int code = http.POST(body);

  if (code == 200) {
    String resp = http.getString();

    g_idToken = extractJsonString(resp, "idToken");
    g_refreshToken = extractJsonString(resp, "refreshToken");
    String expiresInStr = extractJsonString(resp, "expiresIn");
    long expiresIn = expiresInStr.length() ? expiresInStr.toInt() : 3600;

    if (g_idToken.length() > 0) {
      g_tokenExpiryMs = millis() +
          (unsigned long)((expiresIn > 60 ? expiresIn - 60 : expiresIn)) * 1000UL;
      Serial.printf("[Firebase] Signed in OK (device account), token bytes=%d, expires=%ld s.\n",
                    g_idToken.length(), expiresIn);
      http.end();
      return true;
    }

    // Do NOT print resp here: a successful response contains live auth tokens.
    Serial.printf("[Firebase] HTTP 200 but idToken was not found (response bytes=%u).\n",
                  (unsigned)resp.length());
    g_idToken = "";
    g_refreshToken = "";
  } else {
    String errBody = http.getString();
    Serial.printf("[Firebase] Sign-in failed, HTTP %d (%s).\n",
                  code, HTTPClient::errorToString(code).c_str());
    if (errBody.length() > 0) {
      // Firebase error bodies contain the useful reason, but never print the
      // configured password or request body.
      Serial.printf("[Firebase] Error body: %s\n", errBody.c_str());
    }
  }

  http.end();
  return false;
}

bool firebaseRefreshToken() {
  if (WiFi.status() != WL_CONNECTED || g_refreshToken.length() == 0) return false;

  WiFiClientSecure client;
  client.setInsecure();  // Phase-1 prototype only; pin CA before deployment.
  client.setTimeout(15000);

  HTTPClient http;
  String url = String("https://securetoken.googleapis.com/v1/token?key=") + FIREBASE_API_KEY;

  if (!http.begin(client, url)) {
    Serial.println("[Firebase] Refresh HTTP begin failed.");
    return false;
  }

  http.setTimeout(15000);
  http.setReuse(false);
  http.useHTTP10(true);
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  http.addHeader("Connection", "close");

  String body = "grant_type=refresh_token&refresh_token=" + g_refreshToken;
  int code = http.POST(body);

  if (code == 200) {
    String resp = http.getString();
    String newIdToken = extractJsonString(resp, "id_token");
    String newRefreshToken = extractJsonString(resp, "refresh_token");
    String expiresInStr = extractJsonString(resp, "expires_in");
    long expiresIn = expiresInStr.length() ? expiresInStr.toInt() : 3600;

    if (newIdToken.length() > 0) {
      g_idToken = newIdToken;
      if (newRefreshToken.length() > 0) g_refreshToken = newRefreshToken;
      g_tokenExpiryMs = millis() +
          (unsigned long)((expiresIn > 60 ? expiresIn - 60 : expiresIn)) * 1000UL;
      Serial.printf("[Firebase] Token refreshed OK, expires=%ld s.\n", expiresIn);
      http.end();
      return true;
    }

    Serial.printf("[Firebase] HTTP 200 but refreshed id_token was not found (response bytes=%u).\n",
                  (unsigned)resp.length());
  } else {
    String errBody = http.getString();
    Serial.printf("[Firebase] Refresh failed, HTTP %d (%s).\n",
                  code, HTTPClient::errorToString(code).c_str());
    if (errBody.length() > 0) {
      Serial.printf("[Firebase] Refresh error body: %s\n", errBody.c_str());
    }
  }

  http.end();
  return false;
}

bool firebaseReady() {
  return WiFi.status() == WL_CONNECTED && g_idToken.length() > 0 && millis() < g_tokenExpiryMs;
}

void maintainFirebaseAuth() {
  if (WiFi.status() != WL_CONNECTED) return;

  bool needsAuth = (g_idToken.length() == 0) || (millis() > g_tokenExpiryMs);
  if (!needsAuth) return;

  if (millis() - g_lastAuthAttemptMs < AUTH_RETRY_INTERVAL_MS) return;

  g_lastAuthAttemptMs = millis();

  Serial.println("[Firebase] Authentication attempt...");

  bool ok = false;
  if (g_refreshToken.length() > 0) ok = firebaseRefreshToken();
  if (!ok) ok = firebaseSignIn();

  if (!ok) {
    // Do not hammer Firebase. The next attempt is controlled by the normal
    // 30-second auth retry interval.
    Serial.println("[Firebase] Authentication not ready; will retry later.");
  }
}

// ================= FIRESTORE REST FIELD BUILDERS =================
String ffDouble(float v) {
  if (isnan(v) || isinf(v)) return "{\"nullValue\":null}";
  return "{\"doubleValue\":" + String(v, 6) + "}";
}
String ffInt(long v) { return "{\"integerValue\":\"" + String(v) + "\"}"; }
String ffString(const String& s) { return "{\"stringValue\":\"" + s + "\"}"; }
String ffDoubleD(double v) { return "{\"doubleValue\":" + String(v, 6) + "}"; }

String buildFeaturesMap(const float* f) {
  String out = "{\"mapValue\":{\"fields\":{";
  for (int i = 0; i < 23; i++) {
    out += "\"" + String(kFeatureNames[i]) + "\":" + ffDouble(f[i]);
    if (i < 22) out += ",";
  }
  out += "}}}";
  return out;
}

String buildReadingFields(int ni, const String& tsIso, bool dataOk, bool includeLocation) {
  NodeRecord &n = nodes[ni];
  String out = "{\"fields\":{";
  out += "\"node_id\":" + ffInt(n.nodeId) + ",";
  if (tsIso.length() > 0) out += "\"timestamp\":{\"timestampValue\":\"" + tsIso + "\"},";
  out += "\"device_uptime_ms\":" + ffInt((long)millis()) + ",";
  out += "\"rssi\":" + ffInt(n.rssi) + ",";
  out += "\"snr\":" + ffDouble(n.snr) + ",";
  out += "\"deformation_state\":" + ffString(String(kClassNames[n.predictedClass])) + ",";
  out += "\"confidence\":" + ffDouble(n.confidence) + ",";
  out += "\"data_quality\":" + ffString(dataOk ? "OK" : "PARTIAL") + ",";
  if (includeLocation) {
    double lat, lon;
    const char* siteLabel;
    if (getNodeLocation(n.nodeId, lat, lon, siteLabel)) {
      out += "\"latitude\":" + ffDoubleD(lat) + ",";
      out += "\"longitude\":" + ffDoubleD(lon) + ",";
      // Explicitly not "gps" - this is a hand-set reference-site pin, not
      // a live fix. Keeps the frontend/jury from ever mistaking this for
      // hardware physically installed at the named site.
      out += "\"location_source\":" + ffString("hardcoded_reference_site_no_gps") + ",";
      out += "\"site_label\":" + ffString(String(siteLabel)) + ",";
    }
  }
  out += "\"features\":" + buildFeaturesMap(n.features);
  out += "}}";
  return out;
}

// Best-effort push: 1) appends to the "readings" history collection
// (auto-generated document ID -> used for charts/CSV export), and
// 2) overwrites "nodes_latest/{node_id}" (used for the live-status
// tiles on the website). Both are skipped instantly - with no delay -
// if WiFi/auth isn't ready; the USB->Node-RED path above is completely
// unaffected either way.
void pushReadingAndLatest(int ni) {
  if (!firebaseReady()) {
    Serial.println("[Firestore] Skipped push (offline or not authenticated yet) - best-effort only.");
    return;
  }
  bool dataOk = !anyInvalidFeature(nodes[ni].features);
  String tsIso = isoTimestampNow();
  // History never carries location — it's static install metadata, not a
  // time-series value, and repeating it on every packet would bloat every
  // stored reading for no benefit.
  String fieldsHistory = buildReadingFields(ni, tsIso, dataOk, false);
  // The live snapshot DOES carry location - it's what a future GIS/map
  // page on the website would read.
  String fieldsLatest = buildReadingFields(ni, tsIso, dataOk, true);

  WiFiClientSecure client;
  client.setInsecure();

  // 1) history: readings/{auto-id}
  {
    HTTPClient http;
    String url = String("https://firestore.googleapis.com/v1/projects/") + FIREBASE_PROJECT_ID +
                 "/databases/(default)/documents/readings";
    http.begin(client, url);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", "Bearer " + g_idToken);
    int code = http.POST(fieldsHistory);
    if (code != 200) {
      Serial.printf("[Firestore] readings POST failed, HTTP %d: %s\n", code, http.getString().c_str());
    }
    http.end();
  }

  // 2) live snapshot: nodes_latest/{node_id}  (PATCH with no fieldMask = full overwrite)
  {
    HTTPClient http;
    String url = String("https://firestore.googleapis.com/v1/projects/") + FIREBASE_PROJECT_ID +
                 "/databases/(default)/documents/nodes_latest/" + String(nodes[ni].nodeId);
    http.begin(client, url);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", "Bearer " + g_idToken);
    int code = http.PATCH(fieldsLatest);
    if (code != 200 && code != 201) {
      Serial.printf("[Firestore] nodes_latest PATCH failed, HTTP %d: %s\n", code, http.getString().c_str());
    } else {
      Serial.println("[Firestore] Push OK (history + latest snapshot).");
    }
    http.end();
  }
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

void page1_last_node_verdict() {
  drawHeader("AI VERDICT");
  oled.setCursor(0, 16);
  if (lastRxNodeIdx < 0) { oled.println("No packet yet"); return; }
  NodeRecord &n = nodes[lastRxNodeIdx];
  oled.printf("N%d: %s\n", n.nodeId, kClassNames[n.predictedClass]);
  oled.printf("Conf: %d%%\n", (int)(n.confidence * 100));
  oled.printf("Crk:%.1f Vrt:%.1f\n", n.features[0], n.features[1]);
  oled.printf("Temp:%.1fC\n", n.features[7]);
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

void page4_firebase_status() {
  drawHeader("WIFI/FIRESTORE");
  oled.setCursor(0, 16);
  oled.printf("WiFi : %s\n", WiFi.status() == WL_CONNECTED ? "OK" : "OFFLINE");
  oled.printf("Auth : %s\n", firebaseReady() ? "OK" : "NO");
  oled.printf("Time : %s\n", g_timeSynced ? "SYNCED" : "NO NTP");
  oled.println("USB->NodeRED:ALWAYS");
}

void showPage(uint8_t p) {
  oled.clearDisplay();
  switch (p % NUM_PAGES) {
    case 0: page0_overview();          break;
    case 1: page1_last_node_verdict(); break;
    case 2: page2_radio_stats();       break;
    case 3: page4_firebase_status();   break;
  }
  oled.display();
}

// ================= SETUP =================
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== SENSE-MINE COMMAND CENTRE (PHASE-1 PROTOTYPE) BOOT ===");
  Serial.println("[INFO] USB JSON path (Node-RED) is unconditional.");
  Serial.println("[INFO] WiFi->Firestore path is best-effort; see OLED page 5 for live status.");

  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  oledOK = oled.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  if (oledOK) {
    oled.clearDisplay();
    oled.setTextColor(SSD1306_WHITE);
    oled.setCursor(0, 0);
    oled.println("SENSE-MINE CMD CTR");
    oled.println("Phase-1 Boot");
    oled.display();
  } else {
    Serial.println("[OLED] init FAILED - check wiring / I2C address");
  }

  SPI.begin(PIN_LORA_SCK, PIN_LORA_MISO, PIN_LORA_MOSI, PIN_LORA_NSS);
  LoRa.setPins(PIN_LORA_NSS, PIN_LORA_RST, PIN_LORA_DIO0);
  loraOK = LoRa.begin(LORA_FREQ);
  Serial.printf("[LoRa] init %s\n", loraOK ? "OK" : "FAIL (check VSPI wiring / antenna)");

  // Kick off WiFi connect attempt - non-blocking, best-effort. The
  // USB->Node-RED path above does not wait on any of this.
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  wifiConnecting = true;
  wifiConnectStartedMs = millis();
  lastWifiAttemptMs = millis();

  delay(500);
}

// ================= LOOP =================
void loop() {
  int packetSize = LoRa.parsePacket();
  if (packetSize) {
    String pkt = "";
    while (LoRa.available()) pkt += (char)LoRa.read();
    int rssi  = LoRa.packetRssi();
    float snr = LoRa.packetSnr();
    parsePacket(pkt, rssi, snr);
  }

  // Best-effort WiFi/Firestore maintenance - all non-blocking, all
  // rate-limited internally, none of it can stall the LoRa loop above.
  maintainWiFi();
  syncTimeIfNeeded();
  maintainFirebaseAuth();

  if (oledOK && millis() - lastPageSwitch > PAGE_INTERVAL_MS) {
    currentPage = (currentPage + 1) % NUM_PAGES;
    lastPageSwitch = millis();
  }
  if (oledOK) showPage(currentPage);

  delay(50);
}


// /* ============================================================
//    SENSE-MINE  |  COMMAND CENTRE — PHASE-1 PROTOTYPE FIRMWARE
//    Board   : ESP32 DevKitC / WROOM-32
//    Purpose : Receive each node's classification + raw features
//              over LoRa, and fan the data out over TWO INDEPENDENT
//              paths (neither one waits on the other):

//    DATA PATH (updated — supersedes the older "Node-RED owns
//    Firestore" comment from the previous revision of this file)
//    ----------------------------------------------------------
//    Sensor Node --LoRa--> Command Centre
//                              |
//                              +--USB Serial (JSON, always)--> Node-RED (local dashboard)
//                              |
//                              +--WiFi/HTTPS (best-effort)-----> Firestore directly --> Website

//    - The USB-Serial -> Node-RED path is UNCONDITIONAL. It never
//      waits on WiFi, Firestore auth, or anything network-related.
//      If WiFi/Firebase is down, this path is completely unaffected.
//    - The WiFi -> Firestore path is BEST-EFFORT. If there's no
//      WiFi, or Firebase auth hasn't completed yet, the push is
//      silently skipped (logged to Serial only) and the loop moves
//      on immediately — it never blocks LoRa reception.
//    - This is a genuine phase-1 prototype simplification vs. the
//      DPR's final architecture (FastAPI + PostgreSQL/TimescaleDB).
//      Firestore here plays the role that Postgres plays in the
//      final design. Swap it out later without touching the node
//      firmware or the LoRa protocol.

//    FIREBASE AUTH MODEL (Option B — dedicated device account)
//    ----------------------------------------------------------
//    This board signs in to Firebase Auth with a FIXED email/
//    password created just for it (not a real mailbox — just a
//    fixed identity string). Firestore rules then check
//    request.auth.token.email == that fixed address, so only this
//    board (or anything else holding those credentials) can write.
//    Anonymous website visitors get their own anonymous auth
//    session (see the frontend prompt) which has auth != null but
//    NO email claim, so the rule correctly lets them read but not
//    write. See the "Firestore rules" note delivered alongside this
//    file for the exact rules text to paste into the Firebase
//    console.

//    BEFORE FLASHING — create the device account once:
//      Firebase Console -> Authentication -> Sign-in method
//        -> enable "Email/Password"  AND  enable "Anonymous"
//      Firebase Console -> Authentication -> Users -> Add user
//        -> email:    (same as DEVICE_EMAIL below)
//        -> password: (same as DEVICE_PASSWORD below)

//    INVALID-READING HANDLING (the actual bug this revision fixes)
//    ----------------------------------------------------------
//    The node firmware can legitimately send NaN for a feature
//    (e.g. tilt_x/tilt_y when the MPU read fails, temperature_C
//    when the DS18B20 disconnects). Arduino's float printing turns
//    NaN into the literal text "nan", which is NOT valid JSON —
//    that is what was making Node-RED's JSON.parse() throw in
//    debug. Every numeric value this board emits (over BOTH the
//    USB path and the Firestore path) now goes through jsonNum(),
//    which converts NaN/Inf into a proper JSON `null` instead. A
//    top-level "data_quality" field ("OK" / "PARTIAL") is also
//    added so Node-RED/the website can flag a reading without the
//    parse itself ever failing.

//    FROZEN PIN MAP (unchanged from hardware-test build)
//    ----------------------------------------------------------
//    GPIO 18 : SPI SCK   - SX1278 LoRa (VSPI)
//    GPIO 19 : SPI MISO  - SX1278 LoRa (VSPI)
//    GPIO 23 : SPI MOSI  - SX1278 LoRa (VSPI)
//    GPIO 5  : SPI NSS   - SX1278 LoRa
//    GPIO 14 : LoRa RESET
//    GPIO 26 : LoRa DIO0
//    GPIO 16 : UART2 RX  <- SIM800L TXD
//    GPIO 17 : UART2 TX  -> SIM800L RXD
//    GPIO 27 : SIM800L RESET (optional)
//    GPIO 21 : I2C SDA   - OLED
//    GPIO 22 : I2C SCL   - OLED

//    IMPORTANT (repeated from architecture notes):
//    - SIM800L VCC must come from an external ~4.0V regulated
//      supply capable of high current pulses — NOT the ESP32
//      3.3V pin. Add a 1000-2200uF bulk cap + 100nF ceramic right
//      at its VCC/GND pins if you see corrupted/blank AT responses.
//    - This build's SIM800_BAUD is 115200, matching your confirmed
//      hardware - change it back to 9600 if you swap modules.
//    - WiFiClientSecure below uses setInsecure() (no certificate
//      pinning) to keep this phase-1 build simple. That's an
//      accepted, stated shortcut for the prototype — pin Google's
//      root CA (or use the ESP32 core's cert bundle) before this
//      goes anywhere near a real deployment.

//    LIBRARIES REQUIRED (Library Manager + built-in ESP32 core)
//    ----------------------------------------------------------
//    - LoRa by sandeepmistry
//    - Adafruit GFX Library
//    - Adafruit SSD1306
//    - WiFi, WiFiClientSecure, HTTPClient  (bundled with ESP32 core)
//    ============================================================ */

// /* ============================================================
//    SENSE-MINE  |  COMMAND CENTRE — PHASE-1 PROTOTYPE FIRMWARE
//    Board   : ESP32 DevKitC / WROOM-32
//    Purpose : Receive each node's classification + raw features
//              over LoRa, and fan the data out over TWO INDEPENDENT
//              paths (neither one waits on the other):

//    DATA PATH (updated — supersedes the older "Node-RED owns
//    Firestore" comment from the previous revision of this file)
//    ----------------------------------------------------------
//    Sensor Node --LoRa--> Command Centre
//                              |
//                              +--USB Serial (JSON, always)--> Node-RED (local dashboard)
//                              |
//                              +--WiFi/HTTPS (best-effort)-----> Firestore directly --> Website

//    - The USB-Serial -> Node-RED path is UNCONDITIONAL. It never
//      waits on WiFi, Firestore auth, or anything network-related.
//      If WiFi/Firebase is down, this path is completely unaffected.
//    - The WiFi -> Firestore path is BEST-EFFORT. If there's no
//      WiFi, or Firebase auth hasn't completed yet, the push is
//      silently skipped (logged to Serial only) and the loop moves
//      on immediately — it never blocks LoRa reception.
//    - This is a genuine phase-1 prototype simplification vs. the
//      DPR's final architecture (FastAPI + PostgreSQL/TimescaleDB).
//      Firestore here plays the role that Postgres plays in the
//      final design. Swap it out later without touching the node
//      firmware or the LoRa protocol.

//    FIREBASE AUTH MODEL (Option B — dedicated device account)
//    ----------------------------------------------------------
//    This board signs in to Firebase Auth with a FIXED email/
//    password created just for it (not a real mailbox — just a
//    fixed identity string). Firestore rules then check
//    request.auth.token.email == that fixed address, so only this
//    board (or anything else holding those credentials) can write.
//    Anonymous website visitors get their own anonymous auth
//    session (see the frontend prompt) which has auth != null but
//    NO email claim, so the rule correctly lets them read but not
//    write. See the "Firestore rules" note delivered alongside this
//    file for the exact rules text to paste into the Firebase
//    console.

//    BEFORE FLASHING — create the device account once:
//      Firebase Console -> Authentication -> Sign-in method
//        -> enable "Email/Password"  AND  enable "Anonymous"
//      Firebase Console -> Authentication -> Users -> Add user
//        -> email:    (same as DEVICE_EMAIL below)
//        -> password: (same as DEVICE_PASSWORD below)

//    INVALID-READING HANDLING (the actual bug this revision fixes)
//    ----------------------------------------------------------
//    The node firmware can legitimately send NaN for a feature
//    (e.g. tilt_x/tilt_y when the MPU read fails, temperature_C
//    when the DS18B20 disconnects). Arduino's float printing turns
//    NaN into the literal text "nan", which is NOT valid JSON —
//    that is what was making Node-RED's JSON.parse() throw in
//    debug. Every numeric value this board emits (over BOTH the
//    USB path and the Firestore path) now goes through jsonNum(),
//    which converts NaN/Inf into a proper JSON `null` instead. A
//    top-level "data_quality" field ("OK" / "PARTIAL") is also
//    added so Node-RED/the website can flag a reading without the
//    parse itself ever failing.

//    FROZEN PIN MAP (unchanged from hardware-test build)
//    ----------------------------------------------------------
//    GPIO 18 : SPI SCK   - SX1278 LoRa (VSPI)
//    GPIO 19 : SPI MISO  - SX1278 LoRa (VSPI)
//    GPIO 23 : SPI MOSI  - SX1278 LoRa (VSPI)
//    GPIO 5  : SPI NSS   - SX1278 LoRa
//    GPIO 14 : LoRa RESET
//    GPIO 26 : LoRa DIO0
//    GPIO 16 : UART2 RX  <- SIM800L TXD
//    GPIO 17 : UART2 TX  -> SIM800L RXD
//    GPIO 27 : SIM800L RESET (optional)
//    GPIO 21 : I2C SDA   - OLED
//    GPIO 22 : I2C SCL   - OLED

//    IMPORTANT (repeated from architecture notes):
//    - SIM800L VCC must come from an external ~4.0V regulated
//      supply capable of high current pulses — NOT the ESP32
//      3.3V pin. Add a 1000-2200uF bulk cap + 100nF ceramic right
//      at its VCC/GND pins if you see corrupted/blank AT responses.
//    - This build's SIM800_BAUD is 115200, matching your confirmed
//      hardware - change it back to 9600 if you swap modules.
//    - WiFiClientSecure below uses setInsecure() (no certificate
//      pinning) to keep this phase-1 build simple. That's an
//      accepted, stated shortcut for the prototype — pin Google's
//      root CA (or use the ESP32 core's cert bundle) before this
//      goes anywhere near a real deployment.

//    LIBRARIES REQUIRED (Library Manager + built-in ESP32 core)
//    ----------------------------------------------------------
//    - LoRa by sandeepmistry
//    - Adafruit GFX Library
//    - Adafruit SSD1306
//    - WiFi, WiFiClientSecure, HTTPClient  (bundled with ESP32 core)
//    ============================================================ */

// #include <SPI.h>
// #include <LoRa.h>
// #include <Wire.h>
// #include <Adafruit_GFX.h>
// #include <Adafruit_SSD1306.h>
// #include <HardwareSerial.h>
// #include <WiFi.h>
// #include <WiFiClientSecure.h>
// #include <HTTPClient.h>
// #include <time.h>

// // ---------------- PIN MAP ----------------
// #define PIN_LORA_SCK   18
// #define PIN_LORA_MISO  19
// #define PIN_LORA_MOSI  23
// #define PIN_LORA_NSS   5
// #define PIN_LORA_RST   14
// #define PIN_LORA_DIO0  26

// #define PIN_SIM_RX     16
// #define PIN_SIM_TX     17
// #define PIN_SIM_RST    27

// #define PIN_I2C_SDA    21
// #define PIN_I2C_SCL    22

// #define LORA_FREQ      433E6
// #define SIM800_BAUD    115200

// // ---------------- TODO: fill in before relying on SMS alerts ----------------
// #define ALERT_PHONE_NUMBER "+917303609777"
// const unsigned long ALERT_COOLDOWN_MS = 10UL * 60UL * 1000UL;  // 10 min between repeat alerts per node

// // ======================================================================
// // TODO: EDIT THESE FOUR BLOCKS BEFORE FLASHING
// // ======================================================================
// // 1) Your WiFi (best-effort — the USB->Node-RED path works with none of this filled in)
// #define WIFI_SSID            "Airtel_Lakshya"
// #define WIFI_PASSWORD         "Lakshya@2005"

// // 2) From your firebaseConfig (apiKey + projectId) — already filled in from what you shared
// #define FIREBASE_API_KEY     "AIzaSyBwdXQLuA4vffr9BPH9egR6FIRTyd7bgyA"
// #define FIREBASE_PROJECT_ID  "mine-subsidence-monitoring"

// // 3) The fixed device account you create once in the Firebase console (Option B)
// #define DEVICE_EMAIL         "indicmindofficial@gmail.com"
// #define DEVICE_PASSWORD      "Lakshya@@@2005"

// // 4) GIS — no GPS hardware yet, so each node's install location is
// //    hardcoded here instead. IMPORTANT: this is set to the geographic
// //    centroid of the Pachhwara North coal block, Pakur District,
// //    Jharkhand (bounding box 24°32'12.771"N-24°33'53.683"N,
// //    87°25'18.208"E-87°29'55.417"E, centroid computed below) - a real,
// //    named Coal India-linked site, used here as a REFERENCE/TARGET
// //    deployment site for the demo narrative.
// //    This is NOT where the physical testbed actually sits (that's your
// //    lab/college). "location_source" and "site_label" below both say
// //    so explicitly, so nothing downstream - Node-RED, the website, or
// //    a jury member reading the raw Firestore data - can mistake this
// //    for a real GPS fix from hardware physically installed at
// //    Pachhwara North. If you ever want the pin to instead show where
// //    the testbed itself sits, swap in real coordinates for that and
// //    change location_source/site_label accordingly.
// struct NodeLocation { int nodeId; double lat; double lon; const char* siteLabel; };
// const NodeLocation kNodeLocations[] = {
//   { 1, 24.550896, 87.460226, "Pachhwara North Coal Block, Pakur, Jharkhand (reference site - testbed not physically deployed here)" },
// };
// const int kNodeLocationCount = sizeof(kNodeLocations) / sizeof(kNodeLocations[0]);

// bool getNodeLocation(int nodeId, double &lat, double &lon, const char* &siteLabel) {
//   for (int i = 0; i < kNodeLocationCount; i++) {
//     if (kNodeLocations[i].nodeId == nodeId) {
//       lat = kNodeLocations[i].lat;
//       lon = kNodeLocations[i].lon;
//       siteLabel = kNodeLocations[i].siteLabel;
//       return true;
//     }
//   }
//   return false;
// }
// // ======================================================================

// HardwareSerial sim800(2);
// Adafruit_SSD1306 oled(128, 64, &Wire, -1);

// bool oledOK = false, loraOK = false, simOK = false;

// // local copy of class names/order - matches model_rf_compact.cpp exactly.
// // (command centre doesn't compile the model, so it keeps its own small
// // lookup table instead of needing the model files at all.)
// static const char* kClassNames[6] = {
//   "ACCELERATING", "INITIATION", "PROGRESSIVE", "RAPID", "STABILIZING", "STABLE"
// };
// bool isHighRisk(uint8_t classId) {
//   // ACCELERATING(0), PROGRESSIVE(2), RAPID(3) -> alert-worthy
//   return classId == 0 || classId == 2 || classId == 3;
// }

// // feature names, in the exact order the node sends them, for JSON keys
// static const char* kFeatureNames[23] = {
//   "crack_width_mm", "vertical_displacement_mm", "tilt_x_deg", "tilt_y_deg",
//   "vibration_rms_g", "vibration_peak_g", "dominant_frequency_hz", "temperature_C",
//   "crack_growth_rate_mm_day", "vertical_displacement_velocity_mm_day",
//   "displacement_acceleration_mm_day2", "tilt_rate_deg_day",
//   "cumulative_crack_growth_mm", "cumulative_subsidence_mm",
//   "spatial_displacement_gradient", "spatial_coherence",
//   "mining_depth_m", "seam_thickness_m", "mining_height_m",
//   "panel_width_m", "panel_length_m", "face_advance_rate_m_day", "extraction_ratio"
// };

// // ---------------- per-node record table ----------------
// #define MAX_NODES 5
// struct NodeRecord {
//   bool     seen = false;
//   int      nodeId = -1;
//   unsigned long lastSeenMs = 0;
//   unsigned long lastAlertMs = 0;
//   int      rssi = 0;
//   float    snr = 0;
//   uint8_t  predictedClass = 0;
//   float    confidence = 0;
//   float    features[23];
//   uint32_t packetCount = 0;
// };
// NodeRecord nodes[MAX_NODES];
// int knownNodeCount = 0;
// uint32_t totalPacketsReceived = 0;
// int lastRxNodeIdx = -1;

// // ---------------- OLED circular page cycling ----------------
// #define NUM_PAGES 5
// uint8_t currentPage = 0;
// unsigned long lastPageSwitch = 0;
// const unsigned long PAGE_INTERVAL_MS = 3000;

// // ---------------- WiFi / Firebase state (all best-effort) ----------------
// unsigned long lastWifiAttemptMs = 0;
// unsigned long wifiConnectStartedMs = 0;
// const unsigned long WIFI_RETRY_INTERVAL_MS = 15000;
// const unsigned long WIFI_CONNECT_TIMEOUT_MS = 12000;
// bool wifiConnecting = false;

// String g_idToken = "";
// String g_refreshToken = "";
// unsigned long g_tokenExpiryMs = 0;
// unsigned long g_lastAuthAttemptMs = 0;
// const unsigned long AUTH_RETRY_INTERVAL_MS = 30000;

// bool g_timeSynced = false;
// bool g_ntpStarted = false;

// // ---------------- helpers ----------------
// int getNodeIndex(int id) {
//   for (int i = 0; i < knownNodeCount; i++) if (nodes[i].nodeId == id) return i;
//   if (knownNodeCount < MAX_NODES) {
//     nodes[knownNodeCount].nodeId = id;
//     nodes[knownNodeCount].seen = true;
//     return knownNodeCount++;
//   }
//   Serial.println("[WARN] Node table full, discarding new node ID.");
//   return -1;
// }

// // Converts a float to a JSON-safe string: NaN/Inf (e.g. a failed sensor
// // read from the node) becomes the JSON literal null instead of the
// // non-JSON text "nan" that Arduino's default float printing produces.
// // This is the fix for the "Node-RED debug shows a parse error" issue.
// String jsonNum(float v, int decimals) {
//   if (isnan(v) || isinf(v)) return "null";
//   return String(v, decimals);
// }

// bool anyInvalidFeature(const float* f) {
//   for (int i = 0; i < 23; i++) {
//     if (isnan(f[i]) || isinf(f[i])) return true;
//   }
//   return false;
// }

// // CSV format from node: NODE_ID,millis,predicted_class_id,confidence,f1..f23
// #define EXPECTED_FIELD_COUNT 27   // 4 header fields + 23 raw features

// void printNodeJSON(int ni);
// void maybeSendAlert(int ni);
// void pushReadingAndLatest(int ni);

// void parsePacket(String pkt, int rssi, float snr) {
//   // TEMPORARY LoRa RX DIAGNOSTIC: print exactly what the radio received
//   // before parsing. This does NOT change the node firmware or packet format.
//   Serial.println("[LoRa] RAW RX:");
//   Serial.println(pkt);
//   Serial.printf("[LoRa] RX length = %d bytes\n", pkt.length());

//   int idx = -1;
//   int classId = -1;
//   float confidence = 0;
//   float feats[23];
//   int fieldCount = 0;
//   int start = 0;

//   for (int i = 0; i <= (int)pkt.length(); i++) {
//     if (i == (int)pkt.length() || pkt[i] == ',') {
//       String tok = pkt.substring(start, i);
//       if (fieldCount == 0) idx = tok.toInt();
//       else if (fieldCount == 1) { /* millis from node - not currently used here */ }
//       else if (fieldCount == 2) classId = tok.toInt();
//       else if (fieldCount == 3) confidence = tok.toFloat();
//       else if (fieldCount >= 4 && fieldCount <= 26) feats[fieldCount - 4] = tok.toFloat();
//       fieldCount++;
//       start = i + 1;
//     }
//   }

//   if (idx < 0 || fieldCount != EXPECTED_FIELD_COUNT || classId < 0 || classId > 5) {
//     Serial.printf("[LoRa] Malformed packet, discarded (fields=%d, expected=%d).\n",
//                   fieldCount, EXPECTED_FIELD_COUNT);
//     Serial.println("[LoRa] Node firmware is expected to send 27 CSV fields.");
//     return;
//   }

//   int ni = getNodeIndex(idx);
//   if (ni < 0) return;

//   nodes[ni].lastSeenMs     = millis();
//   nodes[ni].rssi           = rssi;
//   nodes[ni].snr            = snr;
//   nodes[ni].predictedClass = (uint8_t)classId;
//   nodes[ni].confidence     = confidence;
//   memcpy(nodes[ni].features, feats, sizeof(feats));
//   nodes[ni].packetCount++;
//   totalPacketsReceived++;
//   lastRxNodeIdx = ni;

//   Serial.printf("---- PACKET FROM NODE %d ----\n", idx);
//   Serial.printf("RSSI: %d dBm   SNR: %.1f dB\n", rssi, snr);
//   Serial.printf("STATE: %s  (confidence %.0f%%)\n",
//                 kClassNames[nodes[ni].predictedClass], nodes[ni].confidence * 100);

//   printNodeJSON(ni);        // <-- Node-RED reads this line (unconditional, USB)
//   maybeSendAlert(ni);       // <-- independent SIM800L safety channel
//   pushReadingAndLatest(ni); // <-- best-effort WiFi->Firestore path (skips instantly if offline)
// }

// // one compact JSON object per line, e.g.:
// // {"node_id":1,"rssi":-82,"snr":9.5,"deformation_state":"STABLE","confidence":0.90,
// //  "data_quality":"OK","features":{"crack_width_mm":12.340,...}}
// // All numeric fields are JSON-safe (see jsonNum) - Node-RED's JSON.parse()
// // will never throw on a NaN/Inf reading from the node.
// void printNodeJSON(int ni) {
//   NodeRecord &n = nodes[ni];
//   bool dataOk = !anyInvalidFeature(n.features);
//   Serial.print("{\"node_id\":"); Serial.print(n.nodeId);
//   Serial.print(",\"rssi\":"); Serial.print(n.rssi);
//   Serial.print(",\"snr\":"); Serial.print(jsonNum(n.snr, 1));
//   Serial.print(",\"deformation_state\":\""); Serial.print(kClassNames[n.predictedClass]); Serial.print("\"");
//   Serial.print(",\"confidence\":"); Serial.print(jsonNum(n.confidence, 3));
//   Serial.print(",\"data_quality\":\""); Serial.print(dataOk ? "OK" : "PARTIAL"); Serial.print("\"");
//   Serial.print(",\"features\":{");
//   for (int i = 0; i < 23; i++) {
//     Serial.print("\""); Serial.print(kFeatureNames[i]); Serial.print("\":");
//     Serial.print(jsonNum(n.features[i], 4));
//     if (i < 22) Serial.print(",");
//   }
//   Serial.println("}}");
// }

// // ---------------- SIM800L bare AT driver ----------------
// bool simSendAT(const char* cmd, const char* expect, unsigned long timeoutMs) {
//   while (sim800.available()) sim800.read();
//   sim800.print(cmd);
//   sim800.print("\r\n");
//   unsigned long t0 = millis();
//   String resp = "";
//   while (millis() - t0 < timeoutMs) {
//     while (sim800.available()) resp += (char)sim800.read();
//     if (resp.indexOf(expect) != -1) return true;
//   }
//   Serial.printf("[SIM800L] '%s' -> expected '%s', got: %s\n", cmd, expect, resp.c_str());
//   return false;
// }

// bool sendSMS(const char* number, const String &message) {
//   if (!simOK) {
//     Serial.println("[SIM800L] Skipping SMS - modem not responding.");
//     return false;
//   }
//   if (!simSendAT("AT+CMGF=1", "OK", 2000)) return false;   // text mode

//   while (sim800.available()) sim800.read();
//   sim800.print("AT+CMGS=\"");
//   sim800.print(number);
//   sim800.print("\"\r\n");

//   unsigned long t0 = millis();
//   String resp = "";
//   while (millis() - t0 < 3000) {
//     while (sim800.available()) resp += (char)sim800.read();
//     if (resp.indexOf(">") != -1) break;
//   }
//   if (resp.indexOf(">") == -1) {
//     Serial.println("[SIM800L] Never got '>' prompt for AT+CMGS - aborting SMS.");
//     return false;
//   }

//   sim800.print(message);
//   sim800.write(0x1A);   // Ctrl+Z ends the message body

//   t0 = millis();
//   resp = "";
//   while (millis() - t0 < 10000) {
//     while (sim800.available()) resp += (char)sim800.read();
//     if (resp.indexOf("+CMGS:") != -1 || resp.indexOf("OK") != -1) {
//       Serial.println("[SIM800L] SMS sent.");
//       return true;
//     }
//     if (resp.indexOf("ERROR") != -1) {
//       Serial.println("[SIM800L] SMS send reported ERROR.");
//       return false;
//     }
//   }
//   Serial.println("[SIM800L] SMS send timed out with no confirmation.");
//   return false;
// }

// void maybeSendAlert(int ni) {
//   NodeRecord &n = nodes[ni];
//   if (!isHighRisk(n.predictedClass)) return;
//   if (millis() - n.lastAlertMs < ALERT_COOLDOWN_MS) return;   // debounce repeat alerts

//   String msg = "SENSE-MINE ALERT: Node " + String(n.nodeId) + " state=" +
//                String(kClassNames[n.predictedClass]) + " conf=" +
//                String((int)(n.confidence * 100)) + "%";
//   Serial.println("[ALERT] " + msg);
//   if (sendSMS(ALERT_PHONE_NUMBER, msg)) {
//     n.lastAlertMs = millis();
//   }
// }

// // ================= WiFi / TIME (all non-blocking, best-effort) =================
// void maintainWiFi() {
//   wl_status_t status = WiFi.status();

//   if (status == WL_CONNECTED) {
//     if (wifiConnecting) {
//       wifiConnecting = false;
//       Serial.print("[WiFi] Connected. IP: ");
//       Serial.println(WiFi.localIP());
//     }
//     return;
//   }

//   // We already called WiFi.begin(). Do NOT call WiFi.begin() again while
//   // the ESP32 is still trying to associate; that is what caused:
//   // "sta is connecting, cannot set config".
//   if (wifiConnecting) {
//     if (millis() - wifiConnectStartedMs >= WIFI_CONNECT_TIMEOUT_MS) {
//       Serial.println("[WiFi] Connection attempt timed out; restarting cleanly.");
//       WiFi.disconnect(false, false);
//       wifiConnecting = false;
//       lastWifiAttemptMs = millis();
//     }
//     return;
//   }

//   if (millis() - lastWifiAttemptMs < WIFI_RETRY_INTERVAL_MS) return;

//   lastWifiAttemptMs = millis();
//   wifiConnectStartedMs = millis();
//   wifiConnecting = true;

//   Serial.printf("[WiFi] Starting connection to %s...\n", WIFI_SSID);
//   WiFi.mode(WIFI_STA);
//   WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
// }

// void syncTimeIfNeeded() {
//   if (g_timeSynced || WiFi.status() != WL_CONNECTED) return;
//   if (!g_ntpStarted) {
//     configTime(0, 0, "pool.ntp.org", "time.nist.gov");
//     g_ntpStarted = true;
//     Serial.println("[NTP] Sync requested...");
//   }
//   time_t now = time(nullptr);
//   if (now > 1700000000) {  // sanity check: after ~Nov 2023, i.e. a real sync happened
//     g_timeSynced = true;
//     Serial.println("[NTP] Time synced.");
//   }
// }

// String isoTimestampNow() {
//   if (!g_timeSynced) return "";   // caller omits the timestamp field if empty
//   time_t now = time(nullptr);
//   struct tm timeinfo;
//   gmtime_r(&now, &timeinfo);
//   char buf[25];
//   strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &timeinfo);
//   return String(buf);
// }

// // ================= FIREBASE AUTH (Option B — fixed device account) =================
// // Minimal hand-rolled extractor for the flat string fields Firebase's
// // REST auth endpoints return - avoids pulling in a full JSON library
// // for two small responses.
// String extractJsonString(const String& json, const String& key) {
//   // Small dependency-free JSON string extractor for flat Firebase Auth fields.
//   // Accepts optional whitespace around the colon, e.g. "idToken":"..."
//   // and "idToken" : "...".
//   String needle = "\"" + key + "\"";
//   int idx = json.indexOf(needle);
//   if (idx == -1) return "";

//   idx += needle.length();
//   while (idx < (int)json.length() && isspace((unsigned char)json[idx])) idx++;
//   if (idx >= (int)json.length() || json[idx] != ':') return "";
//   idx++;
//   while (idx < (int)json.length() && isspace((unsigned char)json[idx])) idx++;
//   if (idx >= (int)json.length() || json[idx] != '"') return "";
//   idx++;

//   String out = "";
//   bool escaped = false;
//   for (int i = idx; i < (int)json.length(); i++) {
//     char c = json[i];
//     if (escaped) {
//       // Firebase token strings normally need no escaping, but preserve the
//       // common JSON escapes rather than terminating early.
//       if (c == '"' || c == '\\' || c == '/') out += c;
//       else if (c == 'n') out += '\\n';
//       else if (c == 'r') out += '\\r';
//       else if (c == 't') out += '\\t';
//       else out += c;
//       escaped = false;
//     } else if (c == '\\') {
//       escaped = true;
//     } else if (c == '"') {
//       return out;
//     } else {
//       out += c;
//     }
//   }
//   return "";
// }

// bool firebaseSignIn() {
//   if (WiFi.status() != WL_CONNECTED) {
//     Serial.println("[Firebase] Sign-in skipped - WiFi not connected.");
//     return false;
//   }

//   WiFiClientSecure client;
//   client.setInsecure();  // Phase-1 prototype only; pin CA before deployment.
//   client.setTimeout(15000);

//   HTTPClient http;
//   String url = String("https://identitytoolkit.googleapis.com/v1/accounts:signInWithPassword?key=") + FIREBASE_API_KEY;

//   if (!http.begin(client, url)) {
//     Serial.println("[Firebase] HTTP begin failed.");
//     return false;
//   }

//   http.setTimeout(15000);
//   http.setReuse(false);
//   http.useHTTP10(true);
//   http.addHeader("Content-Type", "application/json");
//   http.addHeader("Connection", "close");

//   String body = String("{\"email\":\"") + DEVICE_EMAIL +
//                 String("\",\"password\":\"") + DEVICE_PASSWORD +
//                 String("\",\"returnSecureToken\":true}");

//   int code = http.POST(body);

//   if (code == 200) {
//     String resp = http.getString();

//     g_idToken = extractJsonString(resp, "idToken");
//     g_refreshToken = extractJsonString(resp, "refreshToken");
//     String expiresInStr = extractJsonString(resp, "expiresIn");
//     long expiresIn = expiresInStr.length() ? expiresInStr.toInt() : 3600;

//     if (g_idToken.length() > 0) {
//       g_tokenExpiryMs = millis() +
//           (unsigned long)((expiresIn > 60 ? expiresIn - 60 : expiresIn)) * 1000UL;
//       Serial.printf("[Firebase] Signed in OK (device account), token bytes=%d, expires=%ld s.\n",
//                     g_idToken.length(), expiresIn);
//       http.end();
//       return true;
//     }

//     // Do NOT print resp here: a successful response contains live auth tokens.
//     Serial.printf("[Firebase] HTTP 200 but idToken was not found (response bytes=%u).\n",
//                   (unsigned)resp.length());
//     g_idToken = "";
//     g_refreshToken = "";
//   } else {
//     String errBody = http.getString();
//     Serial.printf("[Firebase] Sign-in failed, HTTP %d (%s).\n",
//                   code, HTTPClient::errorToString(code).c_str());
//     if (errBody.length() > 0) {
//       // Firebase error bodies contain the useful reason, but never print the
//       // configured password or request body.
//       Serial.printf("[Firebase] Error body: %s\n", errBody.c_str());
//     }
//   }

//   http.end();
//   return false;
// }

// bool firebaseRefreshToken() {
//   if (WiFi.status() != WL_CONNECTED || g_refreshToken.length() == 0) return false;

//   WiFiClientSecure client;
//   client.setInsecure();  // Phase-1 prototype only; pin CA before deployment.
//   client.setTimeout(15000);

//   HTTPClient http;
//   String url = String("https://securetoken.googleapis.com/v1/token?key=") + FIREBASE_API_KEY;

//   if (!http.begin(client, url)) {
//     Serial.println("[Firebase] Refresh HTTP begin failed.");
//     return false;
//   }

//   http.setTimeout(15000);
//   http.setReuse(false);
//   http.useHTTP10(true);
//   http.addHeader("Content-Type", "application/x-www-form-urlencoded");
//   http.addHeader("Connection", "close");

//   String body = "grant_type=refresh_token&refresh_token=" + g_refreshToken;
//   int code = http.POST(body);

//   if (code == 200) {
//     String resp = http.getString();
//     String newIdToken = extractJsonString(resp, "id_token");
//     String newRefreshToken = extractJsonString(resp, "refresh_token");
//     String expiresInStr = extractJsonString(resp, "expires_in");
//     long expiresIn = expiresInStr.length() ? expiresInStr.toInt() : 3600;

//     if (newIdToken.length() > 0) {
//       g_idToken = newIdToken;
//       if (newRefreshToken.length() > 0) g_refreshToken = newRefreshToken;
//       g_tokenExpiryMs = millis() +
//           (unsigned long)((expiresIn > 60 ? expiresIn - 60 : expiresIn)) * 1000UL;
//       Serial.printf("[Firebase] Token refreshed OK, expires=%ld s.\n", expiresIn);
//       http.end();
//       return true;
//     }

//     Serial.printf("[Firebase] HTTP 200 but refreshed id_token was not found (response bytes=%u).\n",
//                   (unsigned)resp.length());
//   } else {
//     String errBody = http.getString();
//     Serial.printf("[Firebase] Refresh failed, HTTP %d (%s).\n",
//                   code, HTTPClient::errorToString(code).c_str());
//     if (errBody.length() > 0) {
//       Serial.printf("[Firebase] Refresh error body: %s\n", errBody.c_str());
//     }
//   }

//   http.end();
//   return false;
// }

// bool firebaseReady() {
//   return WiFi.status() == WL_CONNECTED && g_idToken.length() > 0 && millis() < g_tokenExpiryMs;
// }

// void maintainFirebaseAuth() {
//   if (WiFi.status() != WL_CONNECTED) return;

//   bool needsAuth = (g_idToken.length() == 0) || (millis() > g_tokenExpiryMs);
//   if (!needsAuth) return;

//   if (millis() - g_lastAuthAttemptMs < AUTH_RETRY_INTERVAL_MS) return;

//   g_lastAuthAttemptMs = millis();

//   Serial.println("[Firebase] Authentication attempt...");

//   bool ok = false;
//   if (g_refreshToken.length() > 0) ok = firebaseRefreshToken();
//   if (!ok) ok = firebaseSignIn();

//   if (!ok) {
//     // Do not hammer Firebase. The next attempt is controlled by the normal
//     // 30-second auth retry interval.
//     Serial.println("[Firebase] Authentication not ready; will retry later.");
//   }
// }

// // ================= FIRESTORE REST FIELD BUILDERS =================
// String ffDouble(float v) {
//   if (isnan(v) || isinf(v)) return "{\"nullValue\":null}";
//   return "{\"doubleValue\":" + String(v, 6) + "}";
// }
// String ffInt(long v) { return "{\"integerValue\":\"" + String(v) + "\"}"; }
// String ffString(const String& s) { return "{\"stringValue\":\"" + s + "\"}"; }
// String ffDoubleD(double v) { return "{\"doubleValue\":" + String(v, 6) + "}"; }

// String buildFeaturesMap(const float* f) {
//   String out = "{\"mapValue\":{\"fields\":{";
//   for (int i = 0; i < 23; i++) {
//     out += "\"" + String(kFeatureNames[i]) + "\":" + ffDouble(f[i]);
//     if (i < 22) out += ",";
//   }
//   out += "}}}";
//   return out;
// }

// String buildReadingFields(int ni, const String& tsIso, bool dataOk, bool includeLocation) {
//   NodeRecord &n = nodes[ni];
//   String out = "{\"fields\":{";
//   out += "\"node_id\":" + ffInt(n.nodeId) + ",";
//   if (tsIso.length() > 0) out += "\"timestamp\":{\"timestampValue\":\"" + tsIso + "\"},";
//   out += "\"device_uptime_ms\":" + ffInt((long)millis()) + ",";
//   out += "\"rssi\":" + ffInt(n.rssi) + ",";
//   out += "\"snr\":" + ffDouble(n.snr) + ",";
//   out += "\"deformation_state\":" + ffString(String(kClassNames[n.predictedClass])) + ",";
//   out += "\"confidence\":" + ffDouble(n.confidence) + ",";
//   out += "\"data_quality\":" + ffString(dataOk ? "OK" : "PARTIAL") + ",";
//   if (includeLocation) {
//     double lat, lon;
//     const char* siteLabel;
//     if (getNodeLocation(n.nodeId, lat, lon, siteLabel)) {
//       out += "\"latitude\":" + ffDoubleD(lat) + ",";
//       out += "\"longitude\":" + ffDoubleD(lon) + ",";
//       // Explicitly not "gps" - this is a hand-set reference-site pin, not
//       // a live fix. Keeps the frontend/jury from ever mistaking this for
//       // hardware physically installed at the named site.
//       out += "\"location_source\":" + ffString("hardcoded_reference_site_no_gps") + ",";
//       out += "\"site_label\":" + ffString(String(siteLabel)) + ",";
//     }
//   }
//   out += "\"features\":" + buildFeaturesMap(n.features);
//   out += "}}";
//   return out;
// }

// // Best-effort push: 1) appends to the "readings" history collection
// // (auto-generated document ID -> used for charts/CSV export), and
// // 2) overwrites "nodes_latest/{node_id}" (used for the live-status
// // tiles on the website). Both are skipped instantly - with no delay -
// // if WiFi/auth isn't ready; the USB->Node-RED path above is completely
// // unaffected either way.
// void pushReadingAndLatest(int ni) {
//   if (!firebaseReady()) {
//     Serial.println("[Firestore] Skipped push (offline or not authenticated yet) - best-effort only.");
//     return;
//   }
//   bool dataOk = !anyInvalidFeature(nodes[ni].features);
//   String tsIso = isoTimestampNow();
//   // History never carries location — it's static install metadata, not a
//   // time-series value, and repeating it on every packet would bloat every
//   // stored reading for no benefit.
//   String fieldsHistory = buildReadingFields(ni, tsIso, dataOk, false);
//   // The live snapshot DOES carry location - it's what a future GIS/map
//   // page on the website would read.
//   String fieldsLatest = buildReadingFields(ni, tsIso, dataOk, true);

//   WiFiClientSecure client;
//   client.setInsecure();

//   // 1) history: readings/{auto-id}
//   {
//     HTTPClient http;
//     String url = String("https://firestore.googleapis.com/v1/projects/") + FIREBASE_PROJECT_ID +
//                  "/databases/(default)/documents/readings";
//     http.begin(client, url);
//     http.addHeader("Content-Type", "application/json");
//     http.addHeader("Authorization", "Bearer " + g_idToken);
//     int code = http.POST(fieldsHistory);
//     if (code != 200) {
//       Serial.printf("[Firestore] readings POST failed, HTTP %d: %s\n", code, http.getString().c_str());
//     }
//     http.end();
//   }

//   // 2) live snapshot: nodes_latest/{node_id}  (PATCH with no fieldMask = full overwrite)
//   {
//     HTTPClient http;
//     String url = String("https://firestore.googleapis.com/v1/projects/") + FIREBASE_PROJECT_ID +
//                  "/databases/(default)/documents/nodes_latest/" + String(nodes[ni].nodeId);
//     http.begin(client, url);
//     http.addHeader("Content-Type", "application/json");
//     http.addHeader("Authorization", "Bearer " + g_idToken);
//     int code = http.PATCH(fieldsLatest);
//     if (code != 200 && code != 201) {
//       Serial.printf("[Firestore] nodes_latest PATCH failed, HTTP %d: %s\n", code, http.getString().c_str());
//     } else {
//       Serial.println("[Firestore] Push OK (history + latest snapshot).");
//     }
//     http.end();
//   }
// }

// // ================= OLED PAGES =================
// void drawHeader(const char* title) {
//   oled.setTextSize(1);
//   oled.setCursor(0, 0);
//   oled.print("CMD  ");
//   oled.println(title);
//   oled.drawLine(0, 10, 128, 10, SSD1306_WHITE);
// }

// void page0_overview() {
//   drawHeader("OVERVIEW");
//   oled.setCursor(0, 16);
//   oled.printf("Nodes known : %d\n", knownNodeCount);
//   oled.printf("Packets tot : %lu\n", totalPacketsReceived);
//   for (int i = 0; i < knownNodeCount && i < 3; i++) {
//     unsigned long age = (millis() - nodes[i].lastSeenMs) / 1000;
//     oled.printf("N%d age:%lus\n", nodes[i].nodeId, age);
//   }
// }

// void page1_last_node_verdict() {
//   drawHeader("AI VERDICT");
//   oled.setCursor(0, 16);
//   if (lastRxNodeIdx < 0) { oled.println("No packet yet"); return; }
//   NodeRecord &n = nodes[lastRxNodeIdx];
//   oled.printf("N%d: %s\n", n.nodeId, kClassNames[n.predictedClass]);
//   oled.printf("Conf: %d%%\n", (int)(n.confidence * 100));
//   oled.printf("Crk:%.1f Vrt:%.1f\n", n.features[0], n.features[1]);
//   oled.printf("Temp:%.1fC\n", n.features[7]);
// }

// void page2_radio_stats() {
//   drawHeader("LORA RADIO");
//   oled.setCursor(0, 16);
//   oled.printf("Status: %s\n", loraOK ? "OK" : "FAIL");
//   if (lastRxNodeIdx >= 0) {
//     oled.printf("RSSI: %d dBm\n", nodes[lastRxNodeIdx].rssi);
//     oled.printf("SNR : %.1f dB\n", nodes[lastRxNodeIdx].snr);
//   }
//   oled.println("Freq: 433 MHz");
// }

// void page3_gsm_status() {
//   drawHeader("GSM/SIM800L");
//   oled.setCursor(0, 16);
//   oled.printf("Modem: %s\n", simOK ? "OK (AT)" : "NO RESPONSE");
//   oled.println("SMS on high-risk");
//   oled.println("state (see code)");
// }

// void page4_firebase_status() {
//   drawHeader("WIFI/FIRESTORE");
//   oled.setCursor(0, 16);
//   oled.printf("WiFi : %s\n", WiFi.status() == WL_CONNECTED ? "OK" : "OFFLINE");
//   oled.printf("Auth : %s\n", firebaseReady() ? "OK" : "NO");
//   oled.printf("Time : %s\n", g_timeSynced ? "SYNCED" : "NO NTP");
//   oled.println("USB->NodeRED:ALWAYS");
// }

// void showPage(uint8_t p) {
//   oled.clearDisplay();
//   switch (p % NUM_PAGES) {
//     case 0: page0_overview();          break;
//     case 1: page1_last_node_verdict(); break;
//     case 2: page2_radio_stats();       break;
//     case 3: page3_gsm_status();        break;
//     case 4: page4_firebase_status();   break;
//   }
//   oled.display();
// }

// // ================= SETUP =================
// void setup() {
//   Serial.begin(115200);
//   delay(300);
//   Serial.println("\n=== SENSE-MINE COMMAND CENTRE (PHASE-1 PROTOTYPE) BOOT ===");
//   Serial.println("[INFO] USB JSON path (Node-RED) is unconditional.");
//   Serial.println("[INFO] WiFi->Firestore path is best-effort; see OLED page 5 for live status.");

//   Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
//   oledOK = oled.begin(SSD1306_SWITCHCAPVCC, 0x3C);
//   if (oledOK) {
//     oled.clearDisplay();
//     oled.setTextColor(SSD1306_WHITE);
//     oled.setCursor(0, 0);
//     oled.println("SENSE-MINE CMD CTR");
//     oled.println("Phase-1 Boot");
//     oled.display();
//   } else {
//     Serial.println("[OLED] init FAILED - check wiring / I2C address");
//   }

//   SPI.begin(PIN_LORA_SCK, PIN_LORA_MISO, PIN_LORA_MOSI, PIN_LORA_NSS);
//   LoRa.setPins(PIN_LORA_NSS, PIN_LORA_RST, PIN_LORA_DIO0);
//   loraOK = LoRa.begin(LORA_FREQ);
//   Serial.printf("[LoRa] init %s\n", loraOK ? "OK" : "FAIL (check VSPI wiring / antenna)");

//   pinMode(PIN_SIM_RST, OUTPUT);
//   digitalWrite(PIN_SIM_RST, HIGH);
//   sim800.begin(SIM800_BAUD, SERIAL_8N1, PIN_SIM_RX, PIN_SIM_TX);
//   delay(3000);

//   simOK = false;
//   for (int attempt = 1; attempt <= 3 && !simOK; attempt++) {
//     simOK = simSendAT("AT", "OK", 2000);
//     if (!simOK) {
//       Serial.printf("[SIM800L] AT attempt %d/3 failed, retrying...\n", attempt);
//       delay(1000);
//     }
//   }
//   Serial.printf("[SIM800L] AT test: %s\n", simOK ? "OK" : "FAIL (check power/UART)");
//   if (simOK) {
//     simSendAT("AT+CSQ", "OK", 2000);
//     simSendAT("AT+CREG?", "OK", 2000);
//   }
//   if (strstr(ALERT_PHONE_NUMBER, "XXXXXXXXXX") != NULL) {
//     Serial.println("[WARN] ALERT_PHONE_NUMBER is still a placeholder - SMS alerts will fail until you set a real number.");
//   }
//   if (strcmp(WIFI_SSID, "YOUR_WIFI_SSID") == 0) {
//     Serial.println("[WARN] WIFI_SSID/PASSWORD are still placeholders - Firestore path will stay offline until you set real credentials.");
//   }
//   if (strcmp(DEVICE_PASSWORD, "CHANGE_ME_TO_A_STRONG_PASSWORD") == 0) {
//     Serial.println("[WARN] DEVICE_PASSWORD is still a placeholder - create the matching Firebase Auth user before relying on Firestore push.");
//   }

//   // Kick off WiFi connect attempt - non-blocking, best-effort. The
//   // USB->Node-RED path above does not wait on any of this.
//   WiFi.mode(WIFI_STA);
//   WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
//   wifiConnecting = true;
//   wifiConnectStartedMs = millis();
//   lastWifiAttemptMs = millis();

//   delay(500);
// }

// // ================= LOOP =================
// void loop() {
//   int packetSize = LoRa.parsePacket();
//   if (packetSize) {
//     String pkt = "";
//     while (LoRa.available()) pkt += (char)LoRa.read();
//     int rssi  = LoRa.packetRssi();
//     float snr = LoRa.packetSnr();
//     parsePacket(pkt, rssi, snr);
//   }

//   static unsigned long lastSimCheck = 0;
//   if (millis() - lastSimCheck > 30000) {
//     simOK = simSendAT("AT", "OK", 1500);
//     lastSimCheck = millis();
//   }

//   // Best-effort WiFi/Firestore maintenance - all non-blocking, all
//   // rate-limited internally, none of it can stall the LoRa loop above.
//   maintainWiFi();
//   syncTimeIfNeeded();
//   maintainFirebaseAuth();

//   if (oledOK && millis() - lastPageSwitch > PAGE_INTERVAL_MS) {
//     currentPage = (currentPage + 1) % NUM_PAGES;
//     lastPageSwitch = millis();
//   }
//   if (oledOK) showPage(currentPage);

//   delay(50);
// }
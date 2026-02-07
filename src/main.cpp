#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_ADS1X15.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <math.h>

// ============================================================
//  ReefDoser pH + Temp -> Firebase RTDB (REST)
//  - Writes LIVE sensor values to:   /devices/<deviceId>/sensors/live
//  - Reads calibration from:         /devices/<deviceId>/sensors/phCal
//  - You calibrate ANYTIME by editing RTDB values (pH7_V, pH4_V)
// ============================================================

// --------------------- ADS1115 ---------------------
Adafruit_ADS1115 ads;

// --------------------- TEMP (DS18B20) ---------------------
const int oneWireBus = 13;
OneWire oneWire(oneWireBus);
DallasTemperature sensors(&oneWire);

// --------------------- WIFI ---------------------
// NOTE: Don’t commit these to GitHub in production
const char* WIFI_SSID = "roods";
const char* WIFI_PASS = "Frinov25!+!";

// --------------------- FIREBASE RTDB ---------------------
// Base URL (no trailing slash)
const char* RTDB_HOST = "https://aidoser-default-rtdb.firebaseio.com";

// If your RTDB rules allow unauthenticated reads/writes for these paths, keep empty.
// If you lock rules later, you must provide a token (ID token) or legacy DB secret.
const char* RTDB_AUTH = "";  // e.g. "xxxxx" or "" for no-auth

// Device Id (must match your site + DB path)
String deviceId = "reefDoser1";

// --------------------- DEFAULT CAL (fallback) ---------------------
float pH7_Voltage_default = 1.82f; // Voltage at pH 7.00 buffer
float pH4_Voltage_default = 1.44f; // Voltage at pH 4.01 buffer
float m_default = (7.0f - 4.01f) / (pH7_Voltage_default - pH4_Voltage_default);
float b_default = 7.0f - (m_default * pH7_Voltage_default);

// --------------------- CAL from RTDB ---------------------
struct PhCal {
  bool  enabled = true;
  float pH7_V = NAN;  // voltage at pH 7.00
  float pH4_V = NAN;  // voltage at pH 4.01
  float m = NAN;      // optional override (computed if missing)
  float b = NAN;      // optional override (computed if missing)
  uint64_t appliedAtMs = 0;
} phCal;

// --------------------- WiFi ---------------------
void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true);
  delay(100);

  Serial.println("Connecting to WiFi...");
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    if (millis() - start > 20000) {
      Serial.println("\nWiFi connect timeout!");
      return;
    }
  }

  Serial.println("\nWiFi connected");
  Serial.print("IP: ");  Serial.println(WiFi.localIP());
  Serial.print("GW: ");  Serial.println(WiFi.gatewayIP());
  Serial.print("DNS: "); Serial.println(WiFi.dnsIP());

  // If you ever hit DNS issues again, uncomment:
  // WiFi.setDNS(IPAddress(8,8,8,8), IPAddress(1,1,1,1));
}

// --------------------- Firebase REST helpers ---------------------
static String makeRtdbUrl(const String& pathNoJson) {
  String url = String(RTDB_HOST) + pathNoJson + ".json";
  if (RTDB_AUTH && strlen(RTDB_AUTH) > 0) {
    url += "?auth=" + String(RTDB_AUTH);
  }
  return url;
}

bool firebaseGet(const String& pathNoJson, String& out) {
  WiFiClientSecure client;
  client.setInsecure(); // quick test

  HTTPClient https;
  String url = makeRtdbUrl(pathNoJson);

  if (!https.begin(client, url)) {
    Serial.println("https.begin(GET) failed");
    return false;
  }

  int code = https.GET();
  out = https.getString();
  https.end();

  Serial.printf("Firebase GET %s code=%d\n", url.c_str(), code);
  if (code < 200 || code >= 300) {
    Serial.println("GET resp: " + out);
    return false;
  }
  return true;
}

bool firebasePut(const String& pathNoJson, const String& jsonBody) {
  WiFiClientSecure client;
  client.setInsecure(); // quick test

  HTTPClient https;
  String url = makeRtdbUrl(pathNoJson);

  Serial.println("---- Firebase PUT ----");
  Serial.println("URL: " + url);
  Serial.println("Body: " + jsonBody);

  if (!https.begin(client, url)) {
    Serial.println("https.begin(PUT) failed");
    return false;
  }
  https.addHeader("Content-Type", "application/json");

  int code = https.PUT(jsonBody);
  String resp = https.getString();
  https.end();

  Serial.printf("HTTP code: %d\n", code);
  Serial.println("Response: " + resp);

  return (code >= 200 && code < 300);
}

// PATCH (so we can update only some fields without overwriting siblings)
bool firebasePatch(const String& pathNoJson, const String& jsonBody) {
  WiFiClientSecure client;
  client.setInsecure(); // quick test

  HTTPClient https;
  String url = makeRtdbUrl(pathNoJson);

  Serial.println("---- Firebase PATCH ----");
  Serial.println("URL: " + url);
  Serial.println("Body: " + jsonBody);

  if (!https.begin(client, url)) {
    Serial.println("https.begin(PATCH) failed");
    return false;
  }
  https.addHeader("Content-Type", "application/json");

  int code = https.sendRequest("PATCH", (uint8_t*)jsonBody.c_str(), jsonBody.length());
  String resp = https.getString();
  https.end();

  Serial.printf("HTTP code: %d\n", code);
  Serial.println("Response: " + resp);

  return (code >= 200 && code < 300);
}

// --------------------- tiny JSON extractors (flat object) ---------------------
float jsonGetFloat(const String& json, const char* key, float fallback = NAN) {
  String k = String("\"") + key + "\":";
  int i = json.indexOf(k);
  if (i < 0) return fallback;
  i += k.length();
  while (i < (int)json.length() && (json[i] == ' ')) i++;

  int j = i;
  while (j < (int)json.length() && json[j] != ',' && json[j] != '}' && json[j] != '\n' && json[j] != '\r') j++;

  String tok = json.substring(i, j);
  tok.trim();
  // handle "null"
  if (tok == "null") return fallback;
  return tok.toFloat();
}

bool jsonGetBool(const String& json, const char* key, bool fallback = true) {
  String k = String("\"") + key + "\":";
  int i = json.indexOf(k);
  if (i < 0) return fallback;
  i += k.length();
  while (i < (int)json.length() && (json[i] == ' ')) i++;

  if (json.startsWith("true", i)) return true;
  if (json.startsWith("false", i)) return false;
  return fallback;
}

// --------------------- Calibration load/apply ---------------------
// Expected node: /devices/<deviceId>/sensors/phCal
// Example you edit anytime:
// {
//   "enabled": true,
//   "pH7_V": 1.8234,
//   "pH4_V": 1.4412
// }
//
// Optional overrides:
// { "m": -7.8943, "b": 21.55 }
bool loadPhCalFromRTDB() {
  String resp;
  String path = "/devices/" + deviceId + "/sensors/phCal";
  if (!firebaseGet(path, resp)) return false;

  resp.trim();
  if (resp == "null" || resp.length() < 2) {
    // no cal in DB; keep whatever we had
    return false;
  }

  phCal.enabled = jsonGetBool(resp, "enabled", true);
  phCal.pH7_V   = jsonGetFloat(resp, "pH7_V", NAN);
  phCal.pH4_V   = jsonGetFloat(resp, "pH4_V", NAN);
  phCal.m       = jsonGetFloat(resp, "m", NAN);
  phCal.b       = jsonGetFloat(resp, "b", NAN);

  // If m/b not provided but pH7_V and pH4_V are, compute m/b
  if (!isfinite(phCal.m) || !isfinite(phCal.b)) {
    if (isfinite(phCal.pH7_V) && isfinite(phCal.pH4_V) && phCal.pH7_V != phCal.pH4_V) {
      float m2 = (7.0f - 4.01f) / (phCal.pH7_V - phCal.pH4_V);
      float b2 = 7.0f - (m2 * phCal.pH7_V);
      phCal.m = m2;
      phCal.b = b2;
    }
  }

  return true;
}

// Apply calibration fast, only when values changed
bool applyPhCalIfChanged(bool writeBackComputedMB = true) {
  static bool  lastEnabled = true;
  static float last7 = NAN, last4 = NAN;
  static float lastM = NAN, lastB = NAN;

  bool loaded = loadPhCalFromRTDB();
  if (!loaded) return false;

  float useM = isfinite(phCal.m) ? phCal.m : m_default;
  float useB = isfinite(phCal.b) ? phCal.b : b_default;

  bool changed =
    (phCal.enabled != lastEnabled) ||
    (!isfinite(last7) && isfinite(phCal.pH7_V)) ||
    (!isfinite(last4) && isfinite(phCal.pH4_V)) ||
    (isfinite(last7) && isfinite(phCal.pH7_V) && fabs(phCal.pH7_V - last7) > 0.0005f) ||
    (isfinite(last4) && isfinite(phCal.pH4_V) && fabs(phCal.pH4_V - last4) > 0.0005f) ||
    (!isfinite(lastM) && isfinite(useM)) ||
    (!isfinite(lastB) && isfinite(useB)) ||
    (isfinite(lastM) && isfinite(useM) && fabs(useM - lastM) > 0.00001f) ||
    (isfinite(lastB) && isfinite(useB) && fabs(useB - lastB) > 0.00001f);

  if (!changed) return true;

  lastEnabled = phCal.enabled;
  last7 = phCal.pH7_V;
  last4 = phCal.pH4_V;
  lastM = useM;
  lastB = useB;

  phCal.appliedAtMs = millis();

  Serial.printf("✅ Calibration updated from RTDB: enabled=%d pH7_V=%.4f pH4_V=%.4f m=%.6f b=%.6f\n",
                lastEnabled, last7, last4, lastM, lastB);

  // OPTIONAL: write back computed m/b so you can SEE them in RTDB
  // (Uses PATCH so it won't overwrite your pH7_V/pH4_V/other fields.)
  if (writeBackComputedMB) {
    // only write if we have pH7/pH4 and we didn't explicitly supply m/b
    bool hadVoltages = isfinite(phCal.pH7_V) && isfinite(phCal.pH4_V);
    bool mbWasExplicit = isfinite(jsonGetFloat(String("{\"m\":" + String(phCal.m, 6) + "}"), "m", NAN)) &&
                         isfinite(jsonGetFloat(String("{\"b\":" + String(phCal.b, 6) + "}"), "b", NAN));

    // That mbWasExplicit test is not perfect; simpler rule:
    // write back always when hadVoltages.
    if (hadVoltages) {
      String path = "/devices/" + deviceId + "/sensors/phCal";
      String body = "{";
      body += "\"m\":" + String(lastM, 6) + ",";
      body += "\"b\":" + String(lastB, 6) + ",";
      body += "\"appliedAt\":" + String((uint64_t)phCal.appliedAtMs);
      body += "}";
      firebasePatch(path, body);
    }
  }

  return true;
}

// --------------------- Write LIVE sensors to RTDB ---------------------
// IMPORTANT: write to /sensors/live so we DO NOT overwrite /sensors/phCal
void writeLiveToRTDB(float tempF, float pH, float probeV, float useM, float useB) {
  String path = "/devices/" + deviceId + "/sensors/live";
  uint64_t ts = (uint64_t)millis(); // swap to epoch later if you add NTP

  String body = "{";
  body += "\"tempF\":" + String(tempF, 2) + ",";
  body += "\"pH\":" + (isfinite(pH) ? String(pH, 2) : String("null")) + ",";
  body += "\"pH_V\":" + String(probeV, 4) + ",";
  body += "\"m\":" + String(useM, 6) + ",";
  body += "\"b\":" + String(useB, 6) + ",";
  body += "\"updatedAt\":" + String(ts);
  body += "}";

  firebasePut(path, body);
}

void setup() {
  Serial.begin(115200);
  delay(100);

  connectWiFi();

  ads.setGain(GAIN_ONE);
  if (!ads.begin()) {
    Serial.println("Failed to initialize ADS1115.");
    while (1) delay(100);
  }

  sensors.begin();

  // First load/apply
  applyPhCalIfChanged(true);
}

void loop() {
  // Poll calibration frequently so you can update RTDB anytime and it applies quickly
  static unsigned long lastCalPoll = 0;
  if (millis() - lastCalPoll > 8000) { // every 8s
    lastCalPoll = millis();
    applyPhCalIfChanged(true);
  }

  // Read ADS1115 A0
  int16_t adc0 = ads.readADC_SingleEnded(0);
  float voltage = ads.computeVolts(adc0);

  // Choose effective calibration
  float useM = isfinite(phCal.m) ? phCal.m : m_default;
  float useB = isfinite(phCal.b) ? phCal.b : b_default;
  bool  enabled = phCal.enabled;

  float phValue = enabled ? (useM * voltage) + useB : NAN;

  // Read temperature
  sensors.requestTemperatures();
  float temperatureF = sensors.getTempFByIndex(0);
  float temperatureC = (temperatureF - 32.0f) * 5.0f / 9.0f;

  // Optional temp compensation
  float compensatedPH = phValue;
  if (isfinite(phValue) && isfinite(temperatureC)) {
    compensatedPH = phValue + (0.03f * (temperatureC - 25.0f));
  }

  Serial.printf("ADC0=%d V=%.4f  Temp=%.2fF  pH=%.2f (raw=%.2f)  m=%.6f b=%.6f enabled=%d\n",
                adc0, voltage, temperatureF,
                isfinite(compensatedPH) ? compensatedPH : -1.0f,
                isfinite(phValue) ? phValue : -1.0f,
                useM, useB, enabled ? 1 : 0);

  // Post live values every 15 seconds
  static unsigned long lastPost = 0;
  if (millis() - lastPost > 15000) {
    lastPost = millis();
    writeLiveToRTDB(temperatureF, compensatedPH, voltage, useM, useB);
  }

  delay(1000);
}

/*
============================================================
HOW TO CALIBRATE (ANYTIME) USING RTDB
============================================================

1) Open your RTDB and go to:
   devices/reefDoser1/sensors/phCal

2) Create/edit this JSON:

{
  "enabled": true,
  "pH7_V": 1.8234,
  "pH4_V": 1.4412
}

- Put probe in pH 7.00 buffer, wait to stabilize, record the VOLTAGE (printed in Serial as V=xxxx).
  Enter that number into "pH7_V".

- Put probe in pH 4.01 buffer, wait to stabilize, record voltage, enter into "pH4_V".

3) The ESP32 polls every ~8 seconds, recomputes m/b immediately, and uses it.
   It also PATCHes back computed:
     devices/reefDoser1/sensors/phCal/m
     devices/reefDoser1/sensors/phCal/b
     devices/reefDoser1/sensors/phCal/appliedAt

LIVE sensor readings are written here (so they don't overwrite calibration):
  devices/reefDoser1/sensors/live/tempF
  devices/reefDoser1/sensors/live/pH
  devices/reefDoser1/sensors/live/pH_V
  devices/reefDoser1/sensors/live/m
  devices/reefDoser1/sensors/live/b
  devices/reefDoser1/sensors/live/updatedAt
============================================================
*/

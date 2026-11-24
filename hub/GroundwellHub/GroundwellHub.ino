/*
  GROUNDWELL hub v0.9.1
  Board: Seeed XIAO ESP32-C6, Arduino core "esp32 by Espressif Systems" 3.x (NOT 4.x: it replaced the BLE API),
         board "XIAO_ESP32C6". Any Partition Scheme: partitions.csv in this folder overrides it with two
         1.875 MB app slots for over-the-air updates. (The IDE still prints the menu's size limit; the real
         limit is 1,966,080 bytes, and the GitHub build checks it.)

  Updates over the air (v0.9.1)
    - http://plants.local/update : upload GroundwellHub.ino.bin (user "admin", the update password).
    - Arduino IDE: the hub shows up under Tools > Port as a network port; upload as usual.
    - A new firmware runs on trial. If it crashes 3 times before it proves itself (Wi-Fi up, a node heard,
      a cloud upload accepted, 10 minutes running), the hub goes back to the previous firmware on its own.
  Setup page (v0.9.1)
    - Wi-Fi, cloud URL, ingest key and the update password live in the hub's own memory, set on /setup.
    - No Wi-Fi saved, or none found within 3 minutes of power-up, or the BOOT button held for 3 s:
      the hub opens its own hotspot "groundwell-setup-XXXX" (password SETUP_AP_PASS) with the setup page.
    - secrets.h is optional: if present, it fills in an empty hub (or one whose secrets.h changed) on boot.

  What it does
    - Listens for BTHome v2 broadcasts from GROUNDWELL nodes (or any BTHome device with "count" objects).
    - Every probe becomes a sensor card. The page has User and Admin tabs at the top: the same
      dashboard, with the calibration panels only on Admin.
    - Factory-calibrated nodes (node firmware 0.6+) send moisture on their own probe scale
      (0 = air, 100 = water). The hub maps that to soil with SOIL_DRY_PS / SOIL_WET_PS below,
      so a new node works out of the box. Admin can still fine-tune each pot.
    - Air temperature and humidity from each node (SHT4x, AHT20 or DHT11) show on its cards and go to the cloud once a minute.
    - Light (lux) from each node's light sensor (photoresistor, VEML7700 or LTR390) does the same, and the UV
      index from an LTR390 (v0.9); nodes without them simply don't send them.
    - Live dashboard at http://<hub IP>/  (last hour at full rate, 24 h at 5-minute averages).
    - Uploads to the GROUNDWELL cloud service once a minute (v0.9; the cloud keeps one soil reading per probe
      per minute). Readings queue up in RAM if Wi-Fi or the internet drops and are sent when it comes back.
    - v0.9 nodes send a reading every 30 s in a short radio burst (battery profile); v0.6 nodes (every 5 s) still work.
    - Probe names and calibration set on the cloud dashboard (Admin > Calibrate) come back in the
      upload reply and are applied and saved here, as if set on this page (v0.6.2).

  First flash over USB. Optional: copy secrets.example.h to secrets.h and fill it in, so the hub joins
  your Wi-Fi straight away. Then open Serial Monitor at 115200 for the IP address.
*/

#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <Update.h>
#include <ArduinoOTA.h>
#include <DNSServer.h>
#include <esp_ota_ops.h>
#include <esp_task_wdt.h>
#include <esp_system.h>
#include <time.h>
#include "bthome.h"
#include "page.h"

// ======================= settings =======================
#ifndef GW_CRASH_TEST
  #define GW_CRASH_TEST 0      // 1 = rollback test build: crashes 20 s after every boot (the GitHub build makes one)
#endif
#if GW_CRASH_TEST
  #define GW_FW_VERSION "0.9.1-crashtest"
#else
  #define GW_FW_VERSION "0.9.1"
#endif
static const char *FW_VERSION = GW_FW_VERSION;
// Optional secrets.h (git-ignored): WIFI_SSID, WIFI_PASS, CLOUD_URL, INGEST_KEY and optionally OTA_PASS.
// It only seeds the hub's memory; after that the setup page is where settings change.
#if __has_include("secrets.h")
  #include "secrets.h"
#endif
#ifndef WIFI_SSID
  #define WIFI_SSID ""
#endif
#ifndef WIFI_PASS
  #define WIFI_PASS ""
#endif
#ifndef CLOUD_URL
  #define CLOUD_URL ""
#endif
#ifndef INGEST_KEY
  #define INGEST_KEY ""
#endif
#ifndef OTA_PASS
  #define OTA_PASS ""
#endif
#define DEFAULT_CLOUD_URL "https://groundwell.groundwell.workers.dev/api/ingest"   // pre-filled on the setup page
#define SETUP_AP_PASS "groundwell"              // setup hotspot password (8+ characters). A product needs one per unit, on its label.
#define HOSTNAME    "plants"
#define HUB_ID      "home"                      // name of this hub in the cloud
#define USE_EXTERNAL_ANTENNA 0                  // 1 if a U.FL antenna is plugged into the XIAO
#define ADMIN_PIN   ""                          // "" = Admin view open to anyone on your Wi-Fi; set digits to require a PIN

// Soil mapping for factory-calibrated probes, on the probe scale (percent between the unit's air
// and water readings). Starting values from typical capacitive-probe data in potting mix; replace
// them with your own lab results (bone-dry mix and freshly drained mix, weighed on a scale).
static const float SOIL_DRY_PS = 30.0f;          // bone-dry potting mix reads about this on the probe scale
static const float SOIL_WET_PS = 75.0f;          // potting mix just watered and drained

static const uint32_t NODE_INTERVAL_MS = 30000; // must match REPORT_MS on the nodes (v0.9: 30 s; v0.6 nodes send every 5 s and still work)
static const uint32_t UPLOAD_MS        = 60000; // one upload a minute keeps Cloudflare D1 well inside its free daily limits
static const uint32_t DAY_BUCKET_MS    = 5UL * 60 * 1000;
static const uint32_t STALE_MS         = 150000; // card shows "no signal" after this (5 missed readings)
static const uint32_t CLIMATE_MS       = 60000; // air temp/humidity and light sent to the cloud once a minute per node
static const uint32_t WIFI_RETRY_MS    = 30000; // Wi-Fi down this long: start the connection over (rescans, picks any mesh point)
static const uint32_t WIFI_REBOOT_MS   = 30UL * 60 * 1000;   // still down after this: restart the hub, once per outage
static const uint32_t SETUP_AFTER_MS   = 3UL * 60 * 1000;    // no Wi-Fi this long after power-up: open the setup hotspot too
static const uint32_t SETUP_OPEN_MS    = 15UL * 60 * 1000;   // hotspot opened with the BOOT button stays this long
static const uint32_t TRIAL_MIN_MS     = 10UL * 60 * 1000;   // a new firmware must run this long (plus the checks) to be kept
static const uint8_t  TRIAL_CRASHES    = 3;                  // crashes on trial before going back to the previous firmware
static const uint32_t WATCHDOG_MS      = 60000;              // the main loop must come round within this, or the hub resets
static const int      SETUP_BTN_PIN    = BOOT_PIN;           // the BOOT button (GPIO 9 on the ESP32-C6)
// ========================================================

static const int MAX_NODES = 8, MAX_SENSORS = 16, LIVE_LEN = 720, DAY_LEN = 288, QUEUE_LEN = 3000, CLIM_LEN = 600, LIGHT_LEN = 600;
static const char *KIND[] = {"Capacitive", "Rod", "Probe 3", "Probe 4"};
static const char *KIND_FC = "Capacitive FC";            // factory-calibrated capacitive probe
static const uint16_t DEF_DRY[] = {3400, 60, 3000, 3000};   // starting guesses until you calibrate
static const uint16_t DEF_WET[] = {1500, 2800, 1000, 1000};

struct Node {
  bool used;
  char mac[18];
  bool havePid;
  uint8_t lastPid;
  uint32_t lastSeen, rx, lost;
  int8_t rssi;
  float volt, tempC, rh, lux, uv;
  bool hasAir, hasLux, hasUV;
  uint32_t airAt, climQueuedAt, luxAt, lightQueuedAt, uvAt;
  uint8_t batt;
  uint32_t lastFreshAt, ivlMs;   // time between new packets (smoothed): should be NODE_INTERVAL_MS
};

struct Sensor {
  bool used;
  uint8_t node, ch;
  char name[25];
  uint16_t dry, wet;
  uint8_t thr;
  uint16_t raw;
  uint16_t live[LIVE_LEN];
  uint16_t liveHead, liveCount;
  uint16_t day[DAY_LEN];
  uint16_t dayHead, dayCount;
  uint32_t daySum;
  uint16_t dayN;
  bool fcal, modeSet;            // value is the node's factory-calibrated probe scale x100 (0..10000)
  uint32_t lastQAt;              // last reading kept for history and the cloud
};

struct Rec {             // one queued reading for the cloud
  uint32_t ts;
  uint8_t node, ch;
  uint16_t raw;
};

Node nodes[MAX_NODES];
Sensor sensors[MAX_SENSORS];
struct ClimRec {         // one queued air reading for the cloud
  uint32_t ts;
  uint8_t node;
  int16_t tempX100;
  uint16_t rhX100;
};

struct LightRec {        // one queued light reading for the cloud
  uint32_t ts;
  uint8_t node;
  float lux;
};

Rec queue_[QUEUE_LEN];
int qHead = 0, qCount = 0;
ClimRec climQ[CLIM_LEN];
int cHead = 0, cCount = 0;
LightRec lightQ[LIGHT_LEN];
int lHead = 0, lCount = 0;
uint32_t lastDayPush = 0, lastUpload = 0, lastUploadOk = 0;
int lastUploadCode = 0;
uint32_t uploadedTotal = 0;
volatile uint32_t advHeard = 0, advFresh = 0;   // Bluetooth packets since the last status line (all / new)
bool wasConnected = false, mdnsUp = false;
// Survives a software restart (not a power cut): set just before a Wi-Fi watchdog restart, cleared once Wi-Fi is back,
// so a long outage (router off for hours) costs at most one restart and the readings queued in RAM survive after that.
RTC_NOINIT_ATTR uint32_t wifiRebootMark;
static const uint32_t WIFI_REBOOT_MAGIC = 0x57494649;   // "WIFI"

SemaphoreHandle_t lock;
WebServer server(80);
Preferences prefs;
BLEScan *scan;

// Settings in the hub's own memory (namespace "gwcfg"): set on /setup, or seeded from secrets.h.
struct Config { String ssid, pass, url, key, otapw; } cfg;
Preferences cfgPrefs, otaPrefs;
DNSServer dns;
bool apOn = false, everConnected = false, otaBusy = false, arduinoOtaUp = false, staPaused = false;
uint32_t apCloseAt = 0, apIdleSince = 0;     // apCloseAt 0 = no BOOT-button window
volatile uint32_t advTotal = 0;              // BTHome packets heard since boot (one of the trial checks)
bool trialActive = false;                    // this firmware is new and hasn't passed its trial yet

// ---------------- helpers ----------------
static int clampPct(long p) { return p < 0 ? 0 : (p > 100 ? 100 : (int)p); }

static int pctOf(const Sensor &s, uint16_t raw) {
  if (s.dry == s.wet) return 0;
  return clampPct(((long)s.dry - (long)raw) * 100L / ((long)s.dry - (long)s.wet));
}

static const char *kindOf(const Sensor &s) { return s.fcal ? KIND_FC : KIND[s.ch & 3]; }

static void keyFor(char *out, char prefix, const Sensor &s) {   // e.g. "nA1B2C3D4E5F60" (14 chars)
  const char *m = nodes[s.node].mac;
  int k = 0;
  out[k++] = prefix;
  for (int i = 0; m[i] && k < 13; i++) if (m[i] != ':') out[k++] = m[i];
  out[k++] = '0' + s.ch;
  out[k] = 0;
}

static void loadSensorPrefs(Sensor &s) {
  char k[16];
  keyFor(k, 'n', s);
  String n = prefs.getString(k, "");
  if (n.length()) strlcpy(s.name, n.c_str(), sizeof(s.name));
  else snprintf(s.name, sizeof(s.name), "%s %s", KIND[s.ch & 3], nodes[s.node].mac + 12);
  keyFor(k, 't', s); s.thr = prefs.getUChar(k, 30);
  // dry/wet are loaded by applyMode() once the first packet says which scale this probe uses.
}

// Raw-count probes keep their calibration under d/w, factory-calibrated ones under D/W (probe scale x100),
// so a node that gets factory-calibrated later starts from the soil defaults instead of stale raw numbers.
static void applyMode(Sensor &s, bool fc) {
  char k[16];
  s.fcal = fc;
  s.modeSet = true;
  keyFor(k, fc ? 'D' : 'd', s); s.dry = prefs.getUShort(k, fc ? (uint16_t)lroundf(SOIL_DRY_PS * 100) : DEF_DRY[s.ch & 3]);
  keyFor(k, fc ? 'W' : 'w', s); s.wet = prefs.getUShort(k, fc ? (uint16_t)lroundf(SOIL_WET_PS * 100) : DEF_WET[s.ch & 3]);
  s.liveHead = s.liveCount = s.dayHead = s.dayCount = 0;    // old points are on the other scale
  s.daySum = 0; s.dayN = 0;
}

static int findNode(const char *mac, bool create) {
  for (int i = 0; i < MAX_NODES; i++) if (nodes[i].used && !strcmp(nodes[i].mac, mac)) return i;
  if (!create) return -1;
  for (int i = 0; i < MAX_NODES; i++) if (!nodes[i].used) {
    memset(&nodes[i], 0, sizeof(Node));
    nodes[i].used = true;
    strlcpy(nodes[i].mac, mac, sizeof(nodes[i].mac));
    return i;
  }
  return -1;
}

static int findSensor(int node, int ch, bool create) {
  for (int i = 0; i < MAX_SENSORS; i++) if (sensors[i].used && sensors[i].node == node && sensors[i].ch == ch) return i;
  if (!create) return -1;
  for (int i = 0; i < MAX_SENSORS; i++) if (!sensors[i].used) {
    memset(&sensors[i], 0, sizeof(Sensor));
    sensors[i].used = true;
    sensors[i].node = node;
    sensors[i].ch = ch;
    loadSensorPrefs(sensors[i]);
    return i;
  }
  return -1;
}

static void enqueue(uint8_t node, uint8_t ch, uint16_t raw) {
  time_t now = time(nullptr);
  if (now < 1700000000) return;                       // clock not set yet (NTP pending)
  int idx = (qHead + qCount) % QUEUE_LEN;
  if (qCount == QUEUE_LEN) { qHead = (qHead + 1) % QUEUE_LEN; qCount--; }   // drop oldest
  queue_[idx] = {(uint32_t)now, node, ch, raw};
  qCount++;
}

// ---------------- BLE ----------------
static void handlePacket(const char *mac, int rssi, const BTHomePacket &p) {
  if (p.nCounts == 0 && !p.hasMoist) return;
  advHeard++;
  advTotal++;
  xSemaphoreTake(lock, portMAX_DELAY);
  int ni = findNode(mac, true);
  if (ni >= 0) {
    Node &n = nodes[ni];
    n.lastSeen = millis();
    n.rssi = (int8_t)rssi;
    bool fresh = !p.hasPid || !n.havePid || p.pid != n.lastPid;
    if (fresh) {
      advFresh++;
      if (p.hasPid && n.havePid) {
        uint8_t gap = (uint8_t)(p.pid - n.lastPid);
        if (gap > 1 && gap < 50) n.lost += gap - 1;      // bigger jumps = node rebooted
      }
      if (p.hasPid) { n.lastPid = p.pid; n.havePid = true; }
      n.rx++;
      uint32_t nowMs = millis();
      if (n.lastFreshAt) { uint32_t dt = nowMs - n.lastFreshAt; n.ivlMs = n.ivlMs ? (n.ivlMs * 7 + dt) / 8 : dt; }
      n.lastFreshAt = nowMs;
      if (p.hasVolt) n.volt = p.volt;
      if (p.hasBatt) n.batt = p.batt;
      if (p.hasUV) { n.uv = p.uv; n.hasUV = true; n.uvAt = millis(); }
      if (p.hasTemp && p.hasHum) {
        n.tempC = p.tempC;
        n.rh = p.humidity;
        n.hasAir = true;
        n.airAt = millis();
        time_t now = time(nullptr);
        if (now > 1700000000 && (n.climQueuedAt == 0 || millis() - n.climQueuedAt >= CLIMATE_MS)) {
          int idx = (cHead + cCount) % CLIM_LEN;
          if (cCount == CLIM_LEN) { cHead = (cHead + 1) % CLIM_LEN; cCount--; }
          climQ[idx] = {(uint32_t)now, (uint8_t)ni, (int16_t)lroundf(n.tempC * 100), (uint16_t)lroundf(n.rh * 100)};
          cCount++;
          n.climQueuedAt = millis();
        }
      }
      if (p.hasLux) {
        n.lux = p.lux;
        n.hasLux = true;
        n.luxAt = millis();
        time_t now = time(nullptr);
        if (now > 1700000000 && (n.lightQueuedAt == 0 || millis() - n.lightQueuedAt >= CLIMATE_MS)) {
          int idx = (lHead + lCount) % LIGHT_LEN;
          if (lCount == LIGHT_LEN) { lHead = (lHead + 1) % LIGHT_LEN; lCount--; }
          lightQ[idx] = {(uint32_t)now, (uint8_t)ni, n.lux};
          lCount++;
          n.lightQueuedAt = millis();
        }
      }
      // Channel 0 is the capacitive probe: factory-calibrated moisture if the node sends it, else its raw count.
      uint16_t vals[4]; bool fc[4]; int nv = 0;
      if (p.hasMoist) { float m = p.moist < 0 ? 0 : p.moist > 100 ? 100 : p.moist; vals[nv] = (uint16_t)lroundf(m * 100); fc[nv++] = true; }
      for (int c = 0; c < p.nCounts && nv < 4; c++) { vals[nv] = (uint16_t)(p.counts[c] > 65535 ? 65535 : p.counts[c]); fc[nv++] = false; }
      for (int c = 0; c < nv; c++) {
        int si = findSensor(ni, c, true);
        if (si < 0) continue;
        Sensor &s = sensors[si];
        if (!s.modeSet || s.fcal != fc[c]) applyMode(s, fc[c]);
        s.raw = vals[c];
        // Keep at most one reading per node interval for history and the cloud, whatever the node
        // sends: a misbehaving node can't flood the cloud's daily write limit.
        if (s.lastQAt && nowMs - s.lastQAt < NODE_INTERVAL_MS * 3 / 4) continue;
        s.lastQAt = nowMs;
        s.live[s.liveHead] = s.raw;
        s.liveHead = (s.liveHead + 1) % LIVE_LEN;
        if (s.liveCount < LIVE_LEN) s.liveCount++;
        s.daySum += s.raw;
        s.dayN++;
        enqueue(ni, c, s.raw);
      }
    }
  }
  xSemaphoreGive(lock);
}

class ScanCallbacks : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice dev) override {
    for (int i = 0; i < dev.getServiceDataCount(); i++) {
      if (!dev.getServiceDataUUID(i).equals(BLEUUID((uint16_t)0xFCD2))) continue;
      auto sd = dev.getServiceData(i);                   // String (core 3.x) or std::string (2.x)
      BTHomePacket p;
      if (!parseBTHome((const uint8_t *)sd.c_str(), sd.length(), p)) continue;
      String mac = String(dev.getAddress().toString().c_str());
      mac.toUpperCase();
      handlePacket(mac.c_str(), dev.getRSSI(), p);
    }
  }
};

// ---------------- history ----------------
static void pushDay() {
  xSemaphoreTake(lock, portMAX_DELAY);
  for (int i = 0; i < MAX_SENSORS; i++) {
    Sensor &s = sensors[i];
    if (!s.used) continue;
    s.day[s.dayHead] = s.dayN ? (uint16_t)(s.daySum / s.dayN) : s.raw;
    s.dayHead = (s.dayHead + 1) % DAY_LEN;
    if (s.dayCount < DAY_LEN) s.dayCount++;
    s.daySum = 0;
    s.dayN = 0;
  }
  xSemaphoreGive(lock);
  lastDayPush = millis();
}

// ---------------- cloud upload ----------------
static void applyCloudCfg(const String &resp);

static void uploadBatch() {
  if (otaBusy || cfg.url.length() == 0 || WiFi.status() != WL_CONNECTED) return;

  // Snapshot up to 240 readings plus the sensor names/calibration, then release the lock.
  String body;
  int n, nc, nl;
  xSemaphoreTake(lock, portMAX_DELAY);
  n = qCount < 240 ? qCount : 240;
  nc = cCount < 100 ? cCount : 100;
  nl = lCount < 100 ? lCount : 100;
  if (n == 0 && nc == 0 && nl == 0) {
    xSemaphoreGive(lock);
    scan->stop(); scan->clearResults(); scan->start(0, nullptr, false);   // keep the result list short
    return;
  }
  int nSens = 0;
  for (int i = 0; i < MAX_SENSORS; i++) if (sensors[i].used) nSens++;
  body.reserve(200 + nSens * 220 + n * 44 + nc * 48 + nl * 40);
  body += "{\"hub\":\"" HUB_ID "\",\"sensors\":[";
  bool first = true;
  for (int i = 0; i < MAX_SENSORS; i++) {
    Sensor &s = sensors[i];
    if (!s.used) continue;
    Node &nd = nodes[s.node];
    if (!first) body += ',';
    first = false;
    body += "{\"node\":\""; body += nd.mac;
    body += "\",\"ch\":"; body += s.ch;
    body += ",\"name\":\""; body += s.name;
    body += "\",\"kind\":\""; body += kindOf(s);
    body += "\",\"dry\":"; body += s.dry;
    body += ",\"wet\":"; body += s.wet;
    body += ",\"thr\":"; body += s.thr;
    body += ",\"volt\":"; body += String(nd.volt, 3);
    if (nd.hasAir) { body += ",\"temp\":"; body += String(nd.tempC, 1); body += ",\"rh\":"; body += String(nd.rh, 1); }
    if (nd.hasLux) { body += ",\"lux\":"; body += String(nd.lux, 1); }
    if (nd.hasUV && millis() - nd.uvAt < 5 * STALE_MS) { body += ",\"uv\":"; body += String(nd.uv, 1); }
    body += ",\"rssi\":"; body += nd.rssi;
    body += '}';
  }
  body += "],\"readings\":[";
  for (int k = 0; k < n; k++) {
    const Rec &r = queue_[(qHead + k) % QUEUE_LEN];
    if (k) body += ',';
    body += '['; body += r.ts; body += ",\""; body += nodes[r.node].mac; body += "\",";
    body += r.ch; body += ','; body += r.raw; body += ']';
  }
  body += "],\"climate\":[";
  for (int k = 0; k < nc; k++) {
    const ClimRec &c = climQ[(cHead + k) % CLIM_LEN];
    if (k) body += ',';
    body += '['; body += c.ts; body += ",\""; body += nodes[c.node].mac; body += "\",";
    body += String(c.tempX100 / 100.0f, 2); body += ','; body += String(c.rhX100 / 100.0f, 2); body += ']';
  }
  body += "],\"light\":[";
  for (int k = 0; k < nl; k++) {
    const LightRec &l = lightQ[(lHead + k) % LIGHT_LEN];
    if (k) body += ',';
    body += '['; body += l.ts; body += ",\""; body += nodes[l.node].mac; body += "\",";
    body += String(l.lux, 1); body += ']';
  }
  body += "]}";
  xSemaphoreGive(lock);

  // The C6 has one 2.4 GHz radio for Bluetooth and Wi-Fi. Pause listening while the TLS
  // upload runs so Wi-Fi gets the radio and the handshake gets the memory it needs.
  // Nodes repeat each packet for 5 s, so a 1 to 3 s pause loses nothing.
  scan->stop();
  scan->clearResults();
  delay(20);

  int code = -1;
  char tlsErr[100] = "";
  String reply;
  uint32_t freeHeap = ESP.getFreeHeap(), maxBlock = ESP.getMaxAllocHeap();
  if (freeHeap < 50000 || maxBlock < 20000) {
    code = -100;   // not enough memory for a TLS handshake (about 40 kB, largest piece about 17 kB): skip this round instead of crashing
  } else {
    WiFiClientSecure tls;
    tls.setInsecure();   // TLS-encrypted; server certificate not pinned yet (see README)
    tls.setHandshakeTimeout(15);
    HTTPClient http;
    http.setTimeout(10000);
    http.setConnectTimeout(10000);
    http.setReuse(false);
    if (http.begin(tls, cfg.url)) {
      http.addHeader("Content-Type", "application/json");
      http.addHeader("Authorization", String("Bearer ") + cfg.key);
      code = http.POST(body);
      if (code < 0) tls.lastError(tlsErr, sizeof(tlsErr));
      else if (code == 200) reply = http.getString();   // may carry calibration from the cloud dashboard
      http.end();
    }
  }
  body = String();   // free the batch before Bluetooth restarts

  scan->start(0, nullptr, false);
  lastUploadCode = code;
  if (code == 200) {
    xSemaphoreTake(lock, portMAX_DELAY);
    int drop = n < qCount ? n : qCount;                 // readings that arrived meanwhile stay queued
    qHead = (qHead + drop) % QUEUE_LEN;
    qCount -= drop;
    int dropC = nc < cCount ? nc : cCount;
    cHead = (cHead + dropC) % CLIM_LEN;
    cCount -= dropC;
    int dropL = nl < lCount ? nl : lCount;
    lHead = (lHead + dropL) % LIGHT_LEN;
    lCount -= dropL;
    xSemaphoreGive(lock);
    uploadedTotal += n;
    lastUploadOk = millis();
  }
  Serial.printf("Upload %d readings + %d air + %d light -> HTTP %d (queued %d)  heap %u free, %u largest\n",
                n, nc, nl, code, qCount, (unsigned)freeHeap, (unsigned)maxBlock);
  if (code == -100) Serial.println("  skipped: too little free memory for a secure connection");
  else if (code < 0) Serial.printf("  %s%s\n", HTTPClient::errorToString(code).c_str(), tlsErr[0] ? (String(" / TLS: ") + tlsErr).c_str() : "");
  if (reply.length()) applyCloudCfg(reply);
}

// ---------------- web ----------------
static void handleRoot() {
  if (!cfg.ssid.length()) { server.sendHeader("Location", "/setup"); server.send(302, "text/plain", ""); return; }
  server.send_P(200, "text/html", PAGE);
}

static void handleLive() {
  String j;
  j.reserve(3000);
  uint32_t now = millis();
  xSemaphoreTake(lock, portMAX_DELAY);
  j += "{\"uptime\":"; j += now / 1000;
  j += ",\"fw\":\""; j += FW_VERSION; j += '"';
  j += ",\"trial\":"; j += trialActive ? "true" : "false";
  j += ",\"pin\":"; j += strlen(ADMIN_PIN) ? "true" : "false";
  j += ",\"rssi\":"; j += (int)WiFi.RSSI();
  j += ",\"ip\":\""; j += WiFi.localIP().toString();
  j += "\",\"cloud\":{\"enabled\":"; j += cfg.url.length() ? "true" : "false";
  j += ",\"code\":"; j += lastUploadCode;
  j += ",\"ageS\":"; j += lastUploadOk ? (long)((now - lastUploadOk) / 1000) : -1L;
  j += ",\"queued\":"; j += qCount;
  j += ",\"sent\":"; j += uploadedTotal;
  j += "},\"sensors\":[";
  bool first = true;
  for (int i = 0; i < MAX_SENSORS; i++) {
    Sensor &s = sensors[i];
    if (!s.used) continue;
    Node &nd = nodes[s.node];
    if (!first) j += ',';
    first = false;
    uint32_t tot = nd.rx + nd.lost;
    j += "{\"id\":"; j += i;
    j += ",\"name\":\""; j += s.name;
    j += "\",\"kind\":\""; j += kindOf(s);
    j += "\",\"fcal\":"; j += s.fcal ? "true" : "false";
    j += ",\"ivl\":"; j += String(nd.ivlMs / 1000.0f, 1);
    j += ",\"dft\":["; j += (int)lroundf(SOIL_DRY_PS * 100); j += ','; j += (int)lroundf(SOIL_WET_PS * 100); j += ']';
    j += ",\"node\":\""; j += nd.mac;
    j += "\",\"raw\":"; j += s.raw;
    j += ",\"pct\":"; j += pctOf(s, s.raw);
    j += ",\"dry\":"; j += s.dry;
    j += ",\"wet\":"; j += s.wet;
    j += ",\"thr\":"; j += s.thr;
    j += ",\"volt\":"; j += String(nd.volt, 2);
    j += ",\"air\":"; j += (nd.hasAir && now - nd.airAt < 5 * STALE_MS) ? "true" : "false";
    j += ",\"temp\":"; j += String(nd.tempC, 1);
    j += ",\"rh\":"; j += String(nd.rh, 0);
    j += ",\"luxOk\":"; j += (nd.hasLux && now - nd.luxAt < 5 * STALE_MS) ? "true" : "false";
    j += ",\"lux\":"; j += String(nd.lux, 0);
    j += ",\"uv\":"; j += (nd.hasUV && now - nd.uvAt < 5 * STALE_MS) ? String(nd.uv, 1) : String("null");
    j += ",\"brssi\":"; j += nd.rssi;
    j += ",\"ageS\":"; j += (now - nd.lastSeen) / 1000;
    j += ",\"loss\":"; j += tot ? String(100.0f * nd.lost / tot, 1) : String("0");
    j += '}';
  }
  j += "]}";
  xSemaphoreGive(lock);
  server.send(200, "application/json", j);
}

static void handleHistory() {
  bool live = server.arg("range") == "live";
  String j;
  xSemaphoreTake(lock, portMAX_DELAY);
  int nUsed = 0;
  for (int i = 0; i < MAX_SENSORS; i++) if (sensors[i].used) nUsed++;
  j.reserve(400 + nUsed * (live ? LIVE_LEN : DAY_LEN) * 5);
  j += "{\"intervalMs\":"; j += live ? NODE_INTERVAL_MS : DAY_BUCKET_MS;
  j += ",\"ageMs\":"; j += live ? 0UL : (unsigned long)(millis() - lastDayPush);
  j += ",\"series\":{";
  bool first = true;
  for (int i = 0; i < MAX_SENSORS; i++) {
    Sensor &s = sensors[i];
    if (!s.used) continue;
    if (!first) j += ',';
    first = false;
    j += '"'; j += i; j += "\":[";
    int len = live ? LIVE_LEN : DAY_LEN, head = live ? s.liveHead : s.dayHead, cnt = live ? s.liveCount : s.dayCount;
    const uint16_t *buf = live ? s.live : s.day;
    int start = (head - cnt + len) % len;
    for (int k = 0; k < cnt; k++) { if (k) j += ','; j += buf[(start + k) % len]; }
    j += ']';
  }
  j += "}}";
  xSemaphoreGive(lock);
  server.send(200, "application/json", j);
}

static String cleanName(const String &in) {
  String out;
  for (size_t k = 0; k < in.length() && out.length() < 24; k++) {
    char c = in[k];
    if (c >= 32 && c <= 126 && c != '"' && c != '\\' && c != '<' && c != '>') out += c;
  }
  out.trim();
  return out;
}

// Calibration set on the cloud dashboard arrives in the upload reply as
//   "cfg":"MAC|ch|dry|wet|name;MAC|ch|D||;..."   (dry "D" = back to the defaults)
// Apply and save each change exactly as the Admin panel on this page would.
static void applyCloudCfg(const String &resp) {
  int a = resp.indexOf("\"cfg\":\"");
  if (a < 0) return;
  a += 7;
  int b = resp.indexOf('"', a);
  if (b < 0) return;
  String all = resp.substring(a, b);
  int start = 0, applied = 0;
  xSemaphoreTake(lock, portMAX_DELAY);
  while (start < (int)all.length()) {
    int end = all.indexOf(';', start);
    if (end < 0) end = all.length();
    String item = all.substring(start, end), f[5];
    start = end + 1;
    int p = 0;
    for (int k = 0; k < 5; k++) {
      int q = item.indexOf('|', p);
      if (q < 0 || k == 4) { f[k] = item.substring(p); break; }
      f[k] = item.substring(p, q);
      p = q + 1;
    }
    f[0].toUpperCase();
    int ni = findNode(f[0].c_str(), false);
    if (ni < 0 || !f[1].length()) continue;
    int si = findSensor(ni, f[1].toInt(), false);
    if (si < 0) continue;
    Sensor &s = sensors[si];
    char key[16];
    const char D = s.fcal ? 'D' : 'd', W = s.fcal ? 'W' : 'w';
    if (f[2] == "D") {
      keyFor(key, D, s); prefs.remove(key); keyFor(key, W, s); prefs.remove(key);
      applyMode(s, s.fcal);
      applied++;
      continue;
    }
    long dry = f[2].toInt(), wet = f[3].toInt();
    if (f[2].length() && f[3].length() && dry >= 0 && dry <= 65535 && wet >= 0 && wet <= 65535 && dry != wet) {
      s.dry = dry; keyFor(key, D, s); prefs.putUShort(key, s.dry);
      s.wet = wet; keyFor(key, W, s); prefs.putUShort(key, s.wet);
    }
    String n = cleanName(f[4]);
    if (n.length() && strcmp(n.c_str(), s.name)) { strlcpy(s.name, n.c_str(), sizeof(s.name)); keyFor(key, 'n', s); prefs.putString(key, n); }
    applied++;
  }
  xSemaphoreGive(lock);
  if (applied) Serial.printf("  applied %d calibration change(s) from the cloud dashboard\n", applied);
}

static bool adminOk() { return strlen(ADMIN_PIN) == 0 || server.arg("pin") == ADMIN_PIN; }

// POST /api/admin  pin=...  -> 200 if the PIN is right (or none is set), 403 if not
static void handleAdmin() { server.send(adminOk() ? 200 : 403, "application/json", adminOk() ? "{\"ok\":true}" : "{\"ok\":false}"); }

// POST /api/config  id=<sensor> and any of: name, and (Admin) dry, wet, thr, action=dry|wet|defaults
static void handleConfig() {
  int i = server.hasArg("id") ? server.arg("id").toInt() : -1;
  if (i < 0 || i >= MAX_SENSORS || !sensors[i].used) { server.send(400, "text/plain", "bad id"); return; }
  bool admin = server.hasArg("action") || server.hasArg("dry") || server.hasArg("wet") || server.hasArg("thr");
  if (admin && !adminOk()) { server.send(403, "text/plain", "admin PIN required"); return; }
  xSemaphoreTake(lock, portMAX_DELAY);
  Sensor &s = sensors[i];
  char k[16];
  const char D = s.fcal ? 'D' : 'd', W = s.fcal ? 'W' : 'w';
  if (server.hasArg("action")) {
    String a = server.arg("action");
    if (a == "dry") { s.dry = s.raw; keyFor(k, D, s); prefs.putUShort(k, s.dry); }
    if (a == "wet") { s.wet = s.raw; keyFor(k, W, s); prefs.putUShort(k, s.wet); }
    if (a == "defaults") { keyFor(k, D, s); prefs.remove(k); keyFor(k, W, s); prefs.remove(k); applyMode(s, s.fcal); }
  }
  if (server.hasArg("name")) {
    String n = cleanName(server.arg("name"));
    if (n.length()) { strlcpy(s.name, n.c_str(), sizeof(s.name)); keyFor(k, 'n', s); prefs.putString(k, n); }
  }
  if (server.hasArg("dry")) { long v = server.arg("dry").toInt(); if (v >= 0 && v <= 65535) { s.dry = v; keyFor(k, D, s); prefs.putUShort(k, s.dry); } }
  if (server.hasArg("wet")) { long v = server.arg("wet").toInt(); if (v >= 0 && v <= 65535) { s.wet = v; keyFor(k, W, s); prefs.putUShort(k, s.wet); } }
  if (server.hasArg("thr")) { long v = server.arg("thr").toInt(); if (v >= 0 && v <= 100) { s.thr = v; keyFor(k, 't', s); prefs.putUChar(k, s.thr); } }
  xSemaphoreGive(lock);
  server.send(200, "application/json", "{\"ok\":true}");
}

// ---------------- settings in the hub's memory ----------------
static uint32_t fnv1a(const char *s, uint32_t h) { while (*s) { h ^= (uint8_t)*s++; h *= 16777619u; } return h; }

static void loadConfig() {
  cfgPrefs.begin("gwcfg", false);
  // secrets.h fills in an empty hub, and replaces the saved values only when secrets.h itself changed,
  // so settings changed on /setup stick until you edit secrets.h and flash again.
  if (strlen(WIFI_SSID)) {
    const char *f[] = {WIFI_SSID, WIFI_PASS, CLOUD_URL, INGEST_KEY, OTA_PASS};
    uint32_t h = 2166136261u;
    for (const char *s : f) { h = fnv1a(s, h); h = fnv1a("\x1f", h); }
    if (cfgPrefs.getULong("seed", 0) != h) {
      cfgPrefs.putString("ssid", WIFI_SSID);
      cfgPrefs.putString("pass", WIFI_PASS);
      cfgPrefs.putString("url", CLOUD_URL);
      cfgPrefs.putString("key", INGEST_KEY);
      if (strlen(OTA_PASS) >= 8) cfgPrefs.putString("otapw", OTA_PASS);
      cfgPrefs.putULong("seed", h);
      Serial.println("Settings filled in from secrets.h");
    }
  }
  cfg.ssid  = cfgPrefs.getString("ssid", "");
  cfg.pass  = cfgPrefs.getString("pass", "");
  cfg.url   = cfgPrefs.getString("url", "");
  cfg.key   = cfgPrefs.getString("key", "");
  cfg.otapw = cfgPrefs.getString("otapw", "");
}

// ---------------- setup hotspot ----------------
static String apName() { String m = WiFi.macAddress(); m.replace(":", ""); return "groundwell-setup-" + m.substring(8); }

static void openSetup(uint32_t forMs, const char *why) {
  if (!apOn) {
    WiFi.softAP(apName().c_str(), SETUP_AP_PASS);
    dns.start(53, "*", WiFi.softAPIP());          // every name points at the hub, so phones show the setup page
    apOn = true;
    Serial.printf("Setup hotspot open (%s): join \"%s\" (password \"%s\"), then open http://%s/setup\n",
                  why, apName().c_str(), SETUP_AP_PASS, WiFi.softAPIP().toString().c_str());
  }
  apIdleSince = 0;
  if (forMs) apCloseAt = (millis() + forMs) | 1;
}

static void closeSetup() {
  if (!apOn) return;
  dns.stop();
  WiFi.softAPdisconnect(true);
  apOn = false;
  apCloseAt = 0;
  Serial.println("Setup hotspot closed");
}

static bool fromHotspot() { return apOn && server.client().localIP() == WiFi.softAPIP(); }

// ---------------- firmware trial and rollback ----------------
// A new firmware runs on trial. Each crash (panic or watchdog) on trial counts; after TRIAL_CRASHES the hub
// switches back to the other slot, which still holds the previous firmware. Power cuts, brownouts and the
// hub's own restarts don't count. The trial ends once the new firmware has proven itself (trialMaybeConfirm).
static const char *slotName() { const esp_partition_t *p = esp_ota_get_running_partition(); return p ? p->label : "?"; }

static void trialStart() {   // called after a new firmware is written, just before restarting into it
  otaPrefs.putBool("trial", true);
  otaPrefs.putUChar("crashes", 0);
  otaPrefs.putString("prev", slotName());
  otaPrefs.putString("prevVer", FW_VERSION);
}

static void trialCheckAtBoot() {
  otaPrefs.begin("gwota", false);
  if (!otaPrefs.getBool("trial", false)) return;
  if (otaPrefs.getString("prev", "") == slotName()) {   // the new firmware never started; the old one is still running
    otaPrefs.putBool("trial", false);
    otaPrefs.putString("note", String("The last update didn't start, so the hub kept ") + FW_VERSION + ".");
    return;
  }
  esp_reset_reason_t r = esp_reset_reason();
  bool crash = r == ESP_RST_PANIC || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT || r == ESP_RST_WDT;
  uint8_t crashes = otaPrefs.getUChar("crashes", 0) + (crash ? 1 : 0);
  otaPrefs.putUChar("crashes", crashes);
  if (crash) Serial.printf("Firmware %s on trial: crash %u of %u (reset reason %d)\n", FW_VERSION, crashes, TRIAL_CRASHES, (int)r);
  if (crashes >= TRIAL_CRASHES) {
    String back = otaPrefs.getString("prevVer", "the previous firmware");
    otaPrefs.putBool("trial", false);
    if (Update.rollBack()) {
      otaPrefs.putString("note", String("Firmware ") + FW_VERSION + " crashed " + crashes + " times on trial, so the hub went back to " + back + ".");
      Serial.printf("Going back to firmware %s\n", back.c_str());
      delay(200);
      ESP.restart();
    }
    otaPrefs.putString("note", String("Firmware ") + FW_VERSION + " crashed on trial, and the previous firmware couldn't be restored.");
    return;
  }
  trialActive = true;
}

static void trialMaybeConfirm() {   // proven: 10 minutes up, on Wi-Fi, a node heard, and a cloud upload accepted (if the cloud is on)
  if (!trialActive || millis() < TRIAL_MIN_MS) return;
  if (WiFi.status() != WL_CONNECTED || advTotal == 0) return;
  if (cfg.url.length() && !lastUploadOk) return;
  trialActive = false;
  otaPrefs.putBool("trial", false);
  otaPrefs.putString("note", String("Firmware ") + FW_VERSION + " passed its trial.");
  Serial.printf("Firmware %s passed its trial\n", FW_VERSION);
}

static void startWatchdog() {   // a hang counts as a crash: the loop must come round within WATCHDOG_MS
  esp_task_wdt_config_t c = {.timeout_ms = WATCHDOG_MS, .idle_core_mask = 0, .trigger_panic = true};
  if (esp_task_wdt_reconfigure(&c) != ESP_OK) esp_task_wdt_init(&c);
  enableLoopWDT();
}

// ---------------- pages: /setup and /update ----------------
static const char PAGE_HEAD[] PROGMEM =
  "<!doctype html><html><head><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
  "<title>groundwell hub</title><style>body{font-family:system-ui,sans-serif;background:#fdf6ee;color:#5a4636;"
  "max-width:30rem;margin:1.5rem auto;padding:0 1rem}h1{font-size:1.35rem;color:#6f8f5e;font-weight:600}"
  "label{display:block;margin:.9rem 0 .25rem;font-weight:600}input{width:100%;box-sizing:border-box;padding:.55rem;"
  "border:1px solid #d9c7b5;border-radius:3px;font-size:1rem;background:#fff}button{margin-top:1.2rem;padding:.6rem 1.2rem;"
  "background:#6f8f5e;color:#fff;border:0;border-radius:3px;font-size:1rem}.note{font-size:.85rem;color:#8a7461}"
  ".msg{padding:.6rem .8rem;background:#f3e3d3;border-radius:3px}.err{background:#f6d6cc}a{color:#6f8f5e}"
  "table{border-collapse:collapse}td{padding:.15rem .8rem .15rem 0}</style></head><body><h1>groundwell hub</h1>";

static String htmlEsc(const String &s) {
  String o;
  o.reserve(s.length() + 8);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '&') o += "&amp;"; else if (c == '<') o += "&lt;"; else if (c == '>') o += "&gt;"; else if (c == '"') o += "&quot;"; else o += c;
  }
  return o;
}

static void sendPage(int code, const String &body) {
  String h = FPSTR(PAGE_HEAD);
  h += body;
  h += "<p class=note>Firmware "; h += FW_VERSION; h += " · <a href=/>Dashboard</a> · <a href=/update>Update</a> · <a href=/setup>Settings</a></p></body></html>";
  server.send(code, "text/html", h);
}

// On the setup hotspot you're next to the hub: no password. On your Wi-Fi: the update password, once one is set.
static bool setupAuthOk() {
  if (fromHotspot() || cfg.otapw.length() == 0) return true;
  if (server.authenticate("admin", cfg.otapw.c_str())) return true;
  server.requestAuthentication(BASIC_AUTH, "groundwell hub", "Update password needed (user admin)");
  return false;
}

static void handleSetupPage(const String &msg, bool isErr) {
  String opts;
  int n = WiFi.scanNetworks(false, false, false, 300);
  for (int i = 0; i < n && i < 20; i++) { String s = WiFi.SSID(i); if (s.length()) { opts += "<option value=\""; opts += htmlEsc(s); opts += "\">"; } }
  WiFi.scanDelete();
  String b;
  if (msg.length()) { b += "<p class='msg"; b += isErr ? " err" : ""; b += "'>"; b += msg; b += "</p>"; }
  b += "<form method=post action=/setup>";
  b += "<label>Wi-Fi network (2.4 GHz)</label><input name=ssid list=nets required maxlength=32 value=\""; b += htmlEsc(cfg.ssid); b += "\"><datalist id=nets>"; b += opts; b += "</datalist>";
  b += "<label>Wi-Fi password</label><input name=pass type=password maxlength=63 placeholder=\""; b += cfg.pass.length() ? "saved (leave blank to keep)" : ""; b += "\">";
  b += "<label>Cloud upload URL</label><input name=url value=\""; b += htmlEsc(cfg.url.length() || cfg.ssid.length() ? cfg.url : String(DEFAULT_CLOUD_URL)); b += "\">";
  b += "<div class=note>Leave empty to turn cloud upload off.</div>";
  b += "<label>Ingest key</label><input name=key type=password placeholder=\""; b += cfg.key.length() ? "saved (leave blank to keep)" : "the INGEST_KEY you gave Cloudflare"; b += "\">";
  b += "<label>Update password</label><input name=otapw type=password minlength=8 placeholder=\""; b += cfg.otapw.length() ? "saved (leave blank to keep)" : "choose one, 8+ characters"; b += "\">";
  b += "<div class=note>Protects firmware updates and this page on your Wi-Fi. User name: admin.</div>";
  b += "<button>Save and restart</button></form>";
  sendPage(isErr ? 400 : 200, b);
}

static void handleSetupGet() { if (setupAuthOk()) handleSetupPage("", false); }

static void handleSetupSave() {
  if (!setupAuthOk()) return;
  String ssid = server.arg("ssid"), pass = server.arg("pass"), url = server.arg("url"), key = server.arg("key"), otapw = server.arg("otapw");
  ssid.trim(); url.trim(); key.trim();
  if (!ssid.length() || ssid.length() > 32) return handleSetupPage("Enter the Wi-Fi network name (up to 32 characters).", true);
  if (pass.length() && (pass.length() < 8 || pass.length() > 63)) return handleSetupPage("Wi-Fi passwords are 8 to 63 characters.", true);
  if (url.length() && !url.startsWith("https://")) return handleSetupPage("The cloud URL must start with https://", true);
  if (otapw.length() && otapw.length() < 8) return handleSetupPage("The update password needs at least 8 characters.", true);
  if (!otapw.length() && !cfg.otapw.length()) return handleSetupPage("Choose an update password (8+ characters).", true);
  cfgPrefs.putString("ssid", ssid);
  if (pass.length() || ssid != cfg.ssid) cfgPrefs.putString("pass", pass);   // a new network with a blank password = open network
  cfgPrefs.putString("url", url);
  if (key.length()) cfgPrefs.putString("key", key);
  if (otapw.length()) cfgPrefs.putString("otapw", otapw);
  sendPage(200, "<p class=msg>Saved. The hub is restarting and will join <b>" + htmlEsc(ssid) +
                "</b>. On that network the dashboard is at <a href=http://" HOSTNAME ".local/>http://" HOSTNAME ".local/</a>.</p>");
  delay(1500);
  ESP.restart();
}

static bool updateAuthOk() {
  if (server.authenticate("admin", cfg.otapw.c_str())) return true;
  server.requestAuthentication(BASIC_AUTH, "groundwell hub", "Update password needed (user admin)");
  return false;
}

static void handleUpdatePage() {
  if (!cfg.otapw.length()) { sendPage(200, "<p class=msg>Choose an update password on the <a href=/setup>settings page</a> first.</p>"); return; }
  if (!updateAuthOk()) return;
  const esp_partition_t *run = esp_ota_get_running_partition(), *next = esp_ota_get_next_update_partition(NULL);
  String b = "<table><tr><td>Firmware</td><td><b>"; b += FW_VERSION; b += "</b>"; b += trialActive ? " (on trial)" : ""; b += "</td></tr>";
  b += "<tr><td>Slot</td><td>"; b += run ? run->label : "?"; b += ", "; b += ESP.getSketchSize() / 1024; b += " of "; b += run ? run->size / 1024 : 0; b += " KB</td></tr></table>";
  String note = otaPrefs.getString("note", "");
  if (note.length()) { b += "<p class=msg>"; b += htmlEsc(note); b += "</p>"; }
  if (!next) {
    b += "<p class='msg err'>This hub has a single app slot. Flash it over USB once with this firmware's partition table.</p>";
  } else {
    b += "<label>New firmware: GroundwellHub.ino.bin</label><input type=file id=f accept=.bin>";
    b += "<button onclick=go()>Upload and restart</button><p id=s class=note></p>";
    b += "<script>function go(){const f=document.getElementById('f').files[0],s=document.getElementById('s');if(!f)return;"
         "const x=new XMLHttpRequest();x.open('POST','/update');"
         "x.upload.onprogress=e=>{if(e.lengthComputable)s.textContent='Uploading '+Math.round(e.loaded*100/e.total)+'%'};"
         "x.onload=()=>{s.textContent=x.responseText;if(x.status==200)setTimeout(()=>location.href='/update',25000)};"
         "x.onerror=()=>{s.textContent='Upload failed: the connection dropped. Nothing was changed.'};"
         "const d=new FormData();d.append('firmware',f);x.send(d)}</script>";
    b += "<p class=note>The new firmware runs on trial. If it crashes 3 times before it proves itself, the hub goes back to this one.</p>";
  }
  sendPage(200, b);
}

static bool upAuthed = false;
static String upErr;

static void handleUpdateUpload() {
  HTTPUpload &up = server.upload();
  if (up.status == UPLOAD_FILE_START) {
    upErr = "";
    upAuthed = cfg.otapw.length() && server.authenticate("admin", cfg.otapw.c_str());
    if (!upAuthed) { upErr = "Update password needed."; return; }
    const esp_partition_t *run = esp_ota_get_running_partition();
    if (!esp_ota_get_next_update_partition(NULL)) { upErr = "This hub has a single app slot: flash it over USB once."; return; }
    if (run && ESP.getSketchSize() > run->size) { upErr = "The running firmware is bigger than its slot: flash over USB."; return; }
    otaBusy = true;
    scan->stop();                                   // give Wi-Fi the radio while the file comes in
    if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) upErr = String("Couldn't start: ") + Update.errorString();
  } else if (up.status == UPLOAD_FILE_WRITE) {
    esp_task_wdt_reset();
    if (!upAuthed || upErr.length()) return;
    if (Update.progress() == 0) {                   // first chunk: is this a hub app image?
      const uint8_t *b = up.buf;
      bool image = up.currentSize >= 36 && b[0] == 0xE9;
      bool app = image && b[32] == 0x32 && b[33] == 0x54 && b[34] == 0xCD && b[35] == 0xAB;   // esp_app_desc_t magic
#ifdef CONFIG_IDF_FIRMWARE_CHIP_ID
      bool chip = image && (b[12] | (b[13] << 8)) == CONFIG_IDF_FIRMWARE_CHIP_ID;
#else
      bool chip = true;
#endif
      if (!app || !chip) { upErr = "That isn't a hub firmware file. Pick GroundwellHub.ino.bin (not the merged file, not a node file)."; Update.abort(); return; }
    }
    if (Update.write(up.buf, up.currentSize) != up.currentSize) { upErr = String("Write failed: ") + Update.errorString(); Update.abort(); }
  } else if (up.status == UPLOAD_FILE_END) {
    if (upAuthed && !upErr.length() && !Update.end(true)) upErr = String("The file didn't check out: ") + Update.errorString();
  } else if (up.status == UPLOAD_FILE_ABORTED) {      // the browser went away: the done handler won't run
    if (Update.isRunning()) Update.abort();
    upErr = "Upload interrupted. Nothing was changed.";
    if (otaBusy) { otaBusy = false; scan->start(0, nullptr, false); }
    Serial.println("Web update interrupted: nothing changed");
  }
}

static void handleUpdateDone() {
  if (!upAuthed) { server.requestAuthentication(BASIC_AUTH, "groundwell hub", "Update password needed (user admin)"); return; }
  if (upErr.length() || !Update.isFinished()) {
    if (!upErr.length()) upErr = "The upload was incomplete. Nothing was changed.";
    if (Update.isRunning()) Update.abort();
    if (otaBusy) { otaBusy = false; scan->start(0, nullptr, false); }
    server.send(400, "text/plain", upErr + " The hub keeps running " + FW_VERSION + ".");
    Serial.printf("Web update failed: %s\n", upErr.c_str());
    return;
  }
  trialStart();
  server.send(200, "text/plain", "Done. The hub is restarting into the new firmware; this page reloads in about 25 s.");
  Serial.println("Web update written: restarting into the new firmware");
  delay(800);
  ESP.restart();
}

static void startArduinoOta() {   // Arduino IDE network uploads, with the same password
  if (arduinoOtaUp || cfg.otapw.length() < 8) return;
  ArduinoOTA.setHostname(HOSTNAME);
  ArduinoOTA.setPassword(cfg.otapw.c_str());
  ArduinoOTA.setMdnsEnabled(false);                 // the hub runs mDNS itself (see loop)
  ArduinoOTA.onStart([]() { otaBusy = true; scan->stop(); Serial.println("Arduino IDE upload started"); });
  ArduinoOTA.onProgress([](unsigned int, unsigned int) { esp_task_wdt_reset(); });
  ArduinoOTA.onEnd([]() { trialStart(); Serial.println("Arduino IDE upload done: restarting into the new firmware"); });
  ArduinoOTA.onError([](ota_error_t e) { otaBusy = false; scan->start(0, nullptr, false); Serial.printf("Arduino IDE upload failed (error %u)\n", (unsigned)e); });
  ArduinoOTA.begin();
  arduinoOtaUp = true;
}

// ---------------- setup / loop ----------------
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.printf("\nGROUNDWELL hub %s starting (slot %s)\n", FW_VERSION, slotName());
  lock = xSemaphoreCreateMutex();
  prefs.begin("groundwell", false);
  trialCheckAtBoot();                              // may switch back to the previous firmware and restart
  loadConfig();
  pinMode(SETUP_BTN_PIN, INPUT_PULLUP);

#if USE_EXTERNAL_ANTENNA
  pinMode(3, OUTPUT); digitalWrite(3, LOW);    // enable the RF switch
  delay(100);
  pinMode(14, OUTPUT); digitalWrite(14, HIGH); // select the U.FL connector
#endif

  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOSTNAME);
  WiFi.setAutoReconnect(true);
  if (cfg.ssid.length()) WiFi.begin(cfg.ssid.c_str(), cfg.pass.c_str());
  else openSetup(0, "no Wi-Fi saved yet");
  configTime(0, 0, "pool.ntp.org", "time.google.com");   // UTC timestamps for the cloud

  BLEDevice::init("");
  scan = BLEDevice::getScan();
  scan->setAdvertisedDeviceCallbacks(new ScanCallbacks(), true);   // true = report every advert
  scan->setActiveScan(false);                                       // passive: listen only
  scan->setInterval(160);
  scan->setWindow(80);                                              // 50% duty: leaves Wi-Fi radio time
  scan->start(0, nullptr, false);                                   // scan forever in the background

  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/live", HTTP_GET, handleLive);
  server.on("/api/history", HTTP_GET, handleHistory);
  server.on("/api/config", HTTP_POST, handleConfig);
  server.on("/api/admin", HTTP_POST, handleAdmin);
  server.on("/setup", HTTP_GET, handleSetupGet);
  server.on("/setup", HTTP_POST, handleSetupSave);
  server.on("/update", HTTP_GET, handleUpdatePage);
  server.on("/update", HTTP_POST, handleUpdateDone, handleUpdateUpload);
  server.onNotFound([]() {
    if (fromHotspot()) {                           // phones probing for a captive portal land on the setup page
      server.sendHeader("Location", String("http://") + WiFi.softAPIP().toString() + "/setup");
      server.send(302, "text/plain", "");
      return;
    }
    server.send(404, "text/plain", "not found");
  });
  server.begin();
  lastDayPush = lastUpload = millis();
  if (trialActive) Serial.printf("Firmware %s is on trial (%u crashes allowed)\n", FW_VERSION, TRIAL_CRASHES - 1);
  if (!esp_ota_get_next_update_partition(NULL)) Serial.println("WARNING: no second app slot, so over-the-air updates are off. Flash once over USB.");
  startWatchdog();
}

void loop() {
  server.handleClient();
  uint32_t now = millis();

  bool connected = WiFi.status() == WL_CONNECTED;
  if (connected && !wasConnected) {
    Serial.printf("Wi-Fi up. Dashboard: http://%s/  (http://%s.local/)\n", WiFi.localIP().toString().c_str(), HOSTNAME);
    everConnected = true;
    startArduinoOta();
    if (mdnsUp) MDNS.end();                          // re-announce on the (possibly new) network address
    mdnsUp = MDNS.begin(HOSTNAME);
    if (mdnsUp) MDNS.addService("http", "tcp", 80);
    if (mdnsUp && arduinoOtaUp) MDNS.enableArduino(3232, true);   // shows up as a network port in Arduino IDE
    wifiRebootMark = 0;
  }
  wasConnected = connected;
  if (arduinoOtaUp && connected) ArduinoOTA.handle();
  trialMaybeConfirm();

  // Setup hotspot: open when no Wi-Fi is saved, when the saved one isn't found after power-up, or when the
  // BOOT button is held for 3 s. Wi-Fi retries pause while a phone is on it (they would knock it off).
  static uint32_t btnSince = 0;
  static bool btnFired = false;
  if (digitalRead(SETUP_BTN_PIN) == LOW) {
    if (!btnSince) btnSince = now | 1;
    if (!btnFired && now - btnSince >= 3000) { btnFired = true; openSetup(SETUP_OPEN_MS, "BOOT button"); }
  } else { btnSince = 0; btnFired = false; }
  if (!apOn && !everConnected && cfg.ssid.length() && now >= SETUP_AFTER_MS) openSetup(0, "saved Wi-Fi not found");
  int apClients = apOn ? WiFi.softAPgetStationNum() : 0;
  if (apOn && apClients && !connected && cfg.ssid.length() && !staPaused) {
    staPaused = true;
    WiFi.setAutoReconnect(false);
    WiFi.disconnect();
    Serial.println("Setup page in use: Wi-Fi retries paused");
  }
  if (staPaused && !apClients) {
    staPaused = false;
    WiFi.setAutoReconnect(true);
    WiFi.begin(cfg.ssid.c_str(), cfg.pass.c_str());
  }
  if (apOn) {
    if (apClients) apIdleSince = 0; else if (!apIdleSince) apIdleSince = now | 1;
    bool buttonWindow = apCloseAt && (int32_t)(apCloseAt - now) > 0;
    if (connected && !buttonWindow && apIdleSince && now - apIdleSince >= 120000) closeSetup();
  }

  // Wi-Fi watchdog. Auto-reconnect handles short drops, but when the router or mesh disappears for minutes
  // (a nightly reboot, say) the ESP32 can stop retrying. Start over every 30 s until it's back; after 30 min,
  // restart the hub once. Readings keep queuing in RAM meanwhile and upload when Wi-Fi returns.
  static uint32_t downSince = 0, lastKick = 0;
  if (connected || !cfg.ssid.length() || staPaused) downSince = 0;
  else {
    if (!downSince) downSince = lastKick = now ? now : 1;
    if (now - lastKick >= WIFI_RETRY_MS) {
      lastKick = now;
      Serial.printf("Wi-Fi down %lu s: reconnecting\n", (unsigned long)((now - downSince) / 1000));
      WiFi.disconnect();
      WiFi.begin(cfg.ssid.c_str(), cfg.pass.c_str());
    }
    if (now - downSince >= WIFI_REBOOT_MS && wifiRebootMark != WIFI_REBOOT_MAGIC && !apOn) {
      Serial.println("Wi-Fi down 30 min: restarting the hub (once per outage)");
      wifiRebootMark = WIFI_REBOOT_MAGIC;
      delay(200);
      ESP.restart();
    }
  }

  // The scan result list is cleared during each upload pause (every 30 s), while the scan is stopped.
  if (now - lastDayPush >= DAY_BUCKET_MS) pushDay();
  if (now - lastUpload >= UPLOAD_MS) { lastUpload = now; uploadBatch(); }

  static uint32_t lastPrint = 0;
  if (now - lastPrint >= 10000) {
    lastPrint = now;
    Serial.printf("[%s %s] ", connected ? "WiFi OK" : "WiFi down", connected ? WiFi.localIP().toString().c_str() : "");
    xSemaphoreTake(lock, portMAX_DELAY);
    int shown = 0;
    for (int i = 0; i < MAX_SENSORS; i++) if (sensors[i].used) {
      Serial.printf("%s=%u  ", sensors[i].name, sensors[i].raw);
      shown++;
    }
    xSemaphoreGive(lock);
    Serial.printf(shown ? "| BLE %u heard, %u new" : "no nodes heard yet (BLE %u heard, %u new)", (unsigned)advHeard, (unsigned)advFresh);
    xSemaphoreTake(lock, portMAX_DELAY);
    for (int i = 0; i < MAX_NODES; i++) if (nodes[i].used && nodes[i].ivlMs)
      Serial.printf(" | node %s every %.1f s", nodes[i].mac + 12, nodes[i].ivlMs / 1000.0f);
    xSemaphoreGive(lock);
    Serial.println();
    advHeard = advFresh = 0;
  }
#if GW_CRASH_TEST
  if (now > 20000) { Serial.println("Crash test build: crashing on purpose"); delay(50); abort(); }
#endif
  delay(2);
}

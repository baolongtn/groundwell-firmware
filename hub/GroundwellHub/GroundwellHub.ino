/*
  GROUNDWELL hub v0.9.0
  Board: Seeed XIAO ESP32-C6, Arduino core "esp32 by Espressif Systems" 3.x, board "XIAO_ESP32C6"
         Tools > Partition Scheme: "Huge APP (3MB No OTA/1MB SPIFFS)"  (Wi-Fi + BLE + TLS is a big binary)

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

  Copy secrets.example.h to secrets.h and fill it in, check the settings block, flash, then open Serial Monitor at 115200 for the IP address.
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
#include <time.h>
#include "bthome.h"
#include "page.h"

// ======================= settings =======================
// Wi-Fi and cloud credentials (WIFI_SSID, WIFI_PASS, CLOUD_URL, INGEST_KEY) live in secrets.h:
// copy secrets.example.h to secrets.h in this folder and fill it in. secrets.h is git-ignored.
#if __has_include("secrets.h")
  #include "secrets.h"
#else
  #error "Missing secrets.h: copy secrets.example.h to secrets.h in the GroundwellHub folder and fill it in."
#endif
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
  if (strlen(CLOUD_URL) == 0 || WiFi.status() != WL_CONNECTED) return;

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
    if (http.begin(tls, CLOUD_URL)) {
      http.addHeader("Content-Type", "application/json");
      http.addHeader("Authorization", String("Bearer ") + INGEST_KEY);
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
static void handleRoot() { server.send_P(200, "text/html", PAGE); }

static void handleLive() {
  String j;
  j.reserve(3000);
  uint32_t now = millis();
  xSemaphoreTake(lock, portMAX_DELAY);
  j += "{\"uptime\":"; j += now / 1000;
  j += ",\"pin\":"; j += strlen(ADMIN_PIN) ? "true" : "false";
  j += ",\"rssi\":"; j += (int)WiFi.RSSI();
  j += ",\"ip\":\""; j += WiFi.localIP().toString();
  j += "\",\"cloud\":{\"enabled\":"; j += strlen(CLOUD_URL) ? "true" : "false";
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

// ---------------- setup / loop ----------------
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\nGROUNDWELL hub starting");
  lock = xSemaphoreCreateMutex();
  prefs.begin("groundwell", false);

#if USE_EXTERNAL_ANTENNA
  pinMode(3, OUTPUT); digitalWrite(3, LOW);    // enable the RF switch
  delay(100);
  pinMode(14, OUTPUT); digitalWrite(14, HIGH); // select the U.FL connector
#endif

  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOSTNAME);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
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
  server.onNotFound([]() { server.send(404, "text/plain", "not found"); });
  server.begin();
  lastDayPush = lastUpload = millis();
}

void loop() {
  server.handleClient();
  uint32_t now = millis();

  bool connected = WiFi.status() == WL_CONNECTED;
  if (connected && !wasConnected) {
    Serial.printf("Wi-Fi up. Dashboard: http://%s/\n", WiFi.localIP().toString().c_str());
    if (mdnsUp) MDNS.end();                          // re-announce on the (possibly new) network address
    mdnsUp = MDNS.begin(HOSTNAME);
    if (mdnsUp) MDNS.addService("http", "tcp", 80);
    wifiRebootMark = 0;
  }
  wasConnected = connected;

  // Wi-Fi watchdog. Auto-reconnect handles short drops, but when the router or mesh disappears for minutes
  // (a nightly reboot, say) the ESP32 can stop retrying. Start over every 30 s until it's back; after 30 min,
  // restart the hub once. Readings keep queuing in RAM meanwhile and upload when Wi-Fi returns.
  static uint32_t downSince = 0, lastKick = 0;
  if (connected) downSince = 0;
  else {
    if (!downSince) downSince = lastKick = now ? now : 1;
    if (now - lastKick >= WIFI_RETRY_MS) {
      lastKick = now;
      Serial.printf("Wi-Fi down %lu s: reconnecting\n", (unsigned long)((now - downSince) / 1000));
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASS);
    }
    if (now - downSince >= WIFI_REBOOT_MS && wifiRebootMark != WIFI_REBOOT_MAGIC) {
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
  delay(2);
}

/*
  GROUNDWELL hub v0.13.0
  Board: Seeed XIAO ESP32-C6, Arduino core "esp32 by Espressif Systems" 3.x (NOT 4.x: it replaced the BLE API),
         board "XIAO_ESP32C6", Tools > Partition Scheme: "Huge APP (3MB No OTA/1MB SPIFFS)".
         The menu only sets the IDE's size check: partitions.csv in this folder replaces the layout with two
         1.875 MB app slots for over-the-air updates. The real limit is 1,966,080 bytes; the GitHub build checks it.

  Updates over the air (v0.9.1)
    - http://plants.local/update : upload GroundwellHub.ino.bin (user "admin", the update password).
    - Arduino IDE: the hub shows up under Tools > Port as a network port; upload as usual.
    - A new firmware runs on trial. If it crashes 3 times before it proves itself (Wi-Fi up, a node heard,
      a cloud upload accepted, 10 minutes running), the hub goes back to the previous firmware on its own.
    - v0.11.1 fixes the /update file check, which refused every real upload up to v0.11.0. A hub on 0.11.0 or
      older takes this update from the Arduino IDE's network port or espota.py (docs/SETUP.md section 6).
  Node updates (v0.10)
    - The hub carries the node firmware inside its own (node_fw.h, made from the node build; the GitHub build
      does this). After a hub update it checks every node, links new ones and updates old ones over Bluetooth,
      one at a time, only on a healthy battery. Status per node: dashboard (Admin) and /update.
    - Linking: a node learns this hub's link key in the 10 minutes after its power-up or reset. After that
      only this hub (proving the key) can update it. A node linked elsewhere: press its reset button once.
  Diagnostics (v0.11)
    - http://plants.local/diag (Admin > Diagnostics): what the hub is doing now, hub, Wi-Fi, Bluetooth and
      cloud state, every node's traffic, signal, battery, firmware and update status, what each node reports
      about itself, the probes, and an event log. "Download snapshot" saves it all as one file for debugging.
  Sensors wired to the hub (v0.12)
    - The hub can carry its own sensors, like a node: capacitive probe, DHT11, photoresistor and the I2C
      sensors (SHT4x, AHT20, LTR390). It reads them every 60 s (v0.12: 30 s) with the node's own code
      (local_sensors.h) and they show up as one more node, "wired to this hub". Wiring: local_sensors.h and
      docs/SETUP.md section 4b. Turn them on at /setup ("Sensors wired to this hub").
    - Factory calibration over USB with the node's commands (INFO, CAL AIR, CAL WATER, FAST) and tools/factory_cal.py.
  Health checks (v0.12)
    - /diag opens with a verdict for every link of the chain (nodes, Bluetooth, the hub's sensors, the hub, Wi-Fi,
      cloud) and a list of findings. Per node: readings received against expected over the last hour, dropped
      packets, timing against the expected interval, copies heard per reading, signal, battery, firmware against
      the version this hub carries, and how far behind the cloud is.
    - Fix: the packet id is one byte, so its 255 -> 0 wrap every 256 readings counted as a node restart (and made
      the hub check that node every 2 h). Restarts are now told apart from the wrap, and a node that has just
      been reset is spotted by its continuous advertising in its link window.
    - Rod probes are gone (node firmware 0.12). A probe a node stops sending is dropped from the cards and the
      upload at once, so the rod card of an updated node goes away by itself. v0.11 nodes still work.
  v0.12.1: crash fixes and crash reports
    - The Bluetooth library keeps a copy of every device it hears, with no limit, until the list is cleared, and
      the hub only cleared it at each cloud upload (never while Wi-Fi was down or uploads were off). In a busy
      room the list grew until an allocation on the Bluetooth task failed and the hub restarted (abort() on
      nimble_host). It is now cleared every 10 s, whatever else is going on.
    - More free memory: 180 live points per probe instead of 720 (the dashboard plots the last hour, about 120),
      and the sensor task only exists when sensors are wired to the hub (6 KB stack).
    - Crash reports: at start the hub reads the crash dump the ESP32 keeps in flash and records the reason, task,
      program counter and the code addresses on that task's stack: in the event log, on /diag, and in the
      /update note when a trial firmware is rolled back. Decode them against that build's .elf.
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
    - v0.13 nodes send a reading every 60 s, and the hub expects that (NODE_INTERVAL_MS). Nodes on 0.12 or older
      still work (their extra readings are dropped, and /diag flags their interval) until the hub updates them.
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
#include <BLEClient.h>
#if defined(CONFIG_BLUEDROID_ENABLED)
  #include <esp_gattc_api.h>
#endif
#include <Update.h>
#include <ArduinoOTA.h>
#include <DNSServer.h>
#include <esp_ota_ops.h>
#include <esp_task_wdt.h>
#include <esp_system.h>
#include <esp_core_dump.h>
#include <esp_app_desc.h>
#include <time.h>
#include "bthome.h"
#if __has_include("pages_gz.h")
  #include "pages_gz.h"                // page.h and diag_page.h gzipped (tools/make_pages_gz.py; the GitHub build makes it)
#else
  #include "page.h"
  #include "diag_page.h"
  #define GW_PAGES_GZ 0
#endif
#include "gw_hmac.h"
#include "node_dfu.h"
#include "ota_check.h"                // web update check: is the file a hub app image (host-tested)
#include "local_sensors.h"            // sensors wired to the hub (v0.12): the node's own measurement code
#if __has_include("node_fw.h")
  #include "node_fw.h"                 // the node firmware to hand out (tools/make_node_fw_h.py; the GitHub build makes it)
#endif
#ifndef NODE_FW_BUNDLED
  #define NODE_FW_BUNDLED 0
  static const char NODE_FW_VERSION[] = "";
  static const uint8_t NODE_FW_BIN[4] = {0};
  static const uint32_t NODE_FW_LEN = 0;
  static const uint8_t NODE_FW_DAT[2] = {0};
  static const uint16_t NODE_FW_DAT_LEN = 0;
#endif

// The IDE checks the sketch size against the Partition Scheme menu, and the other choices on this board
// are too small (Default allows 1.25 MB). partitions.csv still sets the real layout.
#if !defined(ARDUINO_PARTITION_huge_app) && !defined(ARDUINO_PARTITION_no_ota)
  #error "Pick Tools > Partition Scheme > Huge APP (3MB No OTA/1MB SPIFFS). It only sets the size check; partitions.csv in this folder gives the hub its two update slots."
#endif

// ======================= settings =======================
#ifndef GW_CRASH_TEST
  #define GW_CRASH_TEST 0      // 1 = rollback test build: crashes 20 s after every boot (the GitHub build makes one)
#endif
#if GW_CRASH_TEST
  #define GW_FW_VERSION "0.13.0-crashtest"
#else
  #define GW_FW_VERSION "0.13.0"
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
#ifndef HUB_SENSORS_DEFAULT
  #define HUB_SENSORS_DEFAULT 0                 // sensors wired to the hub until /setup says otherwise (LS_* bits in local_sensors.h; 0 = none)
#endif

// Soil mapping for factory-calibrated probes, on the probe scale (percent between the unit's air
// and water readings). Starting values from typical capacitive-probe data in potting mix; replace
// them with your own lab results (bone-dry mix and freshly drained mix, weighed on a scale).
static const float SOIL_DRY_PS = 30.0f;          // bone-dry potting mix reads about this on the probe scale
static const float SOIL_WET_PS = 75.0f;          // potting mix just watered and drained

static const uint32_t NODE_INTERVAL_MS = 60000; // must match REPORT_MS on the nodes (v0.13: 60 s; v0.9 to v0.12: 30 s; v0.6: 5 s)
static const uint32_t UPLOAD_MS        = 60000; // one upload a minute keeps Cloudflare D1 well inside its free daily limits
static const uint32_t DAY_BUCKET_MS    = 5UL * 60 * 1000;
static const uint32_t STALE_MS         = 300000; // card shows "no signal" after this (5 missed readings at 60 s)
static const uint32_t CLIMATE_MS       = 60000; // air temp/humidity and light sent to the cloud once a minute per node
static const uint32_t WIFI_RETRY_MS    = 30000; // Wi-Fi down this long: start the connection over (rescans, picks any mesh point)
static const uint32_t WIFI_REBOOT_MS   = 30UL * 60 * 1000;   // still down after this: restart the hub, once per outage
static const uint32_t SETUP_AFTER_MS   = 3UL * 60 * 1000;    // no Wi-Fi this long after power-up: open the setup hotspot too
static const uint32_t SETUP_OPEN_MS    = 15UL * 60 * 1000;   // hotspot opened with the BOOT button stays this long
static const uint32_t TRIAL_MIN_MS     = 10UL * 60 * 1000;   // a new firmware must run this long (plus the checks) to be kept
static const uint8_t  TRIAL_CRASHES    = 3;                  // crashes on trial before going back to the previous firmware
static const uint32_t WATCHDOG_MS      = 60000;              // the main loop must come round within this, or the hub resets
static const uint32_t SCAN_CLEAR_MS    = 10000;              // empty the Bluetooth library's list of devices heard this often (v0.12.1)
static const int      SETUP_BTN_PIN    = BOOT_PIN;           // the BOOT button (GPIO 9 on the ESP32-C6)
// ========================================================

// LIVE_LEN: the dashboard plots the last hour, 60 points at one reading per 60 s (120 at 30 s); 180 leaves room (v0.12.1, was 720).
static const int MAX_NODES = 8, MAX_SENSORS = 16, LIVE_LEN = 180, DAY_LEN = 288, QUEUE_LEN = 3000, CLIM_LEN = 600, LIGHT_LEN = 600;
static const int H_SLOTS = 12, DT_LEN = 16;               // health window: 12 x 5 min = the last hour; last 16 reading gaps
static const uint32_t H_SLOT_MS = 5UL * 60 * 1000;
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
  // node updates (v0.10)
  uint8_t addrType;              // Bluetooth address type from its adverts (random static for these nodes)
  bool connectable;              // its last advert accepted connections
  char fw[16];                   // firmware version it reported ("" = not checked yet)
  char st[56];                   // update status for the dashboard
  uint8_t pct;                   // update progress, percent
  bool updating, smallPkts;      // smallPkts: its bootloader only takes 20-byte packets
  uint32_t nextCheckAt;          // millis() of the next check (0 = soon)
  uint8_t fails;                 // failed updates in a row (sets the backoff)
  uint32_t rescueAt;             // after a failed transfer: look for its bootloader if it doesn't come back
  // diagnostics (v0.11)
  uint32_t firstSeen, adv;       // when first heard; every advert heard, repeats included
  uint16_t restarts;             // restarts seen (packet id back to the start)
  int8_t rssiMin, rssiMax;
  bool online;                   // for the online/offline events
  uint32_t lastCheckAt;          // last completed check or update job
  uint16_t checks;
  char diag[160];                // what the node reported about itself at the last check
  uint32_t diagAt;
  // health (v0.12): the last hour in 5-minute slots (hSlot is the current one), and the last 16 reading gaps
  uint16_t hRx[H_SLOTS], hLost[H_SLOTS], hAdv[H_SLOTS];
  int32_t hRssi[H_SLOTS];        // sum of advert signal per slot; divided by hAdv it's the average
  uint16_t dts[DT_LEN];          // time between new readings, 10 ms units
  uint8_t dtHead, dtN;
  uint32_t cloudTs;              // time stamp (epoch) of this node's newest soil reading the cloud accepted
  uint32_t windowAt;             // when it was last seen advertising between bursts (its link window after a reset)
  bool local;                    // the hub's own sensors (v0.12)
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
uint8_t nodeKey[16];                         // this hub's link key for its nodes (random, made on first boot)
volatile bool jobBusy = false;               // a node check or update is running (Bluetooth scan paused)
uint32_t jobStartedAt = 0;

// ---------------- event log and counters (v0.11) ----------------
struct LogEnt { uint32_t ms, epoch; char t[100]; };
static const int LOG_LEN = 60;
LogEnt logBuf[LOG_LEN];
int logHead = 0, logCount = 0;
SemaphoreHandle_t logLock;
volatile uint32_t btAll = 0;                 // every Bluetooth advert heard, any device
volatile uint32_t freshTotal = 0;            // new readings from GROUNDWELL nodes
uint32_t uploadsOk = 0, uploadsFail = 0, lastUploadAt = 0, lastUploadMs = 0, wifiUps = 0, wifiDownAt = 0;
char lastUploadErr[100] = "";
bool lastUploadGood = false;
uint16_t uploadFailStreak = 0;               // failed uploads in a row (v0.12)
uint32_t scanClears = 0;                     // times the Bluetooth device list was emptied (v0.12.1)
String crashText;                            // the crash dump in flash, summarised (v0.12.1); "" = none
bool crashIsMine = false;                    // that dump came from this firmware build
uint8_t hSlot = 0, hSlots = 1;               // health window: current slot, and how many slots hold data so far
uint32_t hSlotAt = 0;
char localMac[18] = "";                      // the hub's own sensors appear as a node with this address
volatile uint32_t localReadings = 0, localAirFails = 0, localLastAt = 0, localLastDt = 0;
volatile uint16_t localCap = 0;

static void logEvent(const char *fmt, ...) {   // Serial Monitor and the diagnostics page's event log
  char b[sizeof logBuf[0].t];
  va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
  Serial.println(b);
  if (!logLock) return;
  time_t now = time(nullptr);
  xSemaphoreTake(logLock, portMAX_DELAY);
  LogEnt &e = logBuf[logHead];
  e.ms = millis();
  e.epoch = now > 1700000000 ? (uint32_t)now : 0;
  strlcpy(e.t, b, sizeof e.t);
  logHead = (logHead + 1) % LOG_LEN;
  if (logCount < LOG_LEN) logCount++;
  xSemaphoreGive(logLock);
}

static const char *espResetText(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON: return "power-on"; case ESP_RST_EXT: return "reset pin"; case ESP_RST_SW: return "software restart";
    case ESP_RST_PANIC: return "crash"; case ESP_RST_INT_WDT: return "interrupt watchdog"; case ESP_RST_TASK_WDT: return "loop watchdog";
    case ESP_RST_WDT: return "watchdog"; case ESP_RST_DEEPSLEEP: return "deep sleep"; case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_USB: return "USB"; default: return "other";
  }
}

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
// local = a reading from the hub's own sensors (v0.12): same path, but it isn't Bluetooth traffic.
static void handlePacket(const char *mac, int rssi, uint8_t addrType, bool connectable, const BTHomePacket &p, bool local = false) {
  if (p.nCounts == 0 && !p.hasMoist) return;
  if (!local) { advHeard++; advTotal++; }
  xSemaphoreTake(lock, portMAX_DELAY);
  int ni = findNode(mac, true);
  if (ni >= 0) {
    Node &n = nodes[ni];
    uint32_t nowMs = millis();
    n.lastSeen = nowMs;
    n.rssi = (int8_t)rssi;
    n.addrType = addrType;
    n.connectable = connectable;
    n.rescueAt = 0;                                       // it's running its app again
    n.adv++;
    if (local && !n.local) {
      n.local = true;
      strlcpy(n.fw, FW_VERSION, sizeof n.fw);
      strlcpy(n.st, "wired to this hub", sizeof n.st);
    }
    if (!local) { n.hAdv[hSlot]++; n.hRssi[hSlot] += rssi; }
    if (!n.firstSeen) {
      n.firstSeen = nowMs | 1;
      n.rssiMin = n.rssiMax = (int8_t)rssi;
      if (local) logEvent("Sensors wired to this hub: first reading (as node %s)", mac);
      else logEvent("Node %s first heard (%d dBm)", mac, rssi);
    }
    if (rssi < n.rssiMin) n.rssiMin = (int8_t)rssi;
    if (rssi > n.rssiMax) n.rssiMax = (int8_t)rssi;
    bool fresh = !p.hasPid || !n.havePid || p.pid != n.lastPid;
    if (!fresh && !local && connectable && n.lastFreshAt && nowMs - n.lastFreshAt > 8000 &&
        (!n.windowAt || nowMs - n.windowAt > 600000)) {
      // Still advertising 8 s after its reading, when a burst lasts 6 s: the node is in its 10-minute link
      // window, so it was just reset or powered up. Check it now, while it can be linked.
      n.windowAt = nowMs | 1;
      if (!n.updating) n.nextCheckAt = 0;
      logEvent("Node %s is in its link window (just reset or powered up)", mac);
    }
    if (fresh) {
      if (!local) { advFresh++; freshTotal++; }
      n.hRx[hSlot]++;
      if (p.hasPid && n.havePid) {
        uint8_t gap = (uint8_t)(p.pid - n.lastPid);
        if (gap > 1 && gap < 50) { n.lost += gap - 1; n.hLost[hSlot] += gap - 1; }   // bigger jumps = node restarted
        // Restarted: the id went back to the start (the first packet after a reset is 1). The id is one byte,
        // so 255 -> 0 every 256 readings is the normal wrap, not a restart (v0.11 took it for one).
        if (p.pid <= 2 && gap >= 50 && !n.updating) {
          n.nextCheckAt = 0;
          n.restarts++;
          logEvent("Node %s restarted", mac);
        }
      }
      if (p.hasPid) { n.lastPid = p.pid; n.havePid = true; }
      n.rx++;
      if (n.lastFreshAt) {
        uint32_t dt = nowMs - n.lastFreshAt;
        n.ivlMs = n.ivlMs ? (n.ivlMs * 7 + dt) / 8 : dt;
        n.dts[n.dtHead] = (uint16_t)(dt / 10 > 65535 ? 65535 : dt / 10);
        n.dtHead = (n.dtHead + 1) % DT_LEN;
        if (n.dtN < DT_LEN) n.dtN++;
      }
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
      // A probe this node no longer sends (the rod probe, after a node update to 0.12): drop its card and
      // stop listing it in the upload. Its readings already queued still go to the cloud.
      for (int k = 0; k < MAX_SENSORS; k++) if (sensors[k].used && sensors[k].node == ni && sensors[k].ch >= nv) {
        logEvent("Node %s no longer sends probe %u (%s): card removed", mac, sensors[k].ch, sensors[k].name);
        sensors[k].used = false;
      }
    }
  }
  xSemaphoreGive(lock);
}

class ScanCallbacks : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice dev) override {
    btAll++;
    for (int i = 0; i < dev.getServiceDataCount(); i++) {
      if (!dev.getServiceDataUUID(i).equals(BLEUUID((uint16_t)0xFCD2))) continue;
      auto sd = dev.getServiceData(i);                   // String (core 3.x) or std::string (2.x)
      BTHomePacket p;
      if (!parseBTHome((const uint8_t *)sd.c_str(), sd.length(), p)) continue;
      String mac = String(dev.getAddress().toString().c_str());
      mac.toUpperCase();
      handlePacket(mac.c_str(), dev.getRSSI(), dev.getAddressType(), dev.isConnectable(), p);
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

// Health window (v0.12): every 5 minutes start a new slot, dropping the one from an hour ago.
static void rotateHealthSlot() {
  xSemaphoreTake(lock, portMAX_DELAY);
  hSlot = (hSlot + 1) % H_SLOTS;
  for (int i = 0; i < MAX_NODES; i++) { Node &n = nodes[i]; n.hRx[hSlot] = n.hLost[hSlot] = n.hAdv[hSlot] = 0; n.hRssi[hSlot] = 0; }
  if (hSlots < H_SLOTS) hSlots++;
  hSlotAt = millis();
  xSemaphoreGive(lock);
}

// ---------------- cloud upload ----------------
static void applyCloudCfg(const String &resp);

static void uploadBatch() {
  if (otaBusy || jobBusy || cfg.url.length() == 0 || WiFi.status() != WL_CONNECTED) return;

  // Snapshot up to 240 readings plus the sensor names/calibration, then release the lock.
  String body;
  int n, nc, nl;
  xSemaphoreTake(lock, portMAX_DELAY);
  n = qCount < 240 ? qCount : 240;
  nc = cCount < 100 ? cCount : 100;
  nl = lCount < 100 ? lCount : 100;
  if (n == 0 && nc == 0 && nl == 0) {
    xSemaphoreGive(lock);
    return;                                           // the device list is emptied every 10 s in loop()
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
  delay(20);                                          // let a Bluetooth event already in progress finish first
  scan->clearResults();

  int code = -1;
  char tlsErr[100] = "";
  uint32_t uploadStart = millis();
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
  lastUploadAt = millis();
  lastUploadMs = lastUploadAt - uploadStart;
  if (code == 200) {
    uploadsOk++;
    if (!lastUploadGood) logEvent(uploadsOk == 1 ? "Cloud upload OK (%d readings)" : "Cloud upload OK again (%d readings)", n);
    lastUploadGood = true;
    lastUploadErr[0] = 0;
  } else {
    uploadsFail++;
    snprintf(lastUploadErr, sizeof lastUploadErr, "%s%s%s", code == -100 ? "too little free memory" : code < 0 ? HTTPClient::errorToString(code).c_str() : "HTTP error",
             tlsErr[0] ? " / TLS: " : "", tlsErr);
    if (lastUploadGood || uploadsFail == 1) logEvent("Cloud upload failed: HTTP %d %s", code, lastUploadErr);
    lastUploadGood = false;
  }
  if (code == 200) uploadFailStreak = 0; else uploadFailStreak++;
  if (code == 200) {
    xSemaphoreTake(lock, portMAX_DELAY);
    int drop = n < qCount ? n : qCount;                 // readings that arrived meanwhile stay queued
    for (int k = 0; k < drop; k++) {                    // newest reading of each node the cloud now holds
      const Rec &r = queue_[(qHead + k) % QUEUE_LEN];
      if (r.ts > nodes[r.node].cloudTs) nodes[r.node].cloudTs = r.ts;
    }
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
// The dashboard and diagnostics pages: gzipped when the build made pages_gz.h (about 30 KB less flash).
static void sendAppPage(bool diag) {
#if GW_PAGES_GZ
  server.sendHeader("Content-Encoding", "gzip");
  server.send_P(200, "text/html", (PGM_P)(diag ? DIAG_PAGE_GZ : PAGE_GZ), diag ? DIAG_PAGE_GZ_LEN : PAGE_GZ_LEN);
#else
  server.send_P(200, "text/html", diag ? DIAG_PAGE : PAGE);
#endif
}

static void handleRoot() {
  if (!cfg.ssid.length()) { server.sendHeader("Location", "/setup"); server.send(302, "text/plain", ""); return; }
  sendAppPage(false);
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
    j += ",\"wired\":"; j += nd.local ? "true" : "false";
    j += ",\"nfw\":\""; j += nd.fw;
    j += "\",\"nst\":\""; j += nd.st;
    if (nd.updating) { j += " ("; j += nd.pct; j += "%)"; }
    j += '"';
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
  if (applied) logEvent("Applied %d calibration change(s) from the cloud dashboard", applied);
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
  if (cfgPrefs.getBytesLength("nodekey") != sizeof nodeKey) {
    esp_fill_random(nodeKey, sizeof nodeKey);
    cfgPrefs.putBytes("nodekey", nodeKey, sizeof nodeKey);
  }
  cfgPrefs.getBytes("nodekey", nodeKey, sizeof nodeKey);
}

// ---------------- setup hotspot ----------------
static String apName() { String m = WiFi.macAddress(); m.replace(":", ""); return "groundwell-setup-" + m.substring(8); }

static void openSetup(uint32_t forMs, const char *why) {
  if (!apOn) {
    WiFi.softAP(apName().c_str(), SETUP_AP_PASS);
    dns.start(53, "*", WiFi.softAPIP());          // every name points at the hub, so phones show the setup page
    apOn = true;
    logEvent("Setup hotspot open (%s): join \"%s\", then open http://%s/setup", why, apName().c_str(), WiFi.softAPIP().toString().c_str());
    Serial.printf("  hotspot password: \"%s\"\n", SETUP_AP_PASS);
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
  logEvent("Setup hotspot closed");
}

static bool fromHotspot() { return apOn && server.client().localIP() == WiFi.softAPIP(); }

// ---------------- crash reports (v0.12.1) ----------------
// The ESP32 writes a crash dump to the "coredump" partition on every crash (core setting). At start, summarise
// it in one line: reason, task, program counter, return address and the code addresses on that task's stack,
// newest first. Decode them with the .elf of the build that crashed (riscv32-esp-elf-addr2line -pfiaC -e ...).
// A new dump is logged once; the summary stays on /diag (and goes into the rollback note) until the next crash.
static void readCrashDump() {
  if (esp_core_dump_image_check() != ESP_OK) return;          // no dump in flash
  esp_core_dump_summary_t *sum = (esp_core_dump_summary_t *)calloc(1, sizeof(esp_core_dump_summary_t));
  if (!sum) return;
  if (esp_core_dump_get_summary(sum) == ESP_OK) {
    char reason[96] = "";
    if (esp_core_dump_get_panic_reason(reason, sizeof reason) != ESP_OK) reason[0] = 0;
    char task[17], sha[10], mine[10];
    memcpy(task, sum->exc_task, 16); task[16] = 0;
    memcpy(sha, sum->app_elf_sha256, 9); sha[9] = 0;
    esp_app_get_elf_sha256(mine, sizeof mine);
    crashIsMine = !strncmp(sha, mine, 9);
    char b[420];
    int k = snprintf(b, sizeof b, "%s; task %s, pc %08lx, ra %08lx, mcause %lx, mtval %lx; build %s; stack", reason[0] ? reason : "crash",
                     task, (unsigned long)sum->exc_pc, (unsigned long)sum->ex_info.ra, (unsigned long)sum->ex_info.mcause,
                     (unsigned long)sum->ex_info.mtval, sha);
    int found = 0;
    for (uint32_t off = 0; off + 4 <= sum->exc_bt_info.dump_size && found < 16 && k < (int)sizeof b - 10; off += 4) {
      uint32_t v;
      memcpy(&v, sum->exc_bt_info.stackdump + off, 4);
      if ((v >= 0x42000000 && v < 0x42800000) || (v >= 0x40800000 && v < 0x40880000)) {   // flash or RAM code
        k += snprintf(b + k, sizeof b - k, " %08lx", (unsigned long)v);
        found++;
      }
    }
    crashText = b;
    String id = String(sha) + String(sum->exc_pc, HEX) + String(sum->ex_info.ra, HEX);
    if (otaPrefs.getString("crashid", "") != id) {           // a new crash
      otaPrefs.putString("crashid", id);
      logEvent("Crash on record (%s): %s", crashIsMine ? "this firmware" : "another build", b);
    }
  }
  free(sum);
}

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

static void trialCheckAtBoot() {   // otaPrefs is open (setup)
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
  if (crash) logEvent("Firmware %s on trial: crash %u of %u (%s)", FW_VERSION, crashes, TRIAL_CRASHES, espResetText(r));
  if (crashes >= TRIAL_CRASHES) {
    String back = otaPrefs.getString("prevVer", "the previous firmware");
    otaPrefs.putBool("trial", false);
    if (Update.rollBack()) {
      otaPrefs.putString("note", String("Firmware ") + FW_VERSION + " crashed " + crashes + " times on trial, so the hub went back to " + back + "." +
                                 (crashIsMine && crashText.length() ? String(" Last crash: ") + crashText : String("")));
      logEvent("Going back to firmware %s", back.c_str());
      delay(200);
      ESP.restart();
    }
    otaPrefs.putString("note", String("Firmware ") + FW_VERSION + " crashed on trial, and the previous firmware couldn't be restored." +
                               (crashIsMine && crashText.length() ? String(" Last crash: ") + crashText : String("")));
    return;
  }
  trialActive = true;
}

static void trialMaybeConfirm() {   // proven: 10 minutes up, on Wi-Fi, a node heard, and a cloud upload accepted (if the cloud is on)
  if (!trialActive || millis() < TRIAL_MIN_MS) return;
  // A node heard over Bluetooth; or, for a hub with only its own sensors, a reading from them and proof that
  // the Bluetooth scanner works (it hears some device, a phone say).
  if (WiFi.status() != WL_CONNECTED || !(advTotal > 0 || (localReadings > 0 && btAll > 0))) return;
  if (cfg.url.length() && !lastUploadOk) return;
  trialActive = false;
  otaPrefs.putBool("trial", false);
  otaPrefs.putString("note", String("Firmware ") + FW_VERSION + " passed its trial.");
  logEvent("Firmware %s passed its trial", FW_VERSION);
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
  "table{border-collapse:collapse}td{padding:.15rem .8rem .15rem 0}.c{font-weight:400;display:flex;gap:.5rem;align-items:flex-start;margin:.35rem 0}"
  ".c input{width:auto;margin-top:.2rem}</style></head><body><h1>groundwell hub</h1>";

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
  h += "<p class=note>Firmware "; h += FW_VERSION; h += " · <a href=/>Dashboard</a> · <a href=/diag>Diagnostics</a> · <a href=/update>Update</a> · <a href=/setup>Settings</a></p></body></html>";
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
  uint8_t m = cfgPrefs.getUChar("hubsens", HUB_SENSORS_DEFAULT);
  auto chk = [&](const char *name, uint8_t bit, const char *text) {
    b += "<label class=c><input type=checkbox name="; b += name; b += (m & bit) ? " checked" : ""; b += "> "; b += text; b += "</label>";
  };
  b += "<label>Sensors wired to this hub</label>";
  chk("s_on", LS_ON, "Capacitive probe on D0, powered from D1 (turns the hub's own sensors on)");
  chk("s_dht", LS_DHT, "DHT11 on D6");
  chk("s_ldr", LS_LDR, "Photoresistor from D7 to D2 (1 kOhm from D2 to GND)");
#if HUB_I2C_SENSORS
  chk("s_i2c", LS_I2C, "I2C sensors on D4/D5 (SHT40 or AHT20, LTR390; found automatically)");
#endif
  b += "<div class=note>The hub reads these like a node, every 60 s, and shows them as one more pot. Leave all unticked for a hub without sensors.</div>";
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
  uint8_t sens = 0;
  if (server.hasArg("s_on")) {
    sens = LS_ON | (server.hasArg("s_dht") ? LS_DHT : 0) | (server.hasArg("s_ldr") ? LS_LDR : 0) | (server.hasArg("s_i2c") ? LS_I2C : 0);
  }
  cfgPrefs.putUChar("hubsens", sens);
  logEvent("Settings saved on /setup: restarting to join %s", ssid.c_str());
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
  b += "<h1>Nodes</h1><p class=note>";
  if (NODE_FW_BUNDLED) { b += "This hub carries node firmware <b>"; b += NODE_FW_VERSION; b += "</b> and updates its nodes to it over Bluetooth."; }
  else b += "This hub build carries no node firmware, so it checks its nodes but doesn't update them.";
  b += "</p><table>";
  xSemaphoreTake(lock, portMAX_DELAY);
  int shown = 0;
  for (int i = 0; i < MAX_NODES; i++) if (nodes[i].used) {
    shown++;
    b += "<tr><td>"; b += nodes[i].mac + 9; b += "</td><td>"; b += nodes[i].fw[0] ? nodes[i].fw : "?"; b += "</td><td>";
    b += htmlEsc(nodes[i].st[0] ? String(nodes[i].st) : String("not checked yet"));
    if (nodes[i].updating) { b += " ("; b += nodes[i].pct; b += "%)"; }
    b += "</td></tr>";
  }
  xSemaphoreGive(lock);
  if (!shown) b += "<tr><td>No nodes heard yet.</td></tr>";
  b += "</table>";
  sendPage(200, b);
}

static bool upAuthed = false;
static String upErr;
static GwImgCheck upChk;                            // decides from the file's first 36 bytes, however they arrive
#ifdef CONFIG_IDF_FIRMWARE_CHIP_ID
static const uint16_t HUB_CHIP_ID = CONFIG_IDF_FIRMWARE_CHIP_ID;
#else
static const uint16_t HUB_CHIP_ID = GW_IMG_ANY_CHIP;
#endif
static const char *NOT_HUB_FILE = "That isn't a hub firmware file. Pick GroundwellHub.ino.bin (not the merged file, not a node file).";

static void handleUpdateUpload() {
  HTTPUpload &up = server.upload();
  if (up.status == UPLOAD_FILE_START) {
    upErr = "";
    gwImgReset(upChk);
    upAuthed = cfg.otapw.length() && server.authenticate("admin", cfg.otapw.c_str());
    if (!upAuthed) { upErr = "Update password needed."; return; }
    const esp_partition_t *run = esp_ota_get_running_partition();
    if (!esp_ota_get_next_update_partition(NULL)) { upErr = "This hub has a single app slot: flash it over USB once."; return; }
    if (run && ESP.getSketchSize() > run->size) { upErr = "The running firmware is bigger than its slot: flash over USB."; return; }
    if (jobBusy) { upErr = "The hub is updating a node right now. Try again in a few minutes."; return; }
    otaBusy = true;
    scan->stop();                                   // give Wi-Fi the radio while the file comes in
    if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) upErr = String("Couldn't start: ") + Update.errorString();
  } else if (up.status == UPLOAD_FILE_WRITE) {
    esp_task_wdt_reset();
    if (!upAuthed || upErr.length()) return;
    // Not "Update.progress() == 0": progress only moves once Update has a full 4 KB sector, and the web server
    // hands the file over in 1436-byte pieces (v0.11.0 checked the second piece and refused every upload).
    if (gwImgFeed(upChk, up.buf, up.currentSize, HUB_CHIP_ID) == GW_IMG_BAD) { upErr = NOT_HUB_FILE; Update.abort(); return; }
    if (Update.write(up.buf, up.currentSize) != up.currentSize) { upErr = String("Write failed: ") + Update.errorString(); Update.abort(); }
  } else if (up.status == UPLOAD_FILE_END) {
    if (upAuthed && !upErr.length() && !gwImgAccepted(upChk)) { upErr = NOT_HUB_FILE; Update.abort(); }   // shorter than a header
    if (upAuthed && !upErr.length() && !Update.end(true)) upErr = String("The file didn't check out: ") + Update.errorString();
  } else if (up.status == UPLOAD_FILE_ABORTED) {      // the browser went away: the done handler won't run
    if (Update.isRunning()) Update.abort();
    upErr = "Upload interrupted. Nothing was changed.";
    if (otaBusy) { otaBusy = false; scan->start(0, nullptr, false); }
    logEvent("Web update interrupted: nothing changed");
  }
}

static void handleUpdateDone() {
  if (!upAuthed) { server.requestAuthentication(BASIC_AUTH, "groundwell hub", "Update password needed (user admin)"); return; }
  if (upErr.length() || !Update.isFinished()) {
    if (!upErr.length()) upErr = "The upload was incomplete. Nothing was changed.";
    if (Update.isRunning()) Update.abort();
    if (otaBusy) { otaBusy = false; scan->start(0, nullptr, false); }
    server.send(400, "text/plain", upErr + " The hub keeps running " + FW_VERSION + ".");
    logEvent("Web update failed: %s", upErr.c_str());
    return;
  }
  trialStart();
  server.send(200, "text/plain", "Done. The hub is restarting into the new firmware; this page reloads in about 25 s.");
  logEvent("Web update written: restarting into the new firmware");
  delay(800);
  ESP.restart();
}

static void startArduinoOta() {   // Arduino IDE network uploads, with the same password
  if (arduinoOtaUp || cfg.otapw.length() < 8) return;
  ArduinoOTA.setHostname(HOSTNAME);
  ArduinoOTA.setPassword(cfg.otapw.c_str());
  ArduinoOTA.setMdnsEnabled(false);                 // the hub runs mDNS itself (see loop)
  ArduinoOTA.onStart([]() { otaBusy = true; scan->stop(); logEvent("Arduino IDE upload started"); });
  ArduinoOTA.onProgress([](unsigned int, unsigned int) { esp_task_wdt_reset(); });
  ArduinoOTA.onEnd([]() { trialStart(); logEvent("Arduino IDE upload done: restarting into the new firmware"); });
  ArduinoOTA.onError([](ota_error_t e) { otaBusy = false; if (!jobBusy) scan->start(0, nullptr, false); logEvent("Arduino IDE upload failed (error %u)", (unsigned)e); });
  ArduinoOTA.begin();
  arduinoOtaUp = true;
}

// ---------------- node checks, linking and updates over Bluetooth (v0.10) ----------------
// One job at a time, in its own task, with the Bluetooth scan paused. A job connects to a node during one
// of its bursts, reads its firmware version, links it (window after its reset) or proves the link key,
// and if the bundled node firmware is newer, hands it to the node's bootloader (node_dfu.h).
static const char *GW_SVC   = "6a1c0001-5f2b-4c1e-9d55-67726f756e64";
static const char *GW_INFO  = "6a1c0002-5f2b-4c1e-9d55-67726f756e64";
static const char *GW_NONCE = "6a1c0003-5f2b-4c1e-9d55-67726f756e64";
static const char *GW_AUTH  = "6a1c0004-5f2b-4c1e-9d55-67726f756e64";
static const char *GW_KEY   = "6a1c0005-5f2b-4c1e-9d55-67726f756e64";
static const char *GW_DIAG  = "6a1c0006-5f2b-4c1e-9d55-67726f756e64";
static const char *DFU_SVC  = "00001530-1212-efde-1523-785feabcd123";
static const char *DFU_CTRL = "00001531-1212-efde-1523-785feabcd123";
static const char *DFU_PKT  = "00001532-1212-efde-1523-785feabcd123";
static const uint32_t CHECK_EVERY_MS = 24UL * 3600 * 1000;
static const float    UPDATE_MIN_V   = 2.7f;      // below this a coin cell may sag mid-update: wait
static const uint32_t JOB_MAX_MS     = 8UL * 60 * 1000;
static const uint8_t  ADV_BURST_COPIES = 24;     // a node repeats each reading every 250 ms for 6 s (node sketch: ADV_INTERVAL_MS, ADV_BURST_S)

static BLEClient *nodeClient = nullptr;            // one client for the hub's lifetime (the library keeps a pointer to it)
static QueueHandle_t notifQ;
struct Notif { uint8_t n; uint8_t d[20]; };
static int jobNode = -1;
static bool jobRescue = false;

static void onDfuNotify(BLERemoteCharacteristic *, uint8_t *data, size_t len, bool) {
  Notif m;
  m.n = len > sizeof m.d ? sizeof m.d : len;
  memcpy(m.d, data, m.n);
  xQueueSend(notifQ, &m, 0);
}

struct BleDfuLink : DfuLink {
  BLERemoteCharacteristic *ctrl = nullptr, *pkt = nullptr;
  int node = 0;
  bool writeCtrl(const uint8_t *d, size_t n) override { return ctrl->writeValue((uint8_t *)d, n, true); }
  bool writePkt(const uint8_t *d, size_t n) override {   // without response: retry while the stack's buffers are full
    for (int tries = 0; tries < 400; tries++) {
      if (pkt->writeValue((uint8_t *)d, n, false)) return true;
      if (!nodeClient->isConnected()) return false;
      delay(2);
    }
    return false;
  }
  bool waitNotify(uint8_t *out, size_t &n, uint32_t ms) override {
    Notif m;
    if (xQueueReceive(notifQ, &m, pdMS_TO_TICKS(ms)) != pdTRUE) return false;
    memcpy(out, m.d, m.n); n = m.n;
    return true;
  }
  void drain() override { xQueueReset(notifQ); }
  void progress(uint32_t done, uint32_t total) override { nodes[node].pct = (uint8_t)(done * 100ULL / total); }
};

static void nodeStatus(int i, const char *fmt, ...) {
  char b[sizeof nodes[0].st];
  va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
  xSemaphoreTake(lock, portMAX_DELAY);
  strlcpy(nodes[i].st, b, sizeof nodes[i].st);
  xSemaphoreGive(lock);
  logEvent("Node %s: %s", nodes[i].mac + 9, b);
}

static bool nodeConnect(const char *mac, uint8_t type, bool plusOne, uint32_t ms) {
  char m[18];
  strlcpy(m, mac, sizeof m);
  if (plusOne) {                                   // the bootloader's address when it starts without handover data:
    unsigned last = strtoul(m + 15, nullptr, 16);  // lowest byte + 1, the last pair in "AA:BB:CC:DD:EE:FF"
    snprintf(m + 15, 3, "%02X", (last + 1) & 0xFF);
  }
  BLEAddress a{String(m), type};                   // (byte order differs between Bluetooth stacks; the text doesn't)
#if defined(CONFIG_BLUEDROID_ENABLED)
  esp_ble_gattc_cache_refresh(a.getNative());     // same address, different services once it's the bootloader
#endif
  if (!nodeClient->connect(a, type, ms)) return false;
  nodeClient->getServices();                       // fresh lookup: the client would otherwise reuse the last list
  return true;
}

static void nodeDisconnect() {
  if (nodeClient->isConnected()) nodeClient->disconnect();
  for (uint32_t t = millis(); nodeClient->isConnected() && millis() - t < 3000;) delay(20);
}

static uint32_t nodeFailed(int i, const char *why) {
  uint8_t f = ++nodes[i].fails;
  uint32_t mins = f >= 6 ? 360 : 10u << (f - 1);   // 10, 20, 40, 80, 160 min, then 6 h
  nodeStatus(i, "update failed (%s); retry in %lu min", why, (unsigned long)mins);
  return mins * 60000UL;
}

static bool nodeAuth(BLERemoteService *gw) {       // prove the link key: HMAC over the node's fresh nonce
  BLERemoteCharacteristic *cn = gw->getCharacteristic(BLEUUID(GW_NONCE)), *ca = gw->getCharacteristic(BLEUUID(GW_AUTH));
  if (!cn || !ca) return false;
  String n = cn->readValue();
  const uint8_t *raw = cn->readRawData();
  if (n.length() != 16 || !raw) return false;
  uint8_t tag[32];
  gw_hmac_sha256(nodeKey, sizeof nodeKey, raw, 16, tag);
  return ca->writeValue(tag, 16, true);
}

// The bootloader is running (just handed over, or stuck after a failed try): send it the firmware.
static uint32_t nodeTransfer(int i, const char *mac, uint8_t type, bool small) {
  bool up = false;
  for (int a = 0; a < 6 && !up; a++) up = nodeConnect(mac, type, a & 1, 4000);   // same address first, then +1
  if (!up) { nodes[i].rescueAt = (millis() + 90000) | 1; return nodeFailed(i, "its bootloader didn't answer"); }
  BLERemoteService *dfu = nodeClient->getService(BLEUUID(DFU_SVC));
  BleDfuLink link;
  link.node = i;
  link.ctrl = dfu ? dfu->getCharacteristic(BLEUUID(DFU_CTRL)) : nullptr;
  link.pkt  = dfu ? dfu->getCharacteristic(BLEUUID(DFU_PKT)) : nullptr;
  if (!link.ctrl || !link.pkt) { nodeDisconnect(); nodes[i].rescueAt = (millis() + 90000) | 1; return nodeFailed(i, "no update service in its bootloader"); }
  xQueueReset(notifQ);
  link.ctrl->registerForNotify(onDfuNotify);
  uint16_t mtu = nodeClient->getMTU();
  uint16_t pkt = (small || mtu <= 23) ? 20 : (uint16_t)((mtu - 3 > 244 ? 244 : mtu - 3) & ~3);
  logEvent("Node %s: sending %lu bytes in %u-byte packets (MTU %u)", mac + 9, (unsigned long)NODE_FW_LEN, pkt, mtu);
  uint32_t t0 = millis();
  char err[96];
  const char *e = dfuTransfer(link, NODE_FW_BIN, NODE_FW_LEN, NODE_FW_DAT, NODE_FW_DAT_LEN, pkt, 8, err, sizeof err);
  if (e) {
    uint8_t r = DFU_OP_RESET;
    if (nodeClient->isConnected()) link.ctrl->writeValue(&r, 1, false);   // bootloader restarts: back to the old firmware
    nodeDisconnect();
    if (pkt > 20) nodes[i].smallPkts = true;      // an older bootloader only takes 20-byte packets: use those next time
    nodes[i].rescueAt = (millis() + 90000) | 1;
    return nodeFailed(i, e);
  }
  nodeDisconnect();
  nodes[i].fails = 0;
  nodes[i].rescueAt = 0;
  nodeStatus(i, "updated to %s in %lu s", NODE_FW_VERSION, (unsigned long)((millis() - t0) / 1000));
  return 45000;                                    // check its new version (and link it) on a later burst
}

static uint32_t nodeCheck(int i, const char *mac, uint8_t type, float volt, bool small) {
  if (!nodeConnect(mac, type, false, 5000)) return 120000;   // missed its burst: try again shortly, not a failure
  String ver, info;
  bool linked = false, window = false;
  float v = volt;
  BLERemoteService *gw = nodeClient->getService(BLEUUID(GW_SVC));
  if (gw) {
    BLERemoteCharacteristic *ci = gw->getCharacteristic(BLEUUID(GW_INFO));
    if (ci) info = ci->readValue();                // "0.10.0;l=1;w=0;v=3.29"
    int semi = info.indexOf(';');
    ver = semi >= 0 ? info.substring(0, semi) : info;
    linked = info.indexOf(";l=1") >= 0;
    window = info.indexOf(";w=1") >= 0;
    int vi = info.indexOf(";v=");
    if (vi >= 0) v = info.substring(vi + 3).toFloat();
    BLERemoteCharacteristic *cd = gw->getCharacteristic(BLEUUID(GW_DIAG));   // node 0.11+: its own health report
    if (cd) {
      String d = cd->readValue();
      xSemaphoreTake(lock, portMAX_DELAY);
      size_t k = 0;
      for (size_t c = 0; c < d.length() && k < sizeof nodes[i].diag - 1; c++) { char ch = d[c]; if (ch >= 32 && ch < 127 && ch != '"' && ch != '\\') nodes[i].diag[k++] = ch; }
      nodes[i].diag[k] = 0;
      nodes[i].diagAt = millis() | 1;
      xSemaphoreGive(lock);
    }
  } else {                                         // before v0.10: version from Device Information
    BLERemoteService *dis = nodeClient->getService(BLEUUID((uint16_t)0x180A));
    BLERemoteCharacteristic *cs = dis ? dis->getCharacteristic(BLEUUID((uint16_t)0x2A28)) : nullptr;
    if (cs) ver = cs->readValue();
  }
  ver.trim();
  String clean;                                    // keep it to a plain version string (it goes into the page and JSON)
  for (size_t k = 0; k < ver.length() && clean.length() < 15; k++) { char ch = ver[k]; if (isalnum((unsigned char)ch) || ch == '.' || ch == '-') clean += ch; }
  ver = clean;
  xSemaphoreTake(lock, portMAX_DELAY);
  strlcpy(nodes[i].fw, ver.length() ? ver.c_str() : "?", sizeof nodes[i].fw);
  xSemaphoreGive(lock);

  if (gw && !(linked && nodeAuth(gw))) {
    if (window) {                                  // the node was just reset: link it to this hub
      BLERemoteCharacteristic *ck = gw->getCharacteristic(BLEUUID(GW_KEY));
      bool ok = ck && ck->writeValue(nodeKey, sizeof nodeKey, true);
      nodeDisconnect();
      nodeStatus(i, ok ? "linked to this hub" : "linking failed");
      return ok ? 40000 : 120000;
    }
    nodeDisconnect();
    nodeStatus(i, linked ? "linked to another hub: press its reset button" : "not linked: press its reset button");
    return 3600000;
  }
  if (!NODE_FW_BUNDLED || dfuVersionCompare(NODE_FW_VERSION, ver.c_str()) <= 0) {
    nodeDisconnect();
    nodes[i].fails = 0;
    nodeStatus(i, "up to date (%s)", ver.length() ? ver.c_str() : "?");
    return CHECK_EVERY_MS;
  }
  if (v > 0.5f && v < UPDATE_MIN_V) {
    nodeDisconnect();
    nodeStatus(i, "battery %.2f V: update to %s waits", v, NODE_FW_VERSION);
    return 6UL * 3600 * 1000;
  }
  BLERemoteService *dfu = nodeClient->getService(BLEUUID(DFU_SVC));
  BLERemoteCharacteristic *ctrl = dfu ? dfu->getCharacteristic(BLEUUID(DFU_CTRL)) : nullptr;
  if (!ctrl) { nodeDisconnect(); nodeStatus(i, "%s has no update service: flash it over USB", ver.c_str()); return CHECK_EVERY_MS; }
  ctrl->registerForNotify(onDfuNotify);            // the node only hands over with notifications on
  nodes[i].updating = true;
  nodes[i].pct = 0;
  nodeStatus(i, "updating %s to %s", ver.c_str(), NODE_FW_VERSION);
  uint8_t go[2] = {DFU_OP_START, DFU_IMAGE_APP};
  bool ok = ctrl->writeValue(go, 2, true);
  if (!ok && nodeClient->isConnected()) {          // refused (an old node outside its window): it's still running
    nodeDisconnect();
    nodes[i].updating = false;
    nodeStatus(i, "%s refused the update: press its reset button", ver.c_str());
    return 3600000;
  }
  nodeDisconnect();                                // the node is restarting into its bootloader
  delay(1500);
  uint32_t next = nodeTransfer(i, mac, type, small);
  nodes[i].updating = false;
  return next;
}

static void nodeJobTask(void *) {
  int i = jobNode;
  char mac[18];
  xSemaphoreTake(lock, portMAX_DELAY);
  strlcpy(mac, nodes[i].mac, sizeof mac);
  uint8_t type = nodes[i].addrType;
  float volt = nodes[i].volt;
  bool small = nodes[i].smallPkts;
  xSemaphoreGive(lock);
  uint32_t next;
  if (jobRescue) {
    nodes[i].updating = true;
    nodeStatus(i, "didn't come back: looking for its bootloader");
    next = nodeTransfer(i, mac, type, true);
    nodes[i].updating = false;
  } else {
    next = nodeCheck(i, mac, type, volt, small);
  }
  nodes[i].nextCheckAt = (millis() + next) | 1;
  nodes[i].lastCheckAt = millis() | 1;
  nodes[i].checks++;
  scan->start(0, nullptr, false);
  jobBusy = false;
  vTaskDelete(NULL);
}

// Called from loop(): start a job for a node that is advertising right now and is due, or rescue one
// whose update failed and that hasn't come back.
static void nodeUpdaterLoop() {
  uint32_t now = millis();
  if (jobBusy) {
    if (now - jobStartedAt > JOB_MAX_MS) { logEvent("Node job stuck for 8 min: restarting the hub"); delay(200); ESP.restart(); }
    return;
  }
  if (otaBusy || now < 60000 || !nodeClient) return;   // let the hub settle after boot
  int pick = -1;
  bool rescue = false;
  xSemaphoreTake(lock, portMAX_DELAY);
  for (int i = 0; i < MAX_NODES && pick < 0; i++) {
    Node &n = nodes[i];
    if (!n.used) continue;
    if (n.rescueAt && (int32_t)(now - n.rescueAt) >= 0 && now - n.lastSeen > 60000) {
      if (n.fails >= 4) { n.rescueAt = 0; strlcpy(n.st, "stuck in its bootloader: flash it over USB", sizeof n.st); continue; }
      pick = i; rescue = true;
    } else if (n.connectable && now - n.lastSeen < 2500 && (!n.nextCheckAt || (int32_t)(now - n.nextCheckAt) >= 0)) {
      pick = i;
    }
  }
  if (pick >= 0) nodes[pick].rescueAt = 0;
  xSemaphoreGive(lock);
  if (pick < 0) return;
  jobBusy = true;
  jobStartedAt = now;
  jobNode = pick;
  jobRescue = rescue;
  scan->stop();
  scan->clearResults();
  if (xTaskCreate(nodeJobTask, "gwnode", 8192, nullptr, 1, nullptr) != pdPASS) { jobBusy = false; scan->start(0, nullptr, false); }
}

// ---------------- diagnostics page (v0.11) ----------------
static void jsonStr(String &j, const char *s) {    // a JSON string, escaped
  j += '"';
  for (; *s; s++) {
    char c = *s;
    if (c == '"' || c == '\\') { j += '\\'; j += c; }
    else if ((uint8_t)c < 32) j += ' ';
    else j += c;
  }
  j += '"';
}
static void jsonKV(String &j, const char *k, const char *v) { j += '"'; j += k; j += "\":"; jsonStr(j, v); j += ','; }
static void jsonKV(String &j, const char *k, long v) { j += '"'; j += k; j += "\":"; j += v; j += ','; }
static void jsonKB(String &j, const char *k, bool v) { j += '"'; j += k; j += "\":"; j += v ? "true" : "false"; j += ','; }
static void jsonEnd(String &j, char close) { if (j.endsWith(",")) j.remove(j.length() - 1); j += close; }
static long ageS(uint32_t now, uint32_t at) { return at ? (long)((now - at) / 1000) : -1L; }

static String activityNow() {
  if (otaBusy) return "Receiving a hub firmware update";
  if (jobBusy && jobNode >= 0) {
    Node &n = nodes[jobNode];
    if (n.updating) return String("Updating node ") + n.mac + ": " + n.pct + "%";
    return String(jobRescue ? "Looking for the bootloader of node " : "Checking node ") + n.mac;
  }
  if (apOn && WiFi.softAPgetStationNum()) return "Setup page in use on the hotspot";
  if (WiFi.status() != WL_CONNECTED) return cfg.ssid.length() ? "Waiting for Wi-Fi" : "Waiting for setup (hotspot open)";
  return "Listening for nodes";
}

static void handleDiagPage() { if (setupAuthOk()) sendAppPage(true); }

static void handleDiagCheck() {                    // POST i=<node>: check that node at its next burst
  if (!setupAuthOk()) return;
  int i = server.arg("i").toInt();
  if (i < 0 || i >= MAX_NODES || !nodes[i].used) { server.send(400, "text/plain", "bad node"); return; }
  nodes[i].nextCheckAt = 0;
  logEvent("Node %s: check requested from the diagnostics page", nodes[i].mac);
  server.send(200, "application/json", "{\"ok\":true}");
}

static void handleDiagApi() {
  if (!setupAuthOk()) return;
  uint32_t now = millis();
  time_t epoch = time(nullptr);
  const esp_partition_t *run = esp_ota_get_running_partition();
  String j;
  j.reserve(18000);
  j += '{';
  jsonKV(j, "now", activityNow().c_str());
  jsonKV(j, "uptimeS", (long)(now / 1000));
  jsonKV(j, "epoch", epoch > 1700000000 ? (long)epoch : 0L);
  // What a healthy system looks like (v0.12): the page's health checks compare against these.
  j += "\"expect\":{";
  jsonKV(j, "nodeIvlS", (long)(NODE_INTERVAL_MS / 1000));
  jsonKV(j, "uploadS", (long)(UPLOAD_MS / 1000));
  jsonKV(j, "staleS", (long)(STALE_MS / 1000));
  jsonKV(j, "minMv", (long)lroundf(UPDATE_MIN_V * 1000));
  jsonKV(j, "nodeFw", NODE_FW_VERSION);
  jsonKB(j, "bundled", NODE_FW_BUNDLED);
  jsonKV(j, "checkS", (long)(CHECK_EVERY_MS / 1000));
  jsonKV(j, "hourS", (long)((hSlots - 1) * (H_SLOT_MS / 1000) + (now - hSlotAt) / 1000));   // how much of the last hour the counts cover
  jsonKV(j, "copies", (long)(ADV_BURST_COPIES));
  jsonEnd(j, '}'); j += ',';
  j += "\"hub\":{";
  jsonKV(j, "fw", FW_VERSION);
  jsonKV(j, "slot", run ? run->label : "?");
  jsonKB(j, "trial", trialActive);
  jsonKV(j, "reset", espResetText(esp_reset_reason()));
  jsonKV(j, "heap", (long)ESP.getFreeHeap());
  jsonKV(j, "heapMin", (long)ESP.getMinFreeHeap());
  jsonKV(j, "heapBlock", (long)ESP.getMaxAllocHeap());
  jsonKV(j, "sketch", (long)ESP.getSketchSize());
  jsonKV(j, "slotSize", run ? (long)run->size : 0L);
  jsonKV(j, "nodeFw", NODE_FW_VERSION);
  jsonKV(j, "nodeFwLen", (long)NODE_FW_LEN);
  jsonKV(j, "note", otaPrefs.getString("note", "").c_str());
  jsonKV(j, "crash", crashText.c_str());           // v0.12.1: the crash dump in flash, summarised
  jsonKB(j, "crashMine", crashIsMine);
  j += "\"sens\":{";                                // sensors wired to the hub (v0.12)
  jsonKB(j, "on", lsMask & LS_ON);
  jsonKB(j, "dht", lsMask & LS_DHT); jsonKB(j, "ldr", lsMask & LS_LDR); jsonKB(j, "i2c", lsMask & LS_I2C);
  jsonKV(j, "mac", localMac);
  jsonKV(j, "air", airSrc);
  jsonKV(j, "light", lightName());
  jsonKB(j, "uv", ltrOk);
  jsonKB(j, "i2cFound", shtOk || ahtOk || ltrOk);
  jsonKB(j, "cal", calOk);
  jsonKV(j, "calAir", calOk ? (long)cal.air : 0L); jsonKV(j, "calWater", calOk ? (long)cal.water : 0L);
  jsonKV(j, "n", (long)localReadings);
  jsonKV(j, "airFail", (long)localAirFails);
  jsonKV(j, "lastS", ageS(now, localLastAt));
  jsonKV(j, "dtMs", (long)localLastDt);
  jsonKB(j, "fast", localFast());
  jsonKV(j, "cap", (long)localCap); jsonKV(j, "ldrAdc", (long)ldrAdc);
  jsonEnd(j, '}'); j += ',';
  jsonEnd(j, '}'); j += ',';
  bool wup = WiFi.status() == WL_CONNECTED;
  j += "\"wifi\":{";
  jsonKB(j, "up", wup);
  jsonKV(j, "ssid", cfg.ssid.c_str());
  jsonKV(j, "ip", wup ? WiFi.localIP().toString().c_str() : "");
  jsonKV(j, "rssi", wup ? (long)WiFi.RSSI() : 0L);
  jsonKV(j, "ch", wup ? (long)WiFi.channel() : 0L);
  jsonKV(j, "bssid", wup ? WiFi.BSSIDstr().c_str() : "");
  jsonKV(j, "mac", WiFi.macAddress().c_str());
  jsonKV(j, "host", HOSTNAME ".local");
  jsonKB(j, "mdns", mdnsUp);
  jsonKV(j, "ups", (long)wifiUps);
  jsonKV(j, "downS", ageS(now, wifiDownAt));
  jsonKB(j, "hotspot", apOn);
  jsonKV(j, "hsName", apOn ? apName().c_str() : "");
  jsonKV(j, "hsClients", apOn ? (long)WiFi.softAPgetStationNum() : 0L);
  jsonKB(j, "arduinoOta", arduinoOtaUp);
  jsonKB(j, "updatePw", cfg.otapw.length() > 0);
  jsonEnd(j, '}'); j += ',';
  j += "\"ble\":{";
  jsonKB(j, "scanning", scan && scan->isScanning());
  jsonKV(j, "all", (long)btAll);
  jsonKV(j, "gw", (long)advTotal);
  jsonKV(j, "fresh", (long)freshTotal);
  jsonKV(j, "intervalMs", 100L);                   // scan->setInterval(160) x 0.625 ms
  jsonKV(j, "windowMs", 50L);
  jsonKV(j, "clears", (long)scanClears);
  jsonKB(j, "job", jobBusy);
  jsonKV(j, "jobNode", jobBusy && jobNode >= 0 ? nodes[jobNode].mac : "");
  jsonKV(j, "jobS", jobBusy ? ageS(now, jobStartedAt) : -1L);
  jsonEnd(j, '}'); j += ',';
  j += "\"cloud\":{";
  jsonKB(j, "on", cfg.url.length() > 0);
  jsonKV(j, "url", cfg.url.c_str());
  jsonKB(j, "clock", epoch > 1700000000);
  jsonKV(j, "code", (long)lastUploadCode);
  jsonKV(j, "lastS", ageS(now, lastUploadAt));
  jsonKV(j, "okS", ageS(now, lastUploadOk));
  jsonKV(j, "ms", (long)lastUploadMs);
  jsonKV(j, "ok", (long)uploadsOk);
  jsonKV(j, "fail", (long)uploadsFail);
  jsonKV(j, "err", lastUploadErr);
  jsonKV(j, "sent", (long)uploadedTotal);
  jsonKV(j, "q", (long)qCount); jsonKV(j, "qMax", (long)QUEUE_LEN);
  jsonKV(j, "c", (long)cCount); jsonKV(j, "cMax", (long)CLIM_LEN);
  jsonKV(j, "l", (long)lCount); jsonKV(j, "lMax", (long)LIGHT_LEN);
  jsonKV(j, "streak", (long)uploadFailStreak);
  // How far behind the cloud is: age of the oldest reading still waiting to be sent.
  jsonKV(j, "oldestS", (qCount && epoch > 1700000000) ? (long)(epoch - queue_[qHead].ts) : (qCount ? -2L : -1L));
  jsonEnd(j, '}'); j += ',';
  xSemaphoreTake(lock, portMAX_DELAY);
  j += "\"nodes\":[";
  for (int i = 0; i < MAX_NODES; i++) {
    Node &n = nodes[i];
    if (!n.used) continue;
    uint32_t tot = n.rx + n.lost;
    String probes;
    for (int k = 0; k < MAX_SENSORS; k++) if (sensors[k].used && sensors[k].node == i) { if (probes.length()) probes += ", "; probes += sensors[k].name; }
    j += '{';
    jsonKV(j, "i", (long)i);
    jsonKV(j, "mac", n.mac);
    jsonKV(j, "type", n.local ? "wired to this hub" : n.addrType == 1 ? "random" : n.addrType == 0 ? "public" : "other");
    jsonKV(j, "probes", probes.c_str());
    jsonKB(j, "online", now - n.lastSeen < STALE_MS);
    jsonKV(j, "seenS", ageS(now, n.lastSeen));
    jsonKV(j, "freshS", ageS(now, n.lastFreshAt));
    jsonKV(j, "firstS", ageS(now, n.firstSeen));
    jsonKV(j, "adv", (long)n.adv);
    jsonKV(j, "rx", (long)n.rx);
    jsonKV(j, "lost", (long)n.lost);
    jsonKV(j, "lossX10", tot ? (long)(1000ULL * n.lost / tot) : 0L);
    jsonKV(j, "ivlMs", (long)n.ivlMs);
    jsonKV(j, "rssi", (long)n.rssi); jsonKV(j, "rssiMin", (long)n.rssiMin); jsonKV(j, "rssiMax", (long)n.rssiMax);
    jsonKV(j, "mv", (long)lroundf(n.volt * 1000));
    jsonKV(j, "pid", n.havePid ? (long)n.lastPid : -1L);
    jsonKV(j, "restarts", (long)n.restarts);
    jsonKB(j, "conn", n.connectable);
    jsonKB(j, "air", n.hasAir); jsonKB(j, "lux", n.hasLux); jsonKB(j, "uvOk", n.hasUV);
    jsonKV(j, "fw", n.fw);
    jsonKV(j, "st", n.st);
    jsonKB(j, "upd", n.updating);
    jsonKV(j, "pct", (long)n.pct);
    jsonKV(j, "fails", (long)n.fails);
    jsonKB(j, "small", n.smallPkts);
    jsonKV(j, "nextS", n.nextCheckAt ? (long)((int32_t)(n.nextCheckAt - now) / 1000) : 0L);
    jsonKV(j, "checks", (long)n.checks);
    jsonKV(j, "checkS", ageS(now, n.lastCheckAt));
    jsonKV(j, "diag", n.diag);
    jsonKV(j, "diagS", ageS(now, n.diagAt));
    // health (v0.12): the last hour, the last 16 gaps between readings, and the cloud
    long hrx = 0, hlost = 0, hadv = 0, hrssi = 0;
    for (int k = 0; k < H_SLOTS; k++) { hrx += n.hRx[k]; hlost += n.hLost[k]; hadv += n.hAdv[k]; hrssi += n.hRssi[k]; }
    jsonKB(j, "local", n.local);
    jsonKV(j, "hRx", hrx); jsonKV(j, "hLost", hlost); jsonKV(j, "hAdv", hadv);
    jsonKV(j, "hRssi", hadv ? lroundf((float)hrssi / hadv) : 0L);
    uint16_t g[DT_LEN];
    uint8_t ng = n.dtN;
    for (int k = 0; k < ng; k++) g[k] = n.dts[(n.dtHead - 1 - k + DT_LEN) % DT_LEN];
    long gLast = ng ? (long)g[0] * 10 : 0;
    for (int a = 1; a < ng; a++) { uint16_t v = g[a]; int b = a - 1; while (b >= 0 && g[b] > v) { g[b + 1] = g[b]; b--; } g[b + 1] = v; }
    jsonKV(j, "dtN", (long)ng);
    jsonKV(j, "dtMed", ng ? (long)g[ng / 2] * 10 : 0L);       // ms
    jsonKV(j, "dtMin", ng ? (long)g[0] * 10 : 0L);
    jsonKV(j, "dtMax", ng ? (long)g[ng - 1] * 10 : 0L);
    jsonKV(j, "dtLast", gLast);
    jsonKV(j, "cloudS", (n.cloudTs && epoch > 1700000000) ? (long)(epoch - n.cloudTs) : -1L);
    jsonKV(j, "airS", n.hasAir ? ageS(now, n.airAt) : -1L);
    jsonKV(j, "luxS", n.hasLux ? ageS(now, n.luxAt) : -1L);
    jsonKV(j, "windowS", ageS(now, n.windowAt));
    jsonEnd(j, '}'); j += ',';
  }
  jsonEnd(j, ']'); j += ',';
  j += "\"probes\":[";
  for (int k = 0; k < MAX_SENSORS; k++) {
    Sensor &p = sensors[k];
    if (!p.used) continue;
    j += '{';
    jsonKV(j, "name", p.name);
    jsonKV(j, "node", nodes[p.node].mac);
    jsonKV(j, "kind", kindOf(p));
    jsonKV(j, "raw", (long)p.raw);
    jsonKV(j, "pct", (long)pctOf(p, p.raw));
    jsonKV(j, "dry", (long)p.dry); jsonKV(j, "wet", (long)p.wet);
    jsonKB(j, "fcal", p.fcal);
    jsonKV(j, "qS", ageS(now, p.lastQAt));
    jsonEnd(j, '}'); j += ',';
  }
  jsonEnd(j, ']'); j += ',';
  xSemaphoreGive(lock);
  j += "\"log\":[";
  xSemaphoreTake(logLock, portMAX_DELAY);
  for (int k = 0; k < logCount; k++) {               // newest first
    const LogEnt &e = logBuf[(logHead - 1 - k + LOG_LEN) % LOG_LEN];
    j += '{'; jsonKV(j, "ageS", ageS(now, e.ms)); jsonKV(j, "epoch", (long)e.epoch); jsonKV(j, "t", e.t); jsonEnd(j, '}'); j += ',';
  }
  xSemaphoreGive(logLock);
  jsonEnd(j, ']');
  j += '}';
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", j);
}

// ---------------- sensors wired to the hub (v0.12) ----------------
// Read on the node's schedule in a task of their own, so a slow cloud upload never delays a reading (and the
// factory fixture's 5 s timing check passes). Each reading becomes the node's packet and goes through
// handlePacket(), exactly like one heard over Bluetooth. The same task answers the node's serial commands.
static void localInfo() {
  Serial.printf("INFO fw=%s mac=%s board=hub interval_ms=%lu light=%s air=%s uv=%s ", FW_VERSION, localMac,
                (unsigned long)REPORT_MS, lightName(), airSrc, ltrOk ? "ltr390" : "none");
  if (calOk) Serial.printf("cal=yes air=%u water=%u OK\n", cal.air, cal.water);
  else       Serial.println("cal=no OK");
}

static void localCommand(char *line) {
  for (char *c = line; *c; c++) if (*c >= 'a' && *c <= 'z') *c -= 32;   // case-insensitive
  if (!line[0]) return;
  if (!(lsMask & LS_ON)) { Serial.println("ERR hub sensors are off: tick \"Sensors wired to this hub\" at http://" HOSTNAME ".local/setup"); return; }
  unsigned a = 0, w = 0;
  if (!strcmp(line, "INFO")) localInfo();
  else if (!strcmp(line, "CAL AIR"))   { if (measurePoint("AIR", pendAir))     { if (pendWater) finishCal(pendAir, pendWater); } else pendAir = 0; }
  else if (!strcmp(line, "CAL WATER")) { if (measurePoint("WATER", pendWater)) { if (pendAir) finishCal(pendAir, pendWater); } else pendWater = 0; }
  else if (sscanf(line, "CAL SET %u %u", &a, &w) == 2) finishCal((uint16_t)a, (uint16_t)w);
  else if (!strcmp(line, "CAL CLEAR")) { storeRemove(CAL_FILE); calOk = false; pendAir = pendWater = 0; Serial.println("CAL CLEARED OK"); }
  else if (!strcmp(line, "FAST"))      { fastUntil = millis() + FAST_FOR_MS; Serial.printf("FAST interval_ms=%lu for_s=%lu OK\n", (unsigned long)FAST_REPORT_MS, (unsigned long)(FAST_FOR_MS / 1000)); }
  else Serial.printf("ERR unknown command: %s\n", line);
}

static void serviceLocalSerial() {
  static char buf[48];
  static uint8_t n = 0;
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r' || c == '\n') { buf[n] = 0; if (n) localCommand(buf); n = 0; }
    else if (n < sizeof buf - 1) buf[n++] = c;
  }
}

static void localReading() {
  static uint8_t pid = 0;
  static AirFilter airF = {false, false, 0, 0, 0, 0};
  uint16_t cap  = readCapacitive();
  float tempC = 0, rh = 0;
  bool airRead = false, airOk = false;
  if (hasAirSensor()) {
    airRead = readAir(tempC, rh);
    if (!airRead) localAirFails = localAirFails + 1;
    airOk = airRead && airFilter(airF, tempC, rh);
  }
  float lux = 0;
  bool luxOk = readLight(lux);
  float uvi = 0;
  bool uvOk = readUV(uvi);
  int32_t capPct = calOk ? (int32_t)probeScaleX100(cap) : -1;

  // The node's own packet, decoded by the hub's own parser: identical numbers to a Bluetooth node.
  uint8_t svc[31];
  uint8_t len = buildBTHome(svc, ++pid, airOk, tempC, rh, luxOk, lux, ESP_VDD_MV / 1000.0f, capPct, cap, uvOk, uvi);
  BTHomePacket p;
  if (parseBTHome(svc + 2, len - 2, p)) handlePacket(localMac, 0, 0, false, p, true);
  uint32_t now = millis();
  localLastDt = localLastAt ? now - localLastAt : 0;
  localLastAt = now;
  localReadings = localReadings + 1;
  localCap = cap;

  // What the "node" reports about itself, in the node's diagnostics format (Diagnostics > Details).
  char dg[160];
  snprintf(dg, sizeof dg, "up=%lu;rst=%s;n=%lu;air=%s;airfail=%lu;light=%s;cal=%d;ivl=%lu", (unsigned long)(now / 1000),
           espResetText(esp_reset_reason()), (unsigned long)localReadings, airSrc, (unsigned long)localAirFails, lightName(),
           calOk ? 1 : 0, (unsigned long)((localFast() ? FAST_REPORT_MS : REPORT_MS) / 1000));
  xSemaphoreTake(lock, portMAX_DELAY);
  int ni = findNode(localMac, false);
  if (ni >= 0) { strlcpy(nodes[ni].diag, dg, sizeof nodes[ni].diag); nodes[ni].diagAt = now | 1; }
  xSemaphoreGive(lock);

  // Same line as a node prints, so tools/factory_cal.py reads the hub the same way.
  char airTxt[48], luxTxt[48], capTxt[32];
  if (airOk)        snprintf(airTxt, sizeof airTxt, "air %.1f C %.0f%%RH", tempC, rh);
  else if (airRead) snprintf(airTxt, sizeof airTxt, "air: %.1f C %.0f%%RH held (jump)", tempC, rh);
  else              snprintf(airTxt, sizeof airTxt, hasAirSensor() ? "air: %s read failed" : "air: none", airSrc);
  if (ltrOk) snprintf(luxTxt, sizeof luxTxt, uvOk ? "light %.0f lux uv %.1f" : "light %.0f lux", lux, uvi);
  else if (lsMask & LS_LDR) snprintf(luxTxt, sizeof luxTxt, "light %.0f lux (adc %u)", lux, ldrAdc);
  else snprintf(luxTxt, sizeof luxTxt, "light: none");
  if (capPct >= 0) snprintf(capTxt, sizeof capTxt, "capacitive %4u = %5.1f%%", cap, capPct / 100.0f);
  else             snprintf(capTxt, sizeof capTxt, "capacitive %4u (uncal)", cap);
  Serial.printf("#%u dt=%lums  %s  %s  %s  VDD %.2f V\n", pid, (unsigned long)localLastDt, capTxt, airTxt, luxTxt, ESP_VDD_MV / 1000.0f);
}

static void localTask(void *) {
  uint32_t nextAt = millis() + 5000;                 // first reading 5 s after start
  for (;;) {
    while ((int32_t)(nextAt - millis()) > 0) { serviceLocalSerial(); vTaskDelay(pdMS_TO_TICKS(50)); }
    if (lsMask & LS_ON) localReading();
    // Absolute schedule, like the node: due REPORT_MS after the last one was due, however long this one took.
    uint32_t every = localFast() ? FAST_REPORT_MS : REPORT_MS;
    nextAt += every;
    if ((int32_t)(millis() - nextAt) >= 0) nextAt = millis() + every;   // fell behind (a 10 s calibration, say)
  }
}

static void localStart() {
  uint8_t mask = cfgPrefs.getUChar("hubsens", HUB_SENSORS_DEFAULT);
  lsBegin(mask);
  String a = BLEDevice::getAddress().toString().c_str();
  a.toUpperCase();
  strlcpy(localMac, a.c_str(), sizeof localMac);
  if (mask & LS_ON) {
    logEvent("Sensors wired to this hub: capacitive probe%s%s; air %s, light %s%s; factory calibration %s",
             (mask & LS_DHT) ? ", DHT11" : "", (mask & LS_I2C) ? ", I2C bus" : "",
             airSrc, lightName(), ltrOk ? ", UV ltr390" : "", calOk ? "yes" : "no (run tools/factory_cal.py)");
    if ((mask & LS_I2C) && !shtOk && !ahtOk && !ltrOk) logEvent("Sensors wired to this hub: no I2C sensor answered on %s", I2C_PINS);
  }
  // Only when sensors are wired: a hub without them keeps the memory (loop() answers serial commands instead).
  if ((mask & LS_ON) && xTaskCreate(localTask, "gwlocal", 6144, nullptr, 1, nullptr) != pdPASS)
    logEvent("Sensors wired to this hub: couldn't start their task (out of memory)");
}

// ---------------- setup / loop ----------------
void setup() {
  Serial.begin(115200);
  delay(300);
  logLock = xSemaphoreCreateMutex();
  logEvent("Hub %s started from slot %s (last reset: %s)", FW_VERSION, slotName(), espResetText(esp_reset_reason()));
  lock = xSemaphoreCreateMutex();
  prefs.begin("groundwell", false);
  otaPrefs.begin("gwota", false);
  readCrashDump();                                 // summarise the last crash, if any (v0.12.1)
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
  BLEDevice::setMTU(247);                                           // node updates: bigger packets if the bootloader allows
  notifQ = xQueueCreate(16, sizeof(Notif));
  nodeClient = BLEDevice::createClient();
  scan = BLEDevice::getScan();
  scan->setAdvertisedDeviceCallbacks(new ScanCallbacks(), true);   // true = report every advert
  scan->setActiveScan(false);                                       // passive: listen only
  scan->setInterval(160);
  scan->setWindow(80);                                              // 50% duty: leaves Wi-Fi radio time
  scan->start(0, nullptr, false);                                   // scan forever in the background

  localStart();                                                     // sensors wired to the hub (v0.12), after Bluetooth (its address names them)

  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/live", HTTP_GET, handleLive);
  server.on("/api/history", HTTP_GET, handleHistory);
  server.on("/api/config", HTTP_POST, handleConfig);
  server.on("/api/admin", HTTP_POST, handleAdmin);
  server.on("/setup", HTTP_GET, handleSetupGet);
  server.on("/setup", HTTP_POST, handleSetupSave);
  server.on("/diag", HTTP_GET, handleDiagPage);
  server.on("/api/diag", HTTP_GET, handleDiagApi);
  server.on("/api/diag/check", HTTP_POST, handleDiagCheck);
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
  lastDayPush = lastUpload = hSlotAt = millis();
  Serial.printf(NODE_FW_BUNDLED ? "Node firmware %s bundled (%lu bytes): nodes are checked and updated from here\n"
                                : "No node firmware bundled%s: nodes are checked, not updated\n",
                NODE_FW_VERSION, (unsigned long)NODE_FW_LEN);
  if (trialActive) Serial.printf("Firmware %s is on trial (%u crashes allowed)\n", FW_VERSION, TRIAL_CRASHES - 1);
  if (!esp_ota_get_next_update_partition(NULL)) Serial.println("WARNING: no second app slot, so over-the-air updates are off. Flash once over USB.");
  startWatchdog();
}

void loop() {
  server.handleClient();
  uint32_t now = millis();

  bool connected = WiFi.status() == WL_CONNECTED;
  if (connected && !wasConnected) {
    logEvent("Wi-Fi up: %s, %s, channel %d, %d dBm, access point %s", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(),
             (int)WiFi.channel(), (int)WiFi.RSSI(), WiFi.BSSIDstr().c_str());
    Serial.printf("Dashboard: http://%s/  (http://%s.local/)\n", WiFi.localIP().toString().c_str(), HOSTNAME);
    everConnected = true;
    wifiUps++;
    wifiDownAt = 0;
    startArduinoOta();
    if (mdnsUp) MDNS.end();                          // re-announce on the (possibly new) network address
    mdnsUp = MDNS.begin(HOSTNAME);
    if (mdnsUp) MDNS.addService("http", "tcp", 80);
    if (mdnsUp && arduinoOtaUp) MDNS.enableArduino(3232, true);   // shows up as a network port in Arduino IDE
    wifiRebootMark = 0;
  }
  if (!connected && wasConnected) { wifiDownAt = now | 1; logEvent("Wi-Fi down"); }
  wasConnected = connected;
  if (arduinoOtaUp && connected) ArduinoOTA.handle();

  // Online/offline events per node (not while the scan is paused for a node job or an update)
  static uint32_t lastOnlineCheck = 0;
  if (now - lastOnlineCheck >= 1000 && !jobBusy && !otaBusy) {
    lastOnlineCheck = now;
    char went[MAX_NODES][18]; bool up[MAX_NODES]; int nw = 0;
    xSemaphoreTake(lock, portMAX_DELAY);
    for (int i = 0; i < MAX_NODES; i++) if (nodes[i].used && nodes[i].firstSeen) {
      bool on = now - nodes[i].lastSeen < STALE_MS;
      if (on != nodes[i].online) { nodes[i].online = on; strlcpy(went[nw], nodes[i].mac, 18); up[nw++] = on; }
    }
    xSemaphoreGive(lock);
    for (int k = 0; k < nw; k++) logEvent(up[k] ? "Node %s online" : "Node %s offline (nothing heard for %lu s)", went[k], (unsigned long)(STALE_MS / 1000));
  }
  trialMaybeConfirm();
  nodeUpdaterLoop();

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
  // Bluetooth device list (v0.12.1): the library stores every device it hears, with no limit, until cleared.
  // Clear it every 10 s (only safe while the scan is stopped), so a busy room can't use up the memory.
  static uint32_t lastScanClear = 0;
  if (now - lastScanClear >= SCAN_CLEAR_MS && !jobBusy && !otaBusy) {
    lastScanClear = now;
    scan->stop();
    delay(5);                                      // a Bluetooth event already in progress finishes first
    scan->clearResults();
    scan->start(0, nullptr, false);
    scanClears++;
  }
  if (!(lsMask & LS_ON) && Serial.available()) serviceLocalSerial();   // no sensor task: still answer serial commands
  if (now - lastDayPush >= DAY_BUCKET_MS) pushDay();
  if (now - hSlotAt >= H_SLOT_MS) rotateHealthSlot();
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

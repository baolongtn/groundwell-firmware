/*
  Plant Monitor v1
  Board:   Seeed Studio XIAO ESP32-C6
           Arduino IDE: "esp32 by Espressif Systems" core 3.x, board "XIAO_ESP32C6"
  Sensors: LM393 resistive soil moisture modules (HiLetgo), using the analog AO output

  What it does
    - Reads every probe every 5 s. Probes are powered only for ~0.1 s per read to slow
      electrolytic corrosion of the forks (a big deal with resistive probes).
    - Serves a live dashboard at http://plants.local (or the IP printed on Serial).
    - Keeps a 24 h trend per plant in RAM (one averaged point every 5 min).
    - Per-plant name, dry/wet calibration and "thirsty" threshold, saved in flash.
    - JSON API (/api/live, /api/history) so a pump controller or logger can use it later.

  Wiring, up to 3 plants (direct):
    Module VCC -> D10   (switched sensor power; or 3V3 if SENSOR_PWR_PIN is set to -1)
    Module GND -> GND
    Module AO  -> A0 / A1 / A2
    Module DO  -> not used
  Run the modules at 3.3 V, never 5 V: AO swings up to the module supply and the
  ESP32-C6 ADC tops out around 3.3 V.

  More than 3 plants:
    The XIAO ESP32-C6 only breaks out 3 analog-capable pins (A0, A1, A2).
    Add a CD74HC4067 16-channel analog mux:
      SIG -> A0, S0..S3 -> MUX_PINS below, EN -> GND, VCC -> 3V3, GND -> GND
      Each module AO -> C0..C15, all module VCC -> D10 (see the current note below)
    Then list sensors as {"Name", A0, <mux channel 0..15>}.

  Sensor power current:
    Each module draws roughly 3 to 5 mA, mostly its red power LED. A GPIO is fine for
    3 or 4 modules. For more, switch 3V3 with a small P-channel MOSFET driven from D10,
    or desolder the module power LEDs.
*/

#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>

// ======================= User settings =======================
#define WIFI_SSID  "YOUR_WIFI_SSID"      // 2.4 GHz network
#define WIFI_PASS  "YOUR_WIFI_PASSWORD"
#define HOSTNAME   "plants"              // dashboard at http://plants.local

struct SensorDef {
  const char* name;  // default name (rename from the dashboard)
  int adcPin;        // A0, A1 or A2
  int muxCh;         // -1 = wired straight to adcPin, 0..15 = CD74HC4067 channel
};

// Direct wiring, 2 plants (add {"Plant 3", A2, -1} for a third):
static const SensorDef SENSORS[] = {
  {"Plant 1", A0, -1},
  {"Plant 2", A1, -1},
};
// Mux wiring example (comment out the block above, use this instead):
// static const SensorDef SENSORS[] = {
//   {"Plant 1", A0, 0}, {"Plant 2", A0, 1}, {"Plant 3", A0, 2}, {"Plant 4", A0, 3},
//   {"Plant 5", A0, 4}, {"Plant 6", A0, 5},
// };

static const int SENSOR_PWR_PIN = D10;          // -1 = sensors always powered from 3V3
static const int MUX_PINS[4] = {D3, D7, D8, D9}; // S0, S1, S2, S3 (only used with a mux)
// Left free on purpose: D4/D5 (I2C, for a display later) and D6 (pump driver later).

static const uint32_t SAMPLE_MS    = 5000;            // live reading interval
static const uint32_t HISTORY_MS   = 5UL * 60 * 1000; // trend point interval
static const int      HISTORY_LEN  = 288;             // 288 x 5 min = 24 h
static const int      SETTLE_MS    = 100;             // probe power-up settle time
static const int      OVERSAMPLE   = 16;              // ADC reads averaged per sample

// Starting calibration (mV). Fix per plant with the dashboard buttons.
static const uint16_t DEFAULT_DRY_MV = 3100;  // probe in air / bone-dry soil
static const uint16_t DEFAULT_WET_MV = 1000;  // freshly watered soil
static const uint8_t  DEFAULT_THR    = 30;    // % below which the plant is "thirsty"
// =============================================================

constexpr int N = sizeof(SENSORS) / sizeof(SENSORS[0]);

struct SensorState {
  String   name;
  uint16_t dry, wet;   // calibration points, mV
  uint8_t  thr;        // thirsty threshold, %
  uint16_t mv;         // latest reading
  uint8_t  pct;        // latest moisture %
  uint32_t sum;        // accumulator for the current history bucket
  uint16_t cnt;
};

SensorState S[N];
uint16_t hist[N][HISTORY_LEN];
int histHead = 0, histCount = 0;
static const int LIVE_LEN = 720;  // 720 x 5 s = last hour at full rate
uint16_t live[N][LIVE_LEN];
int liveHead = 0, liveCount = 0;
uint32_t lastSample = 0, lastHist = 0, lastLive = 0;
bool usesMux = false, mdnsUp = false, wasConnected = false;

WebServer server(80);
Preferences prefs;

extern const char PAGE[];

// ---------- helpers ----------
static String key(const char* p, int i) { return String(p) + i; }

static uint8_t toPct(uint16_t mv, uint16_t dry, uint16_t wet) {
  if (dry == wet) return 0;
  long p = ((long)dry - (long)mv) * 100L / ((long)dry - (long)wet);
  if (p < 0) p = 0;
  if (p > 100) p = 100;
  return (uint8_t)p;
}

static String cleanName(const String& in) {
  String out;
  for (size_t k = 0; k < in.length() && out.length() < 24; k++) {
    char c = in[k];
    if (c >= 32 && c <= 126 && c != '"' && c != '\\' && c != '<' && c != '>') out += c;
  }
  out.trim();
  return out;
}

static uint16_t readMv(const SensorDef& d) {
  if (d.muxCh >= 0) {
    for (int b = 0; b < 4; b++) digitalWrite(MUX_PINS[b], (d.muxCh >> b) & 1);
    delayMicroseconds(500);
  }
  analogReadMilliVolts(d.adcPin);  // throw away first reads after switching
  analogReadMilliVolts(d.adcPin);
  uint32_t acc = 0;
  for (int k = 0; k < OVERSAMPLE; k++) acc += analogReadMilliVolts(d.adcPin);
  return (uint16_t)(acc / OVERSAMPLE);
}

static void sampleAll() {
  if (SENSOR_PWR_PIN >= 0) { digitalWrite(SENSOR_PWR_PIN, HIGH); delay(SETTLE_MS); }
  for (int i = 0; i < N; i++) {
    uint16_t mv = readMv(SENSORS[i]);
    S[i].mv  = mv;
    S[i].pct = toPct(mv, S[i].dry, S[i].wet);
    S[i].sum += mv;
    S[i].cnt++;
  }
  if (SENSOR_PWR_PIN >= 0) digitalWrite(SENSOR_PWR_PIN, LOW);
}

static void pushHistory() {
  for (int i = 0; i < N; i++) {
    hist[i][histHead] = S[i].cnt ? (uint16_t)(S[i].sum / S[i].cnt) : S[i].mv;
    S[i].sum = 0;
    S[i].cnt = 0;
  }
  histHead = (histHead + 1) % HISTORY_LEN;
  if (histCount < HISTORY_LEN) histCount++;
  lastHist = millis();
}

static void pushLive() {
  for (int i = 0; i < N; i++) live[i][liveHead] = S[i].mv;
  liveHead = (liveHead + 1) % LIVE_LEN;
  if (liveCount < LIVE_LEN) liveCount++;
  lastLive = millis();
}

static void printSerial() {
  wl_status_t ws = WiFi.status();
  if (ws == WL_CONNECTED) {
    Serial.printf("[WiFi OK  http://%s/  RSSI %d dBm]  ", WiFi.localIP().toString().c_str(), (int)WiFi.RSSI());
  } else {
    const char* why = ws == WL_NO_SSID_AVAIL ? "network not found (check SSID, must be 2.4 GHz)"
                    : ws == WL_CONNECT_FAILED ? "connect failed (check password)"
                    : "not connected yet";
    Serial.printf("[WiFi: %s, status %d]  ", why, (int)ws);
  }
  for (int i = 0; i < N; i++) {
    Serial.printf("%s: %u mV, %u%%%s", S[i].name.c_str(), S[i].mv, S[i].pct,
                  i == N - 1 ? "\n" : "  |  ");
  }
}

// ---------- web handlers ----------
static void handleRoot() { server.send(200, "text/html", PAGE); }

static void handleLive() {
  String j;
  j.reserve(160 + N * 120);
  j += "{\"uptime\":";  j += (unsigned long)(millis() / 1000);
  j += ",\"rssi\":";    j += (int)WiFi.RSSI();
  j += ",\"sampleMs\":"; j += (unsigned long)SAMPLE_MS;
  j += ",\"sensors\":[";
  for (int i = 0; i < N; i++) {
    if (i) j += ',';
    j += "{\"id\":";    j += i;
    j += ",\"name\":\""; j += S[i].name; j += '"';
    j += ",\"mv\":";    j += (unsigned)S[i].mv;
    j += ",\"pct\":";   j += (unsigned)S[i].pct;
    j += ",\"dry\":";   j += (unsigned)S[i].dry;
    j += ",\"wet\":";   j += (unsigned)S[i].wet;
    j += ",\"thr\":";   j += (unsigned)S[i].thr;
    j += '}';
  }
  j += "]}";
  server.send(200, "application/json", j);
}

// GET /api/history            -> 24 h, one point per 5 min
// GET /api/history?range=live -> last hour, one point per 5 s
static void sendSeries(const uint16_t* buf, int len, int head, int count,
                       uint32_t intervalMs, uint32_t last) {
  String j;
  j.reserve(80 + N * (count * 5 + 4));
  j += "{\"intervalMs\":"; j += (unsigned long)intervalMs;
  j += ",\"ageMs\":";      j += (unsigned long)(millis() - last);
  j += ",\"series\":[";
  int start = (head - count + len) % len;
  for (int i = 0; i < N; i++) {
    if (i) j += ',';
    j += '[';
    for (int k = 0; k < count; k++) {
      if (k) j += ',';
      j += (unsigned)buf[i * len + (start + k) % len];
    }
    j += ']';
  }
  j += "]}";
  server.send(200, "application/json", j);
}

static void handleHistory() {
  if (server.arg("range") == "live")
    sendSeries(&live[0][0], LIVE_LEN, liveHead, liveCount, SAMPLE_MS, lastLive);
  else
    sendSeries(&hist[0][0], HISTORY_LEN, histHead, histCount, HISTORY_MS, lastHist);
}

// POST /api/config  form fields: id (required), and any of
//   name, dry, wet, thr, action=dry|wet (capture the current reading)
static void handleConfig() {
  if (!server.hasArg("id")) { server.send(400, "text/plain", "id required"); return; }
  int i = server.arg("id").toInt();
  if (i < 0 || i >= N) { server.send(400, "text/plain", "bad id"); return; }

  if (server.hasArg("action")) {
    String a = server.arg("action");
    sampleAll();  // use a fresh reading, not one up to 5 s old
    if (a == "dry") { S[i].dry = S[i].mv; prefs.putUShort(key("d", i).c_str(), S[i].dry); }
    if (a == "wet") { S[i].wet = S[i].mv; prefs.putUShort(key("w", i).c_str(), S[i].wet); }
  }
  if (server.hasArg("name")) {
    String n = cleanName(server.arg("name"));
    if (n.length()) { S[i].name = n; prefs.putString(key("n", i).c_str(), n); }
  }
  if (server.hasArg("dry")) {
    long v = server.arg("dry").toInt();
    if (v > 0 && v <= 3400) { S[i].dry = v; prefs.putUShort(key("d", i).c_str(), S[i].dry); }
  }
  if (server.hasArg("wet")) {
    long v = server.arg("wet").toInt();
    if (v > 0 && v <= 3400) { S[i].wet = v; prefs.putUShort(key("w", i).c_str(), S[i].wet); }
  }
  if (server.hasArg("thr")) {
    long v = server.arg("thr").toInt();
    if (v >= 0 && v <= 100) { S[i].thr = v; prefs.putUChar(key("t", i).c_str(), S[i].thr); }
  }
  S[i].pct = toPct(S[i].mv, S[i].dry, S[i].wet);
  server.send(200, "application/json", "{\"ok\":true}");
}

// ---------- setup / loop ----------
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\nPlant Monitor starting");

  if (SENSOR_PWR_PIN >= 0) { pinMode(SENSOR_PWR_PIN, OUTPUT); digitalWrite(SENSOR_PWR_PIN, LOW); }
  for (int i = 0; i < N; i++) if (SENSORS[i].muxCh >= 0) usesMux = true;
  if (usesMux) for (int b = 0; b < 4; b++) { pinMode(MUX_PINS[b], OUTPUT); digitalWrite(MUX_PINS[b], LOW); }
  analogReadResolution(12);

  prefs.begin("plants", false);
  for (int i = 0; i < N; i++) {
    S[i].name = prefs.getString(key("n", i).c_str(), SENSORS[i].name);
    S[i].dry  = prefs.getUShort(key("d", i).c_str(), DEFAULT_DRY_MV);
    S[i].wet  = prefs.getUShort(key("w", i).c_str(), DEFAULT_WET_MV);
    S[i].thr  = prefs.getUChar(key("t", i).c_str(), DEFAULT_THR);
    S[i].sum = 0; S[i].cnt = 0;
  }

  sampleAll();
  pushHistory();
  pushLive();
  printSerial();
  lastSample = millis();

  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOSTNAME);
  WiFi.setAutoReconnect(true);

  // Diagnostics: list what the board can see, and log why the router drops us.
  WiFi.onEvent([](WiFiEvent_t, WiFiEventInfo_t info) {
    int r = info.wifi_sta_disconnected.reason;
    const char* why = r == 201 ? "AP not found (SSID wrong or not 2.4 GHz)"
                    : r == 202 ? "auth failed (password)"
                    : r == 15 || r == 204 ? "handshake timeout (usually wrong password)"
                    : r == 210 || r == 211 ? "security mode mismatch (try WPA2 or WPA2/WPA3 mixed)"
                    : r == 203 ? "association failed"
                    : "see ESP-IDF wifi_err_reason_t";
    Serial.printf("[WiFi disconnect reason %d: %s]\n", r, why);
  }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);

  delay(1500);  // give you time to open Serial Monitor after a reset
  Serial.println("Scanning Wi-Fi...");
  int found = WiFi.scanNetworks();
  bool seen = false;
  for (int k = 0; k < found; k++) {
    bool match = WiFi.SSID(k) == WIFI_SSID;
    seen |= match;
    Serial.printf("  %s%-32s  ch %2d  %4d dBm  enc %d\n", match ? ">>" : "  ",
                  WiFi.SSID(k).c_str(), (int)WiFi.channel(k), (int)WiFi.RSSI(k), (int)WiFi.encryptionType(k));
  }
  Serial.printf("%d networks found. \"%s\" %s\n", found, WIFI_SSID,
                seen ? "IS visible" : "NOT visible (name typo, 5 GHz only, or hidden)");
  WiFi.scanDelete();

  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("Connecting to Wi-Fi");
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) { delay(250); Serial.print('.'); }
  Serial.println();

  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/live", HTTP_GET, handleLive);
  server.on("/api/history", HTTP_GET, handleHistory);
  server.on("/api/config", HTTP_POST, handleConfig);
  server.onNotFound([]() { server.send(404, "text/plain", "not found"); });
  server.begin();
}

void loop() {
  server.handleClient();

  bool connected = WiFi.status() == WL_CONNECTED;
  if (connected && !wasConnected) {
    Serial.printf("Wi-Fi up. Dashboard: http://%s/  or  http://%s.local/\n",
                  WiFi.localIP().toString().c_str(), HOSTNAME);
    if (!mdnsUp && MDNS.begin(HOSTNAME)) { MDNS.addService("http", "tcp", 80); mdnsUp = true; }
  } else if (!connected && wasConnected) {
    Serial.println("Wi-Fi lost, reconnecting...");
  }
  wasConnected = connected;

  uint32_t now = millis();
  if (now - lastSample >= SAMPLE_MS) { lastSample = now; sampleAll(); pushLive(); printSerial(); }
  if (now - lastHist >= HISTORY_MS) pushHistory();

  delay(2);
}

// ======================= Dashboard page =======================
const char PAGE[] = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Plant Monitor</title>
<style>
:root{--bg:#f3f1ea;--card:#fffdf8;--ink:#1f2a1f;--mute:#6b7466;--line:#e2ddd2;--track:#ebe6db;--grid:#ebe7dd;
--ok:#3f8f4f;--low:#c27323;--dry:#b8432f;--wet:#2f6fb8;
--s0:#2f7d5b;--s1:#c4572f;--s2:#3b6fb6;--s3:#a37a1c;--s4:#8a4fa8;--s5:#1f8f9e;--s6:#b03a6a;--s7:#5d6b2a}
@media (prefers-color-scheme:dark){:root{--bg:#121612;--card:#1b211b;--ink:#e8ede4;--mute:#97a191;
--line:#2c352c;--track:#283028;--grid:#252d25;--ok:#6cc27d;--low:#e3a05a;--dry:#e2735f;--wet:#6aa5e8;
--s0:#5fc08f;--s1:#ec8a63;--s2:#79a6ec;--s3:#d9b04e;--s4:#b98ad4;--s5:#55c3cf;--s6:#e07aa3;--s7:#a9b86a}}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--ink);font:15px/1.4 system-ui,-apple-system,"Segoe UI",Roboto,sans-serif}
header{display:flex;justify-content:space-between;align-items:baseline;flex-wrap:wrap;gap:8px;
padding:22px 16px 6px;max-width:1100px;margin:auto}
h1{font-size:22px;margin:0;letter-spacing:-.01em}
#status{font-size:13px;color:var(--mute);font-variant-numeric:tabular-nums}
.dot{display:inline-block;width:8px;height:8px;border-radius:50%;background:var(--ok);margin-right:6px;vertical-align:1px}
.dot.off{background:var(--dry)}
.panel{max-width:1100px;margin:8px auto 0;padding:0 16px}
.box{background:var(--card);border:1px solid var(--line);border-radius:14px;padding:14px 16px 10px}
.ctop{display:flex;justify-content:space-between;align-items:center;flex-wrap:wrap;gap:10px}
.ctop h2{font-size:15px;margin:0}
.ctrls{display:flex;gap:8px;flex-wrap:wrap}
.seg{display:inline-flex;border:1px solid var(--line);border-radius:9px;overflow:hidden}
.seg button{border:0;border-radius:0;background:transparent;padding:5px 11px;color:var(--mute)}
.seg button.on{background:var(--ink);color:var(--card)}
.legend{display:flex;flex-wrap:wrap;gap:6px 16px;font-size:13px;margin-top:10px}
.legend span{display:inline-flex;align-items:center;gap:6px;cursor:pointer;user-select:none}
.legend span.off{opacity:.35}
.sw{width:14px;height:3px;border-radius:2px;display:inline-block;flex:none}
#chartwrap{position:relative;margin-top:6px}
#chart{width:100%;height:280px;display:block;touch-action:pan-y}
.axis{font-size:11px;fill:var(--mute);font-variant-numeric:tabular-nums}
#tip{position:absolute;top:8px;pointer-events:none;background:var(--card);border:1px solid var(--line);border-radius:8px;
padding:6px 9px;font-size:12px;box-shadow:0 4px 14px rgba(0,0,0,.12);display:none;white-space:nowrap;font-variant-numeric:tabular-nums}
#tip b{display:block;margin-bottom:3px;font-weight:600}
#tip div{display:flex;align-items:center;gap:6px}
main{display:grid;grid-template-columns:repeat(auto-fill,minmax(260px,1fr));gap:14px;
padding:14px 16px 32px;max-width:1100px;margin:auto}
.card{background:var(--card);border:1px solid var(--line);border-radius:14px;padding:16px}
.top{display:flex;justify-content:space-between;align-items:center;gap:8px}
.nm{display:flex;align-items:center;gap:8px;min-width:0}
.name{font-weight:600;font-size:16px;cursor:pointer;border-bottom:1px dashed transparent;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.name:hover{border-bottom-color:var(--mute)}
.chip{font-size:12px;font-weight:600;padding:2px 9px;border-radius:99px;color:var(--card);white-space:nowrap}
.pct{font-size:46px;font-weight:700;letter-spacing:-.02em;margin:6px 0 4px;font-variant-numeric:tabular-nums;line-height:1.1}
.pct small{font-size:18px;color:var(--mute);font-weight:500;margin-left:2px}
.bar{position:relative;height:10px;background:var(--track);border-radius:99px}
.fill{height:100%;border-radius:99px;transition:width .6s,background .6s}
.mark{position:absolute;top:-3px;bottom:-3px;width:2px;background:var(--ink);opacity:.45}
.meta{display:flex;justify-content:space-between;font-size:12px;color:var(--mute);margin-top:7px;font-variant-numeric:tabular-nums}
svg.spark{width:100%;height:56px;margin-top:10px;display:block;overflow:visible}
.empty{font-size:12px;fill:var(--mute)}
details{margin-top:10px;font-size:13px;border-top:1px solid var(--line);padding-top:8px}
summary{cursor:pointer;color:var(--mute)}
.row{display:flex;gap:8px;align-items:center;margin-top:8px;flex-wrap:wrap}
button{font:inherit;font-size:13px;padding:6px 10px;border-radius:8px;border:1px solid var(--line);
background:var(--bg);color:var(--ink);cursor:pointer}
button:hover{border-color:var(--mute)}
input{font:inherit;font-size:13px;width:66px;padding:5px 6px;border-radius:8px;border:1px solid var(--line);background:var(--bg);color:var(--ink)}
.hint{color:var(--mute);font-size:12px;margin-top:8px}
</style></head><body>
<header><h1>Plant Monitor</h1><div id="status"><span class="dot off"></span>Connecting...</div></header>
<section class="panel"><div class="box">
  <div class="ctop"><h2>Moisture over time</h2>
    <div class="ctrls">
      <div class="seg" id="rangeSeg"><button data-v="live" class="on">Last hour</button><button data-v="day">24 h</button></div>
      <div class="seg" id="unitSeg"><button data-v="pct" class="on">%</button><button data-v="mv">mV</button></div>
    </div></div>
  <div class="legend" id="legend"></div>
  <div id="chartwrap"><svg id="chart"></svg><div id="tip"></div></div>
</div></section>
<main id="grid"></main>
<script>
const grid=document.getElementById('grid'),st=document.getElementById('status'),svgC=document.getElementById('chart'),tip=document.getElementById('tip');
let cfg=[],day=null,chart=null,cards=[],range='live',unit='pct',hidden={},geo=null,ticks=0;
const SC=i=>'var(--s'+(i%8)+')';
const tone=(p,t)=>p<t?'var(--dry)':p<t+15?'var(--low)':p>90?'var(--wet)':'var(--ok)';
const word=(p,t)=>p<t?'Thirsty':p<t+15?'Drying out':p>90?'Soaked':'Happy';
function pctOf(mv,s){if(!s||s.dry===s.wet)return 0;const p=(s.dry-mv)*100/(s.dry-s.wet);return Math.max(0,Math.min(100,Math.round(p)))}
function fmtUp(s){const d=Math.floor(s/86400),h=Math.floor(s%86400/3600),m=Math.floor(s%3600/60);return d?d+'d '+h+'h':h?h+'h '+m+'m':m+'m'}
const clock=(t,sec)=>new Date(t).toLocaleTimeString([],sec?{hour:'numeric',minute:'2-digit',second:'2-digit'}:{hour:'numeric',minute:'2-digit'});
function esc(s){const d=document.createElement('div');d.textContent=s;return d.innerHTML}

function seg(id,cb){document.querySelectorAll('#'+id+' button').forEach(b=>b.onclick=()=>{
  document.querySelectorAll('#'+id+' button').forEach(x=>x.classList.toggle('on',x===b));cb(b.dataset.v)})}
seg('rangeSeg',v=>{range=v;loadChart()});
seg('unitSeg',v=>{unit=v;drawChart()});

function build(sensors){
  grid.innerHTML='';
  cards=sensors.map(s=>{
    const c=document.createElement('div');c.className='card';
    c.innerHTML='<div class="top"><span class="nm"><i class="sw" style="background:'+SC(s.id)+'"></i><span class="name" title="Click to rename"></span></span><span class="chip"></span></div>'+
      '<div class="pct"><span class="v">--</span><small>%</small></div>'+
      '<div class="bar"><div class="fill"></div><div class="mark" title="Thirsty level"></div></div>'+
      '<div class="meta"><span class="mv"></span><span class="span"></span></div>'+
      '<svg class="spark" viewBox="0 0 300 56" preserveAspectRatio="none"></svg>'+
      '<details><summary>Calibrate and alert level</summary>'+
      '<div class="row"><button data-a="dry">Set dry = now</button><button data-a="wet">Set wet = now</button></div>'+
      '<div class="row">Dry <input class="i-dry" type="number"> Wet <input class="i-wet" type="number"> mV</div>'+
      '<div class="row">Thirsty below <input class="i-thr" type="number" min="0" max="100"> % <button data-a="save">Save</button></div>'+
      '<div class="hint">Dry: probe in air or bone-dry soil. Wet: soil right after a full watering.</div></details>';
    const q=k=>c.querySelector(k);
    q('.name').onclick=()=>{const cur=cfg[s.id]?cfg[s.id].name:s.name;const n=prompt('Plant name',cur);if(n!==null&&n.trim())post({id:s.id,name:n.trim()})};
    c.querySelectorAll('button').forEach(b=>b.onclick=()=>{
      const a=b.dataset.a,nm=cfg[s.id]?cfg[s.id].name:s.name;
      if(a==='save')post({id:s.id,dry:q('.i-dry').value,wet:q('.i-wet').value,thr:q('.i-thr').value});
      else if(confirm('Set the '+a+' point for '+nm+' to the current reading?'))post({id:s.id,action:a});
    });
    grid.appendChild(c);return c;
  });
}

function legend(){
  const L=document.getElementById('legend');
  L.innerHTML=cfg.map(s=>'<span data-i="'+s.id+'" class="'+(hidden[s.id]?'off':'')+'"><i class="sw" style="background:'+SC(s.id)+'"></i>'+esc(s.name)+'</span>').join('');
  L.querySelectorAll('span').forEach(e=>e.onclick=()=>{const i=+e.dataset.i;hidden[i]=!hidden[i];legend();drawChart()});
}

function update(d){
  const renamed=cfg.map(s=>s.name).join('|')!==d.sensors.map(s=>s.name).join('|');
  if(cards.length!==d.sensors.length)build(d.sensors);
  cfg=d.sensors;
  if(renamed)legend();
  d.sensors.forEach((s,i)=>{
    const c=cards[i],q=k=>c.querySelector(k),col=tone(s.pct,s.thr);
    q('.name').textContent=s.name;
    q('.v').textContent=s.pct;
    q('.fill').style.width=s.pct+'%';q('.fill').style.background=col;
    q('.mark').style.left='calc('+s.thr+'% - 1px)';
    const ch=q('.chip');ch.textContent=word(s.pct,s.thr);ch.style.background=col;
    q('.mv').textContent=s.mv+' mV';
    const open=q('details').open;
    [['.i-dry',s.dry],['.i-wet',s.wet],['.i-thr',s.thr]].forEach(([k,v])=>{const e=q(k);if(!open||e.value==='')e.value=v});
  });
  st.innerHTML='<span class="dot"></span>Live &middot; '+d.rssi+' dBm &middot; up '+fmtUp(d.uptime);
  drawSparks();
}

function drawSparks(){
  if(!day||!cfg.length)return;
  cards.forEach((c,i)=>{
    const svg=c.querySelector('svg.spark'),s=cfg[i],ser=day.series[i]||[],n=ser.length;
    const hrs=n>1?((n-1)*day.intervalMs/3600000):0;
    c.querySelector('.span').textContent=n>1?(hrs<1?Math.round(hrs*60)+' min':hrs.toFixed(hrs<10?1:0)+' h')+' trend':'';
    const y=p=>52-p*0.48,ty=y(s.thr).toFixed(1);
    let h='<line x1="0" x2="300" y1="'+ty+'" y2="'+ty+'" stroke="var(--mute)" stroke-dasharray="3 4" stroke-width="1" vector-effect="non-scaling-stroke"/>';
    if(n<2){h+='<text class="empty" x="0" y="34">24 h trend fills in every 5 min</text>'}
    else{
      const pts=ser.map((mv,k)=>(k*300/(n-1)).toFixed(1)+','+y(pctOf(mv,s)).toFixed(1));
      h+='<polygon points="0,52 '+pts.join(' ')+' 300,52" fill="'+tone(s.pct,s.thr)+'" opacity=".12"/>';
      h+='<polyline points="'+pts.join(' ')+'" fill="none" stroke="'+tone(s.pct,s.thr)+'" stroke-width="2" stroke-linejoin="round" vector-effect="non-scaling-stroke"/>';
    }
    svg.innerHTML=h;
  });
}

function drawChart(){
  if(!chart||!cfg.length)return;
  const W=Math.max(svgC.clientWidth,280),H=280,L=unit==='pct'?40:48,R=12,T=10,B=26,pw=W-L-R,ph=H-T-B;
  svgC.setAttribute('viewBox','0 0 '+W+' '+H);
  const ser=chart.series,n=ser.length?ser[0].length:0,iv=chart.intervalMs;
  const tEnd=chart.t-chart.ageMs,win=range==='live'?3600000:86400000,t0=tEnd-win;
  const ymax=unit==='pct'?100:3300,Y=v=>T+ph-v/ymax*ph,X=t=>L+(t-t0)/win*pw;
  const val=(mv,i)=>unit==='pct'?pctOf(mv,cfg[i]):mv,tk=k=>tEnd-(n-1-k)*iv;
  let h='';
  (unit==='pct'?[0,25,50,75,100]:[0,1000,2000,3000]).forEach(v=>{
    h+='<line x1="'+L+'" x2="'+(W-R)+'" y1="'+Y(v)+'" y2="'+Y(v)+'" stroke="var(--grid)"/>'+
       '<text class="axis" x="'+(L-6)+'" y="'+(Y(v)+4)+'" text-anchor="end">'+v+(unit==='pct'?'%':'')+'</text>'});
  const step=range==='live'?(W<500?1200000:600000):(W<500?6*3600000:4*3600000);
  for(let t=Math.ceil(t0/step)*step;t<=tEnd;t+=step){const x=X(t);
    h+='<line x1="'+x+'" x2="'+x+'" y1="'+T+'" y2="'+(T+ph)+'" stroke="var(--grid)"/>'+
       '<text class="axis" x="'+x+'" y="'+(H-7)+'" text-anchor="middle">'+clock(t)+'</text>'}
  if(n<2)h+='<text class="axis" x="'+(L+pw/2)+'" y="'+(T+ph/2)+'" text-anchor="middle">Collecting data...</text>';
  else ser.forEach((arr,i)=>{if(hidden[i])return;
    const pts=arr.map((mv,k)=>X(tk(k)).toFixed(1)+','+Y(val(mv,i)).toFixed(1)).join(' ');
    h+='<polyline points="'+pts+'" fill="none" stroke="'+SC(i)+'" stroke-width="2" stroke-linejoin="round"/>'});
  h+='<g id="hov"></g>';
  svgC.innerHTML=h;
  geo={L,pw,T,ph,n,iv,tEnd,t0,win,X,Y,val,tk};
}

function hover(ev){
  if(!geo||geo.n<2||!chart)return;
  const r=svgC.getBoundingClientRect(),x=ev.clientX-r.left,g=geo;
  const t=g.t0+(x-g.L)/g.pw*g.win;
  let k=Math.round((t-g.tk(0))/g.iv);
  if(k<0||x<g.L){unhover();return}
  k=Math.min(k,g.n-1);
  const tx=g.X(g.tk(k));
  let h='<line x1="'+tx+'" x2="'+tx+'" y1="'+g.T+'" y2="'+(g.T+g.ph)+'" stroke="var(--mute)" stroke-dasharray="3 3"/>';
  let tt='<b>'+clock(g.tk(k),range==='live')+'</b>';
  chart.series.forEach((arr,i)=>{if(hidden[i])return;const v=g.val(arr[k],i);
    h+='<circle cx="'+tx+'" cy="'+g.Y(v)+'" r="3.5" fill="'+SC(i)+'" stroke="var(--card)" stroke-width="1.5"/>';
    tt+='<div><i class="sw" style="background:'+SC(i)+'"></i>'+esc(cfg[i]?cfg[i].name:'Plant '+(i+1))+': '+v+(unit==='pct'?'%':' mV')+'</div>'});
  document.getElementById('hov').innerHTML=h;
  tip.innerHTML=tt;tip.style.display='block';
  const w=tip.offsetWidth;tip.style.left=(tx+12+w>r.width?tx-12-w:tx+12)+'px';
}
function unhover(){const g=document.getElementById('hov');if(g)g.innerHTML='';tip.style.display='none'}
svgC.addEventListener('pointermove',hover);svgC.addEventListener('pointerleave',unhover);
window.addEventListener('resize',()=>{drawChart();unhover()});

async function tick(){try{const r=await fetch('/api/live',{cache:'no-store'});update(await r.json())}catch(e){st.innerHTML='<span class="dot off"></span>Offline, retrying...'}}
async function getHist(rg){const r=await fetch('/api/history'+(rg==='live'?'?range=live':''),{cache:'no-store'});const d=await r.json();d.t=Date.now();return d}
async function loadDay(){try{day=await getHist('day');drawSparks();if(range==='day'){chart=day;drawChart()}}catch(e){}}
async function loadChart(){try{chart=range==='day'?(day=await getHist('day')):await getHist('live');drawChart();drawSparks()}catch(e){}}
async function post(o){try{await fetch('/api/config',{method:'POST',body:new URLSearchParams(o)})}catch(e){}await tick();loadChart();loadDay()}

(async()=>{await tick();loadDay();loadChart()})();
setInterval(()=>{tick();if(range==='live')loadChart();if(++ticks%12===0)loadDay()},5000);
</script></body></html>)HTML";

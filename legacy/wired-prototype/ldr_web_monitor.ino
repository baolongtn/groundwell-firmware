/*
  LDR Web Monitor
  Board: Arduino Nano ESP32
  Core:  "Arduino ESP32 Boards" (Arduino) or "esp32" (Espressif) 3.x

  Reads the GM5539 light sensor divider on A0 and serves a live page:
      http://ldr.local/            (or the IP printed on Serial at 115200)
  The page shows the live reading, a 10-minute chart, the dark/lit state,
  and lets you set the dark thresholds. Thresholds are saved in flash and
  use the same 0 to 1 scale as auto_3way_light.ino, so values you dial in
  here carry straight over.

  WIRING (3.3 V ONLY)
    3V3 -> GM5539 -> A0 -> 100k -> GND
    100 nF ceramic from A0 to GND
  The ESP32-S3 pins are NOT 5 V tolerant. Do not feed the divider from the
  5V or VBUS pin: in bright light A0 would sit near 5 V and can damage the chip.

  If the board can't join your WiFi within 15 s, it starts its own network
  named LDR-Monitor (password below). Join it and open http://192.168.4.1
*/

#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>

// ---------------- Your settings ----------------
const char* WIFI_SSID = "YOUR_WIFI_SSID";
const char* WIFI_PASS = "YOUR_WIFI_PASSWORD";
const char* HOSTNAME  = "ldr";          // page at http://ldr.local/
const char* AP_SSID   = "LDR-Monitor";  // fallback network
const char* AP_PASS   = "lightsensor";  // 8+ characters

// ---------------- Hardware ----------------
const uint8_t PIN_LDR  = A0;            // GPIO1, ADC1 (works while WiFi is on)
const float   VCC_MV   = 3300.0f;       // divider supply
const float   R_FIXED  = 100000.0f;     // R4, ohms

// ---------------- Sampling ----------------
const unsigned long SAMPLE_MS  = 100;   // read A0 at 10 Hz
const unsigned long HISTORY_MS = 1000;  // log one point per second
const uint16_t      HISTORY_LEN = 600;  // 10 minutes
const float         EMA_ALPHA  = 0.3f;  // smoothing on the 10 Hz stream

// ---------------- State ----------------
uint16_t history[HISTORY_LEN];          // millivolts, ring buffer
uint16_t histCount = 0;
uint16_t histHead  = 0;                 // next write index

float emaMv   = -1.0f;
float darkOn  = 0.35f;
float darkOff = 0.45f;
bool  dark    = false;
bool  apMode  = false;

unsigned long lastSampleMs  = 0;
unsigned long lastHistoryMs = 0;

WebServer   server(80);
Preferences prefs;

// ---------------- Web page ----------------
const char PAGE[] = R"HTML(<!doctype html>
<html lang="en"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>LDR Monitor</title>
<style>
:root{--bg:#f4f5f3;--card:#fff;--fg:#1c2124;--mu:#5f686e;--rule:#d9dddb;--ac:#0b6e7a;--dark:#2b3a67;--lit:#c98a00;--bad:#b3261e}
@media (prefers-color-scheme:dark){:root{--bg:#14181b;--card:#1c2125;--fg:#e4e8ea;--mu:#9aa4aa;--rule:#2e353a;--ac:#52c1cd;--dark:#8fa6ff;--lit:#f0b429;--bad:#f2867e;color-scheme:dark}}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--fg);font:15px/1.5 system-ui,-apple-system,"Segoe UI",sans-serif;padding:20px max(16px,env(safe-area-inset-left))}
.w{max-width:760px;margin:0 auto;display:flex;flex-direction:column;gap:14px}
h1{font-size:22px;margin:0;font-weight:600}
.top{display:flex;justify-content:space-between;align-items:baseline;gap:12px;flex-wrap:wrap}
.st{font-size:12px;color:var(--mu)}
.st.bad{color:var(--bad)}
.card{background:var(--card);border:1px solid var(--rule);border-radius:10px;padding:14px 16px}
.big{display:flex;align-items:center;gap:16px;flex-wrap:wrap}
.val{font:600 46px/1 ui-monospace,Menlo,monospace;font-variant-numeric:tabular-nums}
.pill{font-weight:600;font-size:13px;padding:4px 12px;border-radius:99px;border:1.5px solid currentColor}
.pill.d{color:var(--dark)}.pill.l{color:var(--lit)}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(130px,1fr));gap:10px}
.k{font-size:11px;text-transform:uppercase;letter-spacing:.06em;color:var(--mu)}
.v{font:500 18px ui-monospace,Menlo,monospace;font-variant-numeric:tabular-nums}
canvas{width:100%;height:240px;display:block}
.leg{display:flex;gap:14px;font-size:12px;color:var(--mu);flex-wrap:wrap;margin-top:6px}
.leg i{display:inline-block;width:16px;height:0;border-top:2px dashed;vertical-align:middle;margin-right:5px}
form{display:flex;gap:10px;align-items:end;flex-wrap:wrap}
label{display:flex;flex-direction:column;font-size:12px;color:var(--mu);gap:3px}
input{width:110px;font:15px ui-monospace,Menlo,monospace;padding:6px 8px;border:1px solid var(--rule);border-radius:6px;background:var(--bg);color:var(--fg)}
button{font:600 14px system-ui,sans-serif;padding:8px 14px;border-radius:6px;border:0;background:var(--ac);color:#fff;cursor:pointer}
button.s{background:transparent;color:var(--ac);border:1px solid var(--ac)}
#msg{font-size:13px;color:var(--mu);min-height:1.2em}
</style></head><body><div class="w">
<div class="top"><h1>LDR Monitor</h1><span id="st" class="st">Connecting</span></div>
<div class="card big"><div class="val" id="frac">--</div><div><div class="k">A0, fraction of 3.3 V</div><span id="pill" class="pill">--</span></div></div>
<div class="grid">
 <div class="card"><div class="k">A0 voltage</div><div class="v" id="mv">--</div></div>
 <div class="card"><div class="k">LDR resistance</div><div class="v" id="r">--</div></div>
 <div class="card"><div class="k">10 min low / high</div><div class="v" id="mm">--</div></div>
 <div class="card"><div class="k">WiFi signal</div><div class="v" id="rssi">--</div></div>
</div>
<div class="card"><canvas id="c"></canvas>
 <div class="leg"><span><i style="border-color:var(--dark)"></i>Dark below</span><span><i style="border-color:var(--lit)"></i>Lit above</span><span>Last 10 minutes, newest at right</span></div></div>
<div class="card"><form id="f">
 <label for="on">Dark below<input id="on" type="number" step="0.01" min="0.01" max="0.98"></label>
 <label for="off">Lit above<input id="off" type="number" step="0.01" min="0.02" max="0.99"></label>
 <button type="submit">Save thresholds</button>
 <button type="button" class="s" id="here">Set dark at current reading</button>
</form><div id="msg"></div></div>
</div>
<script>
const $=id=>document.getElementById(id);let last=null,edited=false;
['on','off'].forEach(id=>$(id).addEventListener('input',()=>edited=true));
function css(n){return getComputedStyle(document.documentElement).getPropertyValue(n).trim()}
function draw(d){
 const c=$('c'),r=window.devicePixelRatio||1,w=c.clientWidth,h=c.clientHeight;
 c.width=w*r;c.height=h*r;const g=c.getContext('2d');g.scale(r,r);
 const L=34,B=18,T=6,pw=w-L-6,ph=h-B-T,y=v=>T+ph*(1-v);
 g.font='11px ui-monospace,monospace';g.fillStyle=css('--mu');g.strokeStyle=css('--rule');g.lineWidth=1;
 for(let v=0;v<=1.001;v+=0.25){g.beginPath();g.moveTo(L,y(v));g.lineTo(L+pw,y(v));g.stroke();g.fillText(v.toFixed(2),2,y(v)+4)}
 g.fillText('-10 min',L,h-3);g.fillText('now',L+pw-22,h-3);
 [[d.on,'--dark'],[d.off,'--lit']].forEach(([v,col])=>{g.setLineDash([5,4]);g.strokeStyle=css(col);g.lineWidth=1.5;g.beginPath();g.moveTo(L,y(v));g.lineTo(L+pw,y(v));g.stroke()});
 g.setLineDash([]);const hs=d.hist;if(hs.length<2)return;
 const n=600,x=i=>L+pw*(n-hs.length+i)/(n-1);
 g.strokeStyle=css('--ac');g.lineWidth=2;g.beginPath();
 hs.forEach((mv,i)=>{const v=Math.min(1,mv/3300);i?g.lineTo(x(i),y(v)):g.moveTo(x(i),y(v))});g.stroke();
}
function show(d){
 last=d;$('frac').textContent=d.frac.toFixed(3);$('mv').textContent=d.mv+' mV';
 $('r').textContent=d.ldr_k<0?'open':(d.ldr_k>=1000?(d.ldr_k/1000).toFixed(2)+' MΩ':d.ldr_k.toFixed(1)+' kΩ');
 const p=$('pill');p.textContent=d.dark?'Dark':'Lit';p.className='pill '+(d.dark?'d':'l');
 if(d.hist.length){const f=d.hist.map(v=>v/3300);$('mm').textContent=Math.min(...f).toFixed(2)+' / '+Math.max(...f).toFixed(2)}
 $('rssi').textContent=d.ap?'own network':d.rssi+' dBm';
 if(!edited){$('on').value=d.on.toFixed(2);$('off').value=d.off.toFixed(2)}
 $('st').textContent='Live, up '+Math.floor(d.up/60)+' min';$('st').className='st';draw(d);
}
async function poll(){
 try{const r=await fetch('/data',{cache:'no-store'});if(!r.ok)throw 0;show(await r.json())}
 catch(e){$('st').textContent='Lost connection, retrying';$('st').className='st bad'}
 setTimeout(poll,1000);
}
async function save(on,off){
 const r=await fetch('/config?on='+on+'&off='+off,{method:'POST'});const t=await r.json();
 $('msg').textContent=r.ok?'Saved. Dark below '+t.on.toFixed(2)+', lit above '+t.off.toFixed(2)+'.':t.error;
 if(r.ok)edited=false;
}
$('f').addEventListener('submit',e=>{e.preventDefault();const on=+$('on').value,off=+$('off').value;
 if(!(on>0&&off<1&&on<off)){$('msg').textContent='Dark must be below lit, both between 0 and 1.';return}save(on,off)});
$('here').addEventListener('click',()=>{if(!last)return;const on=Math.max(0.01,+(last.frac+0.02).toFixed(2));
 const off=Math.min(0.99,+(on+0.10).toFixed(2));$('on').value=on;$('off').value=off;save(on,off)});
window.addEventListener('resize',()=>last&&draw(last));poll();
</script></body></html>)HTML";

// ---------------- Helpers ----------------
float readA0Mv() {
  // analogReadMilliVolts uses the chip's factory ADC calibration
  uint32_t sum = 0;
  for (uint8_t i = 0; i < 8; i++) sum += analogReadMilliVolts(PIN_LDR);
  return sum / 8.0f;
}

// LDR on top (to 3V3), R_FIXED on the bottom (to GND)
float ldrKOhm(float mv) {
  if (mv < 5.0f) return -1.0f;                   // effectively open (very dark)
  if (mv > VCC_MV - 5.0f) mv = VCC_MV - 5.0f;
  return R_FIXED * (VCC_MV - mv) / mv / 1000.0f;
}

void updateDark(float frac) {
  if (dark && frac > darkOff) dark = false;
  else if (!dark && frac < darkOn) dark = true;
}

void pushHistory(float mv) {
  history[histHead] = (uint16_t)(mv + 0.5f);
  histHead = (histHead + 1) % HISTORY_LEN;
  if (histCount < HISTORY_LEN) histCount++;
}

// ---------------- HTTP handlers ----------------
void handleRoot() {
  server.send(200, "text/html", PAGE);
}

void handleData() {
  float mv = emaMv < 0 ? 0 : emaMv;
  float frac = mv / VCC_MV;

  String j;
  j.reserve(4800);
  j += "{\"mv\":";     j += (int)(mv + 0.5f);
  j += ",\"frac\":";   j += String(frac, 4);
  j += ",\"ldr_k\":";  j += String(ldrKOhm(mv), 1);
  j += ",\"dark\":";   j += dark ? "true" : "false";
  j += ",\"on\":";     j += String(darkOn, 2);
  j += ",\"off\":";    j += String(darkOff, 2);
  j += ",\"up\":";     j += millis() / 1000;
  j += ",\"ap\":";     j += apMode ? "true" : "false";
  j += ",\"rssi\":";   j += apMode ? 0 : WiFi.RSSI();
  j += ",\"hist\":[";
  uint16_t start = (histHead + HISTORY_LEN - histCount) % HISTORY_LEN;  // oldest
  for (uint16_t i = 0; i < histCount; i++) {
    if (i) j += ',';
    j += history[(start + i) % HISTORY_LEN];
  }
  j += "]}";
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", j);
}

void handleConfig() {
  if (!server.hasArg("on") || !server.hasArg("off")) {
    server.send(400, "application/json", "{\"error\":\"Send on and off values.\"}");
    return;
  }
  float on  = server.arg("on").toFloat();
  float off = server.arg("off").toFloat();
  if (!(on > 0.0f && off < 1.0f && on < off)) {
    server.send(400, "application/json",
                "{\"error\":\"Dark must be below lit, both between 0 and 1.\"}");
    return;
  }
  darkOn = on;
  darkOff = off;
  prefs.putFloat("on", darkOn);
  prefs.putFloat("off", darkOff);
  Serial.printf("Thresholds saved: dark < %.2f, lit > %.2f\n", darkOn, darkOff);

  String j = "{\"on\":" + String(darkOn, 2) + ",\"off\":" + String(darkOff, 2) + "}";
  server.send(200, "application/json", j);
}

// ---------------- WiFi ----------------
void startWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOSTNAME);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.printf("Joining %s", WIFI_SSID);

  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
    delay(250);
    Serial.print('.');
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    WiFi.setAutoReconnect(true);
    Serial.printf("Connected. Open http://%s.local/ or http://%s/\n",
                  HOSTNAME, WiFi.localIP().toString().c_str());
  } else {
    apMode = true;
    WiFi.disconnect(true);
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, AP_PASS);
    Serial.printf("Could not join WiFi. Join network \"%s\" (password %s) "
                  "and open http://%s/\n",
                  AP_SSID, AP_PASS, WiFi.softAPIP().toString().c_str());
  }

  if (MDNS.begin(HOSTNAME)) MDNS.addService("http", "tcp", 80);
}

// ---------------- Main ----------------
void setup() {
  Serial.begin(115200);
  delay(1500);  // give USB serial a moment so the first lines aren't lost

  analogReadResolution(12);

  prefs.begin("ldr", false);
  darkOn  = prefs.getFloat("on", darkOn);
  darkOff = prefs.getFloat("off", darkOff);

  emaMv = readA0Mv();
  dark = (emaMv / VCC_MV) < darkOn;

  startWifi();

  server.on("/", HTTP_GET, handleRoot);
  server.on("/data", HTTP_GET, handleData);
  server.on("/config", HTTP_POST, handleConfig);
  server.onNotFound([]() { server.send(404, "text/plain", "Not found"); });
  server.begin();
}

void loop() {
  server.handleClient();

  unsigned long now = millis();

  if (now - lastSampleMs >= SAMPLE_MS) {
    lastSampleMs = now;
    float mv = readA0Mv();
    emaMv = (emaMv < 0) ? mv : emaMv + EMA_ALPHA * (mv - emaMv);
    updateDark(emaMv / VCC_MV);
  }

  if (now - lastHistoryMs >= HISTORY_MS) {
    lastHistoryMs = now;
    pushHistory(emaMv);
  }
}
